# ============================================================================
# wf_web — WhiteoutFlakes browser host (Emscripten / WebAssembly + WebGPU)
#
# Builds an ES module `wf-core.js` + companion `wf-core.wasm` that
# `tools/web_viewer/wf-viewer.js` instantiates from a web page. The C
# entry points live in `wf_web_api.cpp`; the JS device handoff,
# canvas/event wiring, and asset prefetch live in `wf-viewer.js`.
#
# This file is only entered from the top-level CMakeLists.txt under
# `if(EMSCRIPTEN)` — it has no desktop equivalent.
# ============================================================================

add_executable(wf_web
    wf_web_api.cpp
)

set_target_properties(wf_web PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME           "wf-core"
    SUFFIX                ".js"  # Emscripten produces wf-core.js + wf-core.wasm
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web"
)

target_link_libraries(wf_web PRIVATE WhiteoutFlakesLib)

# Emscripten link options. emdawnwebgpu mirrors the Dawn webgpu.h surface
# the gfx backend already targets — Phase 1 adds the few #ifdef branches
# in webgpu_init.cpp / webgpu_swap_chain.cpp for JS device handoff and
# canvas-selector surface creation. The wf_web target carries the link
# flag; the gfx target carries the compile flag (set in the parent
# CMakeLists' WDX_ENABLE_WEBGPU/EMSCRIPTEN branch).
target_link_options(wf_web PRIVATE
    --use-port=emdawnwebgpu
    --pre-js "${CMAKE_CURRENT_SOURCE_DIR}/pre.js"
    -sWASM=1
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sENVIRONMENT=web
    -sALLOW_MEMORY_GROWTH=1
    # getExceptionMessage is auto-pulled by exception catching; with
    # DISABLE_EXCEPTION_CATCHING=1 it doesn't exist. Our facade uses
    # wf_last_error() (a stored what() string) instead.
    -sEXPORTED_RUNTIME_METHODS=['cwrap','ccall','HEAPU8','HEAPF32','stringToUTF8','UTF8ToString','lengthBytesUTF8']
    "-sEXPORTED_FUNCTIONS=@${CMAKE_CURRENT_SOURCE_DIR}/exports.txt"
    # Exceptions: see top-level CMakeLists.txt EMSCRIPTEN block.
    -fno-exceptions
    # -Oz: see top-level CMakeLists.txt rationale (avoids V8 sync-compile
    # by keeping functions small enough that TurboFan can stay async).
    -Oz
    # -g1 keeps the wasm name section (function + global names) without
    # the DWARF blowup of -g2 — useful while we're still bisecting size.
    -g1
)
target_compile_options(wf_web PRIVATE -fno-exceptions -Oz -g1)

# ----------------------------------------------------------------------------
# wf_web_minimal — isolation harness. Same Emscripten flags and same
# wf_* export shape as wf_web, but NO WhiteoutFlakes libraries linked in.
# Used to test whether V8 freezes on the Emscripten + emdawnwebgpu glue
# alone, or only when our renderer code is included.
# ----------------------------------------------------------------------------
add_executable(wf_web_minimal wf_web_minimal.cpp)
set_target_properties(wf_web_minimal PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME           "wf-core-min"
    SUFFIX                ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web"
)
target_link_options(wf_web_minimal PRIVATE
    --use-port=emdawnwebgpu
    -sWASM=1 -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web
    -sALLOW_MEMORY_GROWTH=1
    -sEXPORTED_RUNTIME_METHODS=['cwrap','ccall','HEAPU8','HEAPF32','stringToUTF8','UTF8ToString','lengthBytesUTF8']
    "-sEXPORTED_FUNCTIONS=@${CMAKE_CURRENT_SOURCE_DIR}/exports.txt"
    -fno-exceptions -O2
)
target_compile_options(wf_web_minimal PRIVATE -fno-exceptions -O2)

# Stage the JS facade + a smoke-test HTML next to wf-core.{js,wasm} so a
# local web server can serve them with no extra plumbing.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/wf-viewer.js"
               "${CMAKE_BINARY_DIR}/web/wf-viewer.js" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/index.html"
               "${CMAKE_BINARY_DIR}/web/index.html"   COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/minimal.html"
               "${CMAKE_BINARY_DIR}/web/minimal.html" COPYONLY)
