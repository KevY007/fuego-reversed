#include "visuals.h"

#include "../core/settings.h"
#include "../core/trace.h"
#include "../samp/samp.h"
#include "../samp/pools.h"
#include "../samp/samp_sdk.h"   // server_presets (native-nametag through-wall)
#include "../gta/gtahost.h"
#include "friends.h"
#include "status.h"
#include "cheats.h"

#include "imgui.h"

#include <windows.h>   // IsBadReadPtr (object-pool safety)
#include <cstdio>
#include <cmath>

namespace {

constexpr int   kMaxPlayers = 1004; // SAMP_MAX_PLAYERS
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

// Same, but at an arbitrary font scale (nametags want a slightly bigger, more
// readable label than the ImGui default). Returns the drawn text height.
float text_scaled( ImDrawList *dl, float x, float y, ImU32 col, const char *s, bool center, float scale ) {
	ImFont *f  = ImGui::GetFont();
	float   sz = ImGui::GetFontSize() * scale;
	ImVec2  dim = f->CalcTextSizeA( sz, FLT_MAX, 0.0f, s );
	if ( center ) x -= dim.x * 0.5f;
	dl->AddText( f, sz, ImVec2( x + 1, y + 1 ), kShadow, s );
	dl->AddText( f, sz, ImVec2( x, y ), col, s );
	return dim.y;
}

// SA:MP weapon id -> short display name (for Show Players Guns / weapon finder).
const char *weapon_name( int w ) {
	static const char *N[] = {
		"Fist", "Brass Knuckles", "Golf Club", "Nightstick", "Knife", "Baseball Bat",
		"Shovel", "Pool Cue", "Katana", "Chainsaw", "Dildo", "Dildo", "Vibrator",
		"Vibrator", "Flowers", "Cane", "Grenade", "Teargas", "Molotov", "", "", "",
		"Colt 45", "Silenced", "Deagle", "Shotgun", "Sawnoff", "Combat Shotgun",
		"Uzi", "MP5", "AK-47", "M4", "Tec-9", "Rifle", "Sniper", "RPG", "HS Rocket",
		"Flamethrower", "Minigun", "Satchel", "Detonator", "Spraycan", "Extinguisher",
		"Camera", "NV Goggles", "IR Goggles", "Parachute"
	};
	if ( w < 0 || w >= (int) ( sizeof( N ) / sizeof( N[0] ) ) ) return nullptr;
	return ( N[w] && N[w][0] ) ? N[w] : nullptr;
}

// One player's ESP box with toggleable name / health / distance. player_world_pos
// returns the ped CENTRE (origin), so we anchor the box top at centre+1.0 (== head,
// Fuego's ESP_tag_player_posOffsetZ) and the bottom at centre-1.0 (== feet) -- no
// fudge factors, so the box sits exactly on the ped instead of floating above it.
void draw_player( ImDrawList *dl, int id ) {
	auto &set = fx::g_set;
	float center[3];
	if ( !game::player_world_pos( id, center ) )
		return;

	float head[3] = { center[0], center[1], center[2] + 1.0f };
	float feet[3] = { center[0], center[1], center[2] - 1.0f };
	float hx, hy, fxp, fyp;
	if ( !game::world_to_screen( head, hx, hy ) )   return; // behind camera
	if ( !game::world_to_screen( feet, fxp, fyp ) ) return;

	float top = hy < fyp ? hy : fyp;
	float bot = hy < fyp ? fyp : hy;
	float h   = bot - top;
	if ( h < 4.0f || h > 2000.0f )
		return; // degenerate / absurd projection
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
		uint32_t mc = samp::player_color( id ); // 0xRRGGBBAA -> R/G/B from >>24/>>16/>>8
		ImU32 nc = mc ? IM_COL32( ( mc >> 24 ) & 0xFF, ( mc >> 16 ) & 0xFF, ( mc >> 8 ) & 0xFF, 0xDD ) : kName;
		text_shadowed( dl, cx, top - ImGui::GetTextLineHeight() - 2.0f, nc, line, true );
	}
	if ( set.esp_health ) {
		float hp = game::player_gta_health( id );
		hp = hp < 0 ? 0 : ( hp > 100 ? 100 : hp );
		float frac = hp / 100.0f;
		float bx = cx - w * 0.5f - 5.0f;
		dl->AddRectFilled( ImVec2( bx - 3, top ), ImVec2( bx, bot ), kBoxBg );
		dl->AddRectFilled( ImVec2( bx - 3, top + h * ( 1 - frac ) ), ImVec2( bx, bot ),
			ImU32( frac > 0.4f ? kHpGood : kHpLow ) );
	}
	if ( set.esp_distance ) {
		float dist = game::distance_to( center );
		if ( dist >= 0 ) {
			char line[64];
			_snprintf( line, sizeof( line ) - 1, "[%.0fm]", dist );
			text_shadowed( dl, cx, bot + 2.0f, kName, line, true );
		}
	}
}

