#pragma once
/*
	core/logger.h  --  minimal file logger -> fuego_reversed.log

	Free functions (not a namespace) to avoid clashing with <cmath>'s ::log and
	to match the Log(...) convention used throughout the ported Fuego code.
*/

void LogInit();
void Log( const char *fmt, ... );
