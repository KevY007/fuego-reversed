#include "checkpoints.h"

#include <cstring>

namespace checkpoints {

namespace {

struct Spot {
	bool     on = false;
	float    pos[3] = { 0, 0, 0 };
	unsigned order = 0; // when it was set: the newest one wins
};

Spot     g_checkpoint, g_race;
unsigned g_order = 0;

Spot &spot( Kind kind ) { return kind == kRace ? g_race : g_checkpoint; }

// Three raw little-endian floats at byte `at` (RakNet's BitStream reads floats as
// raw bytes, no swap -- the same as BitStream::Read( float ) from that offset).
bool read_xyz( const unsigned char *data, unsigned bits, unsigned at, float pos[3] ) {
	if ( !data || bits < ( at + 12 ) * 8 ) return false;
	std::memcpy( pos, data + at, 12 );
	return true;
}

} // namespace

bool read_checkpoint( const unsigned char *data, unsigned bits, float pos[3] ) {
	return read_xyz( data, bits, 0, pos ); // x, y, z, radius
}

bool read_race_checkpoint( const unsigned char *data, unsigned bits, float pos[3] ) {
	return read_xyz( data, bits, 1, pos ); // u8 type, then x, y, z, next x/y/z, radius
}

void set( Kind kind, float x, float y, float z ) {
	if ( kind != kCheckpoint && kind != kRace ) return;
	Spot &s = spot( kind );
	s.on     = true;
	s.pos[0] = x; s.pos[1] = y; s.pos[2] = z;
	s.order  = ++g_order;
}

void clear( Kind kind ) {
	if ( kind != kCheckpoint && kind != kRace ) return;
	spot( kind ).on = false;
}

void reset() {
	g_checkpoint.on = false;
	g_race.on       = false;
}

Kind active( float pos[3] ) {
	const Spot *best = nullptr;
	Kind kind = kNone;
	if ( g_checkpoint.on ) { best = &g_checkpoint; kind = kCheckpoint; }
	if ( g_race.on && ( !best || g_race.order > best->order ) ) { best = &g_race; kind = kRace; }
	if ( best ) std::memcpy( pos, best->pos, 12 );
	return kind;
}

} // namespace checkpoints