// S0biet-style custom nametag (through-wall by construction, since it projects the
// world head point). Anchored at centre+1.0 like the ESP box. Masked mode appends a
// red [M] to players whose nametag the SERVER has hidden (you still see them);
// friends get a green [F]. This is the "Nametag Type: S0biet" path.
void draw_nametag( ImDrawList *dl, int id ) {
	auto &set = fx::g_set;
	float center[3];
	if ( !game::player_world_pos( id, center ) )
		return;
	float head[3] = { center[0], center[1], center[2] + 1.0f };
	float sx, sy;
	if ( !game::world_to_screen( head, sx, sy ) )
		return;

	std::string nm = samp::player_name( id );
	// Fuego reads SA:MP's SYNCED health/armor (not the GTA ped) + clamps HP at 999.
	float hp = samp::player_synced_health( id );
	if ( hp < 0 ) hp = game::player_gta_health( id ); // fall back to the ped if unsynced
	hp = hp < 0 ? 0 : ( hp > 999 ? 999 : hp );
	float armor = samp::player_synced_armor( id );
	armor = armor < 0 ? 0 : ( armor > 100 ? 100 : armor );
	// SA:MP stores the colour as 0xRRGGBBAA (Fuego's samp_color_get does >>8 | alpha),
	// so red is the top byte -- extract R/G/B from >>24/>>16/>>8, not >>16/>>8/&0xFF.
	uint32_t mc = samp::player_color( id );
	ImU32 col = mc ? IM_COL32( ( mc >> 24 ) & 0xFF, ( mc >> 16 ) & 0xFF, ( mc >> 8 ) & 0xFF, 0xDD )
	               : IM_COL32( 240, 240, 240, 255 );

	char line[96];
	_snprintf( line, sizeof( line ) - 1, "%s (%d)", nm.empty() ? "?" : nm.c_str(), id );

	const float scale = 1.6f; // larger, closer to Fuego's SA:MP nametag font
	ImFont *f   = ImGui::GetFont();
	float   fsz = ImGui::GetFontSize() * scale;
	float   nameW = f->CalcTextSizeA( fsz, FLT_MAX, 0.0f, line ).x;
	float   ny  = sy - fsz - 2.0f;
	text_scaled( dl, sx, ny, col, line, true, scale );   // centred name [id]

	// Right-hand indicators (Fuego, custom-nametag mode): [M] = server has hidden this
	// player's nametag (shown whenever it's hidden -- no toggle); [F] = friend; AFK.
	float rightX = sx + nameW * 0.5f + 4.0f;
	if ( !samp::player_shows_nametag( id ) ) {
		text_scaled( dl, rightX, ny, IM_COL32( 255, 0, 0, 255 ), "[M]", false, scale );
		rightX += f->CalcTextSizeA( fsz, FLT_MAX, 0.0f, "[M] " ).x;
	}
	if ( friends::is_friend( nm ) ) {
		text_scaled( dl, rightX, ny, IM_COL32( 0, 255, 51, 255 ), "[F]", false, scale );
		rightX += f->CalcTextSizeA( fsz, FLT_MAX, 0.0f, "[F] " ).x;
	}
	if ( samp::player_afk( id ) ) // server reports the player idle (iAFKState == 2)
		text_scaled( dl, rightX, ny, IM_COL32( 255, 170, 0, 255 ), "AFK", false, scale );

	// (Show Players Guns is a separate full-inventory pass -- draw_players_guns.)

	// Health + armor bars (Fuego draws an armor bar above the HP bar) + a numeric readout.
	const float bw = 52.0f;
	float barY = sy + 1.0f;
	if ( armor > 0.0f ) {
		float aFrac = armor / 100.0f;
		dl->AddRectFilled( ImVec2( sx - bw * 0.5f, barY ), ImVec2( sx + bw * 0.5f, barY + 3 ), kBoxBg );
		dl->AddRectFilled( ImVec2( sx - bw * 0.5f, barY ), ImVec2( sx - bw * 0.5f + bw * aFrac, barY + 3 ), IM_COL32( 210, 210, 210, 210 ) );
		barY += 4.0f;
	}
	float hpFrac = ( hp > 100 ? 100 : hp ) / 100.0f;
	dl->AddRectFilled( ImVec2( sx - bw * 0.5f, barY ), ImVec2( sx + bw * 0.5f, barY + 3 ), kBoxBg );
	dl->AddRectFilled( ImVec2( sx - bw * 0.5f, barY ), ImVec2( sx - bw * 0.5f + bw * hpFrac, barY + 3 ),
		hpFrac > 0.4f ? kHpGood : kHpLow );
	// Numeric armor + HP under the bars (armor first, then HP -- Fuego's order).
	char hpText[48];
	if ( armor > 0.0f ) _snprintf( hpText, sizeof( hpText ) - 1, "%d  %d", (int) armor, (int) hp );
	else                _snprintf( hpText, sizeof( hpText ) - 1, "%d", (int) hp );
	text_scaled( dl, sx, barY + 4.0f, IM_COL32( 232, 64, 64, 255 ), hpText, true, 0.95f );
}

} // namespace

