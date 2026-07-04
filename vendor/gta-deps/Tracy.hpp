#pragma once
// Minimal no-op Tracy stub. gta-reversed's game_sa layer only uses the scoped
// zone macros for profiling; fuego-reversed doesn't profile, so these compile to
// nothing. Keeps us from vendoring the full Tracy client.
#define ZoneScoped
#define ZoneScopedN(x)
#define ZoneScopedC(x)
#define ZoneScopedNC(x, y)
#define ZoneText(x, y)
#define ZoneName(x, y)
#define ZoneValue(x)
#define ZoneNamed(x, y)
#define ZoneNamedN(x, y, z)
#define FrameMark
#define FrameMarkNamed(x)
#define FrameMarkStart(x)
#define FrameMarkEnd(x)
#define TracyMessage(x, y)
#define TracyMessageL(x)
#define TracyPlot(x, y)
#define TracyAppInfo(x, y)
#define TracyCZoneCtx int
