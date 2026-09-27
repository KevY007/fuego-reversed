#include "hooks.h"

#include "engine.h"
#include "config.h"
#include "outbound.h"
#include "lab.h"
#include "names.h"
#include "chatbuf.h"
#include "surfaces.h"

#include "../samp/samp.h"
#include "../samp/addresses.h"
#include "../hook/CDetour.h"
#include "../render/render.h"
#include "../core/logger.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

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

// Tracks whether the worker engine is currently running, and a signature of the
// settings it was last started with -- so apply_config() (called on every menu
// Save AND every outbound-toggle hotkey press) can skip a same-settings restart.
// See apply_config()'s comment (hooks.h) for why a needless restart is costly.
bool        g_engineOn = false;
std::string g_engineSig;

// Everything that actually changes what the running engine does: every
// ProviderConfig field apply_config() sets, plus the system prompt, target
// language and display codepage engine::start() also takes. Fields apply_config()
// never varies (topP, maxTokens) are deliberately left out -- they can't change,
// so they'd never make two signatures differ anyway. NEVER logged: it embeds the
// API key.
std::string engine_signature( const i18n::ProviderConfig &pc, const std::string &prompt,
                               int targetLang, int displayCp ) {
	const char sep = '\x01';
	std::string sig;
	sig += pc.baseUrl;                           sig += sep;
	sig += pc.apiKey;                            sig += sep;
	sig += pc.model;                             sig += sep;
	sig += std::to_string( pc.providerKind );    sig += sep;
	sig += std::to_string( pc.connectMs );       sig += sep;
	sig += std::to_string( pc.readMs );          sig += sep;
	sig += std::to_string( pc.temperature );     sig += sep;
	sig += std::to_string( pc.minRequestGapMs ); sig += sep;
	sig += prompt;                               sig += sep;
	sig += std::to_string( targetLang );         sig += sep;
	sig += std::to_string( displayCp );
	return sig;
}

// A line shown in its ORIGINAL form while its translation is in flight: the exact
// chat entries it became (AddEntry may have wrapped it into several), identified by
// SystemTime + text, so the translation can be written back over THOSE rows in
// place when it lands -- lines above/below never move (Option A). Main thread only
// (SA:MP calls AddEntry and the pump both run on the game thread).
struct PendingFlip {
	std::vector<i18n::chatbuf::Piece> pieces;
};
std::unordered_map<uint64_t, PendingFlip> g_pendingFlips;

