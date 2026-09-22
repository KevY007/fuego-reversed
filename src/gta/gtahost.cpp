// gta/gtahost.cpp -- the single TU that includes gta-reversed's game layer.
// Compiled at C++23 with the gta-reversed include roots + vendored deps (see the
// per-file settings in FuegoReversed.vcxproj). Keeps the heavy include chain out
// of every other TU, which stays lean C++20 and only sees gtahost.h.

#include "gta_reversed.h"      // one umbrella for the gta-reversed game classes
                              // (StdInc + entities + World/Pools/Camera/Weapon/
                              //  Pickups/Sprite). C++23-only; see that header.

#include "gtahost.h"
#include "../core/memory.h"    // mem::ccall -> call the real game's functions
#include "../core/trace.h"     // traceLastFunc (crash breadcrumbs)
#include "../samp/pools.h"     // SA:MP player pool -> game ped bridge

// carcontrol: the real game's ped-in/out-of-car warp tasks (same ones the SA:MP
// GTA interface drives). Their ProcessPed thunks straight to the real addresses
// (0x647D10 / 0x64B950), so we possess a car exactly like Fuego's GTAfuncs.
#include "TaskSimpleCarSetPedOut.h"
#include "TaskSimpleCarSetPedInAsDriver.h"
#include "TaskSimpleCarSetPedInAsPassenger.h"
#include "ColPoint.h"          // CWorld::ProcessLineOfSight out-param (vehicle picker)
#include "ModelInfo.h"         // CModelInfo::GetModelInfo -> the game's own model-info table
#include "VehicleModelInfo.h"  // CVehicleModelInfo::m_szGameName (GXT key -> display name)

#include <cmath>

namespace {

// FindPlayerPed(int32 playerId = -1) lives at 0x56E210 (gta-reversed's
// RH_ScopedGlobalInstall binding). We call the REAL game's copy so we read the
// live player, and never depend on linking gta-reversed's reimplemented body.
inline CPlayerPed *find_player_ped() {
	return mem::ccall<CPlayerPed *>( 0x56E210, (int32)-1 );
}

// gta-reversed CPed* for a SA:MP player id, or null if not streamed in. The
// SA:MP pool stores the game ped pointer; we just reinterpret it to gta-reversed's
// CPed (same object, accurate layout).
CPed *player_ped( int sampId ) {
	samp::stRemotePlayer *rp = samp::remote_player( sampId );
	if ( !rp || !rp->pPlayerData || !rp->pPlayerData->pSAMP_Actor )
		return nullptr;
	void *ped = rp->pPlayerData->pSAMP_Actor->pGTA_Ped;
	if ( !ped || IsBadReadPtr( ped, 4 ) )
		return nullptr;
	return reinterpret_cast<CPed *>( ped );
}

} // namespace

