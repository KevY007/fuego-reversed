#define _CRT_SECURE_NO_WARNINGS 1
#include "config.h"
#include "settings.h"
#include "logger.h"
#include "trace.h"

#include <cstdio>
#include <cstdint>

namespace {
const char    *kPath  = "fuego_reversed.cfg";
const uint32_t kMagic = 0x47455546; // "FUEG"
}

namespace cfg {

void save() {
	traceLastFunc( "cfg::save" );
	FILE *f = fopen( kPath, "wb" );
	if ( !f ) { Log( "[cfg] save failed (open)." ); return; }
	uint32_t magic = kMagic, size = (uint32_t) sizeof( fx::Settings );
	fwrite( &magic, 4, 1, f );
	fwrite( &size, 4, 1, f );
	fwrite( &fx::g_set, sizeof( fx::Settings ), 1, f );
	fclose( f );
	Log( "[cfg] saved (%u bytes).", size );
}

void load() {
	traceLastFunc( "cfg::load" );
	FILE *f = fopen( kPath, "rb" );
	if ( !f ) return; // no config yet -> keep defaults
	uint32_t magic = 0, size = 0;
	if ( fread( &magic, 4, 1, f ) == 1 && fread( &size, 4, 1, f ) == 1 &&
	     magic == kMagic && size == sizeof( fx::Settings ) ) {
		fx::Settings tmp;
		if ( fread( &tmp, sizeof( fx::Settings ), 1, f ) == 1 ) {
			fx::g_set = tmp;
			fx::g_set.silent_aim_target = -1;   // transient
			fx::g_set.cheat_panic       = false; // don't persist panic
			Log( "[cfg] loaded." );
		}
	} else {
		Log( "[cfg] ignored (magic/size mismatch -- different build)." );
	}
	fclose( f );
}

} // namespace cfg
