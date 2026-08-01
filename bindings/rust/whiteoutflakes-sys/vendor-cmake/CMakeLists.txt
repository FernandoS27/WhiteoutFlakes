# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Fernando Sahmkow
#
# Self-contained build of the renderer + its C ABI, for the published
# `whiteoutflakes-sys` crate. Deliberately NOT the repository's top-level
# CMakeLists: that one configures a viewer (GLFW, Dear ImGui, cubeb,
# nativefiledialog, Tracy, Dawn), drives the Slang shader toolchain, and
# takes ~100 s to configure. A crate consumer wants none of it.
#
# What this drops, and what each costs:
#
#   Dear ImGui    The renderer's ImGui adapter (src/renderer/imgui/) is
#                 excluded and WDX_ENABLE_IMGUI=0. ImGuiRenderer::Render
#                 already no-ops without a context, so a non-C++ host
#                 loses nothing.
#   Tracy         Profiler instrumentation compiles out.
#   WebGPU/Dawn   Dawn arrives via FetchContent — a network fetch during a
#                 build script, which is not acceptable in a published
#                 crate. WDX_HAS_WEBGPU=0.
#   Vulkan/Metal  Need an SDK this build can't vendor. Off by default;
#                 WFS_ENABLE_VULKAN=ON opts in if find_package finds one.
#   CASC / MPQ    Pulled through WhiteoutLib's vcpkg ports. Off by
#                 default — see WFS_ENABLE_CASC below for what that costs
#                 at runtime.
#
# Everything here is globbed rather than listed, so the vendored tree
# stays the single source of truth. `scripts/vendor-rust.ps1` cross-checks
# the glob against the real target's source list and fails on drift.

