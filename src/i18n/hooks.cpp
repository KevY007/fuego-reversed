#include "hooks.h"

#include "engine.h"
#include "config.h"

#include "../samp/samp.h"
#include "../samp/addresses.h"
#include "../hook/CDetour.h"
#include "../render/render.h"
#include "../core/logger.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace i18n::hooks {

namespace {

// CChat::AddEntry(this, type, msg, prefix, textColor, prefixColor) -- __thiscall.
using AddEntryFn = void (__thiscall *)( void *self, int type, const char *msg,
                                         const char *prefix, uint32_t textColor,
                                         uint32_t prefixColor );

// The trampoline CDetour hands back from Create(): the stolen prologue bytes
// followed by a jmp back into the real AddEntry past the patch. Calling
// THIS -- never the patched address itself -- runs the genuine SA:MP
// function. See the re-entrancy note on hooked_add_entry() below.
AddEntryFn g_origAddEntry = nullptr;

bool                  g_installed = false;
std::atomic<uint64_t> g_seq{ 1 };

// Phase 0 marker: a small leading glyph + a faint colour tint, so a landed
// translation reads as machine-translated without shouting. (marker_style
// in config is reserved for a Task 12 menu choice; Phase 0 has just this one.)
constexpr char     kMarker[]    = "\xE2\x97\x86 "; // U+25C6 BLACK DIAMOND + space (UTF-8)
constexpr uint32_t kMarkerColor = 0xFFB7B7FFu;      // faint lavender (ARGB)

// Called from i18n::engine::pump() on the main thread once a translation
// lands. MUST go through the trampoline (g_origAddEntry), never through
// hooked_add_entry() or the patched address itself -- otherwise the line we
// print here would re-enter the detour and get queued for translation again.
void apply_result( const i18n::engine::Result &r ) {
	if ( !r.ok || r.displayBytes.empty() ) return; // leave whatever original already showed
	if ( !g_origAddEntry ) return;

	void *chat = samp::chat_ptr();
	if ( !chat ) return; // left the server before the result landed

	std::string marked;
	marked.reserve( sizeof( kMarker ) + r.displayBytes.size() );
	marked.append( kMarker );
	marked.append( r.displayBytes );

	// CHAT_TYPE_DEBUG: a local-only line (no player prefix), the same type
	// samp::chat() uses for our own overlay/status output -- and, since it's
	// never CHAT_TYPE_CHAT/CHAT_TYPE_INFO, hooked_add_entry() would ignore it
	// for translation even if this call somehow went through the patched
	// address (it doesn't -- see above).
	g_origAddEntry( chat, samp::CHAT_TYPE_DEBUG, marked.c_str(), nullptr, kMarkerColor, 0 );
}

// The detour target: every call to the patched AddEntry address (SA:MP's own
// call sites, and anything else in-process that calls through that same
// address, e.g. samp::chat()) lands here instead. Declared __fastcall with a
// throwaway EDX slot because MSVC won't let a free function declare
// __thiscall directly; __fastcall(self, dummy, rest...) and
// __thiscall(self, rest...) produce the identical register/stack contract
// (ECX = self, remaining args on the stack in the same order, callee-clean),
// so this safely receives exactly what the real thiscall call sites pass.
//
// Re-entrancy: this function shows the original line via the trampoline
// (g_origAddEntry), never via the patched address -- so it cannot re-enter
// itself. apply_result() above (the only other caller of AddEntry we add)
// also only ever uses the trampoline. That means BOTH "show original" and
// "show translated" bypass the patch entirely: hooked_add_entry() only ever
// runs for genuine external calls to the real AddEntry address, never for
// lines we ourselves are displaying -- no loop, and a translated line is
// never itself resubmitted for translation.
void __fastcall hooked_add_entry( void *self, void * /*edx*/, int type, const char *msg,
                                   const char *prefix, uint32_t textColor, uint32_t prefixColor ) {
	// Always show the original line immediately (decided UX: original now,
	// translation appended when it lands), via the real AddEntry.
	if ( g_origAddEntry )
		g_origAddEntry( self, type, msg, prefix, textColor, prefixColor );

	// Only genuine incoming server chat/info lines are worth translating.
	// Never CHAT_TYPE_DEBUG (our own overlay/status output, including the
	// marked line apply_result() just showed above) and never empty text.
	if ( type != samp::CHAT_TYPE_CHAT && type != samp::CHAT_TYPE_INFO ) return;
	if ( !msg || !msg[0] ) return;

	i18n::engine::Job job;
	job.handle      = g_seq.fetch_add( 1, std::memory_order_relaxed );
	job.surface     = 0; // Phase 0: Chat
	job.sourceBytes = msg;
	job.srcCodepage = i18n::config::get().source_cp; // 0 = auto-detect
	i18n::engine::submit( job );
}

// Registered via render::on_frame(); drains finished translations on the
// main thread every frame (see engine::pump()'s contract).
void pump_tick( IDirect3DDevice9 * ) {
	i18n::engine::pump();
}

} // namespace

void install() {
	if ( g_installed ) return;
	if ( !i18n::config::get().inbound_chat ) return; // inert unless enabled

	const uint32_t base = samp::base();
	if ( !base ) {
		Log( "[i18n] hooks::install: samp.dll not present -- skipping." );
		return;
	}

	CDetour detour;
	void *trampoline = detour.Create(
		(BYTE *)( base + samp::addr::FUNC_ADD_TO_CHAT_WND ),
		(BYTE *)hooked_add_entry,
		DETOUR_TYPE_JMP );
	if ( !trampoline ) {
		Log( "[i18n] hooks::install: AddEntry detour failed -- inbound translation disabled." );
		return;
	}
	g_origAddEntry = reinterpret_cast<AddEntryFn>( trampoline );

	render::on_frame( pump_tick );
	i18n::engine::set_apply( &apply_result );

	g_installed = true;
	Log( "[i18n] hooks installed: AddEntry detour + main-thread pump active." );
}

} // namespace i18n::hooks
