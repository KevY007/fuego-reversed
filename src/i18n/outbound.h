#pragma once
// i18n/outbound.h -- translate MY outgoing chat before it is sent.
//
// Registers a net send-seam handler on RPC_Chat: while enabled (and only while the
// per-frame pump runs -- hooks::pump_ready()), it swallows the player's typed chat
// line, translates it the OPPOSITE way (my language -> the server's language,
// config.outbound_target_lang) via the shared engine using a per-job prompt/target
// override, and sends the translation through the real RakClient. A swallowed line
// ALWAYS reaches the server: as typed if the translation fails, hasn't landed
// within 5 s (tick), or the engine is about to be restarted/stopped
// (flush_pending). Commands (/...) always pass through untranslated. Toggled live
// with a settable hotkey (default Ctrl+Shift) or the menu; default off.

#include <cstddef>
#include <cstdint>

namespace i18n::engine { struct Result; }

namespace i18n::outbound {

// Register the RPC_Chat send-seam handler. Call once, BEFORE net::install()
// makes the seam live (alongside the other i18n bring-up). Main thread only.
void install();

// Flip outbound translation on/off (hotkey, default Ctrl+Shift, or menu), (re)start the engine if
// needed, and print a chat notice so an accidental toggle is always visible.
void toggle();

// Current runtime state (== config.outbound_enabled).
bool enabled();

// Delivers a finished outbound translation: called from hooks::apply_result for
// Result.surface == 1, on the main thread. Sends the translated line (or the
// original on failure) via RakClient. A result whose line was already sent as
// typed (deadline / flush) is ignored.
void on_result(const i18n::engine::Result& r);

// Guaranteed delivery: sends the ORIGINAL of every swallowed line still waiting
// for its translation after 5 s. Call every frame (hooks::pump_tick). Main thread.
void tick(uint32_t nowMs);

// Sends the ORIGINAL of every swallowed line right now, in the order typed, and
// forgets them. Call before the engine is stopped or restarted (engine::stop()/
// start() drop every queued job). Main thread.
void flush_pending();

// A new game session began (InitGame): forget every swallowed line WITHOUT
// sending it -- it was typed to the previous server/session. A result that still
// comes back finds no entry and is ignored. Returns how many were dropped. Main
// thread.
size_t discard_pending();

} // namespace i18n::outbound
