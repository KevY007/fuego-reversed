#pragma once
/*
	cheats/veh.h  --  one-shot vehicle actions (the Fuego menu buttons).
*/

namespace veh {
void destroy_doors();
void attach_vehicle_id( int vid ); // /attveh <id>: tow that streamed vehicle to ours
void unlock_vehicle();

// Click-to-pick a vehicle in the world (Fuego's clickUnlock/clickDamageDoors/clickAttach).
// action: 1 = unlock, 2 = damage doors, 3 = destroy, 4 = tow to our vehicle. Menu drives run_picker().
void start_pick( int action );
bool is_picking();
void run_picker( float curX, float curY, bool clicked, bool cancel );
}
