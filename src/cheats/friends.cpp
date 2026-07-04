#define _CRT_SECURE_NO_WARNINGS 1
#include "friends.h"
#include "../core/logger.h"

#include <set>
#include <cstdio>

namespace {
std::set<std::string> g_friends;
const char           *kPath = "fuego_friends.txt";

void save_file() {
	FILE *f = fopen( kPath, "w" );
	if ( !f ) return;
	for ( const auto &n : g_friends )
		fprintf( f, "%s\n", n.c_str() );
	fclose( f );
}
} // namespace

namespace friends {

void load() {
	FILE *f = fopen( kPath, "r" );
	if ( !f ) return;
	char line[128];
	while ( fgets( line, sizeof( line ), f ) ) {
		std::string s( line );
		while ( !s.empty() && ( s.back() == '\n' || s.back() == '\r' || s.back() == ' ' ) )
			s.pop_back();
		if ( !s.empty() )
			g_friends.insert( s );
	}
	fclose( f );
	Log( "[friends] loaded %d.", (int) g_friends.size() );
}

bool is_friend( const std::string &name ) { return g_friends.count( name ) > 0; }

bool add( const std::string &name ) {
	if ( name.empty() ) return false;
	bool inserted = g_friends.insert( name ).second;
	if ( inserted ) save_file();
	return inserted;
}

bool remove( const std::string &name ) {
	bool erased = g_friends.erase( name ) > 0;
	if ( erased ) save_file();
	return erased;
}

int count() { return (int) g_friends.size(); }

void each( void ( *cb )( const char * ) ) {
	for ( const auto &n : g_friends )
		cb( n.c_str() );
}

} // namespace friends
