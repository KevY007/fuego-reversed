#pragma once
/*
	recorder/timeline.h  --  the packet recorder's timeline (pure, host-tested).

	Turns raw network traffic into readable, timestamped lines, to work out how a
	server's minigame (e.g. a fishing script) runs: what the server shows -- game
	text, textdraws (with the box/sprite sizes a progress bar changes), chat,
	dialogs, animations, attached objects, weapons, sounds -- next to what we send
	-- every key press/release with how long it was held, weapon, animation, aim
	camera mode, commands. recorder.cpp feeds it and writes the lines; nothing here
	touches the game, the network or a file.

	What is written for us and for the server is exactly what crossed the wire:
	outgoing traffic is tapped after every send handler (so a cheat's rewrite shows
	as sent), and packets a cheat sent itself are tagged [fuego].

	Never written: dialog input text (nor its length for an input/password dialog),
	the arguments of any login/password-like command (/login, /alogin, /l, /pass...),
	RCON packets, client-check answers and the join packet -- and none of those is
	ever hex-dumped, not even when cut off.

	Noise control: server RPCs about OTHER players (their chat, animations, objects,
	bubbles, cars, streaming in/out, colours, ...) are only counted; objects, 3D
	labels, pickups and actors are written when they're within kNearMeters of us (or
	attached to us / our vehicle), and their later changes only for the ones
	written; a textdraw re-shown unchanged is only counted. A 10-second summary
	line lists the traffic rates, the busiest textdraws and what was skipped.
*/

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace rec {

// Readers for RakNet bit streams: bits are MSB-first in each byte, multi-byte
// values little-endian -- also when not byte-aligned (after a 1-bit bool).
class BitReader {
public:
	BitReader( const uint8_t *p, size_t bits ) : p_( p ), bits_( p ? bits : 0 ) {}
	bool bit( bool &v );
	bool bytes( void *dst, size_t n );
	template <class T> bool get( T &v ) { return bytes( &v, sizeof v ); }
	bool str8( std::string &s, size_t cap = 4096 );  // u8 length + bytes
	bool str16( std::string &s, size_t cap = 4096 ); // u16 length + bytes
	bool str32( std::string &s, size_t cap = 4096 ); // u32 length + bytes
	size_t pos() const { return pos_; }
	size_t left() const { return bits_ - pos_; }
	bool aligned() const { return ( pos_ & 7 ) == 0; }

private:
	const uint8_t *p_;
	size_t bits_;
	size_t pos_ = 0;
};

// "FIRE(attack)+HANDBRAKE(aim)" -- SA:MP's KEY_ names (what a server script tests
// for), with the on-foot key in brackets. Bits 16..18 = KEY_YES / KEY_NO /
// KEY_CTRL_BACK. "none" for 0.
std::string key_names( uint32_t keys, bool onFoot = true );

// Server/user text for a log line: in quotes, printable ASCII as is, \" \\ \n, any
// other byte as \xNN; longer than `cap` bytes -> cut with "...(+N)".
std::string quote( const uint8_t *p, size_t n, size_t cap = 600 );
inline std::string quote( const std::string &s, size_t cap = 600 ) {
	return quote( reinterpret_cast<const uint8_t *>( s.data() ), s.size(), cap );
}

const char *weapon_name( unsigned id );   // "cane", "fists", ... "?" if unknown
const char *in_rpc_name( int id );        // server -> client RPC name, or "rpc"
const char *out_rpc_name( int id );       // client -> server RPC name, or "rpc"

constexpr float    kNearMeters      = 80.0f;
constexpr uint32_t kSummaryEveryMs  = 10000;

class Timeline {
public:
	// A new recording: forgets everything; `nowMs` is the time stamps' zero.
	void reset( uint32_t nowMs );
	void set_local_id( int id ) { localId_ = id; }

	// Each appends the lines to write to `out` (possibly none). `bits` is the
	// payload length in bits; a packet's payload starts with its id byte.
	void incoming_rpc( uint32_t nowMs, int id, const uint8_t *p, size_t bits, std::vector<std::string> &out );
	void outgoing_rpc( uint32_t nowMs, int id, const uint8_t *p, size_t bits, bool injected, std::vector<std::string> &out );
	void outgoing_packet( uint32_t nowMs, const uint8_t *p, size_t bits, bool injected, std::vector<std::string> &out );
	// A numbered note at this moment (/.mark): "MARK 3 "text"".
	void mark( uint32_t nowMs, const std::string &text, std::vector<std::string> &out );
	// The traffic summary, once every kSummaryEveryMs (only if anything moved);
	// force: now, for the part of a window left when the recording stops.
	void tick( uint32_t nowMs, std::vector<std::string> &out, bool force = false );

	// The keys our last sync carried ("none" before any), for the on-screen REC pill.
	std::string keys_now() const;
	int marks() const { return marks_; }

private:
	struct Foot {
		uint32_t keys = 0; // incl. bits 16..18 (KEY_YES/NO/CTRL_BACK)
		int16_t  ud = 0, lr = 0;
		uint8_t  weapon = 0, action = 0, health = 0, armor = 0;
		int16_t  anim = 0, animFlags = 0;
		float    pos[3] = { 0, 0, 0 };
		float    yaw = 0;
	};
	struct Aim { uint8_t cam = 0, state = 0, zoom = 0; };
	struct Car { uint16_t vehicle = 0; uint32_t keys = 0; int16_t ud = 0, lr = 0; uint8_t weapon = 0, health = 0, armor = 0; float pos[3] = { 0, 0, 0 }; float carHp = 0; };
	struct Passenger { uint16_t vehicle = 0; uint8_t seat = 0, driveBy = 0, weapon = 0; uint32_t keys = 0; int16_t ud = 0, lr = 0; float pos[3] = { 0, 0, 0 }; };
	struct TextDraw {
		uint8_t  flags = 0, font = 0, selectable = 0, shadow = 0, outline = 0;
		float    letter[2] = { 0, 0 }, size[2] = { 0, 0 }, pos[2] = { 0, 0 };
		uint32_t color = 0, box = 0, bg = 0;
		uint16_t model = 0;
		std::string text;
		bool shown = false;   // false: only its text is known (TD set before any show)
		bool visible = false; // shown and not hidden since: an unchanged re-show is HUD churn
	};

	// What our syncs carried. Two sets: the game's own syncs, and the ones a cheat
	// sends itself ([fuego]) -- so a cheat's syncs, even of another kind and
	// interleaved, never break the record of the keys we really press.
	struct Syncs {
		bool haveFoot = false, haveAim = false, haveCar = false, havePassenger = false;
		int  lastKind = 0; // 1 on foot, 2 driving, 3 passenger
		Foot foot; Aim aim; Car car; Passenger passenger;
		uint32_t pressedAt[19] = {}; // when each key bit went down (ms since t0)
	};

	std::string stamp( uint32_t nowMs ) const;
	void emit( uint32_t nowMs, const char *tag, const std::string &body, std::vector<std::string> &out );
	void skip( const std::string &what ) { ++skipped_[what]; }
	bool near_me( const float p[3] ) const;
	std::string key_changes( Syncs &st, uint32_t before, uint32_t after, uint32_t nowMs, bool onFoot );
	void forget_session(); // InitGame / GameModeRestart: textdraws, objects, labels are gone
	// A sync of another kind (on foot <-> driving <-> passenger) than the last one:
	// the new kind starts fresh, so no press/hold carries over from the other.
	bool switch_kind( Syncs &st, int kind, uint32_t nowMs );

	bool in_rpc( uint32_t nowMs, int id, const uint8_t *p, size_t bits, std::string &line );
	bool out_rpc( int id, const uint8_t *p, size_t bits, std::string &line );
	int  show_textdraw( const uint8_t *p, size_t bits, std::string &line ); // 1 write, 0 unchanged, -1 cut off
	bool show_dialog( const uint8_t *p, size_t bits, std::string &line );

	uint32_t t0_ = 0;
	int      localId_ = -1;
	int      marks_ = 0;

	Syncs game_, injected_;
	bool havePos_ = false;        // our position, from the game's own syncs
	float myPos_[3] = { 0, 0, 0 };
	uint32_t lastKeys_ = 0;       // the game's last sync's keys (overlay)
	bool lastKeysOnFoot_ = true;
	std::vector<uint8_t> lastStats_, lastWeapons_;

	std::map<uint16_t, TextDraw> textdraws_;
	std::set<uint16_t> objects_, labels_;
	std::set<int32_t>  pickups_;
	std::set<uint16_t> actors_;
	std::map<int16_t, uint8_t> dialogStyle_; // dialog id -> its style, as last shown

	// The current summary window.
	uint32_t windowStart_ = 0;
	std::map<std::string, unsigned> sent_;    // OUT packets by kind
	unsigned rpcsIn_ = 0, rpcsOut_ = 0;
	std::map<std::string, unsigned> skipped_; // what wasn't written, by kind
	std::map<uint16_t, unsigned> tdUpdates_;  // shows + text sets per textdraw (written or not)
};

} // namespace rec
