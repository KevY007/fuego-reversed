#include "timeline.h"

#include "../i18n/huffman.h" // ShowDialog / 3D label text is a compressed CSTRING
#include "../samp/structs.h" // the sync packet layouts (byte-exact, shared with the cheats)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <utility>

namespace rec {

// ---------------------------------------------------------------------------
// BitReader
// ---------------------------------------------------------------------------

bool BitReader::bit( bool &v ) {
	if ( pos_ >= bits_ ) return false;
	v = ( ( p_[pos_ >> 3] >> ( 7 - ( pos_ & 7 ) ) ) & 1 ) != 0;
	++pos_;
	return true;
}

bool BitReader::bytes( void *dst, size_t n ) {
	if ( n == 0 ) return true;
	if ( n > ( bits_ - pos_ ) / 8 ) return false;
	uint8_t *d = static_cast<uint8_t *>( dst );
	if ( ( pos_ & 7 ) == 0 ) {
		std::memcpy( d, p_ + ( pos_ >> 3 ), n );
		pos_ += n * 8;
		return true;
	}
	for ( size_t i = 0; i < n; ++i ) { // not byte-aligned (after a 1-bit bool)
		uint8_t b = 0;
		for ( int k = 0; k < 8; ++k, ++pos_ )
			b = (uint8_t)( ( b << 1 ) | ( ( p_[pos_ >> 3] >> ( 7 - ( pos_ & 7 ) ) ) & 1 ) );
		d[i] = b;
	}
	return true;
}

namespace {
template <class Len>
bool read_str( BitReader &r, std::string &s, size_t cap ) {
	Len len = 0;
	if ( !r.get( len ) || (size_t) len > cap || (size_t) len > r.left() / 8 ) return false;
	s.resize( (size_t) len );
	return r.bytes( s.empty() ? nullptr : &s[0], s.size() );
}
} // namespace

bool BitReader::str8( std::string &s, size_t cap )  { return read_str<uint8_t>( *this, s, cap ); }
bool BitReader::str16( std::string &s, size_t cap ) { return read_str<uint16_t>( *this, s, cap ); }
bool BitReader::str32( std::string &s, size_t cap ) { return read_str<uint32_t>( *this, s, cap ); }

// ---------------------------------------------------------------------------
// Names + formatting
// ---------------------------------------------------------------------------

namespace {

struct KeyName { uint32_t bit; const char *name; const char *onFoot; };
// SA:MP's KEY_ constants in bit order; [16..18] are KEY_YES / KEY_NO / KEY_CTRL_BACK,
// which travel in the top 2 bits of the sync's weapon byte (1, 2, 3).
const KeyName kKeys[19] = {
	{ 1, "ACTION", "tab" },          { 2, "CROUCH", "C" },          { 4, "FIRE", "attack" },
	{ 8, "SPRINT", "space" },        { 16, "SECONDARY_ATTACK", "enter" }, { 32, "JUMP", "shift" },
	{ 64, "LOOK_RIGHT", nullptr },   { 128, "HANDBRAKE", "aim" },   { 256, "LOOK_LEFT", nullptr },
	{ 512, "LOOK_BEHIND", "middle mouse" }, { 1024, "WALK", "alt" }, { 2048, "ANALOG_UP", "num8" },
	{ 4096, "ANALOG_DOWN", "num2" }, { 8192, "ANALOG_LEFT", "num4" }, { 16384, "ANALOG_RIGHT", "num6" },
	{ 32768, "KEY_32768", nullptr }, { 65536, "YES", "Y" },         { 131072, "NO", "N" },
	{ 262144, "CTRL_BACK", "H" },
};

std::string key_name( int i, bool onFoot ) {
	std::string s = kKeys[i].name;
	if ( kKeys[i].onFoot && ( onFoot || i >= 16 ) ) { s += '('; s += kKeys[i].onFoot; s += ')'; }
	return s;
}

// The sync's weapon byte: weapon in the low 6 bits, KEY_YES/NO/CTRL_BACK (1/2/3)
// in the top 2.
uint32_t extra_keys( uint8_t weaponByte ) {
	switch ( weaponByte >> 6 ) {
	case 1: return 65536;
	case 2: return 131072;
	case 3: return 262144;
	default: return 0;
	}
}

std::string fmt( const char *f, ... ) {
	char buf[1024];
	va_list ap;
	va_start( ap, f );
	std::vsnprintf( buf, sizeof buf, f, ap );
	va_end( ap );
	return buf;
}

std::string hex( const uint8_t *p, size_t n, size_t cap ) {
	static const char *H = "0123456789ABCDEF";
	std::string s;
	const size_t m = n < cap ? n : cap;
	for ( size_t i = 0; i < m && p; ++i ) {
		if ( i ) s += ' ';
		s += H[p[i] >> 4];
		s += H[p[i] & 15];
	}
	if ( n > m ) s += " ...";
	return s;
}

std::string generic( const char *name, int id, const uint8_t *p, size_t bits ) {
	const size_t n = p ? ( bits + 7 ) / 8 : 0;
	std::string s = fmt( "%s(%d) %uB", name, id, (unsigned) n );
	if ( n ) { s += ' '; s += hex( p, n, 24 ); }
	return s;
}

std::string td_id( uint16_t id ) { // player textdraws travel as 2048 + their id
	return id >= 2048 ? fmt( "#%u(player %u)", id, id - 2048u ) : fmt( "#%u", id );
}

std::string xyz( const float v[3], int decimals = 1 ) {
	return decimals == 2 ? fmt( "(%.2f,%.2f,%.2f)", v[0], v[1], v[2] ) : fmt( "(%.1f,%.1f,%.1f)", v[0], v[1], v[2] );
}

const char *special_action( unsigned a ) {
	switch ( a ) {
	case 0: return "none";        case 1: return "duck";       case 2: return "jetpack";
	case 3: return "enter car";   case 4: return "exit car";   case 5: return "dance 1";
	case 6: return "dance 2";     case 7: return "dance 3";    case 8: return "dance 4";
	case 10: return "hands up";   case 11: return "cellphone"; case 12: return "sitting";
	case 13: return "stop cellphone"; case 20: return "beer"; case 21: return "smoke";
	case 22: return "wine";       case 23: return "sprunk";    case 24: return "cuffed";
	case 25: return "carry";      case 68: return "piss";
	default: return "?";
	}
}

const char *cam_mode( unsigned m ) {
	switch ( m ) {
	case 4: return "behind";   case 7: return "sniper"; case 8: return "rocket";
	case 15: return "fixed";   case 18: return "in car"; case 53: return "aiming";
	default: return "?";
	}
}

const char *weapon_state( unsigned s ) {
	switch ( s ) {
	case 0: return "no bullets"; case 1: return "last bullet"; case 2: return "more bullets";
	default: return "reloading";
	}
}

const char *dialog_style( unsigned s ) {
	switch ( s ) {
	case 0: return "msgbox"; case 1: return "input"; case 2: return "list";
	case 3: return "password"; case 4: return "tablist"; case 5: return "tablist+headers";
	default: return "?";
	}
}

const char *bone_name( unsigned b ) {
	static const char *k[] = { "?", "spine", "head", "left upper arm", "right upper arm", "left hand",
	                           "right hand", "left thigh", "right thigh", "left foot", "right foot",
	                           "right calf", "left calf", "left forearm", "right forearm",
	                           "left clavicle", "right clavicle", "neck", "jaw" };
	return b < sizeof k / sizeof *k ? k[b] : "?";
}

std::string axis( int16_t v, char neg, char pos ) {
	if ( v == 0 ) return "0";
	if ( v == -128 ) return std::string( 1, neg );
	if ( v == 128 ) return std::string( 1, pos );
	return fmt( "%d", v );
}

float yaw_deg( const float q[4] ) { // q = w, x, y, z
	float a = std::atan2( 2.0f * ( q[0] * q[3] + q[1] * q[2] ), 1.0f - 2.0f * ( q[2] * q[2] + q[3] * q[3] ) ) * 57.2957795f;
	if ( a < 0 ) a += 360.0f;
	return a;
}

// Login/password-like commands: only the command itself is written, never its
// arguments. Matched loosely on purpose (/alogin, /AdminLogin, /setpin, ...): hiding
// a harmless command's arguments costs nothing, writing a password would.
// Where the command word ends: the slashes, then letters/digits/_ . - ("/pw:x" -> "/pw").
size_t command_word_end( const std::string &text ) {
	size_t i = 0;
	while ( i < text.size() && text[i] == '/' ) ++i;
	while ( i < text.size() && ( std::isalnum( (unsigned char) text[i] ) || text[i] == '_' || text[i] == '.' || text[i] == '-' ) ) ++i;
	return i;
}

bool sensitive_command( const std::string &text ) {
	size_t i = 0;
	while ( i < text.size() && text[i] == '/' ) ++i;
	const size_t j = command_word_end( text );
	std::string w = text.substr( i, j - i );
	for ( char &c : w ) c = (char) std::tolower( (unsigned char) c );
	static const char *kWhole[] = { "l", "log", "rcon" };
	for ( const char *h : kWhole )
		if ( w == h ) return true;
	static const char *kPart[] = { "login", "pass", "pw", "pin", "auth", "reg", "code", "otp", "2fa", "secret" };
	for ( const char *h : kPart )
		if ( w.find( h ) != std::string::npos ) return true;
	return false;
}

// Outgoing RPCs whose payload may hold something typed: never hex-dumped, not even
// when cut off (command, dialog response, chat, join, client check).
bool private_out( int id ) { return id == 50 || id == 62 || id == 101 || id == 25 || id == 103; }

bool is_noisy_in( int id ) { // streaming / other players / clocks: only counted (unless about us)
	switch ( id ) {
	case 24: case 29: case 30: case 32: case 34: case 43: case 57: case 60: case 65: case 69:
	case 72: case 80: case 85: case 89: case 94: case 98: case 108: case 112: case 120: case 121:
	case 123: case 147: case 148: case 149: case 152: case 155: case 159: case 160: case 161:
	case 163: case 164: case 165: case 166: case 179: case 183: case 185:
		return true;
	default:
		return false;
	}
}

// Server RPCs whose first field is a player id / a vehicle id (named in the line).
bool player_first( int id ) {
	switch ( id ) {
	case 32: case 34: case 69: case 72: case 80: case 89: case 126: case 163: case 166: return true;
	default: return false;
	}
}
bool vehicle_first( int id ) {
	switch ( id ) {
	case 24: case 57: case 65: case 98: case 123: case 127: case 147: case 148: case 149: case 159:
	case 160: case 161: case 165: return true;
	default: return false;
	}
}

// "| 6B 09 00 FF ..." -- the rest of a payload whose first field was named.
std::string bytes_tail( const uint8_t *p, size_t bits ) {
	const size_t n = p ? ( bits + 7 ) / 8 : 0;
	return fmt( "| %uB ", (unsigned) n ) + hex( p, n, 24 );
}

// A name from the server, for a log line: one line, printable (no quotes added).
std::string clean( const std::string &s, size_t cap = 64 ) {
	std::string r;
	for ( size_t i = 0; i < s.size() && i < cap; ++i ) {
		const unsigned char c = (unsigned char) s[i];
		r += ( c >= 0x20 && c < 0x7F ) ? (char) c : '?';
	}
	return r;
}

// A relayed sync's health/armour byte: a nibble each, in steps of 7 (0xF = 100).
unsigned hp_nibble( unsigned n ) { return n >= 0xF ? 100u : n * 7u; }

const char *game_state_name( int s ) {
	switch ( s ) {
	case 0: return "none";        case 9: return "waiting to connect"; case 13: return "connecting";
	case 14: return "connected";  case 15: return "joining";            case 18: return "restarting";
	default: return "?";
	}
}

std::string life( const LocalState &s ) {
	if ( s.dead ) return "dead";
	return s.spawned ? "spawned" : "not spawned";
}

// "[24(desert eagle) x70, 31(m4) x200]" -- with ammo, or the weapon ids alone.
std::string weapons_list( const LocalState &s, bool ammo ) {
	std::string w;
	for ( int i = 0; i < 13; ++i ) {
		if ( !s.weapons[i] ) continue;
		if ( !w.empty() ) w += ", ";
		w += ammo ? fmt( "%u(%s) x%d", s.weapons[i], weapon_name( s.weapons[i] ), s.ammo[i] )
		          : fmt( "%u(%s)", s.weapons[i], weapon_name( s.weapons[i] ) );
	}
	return '[' + w + ']';
}

std::string vehicle_of( const LocalState &s ) {
	if ( s.vehicle < 0 ) return "on foot";
	const char *m = vehicle_model_name( s.vehicleModel );
	return fmt( "in vehicle %d", s.vehicle ) + ( m ? fmt( " (%s)", m ) : std::string() ) + fmt( " seat %d", s.seat );
}

// A jump farther than this between two frames is a teleport, not a move: 40 m, plus
// what the fastest vehicle could cover in the time since the last frame (a hitch).
constexpr float kTeleportMeters = 40.0f;
constexpr float kFastestMps     = 120.0f;

} // namespace

std::string key_names( uint32_t keys, bool onFoot ) {
	std::string s;
	for ( int i = 0; i < 19; ++i )
		if ( keys & kKeys[i].bit ) {
			if ( !s.empty() ) s += '+';
			s += key_name( i, onFoot );
		}
	return s.empty() ? "none" : s;
}

std::string quote( const uint8_t *p, size_t n, size_t cap ) {
	std::string s = "\"";
	const size_t m = n < cap ? n : cap;
	for ( size_t i = 0; i < m && p; ++i ) {
		const uint8_t c = p[i];
		if ( c == '"' )       s += "\\\"";
		else if ( c == '\\' ) s += "\\\\";
		else if ( c == '\n' ) s += "\\n";
		else if ( c == '\t' ) s += "\\t";
		else if ( c >= 0x20 && c < 0x7F ) s += (char) c;
		else                  s += fmt( "\\x%02X", c );
	}
	if ( n > m ) s += fmt( "...(+%u)", (unsigned) ( n - m ) );
	s += '"';
	return s;
}

const char *weapon_name( unsigned id ) {
	static const char *k[] = {
		"fists", "brass knuckles", "golf club", "nightstick", "knife", "baseball bat", "shovel",
		"pool cue", "katana", "chainsaw", "purple dildo", "dildo", "vibrator", "silver vibrator",
		"flowers", "cane", "grenade", "tear gas", "molotov", "?", "?", "?", "9mm", "silenced 9mm",
		"desert eagle", "shotgun", "sawn-off", "combat shotgun", "uzi", "mp5", "ak-47", "m4",
		"tec-9", "country rifle", "sniper", "rpg", "heat-seeker", "flamethrower", "minigun",
		"satchel", "detonator", "spraycan", "extinguisher", "camera", "night vision", "thermal",
		"parachute" };
	return id < sizeof k / sizeof *k ? k[id] : "?";
}

const char *in_rpc_name( int id ) {
	switch ( id ) {
	case 11: return "SetPlayerName";        case 12: return "SetPos";             case 13: return "SetPosFindZ";
	case 14: return "Health";               case 15: return "Controllable";       case 16: return "Sound";
	case 17: return "SetWorldBounds";       case 18: return "Money";              case 19: return "SetFacing";
	case 20: return "ResetMoney";           case 21: return "ResetWeapons";       case 22: return "GiveWeapon";
	case 24: return "SetVehicleParamsEx";   case 28: return "CancelEdit";         case 29: return "SetPlayerTime";
	case 30: return "ToggleClock";          case 32: return "WorldPlayerAdd";     case 33: return "SetShopName";
	case 34: return "SetSkillLevel";        case 35: return "DrunkLevel";         case 36: return "Label";
	case 37: return "DisableCheckpoint";    case 38: return "RaceCheckpoint";     case 39: return "DisableRaceCheckpoint";
	case 40: return "GameModeRestart";      case 41: return "AudioStream";        case 42: return "StopAudioStream";
	case 43: return "RemoveBuilding";       case 44: return "Object";             case 45: return "SetObjectPos";
	case 46: return "SetObjectRot";         case 47: return "DestroyObject";      case 48: return "SetVirtualWorld";
	case 55: return "DeathMessage";         case 56: return "MapIcon";            case 57: return "RemoveVehicleComponent";
	case 58: return "DeleteLabel";          case 59: return "Bubble";             case 60: return "UpdateSystemTime";
	case 61: return "Dialog";               case 63: return "DestroyPickup";      case 64: return "WeaponPickupDestroy";
	case 65: return "LinkVehicleToInterior"; case 66: return "Armour";            case 67: return "ArmedWeapon";
	case 68: return "SetSpawnInfo";         case 69: return "SetPlayerTeam";      case 70: return "PutInVehicle";
	case 71: return "RemoveFromVehicle";    case 72: return "SetPlayerColor";     case 73: return "GameText";
	case 74: return "ForceClassSelection";  case 75: return "AttachObjectToPlayer"; case 76: return "InitMenu";
	case 77: return "ShowMenu";             case 78: return "HideMenu";           case 79: return "CreateExplosion";
	case 80: return "ShowNameTag";          case 81: return "AttachCameraToObject"; case 82: return "InterpolateCamera";
	case 83: return "SelectTextDraw";       case 84: return "ObjectMaterial";     case 85: return "GangZoneStopFlash";
	case 86: return "Anim";                 case 87: return "ClearAnim";          case 88: return "SpecialAction";
	case 89: return "FightingStyle";        case 90: return "SetVelocity";        case 91: return "SetVehicleVelocity";
	case 93: return "Chat";                 case 94: return "SetWorldTime";       case 95: return "Pickup";
	case 96: return "ScmEvent";             case 98: return "SetVehicleTireStatus"; case 99: return "MoveObject";
	case 26: return "PlayerEnterVehicle";   case 101: return "PlayerChat";        case 106: return "VehicleDamage";
	case 154: return "PlayerExitVehicle";
	case 104: return "StuntBonus";          case 105: return "TD set";            case 107: return "Checkpoint";
	case 108: return "GangZoneCreate";      case 112: return "PlayCrimeReport";   case 113: return "AttachedObject";
	case 120: return "GangZoneDestroy";     case 121: return "GangZoneFlash";     case 122: return "StopObject";
	case 123: return "SetNumberPlate";      case 124: return "Spectating";        case 126: return "SpectatePlayer";
	case 127: return "SpectateVehicle";     case 128: return "RequestClass";      case 129: return "RequestSpawn";
	case 133: return "WantedLevel";         case 134: return "TD show";           case 135: return "TD hide";
	case 137: return "ServerJoin";          case 138: return "ServerQuit";        case 139: return "InitGame";
	case 144: return "RemoveMapIcon";       case 145: return "SetAmmo";           case 146: return "SetGravity";
	case 147: return "SetVehicleHealth";    case 148: return "AttachTrailer";     case 149: return "DetachTrailer";
	case 152: return "SetWeather";          case 153: return "SetSkin";           case 155: return "ScoresAndPings";
	case 156: return "SetInterior";         case 157: return "CameraPos";         case 158: return "CameraLookAt";
	case 159: return "SetVehiclePos";       case 160: return "SetVehicleZAngle";  case 161: return "SetVehicleParamsForPlayer";
	case 162: return "CameraBehind";        case 163: return "WorldPlayerRemove"; case 164: return "WorldVehicleAdd";
	case 165: return "WorldVehicleRemove";  case 166: return "WorldPlayerDeath";  case 171: return "ShowActor";
	case 172: return "HideActor";           case 173: return "ActorAnim";         case 174: return "ClearActorAnim";
	case 175: return "ActorFacing";         case 176: return "ActorPos";          case 178: return "ActorHealth";
	case 179: return "ModelInfo";           case 183: return "ModelFile";         case 185: return "DownloadDone";
	default: return "rpc";
	}
}

const char *vehicle_model_name( int model ) {
	static const char *k[] = {
		"Landstalker", "Bravura", "Buffalo", "Linerunner", "Perennial", "Sentinel", "Dumper", "Firetruck",
		"Trashmaster", "Stretch", "Manana", "Infernus", "Voodoo", "Pony", "Mule", "Cheetah", "Ambulance",
		"Leviathan", "Moonbeam", "Esperanto", "Taxi", "Washington", "Bobcat", "Mr Whoopee", "BF Injection",
		"Hunter", "Premier", "Enforcer", "Securicar", "Banshee", "Predator", "Bus", "Rhino", "Barracks",
		"Hotknife", "Trailer 1", "Previon", "Coach", "Cabbie", "Stallion", "Rumpo", "RC Bandit", "Romero",
		"Packer", "Monster", "Admiral", "Squalo", "Seasparrow", "Pizzaboy", "Tram", "Trailer 2", "Turismo",
		"Speeder", "Reefer", "Tropic", "Flatbed", "Yankee", "Caddy", "Solair", "Berkley's RC Van", "Skimmer",
		"PCJ-600", "Faggio", "Freeway", "RC Baron", "RC Raider", "Glendale", "Oceanic", "Sanchez", "Sparrow",
		"Patriot", "Quad", "Coastguard", "Dinghy", "Hermes", "Sabre", "Rustler", "ZR-350", "Walton", "Regina",
		"Comet", "BMX", "Burrito", "Camper", "Marquis", "Baggage", "Dozer", "Maverick", "News Chopper",
		"Rancher", "FBI Rancher", "Virgo", "Greenwood", "Jetmax", "Hotring", "Sandking", "Blista Compact",
		"Police Maverick", "Boxville", "Benson", "Mesa", "RC Goblin", "Hotring Racer A", "Hotring Racer B",
		"Bloodring Banger", "Rancher", "Super GT", "Elegant", "Journey", "Bike", "Mountain Bike", "Beagle",
		"Cropdust", "Stunt", "Tanker", "Roadtrain", "Nebula", "Majestic", "Buccaneer", "Shamal", "Hydra",
		"FCR-900", "NRG-500", "HPV1000", "Cement Truck", "Tow Truck", "Fortune", "Cadrona", "FBI Truck",
		"Willard", "Forklift", "Tractor", "Combine", "Feltzer", "Remington", "Slamvan", "Blade", "Freight",
		"Streak", "Vortex", "Vincent", "Bullet", "Clover", "Sadler", "Firetruck LA", "Hustler", "Intruder",
		"Primo", "Cargobob", "Tampa", "Sunrise", "Merit", "Utility", "Nevada", "Yosemite", "Windsor",
		"Monster A", "Monster B", "Uranus", "Jester", "Sultan", "Stratum", "Elegy", "Raindance", "RC Tiger",
		"Flash", "Tahoma", "Savanna", "Bandito", "Freight Flat", "Streak Carriage", "Kart", "Mower", "Duneride",
		"Sweeper", "Broadway", "Tornado", "AT-400", "DFT-30", "Huntley", "Stafford", "BF-400", "Newsvan", "Tug",
		"Trailer 3", "Emperor", "Wayfarer", "Euros", "Hotdog", "Club", "Freight Carriage", "Trailer 3",
		"Andromada", "Dodo", "RC Cam", "Launch", "Police Car (LSPD)", "Police Car (SFPD)", "Police Car (LVPD)",
		"Police Ranger", "Picador", "S.W.A.T. Van", "Alpha", "Phoenix", "Glendale", "Sadler",
		"Luggage Trailer A", "Luggage Trailer B", "Stair Trailer", "Boxville", "Farm Plow", "Utility Trailer" };
	static_assert( sizeof k / sizeof *k == 212, "vehicle models 400..611" );
	return model >= 400 && model <= 611 ? k[model - 400] : nullptr;
}

const char *in_packet_name( int id ) {
	switch ( id ) {
	case 6: return "INTERNAL_PING";        case 7: return "PING";                  case 8: return "PING_OPEN_CONNECTIONS";
	case 9: return "CONNECTED_PONG";       case 10: return "REQUEST_STATIC_DATA";  case 11: return "CONNECTION_REQUEST";
	case 12: return "AUTH_KEY";            case 15: return "BROADCAST_PINGS";      case 16: return "SECURED_CONNECTION_RESPONSE";
	case 17: return "SECURED_CONNECTION_CONFIRMATION"; case 18: return "RPC_MAPPING"; case 21: return "SET_RANDOM_NUMBER_SEED";
	case 22: return "RPC";                 case 23: return "RPC_REPLY";            case 24: return "DETECT_LOST_CONNECTIONS";
	case 25: return "OPEN_CONNECTION_REQUEST"; case 26: return "OPEN_CONNECTION_REPLY"; case 28: return "RSA_PUBLIC_KEY_MISMATCH";
	case 29: return "CONNECTION_ATTEMPT_FAILED"; case 30: return "NEW_INCOMING_CONNECTION"; case 31: return "NO_FREE_INCOMING_CONNECTIONS";
	case 32: return "DISCONNECTION_NOTIFICATION"; case 33: return "CONNECTION_LOST"; case 34: return "CONNECTION_REQUEST_ACCEPTED";
	case 35: return "INITIALIZE_ENCRYPTION"; case 36: return "CONNECTION_BANNED"; case 37: return "INVALID_PASSWORD";
	case 38: return "MODIFIED_PACKET";     case 39: return "PONG";                 case 40: return "TIMESTAMP";
	case 41: return "RECEIVED_STATIC_DATA"; case 42: return "REMOTE_DISCONNECTION_NOTIFICATION"; case 43: return "REMOTE_CONNECTION_LOST";
	case 44: return "REMOTE_NEW_INCOMING_CONNECTION"; case 45: return "REMOTE_EXISTING_CONNECTION"; case 46: return "REMOTE_STATIC_DATA";
	case 56: return "ADVERTISE_SYSTEM";
	case 200: return "VEHICLE_SYNC";       case 201: return "RCON_COMMAND";        case 202: return "RCON_RESPONSE";
	case 203: return "AIM_SYNC";           case 204: return "WEAPONS_UPDATE";      case 205: return "STATS_UPDATE";
	case 206: return "BULLET_SYNC";        case 207: return "PLAYER_SYNC";         case 208: return "MARKERS_SYNC";
	case 209: return "UNOCCUPIED_SYNC";    case 210: return "TRAILER_SYNC";        case 211: return "PASSENGER_SYNC";
	case 212: return "SPECTATOR_SYNC";
	default: return "packet";
	}
}

std::string vk_name( unsigned vk ) {
	if ( ( vk >= '0' && vk <= '9' ) || ( vk >= 'A' && vk <= 'Z' ) ) return std::string( 1, (char) vk );
	if ( vk >= 0x70 && vk <= 0x87 ) return fmt( "F%u", vk - 0x6F );
	if ( vk >= 0x60 && vk <= 0x69 ) return fmt( "NUM%u", vk - 0x60 );
	switch ( vk ) {
	case 0x01: return "LMB";       case 0x02: return "RMB";        case 0x04: return "MMB";
	case 0x05: return "MOUSE4";    case 0x06: return "MOUSE5";     case 0x08: return "BACKSPACE";
	case 0x09: return "TAB";       case 0x0D: return "ENTER";      case 0x10: return "SHIFT";
	case 0x11: return "CTRL";      case 0x12: return "ALT";        case 0x13: return "PAUSE";
	case 0x14: return "CAPSLOCK";  case 0x1B: return "ESC";        case 0x20: return "SPACE";
	case 0x21: return "PAGEUP";    case 0x22: return "PAGEDOWN";   case 0x23: return "END";
	case 0x24: return "HOME";      case 0x25: return "LEFT";       case 0x26: return "UP";
	case 0x27: return "RIGHT";     case 0x28: return "DOWN";       case 0x2C: return "PRINTSCREEN";
	case 0x2D: return "INSERT";    case 0x2E: return "DELETE";     case 0x5B: return "LWIN";
	case 0x5C: return "RWIN";      case 0x6A: return "NUM*";       case 0x6B: return "NUM+";
	case 0x6D: return "NUM-";      case 0x6E: return "NUM.";       case 0x6F: return "NUM/";
	case 0x90: return "NUMLOCK";   case 0x91: return "SCROLLLOCK"; case 0xA0: return "LSHIFT";
	case 0xA1: return "RSHIFT";    case 0xA2: return "LCTRL";      case 0xA3: return "RCTRL";
	case 0xA4: return "LALT";      case 0xA5: return "RALT";       case 0xBA: return ";";
	case 0xBB: return "=";         case 0xBC: return ",";          case 0xBD: return "-";
	case 0xBE: return ".";         case 0xBF: return "/";          case 0xC0: return "`";
	case 0xDB: return "[";         case 0xDC: return "BACKSLASH";  case 0xDD: return "]";
	case 0xDE: return "'";
	default: return fmt( "VK 0x%02X", vk );
	}
}

const char *out_rpc_name( int id ) {
	switch ( id ) {
	case 23: return "ClickPlayer";      case 25: return "ClientJoin";       case 26: return "EnterVehicle";
	case 27: return "SelectObject";     case 31: return "ScriptCash";       case 50: return "cmd";
	case 52: return "Spawn";            case 53: return "Death";            case 54: return "NPCJoin";
	case 62: return "DialogResponse";   case 83: return "ClickTextDraw";    case 96: return "ScmEvent";
	case 101: return "chat";            case 102: return "NetStats";        case 103: return "ClientCheck";
	case 106: return "DamageVehicle";   case 115: return "Damage";          case 116: return "EditAttachedObject";
	case 117: return "EditObject";      case 118: return "Interior";        case 119: return "MapMarker";
	case 128: return "RequestClass";    case 129: return "RequestSpawn";    case 131: return "PickedUpPickup";
	case 132: return "MenuSelect";      case 136: return "VehicleDestroyed"; case 140: return "MenuQuit";
	case 154: return "ExitVehicle";     case 155: return "ScoresAndPings";  case 168: return "CameraTarget";
	case 177: return "ActorDamage";     case 181: return "RequestDff";      case 182: return "RequestTxd";
	case 184: return "DownloadDone";
	default: return "rpc";
	}
}

// ---------------------------------------------------------------------------
// Timeline
// ---------------------------------------------------------------------------

void Timeline::reset( uint32_t nowMs ) {
	*this = Timeline();
	t0_ = nowMs;
	windowStart_ = nowMs;
}

std::string Timeline::raw( const uint8_t *p, size_t bits ) const {
	if ( !opt_.everything || !p ) return std::string();
	return " | raw " + hex( p, ( bits + 7 ) / 8, 160 );
}

std::string Timeline::player_name( uint16_t pid ) const {
	std::string name = playerName_ ? playerName_( pid ) : std::string();
	if ( name.empty() ) {
		const auto it = names_.find( pid );
		if ( it != names_.end() ) name = it->second;
	}
	return clean( name );
}

int Timeline::vehicle_model( uint16_t vid ) const {
	int model = vehicleModel_ ? vehicleModel_( vid ) : -1;
	if ( model < 0 ) {
		const auto it = vehicleModels_.find( vid );
		if ( it != vehicleModels_.end() ) model = it->second;
	}
	return model;
}

std::string Timeline::who( uint16_t pid ) const {
	if ( mine( pid ) ) return "me";
	const std::string name = player_name( pid );
	return name.empty() ? fmt( "player %u", pid ) : fmt( "player %u (%s)", pid, name.c_str() );
}

std::string Timeline::vehicle_name( uint16_t vid ) const {
	const int model = vehicle_model( vid );
	const char *name = vehicle_model_name( model );
	if ( name ) return fmt( "vehicle %u (%s)", vid, name );
	return model > 0 ? fmt( "vehicle %u (model %d)", vid, model ) : fmt( "vehicle %u", vid );
}

std::string Timeline::target_suffix( uint8_t type, uint16_t id ) const {
	if ( type == 1 ) {
		if ( mine( id ) ) return " (me)";
		const std::string n = player_name( id );
		return n.empty() ? std::string() : " (" + n + ')';
	}
	if ( type == 2 ) {
		const char *m = vehicle_model_name( vehicle_model( id ) );
		return m ? fmt( " (%s)", m ) : std::string();
	}
	return std::string();
}

bool Timeline::dialog_row( int16_t dlg, int16_t item, std::string &row ) const {
	const auto st = dialogStyle_.find( dlg );
	const auto body = dialogBody_.find( dlg );
	if ( st == dialogStyle_.end() || body == dialogBody_.end() || item < 0 ) return false;
	const std::string &b = body->second;
	const int want = item + ( st->second == 5 ? 1 : 0 ); // a tablist with headers: line 0 is the header
	size_t from = 0;
	for ( int line = 0; from <= b.size(); ++line ) {
		const size_t nl = b.find( '\n', from );
		if ( line == want ) {
			row = b.substr( from, nl == std::string::npos ? std::string::npos : nl - from );
			return true;
		}
		if ( nl == std::string::npos ) break;
		from = nl + 1;
	}
	return false;
}

int Timeline::my_vehicle() const {
	if ( game_.lastKind == 2 && game_.haveCar ) return game_.car.vehicle;
	if ( game_.lastKind == 3 && game_.havePassenger ) return game_.passenger.vehicle;
	return -1;
}

bool Timeline::about_me( int id, const uint8_t *p, size_t bits ) const {
	BitReader r( p, bits );
	uint16_t first = 0;
	if ( !r.get( first ) ) return false;
	if ( player_first( id ) ) return mine( first );
	if ( vehicle_first( id ) ) return my_vehicle() >= 0 && first == (uint16_t) my_vehicle();
	return false;
}

std::string Timeline::stamp( uint32_t nowMs ) const {
	const uint32_t ms = nowMs - t0_;
	return fmt( "%6u.%03u", ms / 1000, ms % 1000 );
}

void Timeline::emit( uint32_t nowMs, const char *tag, const std::string &body, std::vector<std::string> &out ) {
	out.push_back( stamp( nowMs ) + ' ' + tag + body );
}

bool Timeline::near_me( const float p[3] ) const {
	if ( !havePos_ ) return false;
	const float dx = p[0] - myPos_[0], dy = p[1] - myPos_[1]; // 2D: a bobber can be under the water
	return dx * dx + dy * dy <= kNearMeters * kNearMeters;
}

std::string Timeline::key_changes( uint32_t *pressedAt, uint32_t before, uint32_t after, uint32_t nowMs, bool onFoot ) {
	std::string s;
	const uint32_t t = nowMs - t0_;
	for ( int i = 0; i < 19; ++i ) {
		const bool was = ( before & kKeys[i].bit ) != 0, is = ( after & kKeys[i].bit ) != 0;
		if ( was == is ) continue;
		if ( !s.empty() ) s += ", ";
		if ( is ) {
			pressedAt[i] = t;
			s += '+' + key_name( i, onFoot );
		} else {
			s += '-' + key_name( i, onFoot ) + fmt( " held %ums", t - pressedAt[i] );
		}
	}
	return s;
}

void Timeline::forget_session() {
	textdraws_.clear();
	objects_.clear();
	labels_.clear();
	pickups_.clear();
	actors_.clear();
	dialogStyle_.clear();
	dialogBody_.clear();
	others_.clear();
}

bool Timeline::switch_kind( Syncs &st, int kind, uint32_t nowMs ) {
	if ( st.lastKind == kind ) return false;
	st.lastKind = kind;
	if ( kind == 1 ) st.haveFoot = false;
	if ( kind == 2 ) st.haveCar = false;
	if ( kind == 3 ) st.havePassenger = false;
	for ( auto &t : st.pressedAt ) t = nowMs - t0_;
	return true;
}

std::string Timeline::keys_now() const {
	return key_names( lastKeys_, lastKeysOnFoot_ );
}

void Timeline::mark( uint32_t nowMs, const std::string &text, std::vector<std::string> &out ) {
	++marks_;
	emit( nowMs, "MARK ", text.empty() ? fmt( "%d", marks_ ) : fmt( "%d ", marks_ ) + quote( text ), out );
}

void Timeline::tick( uint32_t nowMs, std::vector<std::string> &out, bool force ) {
	if ( !force && nowMs - windowStart_ < kSummaryEveryMs ) return;
	const uint32_t secs = ( nowMs - windowStart_ + 500 ) / 1000;
	if ( !sent_.empty() || rpcsIn_ || rpcsOut_ || !skipped_.empty() ) {
		std::string s = fmt( "--- %u s: sent ", secs );
		if ( sent_.empty() ) s += "no syncs";
		for ( const auto &kv : sent_ ) {
			if ( kv.first != sent_.begin()->first ) s += ", ";
			s += fmt( "%s %u", kv.first.c_str(), kv.second );
		}
		s += fmt( " | RPCs in %u, out %u", rpcsIn_, rpcsOut_ );
		if ( !skipped_.empty() ) {
			std::vector<std::pair<std::string, unsigned>> v( skipped_.begin(), skipped_.end() );
			std::stable_sort( v.begin(), v.end(), []( const auto &a, const auto &b ) { return a.second > b.second; } );
			s += " | not written: ";
			for ( size_t i = 0; i < v.size() && i < 10; ++i ) {
				if ( i ) s += ", ";
				s += fmt( "%s x%u", v[i].first.c_str(), v[i].second );
			}
			if ( v.size() > 10 ) s += fmt( " (+%u more kinds)", (unsigned) ( v.size() - 10 ) );
		}
		// The textdraws updated most (shown or text set, written or not): a HUD clock
		// or meter stands out here instead of in the timeline.
		std::vector<std::pair<uint16_t, unsigned>> busy;
		for ( const auto &kv : tdUpdates_ )
			if ( kv.second >= 3 ) busy.push_back( kv );
		if ( !busy.empty() ) {
			std::stable_sort( busy.begin(), busy.end(), []( const auto &a, const auto &b ) { return a.second > b.second; } );
			s += " | busiest textdraws: ";
			for ( size_t i = 0; i < busy.size() && i < 5; ++i ) {
				if ( i ) s += ", ";
				s += td_id( busy[i].first ) + fmt( " x%u", busy[i].second );
			}
		}
		s += " ---";
		emit( nowMs, "", s, out );
	}
	windowStart_ = nowMs;
	sent_.clear();
	skipped_.clear();
	tdUpdates_.clear();
	rpcsIn_ = rpcsOut_ = 0;
}

// --- incoming ---------------------------------------------------------------

void Timeline::incoming_rpc( uint32_t nowMs, int id, const uint8_t *p, size_t bits, std::vector<std::string> &out ) {
	++rpcsIn_;
	std::string line;
	if ( !in_rpc( nowMs, id, p, bits, line ) ) return;
	if ( !( opt_.hideSecrets && id == 61 ) ) line += raw( p, bits ); // (a hidden password dialog's body stays hidden)
	emit( nowMs, "IN  ", line, out );
}

int Timeline::show_textdraw( const uint8_t *p, size_t bits, std::string &line ) {
	BitReader r( p, bits );
	uint16_t id = 0;
	TextDraw td;
	float rot[3] = {}, zoom = 0;
	int16_t c1 = 0, c2 = 0;
	if ( !( r.get( id ) && r.get( td.flags ) && r.get( td.letter[0] ) && r.get( td.letter[1] ) && r.get( td.color ) &&
	        r.get( td.size[0] ) && r.get( td.size[1] ) && r.get( td.box ) && r.get( td.shadow ) && r.get( td.outline ) &&
	        r.get( td.bg ) && r.get( td.font ) && r.get( td.selectable ) && r.get( td.pos[0] ) && r.get( td.pos[1] ) &&
	        r.get( td.model ) && r.get( rot[0] ) && r.get( rot[1] ) && r.get( rot[2] ) && r.get( zoom ) &&
	        r.get( c1 ) && r.get( c2 ) && r.str16( td.text ) ) )
		return -1;
	++tdUpdates_[id];

	line = fmt( "TD show %s font=%u", td_id( id ).c_str(), td.font );
	if ( td.flags & 1 )  line += " box";
	if ( td.flags & 2 )  line += " left";
	if ( td.flags & 4 )  line += " right";
	if ( td.flags & 8 )  line += " center";
	if ( td.flags & 16 ) line += " prop";
	if ( td.selectable ) line += " sel";
	line += fmt( " pos=(%.1f,%.1f) letter=(%.3f,%.3f) size=(%.1f,%.1f) color=%08X box=%08X bg=%08X shadow=%u outline=%u",
	             td.pos[0], td.pos[1], td.letter[0], td.letter[1], td.size[0], td.size[1], td.color, td.box, td.bg,
	             td.shadow, td.outline );
	if ( td.font == 5 )
		line += fmt( " model=%u rot=(%.1f,%.1f,%.1f) zoom=%.2f colors=%d,%d", td.model, rot[0], rot[1], rot[2], zoom, c1, c2 );
	line += ' ';
	line += quote( td.text );

	td.shown = true;
	td.visible = true;
	auto it = textdraws_.find( id );
	if ( it != textdraws_.end() && it->second.shown ) {
		const TextDraw &o = it->second;
		const bool again = !o.visible; // hidden since: the next attempt's bar, say -- always written
		std::string ch;
		auto num = [&]( const char *name, float a, float b ) {
			if ( a != b ) ch += fmt( ch.empty() ? "%s %.1f->%.1f" : ", %s %.1f->%.1f", name, a, b );
		};
		num( "size.x", o.size[0], td.size[0] );
		num( "size.y", o.size[1], td.size[1] );
		num( "pos.x", o.pos[0], td.pos[0] );
		num( "pos.y", o.pos[1], td.pos[1] );
		if ( o.letter[0] != td.letter[0] || o.letter[1] != td.letter[1] )
			ch += fmt( ch.empty() ? "letter (%.3f,%.3f)->(%.3f,%.3f)" : ", letter (%.3f,%.3f)->(%.3f,%.3f)",
			           o.letter[0], o.letter[1], td.letter[0], td.letter[1] );
		auto col = [&]( const char *name, uint32_t a, uint32_t b ) {
			if ( a != b ) ch += fmt( ch.empty() ? "%s %08X->%08X" : ", %s %08X->%08X", name, a, b );
		};
		col( "color", o.color, td.color );
		col( "box", o.box, td.box );
		col( "bg", o.bg, td.bg );
		if ( o.font != td.font || o.flags != td.flags || o.selectable != td.selectable || o.model != td.model )
			ch += ch.empty() ? "style" : ", style";
		if ( o.text != td.text ) ch += ch.empty() ? "text" : ", text";
		if ( ch.empty() && !again && !opt_.everything ) { skip( "TD show unchanged" ); return 0; } // a HUD re-showing itself: counted only
		if ( again ) line += ch.empty() ? " | (shown again, same as last show)" : " | shown again, changed: " + ch;
		else         line += ch.empty() ? " | (same as last show)" : " | changed: " + ch;
	}
	if ( textdraws_.size() > 4096 ) textdraws_.clear();
	textdraws_[id] = std::move( td );
	return 1;
}

bool Timeline::show_dialog( const uint8_t *p, size_t bits, std::string &line ) {
	BitReader r( p, bits );
	int16_t id = 0;
	uint8_t style = 0;
	std::string title, b1, b2;
	if ( !( r.get( id ) && r.get( style ) && r.str8( title ) && r.str8( b1 ) && r.str8( b2 ) ) ) return false;
	if ( dialogStyle_.size() > 4096 ) { dialogStyle_.clear(); dialogBody_.clear(); }
	dialogStyle_[id] = style; // what its reply is read against
	dialogBody_.erase( id );
	line = fmt( "Dialog id=%d style=%u(%s) title=", id, style, dialog_style( style ) ) + quote( title ) +
	       " buttons=" + quote( b1 ) + ',' + quote( b2 );
	if ( style == 3 && opt_.hideSecrets ) {
		line += " body hidden (password dialog)";
	} else if ( r.aligned() ) {
		const size_t at = r.pos() / 8, total = ( bits + 7 ) / 8;
		std::string body;
		if ( at < total && i18n::huffman::decode_string( p + at, total - at, body ) ) {
			line += " body=" + quote( body, 4096 );
			if ( style == 2 || style == 4 || style == 5 ) dialogBody_[id] = body; // to name the row picked
		} else {
			line += " body=(not decoded)";
		}
	}
	return true;
}

bool Timeline::in_rpc( uint32_t nowMs, int id, const uint8_t *p, size_t bits, std::string &line ) {
	(void) nowMs;
	BitReader r( p, bits );
	const char *name = in_rpc_name( id );
	// Names and models of server-assigned ids, learned whether or not the line is written.
	{
		BitReader q( p, bits );
		uint16_t pid = 0;
		if ( id == 137 ) { // ServerJoin: id, colour, NPC, name
			uint32_t color = 0; uint8_t npc = 0; std::string nm;
			if ( q.get( pid ) && q.get( color ) && q.get( npc ) && q.str8( nm ) && !nm.empty() ) names_[pid] = nm;
		} else if ( id == 11 ) { // SetPlayerName: id, name
			std::string nm;
			if ( q.get( pid ) && q.str8( nm ) && !nm.empty() ) names_[pid] = nm;
		} else if ( id == 164 ) { // WorldVehicleAdd: id, model, ...
			int32_t model = 0;
			if ( q.get( pid ) && q.get( model ) ) vehicleModels_[pid] = model;
		}
		// A player who joins, quits, streams in or out or dies starts fresh: no keys
		// "held" across it, and a new player on a freed id inherits nothing.
		if ( id == 137 || id == 138 || id == 32 || id == 163 || id == 166 ) {
			BitReader z( p, bits );
			uint16_t who16 = 0;
			if ( z.get( who16 ) ) others_.erase( who16 );
		}
		if ( names_.size() > 2048 ) names_.clear();
		if ( vehicleModels_.size() > 4096 ) vehicleModels_.clear();
	}

	if ( is_noisy_in( id ) && !opt_.others && !opt_.everything && !about_me( id, p, bits ) ) { skip( name ); return false; }
	const bool me = localId_ >= 0;
	bool ok = true;

	switch ( id ) {
	case 73: { uint32_t style = 0, time = 0; std::string t;
		ok = r.get( style ) && r.get( time ) && r.str32( t );
		if ( ok ) line = fmt( "GameText style=%u time=%ums ", style, time ) + quote( t );
		break; }
	case 134: {
		const int shown = show_textdraw( p, bits, line );
		if ( shown == 0 ) return false; // re-shown unchanged: counted
		ok = shown > 0;
		break; }
	case 105: { uint16_t td = 0; std::string t;
		ok = r.get( td ) && r.str16( t );
		if ( !ok ) break;
		++tdUpdates_[td];
		auto it = textdraws_.find( td );
		if ( it != textdraws_.end() && it->second.text == t && !opt_.everything ) { skip( "TD set unchanged" ); return false; }
		textdraws_[td].text = t;
		line = "TD set " + td_id( td ) + ' ' + quote( t );
		break; }
	case 135: { uint16_t td = 0;
		ok = r.get( td );
		if ( !ok ) break;
		auto it = textdraws_.find( td );
		if ( it != textdraws_.end() ) it->second.visible = false;
		line = "TD hide " + td_id( td );
		break; }
	case 93: { uint32_t color = 0; std::string t;
		ok = r.get( color ) && r.str32( t );
		if ( ok ) line = fmt( "Chat color=%08X ", color ) + quote( t );
		break; }
	case 61:
		ok = show_dialog( p, bits, line );
		break;
	case 86: { uint16_t pid = 0; std::string lib, an; float delta = 0; bool loop = false, lx = false, ly = false, fr = false; uint32_t time = 0;
		ok = r.get( pid );
		if ( ok && !mine( pid ) && !opt_.others && !opt_.everything ) { skip( "Anim (others)" ); return false; }
		ok = ok && r.str8( lib ) && r.str8( an ) && r.get( delta ) && r.bit( loop ) && r.bit( lx ) && r.bit( ly ) &&
		     r.bit( fr ) && r.get( time );
		if ( ok ) line = "Anim " + who( pid ) + ' ' + quote( lib ) + ' ' + quote( an ) +
		                 fmt( " delta=%.2f loop=%d lock=%d,%d freeze=%d time=%u", delta, loop, lx, ly, fr, time );
		break; }
	case 87: { uint16_t pid = 0;
		ok = r.get( pid );
		if ( ok && !mine( pid ) && !opt_.others && !opt_.everything ) { skip( "ClearAnim (others)" ); return false; }
		if ( ok ) line = "ClearAnim " + who( pid );
		break; }
	case 101: { uint16_t pid = 0; std::string t; // a player's chat line, as the server relays it (anyone's)
		ok = r.get( pid ) && r.str8( t );
		if ( ok ) line = "PlayerChat " + who( pid ) + ' ' + quote( t );
		break; }
	case 26: case 154: { uint16_t pid = 0, veh = 0; uint8_t pass = 0;
		ok = r.get( pid );
		if ( ok && !mine( pid ) && !opt_.others && !opt_.everything ) { skip( std::string( name ) + " (others)" ); return false; }
		ok = ok && r.get( veh ) && ( id == 154 || r.get( pass ) );
		if ( ok ) line = std::string( name ) + ' ' + who( pid ) + ' ' + vehicle_name( veh ) + ( pass ? " as passenger" : "" );
		break; }
	case 106: { uint16_t veh = 0; // damage status of a vehicle
		ok = r.get( veh );
		if ( ok && !( game_.haveCar && veh == game_.car.vehicle ) && !opt_.others && !opt_.everything ) { skip( "VehicleDamage (others)" ); return false; }
		if ( ok ) line = "VehicleDamage " + vehicle_name( veh ) + ' ' + generic( "data", id, p, bits );
		break; }
	case 96: { uint16_t pid = 0; uint32_t ev = 0, veh = 0, a = 0, b = 0;
		ok = r.get( pid );
		if ( ok && !mine( pid ) && !opt_.others && !opt_.everything ) { skip( "ScmEvent (others)" ); return false; }
		ok = ok && r.get( ev ) && r.get( veh ) && r.get( a ) && r.get( b );
		if ( ok ) line = "ScmEvent " + who( pid ) + fmt( " event=%u ", ev ) + vehicle_name( (uint16_t) veh ) + fmt( " params=%u,%u", a, b );
		break; }
	case 88: { uint8_t a = 0;
		ok = r.get( a );
		if ( ok ) line = fmt( "SpecialAction %u(%s)", a, special_action( a ) );
		break; }
	case 113: { uint16_t pid = 0; uint32_t slot = 0; bool create = false;
		ok = r.get( pid ) && r.get( slot ) && r.bit( create );
		if ( ok && !mine( pid ) && !opt_.others && !opt_.everything ) { skip( "AttachedObject (others)" ); return false; }
		if ( !ok ) break;
		if ( !create ) { line = "AttachedObject " + who( pid ) + fmt( " slot=%u removed", slot ); break; }
		int32_t model = 0; uint32_t bone = 0, c1 = 0, c2 = 0; float off[3] = {}, rot[3] = {}, sc[3] = {};
		ok = r.get( model ) && r.get( bone ) && r.get( off ) && r.get( rot ) && r.get( sc ) && r.get( c1 ) && r.get( c2 );
		if ( ok ) line = "AttachedObject " + who( pid ) + fmt( " slot=%u model=%d bone=%u(%s) off=%s rot=%s scale=%s", slot, model, bone,
		                      bone_name( bone ), xyz( off, 2 ).c_str(), xyz( rot ).c_str(), xyz( sc, 2 ).c_str() );
		break; }
	case 22: { uint32_t w = 0, ammo = 0;
		ok = r.get( w ) && r.get( ammo );
		if ( ok ) line = fmt( "GiveWeapon %u(%s) ammo=%u", w, weapon_name( w ), ammo );
		break; }
	case 21: line = "ResetWeapons"; break;
	case 67: { uint32_t w = 0;
		ok = r.get( w );
		if ( ok ) line = fmt( "ArmedWeapon %u(%s)", w, weapon_name( w ) );
		break; }
	case 145: { uint8_t w = 0; uint16_t ammo = 0;
		ok = r.get( w ) && r.get( ammo );
		if ( ok ) line = fmt( "SetAmmo %u(%s) ammo=%u", w, weapon_name( w ), ammo );
		break; }
	case 15: { uint8_t m = 0;
		ok = r.get( m );
		if ( ok ) line = fmt( "Controllable %u(%s)", m, m ? "free" : "frozen" );
		break; }
	case 16: { uint32_t s = 0; float v[3] = {};
		ok = r.get( s ) && r.get( v );
		if ( ok ) line = fmt( "Sound %u at %s", s, xyz( v ).c_str() );
		break; }
	case 14: case 66: { float v = 0;
		ok = r.get( v );
		if ( ok ) line = fmt( "%s %.1f", name, v );
		break; }
	case 35: { int32_t v = 0;
		ok = r.get( v );
		if ( ok ) line = fmt( "DrunkLevel %d", v );
		break; }
	case 12: case 13: case 90: case 157: { float v[3] = {};
		ok = r.get( v );
		if ( ok ) line = std::string( name ) + ' ' + xyz( v, id == 90 ? 2 : 1 );
		break; }
	case 19: { float a = 0;
		ok = r.get( a );
		if ( ok ) line = fmt( "SetFacing %.1f", a );
		break; }
	case 18: { int32_t v = 0;
		ok = r.get( v );
		if ( ok ) line = fmt( "Money %+d", v );
		break; }
	case 20: case 37: case 39: case 42: case 162: case 71:
		line = name;
		break;
	case 41: { std::string url; float v[3] = {}, radius = 0; uint8_t usePos = 0;
		ok = r.str8( url ) && r.get( v ) && r.get( radius ) && r.get( usePos );
		if ( ok ) line = "AudioStream " + quote( url ) + ( usePos ? " at " + xyz( v ) + fmt( " r=%.0f", radius ) : "" );
		break; }
	case 158: { float v[3] = {}; uint8_t cut = 0;
		ok = r.get( v ) && r.get( cut );
		if ( ok ) line = "CameraLookAt " + xyz( v ) + fmt( " cut=%u", cut );
		break; }
	case 82: { bool posSet = false; float a[3] = {}, b[3] = {}; uint32_t time = 0; uint8_t cut = 0;
		ok = r.bit( posSet ) && r.get( a ) && r.get( b ) && r.get( time ) && r.get( cut );
		if ( ok ) line = fmt( "InterpolateCamera %s ", posSet ? "pos" : "look-at" ) + xyz( a ) + " -> " + xyz( b ) +
		                 fmt( " time=%u cut=%u", time, cut );
		break; }
	case 81: { uint16_t obj = 0;
		ok = r.get( obj );
		if ( ok ) line = fmt( "AttachCameraToObject #%u", obj );
		break; }
	case 83: { bool on = false; uint32_t color = 0; // a 1-bit switch, then the hover colour (open.mp)
		ok = r.bit( on ) && r.get( color );
		if ( ok ) line = fmt( "SelectTextDraw %s hover=%08X", on ? "on" : "off", color );
		break; }
	case 107: { float v[3] = {}, radius = 0;
		ok = r.get( v ) && r.get( radius );
		if ( ok ) line = "Checkpoint " + xyz( v ) + fmt( " r=%.1f", radius );
		break; }
	case 38: { uint8_t type = 0; float a[3] = {}, b[3] = {}, radius = 0;
		ok = r.get( type ) && r.get( a ) && r.get( b ) && r.get( radius );
		if ( ok ) line = fmt( "RaceCheckpoint type=%u at ", type ) + xyz( a ) + " next " + xyz( b ) + fmt( " r=%.1f", radius );
		break; }
	case 56: { uint8_t icon = 0, type = 0, style = 0; float v[3] = {}; uint32_t color = 0;
		ok = r.get( icon ) && r.get( v ) && r.get( type ) && r.get( color ) && r.get( style );
		if ( ok ) line = fmt( "MapIcon #%u type=%u at ", icon, type ) + xyz( v );
		break; }
	case 144: case 77: case 78: case 156: { uint8_t v = 0;
		ok = r.get( v );
		if ( ok ) line = fmt( "%s %u", name, v );
		break; }
	case 70: { uint16_t veh = 0; uint8_t seat = 0;
		ok = r.get( veh ) && r.get( seat );
		if ( ok ) line = "PutInVehicle " + vehicle_name( veh ) + fmt( " seat=%u", seat );
		break; }
	case 59: { uint16_t pid = 0; uint32_t color = 0, expire = 0; float dist = 0; std::string t;
		ok = r.get( pid );
		if ( ok && !mine( pid ) && !opt_.others && !opt_.everything ) { skip( "Bubble (others)" ); return false; }
		ok = ok && r.get( color ) && r.get( dist ) && r.get( expire ) && r.str8( t );
		if ( ok && !opt_.everything && t.find_first_not_of( " \t" ) == std::string::npos ) { skip( "Bubble (blank)" ); return false; }
		if ( ok ) line = "Bubble " + who( pid ) + fmt( " color=%08X time=%u ", color, expire ) + quote( t );
		break; }
	case 153: { uint32_t pid = 0, skin = 0; // 0.3.DL: u16 player, skin, custom skin; 0.3.7 (compat): u32 player, skin
		if ( bits == 64 ) {
			ok = r.get( pid ) && r.get( skin );
		} else {
			uint16_t p16 = 0;
			ok = r.get( p16 ) && r.get( skin );
			pid = p16;
		}
		const bool ours = me && pid == (uint32_t) localId_;
		if ( ok && !ours && !opt_.others && !opt_.everything ) { skip( "SetSkin (others)" ); return false; }
		if ( ok ) line = ours ? fmt( "SetSkin %u", skin ) : "SetSkin " + who( (uint16_t) pid ) + fmt( " %u", skin );
		break; }
	case 44: { uint16_t obj = 0, attObj = 0, attVeh = 0; int32_t model = 0; float v[3] = {}, rot[3] = {}, dist = 0; uint8_t noCam = 0;
		// The attached VEHICLE comes first (as Fuego's proven LSRP gun-stealer + open.mp
		// read it -- the packet wiki lists them the other way round).
		ok = r.get( obj ) && r.get( model ) && r.get( v ) && r.get( rot ) && r.get( dist ) && r.get( noCam ) &&
		     r.get( attVeh ) && r.get( attObj );
		if ( !ok ) break;
		const bool onMyCar = attVeh != 0xFFFF && game_.haveCar && attVeh == game_.car.vehicle;
		if ( !near_me( v ) && !onMyCar && !opt_.everything ) { skip( "Object (far)" ); return false; }
		objects_.insert( obj );
		line = fmt( "Object #%u create model=%d at ", obj, model ) + xyz( v ) + " rot=" + xyz( rot );
		if ( attVeh != 0xFFFF ) line += " on " + vehicle_name( attVeh );
		if ( attObj != 0xFFFF ) line += fmt( " on object %u", attObj );
		break; }
	case 45: case 46: { uint16_t obj = 0; float v[3] = {};
		ok = r.get( obj ) && r.get( v );
		if ( !ok ) break;
		if ( !objects_.count( obj ) ) {
			if ( !opt_.everything && ( id == 46 || !near_me( v ) ) ) { skip( std::string( name ) + " (far)" ); return false; }
			objects_.insert( obj );
		}
		line = fmt( "Object #%u %s ", obj, id == 45 ? "pos" : "rot" ) + xyz( v );
		break; }
	case 99: { uint16_t obj = 0; float from[3] = {}, to[3] = {}, speed = 0;
		ok = r.get( obj ) && r.get( from ) && r.get( to ) && r.get( speed );
		if ( !ok ) break;
		if ( !objects_.count( obj ) ) {
			if ( !near_me( to ) && !opt_.everything ) { skip( "MoveObject (far)" ); return false; }
			objects_.insert( obj );
		}
		line = fmt( "Object #%u move ", obj ) + xyz( from ) + " -> " + xyz( to ) + fmt( " speed=%.2f", speed );
		break; }
	case 47: case 122: case 84: { uint16_t obj = 0;
		ok = r.get( obj );
		if ( !ok ) break;
		if ( !objects_.count( obj ) ) { skip( std::string( name ) + " (far)" ); return false; }
		if ( id == 47 ) { objects_.erase( obj ); line = fmt( "Object #%u destroyed", obj ); }
		else if ( id == 122 ) line = fmt( "Object #%u stopped", obj );
		else line = fmt( "ObjectMaterial #%u ", obj ) + generic( "data", id, p, bits );
		break; }
	case 75: { uint16_t obj = 0, pid = 0; float off[3] = {}, rot[3] = {};
		ok = r.get( obj ) && r.get( pid );
		if ( ok && !mine( pid ) && !opt_.others && !opt_.everything ) { skip( "AttachObjectToPlayer (others)" ); return false; }
		ok = ok && r.get( off ) && r.get( rot );
		if ( ok ) { objects_.insert( obj ); line = fmt( "Object #%u attached to ", obj ) + who( pid ) + " off=" + xyz( off, 2 ) + " rot=" + xyz( rot ); }
		break; }
	case 36: { uint16_t lbl = 0, attP = 0, attV = 0; uint32_t color = 0; float v[3] = {}, dist = 0; uint8_t los = 0;
		ok = r.get( lbl ) && r.get( color ) && r.get( v ) && r.get( dist ) && r.get( los ) && r.get( attP ) && r.get( attV );
		if ( !ok ) break;
		const bool onMe = me && attP == (uint16_t) localId_;
		const bool onMyCar = attV != 0xFFFF && game_.haveCar && attV == game_.car.vehicle; // its position is an offset then
		if ( !onMe && !onMyCar && !near_me( v ) && !opt_.everything ) { skip( "Label (far)" ); return false; }
		labels_.insert( lbl );
		std::string text;
		const size_t at = r.pos() / 8, total = ( bits + 7 ) / 8;
		const bool decoded = at < total && i18n::huffman::decode_string( p + at, total - at, text );
		line = fmt( "Label #%u ", lbl ) + ( onMe ? std::string( "on me " ) : onMyCar ? std::string( "on my vehicle " )
		                                                                             : "at " + xyz( v ) + ' ' ) +
		       fmt( "color=%08X ", color ) + ( decoded ? quote( text ) : std::string( "(text not decoded)" ) );
		break; }
	// Actors (server NPCs -- e.g. a fish buyer): the ones near us, and later changes to those.
	case 171: { uint16_t a = 0; uint32_t skin = 0, custom = 0; float v[3] = {}, angle = 0, hp = 0; uint8_t inv = 0;
		ok = r.get( a ) && r.get( skin ) && ( bits < 31 * 8 || r.get( custom ) ) && r.get( v ) && r.get( angle ) &&
		     r.get( hp ) && r.get( inv ); // 0.3.DL has a custom skin id; 0.3.7 doesn't (27 bytes)
		if ( !ok ) break;
		if ( !near_me( v ) && !opt_.everything ) { skip( "Actor (far)" ); return false; }
		actors_.insert( a );
		line = fmt( "Actor #%u show skin=%u at ", a, skin ) + xyz( v ) + fmt( " angle=%.0f hp=%.0f", angle, hp );
		break; }
	case 172: case 174: case 175: case 176: case 178: case 173: { uint16_t a = 0;
		ok = r.get( a );
		if ( !ok ) break;
		if ( !actors_.count( a ) ) { skip( std::string( name ) + " (far)" ); return false; }
		if ( id == 172 ) { actors_.erase( a ); line = fmt( "Actor #%u hidden", a ); break; }
		if ( id == 174 ) { line = fmt( "Actor #%u anim cleared", a ); break; }
		if ( id == 175 || id == 178 ) {
			float f = 0;
			ok = r.get( f );
			if ( ok ) line = fmt( id == 175 ? "Actor #%u facing %.1f" : "Actor #%u health %.1f", a, f );
			break;
		}
		if ( id == 176 ) {
			float v[3] = {};
			ok = r.get( v );
			if ( ok ) line = fmt( "Actor #%u pos ", a ) + xyz( v );
			break;
		}
		std::string lib, an; float delta = 0; bool loop = false, lx = false, ly = false, fr = false; uint32_t time = 0;
		ok = r.str8( lib ) && r.str8( an ) && r.get( delta ) && r.bit( loop ) && r.bit( lx ) && r.bit( ly ) &&
		     r.bit( fr ) && r.get( time );
		if ( ok ) line = fmt( "Actor #%u anim ", a ) + quote( lib ) + ' ' + quote( an ) +
		                 fmt( " delta=%.2f loop=%d lock=%d,%d freeze=%d time=%u", delta, loop, lx, ly, fr, time );
		break; }
	case 58: { uint16_t lbl = 0;
		ok = r.get( lbl );
		if ( !ok ) break;
		if ( !labels_.count( lbl ) ) { skip( "DeleteLabel (far)" ); return false; }
		labels_.erase( lbl );
		line = fmt( "Label #%u deleted", lbl );
		break; }
	case 95: { int32_t pk = 0, model = 0, type = 0; float v[3] = {};
		ok = r.get( pk ) && r.get( model ) && r.get( type ) && r.get( v );
		if ( !ok ) break;
		if ( !near_me( v ) && !opt_.everything ) { skip( "Pickup (far)" ); return false; }
		pickups_.insert( pk );
		line = fmt( "Pickup #%d model=%d type=%d at ", pk, model, type ) + xyz( v );
		break; }
	case 63: { int32_t pk = 0;
		ok = r.get( pk );
		if ( !ok ) break;
		if ( !pickups_.count( pk ) ) { skip( "DestroyPickup (far)" ); return false; }
		pickups_.erase( pk );
		line = fmt( "Pickup #%d destroyed", pk );
		break; }
	case 137: { uint16_t pid = 0; uint32_t color = 0; uint8_t npc = 0; std::string nm; // (others / everything)
		ok = r.get( pid ) && r.get( color ) && r.get( npc ) && r.str8( nm );
		if ( ok ) line = fmt( "ServerJoin player %u ", pid ) + quote( nm ) + ( npc ? " (NPC)" : "" ) + fmt( " color=%08X", color );
		break; }
	case 138: { uint16_t pid = 0; uint8_t reason = 0;
		ok = r.get( pid ) && r.get( reason );
		static const char *kWhy[] = { "timed out", "quit", "kicked/banned" };
		if ( ok ) line = "ServerQuit " + who( pid ) + fmt( " (%s)", reason < 3 ? kWhy[reason] : "?" );
		break; }
	case 11: { uint16_t pid = 0; std::string nm;
		ok = r.get( pid ) && r.str8( nm );
		if ( ok ) line = fmt( "SetPlayerName player %u -> ", pid ) + quote( nm );
		break; }
	case 55: { uint16_t killer = 0, victim = 0; uint8_t reason = 0; // the kill feed
		ok = r.get( killer ) && r.get( victim ) && r.get( reason );
		if ( ok ) line = "Kill " + ( killer == 0xFFFF ? std::string( "(nobody)" ) : who( killer ) ) + " killed " + who( victim ) +
		                 fmt( " with %u(%s)", reason, weapon_name( reason ) );
		break; }
	case 164: { uint16_t vid = 0; int32_t model = 0; float v[3] = {};
		ok = r.get( vid ) && r.get( model ) && r.get( v );
		if ( ok ) line = "WorldVehicleAdd " + vehicle_name( vid ) + " at " + xyz( v );
		break; }
	case 103: { uint8_t type = 0; uint32_t addr = 0; uint16_t off = 0, count = 0; // an anti-cheat check request
		ok = r.get( type ) && r.get( addr ) && r.get( off ) && r.get( count );
		if ( ok ) line = fmt( "ClientCheck request type=%u address=%08X offset=%u count=%u", type, addr, off, count );
		break; }
	case 124: { uint32_t on = 0; // TogglePlayerSpectating (us)
		ok = r.get( on );
		if ( ok ) line = fmt( "Spectating %s", on ? "on" : "off" );
		break; }
	case 126: case 127: { uint16_t target = 0; uint8_t mode = 0; // spectate a player / a vehicle
		ok = r.get( target ) && r.get( mode );
		if ( ok ) line = std::string( name ) + ' ' + ( id == 126 ? who( target ) : vehicle_name( target ) ) + fmt( " mode=%u", mode );
		break; }
	case 139:
		forget_session();
		line = "InitGame (new session)";
		break;
	case 40:
		forget_session();
		line = "GameModeRestart";
		break;
	case 32: case 163: case 166: { uint16_t pid = 0; // a player streamed in / out, died
		ok = r.get( pid );
		if ( ok ) line = std::string( name ) + ' ' + who( pid ) + ( id == 32 ? " (streamed in)" : id == 163 ? " (streamed out)" : "" );
		break; }
	case 165: { uint16_t vid = 0;
		ok = r.get( vid );
		if ( ok ) line = "WorldVehicleRemove " + vehicle_name( vid ) + " (streamed out)";
		break; }
	case 147: { uint16_t vid = 0; float hp = 0;
		ok = r.get( vid ) && r.get( hp );
		if ( ok ) line = "SetVehicleHealth " + vehicle_name( vid ) + fmt( " %.1f", hp );
		break; }
	case 159: { uint16_t vid = 0; float v[3] = {};
		ok = r.get( vid ) && r.get( v );
		if ( ok ) line = "SetVehiclePos " + vehicle_name( vid ) + ' ' + xyz( v );
		break; }
	case 72: { uint16_t pid = 0; uint32_t color = 0;
		ok = r.get( pid ) && r.get( color );
		if ( ok ) line = "SetPlayerColor " + who( pid ) + fmt( " %08X", color );
		break; }
	case 69: { uint16_t pid = 0; uint8_t team = 0;
		ok = r.get( pid ) && r.get( team );
		if ( ok ) line = "SetPlayerTeam " + who( pid ) + fmt( " %u", team );
		break; }
	default: {
		BitReader q( p, bits );
		uint16_t first = 0;
		if ( ( player_first( id ) || vehicle_first( id ) ) && q.get( first ) )
			line = std::string( name ) + ' ' + ( player_first( id ) ? who( first ) : vehicle_name( first ) ) + ' ' + bytes_tail( p, bits );
		else
			line = generic( name, id, p, bits );
		break; }
	}
	if ( !ok ) line = generic( name, id, p, bits ) + " (cut off?)";
	if ( objects_.size() > 4096 ) objects_.clear();
	if ( labels_.size() > 4096 ) labels_.clear();
	if ( pickups_.size() > 4096 ) pickups_.clear();
	return true;
}

// --- outgoing ---------------------------------------------------------------

void Timeline::outgoing_rpc( uint32_t nowMs, int id, const uint8_t *p, size_t bits, bool injected, std::vector<std::string> &out ) {
	++rpcsOut_;
	std::string line;
	if ( !out_rpc( id, p, bits, line ) ) return;
	if ( !( opt_.hideSecrets && private_out( id ) ) ) line += raw( p, bits ); // what was typed stays out of a shared log
	emit( nowMs, injected ? "OUT [fuego] " : "OUT ", line, out );
}

bool Timeline::out_rpc( int id, const uint8_t *p, size_t bits, std::string &line ) {
	BitReader r( p, bits );
	const char *name = out_rpc_name( id );
	bool ok = true;
	switch ( id ) {
	case 50: { std::string t;
		ok = r.str32( t );
		if ( !ok ) break;
		if ( opt_.hideSecrets && sensitive_command( t ) ) { // only the command word; whatever follows it, however joined, is hidden
			const size_t end = command_word_end( t );
			line = "cmd " + quote( t.substr( 0, end ) ) + ( end < t.size() ? " <arguments hidden>" : "" );
		} else {
			line = "cmd " + quote( t );
		}
		break; }
	case 101: { std::string t;
		ok = r.str8( t );
		if ( ok ) line = "chat " + quote( t );
		break; }
	case 62: { int16_t dlg = 0, item = 0; uint8_t button = 0;
		ok = r.get( dlg ) && r.get( button ) && r.get( item );
		if ( !ok ) break;
		line = fmt( "DialogResponse id=%d button=%u(%s) list=%d", dlg, button, button ? "left" : "right", item );
		std::string row;
		if ( dialog_row( dlg, item, row ) ) line += " row " + quote( row ); // the server's text for the row picked
		if ( opt_.hideSecrets ) {
			uint8_t len = 0;
			ok = r.get( len ); // the input text itself: never read
			if ( !ok ) break;
			// Even the typed length only where nothing is typed by hand (a list row, a message
			// box) -- never for an input/password dialog, or one we didn't see shown.
			const auto st = dialogStyle_.find( dlg );
			const bool lengthOk = st != dialogStyle_.end() && st->second != 1 && st->second != 3;
			line += lengthOk ? fmt( " input=<%u chars hidden>", len ) : std::string( " input=<hidden>" );
		} else {
			std::string input;
			ok = r.str8( input );
			if ( ok ) line += " input=" + quote( input );
		}
		break; }
	case 83: { uint16_t td = 0;
		ok = r.get( td );
		if ( ok ) line = td == 0xFFFF ? std::string( "ClickTextDraw cancel" ) : "ClickTextDraw " + td_id( td );
		break; }
	case 23: { uint16_t pid = 0; uint8_t src = 0;
		ok = r.get( pid ) && r.get( src );
		if ( ok ) line = "ClickPlayer " + who( pid ) + fmt( " source=%u", src );
		break; }
	case 26: { uint16_t veh = 0; uint8_t pass = 0;
		ok = r.get( veh ) && r.get( pass );
		if ( ok ) line = "EnterVehicle " + vehicle_name( veh ) + ( pass ? " as passenger" : "" );
		break; }
	case 154: { uint16_t veh = 0;
		ok = r.get( veh );
		if ( ok ) line = "ExitVehicle " + vehicle_name( veh );
		break; }
	case 131: case 128: { int32_t v = 0;
		ok = r.get( v );
		if ( ok ) line = fmt( "%s %d", name, v );
		break; }
	case 115: { bool take = false; uint16_t pid = 0; float amount = 0; uint32_t w = 0, body = 0;
		ok = r.bit( take ) && r.get( pid ) && r.get( amount ) && r.get( w ) && r.get( body );
		if ( ok ) line = fmt( "Damage %s ", take ? "taken from" : "given to" ) + who( pid ) +
		                 fmt( " amount=%.1f wpn=%u(%s) body=%u", amount, w, weapon_name( w ), body );
		break; }
	case 106: { uint16_t veh = 0;
		ok = r.get( veh );
		if ( ok ) line = "DamageVehicle " + vehicle_name( veh ) + ' ' + bytes_tail( p, bits );
		break; }
	case 136: { uint16_t veh = 0;
		ok = r.get( veh );
		if ( ok ) line = "VehicleDestroyed " + vehicle_name( veh );
		break; }
	case 132: case 118: { uint8_t v = 0;
		ok = r.get( v );
		if ( ok ) line = fmt( "%s %u", name, v );
		break; }
	case 53: { uint8_t reason = 0; uint16_t killer = 0;
		ok = r.get( reason ) && r.get( killer );
		if ( ok ) line = fmt( "Death reason=%u(%s) killer=", reason, weapon_name( reason ) ) +
		                 ( killer == 0xFFFF ? std::string( "none" ) : who( killer ) );
		break; }
	case 119: { float v[3] = {};
		ok = r.get( v );
		if ( ok ) line = "MapMarker " + xyz( v );
		break; }
	case 117: { bool playerObj = false; uint16_t obj = 0; uint32_t resp = 0; float v[3] = {}, rot[3] = {};
		ok = r.bit( playerObj ) && r.get( obj ) && r.get( resp ) && r.get( v ) && r.get( rot );
		if ( ok ) line = fmt( "EditObject #%u response=%u at ", obj, resp ) + xyz( v ) + " rot=" + xyz( rot );
		break; }
	case 52: case 129: case 140:
		line = name;
		break;
	case 103: { // an anti-cheat check's answer
		if ( opt_.hideSecrets ) { line = "ClientCheck answer (hidden)"; break; }
		uint8_t type = 0, answer = 0; uint32_t addr = 0;
		ok = r.get( type ) && r.get( addr ) && r.get( answer );
		if ( ok ) line = fmt( "ClientCheck type=%u address=%08X answer=%02X", type, addr, answer );
		break; }
	case 25: { // joining: client version, name, the challenge answer, the auth key (gpci), the client's version string
		int32_t ver = 0; uint8_t mod = 0; uint32_t challenge = 0; std::string nm, auth, client;
		ok = r.get( ver ) && r.get( mod ) && r.str8( nm ) && r.get( challenge ) && r.str8( auth ) && r.str8( client );
		if ( !ok ) break;
		line = fmt( "ClientJoin version=%d mod=%u name=", ver, mod ) + quote( nm ) +
		       ( opt_.hideSecrets ? std::string( " challenge/auth=<hidden>" ) : fmt( " challenge=%08X auth=", challenge ) + quote( auth ) ) +
		       " client=" + quote( client );
		break; }
	case 168: case 155: case 102: case 181: case 182: case 184: // camera target, score requests, stats, downloads
		if ( !opt_.everything ) { skip( name ); return false; }
		line = generic( name, id, p, bits );
		break;
	default:
		line = generic( name, id, p, bits );
		break;
	}
	if ( !ok ) {
		// Cut off. In a log to share something typed may be in there: never dumped then.
		line = opt_.hideSecrets && private_out( id ) ? fmt( "%s (cut off, not shown)", name ) : generic( name, id, p, bits ) + " (cut off?)";
	}
	return true;
}

void Timeline::outgoing_packet( uint32_t nowMs, const uint8_t *p, size_t bits, bool injected, std::vector<std::string> &out ) {
	if ( !p || bits < 8 ) return;
	const uint8_t id = p[0];
	const uint8_t *d = p + 1;
	const size_t n = ( bits + 7 ) / 8 - 1; // payload bytes after the id
	const char *tag = injected ? "OUT [fuego] " : "OUT ";
	const std::string pre = injected ? "[fuego] " : "";
	Syncs &st = injected ? injected_ : game_;
	const std::string rawBytes = raw( p, bits ); // "everything" only
	// What we stand on: a vehicle id, an object as 2000 + its id (0 / 0xFFFF: nothing).
	auto surfing = [&]( uint16_t sid ) {
		if ( sid == 0 || sid == 0xFFFF ) return std::string();
		return sid < 2000 ? " surfing " + vehicle_name( sid ) : fmt( " surfing object #%u", sid - 2000u );
	};

	switch ( id ) {
	case 207: { // on foot
		++sent_[pre + "foot"];
		samp::stOnFootData s;
		if ( n < sizeof s ) { skip( "foot (cut off)" ); return; }
		std::memcpy( &s, d, sizeof s );
		Foot f;
		f.keys = s.sKeys | extra_keys( s.byteCurrentWeapon );
		f.ud = (int16_t) s.sUpDownKeys;
		f.lr = (int16_t) s.sLeftRightKeys;
		f.weapon = s.byteCurrentWeapon & 0x3F;
		f.action = s.byteSpecialAction;
		f.health = s.byteHealth;
		f.armor = s.byteArmor;
		f.anim = s.sCurrentAnimationID;
		f.animFlags = s.sAnimFlags;
		std::memcpy( f.pos, s.fPosition, sizeof f.pos );
		f.yaw = yaw_deg( s.fQuaternion );
		if ( !injected ) { // the game's own position + keys ("near me", the REC pill)
			std::memcpy( myPos_, f.pos, sizeof myPos_ );
			havePos_ = true;
			lastKeys_ = f.keys;
			lastKeysOnFoot_ = true;
		}

		switch_kind( st, 1, nowMs ); // back on foot after driving: starts fresh
		std::string ch;
		if ( !st.haveFoot ) {
			ch = "(start)";
			for ( int i = 0; i < 19; ++i ) st.pressedAt[i] = nowMs - t0_;
		} else {
			ch = key_changes( st.pressedAt, st.foot.keys, f.keys, nowMs, true );
			auto add = [&]( const std::string &s2 ) { if ( !ch.empty() ) ch += ", "; ch += s2; };
			if ( st.foot.ud != f.ud ) add( "ud " + axis( st.foot.ud, 'W', 'S' ) + "->" + axis( f.ud, 'W', 'S' ) );
			if ( st.foot.lr != f.lr ) add( "lr " + axis( st.foot.lr, 'A', 'D' ) + "->" + axis( f.lr, 'A', 'D' ) );
			if ( st.foot.weapon != f.weapon ) add( fmt( "wpn %u->%u(%s)", st.foot.weapon, f.weapon, weapon_name( f.weapon ) ) );
			if ( st.foot.action != f.action ) add( fmt( "act %u->%u(%s)", st.foot.action, f.action, special_action( f.action ) ) );
			if ( st.foot.anim != f.anim ) add( fmt( "anim %d->%d", st.foot.anim, f.anim ) );
			if ( st.foot.health != f.health ) add( fmt( "hp %u->%u", st.foot.health, f.health ) );
			if ( st.foot.armor != f.armor ) add( fmt( "ar %u->%u", st.foot.armor, f.armor ) );
		}
		st.haveFoot = true;
		st.foot = f;
		if ( ch.empty() ) {
			if ( !opt_.everything ) return;
			ch = "(no change)";
		}
		emit( nowMs, tag, "foot " + ch + " | keys=" + key_names( f.keys, true ) +
		      fmt( " ud=%s lr=%s wpn=%u(%s) act=%u anim=%d/%d hp=%u ar=%u pos=", axis( f.ud, 'W', 'S' ).c_str(),
		           axis( f.lr, 'A', 'D' ).c_str(), f.weapon, weapon_name( f.weapon ), f.action, f.anim, f.animFlags,
		           f.health, f.armor ) + xyz( f.pos ) + fmt( " yaw=%.0f", f.yaw ) +
		      surfing( s.sSurfingVehicleID ) + rawBytes, out );
		return; }

	case 203: { // aim
		++sent_[pre + "aim"];
		samp::stAimData s;
		if ( n < sizeof s ) { skip( "aim (cut off)" ); return; }
		std::memcpy( &s, d, sizeof s );
		Aim a;
		a.cam = s.byteCamMode;
		a.state = s.byteWeaponState;
		a.zoom = s.byteCamExtZoom;
		const bool changed = !st.haveAim || a.cam != st.aim.cam || a.state != st.aim.state || a.zoom != st.aim.zoom;
		st.haveAim = true;
		st.aim = a;
		if ( changed || opt_.everything )
			emit( nowMs, tag, fmt( "aim cam=%u(%s) wstate=%u(%s) zoom=%u aimZ=%.2f", a.cam, cam_mode( a.cam ), a.state,
			                       weapon_state( a.state ), a.zoom, s.fAimZ ) + ( changed ? "" : " (no change)" ) + rawBytes, out );
		return; }

	case 200: { // driving
		++sent_[pre + "car"];
		samp::stInCarData s;
		if ( n < sizeof s ) { skip( "car (cut off)" ); return; }
		std::memcpy( &s, d, sizeof s );
		Car c;
		c.vehicle = s.sVehicleID;
		c.keys = s.sKeys | extra_keys( s.byteCurrentWeapon );
		c.ud = (int16_t) s.sUpDownKeys;
		c.lr = (int16_t) s.sLeftRightKeys;
		c.weapon = s.byteCurrentWeapon & 0x3F;
		c.health = s.bytePlayerHealth;
		c.armor = s.byteArmor;
		c.carHp = s.fVehicleHealth;
		std::memcpy( c.pos, s.fPosition, sizeof c.pos );
		if ( !injected ) { // the game's own position + keys ("near me", the REC pill)
			std::memcpy( myPos_, c.pos, sizeof myPos_ );
			havePos_ = true;
			lastKeys_ = c.keys;
			lastKeysOnFoot_ = false;
		}
		switch_kind( st, 2, nowMs );
		std::string ch;
		if ( !st.haveCar || st.car.vehicle != c.vehicle ) {
			ch = "(in " + vehicle_name( c.vehicle ) + ')';
			for ( int i = 0; i < 19; ++i ) st.pressedAt[i] = nowMs - t0_;
		} else {
			ch = key_changes( st.pressedAt, st.car.keys, c.keys, nowMs, false );
			auto add = [&]( const std::string &s2 ) { if ( !ch.empty() ) ch += ", "; ch += s2; };
			if ( st.car.ud != c.ud ) add( fmt( "ud %d->%d", st.car.ud, c.ud ) );
			if ( st.car.lr != c.lr ) add( fmt( "lr %d->%d", st.car.lr, c.lr ) );
			if ( st.car.weapon != c.weapon ) add( fmt( "wpn %u->%u(%s)", st.car.weapon, c.weapon, weapon_name( c.weapon ) ) );
			if ( st.car.health != c.health ) add( fmt( "hp %u->%u", st.car.health, c.health ) );
		}
		st.haveCar = true;
		st.car = c;
		if ( ch.empty() ) {
			if ( !opt_.everything ) return;
			ch = "(no change)";
		}
		emit( nowMs, tag, "car " + ch + " | keys=" + key_names( c.keys, false ) +
		      fmt( " ud=%d lr=%d wpn=%u(%s) hp=%u ar=%u carhp=%.0f pos=", c.ud, c.lr, c.weapon, weapon_name( c.weapon ),
		           c.health, c.armor, c.carHp ) + xyz( c.pos ) +
		      ( s.sTrailerID && s.sTrailerID != 0xFFFF ? " towing " + vehicle_name( s.sTrailerID ) : std::string() ) + rawBytes, out );
		return; }

	case 211: { // passenger
		++sent_[pre + "passenger"];
		samp::stPassengerData s;
		if ( n < sizeof s ) { skip( "passenger (cut off)" ); return; }
		std::memcpy( &s, d, sizeof s );
		Passenger q;
		q.vehicle = s.sVehicleID;
		q.seat = s.byteSeatID & 0x3F;   // seat in the low 6 bits,
		q.driveBy = s.byteSeatID >> 6;  // the drive-by flags in the top 2
		q.weapon = s.byteCurrentWeapon & 0x3F;
		q.keys = s.sKeys | extra_keys( s.byteCurrentWeapon );
		q.ud = (int16_t) s.sUpDownKeys;
		q.lr = (int16_t) s.sLeftRightKeys;
		std::memcpy( q.pos, s.fPosition, sizeof q.pos );
		if ( !injected ) { // the game's own position + keys ("near me", the REC pill)
			std::memcpy( myPos_, q.pos, sizeof myPos_ );
			havePos_ = true;
			lastKeys_ = q.keys;
			lastKeysOnFoot_ = true;
		}
		switch_kind( st, 3, nowMs );
		std::string ch;
		if ( !st.havePassenger || st.passenger.vehicle != q.vehicle || st.passenger.seat != q.seat ) {
			ch = '(' + vehicle_name( q.vehicle ) + fmt( " seat %u)", q.seat );
			for ( int i = 0; i < 19; ++i ) st.pressedAt[i] = nowMs - t0_;
		} else {
			ch = key_changes( st.pressedAt, st.passenger.keys, q.keys, nowMs, true );
			auto add = [&]( const std::string &s2 ) { if ( !ch.empty() ) ch += ", "; ch += s2; };
			if ( st.passenger.weapon != q.weapon ) add( fmt( "wpn %u->%u(%s)", st.passenger.weapon, q.weapon, weapon_name( q.weapon ) ) );
			if ( st.passenger.driveBy != q.driveBy ) add( fmt( "driveby %u->%u", st.passenger.driveBy, q.driveBy ) );
		}
		st.havePassenger = true;
		st.passenger = q;
		if ( ch.empty() ) {
			if ( !opt_.everything ) return;
			ch = "(no change)";
		}
		emit( nowMs, tag, "passenger " + ch + " | keys=" + key_names( q.keys, true ) +
		      fmt( " wpn=%u(%s) pos=", q.weapon, weapon_name( q.weapon ) ) + xyz( q.pos ) + rawBytes, out );
		return; }

	case 206: { // bullet
		++sent_[pre + "bullet"];
		samp::stBulletData s;
		if ( n < sizeof s ) { skip( "bullet (cut off)" ); return; }
		std::memcpy( &s, d, sizeof s );
		static const char *kHit[] = { "nothing", "player", "vehicle", "object", "player object" };
		emit( nowMs, tag, fmt( "bullet hit=%u(%s) target=%u", s.byteType, s.byteType < 5 ? kHit[s.byteType] : "?", s.sTargetID ) +
		      target_suffix( s.byteType, s.sTargetID ) + fmt( " wpn=%u(%s) at ", s.byteWeaponID, weapon_name( s.byteWeaponID ) ) +
		      xyz( s.fTarget ) + rawBytes, out );
		return; }

	case 205: { // money + drunk level, sent every few seconds: only real changes
		++sent_[pre + "stats"];
		if ( n < 8 ) { skip( "stats (cut off)" ); return; }
		int32_t money = 0, drunk = 0;
		std::memcpy( &money, d, 4 );
		std::memcpy( &drunk, d + 4, 4 );
		// The drunk level wears off by itself every frame: only money changes and
		// getting (more) drunk are written.
		const bool first = !haveStats_;
		const bool changed = first || money != lastMoney_ || drunk > lastDrunk_;
		haveStats_ = true;
		lastMoney_ = money;
		lastDrunk_ = drunk;
		if ( changed || opt_.everything )
			emit( nowMs, tag, fmt( "stats money=%d drunk=%d", money, drunk ) + ( changed ? "" : " (no change)" ) + rawBytes, out );
		return; }

	case 204: { // the player / actor we aim at + the weapon slots that changed: (slot, weapon, ammo) each
		++sent_[pre + "weapons"];
		std::vector<uint8_t> v( d, d + n );
		const bool same = v == lastWeapons_;
		if ( same && !opt_.everything ) return;
		lastWeapons_ = v;
		BitReader r( d, n * 8 );
		uint16_t target = 0xFFFF, actor = 0xFFFF;
		std::string line = "weapons";
		if ( r.get( target ) && r.get( actor ) ) {
			line += " target=" + ( target == 0xFFFF ? std::string( "none" ) : who( target ) );
			if ( actor != 0xFFFF ) line += fmt( " actor=#%u", actor );
			std::string slots;
			uint8_t slot = 0, w = 0;
			uint16_t ammo = 0;
			while ( r.get( slot ) && r.get( w ) && r.get( ammo ) ) {
				if ( !slots.empty() ) slots += ", ";
				slots += fmt( "slot %u: %u(%s) x%u", slot, w, weapon_name( w ), ammo );
			}
			line += " [" + slots + ']';
		} else {
			line += ' ' + hex( d, n, 64 );
		}
		emit( nowMs, tag, line + ( same ? " (no change)" : "" ) + rawBytes, out );
		return; }

	case 201: { // RCON: the command as typed (hidden in a log to share)
		++sent_[pre + "rcon"];
		if ( opt_.hideSecrets ) { emit( nowMs, tag, "RCON command (hidden)", out ); return; }
		BitReader r( d, n * 8 );
		std::string cmd;
		if ( r.str32( cmd ) ) emit( nowMs, tag, "RCON command " + quote( cmd ) + rawBytes, out );
		else emit( nowMs, tag, "RCON command (cut off?) " + hex( d, n, 64 ), out );
		return; }

	case 209: { // an empty vehicle we sync (pushed, or a cheat's)
		++sent_[pre + "unoccupied"];
		samp::stUnoccupiedData s;
		if ( !opt_.everything ) return;
		if ( n < sizeof s ) { emit( nowMs, tag, "unoccupied (cut off)" + rawBytes, out ); return; }
		std::memcpy( &s, d, sizeof s );
		emit( nowMs, tag, "unoccupied " + vehicle_name( (uint16_t) s.sVehicleID ) +
		      fmt( " seat=%u hp=%.0f pos=", s.byteSeatID, s.fVehicleHealth ) + xyz( s.fPosition ) + rawBytes, out );
		return; }
	case 210: { // the trailer we tow
		++sent_[pre + "trailer"];
		samp::stTrailerData s;
		if ( !opt_.everything ) return;
		if ( n < sizeof s ) { emit( nowMs, tag, "trailer (cut off)" + rawBytes, out ); return; }
		std::memcpy( &s, d, sizeof s );
		emit( nowMs, tag, "trailer " + vehicle_name( s.sTrailerID ) + " pos=" + xyz( s.fPosition ) + rawBytes, out );
		return; }
	case 212: { // spectating
		++sent_[pre + "spectator"];
		samp::stSpectatorData s;
		if ( !opt_.everything ) return;
		if ( n < sizeof s ) { emit( nowMs, tag, "spectator (cut off)" + rawBytes, out ); return; }
		std::memcpy( &s, d, sizeof s );
		emit( nowMs, tag, "spectator keys=" + key_names( s.wKeys, true ) + " pos=" + xyz( s.fPosition ) + rawBytes, out );
		return; }
	default: // not a sync: rare (the connection's own packets) -- always written
		++sent_[pre + fmt( "packet %u", id )];
		emit( nowMs, tag, fmt( "%s(%u) %uB", in_packet_name( id ), id, (unsigned) n + 1 ) +
		      ( opt_.everything ? rawBytes : n ? ' ' + hex( d, n, 48 ) : std::string() ), out );
		return;
	}
}

// --- incoming packets (not RPCs) -------------------------------------------

void Timeline::incoming_packet( uint32_t nowMs, const uint8_t *p, size_t bits, std::vector<std::string> &out ) {
	if ( !p || bits < 8 ) return;
	const uint8_t id = p[0];
	const size_t bytes = ( bits + 7 ) / 8;
	switch ( id ) {
	case 200: case 203: case 206: case 207: case 209: case 210: case 211: case 212: { // another player's sync
		if ( !opt_.others && !opt_.everything ) { skip( std::string( in_packet_name( id ) ) + " (others)" ); return; }
		std::string line;
		if ( other_sync( nowMs, p, bits, line ) ) emit( nowMs, "IN  ", line + raw( p, bits ), out );
		return; }
	case 208: // every player's radar position, about once a second
		if ( !opt_.everything ) { skip( "MARKERS_SYNC" ); return; }
		emit( nowMs, "IN  ", fmt( "MARKERS_SYNC %uB", (unsigned) bytes ) + raw( p, bits ), out );
		return;
	case 202: { // the server's answer to an RCON command (it can hold the RCON password: hidden in a log to share)
		if ( opt_.hideSecrets ) { emit( nowMs, "IN  ", "RCON_RESPONSE (hidden)", out ); return; }
		BitReader r( p + 1, bits - 8 );
		std::string t;
		if ( r.str32( t ) ) emit( nowMs, "IN  ", "RCON_RESPONSE " + quote( t ) + raw( p, bits ), out );
		else emit( nowMs, "IN  ", "RCON_RESPONSE (cut off?) " + hex( p + 1, bytes - 1, 64 ), out );
		return; }
	case 12: { // the server's auth challenge
		BitReader r( p + 1, bits - 8 );
		std::string key;
		if ( r.str8( key ) ) emit( nowMs, "IN  ", "AUTH_KEY " + quote( key ) + raw( p, bits ), out );
		else emit( nowMs, "IN  ", "AUTH_KEY (cut off?) " + hex( p + 1, bytes - 1, 64 ), out );
		return; }
	default: // connection events (accepted, lost, banned, full, wrong password, ...): always written
		emit( nowMs, "IN  ", fmt( "%s(%u) %uB", in_packet_name( id ), id, (unsigned) bytes ) +
		      ( opt_.everything ? raw( p, bits ) : bytes > 1 ? ' ' + hex( p + 1, bytes - 1, 48 ) : std::string() ), out );
		return;
	}
}

// A player's sync as the server relays it: the player's id first, then (on foot and
// driving) the server's own compact layout, else the client's struct as it was sent.
bool Timeline::other_sync( uint32_t nowMs, const uint8_t *p, size_t bits, std::string &line ) {
	BitReader r( p, bits );
	uint8_t id = 0;
	uint16_t pid = 0;
	if ( !r.get( id ) || !r.get( pid ) ) { line = std::string( in_packet_name( p[0] ) ) + " (cut off?)"; return true; }
	if ( others_.size() > 2048 ) others_.clear();
	Other &o = others_[pid];
	const uint32_t t = nowMs - t0_;
	const std::string name = "sync " + who( pid );
	std::string kind, ch, body;
	auto add = [&]( const std::string &s ) { if ( !ch.empty() ) ch += ", "; ch += s; };
	auto start = [&]( const std::string &what ) { ch = what; for ( auto &x : o.pressedAt ) x = t; };
	bool sg[4] = {};
	uint16_t quat[3] = {};
	auto rotation = [&]() { return r.bit( sg[0] ) && r.bit( sg[1] ) && r.bit( sg[2] ) && r.bit( sg[3] ) && r.get( quat ); };

	switch ( id ) {
	case 207: { // on foot: [lr] [ud] keys, position, rotation, health/armour, weapon, special action, ...
		bool hasLR = false, hasUD = false;
		uint16_t lr = 0, ud = 0, keys = 0;
		uint8_t hpar = 0, wb = 0, act = 0;
		float pos[3] = {};
		const bool ok = r.bit( hasLR ) && ( !hasLR || r.get( lr ) ) && r.bit( hasUD ) && ( !hasUD || r.get( ud ) ) &&
		                r.get( keys ) && r.get( pos ) && rotation() && r.get( hpar ) && r.get( wb ) && r.get( act );
		if ( !ok ) { line = name + " foot (cut off?)"; return true; }
		const uint32_t k = keys | extra_keys( wb );
		const uint8_t w = wb & 0x3F;
		if ( o.kind != 1 ) {
			start( "(start)" );
		} else {
			ch = key_changes( o.pressedAt, o.keys, k, nowMs, true );
			if ( o.ud != (int16_t) ud ) add( "ud " + axis( o.ud, 'W', 'S' ) + "->" + axis( (int16_t) ud, 'W', 'S' ) );
			if ( o.lr != (int16_t) lr ) add( "lr " + axis( o.lr, 'A', 'D' ) + "->" + axis( (int16_t) lr, 'A', 'D' ) );
			if ( o.weapon != w ) add( fmt( "wpn %u->%u(%s)", o.weapon, w, weapon_name( w ) ) );
			if ( o.action != act ) add( fmt( "act %u->%u(%s)", o.action, act, special_action( act ) ) );
			if ( o.hpar != hpar )
				add( fmt( "hp~%u->%u ar~%u->%u", hp_nibble( o.hpar >> 4 ), hp_nibble( hpar >> 4 ), hp_nibble( o.hpar & 15 ), hp_nibble( hpar & 15 ) ) );
		}
		o.kind = 1; o.keys = k; o.ud = (int16_t) ud; o.lr = (int16_t) lr; o.weapon = w; o.action = act; o.hpar = hpar;
		kind = "foot";
		body = " | keys=" + key_names( k, true ) + " ud=" + axis( o.ud, 'W', 'S' ) + " lr=" + axis( o.lr, 'A', 'D' ) +
		       fmt( " wpn=%u(%s) act=%u(%s) hp~%u ar~%u pos=", w, weapon_name( w ), act, special_action( act ),
		            hp_nibble( hpar >> 4 ), hp_nibble( hpar & 15 ) ) + xyz( pos );
		break; }
	case 200: { // driving: vehicle, lr, ud, keys, rotation, position, speed, car health, health/armour, weapon, ...
		uint16_t vid = 0, lr = 0, ud = 0, keys = 0;
		float pos[3] = {};
		const bool ok = r.get( vid ) && r.get( lr ) && r.get( ud ) && r.get( keys ) && rotation() && r.get( pos );
		if ( !ok ) { line = name + " car (cut off?)"; return true; }
		// The speed is a "compressed vector": its length, then (unless 0) a u16 per axis.
		float speed = 0;
		uint16_t axes[3] = {}, carHp = 0;
		uint8_t hpar = 0, wb = 0;
		const bool tail = r.get( speed ) && ( speed == 0.0f || r.get( axes ) ) && r.get( carHp ) && r.get( hpar ) && r.get( wb );
		const uint32_t k = keys | ( tail ? extra_keys( wb ) : 0 );
		const uint8_t w = tail ? (uint8_t) ( wb & 0x3F ) : o.weapon;
		if ( o.kind != 2 || o.vehicle != vid ) {
			start( "(in " + vehicle_name( vid ) + ')' );
		} else {
			ch = key_changes( o.pressedAt, o.keys, k, nowMs, false );
			if ( o.ud != (int16_t) ud ) add( fmt( "ud %d->%d", o.ud, (int16_t) ud ) );
			if ( o.lr != (int16_t) lr ) add( fmt( "lr %d->%d", o.lr, (int16_t) lr ) );
			if ( o.weapon != w ) add( fmt( "wpn %u->%u(%s)", o.weapon, w, weapon_name( w ) ) );
		}
		o.kind = 2; o.vehicle = vid; o.keys = k; o.ud = (int16_t) ud; o.lr = (int16_t) lr; o.weapon = w;
		kind = "car";
		body = " | keys=" + key_names( k, false ) + fmt( " ud=%d lr=%d", o.ud, o.lr ) +
		       ( tail ? fmt( " wpn=%u(%s) carhp=%u", w, weapon_name( w ), carHp ) : std::string() ) + " pos=" + xyz( pos );
		break; }
	case 211: { // passenger
		samp::stPassengerData s;
		if ( !r.bytes( &s, sizeof s ) ) { line = name + " passenger (cut off?)"; return true; }
		const uint8_t seat = s.byteSeatID & 0x3F, w = s.byteCurrentWeapon & 0x3F;
		const uint32_t k = s.sKeys | extra_keys( s.byteCurrentWeapon );
		if ( o.kind != 3 || o.vehicle != s.sVehicleID || o.seat != seat ) {
			start( '(' + vehicle_name( s.sVehicleID ) + fmt( " seat %u)", seat ) );
		} else {
			ch = key_changes( o.pressedAt, o.keys, k, nowMs, true );
			if ( o.weapon != w ) add( fmt( "wpn %u->%u(%s)", o.weapon, w, weapon_name( w ) ) );
		}
		o.kind = 3; o.vehicle = s.sVehicleID; o.seat = seat; o.keys = k; o.weapon = w;
		kind = "passenger";
		body = " | keys=" + key_names( k, true ) + fmt( " wpn=%u(%s) pos=", w, weapon_name( w ) ) + xyz( s.fPosition );
		break; }
	case 203: { // aim: written when the camera mode / weapon state changes
		samp::stAimData s;
		if ( !r.bytes( &s, sizeof s ) ) { line = name + " aim (cut off?)"; return true; }
		const bool changed = !o.haveAim || s.byteCamMode != o.cam || s.byteWeaponState != o.wstate;
		o.haveAim = true; o.cam = s.byteCamMode; o.wstate = s.byteWeaponState;
		if ( !changed && !opt_.everything ) return false;
		line = name + fmt( " aim cam=%u(%s) wstate=%u(%s)", s.byteCamMode, cam_mode( s.byteCamMode ), (unsigned) s.byteWeaponState,
		                   weapon_state( s.byteWeaponState ) ) + ( changed ? "" : " (no change)" );
		return true; }
	case 206: { // a shot: every one
		samp::stBulletData s;
		if ( !r.bytes( &s, sizeof s ) ) { line = name + " bullet (cut off?)"; return true; }
		static const char *kHit[] = { "nothing", "player", "vehicle", "object", "player object" };
		line = name + fmt( " bullet hit=%u(%s) target=%u", s.byteType, s.byteType < 5 ? kHit[s.byteType] : "?", s.sTargetID ) +
		       target_suffix( s.byteType, s.sTargetID ) + fmt( " wpn=%u(%s) at ", s.byteWeaponID, weapon_name( s.byteWeaponID ) ) + xyz( s.fTarget );
		return true; }
	case 209: { // an empty vehicle they sync: written when it's another one
		samp::stUnoccupiedData s;
		if ( !r.bytes( &s, sizeof s ) ) { line = name + " unoccupied (cut off?)"; return true; }
		const bool changed = o.unoccupied != (uint16_t) s.sVehicleID;
		o.unoccupied = (uint16_t) s.sVehicleID;
		if ( !changed && !opt_.everything ) return false;
		line = name + " unoccupied " + vehicle_name( (uint16_t) s.sVehicleID ) + fmt( " hp=%.0f pos=", s.fVehicleHealth ) +
		       xyz( s.fPosition ) + ( changed ? "" : " (no change)" );
		return true; }
	case 210: { // the trailer they tow: written when it's another one
		samp::stTrailerData s;
		if ( !r.bytes( &s, sizeof s ) ) { line = name + " trailer (cut off?)"; return true; }
		const bool changed = o.trailer != s.sTrailerID;
		o.trailer = s.sTrailerID;
		if ( !changed && !opt_.everything ) return false;
		line = name + " trailer " + vehicle_name( s.sTrailerID ) + " pos=" + xyz( s.fPosition ) + ( changed ? "" : " (no change)" );
		return true; }
	default:
		line = name + ' ' + in_packet_name( id ) + ' ' + bytes_tail( p, bits );
		return true;
	}
	if ( ch.empty() ) {
		if ( !opt_.everything ) return false; // only moved
		ch = "(no change)";
	}
	line = name + ' ' + kind + ' ' + ch + body;
	return true;
}

// --- the chat box, real input, our state ---------------------------------------

void Timeline::chat_line( uint32_t nowMs, int type, const std::string &text, std::vector<std::string> &out ) {
	const char *kind = type == 2 ? "chat" : type == 4 ? "info" : type == 8 ? "debug" : nullptr;
	emit( nowMs, "CHATBOX ", ( kind ? std::string( kind ) : fmt( "type %d", type ) ) + ' ' + quote( text ), out );
}

void Timeline::input( uint32_t nowMs, unsigned vk, bool down, std::vector<std::string> &out ) {
	const uint32_t t = nowMs - t0_;
	const auto it = keyDownAt_.find( vk );
	if ( down ) {
		if ( it != keyDownAt_.end() ) return; // a held key's auto-repeat
		if ( keyDownAt_.size() > 256 ) keyDownAt_.clear();
		keyDownAt_[vk] = t;
		emit( nowMs, "KEY  ", vk_name( vk ) + " down", out );
		return;
	}
	if ( it == keyDownAt_.end() ) return; // its press wasn't written
	const uint32_t held = t - it->second;
	keyDownAt_.erase( it );
	emit( nowMs, "KEY  ", vk_name( vk ) + fmt( " up (held %ums)", held ), out );
}

void Timeline::wheel( uint32_t nowMs, int notches, std::vector<std::string> &out ) {
	if ( notches == 0 ) return;
	emit( nowMs, "KEY  ", fmt( "WHEEL %s x%d", notches > 0 ? "up" : "down", notches > 0 ? notches : -notches ), out );
}

void Timeline::focus( uint32_t nowMs, bool active, std::vector<std::string> &out ) {
	if ( active == focused_ ) return;
	focused_ = active;
	if ( active ) { emit( nowMs, "FOCUS ", "back in the game", out ); return; }
	std::string held;
	for ( const auto &kv : keyDownAt_ ) {
		if ( !held.empty() ) held += ", ";
		held += vk_name( kv.first );
	}
	keyDownAt_.clear();
	emit( nowMs, "FOCUS ", "lost (alt-tab / minimized)" + ( held.empty() ? std::string() : " -- released " + held ), out );
}

std::string Timeline::full_state( const LocalState &s ) const {
	std::string l = fmt( "%s id=%d ", game_state_name( s.gameState ), s.id ) + life( s ) + " pos=" + xyz( s.pos ) +
	                fmt( " int=%d hp=%.0f ar=%.0f money=%d wpn=%d(%s) weapons=", s.interior, s.health, s.armor, s.money,
	                     s.weapon, weapon_name( (unsigned) s.weapon ) ) + weapons_list( s, true ) + ' ' + vehicle_of( s );
	if ( s.vehicle >= 0 ) l += fmt( " carhp=%.0f", s.vehicleHealth );
	l += fmt( " skin=%d ping=%d", s.skin, s.ping );
	if ( s.chatInput ) l += " | typing in chat";
	if ( s.dialogOpen ) l += " | dialog open";
	if ( s.gtaMenu ) l += " | GTA menu open";
	if ( s.fuegoMenu ) l += " | menu open";
	return l;
}

void Timeline::state( uint32_t nowMs, const LocalState &s, std::vector<std::string> &out ) {
	const uint32_t sinceLast = nowMs - lastStateAt_;
	lastStateAt_ = nowMs;
	if ( !haveState_ || nowMs - lastFullStateAt_ >= kStateEveryMs ) {
		emit( nowMs, "STATE ", full_state( s ), out );
		haveState_ = true;
		lastState_ = s;
		lastFullStateAt_ = nowMs;
		return;
	}
	const LocalState &o = lastState_;
	std::string ch;
	auto add = [&]( const std::string &x ) { if ( !ch.empty() ) ch += ", "; ch += x; };
	auto flag = [&]( bool was, bool is, const char *on, const char *off ) { if ( was != is ) add( is ? on : off ); };

	if ( o.gameState != s.gameState ) add( std::string( "game " ) + game_state_name( o.gameState ) + "->" + game_state_name( s.gameState ) );
	if ( o.id != s.id ) add( fmt( "id %d->%d", o.id, s.id ) );
	if ( life( o ) != life( s ) ) add( life( o ) + "->" + life( s ) );
	const float dx = s.pos[0] - o.pos[0], dy = s.pos[1] - o.pos[1], dz = s.pos[2] - o.pos[2];
	const float jump = std::sqrt( dx * dx + dy * dy + dz * dz );
	if ( jump > kTeleportMeters + kFastestMps * ( sinceLast / 1000.0f ) )
		add( "teleported " + xyz( o.pos ) + "->" + xyz( s.pos ) + fmt( " %.0fm", jump ) );
	if ( o.interior != s.interior ) add( fmt( "interior %d->%d", o.interior, s.interior ) );
	if ( std::lround( o.health ) != std::lround( s.health ) ) add( fmt( "hp %.0f->%.0f", o.health, s.health ) );
	if ( std::lround( o.armor ) != std::lround( s.armor ) ) add( fmt( "ar %.0f->%.0f", o.armor, s.armor ) );
	if ( o.money != s.money ) add( fmt( "money %d->%d (%+d)", o.money, s.money, s.money - o.money ) );
	if ( o.weapon != s.weapon ) add( fmt( "wpn %d->%d(%s)", o.weapon, s.weapon, weapon_name( (unsigned) s.weapon ) ) );
	if ( std::memcmp( o.weapons, s.weapons, sizeof o.weapons ) != 0 ) add( "weapons " + weapons_list( o, false ) + "->" + weapons_list( s, false ) );
	const bool sameCar = o.vehicle == s.vehicle;
	if ( !sameCar || o.seat != s.seat ) add( vehicle_of( o ) + "->" + vehicle_of( s ) );
	// A car's health: big steps only (a burning car loses some every frame).
	float carHp = s.vehicleHealth;
	if ( sameCar && s.vehicle >= 0 ) {
		if ( std::fabs( s.vehicleHealth - o.vehicleHealth ) >= 50.0f ) add( fmt( "carhp %.0f->%.0f", o.vehicleHealth, s.vehicleHealth ) );
		else carHp = o.vehicleHealth;
	}
	if ( o.skin != s.skin ) add( fmt( "skin %d->%d", o.skin, s.skin ) );
	flag( o.chatInput, s.chatInput, "typing in chat", "chat input closed" );
	flag( o.dialogOpen, s.dialogOpen, "dialog open", "dialog closed" );
	flag( o.gtaMenu, s.gtaMenu, "GTA menu open", "GTA menu closed" );
	flag( o.fuegoMenu, s.fuegoMenu, "menu open", "menu closed" );

	lastState_ = s; // the position too, every frame: only a jump counts, never the sum of steps
	lastState_.vehicleHealth = carHp;
	if ( !ch.empty() ) emit( nowMs, "STATE ", ch, out );
}

void Timeline::roster( uint32_t nowMs, const std::vector<std::pair<int, std::string>> &players, std::vector<std::string> &out ) {
	constexpr size_t kPerLine = 40;
	const size_t lines = players.empty() ? 1 : ( players.size() + kPerLine - 1 ) / kPerLine;
	for ( size_t part = 0; part < lines; ++part ) {
		std::string s = fmt( "PLAYERS %u", (unsigned) players.size() ) +
		                ( lines > 1 ? fmt( " (%u/%u)", (unsigned) part + 1, (unsigned) lines ) : std::string() ) + ':';
		for ( size_t i = part * kPerLine; i < players.size() && i < ( part + 1 ) * kPerLine; ++i ) {
			const int pid = players[i].first;
			if ( pid >= 0 && pid < 0xFFFF && !players[i].second.empty() ) names_[(uint16_t) pid] = players[i].second;
			s += ( i == part * kPerLine ? " " : ", " ) + fmt( "%d ", pid ) + clean( players[i].second ) +
			     ( pid >= 0 && mine( (uint16_t) pid ) ? " (me)" : "" );
		}
		emit( nowMs, "", s, out );
	}
}

} // namespace rec
