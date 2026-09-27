#pragma once

namespace i18n::indicator {

// On-screen translation hints, drawn inside the ImGui frame on the background draw
// list: "your message will be sent in <lang>" while typing in chat or while an INPUT
// dialog is open with outgoing translation on, and "translating dialog..." while a
// dialog is held for translation. No-op unless config.show_translate_indicators.
void draw();

} // namespace i18n::indicator
