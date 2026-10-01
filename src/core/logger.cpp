#define _CRT_SECURE_NO_WARNINGS 1
#include "logger.h"

#include <cstdio>
#include <cstdarg>
#include <windows.h>

namespace {
const char       *kLogPath    = "fuego_reversed.log";
const char       *kOldLogPath = "fuego_reversed.old.log";
// A session's log is moved aside once it passes this, so it can never grow without
// bound (a debug option left on once made it 7+ MB): the newest lines -- what a crash
// report needs -- are always in fuego_reversed.log, the earlier ones in .old.log.
constexpr long    kRotateBytes = 32L * 1024 * 1024;
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
		const long size = std::ftell( f );
		std::fclose( f );
		if ( size > kRotateBytes ) {
			// Move it aside in one step (replacing an older .old.log); only then start a
			// fresh log. If that fails (the file held open elsewhere), both files stay as
			// they are and it keeps appending.
			if ( MoveFileExA( kLogPath, kOldLogPath, MOVEFILE_REPLACE_EXISTING ) ) {
				if ( FILE *n = std::fopen( kLogPath, "w" ) ) {
					std::fputs( "=== Fuego Reversed (continued -- the earlier part is in fuego_reversed.old.log) ===\n", n );
					std::fclose( n );
				}
			}
		}
	}
	if ( g_ready ) LeaveCriticalSection( &g_cs );
}
