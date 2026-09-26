#include "surfaces.h"

#include "config.h"
#include "engine.h"

#include "../net/net.h"         // on_send_rpc (DialogResponse reverse-map)
#include "../net/RakClient.h"   // RPCParameters, g_RakClient
#include "../net/BitStream.h"
#include "../core/logger.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// The pure rebuild_dialog()/rebuild_label() live in surfaces_rebuild.cpp (no game
// types) so they can be host-tested. This TU is the game/engine glue.

namespace i18n::surfaces {

namespace {

std::atomic<uint64_t> g_seq{ 1 };

// List-dialog selection reverse-map (translated row text -> original row text), so a
// selection on a TRANSLATED list dialog still sends what a text-matching server
// expects. Populated when a multi-line dialog body is translated; read on outbound
// RPC 62. Main/net thread only (incoming dispatch + outgoing send both run there).
std::unordered_map<std::string, std::string> g_listRevMap;

void split_lines(const std::string& s, std::vector<std::string>& out) {
    out.clear();
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == '\n') { out.push_back(s.substr(start, i - start)); start = i + 1; }
}

// Record translated<->original rows of a translated multi-line body (a list dialog's
// items are its newline-separated body). Harmless for non-list bodies (unused unless
// a matching RPC 62 arrives). Capped to bound memory.
void note_list_rows(const std::string& original, const std::string& translated) {
    if (original.find('\n') == std::string::npos) return;
    std::vector<std::string> o, t;
    split_lines(original, o);
    split_lines(translated, t);
    if (o.size() != t.size()) return;
    if (g_listRevMap.size() > 500) g_listRevMap.clear();
    for (size_t i = 0; i < t.size(); ++i)
        if (!t[i].empty() && t[i] != o[i]) g_listRevMap[t[i]] = o[i];
}

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
    if (i18n::engine::cache_lookup(src, s.source_cp, s.target_lang, out)) {
        note_list_rows(src, out); // remember rows in case this is a list dialog body
        return true;
    }
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

// Outbound RPC 62 (DialogResponse): if the selected LIST-row text is one we recorded
// as a translation, re-send it with the ORIGINAL row text so a text-matching server
// recognises the selection. Bypasses the seam via g_RakClient (like outbound chat) ->
// no re-entry. Index-based servers are unaffected (wListItem is unchanged).
bool on_send_dialogresponse(int id, BitStream* params, PacketPriority, PacketReliability, char, bool) {
    if (id != 62 || !params) return true;
    if (!i18n::config::get().translate_dialogs || g_listRevMap.empty()) return true;

    params->ResetReadPointer();
    int16_t dlgId = 0, listItem = 0;
    uint8_t resp = 0, tlen = 0;
    if (!params->Read(dlgId) || !params->Read(resp) || !params->Read(listItem) || !params->Read(tlen)
        || tlen == 0 || tlen > 200) { params->ResetReadPointer(); return true; }
    char buf[256] = {};
    if (!params->Read(buf, tlen)) { params->ResetReadPointer(); return true; }
    params->ResetReadPointer();

    auto it = g_listRevMap.find(std::string(buf, tlen));
    if (it == g_listRevMap.end()) return true; // not a translated row -> send unchanged

    const std::string& orig = it->second;
    if (!g_RakClient) return true;
    size_t ol = orig.size() > 255 ? 255 : orig.size();
    BitStream bs;
    bs.Write(dlgId);
    bs.Write(resp);
    bs.Write(listItem);
    bs.Write((uint8_t)ol);
    if (ol) bs.Write((char*)orig.data(), (int)ol);
    g_RakClient->RPC(62, &bs);
    return false; // swallow the translated-text response; we sent the original
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

void install() {
    net::on_send_rpc(on_send_dialogresponse);
    Log("[i18n] surfaces: dialog-response reverse-map registered.");
}

} // namespace i18n::surfaces
