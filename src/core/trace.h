#pragma once
/*
	core/trace.h  --  last-function tracer for crash diagnostics.

	Ported code (e.g. the RakNet thunks) calls traceLastFunc("..."); the crash
	handler reads getLastFunc() to report where we were.
*/

void traceLastFunc( const char *fn );
const char *getLastFunc();
