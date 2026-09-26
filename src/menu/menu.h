#pragma once
/*
	menu/menu.h  --  in-house overlay menu (Dear ImGui, DX9, compiled-in).

	Registers on the render hook. Toggle with F2. No external DLL.
*/

namespace menu {
void install();          // register ImGui with the render hook
bool is_open();          // menu currently visible (cheats can pause while it's open)
void set_open( bool on ); // open/close programmatically (panic closes + later restores it)
}
