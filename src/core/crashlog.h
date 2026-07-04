#pragma once
/*
	core/crashlog.h  --  crash diagnostics.

	Installs a vectored exception handler that, on any serious fault, writes the
	faulting address (resolved to module+offset), the access type/target, and the
	last traceLastFunc() breadcrumb to the log -- so an in-game crash tells us
	exactly where it happened. Does NOT swallow the crash (returns
	CONTINUE_SEARCH), so the normal crash flow still proceeds.
*/

namespace crashlog {
void install();
}
