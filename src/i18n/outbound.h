#pragma once
// i18n/outbound.h -- translate MY outgoing chat before it is sent.
//
// Registers a net send-seam handler on RPC_Chat and RPC_ServerCommand: while
// enabled (and only while the per-frame pump runs -- hooks::pump_ready()), it
// swallows the player's typed chat line, translates it the OPPOSITE way (my
// language -> the server's language, config.outbound_target_lang) via the shared
// engine using a per-job prompt/target override, and sends the translation through
// the real RakClient. A message command (/pm 35 ..., /me ..., /b ..., see
// protect::split_message_command) is handled the same way, but only its message is
// translated -- the command and its target go out exactly as typed; every other
// command passes through untranslated. A swallowed line ALWAYS reaches the server:
// as typed if the translation fails, hasn't landed within 5 s (tick), or the engine
// is about to be restarted/stopped (flush_pending). Toggled live with a settable
// hotkey (default Ctrl+Shift) or the menu; default off.

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
// Result.surface == 1, on the main thread. The line goes out through RakClient --
// translated (cut at a word to fit the input's 127 bytes), or as typed on failure
// -- once every line typed before it has gone out, and no sooner than the gap the
// player left after the previous one (lines leave strictly in the order typed; a
// line that needs no translation but was typed behind a waiting one queues too).
// A result whose line was already sent as typed (deadline / flush) is ignored.
void on_result(const i18n::engine::Result& r);

// Sends the lines whose turn has come, in order: translated, or -- 5 s after it was
// typed with no translation yet -- as typed. Call every frame (hooks::pump_tick).
// Main thread.
void tick(uint32_t nowMs);

// Sends every swallowed line right now, in the order typed -- translated where its
// translation is already in, else as typed -- and forgets them. Call before the
// engine is stopped or restarted (engine::stop()/start() drop every queued job).
// Main thread.
void flush_pending();

// A new game session began (InitGame): forget every swallowed line WITHOUT
// sending it -- it was typed to the previous server/session. A result that still
// comes back finds no entry and is ignored. Returns how many were dropped. Main
// thread.
size_t discard_pending();

} // namespace i18n::outbound