// Pick the connected player nearest the crosshair (screen centre) within the FOV
// radius. Stores the result in g_set.silent_aim_target for the net-seam bullet
// rewrite. Runs every frame so aim tracks even with the menu closed.
void update_aim_target( int me ) {
	traceLastFunc( "visuals::update_aim_target" );
	auto &set = fx::g_set;
	if ( !set.silent_aim ) { set.silent_aim_target = -1; return; }

	ImGuiIO &io = ImGui::GetIO();
	const float cxs = io.DisplaySize.x * 0.5f, cys = io.DisplaySize.y * 0.5f;
	const float radius = (float) set.silent_aim_fov + 8.0f; // raw pixel radius + 8 (Fuego)

	int best = -1;
	float bestD = radius;
	for ( int id = 0; id < kMaxPlayers; id++ ) {
		if ( id == me || !samp::is_connected( id ) || !game::player_streamed( id ) )
			continue;
		if ( set.silent_aim_team_filter && game::same_team( id ) )
			continue; // don't aim teammates
		if ( set.silent_aim_friend_filter && friends::is_friend( samp::player_name( id ) ) )
			continue; // don't aim friends
		if ( set.silent_aim_alive_filter && game::player_gta_health( id ) < 0.9f )
			continue; // Alive Filter: skip dead/downed players
		if ( set.silent_aim_afk_filter && samp::player_afk( id ) )
			continue; // AFK Filter: skip AFK players
		if ( set.silent_aim_skin_filter && game::player_skin( id ) >= 0 && game::player_skin( id ) == game::local_skin() )
			continue; // Skin Filter: skip players wearing your skin
		if ( set.silent_aim_color_filter && samp::player_color( id ) != 0 && samp::player_color( id ) == samp::player_color( me ) )
			continue; // Colour Filter: skip players with your marker colour
		float world[3];
		if ( !game::player_world_pos( id, world ) )
			continue;
		// Shoot Through Walls: when OFF, only target players in line of sight.
		if ( !set.silent_aim_wall_shot && !game::position_visible( world ) )
			continue;
		// Range Hack: when OFF, don't target beyond the current weapon's range.
		if ( !set.silent_aim_range_hack ) {
			static const float fWeaponRange[39] = {
				0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0, 90.0f,
				75.0f,0, 35.0f,35.0f,35.0f,40.0f,35.0f,40.0f,35.0f,45.0f,
				70.0f,90.0f,35.0f,100.0f,320.0f,0,0,0,75.0f };
			int wep = game::local_weapon_type();
			float range = ( wep >= 0 && wep < 39 ) ? fWeaponRange[wep] : 0.0f;
			float dd = game::distance_to( world );
			if ( dd >= 0 && dd > range ) // range 0 (melee) => skip everyone, like Fuego
				continue;
		}
		float sx, sy;
		if ( !game::world_to_screen( world, sx, sy ) )
			continue;
		float d = sqrtf( ( sx - cxs ) * ( sx - cxs ) + ( sy - cys ) * ( sy - cys ) );
		if ( d < bestD ) { bestD = d; best = id; }
	}
	set.silent_aim_target = best;
	if ( best != -1 )
		status::set( "Silent Aim -> %s [%d]", samp::player_name( best ).c_str(), best );

	// FOV circle (only when Draw Tracer is in ESP&FOV mode) + target marker + the
	// tracer line. silent_aim_tracer: 0 = Off, 1 = ESP (line only), 2 = ESP & FOV.
	ImDrawList *dl = ImGui::GetBackgroundDrawList();
	if ( set.silent_aim_tracer >= 2 )
		dl->AddCircle( ImVec2( cxs, cys ), radius, IM_COL32( 255, 255, 255, 40 ), 64, 1.0f );
	if ( best != -1 ) {
		float w[3], sx, sy;
		if ( game::player_world_pos( best, w ) && game::world_to_screen( w, sx, sy ) ) {
			dl->AddCircleFilled( ImVec2( sx, sy ), 4.0f, IM_COL32( 255, 60, 30, 230 ) );
			// Tracer: a line from the bottom-centre of the screen to the target.
			if ( set.silent_aim_tracer >= 1 )
				dl->AddLine( ImVec2( cxs, io.DisplaySize.y ), ImVec2( sx, sy ),
					IM_COL32( 255, 60, 30, 180 ), 1.4f );
		}
	}
}

