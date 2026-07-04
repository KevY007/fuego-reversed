#pragma once
/*
	cheats/chams.h  --  player chams (see-through-wall solid/wireframe overlay).

	Hooks IDirect3DDevice9::DrawIndexedPrimitive on the game device and, for ped
	draws (identified by the GTA render call-site return address 0x761142), renders
	each ped twice with a flat-colour pixel shader: once behind walls (z-test off,
	blue) and once in front (z-test on, green). 1:1 with Fuego's DrawIndexedPrimitive
	chams. Armed lazily on the first frame (needs the live device for the shaders).
*/

namespace chams {
void install();
}
