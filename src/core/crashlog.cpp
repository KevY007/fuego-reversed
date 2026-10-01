#include "crashlog.h"
#include "logger.h"
#include "trace.h"

#include "../hook/CDetour.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

namespace {

volatile long g_logged  = 0; // cap the number of crash reports so we never spam
thread_local int g_probing = 0; // this thread's guard is reading memory that may be gone: its faults are expected

// The module whose executable code `p` is in ("gta_sa.exe"), with the offset; null if
// it isn't code (data, heap, stack, unmapped).
const char *code_module( const void *p, uintptr_t &offOut ) {
	static char name[MAX_PATH];
	offOut = 0;
	MEMORY_BASIC_INFORMATION mbi = {};
	if ( !p || !VirtualQuery( p, &mbi, sizeof mbi ) || mbi.State != MEM_COMMIT ) return nullptr;
	if ( !( mbi.Protect & ( PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY ) ) ) return nullptr;
	HMODULE mod = nullptr;
	if ( !GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                          (LPCSTR) p, &mod ) || !mod )
		return nullptr;
	char full[MAX_PATH] = { 0 };
	GetModuleFileNameA( mod, full, sizeof( full ) );
	const char *base = strrchr( full, '\\' );
	lstrcpynA( name, base ? base + 1 : full, sizeof( name ) );
	offOut = (uintptr_t) p - (uintptr_t) mod;
	return name;
}

const char *module_at( void *addr, uintptr_t &offOut ) {
	static char name[MAX_PATH];
	HMODULE mod = nullptr;
	if ( GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                         (LPCSTR) addr, &mod ) && mod ) {
		char full[MAX_PATH] = { 0 };
		GetModuleFileNameA( mod, full, sizeof( full ) );
		const char *base = strrchr( full, '\\' );
		lstrcpynA( name, base ? base + 1 : full, sizeof( name ) );
		offOut = (uintptr_t) addr - (uintptr_t) mod;
		return name;
	}
	offOut = 0;
	return "?";
}

// "eax=0x...(gta_sa.exe+0x1234)" for a register that points into code.
void log_registers( const CONTEXT *c ) {
	struct { const char *n; DWORD v; } regs[] = { { "eax", c->Eax }, { "ebx", c->Ebx }, { "ecx", c->Ecx }, { "edx", c->Edx },
	                                              { "esi", c->Esi }, { "edi", c->Edi }, { "ebp", c->Ebp }, { "esp", c->Esp } };
	char line[400];
	int at = 0;
	for ( const auto &r : regs )
		at += wsprintfA( line + at, "%s=%08X ", r.n, r.v );
	Log( "[CRASH] %s", line );
}

// The call chain, roughly: values on the stack that point just past a CALL in a
// module's code -- return addresses. (No symbols: module+offset, to look up.)
void log_stack( const CONTEXT *c ) {
	const NT_TIB *tib = reinterpret_cast<const NT_TIB *>( NtCurrentTeb() );
	const uintptr_t top = (uintptr_t) tib->StackBase;
	uintptr_t sp = c->Esp;
	if ( sp < (uintptr_t) tib->StackLimit || sp >= top ) return;
	const uintptr_t end = ( top - sp > 8192 ) ? sp + 8192 : top;
	int shown = 0;
	char line[600];
	int at = wsprintfA( line, "[CRASH] stack:" );
	for ( ; sp + 4 <= end && shown < 20; sp += 4 ) {
		const uint8_t *ret = *reinterpret_cast<const uint8_t *const *>( sp );
		uintptr_t off = 0;
		const char *mod = code_module( ret, off );
		if ( !mod || off < 8 ) continue;
		// Just past a CALL: E8 rel32, or FF /2 (call [reg/mem]) in its 2..7-byte forms.
		uintptr_t prev = 0;
		if ( !code_module( ret - 7, prev ) ) continue;
		const bool call = ret[-5] == 0xE8 || ( ret[-2] == 0xFF && ( ret[-1] & 0x38 ) == 0x10 ) ||
		                  ( ret[-3] == 0xFF && ( ret[-2] & 0x38 ) == 0x10 ) || ( ret[-6] == 0xFF && ( ret[-5] & 0x38 ) == 0x10 ) ||
		                  ( ret[-7] == 0xFF && ( ret[-6] & 0x38 ) == 0x10 );
		if ( !call ) continue;
		char entry[64];
		char shortMod[40];
		lstrcpynA( shortMod, mod, sizeof shortMod ); // a very long module name can't overflow `line`
		wsprintfA( entry, " %s+0x%X", shortMod, (unsigned) off );
		if ( at + (int) lstrlenA( entry ) >= (int) sizeof line ) { Log( "%s", line ); at = wsprintfA( line, "[CRASH]       " ); }
		at += wsprintfA( line + at, "%s", entry );
		if ( ++shown % 5 == 0 ) { Log( "%s", line ); at = wsprintfA( line, "[CRASH]       " ); }
	}
	if ( shown % 5 ) Log( "%s", line );
}

