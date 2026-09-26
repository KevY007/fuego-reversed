#include "visuals.h"

#include "../core/settings.h"
#include "../core/trace.h"
#include "../core/input.h"
#include "../core/memory.h"
#include "../samp/samp.h"
#include "../samp/pools.h"
#include "../samp/samp_sdk.h"   // server_presets, scoreboard, object pool
#include "../gta/gtahost.h"
#include "../render/render.h"   // overlay_hidden (F8 screenshot hide)
#include "friends.h"
#include "status.h"
#include "cheats.h"

#include "imgui.h"

#include <windows.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/*
	Everything here mirrors what Project Fuego draws (proxyIDirect3DDevice9.cpp +
	cheat_samp.cpp), with its shipped config (bin/project_fuego/fuego.ini): same fonts,
	colours, offsets, conditions and gating. Fuego's draw calls are CD3DFont (GDI-baked,
	outlined glyphs) and CD3DRender (1px D3D lines/quads); the helpers below reproduce
	those on ImGui's background draw list. Reversed-only extras (box ESP, streamed-out
	markers, the watermark, the F8 hide) are kept and marked as such.
*/

namespace {

using visuals::Font;

constexpr int kMaxPlayers = samp::SAMP_MAX_PLAYERS;
constexpr int kMaxPoolPeds = 256; // GTA's ped pool holds 140

// ---- colours ------------------------------------------------------------------
constexpr uint32_t ARGB( uint32_t a, uint32_t r, uint32_t g, uint32_t b ) { return ( a << 24 ) | ( r << 16 ) | ( g << 8 ) | b; }
constexpr uint32_t XRGB( uint32_t r, uint32_t g, uint32_t b ) { return ARGB( 0xFF, r, g, b ); }
constexpr uint32_t RGBA( uint32_t r, uint32_t g, uint32_t b, uint32_t a ) { return ARGB( a, r, g, b ); } // D3DCOLOR_RGBA
inline ImU32 im( uint32_t argb ) {
	return IM_COL32( ( argb >> 16 ) & 0xFF, ( argb >> 8 ) & 0xFF, argb & 0xFF, ( argb >> 24 ) & 0xFF );
}

// Fuego samp_color_get(id, trans): SA:MP's per-player colour (0xRRGGBBAA) as ARGB.
uint32_t samp_color( int id, uint32_t trans = 0xFF000000 ) {
	if ( id < 0 || id >= kMaxPlayers + 3 ) return XRGB( 0x99, 0x99, 0x99 );
	if ( id == kMaxPlayers )     return 0xFF888888;
	if ( id == kMaxPlayers + 1 ) return 0xFF0000AA;
	if ( id == kMaxPlayers + 2 ) return 0xFF63C0E2;
	return ( samp::player_color( id ) >> 8 ) | trans;
}

// ---- fonts ------------------------------------------------------------------
// Fuego's CD3DFont set (proxyIDirect3DDevice9.cpp:63-79). CD3DFont takes an int point
// size (8.5 -> 8, 6.5 -> 6), rasterises it with GDI at -pt*96/72 px (the em height) and
// bakes a 1px black border into every glyph (FCR_BORDER). "Samsung Sans" is the optional
// fuego_font.ttf; "Small Fonts" is a GDI raster font ImGui can't load (-> Tahoma).
struct FontDef { bool smallFace; int pt; };
constexpr FontDef kFontDefs[] = {
	{ false, 10 }, // Main
	{ false, 8 },  // SampStuff
	{ false, 9 },  // Fixed
	{ true,  6 },  // FixedSmall
	{ false, 11 }, // Chat
};
ImFont *g_sans   = nullptr;
ImFont *g_small  = nullptr;
float   g_sansK  = 1.0f; // ImGui pixel size per GDI em pixel: (hhea asc - desc) / unitsPerEm
float   g_smallK = 1.0f;

const FontDef &def( Font f ) { return kFontDefs[(int) f]; }
ImFont *face( Font f ) {
	ImFont *ff = def( f ).smallFace ? g_small : g_sans;
	return ff ? ff : ImGui::GetFont();
}
float font_px( Font f ) {
	const FontDef &d = def( f );
	const float em = (float) ( d.pt * 96 / 72 ); // CD3DFont: -pt * LOGPIXELSY / 72 (int)
	if ( d.smallFace ) return em * ( g_small ? g_smallK : 1.0f );
	return em * ( g_sans ? g_sansK : 1.0f );
}

std::vector<uint8_t> read_file( const std::string &path ) {
	std::vector<uint8_t> out;
	FILE *f = fopen( path.c_str(), "rb" );
	if ( !f ) return out;
	fseek( f, 0, SEEK_END );
	long n = ftell( f );
	fseek( f, 0, SEEK_SET );
	if ( n > 0 && n < ( 64 << 20 ) ) {
		out.resize( (size_t) n );
		if ( fread( out.data(), 1, (size_t) n, f ) != (size_t) n ) out.clear();
	}
	fclose( f );
	return out;
}

// GDI's CreateFont(-h) matches the em height; ImGui sizes by (ascent - descent)
// (stbtt_ScaleForPixelHeight). Ratio from the font's own head/hhea tables.
float ttf_height_per_em( const std::vector<uint8_t> &f ) {
	auto u16 = [&]( size_t o ) -> uint32_t { return o + 2 <= f.size() ? ( (uint32_t) f[o] << 8 ) | f[o + 1] : 0; };
	auto u32 = [&]( size_t o ) -> uint32_t { return o + 4 <= f.size() ? ( u16( o ) << 16 ) | u16( o + 2 ) : 0; };
	if ( f.size() < 12 ) return 1.0f;
	size_t base = 0;
	if ( memcmp( f.data(), "ttcf", 4 ) == 0 ) base = u32( 12 ); // collection: first face
	const uint32_t numTables = u16( base + 4 );
	size_t head = 0, hhea = 0;
	for ( uint32_t i = 0; i < numTables; i++ ) {
		const size_t rec = base + 12 + 16 * (size_t) i;
		if ( rec + 16 > f.size() ) break;
		if ( !memcmp( &f[rec], "head", 4 ) ) head = u32( rec + 8 );
		if ( !memcmp( &f[rec], "hhea", 4 ) ) hhea = u32( rec + 8 );
	}
	if ( !head || !hhea ) return 1.0f;
	const int upem = (int) u16( head + 18 );
	const int asc  = (int16_t) u16( hhea + 4 );
	const int desc = (int16_t) u16( hhea + 6 );
	if ( upem <= 0 || asc - desc <= 0 ) return 1.0f;
	return (float) ( asc - desc ) / (float) upem;
}

bool file_exists( const std::string &p ) {
	DWORD a = GetFileAttributesA( p.c_str() );
	return a != INVALID_FILE_ATTRIBUTES && !( a & FILE_ATTRIBUTE_DIRECTORY );
}
std::string game_dir() {
	char p[MAX_PATH] = {};
	GetModuleFileNameA( nullptr, p, MAX_PATH );
	char *slash = strrchr( p, '\\' );
	if ( slash ) *slash = '\0';
	return p;
}
std::string fonts_dir() {
	char w[MAX_PATH] = {};
	GetWindowsDirectoryA( w, MAX_PATH );
	return std::string( w ) + "\\Fonts\\";
}

// The file behind an installed font face (per-user first, then machine-wide), the way
// GDI would resolve "Samsung Sans" for Fuego's CreateFont.
std::string installed_face( const char *prefix ) {
	const HKEY roots[] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE };
	for ( HKEY root : roots ) {
		HKEY k;
		if ( RegOpenKeyExA( root, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0, KEY_READ, &k ) != ERROR_SUCCESS )
			continue;
		std::string found;
		for ( DWORD i = 0; found.empty(); i++ ) {
			char name[256], data[MAX_PATH];
			DWORD nl = sizeof name, dl = sizeof data - 1, type = 0;
			LONG r = RegEnumValueA( k, i, name, &nl, nullptr, &type, (BYTE *) data, &dl );
			if ( r == ERROR_NO_MORE_ITEMS ) break;
			if ( r != ERROR_SUCCESS || type != REG_SZ ) continue;
			if ( _strnicmp( name, prefix, strlen( prefix ) ) != 0 ) continue;
			data[dl < sizeof data ? dl : sizeof data - 1] = '\0';
			std::string path = data;
			if ( path.find( ':' ) == std::string::npos ) path = fonts_dir() + path;
			if ( file_exists( path ) ) found = path;
		}
		RegCloseKey( k );
		if ( !found.empty() ) return found;
	}
	return {};
}