namespace game {

bool available() {
	return find_player_ped() != nullptr;
}

bool local_player_pos( float out[3] ) {
	CPlayerPed *p = find_player_ped();
	if ( !p )
		return false;
	const CVector &pos = p->GetPosition(); // gta-reversed inline (reads the entity matrix)
	out[0] = pos.x;
	out[1] = pos.y;
	out[2] = pos.z;
	return true;
}

float local_player_health() {
	CPlayerPed *p = find_player_ped();
	return p ? p->m_fHealth : -1.0f;
}

float local_player_max_health() {
	CPlayerPed *p = find_player_ped();
	return p ? p->m_fMaxHealth : -1.0f;
}

void set_local_player_health( float hp ) {
	traceLastFunc( "game::set_local_player_health" );
	if ( CPlayerPed *p = find_player_ped() )
		p->m_fHealth = hp;
}

void teleport_local( const float pos[3] ) {
	traceLastFunc( "game::teleport_local" );
	if ( CPlayerPed *p = find_player_ped() )
		p->Teleport( CVector( pos[0], pos[1], pos[2] ), false ); // virtual -> game's teleport
}

bool world_to_screen( const float world[3], float &sx, float &sy ) {
	traceLastFunc( "game::world_to_screen" );
	RwV3d in { world[0], world[1], world[2] }, out;
	float w = 0, h = 0;
	if ( !CSprite::CalcScreenCoors( in, &out, &w, &h, true, true ) )
		return false;
	sx = out.x;
	sy = out.y;
	return true;
}

bool player_world_pos( int sampId, float out[3] ) {
	traceLastFunc( "game::player_world_pos" );
	if ( CPed *p = player_ped( sampId ) ) {
		const CVector &pos = p->GetPosition();
		out[0] = pos.x; out[1] = pos.y; out[2] = pos.z;
		return true;
	}
	return samp::player_pos( sampId, out ); // fall back to last synced position
}

float player_gta_health( int sampId ) {
	traceLastFunc( "game::player_gta_health" );
	CPed *p = player_ped( sampId );
	return p ? p->m_fHealth : -1.0f;
}

int player_skin( int sampId ) {
	traceLastFunc( "game::player_skin" );
	CPed *p = player_ped( sampId );
	return p ? (int) p->m_nModelIndex : -1;
}

int local_skin() {
	CPlayerPed *p = find_player_ped();
	return p ? (int) p->m_nModelIndex : -1;
}

bool player_streamed( int sampId ) {
	return player_ped( sampId ) != nullptr;
}

bool same_team( int sampId ) {
	samp::stRemotePlayer *rp = samp::remote_player( sampId );
	samp::stLocalPlayer  *lp = samp::local_player();
	if ( !rp || !rp->pPlayerData || !lp )
		return false;
	uint8_t mine = lp->byteTeamID2, theirs = rp->pPlayerData->byteTeamID;
	return mine != 255 && mine == theirs; // 255 = NO_TEAM
}

float distance_to( const float world[3] ) {
	traceLastFunc( "game::distance_to" );
	CPlayerPed *p = find_player_ped();
	if ( !p ) return -1.0f;
	const CVector &me = p->GetPosition();
	float dx = world[0] - me.x, dy = world[1] - me.y, dz = world[2] - me.z;
	return sqrtf( dx * dx + dy * dy + dz * dz );
}

bool player_bone_pos( int sampId, int bone, float out[3] ) {
	traceLastFunc( "game::player_bone_pos" );
	CPed *p = player_ped( sampId );
	if ( !p ) return false;
	CVector v = p->GetBonePosition( (eBoneTag) bone, false );
	out[0] = v.x; out[1] = v.y; out[2] = v.z;
	return true;
}

bool player_velocity( int sampId, float out[3] ) {
	CPed *p = player_ped( sampId );
	if ( !p ) return false;
	const CVector &s = p->m_vecMoveSpeed;
	out[0] = s.x; out[1] = s.y; out[2] = s.z;
	return true;
}

float ground_z( float x, float y ) {
	traceLastFunc( "game::ground_z" );
	return CWorld::FindGroundZForCoord( x, y );
}

float player_heading( int sampId ) {
	CPed *p = player_ped( sampId );
	return p ? p->m_fCurrentRotation : 0.0f;
}

bool player_vehicle_axes( int sampId, float right[3], float up[3] ) {
	traceLastFunc( "game::player_vehicle_axes" );
	samp::stRemotePlayer *rp = samp::remote_player( sampId );
	if ( !rp || !rp->pPlayerData || !rp->pPlayerData->pSAMP_Vehicle )
		return false;
	void *veh = rp->pPlayerData->pSAMP_Vehicle->pGTA_Vehicle;
	if ( !veh || IsBadReadPtr( veh, 4 ) )
		return false;
	CVehicle *v = reinterpret_cast<CVehicle *>( veh );
	CVector r = v->GetRight(), u = v->GetUp();
	right[0] = r.x; right[1] = r.y; right[2] = r.z;
	up[0] = u.x; up[1] = u.y; up[2] = u.z;
	return true;
}

CVehicle *local_vehicle_internal(); // defined below (fly/air-brake section)

bool camera_forward( float out[3] ) {
	traceLastFunc( "game::camera_forward" );
	CVector f = TheCamera.m_mCameraMatrix.GetForward();
	out[0] = f.x; out[1] = f.y; out[2] = f.z;
	return true;
}

bool position_visible( const float pos[3] ) {
	traceLastFunc( "game::position_visible" );
	CVector *cam = TheCamera.GetGameCamPosition();
	if ( !cam )
		return true; // no camera -> don't hide it
	CVector tgt( pos[0], pos[1], pos[2] );
	// Call the REAL game's CWorld::GetIsLineOfSightClear @0x56A490 (we never install
	// gta-reversed's hook, so the original body is live). Clear only if no building/
	// object blocks the ray -- ignore peds/vehicles so a body between isn't a "wall".
	// Flags mirror Fuego's IsLineOfSightClear(...,true,false,false,true,true,false,false):
	// check buildings + objects + DUMMIES, ignore vehicles/peds (a body between isn't a wall).
	return mem::ccall<bool>( 0x56A490, cam, &tgt, true, false, false, true, true, false, false );
}

// True if a remote player's ped is currently playing the named animation. Fuego 1:1
// (CEntity::IsPlayingAnimation): the real cdecl RpAnimBlendClumpGetAssociation @0x4D6870
// takes (ped RwClump, animName) and returns the association if it's active. Used by the
// silent-aim Alive Filter to skip downed/KO'd players (LSRP) whose health isn't zeroed but
// who are playing an injured/dead anim.
bool player_playing_anim( int sampId, const char *animName ) {
	traceLastFunc( "game::player_playing_anim" );
	CPed *ped = player_ped( sampId );
	if ( !ped ) return false;
	void *clump = ped->GetRwObject(); // ped's RwObject IS its RpClump
	if ( !clump || IsBadReadPtr( clump, 4 ) ) return false;
	return mem::ccall<void *>( 0x4D6870, clump, animName ) != nullptr;
}

bool launch_vehicle( int sampVehId, const float pos[3], const float speed[3] ) {
	traceLastFunc( "game::launch_vehicle" );
	samp::stVehiclePool *vp = samp::vehicle_pool();
	if ( !vp || sampVehId < 0 || sampVehId >= samp::SAMP_MAX_VEHICLES )
		return false;
	if ( !vp->iIsListed[sampVehId] )
		return false;
	void *gv = vp->pGTA_Vehicle[sampVehId];
	if ( !gv || IsBadReadPtr( gv, 4 ) )
		return false;
	CVehicle *v = reinterpret_cast<CVehicle *>( gv );
	v->SetPosn( CVector( pos[0], pos[1], pos[2] ) );
	v->m_vecMoveSpeed = CVector( speed[0], speed[1], speed[2] );
	return true;
}

// Read a streamed SA:MP vehicle's live GTA state (position, velocity, health).
bool vehicle_live_data( int sampVid, float pos[3], float speed[3], float &health ) {
	samp::stVehiclePool *vp = samp::vehicle_pool();
	if ( !vp || sampVid < 0 || sampVid >= samp::SAMP_MAX_VEHICLES || !vp->iIsListed[sampVid] )
		return false;
	void *gv = vp->pGTA_Vehicle[sampVid];
	if ( !gv || IsBadReadPtr( gv, 4 ) )
		return false;
	CVehicle *v = reinterpret_cast<CVehicle *>( gv );
	CVector p = v->GetPosition();
	pos[0] = p.x; pos[1] = p.y; pos[2] = p.z;
	speed[0] = v->m_vecMoveSpeed.x; speed[1] = v->m_vecMoveSpeed.y; speed[2] = v->m_vecMoveSpeed.z;
	health = v->m_fHealth;
	return true;
}

// Client-side teleport a streamed SA:MP vehicle + set its velocity + health + pull
// it into our interior (the local half of the car trolls / Fuego cheat_vehicle_teleport).
void teleport_vehicle( int sampVid, const float pos[3], const float speed[3], float health ) {
	traceLastFunc( "game::teleport_vehicle" );
	samp::stVehiclePool *vp = samp::vehicle_pool();
	if ( !vp || sampVid < 0 || sampVid >= samp::SAMP_MAX_VEHICLES || !vp->iIsListed[sampVid] )
		return;
	void *gv = vp->pGTA_Vehicle[sampVid];
	if ( !gv || IsBadReadPtr( gv, 4 ) )
		return;
	CVehicle *v = reinterpret_cast<CVehicle *>( gv );
	v->SetPosn( CVector( pos[0], pos[1], pos[2] ) );
	v->m_vecMoveSpeed = CVector( speed[0], speed[1], speed[2] );
	v->m_fHealth = health;
	if ( CPlayerPed *me = find_player_ped() ) v->SetAreaCode( me->GetAreaCode() ); // our interior
}

// Helper: fetch the live CVehicle for a streamed SA:MP vehicle id (null-safe).
static CVehicle *samp_vehicle( int sampVid ) {
	samp::stVehiclePool *vp = samp::vehicle_pool();
	if ( !vp || sampVid < 0 || sampVid >= samp::SAMP_MAX_VEHICLES || !vp->iIsListed[sampVid] )
		return nullptr;
	void *gv = vp->pGTA_Vehicle[sampVid];
	if ( !gv || IsBadReadPtr( gv, 4 ) )
		return nullptr;
	return reinterpret_cast<CVehicle *>( gv );
}

// Model bounding-box size (width,length,height) + model index (gethere/carpark drop).
bool vehicle_dims_model( int sampVid, float out[3], int &modelIdx ) {
	traceLastFunc( "game::vehicle_dims_model" );
	CVehicle *v = samp_vehicle( sampVid );
	if ( !v ) return false;
	CColModel *cm = v->GetColModel();
	if ( !cm ) return false;
	CVector sz = cm->GetBoundingBox().GetSize();
	out[0] = sz.x; out[1] = sz.y; out[2] = sz.z;
	modelIdx = v->m_nModelIndex;
	return true;
}

// Display name for a streamed SA:MP vehicle (picker hover/confirm label). No hardcoded
// name table -- reads the game's OWN model info: CModelInfo::GetModelInfo(modelIdx) uses
// the real ms_modelInfoPtrs @0xA9B0C8, CVehicleModelInfo::m_szGameName is the GXT key
// ("INFERN"), and the real CText::Get @0xC1B340/0x6A0050 resolves it to "Infernus"
// (GxtChar is 8-bit, so the result is a plain char*). Null if not a real vehicle / no name.
const char *vehicle_model_name( int sampVid ) {
	traceLastFunc( "game::vehicle_model_name" );
	CVehicle *v = samp_vehicle( sampVid );
	if ( !v ) return nullptr;
	int model = v->m_nModelIndex;
	if ( model < 400 || model >= 612 ) return nullptr;        // GTA vehicle model-id range
	CBaseModelInfo *bmi = CModelInfo::GetModelInfo( model );  // real ms_modelInfoPtrs[model]
	if ( !bmi || IsBadReadPtr( bmi, 4 ) ) return nullptr;
	const char *key = bmi->AsVehicleModelInfoPtr()->m_szGameName; // inline char[8] GXT key
	if ( !key[0] ) return nullptr;
	const char *name = mem::thiscall<const char *>( 0x6A0050, reinterpret_cast<void *>( 0xC1B340 ), key );
	if ( !name || IsBadReadPtr( name, 1 ) || !name[0] ) return nullptr;
	return name;
}

// Forward vector + bounding-box width + position (njkill side-offset).
bool vehicle_forward( int sampVid, float fwd[3], float &width, float pos[3] ) {
	traceLastFunc( "game::vehicle_forward" );
	CVehicle *v = samp_vehicle( sampVid );
	if ( !v ) return false;
	CVector f = v->GetMatrix().GetForward();
	fwd[0] = f.x; fwd[1] = f.y; fwd[2] = f.z;
	CVector p = v->GetPosition();
	pos[0] = p.x; pos[1] = p.y; pos[2] = p.z;
	CColModel *cm = v->GetColModel();
	width = cm ? cm->GetBoundingBox().GetWidth() : 2.0f;
	return true;
}

// Spin quaternion for iTroll mode 5. Byte-exact port of Fuego's MatrixToQuaternion
// (math_stuff.cpp): the basis is (-right, -forward, +up) from the vehicle matrix, and
// the output is SA:MP order [W, X, Y, Z] using the same copysign/transpose convention
// Fuego's IDA-derived helper uses (right.X/up.Y/at.Z diagonal). NOT the standard column
// trace method -- that produced different bytes.
bool vehicle_spin_quat( int sampVid, float quat[4] ) {
	traceLastFunc( "game::vehicle_spin_quat" );
	CVehicle *v = samp_vehicle( sampVid );
	if ( !v ) return false;
	CMatrix &m = v->GetMatrix();
	// Fuego: right = -matrix[col0], up = -matrix[col1], at = +matrix[col2].
	// gta-reversed GetRight/GetForward/GetUp map to matrix cols 0/1/2 respectively.
	CVector R = m.GetRight()   * -1.0f; // right  (-col0)
	CVector U = m.GetForward() * -1.0f; // "up"   (-col1)
	CVector A = m.GetUp();              // "at"   (+col2)
	float w = R.x + U.y + A.z + 1.0f; if ( w < 0.0f ) w = 0.0f; w = sqrtf( w ) * 0.5f;
	float x = R.x + 1.0f - U.y - A.z;  if ( x < 0.0f ) x = 0.0f; x = sqrtf( x ) * 0.5f;
	float y = U.y + ( 1.0f - R.x ) - A.z; if ( y < 0.0f ) y = 0.0f; y = sqrtf( y ) * 0.5f;
	float z = ( 1.0f - R.x ) - U.y + A.z; if ( z < 0.0f ) z = 0.0f; z = sqrtf( z ) * 0.5f;
	quat[0] = w;                              // W
	quat[1] = copysignf( x, A.y - U.z );      // X
	quat[2] = copysignf( y, R.z - A.x );      // Y
	quat[3] = copysignf( z, U.x - R.y );      // Z
	return true;
}

// Ground Z below a target ped's live position (iTroll mode 1). Fuego uses
// CWorld::FindGroundZFor3DPosition(ped->GetPosition()); gta-reversed's equivalent is
// FindGroundZFor3DCoord -- a 3D query (uses the Z), unlike the flat FindGroundZForCoord.
bool ped_ground_z( int sampId, float &outZ ) {
	traceLastFunc( "game::ped_ground_z" );
	CPed *p = player_ped( sampId );
	if ( !p ) return false;
	outZ = CWorld::FindGroundZFor3DCoord( p->GetPosition(), nullptr, nullptr );
	return true;
}

// Local player's health + armour (for the bytePlayerHealth/byteArmor fields the
// fake-driver syncs carry).
void local_health_armor( float &hp, float &armor ) {
	CPlayerPed *p = find_player_ped();
	hp    = p ? p->m_fHealth  : 0.0f;
	armor = p ? p->m_fArmour  : 0.0f;
}

// Local vehicle's current health (-1 if on foot) -- for the extra-godmode gate that
// swallows only server vehicle-health *drops* (Fuego compares fHealth < vself->health).
float local_vehicle_health() {
	CVehicle *v = local_vehicle_internal();
	return v ? v->m_fHealth : -1.0f;
}

void set_local_armor( float armor ) {
	if ( CPlayerPed *p = find_player_ped() ) p->m_fArmour = armor;
}

void set_local_movespeed( const float v[3] ) {
	if ( CPlayerPed *p = find_player_ped() ) p->m_vecMoveSpeed = CVector( v[0], v[1], v[2] );
}

// ==== fake-lag backtrack (Fuego 1:1) ======================================
// At enqueue Fuego snapshots the local ped/vehicle's live matrix + position +
// movespeed; BACKSPACE restores the last-sent snapshot. We shuttle the matrix as
// 12 floats [right.xyz, forward.xyz, up.xyz, pos.xyz].
namespace {
bool    g_btStashValid = false; // pending backTrackSetVehicle (+200ms) re-apply
CMatrix g_btMat;
CVector g_btSpeed;

CMatrix mat_from12( const float m[12], float zAdd ) {
	return CMatrix( CVector( m[9], m[10], m[11] + zAdd ),  // pos
	                CVector( m[0], m[1], m[2] ),            // right
	                CVector( m[3], m[4], m[5] ),            // forward
	                CVector( m[6], m[7], m[8] ) );          // up
}
} // namespace

bool fakelag_capture( float mat[12], float speed[3], bool &inVehicle ) {
	CPlayerPed *p = find_player_ped();
	if ( !p ) return false;
	CVehicle *v = p->GetVehicleIfInOne();
	inVehicle = ( v != nullptr );
	CMatrix &m = inVehicle ? v->GetMatrix() : p->GetMatrix();
	CVector r = m.GetRight(), f = m.GetForward(), u = m.GetUp();
	CVector pos = inVehicle ? v->GetPosition() : p->GetPosition();
	mat[0] = r.x; mat[1] = r.y; mat[2] = r.z;
	mat[3] = f.x; mat[4] = f.y; mat[5] = f.z;
	mat[6] = u.x; mat[7] = u.y; mat[8] = u.z;
	mat[9] = pos.x; mat[10] = pos.y; mat[11] = pos.z;
	CVector s = inVehicle ? v->m_vecMoveSpeed : p->m_vecMoveSpeed;
	speed[0] = s.x; speed[1] = s.y; speed[2] = s.z;
	return true;
}

void fakelag_restore_foot( float armor, float health, int weaponId, const float mat[12], const float speed[3] ) {
	traceLastFunc( "game::fakelag_restore_foot" );
	CPlayerPed *p = find_player_ped();
	if ( !p ) return;
	p->m_fArmour = armor;
	p->m_fHealth = health;
	p->SetCurrentWeapon( (eWeaponType) weaponId );      // = SetCurrentWeaponSlot(getWeaponSlot(w))
	CMatrix m = mat_from12( mat, 0.0f );
	p->SetMatrix( m );
	p->m_vecMoveSpeed = CVector( speed[0], speed[1], speed[2] );
}

void fakelag_restore_vehicle( float armor, float health, int weaponId, const float mat[12], const float speed[3], float vehHealth ) {
	traceLastFunc( "game::fakelag_restore_vehicle" );
	CPlayerPed *p = find_player_ped();
	if ( !p ) return;
	CVehicle *v = p->GetVehicleIfInOne();
	if ( !v ) return;
	p->m_fArmour = armor;
	p->m_fHealth = health;
	p->SetCurrentWeapon( (eWeaponType) weaponId );
	v->m_vecMoveSpeed = CVector( 0, 0, 0 );             // Fuego zeroes then re-applies at +200ms
	CMatrix vm = mat_from12( mat, 0.25f );              // Fuego: position.z += 0.25
	v->SetMatrix( vm );
	v->m_fHealth = vehHealth;
	g_btMat = mat_from12( mat, 0.0f );                  // backTrackSetVehicle re-applies orig-z matrix
	g_btSpeed = CVector( speed[0], speed[1], speed[2] );
	g_btStashValid = true;
}

void fakelag_reapply_vehicle() { // backTrackSetVehicle body (fired +200ms after the restore)
	if ( !g_btStashValid ) return;
	g_btStashValid = false;
	CPlayerPed *p = find_player_ped();
	if ( !p ) return;
	CVehicle *v = p->GetVehicleIfInOne();
	if ( !v ) return;
	v->SetMatrix( g_btMat );
	v->m_vecMoveSpeed = g_btSpeed;
}

int locked_vehicle_positions( float out[][3], int maxN ) {
	traceLastFunc( "game::locked_vehicle_positions" );
	CVehiclePool *pool = GetVehiclePool();
	if ( !pool || maxN <= 0 )
		return 0;
	int n = 0;
	for ( CVehicle &v : pool->GetAllValid() ) {
		if ( n >= maxN ) break;
		// Fuego CVehicle::AreDoorsLocked = lock states 2,3,4,5,7 (not just LOCKED + inside).
		if ( v.m_nDoorLock == CARLOCK_LOCKED || v.m_nDoorLock == CARLOCK_LOCKOUT_PLAYER_ONLY ||
		     v.m_nDoorLock == CARLOCK_LOCKED_PLAYER_INSIDE || v.m_nDoorLock == CARLOCK_COP_CAR ||
		     v.m_nDoorLock == CARLOCK_SKIP_SHUT_DOORS ) {
			CVector p = v.GetPosition();
			out[n][0] = p.x; out[n][1] = p.y; out[n][2] = p.z;
			n++;
		}
	}
	return n;
}

// LSRP gun-stealer force-adjust target: where to snap the tracked gun objects. Fuego uses
// the target vehicle occupant's left-knee (first passenger, else driver), or the vehicle
// position + half its height if empty, falling back to our own ped's left-knee.
bool stealguns_target_pos( int sampVid, float out[3] ) {
	traceLastFunc( "game::stealguns_target_pos" );
	CVector target;
	bool have = false;
	if ( CVehicle *veh = samp_vehicle( sampVid ) ) {
		CPed *occ = veh->m_apPassengers[0] ? veh->m_apPassengers[0] : veh->m_pDriver;
		if ( occ && !IsBadReadPtr( occ, 4 ) ) {
			target = occ->GetBonePosition( (eBoneTag) 42 /*L_CALF ~ knee*/, false );
		} else {
			CVector p = veh->GetPosition();
			CColModel *cm = veh->GetColModel();
			float halfH = cm ? cm->GetBoundingBox().GetSize().z / 2.0f : 1.0f;
			target = CVector( p.x, p.y, p.z + halfH );
		}
		have = true;
	}
	if ( !have ) {
		CPlayerPed *self = find_player_ped();
		if ( !self ) return false;
		target = self->GetBonePosition( (eBoneTag) 42, false );
	}
	out[0] = target.x; out[1] = target.y; out[2] = target.z;
	return true;
}

void unlock_local_vehicle() {
	traceLastFunc( "game::unlock_local_vehicle" );
	if ( CVehicle *v = local_vehicle_internal() )
		v->m_nDoorLock = CARLOCK_UNLOCKED;
}

void unlock_vehicle_id( int sampVid ) {
	traceLastFunc( "game::unlock_vehicle_id" );
	if ( CVehicle *v = samp_vehicle( sampVid ) )
		v->m_nDoorLock = CARLOCK_UNLOCKED;
}

// General row-major 4x4 inverse (MESA). false if singular.
static bool invert4x4( const float m[16], float out[16] ) {
	float inv[16];
	inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
	inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
	inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
	inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
	inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
	inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
	inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
	inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
	inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
	inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
	inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
	inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
	inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
	inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
	inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
	inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
	float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
	if ( det == 0.0f ) return false;
	det = 1.0f / det;
	for ( int i = 0; i < 16; i++ ) out[i] = inv[i] * det;
	return true;
}

// Click-to-pick: the streamed SA:MP vehicle directly under the cursor, via Fuego's exact
// method -- CalcWorldCoors (invert the view matrix @0xB6FA2C, scale by the render size
// @0xC17044/48, unproject the cursor to a far world point) + the REAL CWorld::Process-
// LineOfSight @0x56BA00 from the camera. -1 if the ray misses / doesn't hit a vehicle.
int vehicle_under_cursor( float sx, float sy ) {
	traceLastFunc( "game::vehicle_under_cursor" );
	const float *vm = reinterpret_cast<const float *>( 0xB6FA2C ); // D3DXMATRIX (row-major)
	if ( IsBadReadPtr( (void *) vm, 64 ) ) return -1;
	float m[16], inv[16];
	for ( int i = 0; i < 16; i++ ) m[i] = vm[i];
	m[15] = 1.0f; // Fuego sets _44 = 1 before inverting
	if ( !invert4x4( m, inv ) ) return -1;
	int lenX = *reinterpret_cast<const int *>( 0xC17044 );
	int lenY = *reinterpret_cast<const int *>( 0xC17048 );
	if ( lenX <= 0 || lenY <= 0 ) return -1;
	const float z = 700.0f;
	float scaledX = sx * z / (float) lenX;
	float scaledY = sy * z / (float) lenY;
	CVector target( z * inv[8]  + scaledY * inv[4] + scaledX * inv[0] + inv[12],
	                z * inv[9]  + scaledY * inv[5] + scaledX * inv[1] + inv[13],
	                z * inv[10] + scaledY * inv[6] + scaledX * inv[2] + inv[14] );
	CVector *camPos = TheCamera.GetGameCamPosition();
	if ( !camPos ) return -1;
	CVector origin = *camPos;
	CColPoint col{};
	CEntity  *ent = nullptr;
	// Real CWorld::ProcessLineOfSight @0x56BA00, Fuego's flags (buildings, vehicles,
	// !peds, objects, dummies, !seeThrough, !camIgnore, !shootThrough).
	typedef bool( __cdecl * ProcessLOS_t )( const CVector *, const CVector *, CColPoint *, CEntity **,
	                                        bool, bool, bool, bool, bool, bool, bool, bool );
	bool hit = reinterpret_cast<ProcessLOS_t>( 0x56BA00 )( &origin, &target, &col, &ent,
	           true, true, false, true, true, false, false, false );
	if ( !hit || !ent || ent->GetType() != ENTITY_TYPE_VEHICLE ) return -1;
	// Resolve the SA:MP id from the GTA vehicle pointer.
	samp::stVehiclePool *vp = samp::vehicle_pool();
	if ( vp )
		for ( int v = 0; v < samp::SAMP_MAX_VEHICLES; v++ )
			if ( vp->iIsListed[v] && (void *) vp->pGTA_Vehicle[v] == (void *) ent )
				return v;
	return -1;
}

int local_vehicle_samp_id() {
	samp::stLocalPlayer *lp = samp::local_player();
	if ( !lp ) return -1;
	int id = lp->inCarData.sVehicleID;
	return id ? id : -1;
}

int pickup_count() { return (int) CPickups::aPickUps.size(); }

bool pickup_pos( int idx, float out[3], int &modelOut ) {
	if ( idx < 0 || idx >= (int) CPickups::aPickUps.size() )
		return false;
	CPickup &p = CPickups::aPickUps[idx];
	if ( p.m_nPickupType == PICKUP_NONE )
		return false;
	CVector v = p.GetPosn();
	out[0] = v.x; out[1] = v.y; out[2] = v.z;
	modelOut = p.m_nModelIndex;
	return true;
}

int local_weapon_type() {
	CPlayerPed *p = find_player_ped();
	return p ? (int) p->GetActiveWeapon().m_Type : -1;
}

void top_up_clip() {
	traceLastFunc( "game::top_up_clip" );
	CPlayerPed *p = find_player_ped();
	if ( !p ) return;
	CWeapon &w = p->GetActiveWeapon();
	if ( w.m_AmmoInClip < 100 ) w.m_AmmoInClip = 1000; // never drops to a reload
	if ( w.m_TotalAmmo  < 100 ) w.m_TotalAmmo  = 99999;
}

int player_weapon( int sampId ) {
	traceLastFunc( "game::player_weapon" );
	CPed *p = player_ped( sampId );
	return p ? (int) p->GetActiveWeapon().m_Type : -1;
}

// Fill out[13] with each carried weapon type (Fuego: type 16-38 with ammo, or the
// knife); -1 for empty slots. Returns how many were filled.
int player_weapon_slots( int sampId, int out[13] ) {
	traceLastFunc( "game::player_weapon_slots" );
	for ( int s = 0; s < 13; s++ ) out[s] = -1;
	CPed *p = player_ped( sampId );
	if ( !p ) return 0;
	int n = 0;
	for ( int s = 0; s < 13; s++ ) {
		CWeapon &w = p->GetWeaponInSlot( (size_t) s );
		int t = (int) w.m_Type;
		if ( ( t >= 16 && t <= 38 && w.m_TotalAmmo > 0 ) || t == 4 /*WEAPONTYPE_KNIFE*/ ) { out[s] = t; n++; }
	}
	return n;
}

void apply_rotory_pulse( float x, float y, float z ) {
	traceLastFunc( "game::apply_rotory_pulse" );
	CPlayerPed *p = find_player_ped();
	if ( !p ) return;
	// Rotate the input into the ped's local frame (CMatrix operator* == the game's
	// Multiply3x3 @0x59C790, exactly what Fuego used) then add to angular velocity.
	CVector v( x * 0.02f, y * 0.02f, z * 0.02f );
	CVector world = p->GetMatrix() * v;
	p->GetTurnSpeed() += world;
}

void switch_weapon( int weaponId ) {
	traceLastFunc( "game::switch_weapon" );
	if ( CPlayerPed *p = find_player_ped() )
		p->SetCurrentWeapon( (eWeaponType) weaponId );
}

void give_ammo( int amount ) {
	traceLastFunc( "game::give_ammo" );
	if ( CPlayerPed *p = find_player_ped() )
		p->GetActiveWeapon().m_TotalAmmo += amount;
}

void set_local_invulnerable( bool on ) {
	traceLastFunc( "game::set_local_invulnerable" );
	CPlayerPed *p = find_player_ped();
	if ( !p ) return;
	// Fuego's health hack sets the ped's damage-immunity flags (bullet/fire/collision/
	// melee/explosion). Combined with the RPC_ClientCheck flag-strip, godmode is both
	// effective locally and hidden from the server's invulnerability query.
	p->physicalFlags.bBulletProof    = on;
	p->physicalFlags.bFireProof      = on;
	p->physicalFlags.bCollisionProof = on;
	p->physicalFlags.bMeleeProof     = on;
	p->physicalFlags.bExplosionProof = on;
}

// Health hack in a vehicle: Fuego pins the CAR every frame too (cheat_handle_hp) so local
// collision/explosion damage can't wreck it -- the ped flags don't cover the vehicle.
void set_local_vehicle_invulnerable( bool on ) {
	CVehicle *v = local_vehicle_internal();
	if ( !v ) return;
	v->physicalFlags.bBulletProof    = on;
	v->physicalFlags.bFireProof      = on;
	v->physicalFlags.bCollisionProof = on;
	v->physicalFlags.bMeleeProof     = on;
	v->physicalFlags.bExplosionProof = on;
	v->vehicleFlags.bCanBeDamaged = !on; // Fuego: bCanBeDamaged = false while pinned
}

void make_player_controllable() {
	traceLastFunc( "game::make_player_controllable" );
	// CPlayerInfo::MakePlayerSafe(false, 10) @0x56E870 on CWorld::Players @0xB7CD98
	// -- Fuego's TogglePlayerControllable(false): restores control (unfreeze).
	mem::thiscall<void>( 0x56E870, reinterpret_cast<void *>( 0xB7CD98 ), false, 10.0f );
}

CVehicle *local_vehicle_internal() {
	CPlayerPed *p = find_player_ped();
	return p ? p->GetVehicleIfInOne() : nullptr;
}

bool in_vehicle() { return local_vehicle_internal() != nullptr; }

void fly_move( float fwd, float right, float up ) {
	traceLastFunc( "game::fly_move" );
	CPlayerPed *p = find_player_ped();
	if ( !p ) return;
	CVector f = TheCamera.m_mCameraMatrix.GetForward();
	CVector r = TheCamera.m_mCameraMatrix.GetRight();
	CVector pos = p->GetPosition();
	pos.x += f.x * fwd + r.x * right;
	pos.y += f.y * fwd + r.y * right;
	pos.z += f.z * fwd + r.z * right + up;
	p->SetPosn( pos );
	p->m_vecMoveSpeed = CVector( 0, 0, 0 ); // cancel gravity
}

void set_local_collision( bool on ) {
	if ( CPlayerPed *p = find_player_ped() )
		p->SetUsesCollision( on );
}

void freeze_local() {
	if ( CVehicle *v = local_vehicle_internal() )
		v->m_vecMoveSpeed = CVector( 0, 0, 0 );
	else if ( CPlayerPed *p = find_player_ped() )
		p->m_vecMoveSpeed = CVector( 0, 0, 0 );
}

void airbrake_move( float fwd, float right, float up ) {
	traceLastFunc( "game::airbrake_move" );
	CVector f = TheCamera.m_mCameraMatrix.GetForward();
	CVector r = TheCamera.m_mCameraMatrix.GetRight();
	CVector d( f.x * fwd + r.x * right, f.y * fwd + r.y * right, f.z * fwd + r.z * right + up );
	if ( CVehicle *v = local_vehicle_internal() ) {
		CVector pos = v->GetPosition();
		pos.x += d.x; pos.y += d.y; pos.z += d.z;
		v->SetPosn( pos );
		v->m_vecMoveSpeed = CVector( 0, 0, 0 );
	} else if ( CPlayerPed *p = find_player_ped() ) {
		CVector pos = p->GetPosition();
		pos.x += d.x; pos.y += d.y; pos.z += d.z;
		p->SetPosn( pos );
		p->m_vecMoveSpeed = CVector( 0, 0, 0 );
	}
}

