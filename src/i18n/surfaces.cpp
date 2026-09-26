#include "surfaces.h"

#include "config.h"
#include "engine.h"

#include "../net/RakClient.h"   // RPCParameters
#include "../net/BitStream.h"

#include <atomic>
#include <cstdint>

// The pure rebuild_dialog()/rebuild_label() live in surfaces_rebuild.cpp (no game
// types) so they can be host-tested. This TU is the game/engine glue.

namespace i18n::surfaces {

namespace {

std::atomic<uint64_t> g_seq{ 1 };

// Queue a background cache-fill translation (surface 2 -> apply_result ignores it;
// process_one() translates through the full pipeline -- mask/glossary/validate --
// and stores the result in the TM cache). Dedup ON, so a repeatedly-seen miss is
// only translated once.
void queue_fill(const std::string& src) {
    if (src.size() < 2) return;
    i18n::engine::Job j;
    j.handle      = g_seq.fetch_add(1, std::memory_order_relaxed);
    j.surface     = 2;
    j.sourceBytes = src;
    j.srcCodepage = i18n::config::get().source_cp;
    j.skipContext = true;
    i18n::engine::submit(j);
}

// TM-first translator for a piece of text: cache hit -> out=translation, true;
// trivial string (<2 chars, e.g. a lone space) -> keep as-is, true; miss -> queue
// a fill and return false (caller shows the original this time).
bool tm_translate(const std::string& src, std::string& out) {
    if (src.size() < 2) { out = src; return true; }
    auto& s = i18n::config::get();
    if (i18n::engine::cache_lookup(src, s.source_cp, s.target_lang, out)) return true;
    queue_fill(src);
    return false;
}

// HUD variant: additionally SKIP dynamic/numeric-heavy strings (timers, counters,
// stats) so a textdraw that updates every tick with a changing number never spams
// the translator -- return false (pass original) without queueing anything.
bool tm_translate_hud(const std::string& src, std::string& out) {
    if (is_dynamic_text(src)) return false;
    return tm_translate(src, out);
}

// Replace the RPC's payload with `pkt` (all byte-aligned). Mirrors compat's
// incoming-RPC rewrite: build a BitStream, CopyData into rpc->input, set the bit
// count. RenderWare/SA:MP then parses our rebuilt packet exactly as a real one.
void commit(RPCParameters* rpc, const std::vector<uint8_t>& pkt) {
    BitStream bs;
    if (!pkt.empty()) bs.Write((char*)pkt.data(), (int)pkt.size());
    bs.CopyData(&rpc->input);
    rpc->numberOfBitsOfData = bs.GetNumberOfBitsUsed();
}

} // namespace

void on_incoming_rpc(unsigned char id, RPCParameters* rpc) {
    if (!rpc || !rpc->input || rpc->numberOfBitsOfData < 8) return;
    auto& s = i18n::config::get();
    const uint8_t* p = rpc->input;
    size_t n = (size_t)((rpc->numberOfBitsOfData + 7) / 8);

    try {
        std::vector<uint8_t> out;
        bool ok = false;
        if (s.translate_dialogs && (id == 61 || id == 36)) {
            ok = (id == 61) ? rebuild_dialog(p, n, tm_translate, out)
                            : rebuild_label(p, n, tm_translate, out);
        } else if (s.translate_hud && (id == 73 || id == 59 || id == 105 || id == 134)) {
            switch (id) {
                case 73:  ok = rebuild_gametext(p, n, tm_translate_hud, out); break;
                case 59:  ok = rebuild_chatbubble(p, n, tm_translate_hud, out); break;
                case 105: ok = rebuild_textdraw_setstring(p, n, tm_translate_hud, out); break;
                case 134: ok = rebuild_textdraw_show(p, n, tm_translate_hud, out); break;
            }
        } else {
            return;
        }
        if (ok && !out.empty()) commit(rpc, out);
    } catch (...) {
        // Never let an exception cross back into SA:MP's RPC dispatch.
    }
}

} // namespace i18n::surfaces