ImFont *load_font( const std::string &path, float &heightPerEm ) {
	std::vector<uint8_t> bytes = read_file( path );
	if ( bytes.empty() ) return nullptr;
	heightPerEm = ttf_height_per_em( bytes );
	ImFontConfig cfg;
	cfg.PixelSnapH = true; // GDI-like whole-pixel advances for the small overlay text
	return ImGui::GetIO().Fonts->AddFontFromFileTTF( path.c_str(), 13.0f, &cfg );
}

// ---- text (CD3DFont) ----------------------------------------------------------
// CD3DFont GetColorTag: "{RRGGBB}" -> opaque colour, "{AARRGGBB}" -> with alpha.
// Returns the tag length (8 / 10) or 0 if this '{' isn't a tag.
size_t color_tag( const char *s, size_t left, uint32_t &argb ) {
	if ( left < 8 || s[0] != '{' ) return 0;
	size_t digits = 0;
	if ( s[7] == '}' ) digits = 6;
	else if ( left >= 10 && s[9] == '}' ) digits = 8;
	else return 0;
	for ( size_t i = 1; i <= digits; i++ )
		if ( !isxdigit( (unsigned char) s[i] ) ) return 0;
	char hex[9] = {};
	memcpy( hex, s + 1, digits );
	argb = (uint32_t) strtoul( hex, nullptr, 16 );
	if ( digits == 6 ) argb |= 0xFF000000;
	return digits + 2;
}

// GDI rendered the ANSI bytes through the system code page; ImGui wants UTF-8.
std::string acp_to_utf8( const char *s, size_t n ) {
	bool high = false;
	for ( size_t i = 0; i < n; i++ ) if ( (unsigned char) s[i] >= 0x80 ) { high = true; break; }
	if ( !high ) return std::string( s, n );
	int wl = MultiByteToWideChar( CP_ACP, 0, s, (int) n, nullptr, 0 );
	std::wstring w( (size_t) wl, L'\0' );
	MultiByteToWideChar( CP_ACP, 0, s, (int) n, w.data(), wl );
	int ul = WideCharToMultiByte( CP_UTF8, 0, w.data(), wl, nullptr, 0, nullptr, nullptr );
	std::string u( (size_t) ul, '\0' );
	WideCharToMultiByte( CP_UTF8, 0, w.data(), wl, u.data(), ul, nullptr, nullptr );
	return u;
}

struct Segment { std::string text; uint32_t color; int glyphs; };

// Split into colour runs like CD3DFont::Print (skipTags: consume tags, keep the colour).
void segments( const char *text, uint32_t color, bool skipTags, std::vector<Segment> &out ) {
	out.clear();
	const size_t len = strlen( text );
	std::string run;
	int glyphs = 0;
	uint32_t cur = color;
	auto flush = [&]() {
		if ( !run.empty() ) out.push_back( { acp_to_utf8( run.data(), run.size() ), cur, glyphs } );
		run.clear();
		glyphs = 0;
	};
	for ( size_t i = 0; i < len; i++ ) {
		const unsigned char c = (unsigned char) text[i];
		if ( c < 32 ) continue; // CD3DFont renders bytes 32..255 only
		if ( c == '{' ) {
			uint32_t tagColor = 0;
			if ( size_t tl = color_tag( text + i, len - i, tagColor ) ) {
				flush();
				if ( !skipTags ) cur = tagColor;
				i += tl - 1;
				continue;
			}
		}
		run.push_back( (char) c );
		glyphs++;
	}
	flush();
}

// FCR_BORDER glyphs carry a 1px black border (in the colour's alpha) and their ink sits
// at (+1,+1) of the requested position.
void print_pass( ImDrawList *dl, Font f, float x, float y, uint32_t color, const char *text, bool skipTags ) {
	static std::vector<Segment> segs;
	segments( text, color, skipTags, segs );
	ImFont *ff = face( f );
	const float px = font_px( f );
	float pen = x;
	for ( const Segment &s : segs ) {
		const ImU32 border = im( s.color & 0xFF000000 );
		const ImU32 fill   = im( s.color );
		const float gx = pen + 1.0f, gy = y + 1.0f;
		static const float kOff[8][2] = { { -1, -1 }, { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 } };
		for ( const auto &o : kOff )
			dl->AddText( ff, px, ImVec2( gx + o[0], gy + o[1] ), border, s.text.c_str() );
		dl->AddText( ff, px, ImVec2( gx, gy ), fill, s.text.c_str() );
		pen += ff->CalcTextSizeA( px, FLT_MAX, 0.0f, s.text.c_str() ).x + (float) s.glyphs; // +1 px/glyph (border)
	}
}

ImDrawList *bg() { return ImGui::GetBackgroundDrawList(); }

// ---- primitives (CD3DRender) --------------------------------------------------
void d3d_line( float x1, float y1, float x2, float y2, uint32_t c ) {
	bg()->AddLine( ImVec2( x1 + 0.5f, y1 + 0.5f ), ImVec2( x2 + 0.5f, y2 + 0.5f ), im( c ), 1.0f );
}
// D3DLineOutlined(int x, int y, int x2, int y2, float w, ...): the int params truncate
// x +/- w, so the outline lines land at (+trunc) and (-ceil) of the width.
void d3d_line_outlined( int x, int y, int x2, int y2, float w, uint32_t c, uint32_t o ) {
	d3d_line( (float) (int) ( x + w ), (float) (int) ( y + w ), (float) (int) ( x2 + w ), (float) (int) ( y2 + w ), o );
	d3d_line( (float) (int) ( x - w ), (float) (int) ( y - w ), (float) (int) ( x2 - w ), (float) (int) ( y2 - w ), o );
	d3d_line( (float) x, (float) y, (float) x2, (float) y2, c );
}
void d3d_box( float x, float y, float w, float h, uint32_t c ) {
	if ( w <= 0.0f || h <= 0.0f ) return;
	bg()->AddRectFilled( ImVec2( x, y ), ImVec2( x + w, y + h ), im( c ) );
}
void d3d_box_border( float x, float y, float w, float h, uint32_t border, uint32_t fill ) {
	d3d_box( x, y, w - 1.0f, 1.0f, border );
	d3d_box( x + w - 1.0f, y, 1.0f, h - 1.0f, border );
	d3d_box( x + 1.0f, y + h - 1.0f, w - 1.0f, 1.0f, border );
	d3d_box( x, y + 1.0f, 1.0f, h - 1.0f, border );
	d3d_box( x + 1.0f, y + 1.0f, w - 2.0f, h - 2.0f, fill );
}
void circle( int x, int y, int r, int sides, uint32_t c ) {
	if ( r <= 0 ) return;
	bg()->AddCircle( ImVec2( x + 0.5f, y + 0.5f ), (float) r, im( c ), sides, 1.0f );
}
// CircleOutlined(int X, int Y, int radius, ...): radius +/- outline truncates to int.
void circle_outlined( int x, int y, int r, int sides, float w, uint32_t c, uint32_t o ) {
	circle( x, y, (int) ( r + w ), sides, o );
	circle( x, y, (int) ( r - w ), sides, o );
	circle( x, y, r, sides, c );
}

