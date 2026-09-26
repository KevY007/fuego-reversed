// gta/gtastubs.cpp -- stubs for gta-reversed symbols that get pulled into the link
// but live in code fuego NEVER drives. scripts\bootstrap.ps1 leaves upstream's entry
// point TUs (dllmain.cpp, app/platform/win/WinMain.cpp, WinPlatform.cpp) out of
// gta_reversed.lib -- we coexist with the running game, we don't replace its
// WinMain/DllMain -- yet the rest of the library references a few globals from them.
// Trivial definitions satisfy the linker; none of these is ever called (we only read
// game data and call leaf helpers; gta-reversed's app loop, audio streaming and
// windowing paths never run).

// --- app / platform (from the excluded WinMain/WinPlatform entry points) ---
void  MessageLoop() {}
bool  IsForegroundApp() { return true; }
char *getDvdGamePath() { return nullptr; }
void  ResetGammaWhenExiting() {}
