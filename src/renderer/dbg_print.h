#pragma once

// Tiny portable debug-print helper. On Windows it routes to the
// debugger's output pane via OutputDebugStringA; everywhere else it
// fputs to stderr. Used for occasional renderer diagnostics that
// shouldn't go through the normal logging path.

#include <cstdio>

#if defined(_WIN32)
extern "C" __declspec(dllimport) void __stdcall OutputDebugStringA(const char* s);
#endif

namespace whiteout::flakes::renderer {

inline void DbgPrint(const char* msg) {
    if (!msg) return;
#if defined(_WIN32)
    OutputDebugStringA(msg);
#else
    std::fputs(msg, stderr);
#endif
}

}
