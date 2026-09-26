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

// Phase 0 marker: a small leading marker + a faint colour tint, so a landed
// translation reads as machine-translated without shouting. ASCII, not a
// UTF-8 glyph: displayBytes are cp1252 bytes that SA:MP renders via
// DrawTextA under the process's ANSI codepage (CP_ACP), so a multi-byte
// UTF-8 marker (originally the U+25C6 diamond here) prints as mojibake --
// each UTF-8 byte redecoded as its own cp1252 character -- instead of a
// glyph. ">> " is pure ASCII, so it renders identically under any ANSI
// codepage. (marker_style in config is reserved for a Task 12 menu choice;
// Phase 0 has just this one.)
constexpr char     kMarker[]    = ">> ";
constexpr uint32_t kMarkerColor = 0xFFB7B7FFu;      // faint lavender (ARGB)

// Called from i18n::engine::pump() on the main thread once a translation
// lands. MUST go through the trampoline (g_origAddEntry), never through
// hooked_add_entry() or the patched address itself -- otherwise the line we
// print here would re-enter the detour and get queued for translation again.
void apply_result( const i18n::engine::Result &r ) {
	// Guards against an exception (realistically only std::bad_alloc, from
	// the std::string work below) crossing this C-ABI seam: apply_result()
	// runs from pump_tick() (below), itself a D3D EndScene callback invoked
	// from inside SA:MP/D3D's own C call stack via render::on_frame() -- an
	// escaping exception there is undefined behaviour, not a clean crash.
	// Swallow it; the worst case is the translated line silently doesn't
	// show (the original already did, from hooked_add_entry()).
	try {
		if ( !r.ok || r.displayBytes.empty() ) return; // leave whatever original already showed
		if ( !g_origAddEntry ) return;

		void *chat = samp::chat_ptr();
		if ( !chat ) return; // left the server before the result landed

		std::string marked;
		marked.reserve( sizeof( kMarker ) - 1 + r.displayBytes.size() );
		marked.append( kMarker );
		marked.append( r.displayBytes );

		// engine::process_one() already caps displayBytes to 144 bytes (the
		// crash-safety limit for SA:MP's fixed chat-line buffer) on its own,
		// but prepending kMarker here can push the COMBINED line back over
		// that same cap. marker + displayBytes are cp1252/ASCII -- one byte
		// per character -- so a plain byte-count truncate can't split a
		// multi-byte sequence; that makes it safe here even though it
		// wouldn't be for arbitrary UTF-8 (contrast
		// charset::cap_and_sanitize(), which is UTF-8-boundary-aware).
		constexpr size_t kMaxLine = 144;
		if ( marked.size() > kMaxLine )
			marked.resize( kMaxLine );

		// CHAT_TYPE_DEBUG: a local-only line (no player prefix), the same type
		// samp::chat() uses for our own overlay/status output -- and, since it's
		// never CHAT_TYPE_CHAT/CHAT_TYPE_INFO, hooked_add_entry() would ignore it
		// for translation even if this call somehow went through the patched
		// address (it doesn't -- see above).
		g_origAddEntry( chat, samp::CHAT_TYPE_DEBUG, marked.c_str(), nullptr, kMarkerColor, 0 );
	} catch ( ... ) {
	}
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
	// Deliberately OUTSIDE, and before, the try/catch below: even if the
	// submit work that follows throws, the original line must already be on
	// screen.
	if ( g_origAddEntry )
		g_origAddEntry( self, type, msg, prefix, textColor, prefixColor );

	// Guards against an exception (realistically only std::bad_alloc, from
	// the Job/std::string allocations below) crossing this C-ABI seam:
	// hooked_add_entry() is the detour target, called directly from SA:MP's
	// own code on the game thread -- an escaping exception there is
	// undefined behaviour, not a clean crash. Swallow it; the only effect is
	// that this one line doesn't get queued for translation (the original
	// line already showed, above).
	try {
		// Runtime enable check: the seam is always wired (install() is one-time
		// at startup, race-free), but we only queue work while translation is
		// switched on. apply_config() toggles this live from the menu.
		if ( !i18n::config::get().inbound_chat ) return;

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
	} catch ( ... ) {
	}
}

// Registered via render::on_frame(); drains finished translations on the
// main thread every frame (see engine::pump()'s contract).
void pump_tick( IDirect3DDevice9 * ) {
	i18n::engine::pump();
}

} // namespace

void install() {
	if ( g_installed ) return;

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
		Log( "[i18n] hooks::install: AddEntry detour failed -- inbound translation unavailable." );
		return;
	}
	g_origAddEntry = reinterpret_cast<AddEntryFn>( trampoline );

	render::on_frame( pump_tick );
	i18n::engine::set_apply( &apply_result );

	g_installed = true;
	Log( "[i18n] hooks installed: AddEntry detour + main-thread pump active (idle until enabled)." );
}

void apply_config() {
	auto &s = i18n::config::get();
	if ( !s.inbound_chat ) {
		i18n::engine::stop();
		Log( "[i18n] translation disabled (engine stopped)." );
		return;
	}

	i18n::ProviderConfig pc;
	pc.baseUrl   = s.base_url;
	pc.apiKey    = s.api_key;
	pc.model     = s.model;
	pc.connectMs = 2000;
	pc.readMs    = s.read_timeout_ms > 0 ? s.read_timeout_ms : 30000; // local models cold-load slowly

	int dispCp = i18n::config::display_codepage( s.target_lang, s.roman_output );
	i18n::engine::start( pc,
	                     i18n::config::build_system_prompt( s.target_lang, s.roman_output ),
	                     s.target_lang, dispCp );
	Log( "[i18n] translation ENABLED (target_lang=%d, roman=%d, cp=%d, model=%s).",
	     s.target_lang, (int)s.roman_output, dispCp, s.model.c_str() );
}

} // namespace i18n::hooks
