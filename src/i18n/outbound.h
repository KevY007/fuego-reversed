#pragma once
// i18n/outbound.h -- translate MY outgoing chat before it is sent.
//
// Registers a net send-seam handler on RPC_Chat: while enabled, it swallows the
// player's typed chat line, translates it the OPPOSITE way (my language -> the
// server's language, config.outbound_target_lang) via the shared engine using a
// per-job prompt/target override, and sends the translation (or the original, if
// translation fails) through the real RakClient. Commands (/...) always pass
// through untranslated. Toggled live with Ctrl+Shift or the menu; default off.

namespace i18n::engine { struct Result; }

namespace i18n::outbound {

// Register the RPC_Chat send-seam handler. Call once, BEFORE net::install()
// makes the seam live (alongside the other i18n bring-up). Main thread only.
void install();

// Flip outbound translation on/off (Ctrl+Shift / menu), (re)start the engine if
// needed, and print a chat notice so an accidental toggle is always visible.
void toggle();

// Current runtime state (== config.outbound_enabled).
bool enabled();

// Delivers a finished outbound translation: called from hooks::apply_result for
// Result.surface == 1, on the main thread. Sends the translated line (or the
// original on failure) via RakClient.
void on_result(const i18n::engine::Result& r);

} // namespace i18n::outbound
