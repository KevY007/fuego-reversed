#define _CRT_SECURE_NO_WARNINGS 1
#include "config.h"
#include "settings.h"
#include "logger.h"
#include "trace.h"
#include "../i18n/config.h"   // unified config: translator settings share this file

#include <cstdio>
#include <cstdint>
#include <string>

namespace {
const char    *kPath  = "fuego_reversed.cfg";
const uint32_t kMagic = 0x47455546; // "FUEG"
}

namespace cfg {

bool save() {
	traceLastFunc( "cfg::save" );
	FILE *f = fopen( kPath, "wb" );
	if ( !f ) { Log( "[cfg] save failed (open)." ); return false; }
	uint32_t magic = kMagic, size = (uint32_t) sizeof( fx::Settings );
	fwrite( &magic, 4, 1, f );
	fwrite( &size, 4, 1, f );
	fwrite( &fx::g_set, sizeof( fx::Settings ), 1, f );
	// Unified config: the translator settings (text key=value) live in the SAME
	// file, right after the fixed-size binary block. This can include an
	// api_key, which is why fuego_reversed.cfg is gitignored.
	std::string i18nText = i18n::config::to_text();
	fwrite( i18nText.data(), 1, i18nText.size(), f );
	const bool written = !ferror( f );
	if ( fclose( f ) != 0 || !written ) { Log( "[cfg] save failed (write)." ); return false; }
	Log( "[cfg] saved (%u binary + %u text bytes).", size, (unsigned) i18nText.size() );
	return true;
}

void load() {
	traceLastFunc( "cfg::load" );
	i18n::config::mark_saved(); // the translator's baseline: its defaults, until the file says otherwise
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
			// Unified config: whatever follows the binary block is the
			// translator text section (key=value lines) -> hand it to i18n.
			std::string i18nText;
			char buf[1024];
			size_t n;
			while ( ( n = fread( buf, 1, sizeof buf, f ) ) > 0 )
				i18nText.append( buf, n );
			if ( !i18nText.empty() ) {
				i18n::config::from_text( i18nText );
				i18n::config::mark_saved();
			}
			Log( "[cfg] loaded." );
		}
	} else {
		Log( "[cfg] ignored (magic/size mismatch -- different build)." );
	}
	fclose( f );
}

} // namespace cfg