// ---- projection (Fuego CalcScreenCoors) ------------------------------------------
bool calc_screen_z( const float w[3], float &sx, float &sy, float &sz ) {
	const float *m = reinterpret_cast<const float *>( 0xB6FA2C ); // D3DXMATRIX, row-major
	const float x = w[2] * m[8]  + w[1] * m[4] + w[0] * m[0] + m[12];
	const float y = w[2] * m[9]  + w[1] * m[5] + w[0] * m[1] + m[13];
	const float z = w[2] * m[10] + w[1] * m[6] + w[0] * m[2] + m[14];
	const double recip = 1.0 / z;
	sx = x * (float) ( recip * (double) mem::read<uint32_t>( 0xC17044 ) ); // RsGlobal.maximumWidth
	sy = y * (float) ( recip * (double) mem::read<uint32_t>( 0xC17048 ) ); // RsGlobal.maximumHeight
	sz = z;
	return z >= 1.0f;
}

float dist3( const float a[3], const float b[3] ) {
	const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return sqrtf( dx * dx + dy * dy + dz * dz );
}

// Real CWorld::GetIsLineOfSightClear @0x56A490 with Fuego's flags: buildings, objects,
// dummies; vehicles/peds ignored.
bool los_clear( const float from[3], const float to[3] ) {
	return mem::ccall<bool>( 0x56A490, from, to, true, false, false, true, true, false, false );
}

bool scoreboard_open() {
	samp::stScoreboardInfo *sb = samp::scoreboard_info();
	return sb && !IsBadReadPtr( sb, sizeof( int ) ) && sb->iIsEnabled;
}
bool f10_down() { return GetAsyncKeyState( VK_F10 ) < 0; } // Fuego: raw GetAsyncKeyState

// Fuego's "don't run during certain samp events" (scoreboard open / F10 held).
bool samp_event_hide() { return scoreboard_open() || f10_down(); }

// GTA ped pointer -> SA:MP player id for every streamed remote player (Fuego's
// translateGTASAMP_pedPool). Built once per frame.
struct PedMapEntry { void *ped; int id; };
PedMapEntry g_pedMap[kMaxPlayers];
int         g_pedMapCount = 0;

void build_ped_map( int me ) {
	g_pedMapCount = 0;
	for ( int id = 0; id < kMaxPlayers; id++ ) {
		if ( id == me ) continue;
		samp::stRemotePlayer *rp = samp::remote_player( id );
		if ( !rp || !rp->pPlayerData || IsBadReadPtr( rp->pPlayerData, sizeof( samp::stRemotePlayerData ) ) ) continue;
		samp::stSAMPPed *a = rp->pPlayerData->pSAMP_Actor;
		if ( !a || IsBadReadPtr( a, sizeof( samp::stSAMPPed ) ) || !a->pGTA_Ped ) continue;
		g_pedMap[g_pedMapCount++] = { a->pGTA_Ped, id };
	}
}
int samp_id_of_ped( void *ped ) {
	for ( int i = 0; i < g_pedMapCount; i++ )
		if ( g_pedMap[i].ped == ped ) return g_pedMap[i].id;
	return -1;
}

game::PoolPed g_peds[kMaxPoolPeds];
int           g_pedCount = 0;
int           g_selfPool = -1;

// ---- mod_sa weapon_list (cheat_funcs.cpp:27-70) -----------------------------------
const char *weapon_list_name( int id ) {
	switch ( id ) {
		case 0:  return "Fist";              case 1:  return "Brass Knuckles";   case 2:  return "Golf Club";
		case 3:  return "Nitestick";         case 4:  return "Knife";            case 5:  return "Baseball Bat";
		case 6:  return "Shovel";            case 7:  return "Pool Cue";         case 8:  return "Katana";
		case 9:  return "Chainsaw";          case 10: return "Dildo 1";          case 11: return "Dildo 2";
		case 12: return "Vibe 1";            case 13: return "Vibe 2";           case 14: return "Flowers";
		case 15: return "Cane";              case 16: return "Grenade";          case 17: return "Teargas";
		case 18: return "Molotov Cocktail";  case 22: return "Pistol";           case 23: return "Silenced Pistol";
		case 24: return "Desert Eagle";      case 25: return "Shotgun";          case 26: return "Sawn-Off Shotgun";
		case 27: return "SPAZ12";            case 28: return "Micro UZI";        case 29: return "MP5";
		case 30: return "AK47";              case 31: return "M4";               case 32: return "Tech9";
		case 33: return "Country Rifle";     case 34: return "Sniper Rifle";     case 35: return "Rocket Launcher";
		case 36: return "Heat Seeking RPG";  case 37: return "Flame Thrower";    case 38: return "Minigun";
		case 39: return "Remote Explosives"; case 40: return "Detonator";        case 41: return "Spray Can";
		case 42: return "Fire Extinguisher"; case 43: return "Camera";           case 44: return "NV Goggles";
		case 45: return "IR Goggles";        case 46: return "Parachute";
		default: return nullptr;
	}
}
// Dropped-weapon object models (the weapon_list entries with model 335..363).
const char *weapon_name_by_model( int m ) {
	switch ( m ) {
		case 335: return "Knife";            case 336: return "Baseball Bat";     case 337: return "Shovel";
		case 338: return "Pool Cue";         case 339: return "Katana";           case 341: return "Chainsaw";
		case 342: return "Grenade";          case 343: return "Teargas";          case 344: return "Molotov Cocktail";
		case 346: return "Pistol";           case 347: return "Silenced Pistol";  case 348: return "Desert Eagle";
		case 349: return "Shotgun";          case 350: return "Sawn-Off Shotgun"; case 351: return "SPAZ12";
		case 352: return "Micro UZI";        case 353: return "MP5";              case 355: return "AK47";
		case 356: return "M4";               case 357: return "Country Rifle";    case 358: return "Sniper Rifle";
		case 359: return "Rocket Launcher";  case 360: return "Heat Seeking RPG"; case 361: return "Flame Thrower";
		case 362: return "Minigun";          case 363: return "Remote Explosives";
		default:  return nullptr;
	}
}

// ---- reversed-only extras (ImGui default font) ---------------------------------
constexpr ImU32 kBox    = IM_COL32( 255, 80, 40, 235 );   // fuego orange-red
constexpr ImU32 kBoxBg  = IM_COL32( 0, 0, 0, 160 );
constexpr ImU32 kName   = IM_COL32( 255, 255, 255, 255 );
constexpr ImU32 kShadow = IM_COL32( 0, 0, 0, 220 );
constexpr ImU32 kHpGood = IM_COL32( 90, 220, 90, 255 );
constexpr ImU32 kHpLow  = IM_COL32( 230, 70, 70, 255 );

void text_shadowed( ImDrawList *dl, float x, float y, ImU32 col, const char *s, bool center ) {
	ImVec2 sz = ImGui::CalcTextSize( s );
	if ( center ) x -= sz.x * 0.5f;
	dl->AddText( ImVec2( x + 1, y + 1 ), kShadow, s );
	dl->AddText( ImVec2( x, y ), col, s );
}

