#pragma once
/*
	menu/menu.h  --  in-house overlay menu (Dear ImGui, DX9, compiled-in).

	Registers on the render hook. Toggle with INSERT. No external DLL.
*/

namespace menu {
void install();      // register ImGui with the render hook
bool is_open();      // menu currently visible (cheats can pause while it's open)
}
