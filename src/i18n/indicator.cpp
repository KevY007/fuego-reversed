#include "indicator.h"

#include "config.h"
#include "surfaces.h"

#include "../core/input.h"
#include "../samp/samp.h"
#include "../samp/addresses.h"

#include "imgui.h"

#include <windows.h> // GetTickCount
#include <cstdio>

namespace i18n::indicator {

namespace {

// The fading notice (toast_outgoing): its text, colour and when it started.
constexpr uint32_t kToastHoldMs = 1500; // fully visible
constexpr uint32_t kToastFadeMs = 1000; // then fades out
char     g_toastText[160] = {};
ImU32    g_toastColor     = 0;
uint32_t g_toastStart     = 0;
bool     g_toastOn        = false;

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

// A small dark pill, horizontally centred near the top of the screen. `alpha` fades
// both the pill and its text (1 = fully visible).
void pill(ImDrawList* dl, float y, ImU32 col, const char* text, float alpha = 1.0f) {
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const float x = (ImGui::GetIO().DisplaySize.x - sz.x) * 0.5f;
    const ImU32 textA = (col & 0x00FFFFFFu) | ((ImU32)(((col >> 24) & 0xFF) * alpha) << 24);
    dl->AddRectFilled(ImVec2(x - 8, y - 4), ImVec2(x + sz.x + 8, y + sz.y + 4), IM_COL32(0, 0, 0, (int)(170 * alpha)), 5.0f);
    dl->AddText(ImVec2(x, y), textA, text);
}

} // namespace

void toast_outgoing(bool on) {
    if (on)
        std::snprintf(g_toastText, sizeof(g_toastText), "[translate] outgoing ON -- your messages go out in %s",
                      lang_name(config::get().outbound_target_lang));
    else
        std::snprintf(g_toastText, sizeof(g_toastText), "[translate] outgoing OFF");
    g_toastColor = on ? IM_COL32(120, 230, 120, 255) : IM_COL32(255, 120, 120, 255);
    g_toastStart = GetTickCount();
    g_toastOn    = true;
}

void draw() {
    const auto& s = config::get();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    float y = ImGui::GetIO().DisplaySize.y * 0.06f;
    if (g_toastOn) {
        const uint32_t t = GetTickCount() - g_toastStart;
        if (t >= kToastHoldMs + kToastFadeMs) {
            g_toastOn = false;
        } else {
            const float alpha = t <= kToastHoldMs ? 1.0f : 1.0f - (float)(t - kToastHoldMs) / (float)kToastFadeMs;
            pill(dl, y, g_toastColor, g_toastText, alpha);
            y += ImGui::GetFontSize() + 14;
        }
    }
    if (!s.show_translate_indicators) return;
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
