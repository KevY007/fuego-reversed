#include "trace.h"

namespace { const char *g_lastFunc = "(none)"; }

void traceLastFunc( const char *fn ) { g_lastFunc = fn; }
const char *getLastFunc() { return g_lastFunc; }
