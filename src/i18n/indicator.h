#pragma once

namespace i18n::indicator {

// On-screen translation hints, drawn inside the ImGui frame on the background draw
// list: "your message will be sent in <lang>" while typing in chat or while an INPUT
// dialog is open with outgoing translation on, and "translating dialog..." while a
// dialog is held for translation. No-op unless config.show_translate_indicators.
void draw();

// The outgoing-translation hotkey flipped it ON/OFF: a short notice at the top of
// the screen that fades out by itself (~2.5 s) -- instead of a chat line, so the
// chat stays clean. Shown even with the translation hints turned off (it answers
// a key the player just pressed). Main thread.
void toast_outgoing(bool on);

} // namespace i18n::indicator
