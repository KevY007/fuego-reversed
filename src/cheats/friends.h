#pragma once
/*
	cheats/friends.h  --  friends list (names) for the aim friend-filter.
	Persisted to fuego_friends.txt.
*/

#include <string>

namespace friends {
void load();
bool is_friend( const std::string &name );
bool add( const std::string &name );
bool remove( const std::string &name );
int  count();
// Call cb(name) for each friend (for listing).
void each( void ( *cb )( const char * ) );
}