	// ==== carcontrol: local ped possession (Fuego GTAfuncs 1:1) ================
	// Fuego's /cc warps the local ped INTO the target's car so the game itself
	// emits that car's in-car sync (you drive their vehicle), then warps back out
	// on release. We build the SAME real game tasks the SA:MP GTA interface uses;
	// their ProcessPed thunks to the real addresses 0x647D10 / 0x64B950.

	namespace {
	// Stash of the possessed vehicle's live transform, re-applied by the delayed
	// carControlSetVehicle step so the warp-in doesn't disturb the car.
	bool    g_ccStashValid = false;
	CMatrix g_ccStashMatrix;
	CVector g_ccStashPos;
	CVector g_ccStashSpeed;

	// Warp the local ped out of whatever car it's in (GTAfunc_PutActorInCar's
	// first half). Same task + flags Fuego uses ("crash without SetKnockedOffBike").
	void cc_warp_out( CPlayerPed *self ) {
		CVehicle *cur = self->GetVehicleIfInOne();
		if ( !cur ) return;
		CTaskSimpleCarSetPedOut out( cur, (eTargetDoor) 1, false );
		out.m_bKnockedOffBike = true;   // Fuego: "illogical, but it'll crash without this"
		out.ProcessPed( self );         // -> real 0x647D10 (instant ejection)
		out.m_bWarpingOutOfCar = true;  // Fuego sets this after ProcessPed (matched verbatim)
	}
	} // namespace

