#include "indicator.h"

#include "config.h"
#include "surfaces.h"

#include "../core/input.h"
#include "../samp/samp.h"
#include "../samp/addresses.h"

#include "imgui.h"

#include <cstdio>

namespace i18n::indicator {

namespace {

// An INPUT-style dialog is open (SA:MP's own state: iIsActive, iType).
bool input_dialog_open() {
    const uint32_t base = samp::base();
    if (!base) return false;
    const uint8_t* dlg = *reinterpret_cast<const uint8_t* const*>(base + samp::addr::DIALOG_INFO);
    if (!dlg) return false;
    return *reinterpret_cast<const int*>(dlg + samp::addr::DIALOG_ACTIVE) != 0 &&
           *reinterpret_cast<const int*>(dlg + samp::addr::DIALOG_TYPE) == 1;
}

const char* lang_name(int code) {
    for (const auto& o : config::language_options())
        if (o.code == code) return o.name;
    return "the server's language";
}

// A small dark pill, horizontally centred near the top of the screen.
void pill(ImDrawList* dl, float y, ImU32 col, const char* text) {
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const float x = (ImGui::GetIO().DisplaySize.x - sz.x) * 0.5f;
    dl->AddRectFilled(ImVec2(x - 8, y - 4), ImVec2(x + sz.x + 8, y + sz.y + 4), IM_COL32(0, 0, 0, 170), 5.0f);
    dl->AddText(ImVec2(x, y), col, text);
}

} // namespace

void draw() {
    const auto& s = config::get();
    if (!s.show_translate_indicators) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float y = ImGui::GetIO().DisplaySize.y * 0.06f;
    char buf[160];
    if (s.outbound_enabled && (input::typing() || input_dialog_open())) {
        std::snprintf(buf, sizeof(buf), "[translate] your message will be sent in %s", lang_name(s.outbound_target_lang));
        pill(dl, y, IM_COL32(120, 230, 120, 255), buf);
        y += ImGui::GetFontSize() + 14;
    }
    if (s.translate_dialogs && surfaces::holding())
        pill(dl, y, IM_COL32(235, 200, 90, 255), "[translate] translating dialog...");
}

} // namespace i18n::indicator
