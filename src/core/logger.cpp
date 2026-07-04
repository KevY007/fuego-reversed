#define _CRT_SECURE_NO_WARNINGS 1
#include "logger.h"

#include <cstdio>
#include <cstdarg>
#include <windows.h>

namespace {
const char       *kLogPath = "fuego_reversed.log";
CRITICAL_SECTION  g_cs;
bool              g_ready = false;
}

void LogInit() {
	InitializeCriticalSection( &g_cs );
	g_ready = true;
	FILE *f = std::fopen( kLogPath, "w" );
	if ( f ) {
		std::fputs( "=== Fuego Reversed ===\n", f );
		std::fclose( f );
	}
}

void Log( const char *fmt, ... ) {
	if ( g_ready ) EnterCriticalSection( &g_cs );
	FILE *f = std::fopen( kLogPath, "a" );
	if ( f ) {
		std::va_list ap;
		va_start( ap, fmt );
		std::vfprintf( f, fmt, ap );
		va_end( ap );
		std::fputc( '\n', f );
		std::fclose( f );
	}
	if ( g_ready ) LeaveCriticalSection( &g_cs );
}