	void cc_possess_driver( int sampVid ) {
		traceLastFunc( "game::cc_possess_driver" );
		CPlayerPed *self = find_player_ped();
		if ( !self ) return;
		cc_warp_out( self );
		CVehicle *veh = samp_vehicle( sampVid );
		if ( !veh ) return;
		CTaskSimpleCarSetPedInAsDriver in( veh, /*warpingInToCar*/ true );
		in.ProcessPed( self );          // -> real 0x64B950 (instant warp into driver seat)
	}

	void cc_possess_passenger( int sampVid, int seat ) {
		traceLastFunc( "game::cc_possess_passenger" );
		CPlayerPed *self = find_player_ped();
		if ( !self ) return;
		cc_warp_out( self );
		CVehicle *veh = samp_vehicle( sampVid );
		if ( !veh ) return;
		CTaskSimpleCarSetPedInAsPassenger in( veh, (eTargetDoor) seat, /*warpingInToCar*/ true );
		in.ProcessPed( self );
	}

	void cc_release_to( const float pos[3] ) {
		traceLastFunc( "game::cc_release_to" );
		CPlayerPed *self = find_player_ped();
		if ( !self ) return;
		cc_warp_out( self );            // GTAfunc_RemoveActorFromCarAndPutAt: warp out ...
		self->Teleport( CVector( pos[0], pos[1], pos[2] ), false ); // ... then teleport
	}

