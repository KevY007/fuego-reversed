#pragma once
/*
	recorder/timeline.h  --  the packet tracer + state recorder's timeline (pure, host-tested).

	Turns everything that crosses the wire -- and what we do and see around it --
	into readable, time-stamped lines, to debug, reverse-engineer or build things for
	any server later: what the server sends (game text, textdraws with the box sizes
	a progress bar changes, chat, dialogs, animations, objects, sounds, ...), what we
	send (every key press/release with how long it was held, commands, chat, dialog
	replies WITH what was typed or picked, ...), packets that aren't RPCs (connection
	events, the auth challenge, other players' syncs), every line shown in the chat
	box, our real keyboard/mouse input, and our state (position, health, weapons,
	vehicle, interior, menus) as snapshots + changes. recorder.cpp feeds it and writes
	the lines; nothing here touches the game, the network or a file.

	What crossed the wire is written exactly: outgoing traffic is tapped after every
	send handler (a cheat's rewrite shows as sent), and packets a cheat sent itself are
	tagged [fuego] (tracked apart, so they never break the record of our real keys).

	Detail (Options): normal = us + the server + everyone's chat, joins, quits and kills
	(other players' own traffic, streaming and far objects only counted, unchanged
	re-sends counted); others = + other players' RPCs and their syncs whenever they
	change; everything = every packet, unfiltered, each with its raw bytes. Nothing is
	hidden by default -- passwords typed into dialogs, login commands, RCON, the join
	packet and anti-cheat answers are all written -- unless hideSecrets is set (for a
	log to share), which brings back the redaction.

	A 10-second summary line lists the traffic rates, the busiest textdraws and what
	wasn't written.
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

const char *in_packet_name( int id );     // packet id name ("CONNECTION_LOST", "PLAYER_SYNC", ...)
const char *vehicle_model_name( int model ); // "Infernus" for 411; nullptr outside 400..611
std::string vk_name( unsigned vk );       // "W", "SPACE", "LMB", "F2", ... ("VK 0xNN" otherwise)

constexpr float    kNearMeters      = 80.0f;
constexpr uint32_t kSummaryEveryMs  = 10000;
constexpr uint32_t kStateEveryMs    = 30000; // a full STATE snapshot this often (changes at once)

struct Options {
	bool others      = false; // other players' RPCs + their syncs (on change)
	bool everything  = false; // every packet unfiltered (+ others), each with its raw bytes
	bool hideSecrets = false; // for sharing: no passwords / login args / RCON / join / anti-cheat
};

// Our own state, read from the game by recorder.cpp each frame.
struct LocalState {
	int   gameState = 0;          // CNetGame::iGameState (14 = connected)
	int   id = -1;                // our player id
	bool  spawned = false, dead = false; // (spectating / class selection: the server's RPCs show those)
	float pos[3] = { 0, 0, 0 };
	int   interior = 0;
	float health = 0, armor = 0;
	int   money = 0;
	int   weapon = 0;             // current weapon id
	uint8_t weapons[13] = {};     // per slot (the game ped's)
	int   ammo[13] = {};
	int   vehicle = -1, seat = -1, vehicleModel = 0; // -1: on foot
	float vehicleHealth = 0;
	int   skin = -1;
	bool  chatInput = false, dialogOpen = false, gtaMenu = false, fuegoMenu = false;
	int   ping = 0;
};

class Timeline {
public:
	// A new recording: forgets everything; `nowMs` is the time stamps' zero.
	void reset( uint32_t nowMs );
	void set_local_id( int id ) { localId_ = id; }
	void set_options( const Options &o ) { opt_ = o; }
	const Options &options() const { return opt_; }

	// Server-assigned ids get named in every line: a player id with the player's name
	// ("player 9 (Panzer_)"), a vehicle id with its model ("vehicle 412 (Voodoo)").
	// recorder.cpp hands in lookups into SA:MP's pools (players / vehicles already there
	// when the recording starts); joins, name changes and vehicles streaming in during
	// it are learned here. Either may be null.
	using PlayerNameFn   = std::string ( * )( int playerId ); // "" if unknown
	using VehicleModelFn = int ( * )( int vehicleId );        // GTA model id, -1 if unknown
	void set_name_lookups( PlayerNameFn player, VehicleModelFn vehicle ) { playerName_ = player; vehicleModel_ = vehicle; }

	// Who is online, at the start and every few minutes: "PLAYERS 37: 0 Foo, 3 Bar, ...".
	void roster( uint32_t nowMs, const std::vector<std::pair<int, std::string>> &players, std::vector<std::string> &out );

	// Each appends the lines to write to `out` (possibly none). `bits` is the
	// payload length in bits; a packet's payload starts with its id byte.
	void incoming_rpc( uint32_t nowMs, int id, const uint8_t *p, size_t bits, std::vector<std::string> &out );
	void outgoing_rpc( uint32_t nowMs, int id, const uint8_t *p, size_t bits, bool injected, std::vector<std::string> &out );
	void outgoing_packet( uint32_t nowMs, const uint8_t *p, size_t bits, bool injected, std::vector<std::string> &out );
	// A packet from the server that isn't an RPC (connection events, the auth challenge,
	// other players' syncs, ...); payload starts with its id byte.
	void incoming_packet( uint32_t nowMs, const uint8_t *p, size_t bits, std::vector<std::string> &out );
	// A line shown in the chat box that didn't come from a server chat RPC (those are
	// written as RPCs): SA:MP's own messages, a cheat's, local commands' replies.
	void chat_line( uint32_t nowMs, int type, const std::string &text, std::vector<std::string> &out );
	// Our real keyboard / mouse input (the game window's messages): KEY <name> down /
	// up with how long it was held. Repeats of a held key aren't written, nor the
	// release of a key whose press wasn't (held before the recording, or typed in chat).
	void input( uint32_t nowMs, unsigned vk, bool down, std::vector<std::string> &out );
	void wheel( uint32_t nowMs, int notches, std::vector<std::string> &out ); // + up, - down
	// The game window lost / got back the focus (alt-tab): written on a change only;
	// losing it releases every key still held (Windows sends no key-up then).
	void focus( uint32_t nowMs, bool active, std::vector<std::string> &out );
	// Our state, every frame: a full STATE line at the start and every kStateEveryMs,
	// and a STATE line naming what changed whenever something does (position only when
	// it jumps -- a teleport -- or in a full snapshot).
	void state( uint32_t nowMs, const LocalState &s, std::vector<std::string> &out );
	// A numbered note at this moment (/.mark): "MARK 3 "text"".
	void mark( uint32_t nowMs, const std::string &text, std::vector<std::string> &out );
	// A line from the recorder itself ("--- detail: everything ---").
	void note( uint32_t nowMs, const std::string &text, std::vector<std::string> &out ) { emit( nowMs, "", "--- " + text + " ---", out ); }
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
	// "+KEY, -KEY held Nms" between two key sets; pressedAt[19] = when each bit went down.
	std::string key_changes( uint32_t *pressedAt, uint32_t before, uint32_t after, uint32_t nowMs, bool onFoot );
	void forget_session(); // InitGame / GameModeRestart: textdraws, objects, labels are gone
	// A sync of another kind (on foot <-> driving <-> passenger) than the last one:
	// the new kind starts fresh, so no press/hold carries over from the other.
	bool switch_kind( Syncs &st, int kind, uint32_t nowMs );

	bool in_rpc( uint32_t nowMs, int id, const uint8_t *p, size_t bits, std::string &line );
	bool out_rpc( int id, const uint8_t *p, size_t bits, std::string &line );
	bool other_sync( uint32_t nowMs, const uint8_t *p, size_t bits, std::string &line ); // a player's sync from the server
	std::string raw( const uint8_t *p, size_t bits ) const; // " | raw ..." in "everything" mode, else ""
	std::string player_name( uint16_t pid ) const;          // "" if unknown (one line, printable)
	int vehicle_model( uint16_t vid ) const;                // GTA model id, -1 if unknown
	std::string who( uint16_t pid ) const;                  // "me" / "player 12 (Name)"
	std::string vehicle_name( uint16_t vid ) const;         // "vehicle 412 (Voodoo)"
	std::string target_suffix( uint8_t type, uint16_t id ) const; // a bullet's target: " (Name)" / " (Voodoo)"
	bool dialog_row( int16_t dlg, int16_t item, std::string &row ) const; // a list dialog's picked row
	bool mine( uint16_t pid ) const { return localId_ >= 0 && pid == (uint16_t) localId_; }
	int  my_vehicle() const; // the vehicle our last sync was in, -1 on foot
	// A streaming/world RPC that is about us (our player id, or the vehicle we're in):
	// written even when that kind of RPC is otherwise only counted.
	bool about_me( int id, const uint8_t *p, size_t bits ) const;
	std::string full_state( const LocalState &s ) const;
	int  show_textdraw( const uint8_t *p, size_t bits, std::string &line ); // 1 write, 0 unchanged, -1 cut off
	bool show_dialog( const uint8_t *p, size_t bits, std::string &line );

	uint32_t t0_ = 0;
	int      localId_ = -1;
	int      marks_ = 0;
	Options  opt_;
	PlayerNameFn   playerName_ = nullptr;
	VehicleModelFn vehicleModel_ = nullptr;
	std::map<uint16_t, std::string> names_;      // from ServerJoin / SetPlayerName
	std::map<uint16_t, int32_t>     vehicleModels_; // from WorldVehicleAdd

	Syncs game_, injected_;
	bool havePos_ = false;        // our position, from the game's own syncs
	float myPos_[3] = { 0, 0, 0 };
	uint32_t lastKeys_ = 0;       // the game's last sync's keys (overlay)
	bool lastKeysOnFoot_ = true;
	std::vector<uint8_t> lastWeapons_;
	bool    haveStats_ = false;
	int32_t lastMoney_ = 0, lastDrunk_ = 0;

	std::map<uint16_t, TextDraw> textdraws_;
	std::set<uint16_t> objects_, labels_;
	std::set<int32_t>  pickups_;
	std::set<uint16_t> actors_;
	std::map<int16_t, uint8_t> dialogStyle_; // dialog id -> its style, as last shown
	std::map<int16_t, std::string> dialogBody_; // dialog id -> its body (to name the row picked)

	// Other players' syncs (others / everything): what each last sent.
	struct Other {
		int      kind = 0; // 1 on foot, 2 driving, 3 passenger
		uint32_t keys = 0;
		int16_t  ud = 0, lr = 0;
		uint16_t vehicle = 0, unoccupied = 0xFFFF, trailer = 0xFFFF;
		uint8_t  seat = 0, weapon = 0, action = 0, hpar = 0;
		bool     haveAim = false;
		uint8_t  cam = 0, wstate = 0;
		uint32_t pressedAt[19] = {};
	};
	std::map<uint16_t, Other> others_;

	// Real input: when each key / button went down (ms since t0); absent = up.
	std::map<unsigned, uint32_t> keyDownAt_;
	bool focused_ = true;

	// State.
	bool       haveState_ = false;
	LocalState lastState_;
	uint32_t   lastFullStateAt_ = 0;
	uint32_t   lastStateAt_ = 0; // the previous state() call (a teleport is a jump faster than any vehicle)

	// The current summary window.
	uint32_t windowStart_ = 0;
	std::map<std::string, unsigned> sent_;    // OUT packets by kind
	unsigned rpcsIn_ = 0, rpcsOut_ = 0;
	std::map<std::string, unsigned> skipped_; // what wasn't written, by kind
	std::map<uint16_t, unsigned> tdUpdates_;  // shows + text sets per textdraw (written or not)
};

} // namespace rec
