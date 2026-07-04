// gta/gtastubs.cpp -- stubs for gta-reversed symbols that get pulled into the link
// but live in code fuego NEVER drives (its engine loop, windowing, and imgui-docking
// debug UI). The CPlayerPed hierarchy's transitive closure reaches CGame/CPad/audio
// and gta-reversed's UIRenderer; those reference entry-point + docking-imgui symbols
// absent from our build. Trivial definitions satisfy the linker without pulling
// gta-reversed's real windowing/DVD/gamma code or a second (docking) imgui.
//
// None of these are ever called: we only read game DATA (position/health) via the
// bridge; we don't run gta-reversed's app, audio, or debug-menu render paths.

// --- app / platform (from the excluded WinMain/WinPlatform entry points) ---
void  MessageLoop() {}
bool  IsForegroundApp() { return true; }
char *getDvdGamePath() { return nullptr; }
void  ResetGammaWhenExiting() {}

// --- imgui docking / viewport / demo (referenced only by gta-reversed's debug UI) ---
// fuego ships a non-docking imgui, so these are missing; no-ops match the symbols
// gta-reversed's imgui_impl_sdl3 / UIRenderer / ImGuiDebugModule expect.
#include "imgui.h"

namespace ImGui {
void           UpdatePlatformWindows() {}
void           RenderPlatformWindowsDefault( void *, void * ) {}
void           DestroyPlatformWindows() {}
ImGuiViewport *FindViewportByID( ImGuiID ) { return nullptr; }
ImGuiViewport *FindViewportByPlatformHandle( void * ) { return nullptr; }
void           ShowDemoWindow( bool * ) {}
void           ShowAboutWindow( bool * ) {}
} // namespace ImGui

void ImGuiIO::AddMouseViewportEvent( ImGuiID ) {}