LONG WINAPI veh( EXCEPTION_POINTERS *ep ) {
	const DWORD code = ep->ExceptionRecord->ExceptionCode;

	// Only real faults -- ignore C++/debug/control exceptions that are handled.
	switch ( code ) {
		case EXCEPTION_ACCESS_VIOLATION:
		case EXCEPTION_ILLEGAL_INSTRUCTION:
		case EXCEPTION_PRIV_INSTRUCTION:
		case EXCEPTION_STACK_OVERFLOW:
		case EXCEPTION_INT_DIVIDE_BY_ZERO:
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
			break;
		default:
			return EXCEPTION_CONTINUE_SEARCH;
	}
	if ( g_probing ) return EXCEPTION_CONTINUE_SEARCH; // a guard's probe: handled by its __except

	if ( InterlockedIncrement( &g_logged ) > 40 )
		return EXCEPTION_CONTINUE_SEARCH; // stop logging after a burst

	void     *addr = ep->ExceptionRecord->ExceptionAddress;
	uintptr_t off  = 0;
	const char *mod = module_at( addr, off );

	Log( "==================== CRASH ====================" );
	Log( "[CRASH] code=0x%08X at 0x%p  (%s+0x%X)", code, addr, mod, (unsigned) off );
	Log( "[CRASH] last traced function: %s", getLastFunc() );
	if ( code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2 ) {
		Log( "[CRASH] %s access to 0x%p",
			ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "WRITE" :
			ep->ExceptionRecord->ExceptionInformation[0] == 8 ? "EXECUTE" : "READ",
			(void *) ep->ExceptionRecord->ExceptionInformation[1] );
	}
	if ( code != EXCEPTION_STACK_OVERFLOW && ep->ContextRecord ) { // (no stack left to walk with then)
		log_registers( ep->ContextRecord );
		log_stack( ep->ContextRecord );
	}
	Log( "==============================================" );
	return EXCEPTION_CONTINUE_SEARCH; // don't swallow -- let the crash proceed
}

// --- crash guards ----------------------------------------------------------------
// CVisibilityPlugins::RenderAtomicWithAlpha (gta_sa.exe 0x732480, cdecl atomic, alpha)
// draws a see-through / fading model. GTA can keep an atomic in that draw list after
// the model under it was freed; it then reads the dead geometry's material list and
// crashes (seen 2026-10-02: a "material" of 0xC1000000 -- a float -- at
// gta_sa.exe+0x3324B6). The guard checks the material list first and skips that one
// draw if it's gone.
using RenderAtomicWithAlpha_t = void( __cdecl * )( void *atomic, int alpha );
RenderAtomicWithAlpha_t g_origRenderAtomicWithAlpha = nullptr;
volatile long           g_skippedAtomics = 0;

bool materials_alive( const uint8_t *atomic ) {
	__try {
		const uint8_t *geo = *reinterpret_cast<const uint8_t *const *>( atomic + 0x18 ); // RpAtomic::geometry
		if ( !geo ) return false;
		const int n = *reinterpret_cast<const int *>( geo + 0x24 ); // matList.numMaterials
		if ( n < 0 || n > 150 ) return false; // GTA keeps one alpha per material in a 152-byte stack buffer
		const uint8_t *const *mats = *reinterpret_cast<const uint8_t *const *const *>( geo + 0x20 ); // matList.materials
		for ( int i = 0; i < n; ++i ) {
			const uint8_t *m = mats[i];
			if ( !m || *reinterpret_cast<const int16_t *>( m + 0x18 ) <= 0 ) return false; // RpMaterial::refCount
			(void) *reinterpret_cast<const volatile uint8_t *>( m + 7 );                    // color.alpha (what crashed)
		}
		return true;
	} __except ( EXCEPTION_EXECUTE_HANDLER ) {
		return false;
	}
}

void __cdecl guarded_render_atomic_with_alpha( void *atomic, int alpha ) {
	++g_probing;
	const bool ok = atomic && materials_alive( static_cast<const uint8_t *>( atomic ) );
	--g_probing;
	if ( ok ) {
		g_origRenderAtomicWithAlpha( atomic, alpha );
		return;
	}
	if ( InterlockedIncrement( &g_skippedAtomics ) <= 5 )
		Log( "[guard] skipped drawing a model whose materials were already freed (GTA would have crashed)." );
}

} // namespace

namespace crashlog {

void install() {
	AddVectoredExceptionHandler( 1 /*call first*/, veh );
	Log( "[crashlog] exception handler installed (crashes will be located in this log)." );
}

void install_guards() {
	static bool done = false;
	if ( done ) return;
	done = true;
	// gta_sa.exe 1.0 US's own first bytes (sub esp, 98h; push ebx): anything else means
	// another mod hooked it, or another exe -- leave it alone.
	static const uint8_t kOriginal[] = { 0x81, 0xEC, 0x98, 0x00, 0x00, 0x00, 0x53 };
	if ( std::memcmp( reinterpret_cast<const void *>( 0x732480 ), kOriginal, sizeof kOriginal ) != 0 ) {
		Log( "[guard] RenderAtomicWithAlpha isn't GTA 1.0 US's own -- its crash guard stays off." );
		return;
	}
	CDetour detour;
	void *tramp = detour.Create( (BYTE *) 0x732480, (BYTE *) guarded_render_atomic_with_alpha, DETOUR_TYPE_JMP );
	if ( !tramp ) {
		Log( "[guard] RenderAtomicWithAlpha crash guard couldn't be installed." );
		return;
	}
	g_origRenderAtomicWithAlpha = reinterpret_cast<RenderAtomicWithAlpha_t>( tramp );
	Log( "[guard] crash guard on: a see-through model whose materials were freed is skipped, not a crash." );
}

} // namespace crashlog