// EXTRA: one player's ESP box with toggleable name / health / distance. The box spans
// ped centre +/-1.0 (head .. feet).
void draw_player( ImDrawList *dl, int id ) {
	auto &set = fx::g_set;
	float center[3];
	if ( !game::player_world_pos( id, center ) )
		return;
	float head[3] = { center[0], center[1], center[2] + 1.0f };
	float feet[3] = { center[0], center[1], center[2] - 1.0f };
	float hx, hy, fxp, fyp, z;
	if ( !calc_screen_z( head, hx, hy, z ) ) return;
	if ( !calc_screen_z( feet, fxp, fyp, z ) ) return;
	float top = hy < fyp ? hy : fyp;
	float bot = hy < fyp ? fyp : hy;
	float h   = bot - top;
	if ( h < 4.0f || h > 2000.0f )
		return;
	float w  = h * 0.42f;
	float cx = ( hx + fxp ) * 0.5f;
	if ( set.esp_box ) {
		dl->AddRect( ImVec2( cx - w * 0.5f, top ), ImVec2( cx + w * 0.5f, bot ), kBoxBg, 0, 0, 3.0f );
		dl->AddRect( ImVec2( cx - w * 0.5f, top ), ImVec2( cx + w * 0.5f, bot ), kBox, 0, 0, 1.2f );
	}
	if ( set.esp_name ) {
		std::string nm = samp::player_name( id );
		char line[96];
		_snprintf( line, sizeof( line ) - 1, "%s (%d)", nm.empty() ? "?" : nm.c_str(), id );
		line[sizeof( line ) - 1] = '\0';
		uint32_t mc = samp::player_color( id );
		ImU32 nc = mc ? IM_COL32( ( mc >> 24 ) & 0xFF, ( mc >> 16 ) & 0xFF, ( mc >> 8 ) & 0xFF, 0xDD ) : kName;
		text_shadowed( dl, cx, top - ImGui::GetTextLineHeight() - 2.0f, nc, line, true );
	}
	if ( set.esp_health ) {
		float hp = game::player_gta_health( id );
		hp = hp < 0 ? 0 : ( hp > 100 ? 100 : hp );
		float frac = hp / 100.0f;
		float bx = cx - w * 0.5f - 5.0f;
		dl->AddRectFilled( ImVec2( bx - 3, top ), ImVec2( bx, bot ), kBoxBg );
		dl->AddRectFilled( ImVec2( bx - 3, top + h * ( 1 - frac ) ), ImVec2( bx, bot ), frac > 0.4f ? kHpGood : kHpLow );
	}
	if ( set.esp_distance ) {
		float dist = game::distance_to( center );
		if ( dist >= 0 ) {
			char line[64];
			_snprintf( line, sizeof( line ) - 1, "[%.0fm]", dist );
			line[sizeof( line ) - 1] = '\0';
			text_shadowed( dl, cx, bot + 2.0f, kName, line, true );
		}
	}
}

// EXTRA: a streamed-OUT (distant) player from the cached ID_MARKERS_SYNC position.
void draw_streamed_out( ImDrawList *dl, int id, const float pos[3] ) {
	float head[3] = { pos[0], pos[1], pos[2] + 1.0f };
	float sx, sy, z;
	if ( !calc_screen_z( head, sx, sy, z ) )
		return;
	dl->AddNgon( ImVec2( sx, sy ), 4.0f, IM_COL32( 120, 180, 255, 210 ), 4, 1.4f );
	char line[96];
	_snprintf( line, sizeof( line ) - 1, "%s [%d]", samp::player_name( id ).c_str(), id );
	line[sizeof( line ) - 1] = '\0';
	text_shadowed( dl, sx, sy - ImGui::GetTextLineHeight() - 2.0f, IM_COL32( 150, 190, 255, 220 ), line, true );
}

// EXTRA: "Fuego Reversed" + active-cheat list (only with set.show_active_list).
void draw_active_list( ImDrawList *dl ) {
	auto &set = fx::g_set;
	struct { bool on; const char *name; } items[] = {
		{ set.silent_aim, "Silent Aim" }, { set.trigger_bot, "Trigger Bot" },
		{ set.no_spread, "No Spread" }, { set.no_reload, "No Reload" },
		{ set.player_esp, "ESP" }, { set.nametags, "Nametags" }, { set.weapon_finder, "Weapon Finder" },
		{ set.health_hack, "Health Hack" }, { set.invisibility, "Invisibility" },
		{ set.fly, "Fly" }, { set.air_brake, "Air Brake" },
		{ set.moonwalk, "Moonwalk" }, { set.upside_down, "Upside Down" }, { set.fake_lag, "Fake Lag" },
		{ set.explode_cars || set.explode_cars_silent, "Explode Cars" },
		{ set.floating_cars, "Floating Cars" }, { set.remove_cars, "Remove Cars" },
	};
	float x = 12, y = 10, lh = ImGui::GetTextLineHeight();
	dl->AddText( ImVec2( x + 1, y + 1 ), IM_COL32( 0, 0, 0, 220 ), "Fuego Reversed" );
	dl->AddText( ImVec2( x, y ), IM_COL32( 255, 120, 40, 255 ), "Fuego Reversed" );
	y += lh + 4;
	for ( auto &it : items ) {
		if ( !it.on ) continue;
		dl->AddText( ImVec2( x + 8 + 1, y + 1 ), IM_COL32( 0, 0, 0, 200 ), it.name );
		dl->AddText( ImVec2( x + 8, y ), IM_COL32( 220, 220, 220, 255 ), it.name );
		y += lh + 1;
	}
}

// ---- SA-MP nametag type (native tags through walls) ---------------------------------
// Fuego tempnt_*: force SA:MP's own tags to 2000 units + through walls, restoring the
// server's presets when switched off.
void apply_native_nametags( bool on ) {
	static bool    toggled  = false;
	static float   saveDist = 0.0f;
	static uint8_t saveShow = 0, saveWalls = 0;
	samp::stServerPresets *sp = samp::server_presets();
	if ( !sp )
		return;
	if ( on ) {
		if ( !toggled ) {
			saveDist  = sp->fNameTagsDistance;
			saveShow  = sp->byteShowNameTags;
			saveWalls = sp->byteNoNametagsBehindWalls;
			toggled   = true;
		}
		sp->fNameTagsDistance         = 2000.0f;
		sp->byteShowNameTags          = 1;
		sp->byteNoNametagsBehindWalls = 0;
	} else if ( toggled ) {
		sp->fNameTagsDistance         = saveDist;
		sp->byteShowNameTags          = saveShow;
		sp->byteNoNametagsBehindWalls = saveWalls;
		toggled = false;
	}
}

// ---- S0biet nametags (Fuego renderPlayerTags, proxyIDirect3DDevice9.cpp:951-1453) ----
// "Air Ride" layout: per GTA ped-pool slot, a screen anchor + a vertical offset that
// climbs 2px/frame while it collides with a nearer tag and settles back afterwards.
struct TagInfo {
	float x, y, z;       // tagPosition (screen x/y + depth)
	float offY;          // tagOffsetY
	bool  stair;         // isStairStacked
	float stairOff;      // stairStackedOffset
	bool  past;          // isPastMaxDistance
};
TagInfo g_tag[kMaxPoolPeds]; // zero-initialised global, exactly like g_playerTagInfo

