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
# WebGPU needs a Dawn distribution. The repository fetches one by cloning
# eliemichel/WebGPU-distribution at configure time; a published crate must
# not do that, so here the path is supplied and nothing is downloaded.
# WFS_DAWN_DIR points at an unpacked distribution containing include/ and
# lib/ (what WebGPU-distribution's prebuilt zip unpacks to).
option(WFS_ENABLE_WEBGPU "Build the WebGPU backend (requires WFS_DAWN_DIR)" OFF)
set(WFS_DAWN_DIR "" CACHE PATH "Unpacked Dawn distribution (include/ + lib/)")
# WhiteoutLib implements CASC itself — no CascLib, no external package — so
# this costs compile time and nothing else. On by default: without it the
# renderer sees only loose files, and a Reforged model whose textures live
# in the game's storage renders as untextured placeholder pink.
option(WFS_ENABLE_CASC   "Build CASC storage support"                       ON)

# WhiteoutLib can come from here (the vendored copy, default) or from the
# `whiteoutlib` crate, which owns `whiteout_native` and publishes where it
# put it. Building both copies links — MSVC silently keeps one definition
# per duplicated COMDAT — but compiles WhiteoutLib twice and leaves the
# choice of surviving copy to the linker. See BINDINGS.md §17.
option(WFS_EXTERNAL_WHITEOUTLIB
    "Compile against an externally-built WhiteoutLib instead of the vendored copy" OFF)
set(WFS_WHITEOUTLIB_INCLUDE "" CACHE PATH "WhiteoutLib headers (external mode)")

# ── Sources ───────────────────────────────────────────────────────────────
# `src/` holds library code only — the viewer, Max plugin and explorer all
# live under tools/, which is not vendored. So the whole tree is in scope
# apart from the backends and adapters excluded above.

file(GLOB_RECURSE _flakes_src CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
if(WFS_EXTERNAL_WHITEOUTLIB)
    if(NOT WFS_WHITEOUTLIB_INCLUDE OR NOT EXISTS "${WFS_WHITEOUTLIB_INCLUDE}")
        message(FATAL_ERROR
            "WFS_EXTERNAL_WHITEOUTLIB=ON needs WFS_WHITEOUTLIB_INCLUDE pointing "
            "at WhiteoutLib's headers. The whiteoutlib crate publishes this as "
            "DEP_WHITEOUT_NATIVE_INCLUDE.")
    endif()
    # Not one translation unit of WhiteoutLib: only its headers, and the
    # archive the other crate already built and linked.
    set(_wlib_src)
else()
    file(GLOB_RECURSE _wlib_src CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_SOURCE_DIR}/whiteoutlib/src/*.cpp")
endif()

set(_excluded_dirs
    "/src/renderer/imgui/"      # needs externals/imgui
    "/src/gfx/metal/"           # needs the macOS SDK; .mm anyway
)
if(NOT WFS_ENABLE_WEBGPU)
    list(APPEND _excluded_dirs "/src/gfx/webgpu/")
endif()
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
    "${CMAKE_CURRENT_SOURCE_DIR}/embedded"
    "${CMAKE_CURRENT_SOURCE_DIR}/c"
)

# WhiteoutLib headers: the external tree when the other crate owns the
# library, the vendored copy otherwise.
if(WFS_EXTERNAL_WHITEOUTLIB)
    # Headers only. This target is a static archive, which cannot absorb
    # another one, and the linking happens a level up: the `whiteoutlib`
    # crate already tells cargo to link its archives, and cargo passes a
    # dependency's link directives on to the final binary.
    target_include_directories(whiteoutflakes_native PUBLIC "${WFS_WHITEOUTLIB_INCLUDE}")
else()
    target_include_directories(whiteoutflakes_native PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/whiteoutlib/include")
endif()

target_compile_definitions(whiteoutflakes_native PUBLIC
    WDX_ENABLE_IMGUI=0
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
    # VMA ships under <Vulkan SDK>/Include/vma/vk_mem_alloc.h — a directory
    # the Vulkan::Vulkan target does not put on the include path. The
    # default silent SDK install omits the VMA component entirely, so a
    # missing vk_mem_alloc.h here means the SDK needs reinstalling with it.
    target_include_directories(whiteoutflakes_native PRIVATE
        "${Vulkan_INCLUDE_DIRS}/vma")
    target_compile_definitions(whiteoutflakes_native PUBLIC WDX_HAS_VULKAN=1)
    # vulkan.hpp is configured by macros, not headers: without these the
    # RAII wrappers return exception-throwing types instead of the
    # result-carrying ones the backend is written against, and every call
    # site fails with "'result' is not a member of vk::raii::…". Must match
    # the top-level CMakeLists exactly.
    target_compile_definitions(whiteoutflakes_native PRIVATE
        VULKAN_HPP_NO_EXCEPTIONS
        VULKAN_HPP_NO_SMART_HANDLE
        VULKAN_HPP_NO_CONSTRUCTORS)
    if(WIN32)
        target_compile_definitions(whiteoutflakes_native PRIVATE
            VK_USE_PLATFORM_WIN32_KHR=1
            NOMINMAX
            WIN32_LEAN_AND_MEAN)
    endif()
else()
    target_compile_definitions(whiteoutflakes_native PUBLIC WDX_HAS_VULKAN=0)
endif()

if(WFS_ENABLE_WEBGPU)
    if(NOT WFS_DAWN_DIR OR NOT EXISTS "${WFS_DAWN_DIR}/include")
        message(FATAL_ERROR
            "WFS_ENABLE_WEBGPU=ON needs WFS_DAWN_DIR pointing at an unpacked "
            "Dawn distribution (include/ + lib/). Nothing is downloaded here: "
            "fetch one yourself, e.g. from eliemichel/WebGPU-distribution's "
            "prebuilt releases, and set WHITEOUTFLAKES_DAWN_DIR.")
    endif()
    if(NOT WIN32)
        # The macOS and Linux surface paths call glfwGetCocoaWindow /
        # glfwGetX11Display / glfwGetWaylandDisplay to assemble handle pairs
        # that don't fit in one void*. GLFW is not vendored, and a binding
        # host has its own windowing library, so those paths need to take
        # raw handles before this can build off Windows.
        message(FATAL_ERROR
            "WFS_ENABLE_WEBGPU is Windows-only in this build: the macOS and "
            "Linux surface paths in src/gfx/webgpu/webgpu_swap_chain.cpp go "
            "through GLFW, which is not vendored.")
    endif()
    target_include_directories(whiteoutflakes_native PUBLIC "${WFS_DAWN_DIR}/include")
    find_library(WFS_DAWN_LIB NAMES webgpu_dawn dawn
                 PATHS "${WFS_DAWN_DIR}/lib" REQUIRED NO_DEFAULT_PATH)
    target_link_libraries(whiteoutflakes_native PUBLIC "${WFS_DAWN_LIB}")
    target_compile_definitions(whiteoutflakes_native PUBLIC WDX_HAS_WEBGPU=1)
else()
    target_compile_definitions(whiteoutflakes_native PUBLIC WDX_HAS_WEBGPU=0)
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