// Weapon finder: mark nearby weapon pickups (GTA weapon models ~321-373).
// SA:MP weapon OBJECT model id (335-363) -> name (Fuego weapon_list). Only this range
// is shown by the dropped-weapon finder.
const char *weapon_name_by_model( int m ) {
	switch ( m ) {
		case 335: return "Knife";           case 336: return "Baseball Bat";  case 337: return "Shovel";
		case 338: return "Pool Cue";        case 339: return "Katana";        case 341: return "Chainsaw";
		case 342: return "Grenade";         case 343: return "Teargas";       case 344: return "Molotov Cocktail";
		case 346: return "Pistol";          case 347: return "Silenced Pistol"; case 348: return "Desert Eagle";
		case 349: return "Shotgun";         case 350: return "Sawn-Off Shotgun"; case 351: return "SPAZ12";
		case 352: return "Micro UZI";       case 353: return "MP5";           case 355: return "AK47";
		case 356: return "M4";              case 357: return "Country Rifle"; case 358: return "Sniper Rifle";
		case 359: return "Rocket Launcher"; case 360: return "Heat Seeking RPG"; case 361: return "Flame Thrower";
		case 362: return "Minigun";         case 363: return "Remote Explosives";
		default:  return nullptr;
	}
}

// Dropped-weapon finder (Fuego 1:1): SA:MP object-pool objects with a weapon model
// (335-363) -> name + distance; a total "N Weapons" at your head. Bat (336) is shown
// but not counted; underground objects far in Z are skipped; cull past 1800 units.
void draw_weapon_finder( ImDrawList *dl ) {
	traceLastFunc( "visuals::draw_weapon_finder" );
	samp::stObjectPool *op = samp::object_pool();
	if ( !op ) return;
	float me[3];
	if ( !game::local_player_pos( me ) ) return;
	me[2] += 0.6f; // ~head
	int count = 0;
	for ( int id = 0; id < samp::SAMP_MAX_OBJECTS; id++ ) {
		if ( !op->iIsListed[id] ) continue;
		samp::stObject *o = op->object[id];
		if ( !o || IsBadReadPtr( o, sizeof( void * ) ) ) continue;
		const char *wn = weapon_name_by_model( o->iModel );
		if ( !wn ) continue;
		float pos[3] = { o->fPos[0], o->fPos[1], o->fPos[2] };
		if ( pos[0] == 0 && pos[1] == 0 && pos[2] == 0 ) continue;
		if ( fabsf( pos[2] - me[2] ) >= 200.0f && pos[2] < -100.0f ) continue; // underground
		float dist = game::distance_to( pos );
		if ( dist < 0 || dist >= 1800.0f ) continue;
		float sx, sy;
		if ( !game::world_to_screen( pos, sx, sy ) ) continue;
		count++;
		if ( o->iModel == 336 ) count--; // Bat shown but not counted
		char line[96];
		_snprintf( line, sizeof( line ) - 1, "%s [%.1fm]", wn, dist );
		text_shadowed( dl, sx, sy, IM_COL32( 61, 237, 237, 200 ), line, false );
	}
	float hx, hy;
	if ( game::world_to_screen( me, hx, hy ) ) {
		char b[48]; _snprintf( b, sizeof( b ) - 1, "%d Weapons", count );
		text_shadowed( dl, hx, hy, IM_COL32( 255, 255, 0, 200 ), b, true );
	}
}