void render_player_tags( const float ourPos[3] ) {
	traceLastFunc( "visuals::render_player_tags" );
	auto &set = fx::g_set;

	// alignment settings (Fuego)
	constexpr float kPixelOffsetY = -10.0f;
	constexpr float kBoxOffsetX   = -0.43f;
	constexpr float kBoxOffsetY   = 0.68f;
	constexpr float kPosOffsetZ   = 1.0f;
	constexpr float kEspHeight    = 20.0f;

	bool collided[kMaxPoolPeds];
	bool stairStacked[kMaxPoolPeds];
	for ( int i = 0; i < kMaxPoolPeds; i++ ) { collided[i] = false; stairStacked[i] = true; }

	// initial variables for peds streamed in
	for ( int p = 0; p < g_pedCount; p++ ) {
		const game::PoolPed &e = g_peds[p];
		if ( e.poolIndex == g_selfPool || e.poolIndex >= kMaxPoolPeds ) continue;
		TagInfo &t = g_tag[e.poolIndex];
		if ( dist3( ourPos, e.pos ) > set.player_tags_dist ) { t.past = true; continue; }
		t.past = false;
		float world[3] = { e.pos[0], e.pos[1], e.pos[2] + kPosOffsetZ };
		float sx, sy, sz;
		calc_screen_z( world, sx, sy, sz );
		if ( sz < 1.f ) { t.offY = 0.0f; t.past = true; continue; }
		t.x = sx; t.y = sy; t.z = sz;
	}

	// remove the staircase problem
	for ( int p = 0; p < g_pedCount; p++ ) {
		const int i = g_peds[p].poolIndex;
		if ( i == g_selfPool || i >= kMaxPoolPeds ) continue;
		TagInfo &ti = g_tag[i];
		if ( ti.past || ( !ti.stair && ti.offY < 40.0f ) ) continue;
		if ( !ti.stair ) {
			for ( int q = 0; q < g_pedCount; q++ ) {
				const int j = g_peds[q].poolIndex;
				if ( j >= kMaxPoolPeds || g_tag[j].past || j == i ) continue;
				const TagInfo &tj = g_tag[j];
				if ( fabsf( ti.x - tj.x ) <= 100.0f &&
				     fabsf( ( ti.y - ( ti.offY / 2.0f ) ) - ( tj.y - tj.offY ) ) <= kEspHeight )
					stairStacked[i] = false;
			}
			if ( stairStacked[i] ) {
				ti.stair    = true;
				ti.stairOff = ti.offY / 2.0f;
			}
		}
		if ( ti.stair ) {
			ti.offY     -= 2.0f;
			ti.stairOff -= 2.0f;
			if ( ti.stairOff < 2.0f ) {
				ti.stairOff = 0.0f;
				ti.stair    = false;
			}
		}
	}

	// detect & adjust for collisions (Fuego doesn't skip itself or far/culled partners here)
	for ( int p = 0; p < g_pedCount; p++ ) {
		const int i = g_peds[p].poolIndex;
		if ( i >= kMaxPoolPeds ) continue;
		TagInfo &ti = g_tag[i];
		if ( ti.past || ti.stair ) continue;
		for ( int q = 0; q < g_pedCount; q++ ) {
			const int j = g_peds[q].poolIndex;
			if ( j >= kMaxPoolPeds ) continue;
			TagInfo &tj = g_tag[j];
			if ( ti.past || tj.stair || i == j ) continue;
			const float dy = fabsf( ( ti.y - ti.offY ) - ( tj.y - tj.offY ) );
			if ( fabsf( ti.x - tj.x ) <= 100.0f && dy <= kEspHeight ) {
				if ( ti.z < tj.z )      { tj.offY += 2.0f; collided[j] = true; } // j is farther: move it up
				else if ( ti.z > tj.z ) { ti.offY += 2.0f; collided[i] = true; }
				else if ( i < j )       { tj.offY += 2.0f; collided[j] = true; } // same depth: lower id stays
				else                    { ti.offY += 2.0f; collided[i] = true; }
			}
			// "are we jigglin?"
			if ( fabsf( ti.x - tj.x ) <= 100.0f &&
			     fabsf( ( ti.y - ti.offY ) - ( tj.y - tj.offY ) ) - 2.0f <= kEspHeight ) {
				if ( ti.z < tj.z ) collided[j] = true;
				else               collided[i] = true;
			}
		}
		if ( !collided[i] ) {
			if ( ti.offY >= 2.0f ) ti.offY -= 2.0f;
			else                   ti.offY = 0.0f;
		}
	}

	// render
	const float hSmall = draw_height( Font::FixedSmall );
	const float hSamp  = draw_height( Font::SampStuff ) - 1.0f;
	(void) hSmall;
	char buf[256];
	for ( int p = 0; p < g_pedCount; p++ ) {
		const game::PoolPed &e = g_peds[p];
		const int i = e.poolIndex;
		if ( i >= kMaxPoolPeds || g_tag[i].past || i == g_selfPool ) continue;
		const TagInfo &t = g_tag[i];
		const float baseY = t.y - t.offY + kPixelOffsetY;

		// SA:MP maintains its own health: only remote players whose GTA ped is this one.
		const int id = samp_id_of_ped( e.ped );
		if ( id < 0 ) continue;
		samp::stRemotePlayer *rp = samp::remote_player( id );
		if ( !rp || !rp->pPlayerData ) continue;
		const float vh = rp->pPlayerData->fActorHealth;
		const float va = rp->pPlayerData->fActorArmor;

		float hpbar = vh;
		if ( hpbar > 999.0f ) hpbar = 999.0f;
		hpbar *= 70.0f / 100.0f;

		float offY = kBoxOffsetY;
		if ( va > 0.0f ) {
			float armbar = va;
			if ( armbar > 100.0f ) armbar = 100.0f;
			armbar *= 70.0f / 100.0f;
			d3d_box_border( t.x + kBoxOffsetX, baseY + offY, 70.0f, 10.0f, ARGB( 255, 0, 0, 0 ), ARGB( 128, 50, 50, 50 ) );
			d3d_box( t.x + 1.0f + kBoxOffsetX, baseY + 1.0f + offY, armbar - 2.0f, 8.0f, ARGB( 128, 250, 250, 250 ) );
			offY += 11.0f;
		}
		d3d_box_border( t.x + kBoxOffsetX, baseY + offY, 70.0f, 10.0f, ARGB( 255, 0, 0, 0 ), ARGB( 128, 80, 0, 0 ) );
		d3d_box( t.x + 1.0f + kBoxOffsetX, baseY + 1.0f + offY, hpbar - 2.0f, 8.0f, ARGB( 128, 255, 0, 0 ) );

		if ( va > 0.0f ) _snprintf( buf, sizeof buf - 1, "%d {E00000}%d", (int) va, (int) vh );
		else             _snprintf( buf, sizeof buf - 1, "{E00000}%d", (int) vh );
		buf[sizeof buf - 1] = '\0';
		const float w = draw_length( Font::FixedSmall, buf );
		print_shadow( Font::FixedSmall, t.x + 70.0f - w - 1.0f, baseY + 10.0f + offY, ARGB( 0xFF, 0xF0, 0xF0, 0xF0 ), buf );

		// the main nametag last, so it's on top
		float drawX = t.x;
		const float drawY = baseY - hSamp;
		const std::string name = samp::player_name( id );
		_snprintf( buf, sizeof buf - 1, "%s (%d)", name.c_str(), id );
		buf[sizeof buf - 1] = '\0';
		print_shadow( Font::SampStuff, drawX, drawY, samp_color( id, 0xDD000000 ), buf );
		drawX += draw_length( Font::SampStuff, buf );

		if ( !rp->pPlayerData->iShowNameTag ) {
			print_shadow( Font::SampStuff, drawX, drawY, XRGB( 255, 0, 0 ), "  [M]" );
			drawX += draw_length( Font::SampStuff, "  [M]" );
		}
		if ( friends::count() > 0 && friends::is_friend( name ) ) {
			print_shadow( Font::SampStuff, drawX, drawY, XRGB( 0, 0xFF, 0x33 ), "  [F]" );
			drawX += draw_length( Font::SampStuff, "  [F]" );
		}
		if ( rp->pPlayerData->iAFKState == 2 )
			print_shadow( Font::FixedSmall, t.x + 1.0f, baseY + 10.0f + offY, ARGB( 0xFF, 0xF0, 0xF0, 0xF0 ), "AFK" );
	}
}

// Fuego's gating around renderPlayerTags: pause menu -> untouched; SA-MP type / off /
// panic -> SA:MP's own tags back; scoreboard / F10 -> skip (own tags stay patched off).
void nametags( const float ourPos[3] ) {
	auto &set = fx::g_set;
	const bool renderTags = set.nametags;
	const bool defaultNt  = set.nametags_samp;
	apply_native_nametags( defaultNt && renderTags );
	if ( !renderTags || defaultNt ) {
		samp::disable_native_nametags( false );
		return;
	}
	if ( samp_event_hide() )
		return;
	samp::disable_native_nametags( true );
	render_player_tags( ourPos );
}

