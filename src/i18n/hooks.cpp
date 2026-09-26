#include "hooks.h"

#include "engine.h"
#include "config.h"
#include "outbound.h"

#include "../samp/samp.h"
#include "../samp/addresses.h"
#include "../hook/CDetour.h"
#include "../render/render.h"
#include "../core/logger.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

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

// The original AddEntry call's formatting, kept while its translation is in
// flight so apply_result() can re-emit the finished line with the SAME
// type/prefix/colours. That REPLACES the line we swallowed in hooked_add_entry()
// rather than adding a second (marked) line on top of it -- the previous
// "original now + translation appended" design showed every line twice.
// Main-thread only (SA:MP calls AddEntry and the pump both run on the game
// thread), so no lock is needed -- same reasoning as outbound's pending map.
struct PendingLine {
	int         type = 0;
	std::string msg;             // original text -> shown if translation declines/fails
	std::string prefix;
	bool        hasPrefix = false;
	uint32_t    textColor = 0;
	uint32_t    prefixColor = 0;
};
std::unordered_map<uint64_t, PendingLine> g_pendingLines;

// Called from i18n::engine::pump() on the main thread once a translation
// lands. MUST go through the trampoline (g_origAddEntry), never through
// hooked_add_entry() or the patched address itself -- otherwise the line we
// print here would re-enter the detour and get queued for translation again.
void apply_result( const i18n::engine::Result &r ) {
	// Outbound translations (surface 1) are SENT to the server, not shown in the
	// local chat window -- route them to the outbound module.
	if ( r.surface == 1 ) { i18n::outbound::on_result( r ); return; }

	// Surface >= 2: non-chat cache-fill jobs (dialogs/labels/textdraws). process_one
	// already stored the result in the translation-memory cache; nothing to display
	// here -- the surface re-emits it from cache on the packet's next arrival.
	if ( r.surface >= 2 ) return;

	// Inbound: emit the finished line exactly ONCE, with the original sender's
	// type/prefix/colours -- REPLACING the line hooked_add_entry() swallowed
	// (never a second line on top). Guarded try/catch: apply_result() runs from
	// pump_tick() inside SA:MP/D3D's C call stack, where an escaping exception is
	// UB, not a clean crash.
	try {
		auto it = g_pendingLines.find( r.handle );
		if ( it == g_pendingLines.end() ) return; // no record -> nothing to emit
		PendingLine pl = std::move( it->second );
		g_pendingLines.erase( it );

		if ( !g_origAddEntry ) return;
		void *chat = samp::chat_ptr();
		if ( !chat ) return; // left the server before the result landed

		// Translated text if we have it; otherwise the original (translation
		// declined -- e.g. already target language / nothing translatable -- or
		// failed). Either way the line appears once, in its native formatting.
		const std::string &text = ( r.ok && !r.displayBytes.empty() ) ? r.displayBytes : pl.msg;
		const char *prefix = pl.hasPrefix ? pl.prefix.c_str() : nullptr;
		g_origAddEntry( chat, pl.type, text.c_str(), prefix, pl.textColor, pl.prefixColor );
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
	// Decide whether to translate this line. If we will, we must NOT show the
	// original now -- we swallow it and apply_result() emits the finished line
	// once (translated, or original on failure). Showing it here too is exactly
	// the "double text" bug. Never CHAT_TYPE_DEBUG (our own / re-emitted lines).
	bool doTranslate = false;
	try {
		if ( i18n::config::get().inbound_chat && msg && msg[0] &&
		     ( type == samp::CHAT_TYPE_CHAT || type == samp::CHAT_TYPE_INFO ) )
			doTranslate = true;
	} catch ( ... ) {}

	if ( !doTranslate ) {
		// Not translating -> show the line immediately, unchanged, via the real
		// AddEntry (the trampoline, never the patched address -> no re-entry).
		if ( g_origAddEntry )
			g_origAddEntry( self, type, msg, prefix, textColor, prefixColor );
		return;
	}

	// Translating: SWALLOW the original (do not show it now), remember its
	// formatting, and queue the job. Guarded: an escaping exception across this
	// C-ABI detour seam is UB. If queueing throws, fall back to showing the
	// original so the line is never lost.
	try {
		uint64_t h = g_seq.fetch_add( 1, std::memory_order_relaxed );

		PendingLine pl;
		pl.type        = type;
		pl.msg         = msg;
		pl.hasPrefix   = ( prefix != nullptr );
		if ( prefix ) pl.prefix = prefix;
		pl.textColor   = textColor;
		pl.prefixColor = prefixColor;
		g_pendingLines[h] = std::move( pl );

		i18n::engine::Job job;
		job.handle      = h;
		job.surface     = 0; // inbound chat display
		job.sourceBytes = msg;
		job.srcCodepage = i18n::config::get().source_cp; // 0 = auto-detect
		job.noDedup     = true; // every swallowed line must produce exactly one shown line
		i18n::engine::submit( job );
	} catch ( ... ) {
		if ( g_origAddEntry )
			g_origAddEntry( self, type, msg, prefix, textColor, prefixColor );
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
	// The one worker engine serves every surface (inbound chat display, outbound
	// send, and the dialog/label cache-fill jobs), so it runs whenever ANY of them
	// is enabled.
	if ( !s.inbound_chat && !s.outbound_enabled && !s.translate_dialogs ) {
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
	Log( "[i18n] engine running (inbound=%d target=%d roman=%d cp=%d | outbound=%d server=%d | model=%s).",
	     (int)s.inbound_chat, s.target_lang, (int)s.roman_output, dispCp,
	     (int)s.outbound_enabled, s.outbound_target_lang, s.model.c_str() );
}

} // namespace i18n::hooks