	void cc_camera_on_vehicle( int sampVid ) {
		traceLastFunc( "game::cc_camera_on_vehicle" );
		CVehicle *veh = samp_vehicle( sampVid );
		if ( !veh ) return;
		// GTAfunc_CameraOnVehicle(veh, 2): behind-car, jumpcut switch, script-controlled.
		TheCamera.TakeControl( veh, MODE_BEHINDCAR, eSwitchType::JUMPCUT, 1 );
	}

	void cc_camera_restore() {
		traceLastFunc( "game::cc_camera_restore" );
		TheCamera.Restore();
		TheCamera.RestoreWithJumpCut();
	}

	void cc_capture_vehicle( int sampVid ) {
		CVehicle *veh = samp_vehicle( sampVid );
		if ( !veh ) { g_ccStashValid = false; return; }
		g_ccStashMatrix = veh->GetMatrix();
		g_ccStashPos    = veh->GetPosition();
		g_ccStashSpeed  = veh->m_vecMoveSpeed;
		g_ccStashValid  = true;
	}

	void cc_reapply_vehicle( int sampVid ) {
		traceLastFunc( "game::cc_reapply_vehicle" );
		if ( !g_ccStashValid ) return;
		CVehicle *veh = samp_vehicle( sampVid );
		if ( !veh ) return;
		veh->SetMatrix( g_ccStashMatrix ); // carControlSetVehicle: restore the car's transform
		veh->SetPosn( g_ccStashPos );
		veh->m_vecMoveSpeed = g_ccStashSpeed;
	}

