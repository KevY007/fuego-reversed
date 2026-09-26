#include "capture.h"

#include "config.h"
#include "../net/RakClient.h"   // RPCParameters
#include "../core/logger.h"

#include <string>
#include <cstdint>

namespace i18n::capture {

namespace {

// Hex-dump up to `cap` bytes (dialog/label blobs are small; cap keeps the log sane).
std::string hex(const unsigned char* d, int n, int cap = 700) {
    static const char* H = "0123456789ABCDEF";
    int m = (n < cap) ? n : cap;
    std::string s;
    s.reserve((size_t)m * 2 + 16);
    for (int i = 0; i < m; ++i) { s += H[(d[i] >> 4) & 0xF]; s += H[d[i] & 0xF]; }
    if (n > cap) s += "..(+more)";
    return s;
}

// RakNet is little-endian on the wire.
inline uint16_t rd16(const unsigned char* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t rd32(const unsigned char* p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

// Printable-ASCII copy of a byte range (non-printables -> '.') for a readable log line.
std::string ascii(const unsigned char* p, int n) {
    std::string s; s.reserve((size_t)n);
    for (int i = 0; i < n; ++i) { unsigned char c = p[i]; s += (c >= 0x20 && c < 0x7F) ? (char)c : '.'; }
    return s;
}

} // namespace

void on_incoming_rpc(unsigned char id, RPCParameters* rpc) {
    if (!i18n::config::get().debug_capture) return;
    if (!rpc || !rpc->input || rpc->numberOfBitsOfData < 8) return;

    // Only the server-text surfaces we intend to translate.
    if (id != 61 && id != 36 && id != 134 && id != 105 && id != 73 && id != 93 && id != 120) return;

    const unsigned char* p = rpc->input;
    const int nbytes = (int)((rpc->numberOfBitsOfData + 7) / 8);
    if (nbytes <= 0) return;

    const char* name =
        id == 61  ? "ShowDialog" :
        id == 36  ? "Create3DTextLabel" :
        id == 134 ? "ShowTextDraw" :
        id == 105 ? "TextDrawSetString" :
        id == 73  ? "ShowGameText" :
        id == 93  ? "ClientMessage" :
        id == 120 ? "SetObjectMaterial(Text)" : "?";

    Log("[capture] RPC %u (%s) bits=%u bytes=%d hex=%s",
        (unsigned)id, name, rpc->numberOfBitsOfData, nbytes, hex(p, nbytes).c_str());

    // Convenience decodes of the byte-aligned plain fields (everything before a
    // CSTRING body is byte-aligned, so we can index raw bytes safely). All reads
    // are bounds-checked; a short/odd packet just skips the convenience line.
    if (id == 61 && nbytes >= 4) {
        // u16 dlgId, u8 style, u8 titleLen + title, u8 b1Len + b1, u8 b2Len + b2, CSTRING szInfo
        int off = 0;
        uint16_t dlg = rd16(p + off); off += 2;
        uint8_t style = p[off]; off += 1;
        auto plain = [&](std::string& out) -> bool {
            if (off >= nbytes) return false;
            int len = p[off]; off += 1;
            if (off + len > nbytes) return false;
            out.assign((const char*)(p + off), (size_t)len); off += len; return true;
        };
        std::string title, b1, b2;
        if (plain(title) && plain(b1) && plain(b2)) {
            int szOff = off, szLen = nbytes - off;
            Log("[capture]   dialog id=%u style=%u title='%s' btn1='%s' btn2='%s'",
                dlg, style, title.c_str(), b1.c_str(), b2.c_str());
            if (szLen > 0)
                Log("[capture]   szInfo(CSTRING) byteOffset=%d len=%d hex=%s",
                    szOff, szLen, hex(p + szOff, szLen).c_str());
        }
    } else if (id == 36 && nbytes >= 27) {
        // u16 id, u32 color, f x,y,z, f dist, u8 testLOS, u16 aPlayer, u16 aVeh, CSTRING text
        uint16_t lbl = rd16(p);
        int txtOff = 27, txtLen = nbytes - 27;
        Log("[capture]   label id=%u text(CSTRING) byteOffset=%d len=%d hex=%s",
            lbl, txtOff, txtLen, (txtLen > 0 ? hex(p + txtOff, txtLen).c_str() : ""));
    } else if (id == 105 && nbytes >= 4) {
        // u16 textdrawId, u16 len, char[] text (plain)
        uint16_t tdid = rd16(p);
        int len = (int)rd16(p + 2);
        int avail = nbytes - 4; if (len > avail) len = avail;
        if (len > 0) Log("[capture]   textdraw id=%u setstring len=%d text='%s'",
                         tdid, (int)rd16(p + 2), ascii(p + 4, len).c_str());
    } else if (id == 73 && nbytes >= 12) {
        // u32 style, u32 time, u32 msgLen, char[] message (plain)
        uint32_t style = rd32(p), time = rd32(p + 4), len = rd32(p + 8);
        int avail = nbytes - 12; int show = (len < (uint32_t)avail) ? (int)len : avail;
        if (show > 0) Log("[capture]   gametext style=%u time=%u len=%u text='%s'",
                         style, time, len, ascii(p + 12, show).c_str());
    }
}

} // namespace i18n::capture
