#pragma once
/*
	cheats/checkpoints.h  --  where the server's checkpoints are (for /triggercp).

	SA:MP has NO client RPC for entering a checkpoint (the client->server RPC list
	has PickedUpPickup, but nothing for checkpoints). The server decides it: on every
	sync packet it receives (on foot, driving, passenger) it compares our position
	with the checkpoint's radius and fires OnPlayerEnterCheckpoint /
	OnPlayerEnterRaceCheckpoint on the outside -> inside step. So entering one means
	sending a sync from its position -- cheats::trigger_checkpoint().

	The positions come from the server's own RPCs: SetCheckpoint (107: x, y, z,
	radius) and SetRaceCheckpoint (38: u8 type, x, y, z, next x/y/z, radius);
	DisableCheckpoint (37) / DisableRaceCheckpoint (39) remove them, and a new
	session (InitGame / GameModeRestart) forgets both. Pure bookkeeping, no game or
	network access: host-tested in tests/checkpoints_test.cpp.
*/

namespace checkpoints {

enum Kind { kNone = 0, kCheckpoint = 1, kRace = 2 };

// The position in an RPC's raw payload (as the server wrote it); false if it's too
// short to hold one.
bool read_checkpoint( const unsigned char *data, unsigned bits, float pos[3] );      // SetCheckpoint (107)
bool read_race_checkpoint( const unsigned char *data, unsigned bits, float pos[3] ); // SetRaceCheckpoint (38)

void set( Kind kind, float x, float y, float z ); // the server put one there (107 / 38)
void clear( Kind kind );                          // ... and removed it (37 / 39)
void reset();                                     // a new session: forget both

// The checkpoint to enter: the most recently set one that is still there (a server
// rarely shows both kinds at once). kNone if there is none.
Kind active( float pos[3] );

} // namespace checkpoints