	int cc_local_seat() {
		CPlayerPed *self = find_player_ped();
		if ( !self ) return -1;
		CVehicle *v = self->GetVehicleIfInOne();
		if ( !v ) return -1;
		return v->GetDriver() == self ? 0 : 1;
	}

	int cc_local_vehicle_id() {
		// SA:MP id of the car the local ped is physically in, resolved from the GTA
		// vehicle pointer (Fuego getSAMPVehicleIDFromGTAVehicleEx) -- works for a
		// passenger / a freshly-warped seat, unlike the driver-only in-car sync.
		CVehicle *mine = local_vehicle_internal();
		if ( !mine ) return -1;
		samp::stVehiclePool *vp = samp::vehicle_pool();
		if ( !vp ) return -1;
		for ( int v = 0; v < samp::SAMP_MAX_VEHICLES; v++ ) {
			if ( vp->iIsListed[v] && (void *) vp->pGTA_Vehicle[v] == (void *) mine )
				return v;
		}
		return -1;
	}

	// Fuego getVehicleEmptySeat(veh, passengerOnly): m_pDriver + m_apPassengers are
	// contiguous, so index 0 = driver seat, 1.. = passengers. passengerOnly skips seat
	// 0; either way a result of 0 is rejected (-1). carcontrol passes true; the itroll/
	// fuckup/gun-stealer passenger syncs pass false (Fuego's one-arg default).
	int vehicle_empty_seat( int sampVid, bool passengerOnly ) {
		CVehicle *veh = samp_vehicle( sampVid );
		if ( !veh ) return -1;
		CPed **seats = &veh->m_pDriver;
		int seat = -1;
		for ( int i = 0; i < veh->m_nMaxPassengers; i++ ) {
			if ( i == 0 && passengerOnly ) continue;
			if ( seats[i] == nullptr ) { seat = i; break; }
		}
		if ( seat == 0 ) return -1;
		return seat;
	}