// ---- vehicle tags: [LOCKED] (Fuego renderVehicleTags, 1455-1601) -----------------
void vehicle_tags( const float ourPos[3] ) {
	traceLastFunc( "visuals::vehicle_tags" );
	auto &set = fx::g_set;
	if ( !set.locked_vehicle_esp ) return;
	if ( samp_event_hide() ) return;
	static float pos[512][3];
	const int n = game::locked_vehicle_positions( pos, 512 ); // CVehicle::AreDoorsLocked()
	float cam[3];
	if ( !game::active_cam_source( cam ) ) return;
	const float h = draw_height( Font::Fixed );
	for ( int i = 0; i < n; i++ ) {
		const float *vp = pos[i];
		if ( dist3( ourPos, vp ) > set.vehicle_tags_dist ) continue;
		if ( fabsf( vp[0] ) < 0.0001f && fabsf( vp[1] ) < 0.0001f && fabsf( vp[2] ) < 0.0001f ) continue; // IsNearZero
		float world[3] = { vp[0], vp[1], vp[2] - 1.0f };
		float sx, sy, sz;
		calc_screen_z( world, sx, sy, sz );
		if ( sz < 1.f ) continue;
		bool visible = los_clear( cam, vp );
		if ( dist3( vp, cam ) >= 70.0f ) visible = false;
		if ( visible )
			print_shadow( Font::Fixed, sx, sy - h + 0.0f + 2.5f, XRGB( 255, 0, 0 ), "[LOCKED]" );
	}
}

// ---- weapon finder + players' guns (Fuego cheat_handle_weaponfinder, 5863-6070) ----
void weapon_finder() {
	traceLastFunc( "visuals::weapon_finder" );
	auto &set = fx::g_set;
	if ( samp_event_hide() ) return;
	if ( !set.show_players_guns && !set.weapon_finder ) return;

	float headPos[3];
	if ( !game::local_bone_pos( 8, headPos ) ) return; // mod_sa BONE_HEAD == game bone 8
	const float hSamp = draw_height( Font::SampStuff ) - 1.0f;
	char name[256];

	if ( set.show_players_guns ) { // playerWeaponFinder: every ped in the pool, actors too
		for ( int p = 0; p < g_pedCount; p++ ) {
			const game::PoolPed &e = g_peds[p];
			if ( e.poolIndex == g_selfPool ) continue;
			int types[13], ammo[13];
			if ( game::ped_weapons( e.ped, types, ammo ) != 13 ) continue;
			int row = 0;
			for ( int s = 0; s < 13; s++ ) {
				if ( !( ( types[s] >= 16 && types[s] <= 38 && ammo[s] > 0 ) || types[s] == 4 /*knife*/ ) ) continue;
				const char *wn = weapon_list_name( types[s] );
				if ( wn ) _snprintf( name, sizeof name - 1, "%s", wn );
				else      _snprintf( name, sizeof name - 1, "%d", types[s] );
				name[sizeof name - 1] = '\0';
				if ( strlen( name ) < 2 ) continue;
				float sx, sy, sz;
				calc_screen_z( e.pos, sx, sy, sz );
				if ( sz < 1.f ) continue;
				if ( dist3( e.pos, headPos ) >= 1800.0f ) continue;
				print_shadow( Font::SampStuff, sx, sy + ( (float) row * 0.75f ) - hSamp, RGBA( 235, 171, 52, 156 ), name );
				row++;
			}
		}
	}

	if ( set.weapon_finder ) { // dropped weapons (SA:MP objects with a weapon model)
		samp::stObjectPool *op = samp::object_pool();
		int count = 0;
		if ( op && !IsBadReadPtr( op, sizeof( samp::stObjectPool ) ) ) {
			for ( int id = 0; id < samp::SAMP_MAX_OBJECTS; id++ ) {
				if ( !op->iIsListed[id] ) continue;
				samp::stObject *o = op->object[id];
				if ( !o || IsBadReadPtr( o, sizeof( samp::stObject ) ) ) continue;
				float fPos[3];
				if ( !game::entity_pos( o->pGTAEntity, fPos ) ) continue; // the object's live matrix
				if ( fabsf( fPos[0] ) < 0.0001f && fabsf( fPos[1] ) < 0.0001f && fabsf( fPos[2] ) < 0.0001f ) continue;
				if ( fabsf( fPos[2] - headPos[2] ) >= 200.0f && fPos[2] < -100.0f ) continue; // underground + far in Z
				const char *wn = weapon_name_by_model( o->iModel );
				if ( !wn ) continue;
				count++; // Fuego counts before the on-screen test
				float sx, sy, sz;
				calc_screen_z( fPos, sx, sy, sz );
				if ( sz < 1.f ) continue;
				const float d = dist3( fPos, headPos );
				if ( d >= 1800.0f ) { count--; continue; }
				if ( o->iModel == 336 ) count--; // Bat: shown, not counted
				_snprintf( name, sizeof name - 1, "%s [%0.1fm]", wn, d );
				name[sizeof name - 1] = '\0';
				print_shadow( Font::Main, sx, sy - hSamp, RGBA( 61, 237, 237, 128 ), name );
			}
		}
		float sx, sy, sz;
		if ( calc_screen_z( headPos, sx, sy, sz ) ) {
			_snprintf( name, sizeof name - 1, "%d Weapons", count );
			name[sizeof name - 1] = '\0';
			print_shadow( Font::SampStuff, sx, sy - hSamp, RGBA( 255, 255, 0, 128 ), name );
		}
	}
}

// ---- silent aim (Fuego GetSilentAimTarget 6368-6528 + drawing 546-606) ------------
const float kWeaponRange[39] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	90.0f,  // 19 vehicle M4 (custom)
	75.0f,  // 20 vehicle minigun (custom)
	0,
	35.0f, 35.0f, 35.0f, 40.0f, 35.0f, 40.0f, 35.0f, 45.0f, // 22-29
	70.0f, 90.0f, 35.0f, 100.0f, 320.0f, 0, 0, 0, 75.0f       // 30-38
};

// GetAimingCenter: GTA's crosshair (sniper: screen centre).
void aim_center( float &cx, float &cy ) {
	const ImGuiIO &io = ImGui::GetIO();
	samp::stLocalPlayer *lp = samp::local_player();
	if ( lp && lp->byteCurrentWeapon != 34 ) {
		cx = io.DisplaySize.x * 0.5299999714f;
		cy = io.DisplaySize.y * 0.4f;
	} else {
		cx = io.DisplaySize.x / 2.f;
		cy = io.DisplaySize.y / 2.f;
	}
}

// GTAfunc_gtaKeyToVirtualKey.
int gta_key_to_vk( int key ) {
	if ( key >= 44 && key <= 93 ) return key;                // GTA_KEY_COMMA .. GTA_KEY_RBRACKET
	if ( key >= 1001 && key <= 1012 ) return VK_F1 + key - 1001;
	if ( key >= 1028 && key <= 1037 ) return VK_NUMPAD1 + key - 1028;
	switch ( key ) {
		case 2:    return VK_MBUTTON;  case 3:    return VK_RBUTTON;  case 6:    return VK_XBUTTON1;
		case 7:    return VK_XBUTTON2; case 1013: return VK_INSERT;   case 1014: return VK_DELETE;
		case 1015: return VK_HOME;     case 1016: return VK_END;      case 1017: return VK_PRIOR;
		case 1018: return VK_NEXT;     case 1019: return VK_UP;       case 1020: return VK_DOWN;
		case 1021: return VK_LEFT;     case 1022: return VK_RIGHT;    case 1023: return VK_DIVIDE;
		case 1024: return VK_MULTIPLY; case 1025: return VK_ADD;      case 1026: return VK_SUBTRACT;
		case 1027: return VK_DECIMAL;  case 1038: return VK_NUMPAD0;  case 1040: return VK_SCROLL;
		case 1041: return VK_PAUSE;    case 1042: return VK_BACK;     case 1043: return VK_TAB;
		case 1044: return VK_CAPITAL;  case 1045: return VK_RETURN;   case 1046: return VK_LSHIFT;
		case 1047: return VK_RSHIFT;   case 1049: return VK_LCONTROL; case 1050: return VK_RCONTROL;
		case 1051: return VK_LMENU;    case 1052: return VK_RMENU;    case 1053: return VK_LWIN;
		case 1054: return VK_RWIN;
		case 1: case 32: case 35: case 39: case 96: return key; // LMOUSE, SPACE, HASH, APOSTROPHE, GRAVE
		default:   return 0;
	}
}

