// wf_web_minimal.cpp — isolation harness.
//
// Links ONLY the Emscripten runtime + emdawnwebgpu glue, with the same
// `wf_*` export shape so wf-viewer.js can attempt `wf_create()` against
// it. If V8 still freezes compiling this binary, our renderer code is
// not the cause; the freeze is in Emscripten's runtime / emdawnwebgpu
// bindings. If this loads cleanly, our renderer triggers the freeze and
// we have a target to bisect.

#include <cstdint>
#include <cstdio>
#include <new>

namespace { struct Handle { int v = 0; }; }

extern "C" {
Handle* wf_create()                                          { return new (std::nothrow) Handle{}; }
void    wf_destroy(Handle* h)                                { delete h; }
int     wf_init(Handle*, const char*, int, int)              { std::printf("[wf-min] init\n"); return 1; }
void    wf_resize(Handle*, int, int)                         {}
void    wf_tick(Handle*, float)                              {}
void    wf_render(Handle*)                                   {}
void    wf_set_background(Handle*, int, int, int)            {}
const char* wf_last_error()                                  { return ""; }
}