	int cc_vehicle_empty_seat( int sampVid ) { return vehicle_empty_seat( sampVid, true ); }

	int cc_nearest_empty_vehicle( float radius ) {
		traceLastFunc( "game::cc_nearest_empty_vehicle" );
		CPlayerPed *self = find_player_ped();
		if ( !self ) return -1;
		CVector selfPos = self->GetPosition();
		samp::stVehiclePool *vp = samp::vehicle_pool();
		if ( !vp ) return -1;
		float best = 69420.0f;
		int   bestId = -1;
		for ( int v = 0; v < samp::SAMP_MAX_VEHICLES; v++ ) {
			if ( !vp->iIsListed[v] ) continue;
			CVehicle *veh = samp_vehicle( v );
			if ( !veh ) continue;
			CVector vpos = veh->GetPosition();
			// Fuego: skip if any seat (driver or passenger) is occupied.
			CPed **seats = &veh->m_pDriver;
			bool occupied = false;
			for ( int i = 0; i < veh->m_nMaxPassengers; i++ )
				if ( seats[i] ) { occupied = true; break; }
			if ( occupied ) continue;
			// Fuego: skip if any streamed remote player is within `radius` of the car.
			bool nearPlayer = false;
			for ( int p = 0; p < samp::SAMP_MAX_PLAYERS; p++ ) {
				if ( !player_streamed( p ) ) continue;
				float pp[3];
				if ( !player_world_pos( p, pp ) ) continue;
				float dx = vpos.x - pp[0], dy = vpos.y - pp[1], dz = vpos.z - pp[2];
				if ( sqrtf( dx * dx + dy * dy + dz * dz ) <= radius ) { nearPlayer = true; break; }
			}
			if ( nearPlayer ) continue;
			float dx = vpos.x - selfPos.x, dy = vpos.y - selfPos.y, dz = vpos.z - selfPos.z;
			float d = sqrtf( dx * dx + dy * dy + dz * dz );
			if ( d <= best ) { best = d; bestId = v; }
		}
		return bestId;
	}

	void cc_vehicle_collision_proof( int sampVid, bool on ) {
		// Closest available stand-in for Fuego's global nocols hook: keep the car
		// being possessed from colliding/exploding across the 1.5s warp window.
		if ( CVehicle *v = samp_vehicle( sampVid ) )
			v->physicalFlags.bCollisionProof = on;
	}

} // namespace game