cmake_minimum_required(VERSION 3.20)
project(whiteoutflakes_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(WFS_ENABLE_VULKAN "Build the Vulkan backend (requires a Vulkan SDK)" OFF)
# WhiteoutLib implements CASC itself — no CascLib, no external package — so
# this costs compile time and nothing else. On by default: without it the
# renderer sees only loose files, and a Reforged model whose textures live
# in the game's storage renders as untextured placeholder pink.
option(WFS_ENABLE_CASC   "Build CASC storage support"                       ON)

# ── Sources ───────────────────────────────────────────────────────────────
# `src/` holds library code only — the viewer, Max plugin and explorer all
# live under tools/, which is not vendored. So the whole tree is in scope
# apart from the backends and adapters excluded above.

file(GLOB_RECURSE _flakes_src CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE _wlib_src   CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/whiteoutlib/src/*.cpp")

set(_excluded_dirs
    "/src/renderer/imgui/"      # needs externals/imgui
    "/src/gfx/webgpu/"          # needs Dawn
    "/src/gfx/metal/"           # needs the macOS SDK; .mm anyway
)
if(NOT WFS_ENABLE_VULKAN)
    list(APPEND _excluded_dirs "/src/gfx/vulkan/")
endif()
if(NOT WIN32)
    list(APPEND _excluded_dirs "/src/gfx/d3d11/" "/src/gfx/d3d12/")
endif()

foreach(_dir IN LISTS _excluded_dirs)
    list(FILTER _flakes_src EXCLUDE REGEX "${_dir}")
    list(FILTER _wlib_src   EXCLUDE REGEX "${_dir}")
endforeach()

# WhiteoutLib's storage headers `#error` unless their feature macro is set,
# so every TU that reaches them has to go. Consumers guard their includes
# with `#if WHITEOUT_HAS_CASC` (file_content_provider.cpp does); the storage
# implementations themselves do not, and neither do the *_file_system
# adapters, which live outside the storage directories.
if(NOT WFS_ENABLE_CASC)
    list(FILTER _wlib_src EXCLUDE REGEX "/casc/")
    list(FILTER _wlib_src EXCLUDE REGEX "casc_file_system\\.cpp$")
endif()
list(FILTER _wlib_src EXCLUDE REGEX "/mpq/")
list(FILTER _wlib_src EXCLUDE REGEX "mpq_file_system\\.cpp$")

# The DEFLATE codec has two implementations; the built-in one is the
# upstream default (WHITEOUT_USE_ZLIBNG=OFF) and needs no external header.
list(FILTER _wlib_src EXCLUDE REGEX "deflate_zlibng\\.cpp$")

# WhiteoutLib parses MDX, M2, M3 and WEM. The renderer reads MDX only —
# nothing under src/ mentions the others — so the rest are dropped. Worth
# ~30 translation units off every consumer's build.
foreach(_fmt "/models/m2/" "/models/m3/" "/models/wem/")
    list(FILTER _wlib_src EXCLUDE REGEX "${_fmt}")
endforeach()

# whiteout/sno/ (Diablo asset naming) looks equally droppable, and isn't:
# the CASC layer's d3_root.cpp / d4_root.cpp link against it
# unconditionally. Keep it whenever CASC is on.
if(NOT WFS_ENABLE_CASC)
    list(FILTER _wlib_src EXCLUDE REGEX "/sno/")
endif()

# sno/sno_defs.cpp is orphaned upstream — WhiteoutLib's CMakeLists builds
# sno/d3/sno_defs.cpp and sno/d4/sno_defs.cpp but not this one, and it does
# not compile on its own (references an undeclared D4SnoTypeRegistry). A
# glob picks it up where an explicit source list would not.
list(FILTER _wlib_src EXCLUDE REGEX "/sno/sno_defs\\.cpp$")

set(_c_abi
    "${CMAKE_CURRENT_SOURCE_DIR}/c/whiteout_flakes.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/c/whiteout_flakes_common.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/c/whiteout_flakes_shims.cpp"
)

add_library(whiteoutflakes_native STATIC ${_flakes_src} ${_wlib_src} ${_c_abi})

target_include_directories(whiteoutflakes_native PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/io"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/corn_effects"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/corn_effects/cornflakes/internal"
    "${CMAKE_CURRENT_SOURCE_DIR}/whiteoutlib/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/embedded"
    "${CMAKE_CURRENT_SOURCE_DIR}/c"
)

target_compile_definitions(whiteoutflakes_native PUBLIC
    WDX_ENABLE_IMGUI=0
    WDX_HAS_WEBGPU=0
    WDX_HAS_METAL=0
    # cornflakes asserts this is defined in every TU that includes
    # core/determinism.hpp.
    CORNFLAKES_DETERMINISM_FLAGS_APPLIED=1
)
# WDX_DEFAULT_COORD_SPACE is deliberately NOT defined: coordinate_system.h
# pastes it after `CoordSpace::`, so it has to be an enumerator name rather
# than a number, and the header already defaults it to Blizzard.

if(WFS_ENABLE_VULKAN)
    find_package(Vulkan REQUIRED)
    target_link_libraries(whiteoutflakes_native PUBLIC Vulkan::Vulkan)
    target_compile_definitions(whiteoutflakes_native PUBLIC WDX_HAS_VULKAN=1)
else()
    target_compile_definitions(whiteoutflakes_native PUBLIC WDX_HAS_VULKAN=0)
endif()

if(WFS_ENABLE_CASC)
    target_compile_definitions(whiteoutflakes_native PUBLIC WHITEOUT_HAS_CASC=1)
endif()

if(WIN32)
    target_link_libraries(whiteoutflakes_native PUBLIC
        d3d11 d3d12 dxgi user32 gdi32 ole32 advapi32 shell32 dbghelp ws2_32)
endif()

if(MSVC)
    # /EHsc is normally in CMake's own default CXX flags, but the `cmake`
    # crate replaces CMAKE_CXX_FLAGS wholesale, so it has to be restored
    # here. Without it MSVC compiles the STL and the MDX parser's throw
    # sites with unwind semantics disabled (warning C4530) and a thrown
    # exception terminates instead of unwinding.
    #
    # /fp:precise + /fp:except- match the main build: cornflakes evaluation
    # has to stay bit-deterministic.
    target_compile_options(whiteoutflakes_native PRIVATE
        /EHsc /fp:precise /fp:except- /bigobj)
else()
    target_compile_options(whiteoutflakes_native PRIVATE -ffp-contract=off)
endif()

install(TARGETS whiteoutflakes_native ARCHIVE DESTINATION lib)