// Fuego aimKeyDown: the key(s) bound to AIM_WEAPON in GTA's controller config.
bool aim_key_down() {
	auto keyFor = []( int controllerType ) -> int {
		// CControllerConfigManager::GetControllerKeyAssociatedWithAction(AIM_WEAPON = 0x2D, type)
		return mem::thiscall<int>( 0x52F4F0, reinterpret_cast<void *>( 0xB70198 ), 0x2D, controllerType );
	};
	if ( mem::read<uint8_t>( 0xB6EC2E ) ) { // input type: mouse + keyboard
		if ( input::down( gta_key_to_vk( keyFor( 2 /*MOUSE*/ ) ) ) ) return true;
		if ( input::down( gta_key_to_vk( keyFor( 0 /*KEYBOARD*/ ) ) ) ) return true;
		return false;
	}
	return input::down( gta_key_to_vk( keyFor( 3 /*JOYSTICK*/ ) ) );
}

bool inside_circle( int cx, int cy, int rad, int x, int y ) {
	return ( x - cx ) * ( x - cx ) + ( y - cy ) * ( y - cy ) <= rad * rad;
}

int silent_aim_select( int me ) {
	auto &set = fx::g_set;
	samp::stLocalPlayer *lp = samp::local_player();
	if ( !lp ) return -1;
	float ourPos[3];
	if ( !game::local_tag_origin( ourPos ) ) return -1; // own vehicle's position when driving
	float cx, cy;
	aim_center( cx, cy );
	float campos[3];
	game::active_cam_source( campos );

	double lastAimDist = 0.0;
	int    targetID    = -1;
	for ( int p = 0; p < g_pedCount; p++ ) {
		const game::PoolPed &e = g_peds[p];
		if ( e.poolIndex == g_selfPool ) continue;
		const int id = samp_id_of_ped( e.ped ); // SA:MP players only (not actors / NPCs)
		if ( id < 0 ) continue;
		samp::stRemotePlayer *rp = samp::remote_player( id );
		if ( !rp || !rp->pPlayerData ) continue;

		float fPos[3];
		if ( !game::player_world_pos( id, fPos ) ) continue; // getPlayerPos
		float sx, sy, sz;
		calc_screen_z( fPos, sx, sy, sz );
		if ( !inside_circle( (int) cx, (int) cy, (int) ( set.silent_aim_fov + 8 ), (int) sx, (int) sy ) ) continue;
		const double distance = sqrt( (double) ( ( cx - sx ) * ( cx - sx ) + ( cy - sy ) * ( cy - sy ) ) );
		float head[3];
		const bool haveHead = game::player_bone_pos( id, 8, head ); // mod_sa BONE_HEAD
		if ( sz < 1.f ) continue;
		if ( distance > lastAimDist && lastAimDist != 0.0 ) continue;

		// FILTERS
		if ( !set.silent_aim_range_hack ) {
			const int w = lp->byteCurrentWeapon;
			const float range = ( w < 39 ) ? kWeaponRange[w] : 0.0f;
			if ( dist3( ourPos, e.pedPos ) > range ) continue;
		}
		const bool afk     = rp->pPlayerData->iAFKState == 2;
		const bool visible = haveHead && los_clear( campos, head );
		bool alive = true;
		if ( rp->pPlayerData->fActorHealth < 0.9f ) alive = false;
		if ( game::player_playing_anim( id, "SWEET_INJUREDLOOP" ) ) alive = false;
		if ( game::player_playing_anim( id, "CS_DEAD_GUY" ) )       alive = false;
		if ( game::player_playing_anim( id, "KO_SKID_FRONT" ) )     alive = false;
		if ( game::player_playing_anim( id, "KO_SKID_BACK" ) )      alive = false;

		if ( set.silent_aim_team_filter && lp->byteTeamID2 != 255 && rp->pPlayerData->byteTeamID == lp->byteTeamID2 ) continue;
		if ( set.silent_aim_color_filter && samp_color( id ) == samp_color( me ) ) continue;
		if ( set.silent_aim_skin_filter && game::player_skin( id ) == game::local_skin() ) continue;
		if ( !set.silent_aim_wall_shot && !visible ) continue;
		if ( set.silent_aim_afk_filter && afk ) continue;
		if ( set.silent_aim_alive_filter && !alive ) continue;
		if ( set.silent_aim_friend_filter && friends::count() > 0 && friends::is_friend( samp::player_name( id ) ) ) continue;
		// FILTERS END

		lastAimDist = distance;
		targetID    = id;
	}
	return targetID;
}

// Pick the silent-aim target every frame (the net seam's bullet rewrite reads it).
void update_aim_target( int me ) {
	traceLastFunc( "visuals::update_aim_target" );
	auto &set = fx::g_set;
	if ( !set.silent_aim ) { set.silent_aim_target = -1; return; }
	const int aimed = silent_aim_select( me );
	const int prev  = set.silent_aim_target;
	if ( prev != -1 && samp::is_connected( aimed ) && game::player_streamed( aimed ) &&
	     set.silent_aim_friend_filter && friends::count() > 0 && friends::is_friend( samp::player_name( prev ) ) ) {
		set.silent_aim_target = -1;
		status::set( "Friend! Avoided" );
	} else {
		set.silent_aim_target = aimed;
	}
}

// Drawn only while the GTA aim key is held and Draw Tracer isn't Off.
void silent_aim_visuals() {
	auto &set = fx::g_set;
	if ( !set.silent_aim || set.silent_aim_tracer == 0 || !aim_key_down() ) return;
	float cx, cy;
	aim_center( cx, cy );
	if ( set.silent_aim_tracer == 2 ) // ESP & FOV
		circle_outlined( (int) cx, (int) cy, (int) set.silent_aim_fov, 75, 1.25f, ARGB( 32, 255, 255, 255 ), ARGB( 32, 0, 0, 0 ) );

	const int tgt = set.silent_aim_target;
	if ( tgt == -1 || !game::player_streamed( tgt ) ) return;
	float spine[3], self[3];
	if ( !game::player_bone_pos( tgt, 3, spine ) ) return; // BONE_SPINE1
	if ( !game::local_player_pos( self ) ) return;
	float sx, sy, sz;
	calc_screen_z( spine, sx, sy, sz );
	if ( !( sz > 1.f ) ) return;
	const float dist = dist3( spine, self );
	const uint32_t col = samp_color( tgt );
	d3d_line_outlined( (int) sx, (int) sy, (int) cx, (int) cy, 1.25f, col, XRGB( 0, 0, 0 ) );
	const float w = 80.0f - ( dist * 0.75f );
	const float h = 125.0f - ( dist * 0.75f );
	if ( w > 0.0f && h > 0.0f ) {
		d3d_box_border( sx - ( dist * 0.50f ) + 1.0f, sy - ( dist * 0.75f ) + 1.0f, w, h, XRGB( 1, 1, 1 ), ARGB( 0, 1, 1, 1 ) );
		d3d_box_border( sx - ( dist * 0.50f ) - 1.0f, sy - ( dist * 0.75f ) - 1.0f, w, h, XRGB( 1, 1, 1 ), ARGB( 0, 1, 1, 1 ) );
		d3d_box_border( sx - ( dist * 0.50f ), sy - ( dist * 0.75f ), w, h, col, ARGB( 0, 1, 1, 1 ) );
	}
}

// ---- slapper ESP (Fuego cheat_handle_slapperesp, 6190-6243) + fake-lag marker ------
// Both anchor on your own SPINE1 and draw the same outlined line + circle.
void marker_line_circle( const float headS[2], float lx, float ly ) {
	d3d_line_outlined( (int) headS[0], (int) headS[1], (int) lx, (int) ly, 2.0f, XRGB( 192, 0, 0 ), XRGB( 0, 0, 0 ) );
	circle_outlined( (int) ( lx - 2 ), (int) ( ly - 2 ), 10, 56, 2.0f, XRGB( 192, 0, 0 ), XRGB( 0, 0, 0 ) );
}

