#pragma once
/*
	cheats/veh.h  --  one-shot vehicle actions (the Fuego menu buttons).
*/

namespace veh {
void destroy_doors();
void attach_trailer();
void unlock_vehicle();

// Click-to-pick a vehicle in the world (Fuego's clickUnlock/clickDamageDoors picker).
// action: 1 = unlock, 2 = damage doors, 3 = destroy. The menu drives run_picker().
void start_pick( int action );
bool is_picking();
void run_picker( float curX, float curY, bool clicked, bool cancel );
}
