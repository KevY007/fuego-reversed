#include "crashlog.h"
#include "logger.h"
#include "trace.h"

#include <windows.h>

namespace {

volatile long g_logged = 0; // cap the number of crash reports so we never spam

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
	Log( "==============================================" );
	return EXCEPTION_CONTINUE_SEARCH; // don't swallow -- let the crash proceed
}

} // namespace

namespace crashlog {

void install() {
	AddVectoredExceptionHandler( 1 /*call first*/, veh );
	Log( "[crashlog] exception handler installed (crashes will be located in this log)." );
}

} // namespace crashlog