void slapper_esp() {
	auto &set = fx::g_set;
	if ( set.no_big_hacks ) return;
	float sp[3];
	if ( !cheats::slapper_pos( sp ) ) return; // set by near slapper / slapkill, fresh for 1s
	float spine[3];
	if ( !game::local_bone_pos( 3, spine ) ) return; // BONE_SPINE1
	float head[2], hz, lx, ly, lz;
	calc_screen_z( spine, head[0], head[1], hz );
	calc_screen_z( sp, lx, ly, lz );
	if ( lz > 1.f ) {
		marker_line_circle( head, lx, ly );
		const char *s = "Currently Slapping";
		print_shadow( Font::Main, head[0] - ( draw_length( Font::Main, s ) / 2 ), head[1] - 5, RGBA( 255, 255, 255, 224 ), s );
	}
}

void fakelag_marker() {
	float held[3];
	if ( !cheats::fakelag_held( held ) ) return; // fakeLagLastPosTick >= now
	float spine[3];
	if ( !game::local_bone_pos( 3, spine ) ) return;
	float head[2], hz, lx, ly, lz;
	calc_screen_z( spine, head[0], head[1], hz );
	calc_screen_z( held, lx, ly, lz );
	if ( lz > 1.f ) {
		marker_line_circle( head, lx, ly );
		const float bt = cheats::fakelag_backtrack_left();
		char msg[128];
		if ( bt == 0.5f )       _snprintf( msg, sizeof msg - 1, "You appear at this area for others. (Hold BACKSPACE to backtrack)" );
		else if ( bt <= -1.0f ) _snprintf( msg, sizeof msg - 1, "Backtracked! Release BACKSPACE." );
		else                    _snprintf( msg, sizeof msg - 1, "Backtracking in %0.1fs...", bt );
		msg[sizeof msg - 1] = '\0';
		print_shadow( Font::Main, head[0] - ( draw_length( Font::Main, msg ) / 2 ), head[1] - 5, RGBA( 255, 255, 255, 224 ), msg );
	} else {
		print_shadow( Font::Main, head[0] - 10, head[1] - 5, RGBA( 255, 255, 255, 92 ), "Backtrack vision blocked, look back to view." );
	}
}

// ---- aim slapper crosshair (proxyIDirect3DDevice9.cpp:3978-3990) ------------------
void aim_slapper_crosshair() {
	auto &set = fx::g_set;
	if ( !set.aim_slapper || set.no_big_hacks || scoreboard_open() ) return; // noBigHacks force-clears aimslapper in Fuego
	const ImGuiIO &io = ImGui::GetIO();
	const float cx = io.DisplaySize.x / 2, cy = io.DisplaySize.y / 2;
	const uint32_t c = XRGB( 253, 255, 12 );
	d3d_line( (float) (int) ( cx + 15 ), (float) (int) ( cy + 15 ), (float) (int) ( cx + 3 ), (float) (int) ( cy + 3 ), c );
	d3d_line( (float) (int) ( cx - 15 ), (float) (int) ( cy + 15 ), (float) (int) ( cx - 3 ), (float) (int) ( cy + 3 ), c );
	d3d_line( (float) (int) ( cx + 15 ), (float) (int) ( cy - 15 ), (float) (int) ( cx + 3 ), (float) (int) ( cy - 3 ), c );
	d3d_line( (float) (int) ( cx - 15 ), (float) (int) ( cy - 15 ), (float) (int) ( cx - 3 ), (float) (int) ( cy - 3 ), c );
}

} // namespace

namespace visuals {

void init_fonts() {
	// "Samsung Sans": installed (as GDI would find it for Fuego), else Fuego's shipped
	// fuego_font.ttf beside the game, else GDI's usual substitute (Arial).
	std::string sans = installed_face( "Samsung Sans" );
	if ( sans.empty() ) {
		const std::string dir = game_dir();
		for ( const char *rel : { "\\fuego_font.ttf", "\\project_fuego\\fuego_font.ttf" } )
			if ( file_exists( dir + rel ) ) { sans = dir + rel; break; }
	}
	if ( sans.empty() ) sans = fonts_dir() + "arial.ttf";
	std::string smallPath = fonts_dir() + "tahoma.ttf"; // "Small Fonts" is raster-only
	if ( !file_exists( smallPath ) ) smallPath = fonts_dir() + "arial.ttf";
	g_sans  = load_font( sans, g_sansK );
	g_small = load_font( smallPath, g_smallK );
}

void print_shadow( Font f, float x, float y, uint32_t argb, const char *text ) {
	if ( !text || !*text ) return;
	ImDrawList *dl = bg();
	print_pass( dl, f, x + 1.0f, y + 1.0f, argb & 0xFF000000, text, true ); // render_text_shadows = true (shipped)
	print_pass( dl, f, x, y, argb, text, false );
}

float draw_length( Font f, const char *text ) {
	if ( !text ) return 0.0f;
	static std::vector<Segment> segs;
	segments( text, 0xFFFFFFFF, true, segs );
	ImFont *ff = face( f );
	const float px = font_px( f );
	float len = 0.0f;
	for ( const Segment &s : segs )
		len += ff->CalcTextSizeA( px, FLT_MAX, 0.0f, s.text.c_str() ).x + (float) s.glyphs;
	return len;
}

float draw_height( Font f ) { return floorf( font_px( f ) + 0.5f ); }

bool calc_screen( const float world[3], float &sx, float &sy ) {
	float sz;
	return calc_screen_z( world, sx, sy, sz );
}

void draw() {
	traceLastFunc( "visuals::draw" );
	auto &set = fx::g_set;
	if ( set.cheat_panic ) {
		apply_native_nametags( false );          // Fuego: panic restores the presets ...
		samp::disable_native_nametags( false );  // ... and SA:MP's own nametags
		return;
	}
	if ( !game::available() )
		return;

	const int me = samp::local_player_id();
	g_pedCount = game::ped_pool_snapshot( g_peds, kMaxPoolPeds, g_selfPool );
	build_ped_map( me );

	update_aim_target( me ); // every frame, for the net seam (independent of drawing)

	// EXTRA: F8 (SA:MP's screenshot key) hides every overlay and puts SA:MP's own
	// nametags back so the shot looks like a vanilla client.
	if ( render::overlay_hidden() ) {
		apply_native_nametags( false );
		samp::disable_native_nametags( false );
		return;
	}

	ImDrawList *dl = bg();
	const bool paused = input::gta_menu_active();

	if ( set.show_active_list )
		draw_active_list( dl );

	float ourPos[3];
	const bool haveOur = game::local_tag_origin( ourPos );

	// Fuego skips nametags / vehicle tags / the finders entirely in GTA's pause menu.
	if ( !paused && haveOur ) {
		nametags( ourPos );
		vehicle_tags( ourPos );
		weapon_finder();
	}

	// EXTRA: box ESP + distant (streamed-out) players; hidden like Fuego's overlays.
	if ( set.player_esp && !paused && !samp_event_hide() ) {
		for ( int id = 0; id < kMaxPlayers; id++ ) {
			if ( id == me || !samp::is_connected( id ) )
				continue;
			if ( game::player_streamed( id ) ) {
				draw_player( dl, id );
			} else {
				float mp[3];
				if ( cheats::streamed_out_pos( id, mp ) )
					draw_streamed_out( dl, id, mp );
			}
		}
	}

	// sampMainCheat-side visuals: not pause-gated in Fuego.
	silent_aim_visuals();
	slapper_esp();
	fakelag_marker();
	aim_slapper_crosshair();

	status::draw(); // cheat state text (only with set.status_text, Fuego d3dtext_hud)
}

} // namespace visuals
