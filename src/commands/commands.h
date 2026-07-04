#pragma once
/*
	commands/commands.h  --  client chat-command registration.

	Wraps SA:MP's native CInput::AddClientCommand, so a typed /name is routed by
	SA:MP itself to our handler (same mechanism Project Fuego used). Handlers get
	the raw text after the command name.
*/

namespace cmd {

// SA:MP command handler: receives the text after "/name".
using Handler = void( __cdecl * )( char *params );

// Register a command (name WITHOUT the leading '/'). hidden => omit from /fuego_cmds.
void add( const char *name, Handler handler, bool hidden = false );

// Register the built-in commands (/compat, /fuego_cmds). Call once, in-game.
void install();

} // namespace cmd
