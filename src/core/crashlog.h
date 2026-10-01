#pragma once
/*
	core/crashlog.h  --  crash diagnostics + guards for known GTA crashes.

	Installs a vectored exception handler that, on any serious fault, writes the
	faulting address (resolved to module+offset), the access type/target, the
	registers, the return addresses on the stack (the call chain, as module+offset)
	and the last traceLastFunc() breadcrumb to the log -- so an in-game crash tells us
	exactly where it happened. Does NOT swallow the crash (returns CONTINUE_SEARCH), so
	the normal crash flow still proceeds.

	install_guards() turns known GTA crashes into a skipped draw (see crashlog.cpp);
	call it once on the game's render thread (the first frame), never from another
	thread -- it patches code GTA runs.
*/

namespace crashlog {
void install();
void install_guards();
}