// Player weapon finder / Show Players Guns (Fuego 1:1): every carried weapon of each
// streamed player, stacked above them.
void draw_players_guns( ImDrawList *dl, int me ) {
	traceLastFunc( "visuals::draw_players_guns" );
	for ( int id = 0; id < kMaxPlayers; id++ ) {
		if ( id == me || !samp::is_connected( id ) || !game::player_streamed( id ) ) continue;
		int slots[13];
		if ( game::player_weapon_slots( id, slots ) == 0 ) continue;
		float pos[3];
		if ( !game::player_world_pos( id, pos ) ) continue;
		float dist = game::distance_to( pos );
		if ( dist < 0 || dist >= 1800.0f ) continue;
		float sx, sy;
		if ( !game::world_to_screen( pos, sx, sy ) ) continue;
		int row = 0;
		for ( int s = 0; s < 13; s++ ) {
			if ( slots[s] < 0 ) continue;
			const char *wn = weapon_name( slots[s] );
			if ( !wn ) continue;
			text_shadowed( dl, sx, sy + row * ImGui::GetTextLineHeight() * 0.75f,
				IM_COL32( 235, 171, 52, 210 ), wn, true );
			row++;
		}
	}
}

// Active-cheats watermark (top-left) so you can always see what's enabled.
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

// Fake-lag: draw where other players currently see you (the held position),
// with a line from your real position -- 1:1 with Fuego's backtrack marker.
void draw_fakelag( ImDrawList *dl ) {
	float held[3];
	if ( !cheats::fakelag_held( held ) )
		return;
	float me[3];
	bool okMe = game::local_player_pos( me );
	if ( okMe ) me[2] += 0.5f;

	float sx, sy, mx, my;
	if ( !game::world_to_screen( held, sx, sy ) )
		return;
	if ( okMe && game::world_to_screen( me, mx, my ) )
		dl->AddLine( ImVec2( mx, my ), ImVec2( sx, sy ), IM_COL32( 200, 0, 0, 200 ), 2.0f );
	dl->AddCircle( ImVec2( sx, sy ), 12.0f, IM_COL32( 200, 0, 0, 235 ), 32, 2.0f );

	// Backtrack prompt / countdown (1:1 with Fuego's wording).
	float bt = cheats::fakelag_backtrack_left();
	char msg[128];
	if ( bt >= 0.5f )
		std::snprintf( msg, sizeof msg, "You appear here to others. (Hold BACKSPACE to backtrack)" );
	else if ( bt <= 0.0f )
		std::snprintf( msg, sizeof msg, "Backtracked! Release BACKSPACE." );
	else
		std::snprintf( msg, sizeof msg, "Backtracking in %0.1fs...", bt );
	text_shadowed( dl, sx, sy - ImGui::GetTextLineHeight() - 14.0f,
		IM_COL32( 255, 255, 255, 235 ), msg, true );
}

// Force SA:MP's own nametags to render through walls at max distance (the "SA-MP"
// nametag type / native nametag hack). Saves the server's values once so toggling
// it off restores them. 1:1 with Fuego's tempnt_* dance.
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
		sp->fNameTagsDistance        = 2000.0f;
		sp->byteShowNameTags         = 1;
		sp->byteNoNametagsBehindWalls = 0; // <- the through-wall bit
	} else if ( toggled ) {
		sp->fNameTagsDistance        = saveDist;
		sp->byteShowNameTags         = saveShow;
		sp->byteNoNametagsBehindWalls = saveWalls;
		toggled = false;
	}
}

// Locked Vehicle ESP: a small marker + label over every locked streamed vehicle.
// Only drawn when the vehicle is in line of sight (NOT through walls).
void draw_locked_vehicles( ImDrawList *dl ) {
	static float pos[128][3];
	int n = game::locked_vehicle_positions( pos, 128 );
	for ( int i = 0; i < n; i++ ) {
		if ( !game::position_visible( pos[i] ) )
			continue; // occluded by geometry -> don't reveal through the wall
		float sx, sy;
		if ( !game::world_to_screen( pos[i], sx, sy ) )
			continue;
		if ( game::distance_to( pos[i] ) > 70.0f ) continue; // Fuego: only within 70 units
		text_shadowed( dl, sx, sy - ImGui::GetTextLineHeight() - 2.0f, IM_COL32( 255, 0, 0, 235 ), "[LOCKED]", true );
	}
}

