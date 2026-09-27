#include "lab.h"

#include "config.h"
#include "huffman.h"

#include "../samp/samp.h"       // samp::base()
#include "../net/RakClient.h"   // RPCParameters
#include "../core/logger.h"

#include <windows.h>            // GetTickCount + SEH (__try/__except)
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

namespace i18n::lab {

namespace {

// --- samp.dll offsets, CONFIRMED by disassembly of the user's samp.dll (0.3.DL,
//     2026-09-27). See the translation-layer memory notes. ---------------------
// Chat: singleton ptr @ base+0x2ACA10; chatEntry[0] @ this+0x132, stride 0xFC,
//   szText @ entry+0x20 (144 bytes). The dirty/redraw flag AddEntry itself sets is
//   @ this+0x63DA (NOT the header's m_iRedraw=+0x63D6, which is only "texture valid").
constexpr uint32_t OFF_CHAT_PTR    = 0x2ACA10;
constexpr uint32_t OFF_CHAT_ENTRY0 = 0x132;
constexpr uint32_t CHAT_STRIDE     = 0xFC;
constexpr uint32_t OFF_ENTRY_TEXT  = 0x20;
constexpr uint32_t OFF_CHAT_DIRTY  = 0x63DA;
// Dialog: singleton ptr @ base+0x2AC9E0; iIsActive @ this+0x28, DialogID @ this+0x30.
//   Build/show routine (thiscall) @ base+0x6FA50 -- the same one SA:MP runs to
//   re-show a same-id dialog; updates caption/body/buttons/list + layout, no network.
constexpr uint32_t OFF_DLG_PTR     = 0x2AC9E0;
constexpr uint32_t OFF_DLG_ACTIVE  = 0x28;
constexpr uint32_t OFF_DLG_ID      = 0x30;
constexpr uint32_t FUNC_DLG_SHOW   = 0x6FA50;

// void CDialog::Show(u16 id, u8 style, char* caption, char* body, char* btn1,
//                    char* btn2, int flag)  -- thiscall (this in ECX), ret 0x1c.
using ShowDlgFn = void( __thiscall * )( void* self, int id, int style,
                                        const char* caption, const char* body,
                                        const char* btn1, const char* btn2, int flag );

// SEH-isolated call into SA:MP's dialog builder. No C++ objects live in this frame
// (only PODs + the call), so __except is legal here -- a bad game state during the
// experiment degrades to a logged failure instead of crashing the process.
bool safe_show( void* fn, void* self, int id, int style,
                const char* cap, const char* body, const char* b1, const char* b2 ) {
    __try {
        ( (ShowDlgFn) fn )( self, id, style, cap, body, b1, b2, 1 );
        return true;
    } __except ( EXCEPTION_EXECUTE_HANDLER ) {
        return false;
    }
}

std::mutex g_mtx;

struct ChatExp { std::string text; unsigned long fireTick; };
std::deque<ChatExp> g_chatQ;
std::string g_lastBottom;

struct DlgExp { int id; int style; std::string caption, body, btn1, btn2; unsigned long fireTick; };
std::deque<DlgExp> g_dlgQ;

bool is_lab( const char* s ) { return s && s[0] == '[' && s[1] == 'L' && s[2] == 'A' && s[3] == 'B'; }

// u8-length-prefixed string reader (RPC dialog fields).
bool read_u8str( const uint8_t* p, size_t n, size_t& off, std::string& out ) {
    if ( off >= n ) return false;
    uint8_t len = p[off++];
    if ( off + len > n ) return false;
    out.assign( (const char*)( p + off ), len );
    off += len;
    return true;
}

} // namespace

void on_incoming_rpc( unsigned char id, RPCParameters* rpc ) {
    auto& s = i18n::config::get();
    if ( !s.debug_lab && !s.debug_lab_replace ) return;
    if ( id != 61 || !rpc || !rpc->input || rpc->numberOfBitsOfData < 48 ) return;

    const uint8_t* p = rpc->input;
    size_t n = (size_t)( ( rpc->numberOfBitsOfData + 7 ) / 8 );

    // Wire layout (see surfaces_rebuild::rebuild_dialog):
    //   u16 dialogId, u8 style, u8+title, u8+btn1, u8+btn2, CSTRING(Huffman) body.
    size_t off = 2;
    int dlgId = p[0] | ( p[1] << 8 );
    if ( off >= n ) return;
    int style = p[off++];
    std::string title, b1, b2, body;
    if ( !read_u8str( p, n, off, title ) ) return;
    if ( !read_u8str( p, n, off, b1 ) ) return;
    if ( !read_u8str( p, n, off, b2 ) ) return;
    if ( n > off ) huffman::decode_string( p + off, n - off, body );

    if ( s.debug_lab )
        Log( "[lab] dialog RPC61: id=%d style=%d title='%s' btn1='%s' btn2='%s' body='%.80s'",
             dlgId, style, title.c_str(), b1.c_str(), b2.c_str(), body.c_str() );

    if ( s.debug_lab_replace ) {
        std::lock_guard<std::mutex> lk( g_mtx );
        g_dlgQ.push_back( { dlgId, style, title, body, b1, b2, GetTickCount() + 2000 } );
    }
}

void on_frame() {
    auto& s = i18n::config::get();
    if ( !s.debug_lab && !s.debug_lab_replace ) return;

    uint32_t base = samp::base();
    if ( !base ) return;
    unsigned long now = GetTickCount();

    void* chat = *(void**)( base + OFF_CHAT_PTR );

    // --- CHAT: watch the newest entry [99]; log new lines + queue a flip ---
    if ( chat ) {
        const char* bottom = (const char*)( (uint8_t*)chat + OFF_CHAT_ENTRY0 + 99 * CHAT_STRIDE + OFF_ENTRY_TEXT );
        if ( bottom[0] && !is_lab( bottom ) ) {
            std::string cur( bottom );
            if ( cur != g_lastBottom ) {
                g_lastBottom = cur;
                if ( s.debug_lab ) Log( "[lab] chat[99] new line: '%s'", cur.c_str() );
                if ( s.debug_lab_replace ) {
                    std::lock_guard<std::mutex> lk( g_mtx );
                    g_chatQ.push_back( { cur, now + 2000 } );
                }
            }
        }
    }

    // --- fire due CHAT flips (edit szText + set dirty flag) ---
    for ( ;; ) {
        ChatExp exp;
        {
            std::lock_guard<std::mutex> lk( g_mtx );
            if ( g_chatQ.empty() || (long)( now - g_chatQ.front().fireTick ) < 0 ) break;
            exp = g_chatQ.front();
            g_chatQ.pop_front();
        }
        if ( !chat ) continue;
        int found = -1;
        for ( int i = 99; i >= 0; --i ) {
            char* t = (char*)( (uint8_t*)chat + OFF_CHAT_ENTRY0 + i * CHAT_STRIDE + OFF_ENTRY_TEXT );
            if ( !is_lab( t ) && exp.text == t ) { found = i; break; }
        }
        if ( found >= 0 ) {
            char* t = (char*)( (uint8_t*)chat + OFF_CHAT_ENTRY0 + found * CHAT_STRIDE + OFF_ENTRY_TEXT );
            std::string rep = "[LAB] " + exp.text;
            if ( rep.size() > 143 ) rep.resize( 143 );
            memcpy( t, rep.c_str(), rep.size() + 1 );
            *(int*)( (uint8_t*)chat + OFF_CHAT_DIRTY ) = 1;
            Log( "[lab] chat FLIP slot=%d -> '%s' (dirty@+0x63DA=1). If this line VISIBLY changed, in-place chat WORKS.",
                 found, rep.c_str() );
        } else {
            Log( "[lab] chat flip: '%s' not found (scrolled off before fire).", exp.text.c_str() );
        }
    }

    // --- fire due DIALOG re-show experiments (call SA:MP's builder) ---
    for ( ;; ) {
        DlgExp exp;
        {
            std::lock_guard<std::mutex> lk( g_mtx );
            if ( g_dlgQ.empty() || (long)( now - g_dlgQ.front().fireTick ) < 0 ) break;
            exp = g_dlgQ.front();
            g_dlgQ.pop_front();
        }
        void* dlg = *(void**)( base + OFF_DLG_PTR );
        if ( !dlg ) { Log( "[lab] dialog re-show skip: singleton ptr null." ); continue; }
        int active = *(int*)( (uint8_t*)dlg + OFF_DLG_ACTIVE );
        int curId  = *(uint16_t*)( (uint8_t*)dlg + OFF_DLG_ID );
        Log( "[lab] dialog guard: active=%d curId=%d expId=%d", active, curId, exp.id );
        if ( active != 1 || curId != exp.id ) { Log( "[lab] dialog re-show skip (answered/closed/superseded)." ); continue; }

        std::string cap  = "[LAB] " + exp.caption;
        std::string body = "[LAB] " + exp.body;
        void* fn = (void*)( base + FUNC_DLG_SHOW );
        bool ok = safe_show( fn, dlg, exp.id, exp.style, cap.c_str(), body.c_str(), exp.btn1.c_str(), exp.btn2.c_str() );
        Log( "[lab] dialog RE-SHOW via 0x6FA50 %s. If the OPEN dialog now shows [LAB], dialog re-show WORKS.",
             ok ? "called ok" : "FAILED (crash-guarded)" );
    }
}

} // namespace i18n::lab