// Called from i18n::engine::pump() on the main thread once a translation lands.
// Routes each result by surface. Nothing here may call the patched AddEntry.
void apply_result( const i18n::engine::Result &r ) {
	// Outbound chat (surface 1) is SENT to the server, not shown.
	if ( r.surface == 1 ) { i18n::outbound::on_result( r ); return; }
	// Dialog unit translations (surface 3) complete a held dialog.
	if ( r.surface == 3 ) { i18n::surfaces::on_part_result( r ); return; }
	// Outgoing dialog-input translations (surface 4) are sent to the server.
	if ( r.surface == 4 ) { i18n::surfaces::on_response_result( r ); return; }
	// Surface 2: translation-memory fill (labels/HUD, and the background batches of
	// a dialog shown right away because translating it would take too long) --
	// already cached by process_one()/process_batch(); nothing to show.
	if ( r.surface != 0 ) return;

	// Inbound chat (surface 0): overwrite the rows the original line became with the
	// translation, in place. Declined/failed -> the original simply stays. Guarded:
	// this runs inside SA:MP/D3D's C call stack, where an escaping exception is UB.
	try {
		auto it = g_pendingFlips.find( r.handle );
		if ( it == g_pendingFlips.end() ) return;
		PendingFlip pf = std::move( it->second );
		g_pendingFlips.erase( it );
		if ( !r.ok || r.displayBytes.empty() ) return;

		uint8_t *chat = static_cast<uint8_t *>( samp::chat_ptr() );
		if ( !chat ) return; // left the server before the result landed
		int first = i18n::chatbuf::find_block( chat, pf.pieces );
		if ( first < 0 ) return; // scrolled off (or cleared) before the translation came back

		// SA:MP wraps a CHAT line at exactly 72 bytes (verified in samp.dll 0.3.DL) --
		// use the rows' actual width if the original rows were wider.
		size_t width = 72;
		for ( const auto &p : pf.pieces )
			if ( p.text.size() > width ) width = p.text.size(); // (no std::max: windows.h defines max)
		i18n::chatbuf::write_block( chat, first,
		                            i18n::chatbuf::wrap_to( r.displayBytes, (int) pf.pieces.size(), width ) );
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
// itself. apply_result() above no longer calls AddEntry at all -- a landed
// translation is written directly into the chat buffer's rows instead. That
// means BOTH "show original" and "show translated" bypass the patch entirely:
// hooked_add_entry() only ever runs for genuine external calls to the real
// AddEntry address, never for lines we ourselves are displaying -- no loop,
// and a translated line is never itself resubmitted for translation.
void __fastcall hooked_add_entry( void *self, void * /*edx*/, int type, const char *msg,
                                   const char *prefix, uint32_t textColor, uint32_t prefixColor ) {
	bool doTranslate = false;
	try {
		if ( i18n::config::get().inbound_chat && msg && msg[0] &&
		     ( type == samp::CHAT_TYPE_CHAT || type == samp::CHAT_TYPE_INFO ) )
			doTranslate = true;
	} catch ( ... ) {}

	if ( !doTranslate ) {
		if ( g_origAddEntry ) g_origAddEntry( self, type, msg, prefix, textColor, prefixColor );
		return;
	}

	// Option A: the line ALWAYS shows immediately (live chat, exact order + timing).
	// `shown` guards the catch below so a line is never lost and never shown twice.
	bool shown = false;
	try {
		auto &s = i18n::config::get();

		// Translation-memory hit: show it ALREADY translated -- SA:MP wraps it with its
		// own rules, and there is nothing to flip later.
		std::string cached;
		if ( i18n::engine::cache_lookup( msg, s.source_cp, s.target_lang, cached ) && !cached.empty() ) {
			g_origAddEntry( self, type, cached.c_str(), prefix, textColor, prefixColor );
			shown = true;
			return;
		}

		// Miss: show the original now and remember exactly which rows it became.
		const uint8_t *chat = static_cast<const uint8_t *>( self );
		std::vector<uint8_t> snap = i18n::chatbuf::snapshot_newest( chat );
		g_origAddEntry( self, type, msg, prefix, textColor, prefixColor );
		shown = true;

		int n = i18n::chatbuf::added_since( chat, snap );
		if ( n <= 0 || n > 4 ) return; // can't tell which rows it became -> leave it as is

		if ( g_pendingFlips.size() > 256 ) g_pendingFlips.clear(); // results that never came back
		uint64_t h = g_seq.fetch_add( 1, std::memory_order_relaxed );
		g_pendingFlips[h].pieces = i18n::chatbuf::read_newest( chat, n );

		i18n::engine::Job job;
		job.handle      = h;
		job.surface     = 0; // inbound chat -> apply_result flips the rows in place
		job.sourceBytes = msg;
		job.srcCodepage = s.source_cp; // 0 = auto-detect
		job.noDedup     = true;        // every shown line gets its own flip
		i18n::engine::submit( job );
	} catch ( ... ) {
		if ( !shown && g_origAddEntry ) g_origAddEntry( self, type, msg, prefix, textColor, prefixColor );
	}
}

// Registered via render::on_frame(); drains finished translations on the
// main thread every frame (see engine::pump()'s contract).
void pump_tick( IDirect3DDevice9 * ) {
	// Runs inside hkEndScene (a D3D9 vtable hook) -- no C++ exception may escape this seam.
	try {
		i18n::engine::pump();
		const uint32_t now = GetTickCount();
		i18n::outbound::tick( now ); // swallowed chat past its deadline goes out as typed
		i18n::names::tick( now );    // live player names -> masked, never translated
		i18n::surfaces::tick( now ); // release held dialogs that are due
		i18n::lab::on_frame();       // debug lab (read-only logging)
	} catch ( ... ) {
	}
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

// install() registered pump_tick AND render's EndScene hook, which calls it every
// frame, went live -- only then does a submitted job's result ever come back.
bool pump_ready() { return g_installed && render::installed(); }

void apply_config() {
	// Called from the menu and the outbound hotkey INSIDE hkEndScene (a D3D9 vtable
	// hook -- a C-ABI seam), and engine::start() can throw (std::thread): nothing may
	// escape. `engineTouched` is set right before engine::stop()/start(): if either
	// throws, the engine's real state is unknown, so stop whatever is left and mark
	// it OFF with no signature -- the next apply_config() then retries a clean start
	// instead of skipping it as "unchanged".
	bool engineTouched = false;
	try {
		auto &s = i18n::config::get();
		// The one worker engine serves every surface (inbound chat display, outbound
		// send, and the dialog/label cache-fill jobs), so it runs whenever ANY of them
		// is enabled.
		if ( !s.inbound_chat && !s.outbound_enabled && !s.translate_dialogs && !s.translate_hud ) {
			if ( g_engineOn ) {
				i18n::surfaces::flush_responses(); // about to drop the queue -- never lose a swallowed reply
				i18n::outbound::flush_pending();   // ... or a swallowed chat line
				engineTouched = true;
				i18n::engine::stop();
				g_engineOn = false;
				g_engineSig.clear();
			}
			Log( "[i18n] translation disabled (engine stopped)." );
			return;
		}

		i18n::ProviderConfig pc;
		pc.baseUrl      = s.base_url;
		pc.apiKey       = s.api_key;
		pc.model        = s.model;
		pc.providerKind = s.provider_kind; // per-provider request quirks (see provider.h)
		pc.connectMs = 2000;
		pc.readMs    = s.read_timeout_ms > 0 ? s.read_timeout_ms : 30000; // local models cold-load slowly
		pc.temperature = ( s.translate_temp >= 0 ? s.translate_temp : 30 ) / 100.0; // 30 -> 0.30
		pc.minRequestGapMs = s.request_min_gap_ms; // request pacer (free-tier RPM limits)

		int dispCp = i18n::config::display_codepage( s.target_lang, s.roman_output );
		std::string prompt = i18n::config::build_system_prompt( s.target_lang, s.roman_output, s.source_lang );
		std::string sig = engine_signature( pc, prompt, s.target_lang, dispCp );

		// Nothing that actually matters changed (the outbound hotkey and plenty of menu
		// widgets call this on every press/Save): keep the running engine as-is. See
		// apply_config()'s doc comment (hooks.h) for why restarting is costly.
		if ( g_engineOn && sig == g_engineSig ) {
			Log( "[i18n] settings applied (engine unchanged)." );
			return;
		}

		i18n::surfaces::flush_responses(); // the queue below is about to be dropped
		i18n::outbound::flush_pending();
		engineTouched = true;
		i18n::engine::start( pc, prompt, s.target_lang, dispCp );
		g_engineOn  = true;
		g_engineSig = std::move( sig );
		Log( "[i18n] engine running (inbound=%d target=%d roman=%d cp=%d | outbound=%d server=%d | model=%s).",
		     (int)s.inbound_chat, s.target_lang, (int)s.roman_output, dispCp,
		     (int)s.outbound_enabled, s.outbound_target_lang, s.model.c_str() );
	} catch ( ... ) {
		if ( engineTouched ) {
			try { i18n::engine::stop(); } catch ( ... ) {} // a half-started pool must not linger
			g_engineOn = false;
			g_engineSig.clear(); // never matches -> the next apply_config() retries start()
		}
		Log( "[i18n] applying translator settings failed (exception)%s.",
		     engineTouched ? " -- engine is off; the next Save retries" : " -- engine left as it was" );
	}
}

} // namespace i18n::hooks