// Slapper ESP (Fuego 1:1): a red line from you to the slapped point + a circle +
// "Currently Slapping", for the ~1s after a slap. Shown while a slapper is active.
void draw_slapper_esp( ImDrawList *dl ) {
	float sp[3];
	if ( !cheats::slapper_pos( sp ) )
		return;
	float me[3];
	if ( !game::local_player_pos( me ) ) return;
	me[2] += 0.5f; // roughly the spine
	float mx, my, sx, sy;
	if ( !game::world_to_screen( sp, sx, sy ) ) return;
	if ( game::world_to_screen( me, mx, my ) )
		dl->AddLine( ImVec2( mx, my ), ImVec2( sx, sy ), IM_COL32( 192, 0, 0, 255 ), 2.0f );
	dl->AddCircle( ImVec2( sx - 2, sy - 2 ), 10.0f, IM_COL32( 192, 0, 0, 255 ), 56, 2.0f );
	text_shadowed( dl, sx, sy - ImGui::GetTextLineHeight() - 4.0f, IM_COL32( 255, 255, 255, 224 ), "Currently Slapping", true );
}

// A streamed-OUT (distant) player, drawn from the cached ID_MARKERS_SYNC position:
// a small blue diamond + name so ESP covers the whole server, not just nearby peds.
void draw_streamed_out( ImDrawList *dl, int id, const float pos[3] ) {
	float head[3] = { pos[0], pos[1], pos[2] + 1.0f };
	float sx, sy;
	if ( !game::world_to_screen( head, sx, sy ) )
		return;
	dl->AddNgon( ImVec2( sx, sy ), 4.0f, IM_COL32( 120, 180, 255, 210 ), 4, 1.4f );
	char line[96];
	_snprintf( line, sizeof( line ) - 1, "%s [%d]", samp::player_name( id ).c_str(), id );
	text_shadowed( dl, sx, sy - ImGui::GetTextLineHeight() - 2.0f, IM_COL32( 150, 190, 255, 220 ), line, true );
}

namespace visuals {

void draw() {
	traceLastFunc( "visuals::draw" );
	auto &set = fx::g_set;
	if ( set.cheat_panic ) {
		apply_native_nametags( false );          // restore the server presets under panic
		samp::disable_native_nametags( false );  // un-suppress SA:MP's own nametags
		return;
	}
	if ( !game::available() )
		return;

	const int me = samp::local_player_id();
	ImDrawList *dl = ImGui::GetBackgroundDrawList();

	update_aim_target( me ); // silent-aim targeting (always, for the net seam)

	// Hide ALL overlays while the scoreboard is open or F10 is held (Fuego 1:1) -- restore
	// SA:MP's own nametags so the scoreboard stays readable, then bail before any drawing.
	bool suppress = ( GetAsyncKeyState( VK_F10 ) & 0x8000 ) != 0;
	if ( samp::stScoreboardInfo *sb = samp::scoreboard_info() )
		if ( sb->iIsEnabled ) suppress = true;
	if ( suppress ) {
		apply_native_nametags( false );
		samp::disable_native_nametags( false );
		return;
	}

	draw_active_list( dl );

	// Nametag type: SA-MP (native, through-wall via server presets) vs S0biet (our
	// custom overlay). SA-MP mode is a per-frame preset flip; S0biet is drawn below.
	// When drawing S0biet we RET-patch SA:MP's own nametags so they don't double up.
	bool customTags = set.nametags && !set.nametags_samp;
	apply_native_nametags( set.nametags && set.nametags_samp );
	samp::disable_native_nametags( customTags );

	if ( set.player_esp || customTags ) {
		for ( int id = 0; id < kMaxPlayers; id++ ) {
			if ( id == me || !samp::is_connected( id ) )
				continue;
			if ( game::player_streamed( id ) ) {
				if ( set.player_esp ) draw_player( dl, id );
				if ( customTags )     draw_nametag( dl, id ); // S0biet custom nametag
			} else if ( set.player_esp ) {
				float mp[3];
				if ( cheats::streamed_out_pos( id, mp ) )
					draw_streamed_out( dl, id, mp ); // distant player from marker sync
			}
		}
	}

	if ( set.weapon_finder )
		draw_weapon_finder( dl );
	if ( set.show_players_guns )
		draw_players_guns( dl, me );

	if ( set.locked_vehicle_esp )
		draw_locked_vehicles( dl );

	draw_slapper_esp( dl ); // self-gates on a recent slap (near-slap or slapkill)

	draw_fakelag( dl ); // where you appear to others while frozen

	status::draw(); // transient "X is ACTIVE" text
}

} // namespace visuals
