#include "lab.h"

#include "config.h"
#include "surfaces.h"
#include "chatbuf.h"

#include "../samp/samp.h"
#include "../net/RakClient.h"
#include "../core/logger.h"

#include <cstdint>
#include <string>

namespace i18n::lab {

namespace {
std::string g_lastBottom;
} // namespace

void on_incoming_rpc(unsigned char id, RPCParameters* rpc) {
    if (!i18n::config::get().debug_lab) return;
    if (id != 61 || !rpc || !rpc->input) return;

    const uint8_t* p = rpc->input;
    const size_t n = (size_t)((rpc->numberOfBitsOfData + 7) / 8);

    i18n::surfaces::DialogPlan plan;
    if (!i18n::surfaces::plan_dialog(p, n, plan)) return; // malformed -> log nothing

    const std::string title = i18n::surfaces::unit_text(plan.units[0], plan.units[0].core);
    const std::string btn1  = i18n::surfaces::unit_text(plan.units[1], plan.units[1].core);
    const std::string btn2  = i18n::surfaces::unit_text(plan.units[2], plan.units[2].core);

    // Password dialogs (style 3) never have their body logged. Build that string
    // BEFORE touching the body at all -- no password body is ever assembled.
    std::string body;
    if (plan.style == 3) {
        body = "(password dialog body not logged)";
    } else if (plan.haveBody) {
        body = i18n::surfaces::assemble_body(plan, i18n::surfaces::original_cores(plan));
    }

    Log("[lab] dialog RPC61: id=%d style=%d title='%s' btn1='%s' btn2='%s' body='%.120s'",
        (int)(int16_t)plan.id, (int)plan.style, title.c_str(), btn1.c_str(), btn2.c_str(), body.c_str());
}

void on_frame() {
    if (!i18n::config::get().debug_lab) return;

    const uint8_t* chat = (const uint8_t*)samp::chat_ptr();
    if (!chat) return;

    std::vector<i18n::chatbuf::Piece> newest = i18n::chatbuf::read_newest(chat, 1);
    if (newest.empty() || newest[0].text.empty()) return;
    if (newest[0].text == g_lastBottom) return;
    g_lastBottom = newest[0].text;
    Log("[lab] chat[99]: '%s'", g_lastBottom.c_str());
}

} // namespace i18n::lab
