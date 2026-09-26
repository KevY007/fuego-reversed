#pragma once
/*
	gta/gta_reversed.h  --  ONE include for the gta-reversed game-class library.

	================================  C++23 TUs ONLY  ==========================
	gta-reversed's headers require C++23 (/std:c++latest, /Zc:preprocessor) plus
	the gta-reversed include roots + defines. That environment is set up per-file
	in FuegoReversed.vcxproj for gtahost.cpp. Do NOT include this from a normal
	C++20 TU -- it will not compile there. Those TUs reach the game through
	gta/gtahost.h (the lean accessor bridge) instead.

	To use the raw game classes (CPed, CVehicle, CWorld, CPools, CWeapon, ...) in
	NEW code:
	  1. Add your .cpp to the vcxproj with the SAME per-file block gtahost.cpp has:
	         <LanguageStandard>stdcpplatest</LanguageStandard>
	         <AdditionalOptions>/Zc:preprocessor %(AdditionalOptions)</AdditionalOptions>
	         <AdditionalIncludeDirectories>$(GtaReversedIncludes);...
	         <PreprocessorDefinitions>$(GtaReversedDefines);...
	  2. #include "gta_reversed.h" at the top.
	  3. Bridge LIVE game objects the way gtahost.cpp does -- take the pointer from
	     the SA:MP pool (pGTA_Ped / pGTA_Vehicle) or call FindPlayerPed through
	     mem::ccall, then reinterpret_cast to the gta-reversed class. gta-reversed's
	     layouts are address-accurate, so the cast object reads the real game state.

	This umbrella just gathers the headers you almost always need; include any other
	game_sa header directly (all of game_sa's subdirs are on the include path).
*/

#include "StdInc.h"       // RenderWare-ordered core: CVector, CMatrix, RwXxx, globals

// --- entity hierarchy -------------------------------------------------------
#include "Entity.h"       // CEntity           (base: matrix, model, placement)
#include "Physical.h"     // CPhysical         (m_vecMoveSpeed / m_vecTurnSpeed / mass)
#include "Ped.h"          // CPed              (health, weapons, tasks, bones)
#include "PlayerPed.h"    // CPlayerPed        (the local player ped)
#include "Vehicle.h"      // CVehicle          (health, doors, driver, handling)
#include "Automobile.h"   // CAutomobile       (cars specifically)
#include "Object.h"       // CObject           (world objects)

// --- world / pools / core systems -------------------------------------------
#include "World.h"        // CWorld            (FindGroundZ, entity queries)
#include "Pools/Pools.h"  // CPools            (GetPedPool / GetVehiclePool / ...)
#include "Camera.h"       // TheCamera         (matrices, look direction)
#include "Weapon.h"       // CWeapon           (m_Type, ammo, fire)
#include "Pickups.h"      // CPickups          (aPickUps)
#include "Sprite.h"       // CSprite           (CalcScreenCoors: world -> screen)
