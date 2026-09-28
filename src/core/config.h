#pragma once
/*
	core/config.h  --  persist cheat settings to fuego_reversed.cfg.

	Binary dump of fx::Settings guarded by a magic + size, so an older/newer cfg
	from a different build is simply ignored (defaults kept). Transient fields
	(current aim target, panic) are reset on load.
*/

namespace cfg {
bool save(); // false if the file couldn't be written
void load();
}
