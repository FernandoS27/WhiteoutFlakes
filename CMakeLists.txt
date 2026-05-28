# ============================================================================
# WhiteoutFlakes — standalone renderer + basic_viewer tool.
# ============================================================================

cmake_minimum_required(VERSION 3.20)
project(WhiteoutFlakes LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Emscripten / WebAssembly target ----
# Configured via `emcmake cmake ...`. The web build is a lean subset:
# WebGPU only, no desktop windowing/audio/UI, no native game archives.
# Every gating decision below keys off EMSCRIPTEN; the desktop build is
# unaffected.
if(EMSCRIPTEN)
    set(WDX_ENABLE_TRACY  OFF CACHE BOOL "" FORCE)  # no web transport
    set(WDX_ENABLE_IMGUI  OFF CACHE BOOL "" FORCE)  # JS owns all UI chrome
    set(WDX_ENABLE_WEBGPU ON  CACHE BOOL "" FORCE)  # only web backend
    set(WDX_ENABLE_WEBP   OFF CACHE BOOL "" FORCE)  # viewer-only export
    # Exceptions: -fno-exceptions everywhere. The renderer doesn't throw
    # and WhiteoutLib has been adjusted to compile without `throw` under
    # Emscripten — so there's no reason to ship EH machinery. This is
    # what avoids the Chrome wasm-compile freeze we hit with both
    # -fexceptions (JS-trampolined invoke_*) and -fwasm-exceptions
    # (native try_table) — both emit code Chrome's wasm compiler stalls
    # on for >60 s, while the equivalent EH-free wasm compiles in ms.
    add_compile_options(-fno-exceptions)
    add_link_options(-fno-exceptions)
    # Compile-time: -O2 for better per-function codegen quality (register
    # allocation, CSE, loop opts, constant propagation). Link-time: -Oz
    # to keep the wasm binary small via wasm-opt. The Chrome V8 freeze we
    # used to hit was caused by *huge inlined functions* (50k+ wasm
    # instructions per func, e.g. PIMPL ctors absorbing their whole
    # subsystem graph) tipping V8 into synchronous TurboFan — that's
    # already blocked by `-fno-inline-functions` + `-fno-lto`, which we
    # keep. With those guards in place, -O2 produces faster code without
    # creating the giant functions V8 chokes on.
    #
    # Was -Oz globally; the Firefox profile showed 73% of frame time in
    # our WASM (Firefox WGPU/SpiderMonkey is more sensitive to AOT code
    # quality than Chrome/V8, which JIT-optimizes hot paths at run time).
    # Lifting compile -O to O2 closes a meaningful chunk of the gap.
    #
    # Flags retained:
    #   -fno-lto           : no cross-TU inlining at link time
    #   -fno-inline-functions: no intra-TU inlining of large funcs
    # Combined with `-O2`, individual function sizes stay manageable.
    add_compile_options(-O2 -fno-lto -fno-inline-functions)
    add_link_options(-Oz -fno-lto)
    message(STATUS "WhiteoutFlakes: Emscripten/WebAssembly build "
                   "(no exceptions, compile -O2 / link -Oz, no inlining/LTO)")
endif()

# Disable vcpkg's user-wide MSBuild integration (`vcpkg integrate install`)
# for every generated .vcxproj. WhiteoutFlakes pulls all its dependencies via
# FetchContent / add_subdirectory and uses no vcpkg toolchain or manifest, but
# a machine-wide vcpkg integration would otherwise auto-link every installed
# vcpkg package into our binaries — e.g. silently importing vcpkg's shared
# libwebp.dll alongside our own statically-built libwebp. Emitted into the
# Globals PropertyGroup, which vcpkg.user.targets honours. (VS generator only;
# harmless elsewhere, where the integration does not apply anyway.)
set(CMAKE_VS_GLOBALS "VcpkgEnabled=false")

# Pull in WhiteoutLib (provides whiteout_lib; optionally whiteout_casc, whiteout_mpq).
# CASC/MPQ are native file-I/O + threading — disabled under Emscripten; the web
# build receives bytes from JS via FetchContentProvider instead.
if(EMSCRIPTEN)
    set(WHITEOUT_ENABLE_CASC OFF CACHE BOOL "Enable CASC archive support" FORCE)
    set(WHITEOUT_ENABLE_MPQ  OFF CACHE BOOL "Enable MPQ archive support"  FORCE)
    # WhiteoutLib auto-enables embind bindings under EMSCRIPTEN. Those
    # bindings register every public C++ class/method into JS via a
    # massive static-init constructor — easily 40k+ wasm instructions in
    # a single function, which trips V8's synchronous-compile fallback
    # and freezes the page. We have our own `wf_*` C facade and never
    # call into JS-bound WhiteoutLib symbols, so the bindings are pure
    # dead weight here.
    set(WHITEOUT_BUILD_WASM_BINDINGS OFF CACHE BOOL "" FORCE)
else()
    set(WHITEOUT_ENABLE_CASC ON CACHE BOOL "Enable CASC archive support" FORCE)
    set(WHITEOUT_ENABLE_MPQ  ON CACHE BOOL "Enable MPQ archive support"  FORCE)
endif()
add_subdirectory(externals/WhiteoutLib)

# Tracy — opt-in CPU + GPU profiler. When WDX_ENABLE_TRACY is ON, the
# Tracy::TracyClient target is built and TRACY_ENABLE is defined for
# everyone linking it; otherwise every TracyVk*/ZoneScoped macro expands
# to nothing and there's zero runtime cost. TRACY_ON_DEMAND keeps the
# client buffering minimal until Tracy.exe connects.
option(WDX_ENABLE_TRACY "Enable Tracy profiler (CPU + Vulkan GPU)" ON)
if(WDX_ENABLE_TRACY AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/tracy/CMakeLists.txt")
    set(TRACY_ENABLE    ON  CACHE BOOL "" FORCE)
    set(TRACY_ON_DEMAND ON  CACHE BOOL "" FORCE)
    set(TRACY_NO_FRAME_IMAGE ON CACHE BOOL "" FORCE)
    add_subdirectory(externals/tracy)
    set(WDX_TRACY_LIB Tracy::TracyClient)
    message(STATUS "Tracy: enabled (TRACY_ENABLE + TRACY_ON_DEMAND)")
else()
    set(WDX_TRACY_LIB "")
    if(WDX_ENABLE_TRACY)
        message(WARNING "WDX_ENABLE_TRACY=ON but externals/tracy is missing. "
                        "Run: git submodule update --init externals/tracy")
    endif()
endif()

# cornflakes is folded directly into WhiteoutFlakesLib as a renderer module
# the cornflakes determinism + warnings INTERFACE targets the upstream project
# defines are replaced by inline compile flags on WhiteoutFlakesLib.
set(CORNFLAKES_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/corn_effects/cornflakes")
set(CORNFLAKES_SOURCES
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/version.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/core/arena.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/diagnostics/issue.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/diagnostics/frame_trace.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/scheduler/serial_worker_pool.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/service/cornflakes_service.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/asset/asset_reader.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/asset/pkb_reader.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/asset/text_pkfx_reader.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/asset/war3_compatibility_validator.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/asset/object_accessors.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/asset/field_defs_generated.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/binding/effect_binder.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/binding/program_binder.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/binding/renderer_binder.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/binding/sampler_binder.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/binding/external_binding.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/binding/ir_to_cbem_lowerer.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/binding/sampler_resource.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/binding/spatial_layer_resource.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/vm/bytecode_trace.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/vm/cbem/cbem_interpreter.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/vm/cbem/cbem_ir_ops.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/vm/cbem/cbem_cbem_ops.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/vm/cbem/cbem_dispatchers.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/vm/math_functions.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/vm/bytecode_decoder.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/sampler/shape.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/sampler/curve.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/sampler/texture.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/sampler/turbulence.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/sim/particle_page.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/sim/spawn_processor.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/sim/spawn_evaluator_driver.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/sim/evolve_page_task.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/sim/simulation_runtime.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/sim/layer_tick_harness.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/sim/particle_pool.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/sim/effect_runtime.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/events/payload_cache_store.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/events/payload_builder.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/events/event_generator.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/events/event_router.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/events/kick_processor.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/render/semantic_slot_reader.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/render/renderer_class_bucket.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/render/render_extractor.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/render/pool_extractor.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/render/ribbon_geometry.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/render/shader_perm.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/render/instance_stream.cpp"
    "${CORNFLAKES_MODULE_DIR}/internal/cornflakes/validation/batch_resource_tracker.cpp"
)

# ============================================================================
# Slang shader compilation (compile-time: .slang → DXBC → embedded C++ header)
# ============================================================================

set(SHADER_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/shaders")
set(SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT_DIR}")

find_program(SLANGC slangc
    HINTS "${SLANG_DIR}/bin" "$ENV{SLANG_DIR}/bin"
    DOC   "Path to Slang compiler (slangc)")

if(NOT SLANGC)
    message(FATAL_ERROR
        "slangc not found. Install the Slang SDK from https://github.com/shader-slang/slang/releases "
        "and set -DSLANG_DIR=<path-to-slang-sdk> or add slangc to PATH.")
endif()
message(STATUS "Using slangc (default): ${SLANGC}")

# Optional override for the Vulkan-target slangc. Some upstream slang
# bugs (e.g. `Conditional<>` + SPIR-V specialise emit) are fixed in
# tip-of-tree slang but not in the Vulkan SDK build, while the SDK
# slangc still emits cleaner DXIL than tip — pass
# -DSLANGC_VULKAN=<path-to-newer-slangc.exe> to use the newer build
# for SPIR-V only, and keep the SDK slangc for d3d11 / d3d12.
if(NOT DEFINED SLANGC_VULKAN OR SLANGC_VULKAN STREQUAL "")
    set(SLANGC_VULKAN "${SLANGC}"
        CACHE FILEPATH "slangc used for the Vulkan (SPIR-V) target. Defaults to SLANGC.")
endif()
message(STATUS "Using slangc (vulkan): ${SLANGC_VULKAN}")

# Common Slang flags: column-major matrices (matches C++ transpose-before-upload convention)
# Use DXC as the downstream compiler instead of FXC for DXBC output
set(SLANG_COMMON_FLAGS -matrix-layout-column-major -I "${SHADER_SRC_DIR}"
    -default-downstream-compiler hlsl dxc)

# Shader entry points: <slang_file>  <entry>  <stage>  <output_name>
# Only line.slang (debug wires), blit.slang and framebuffer_capture.slang
# remain as Slang sources; every MDX-material draw goes through BLS, and the
# tonemap pass uses the engine's sprite.bls VS directly.
set(SHADER_ENTRIES
    "line.slang|vsLine|vertex|kLineVS"
    "line.slang|psLine|fragment|kLinePS"
    "blit.slang|vsBlit|vertex|kBlitVS"
    "blit.slang|psBlit|fragment|kBlitPS"
    "framebuffer_capture.slang|csCapture|compute|kCaptureCS"
)

set(SHADER_DXBC_FILES "")
set(SHADER_SPV_FILES  "")
set(SHADER_WGSL_FILES "")

foreach(entry IN LISTS SHADER_ENTRIES)
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 src_file)
    list(GET parts 1 entry_name)
    list(GET parts 2 stage)
    list(GET parts 3 var_name)

    set(dxbc_path "${SHADER_OUT_DIR}/${var_name}.dxbc")
    set(spv_path  "${SHADER_OUT_DIR}/${var_name}.spv")

    # DXBC is Windows-only: slangc's DXBC backend invokes fxc internally,
    # and fxc.exe ships with the Windows SDK / d3dcompiler — there's no
    # Linux/macOS equivalent. On non-Windows hosts we skip the DXBC step
    # entirely and let embed_shaders.cmake stub the DXBC C++ symbols so
    # the consumer call sites still compile (they're never reached at
    # runtime on platforms without D3D).
    if(WIN32)
        add_custom_command(
            OUTPUT  "${dxbc_path}"
            COMMAND "${SLANGC}" "${SHADER_SRC_DIR}/${src_file}"
                    ${SLANG_COMMON_FLAGS}
                    -entry ${entry_name} -stage ${stage}
                    -target dxbc -profile sm_5_0
                    -o "${dxbc_path}"
            DEPENDS "${SHADER_SRC_DIR}/${src_file}"
                    "${SHADER_SRC_DIR}/common.slang"
            COMMENT "Slang: ${src_file} [${entry_name}] → ${var_name}.dxbc"
            VERBATIM
        )
        list(APPEND SHADER_DXBC_FILES "${dxbc_path}")
    endif()

    # SPIR-V variant for the Vulkan backend. The embed script picks these
    # up alongside the .dxbc blobs and emits a parallel `<var>Spv[]` array
    # in compiled_shaders.h. Compiled here unconditionally — the resulting
    # bytes are unused (and harmless) when WDX_HAS_VULKAN is off.
    add_custom_command(
        OUTPUT  "${spv_path}"
        COMMAND "${SLANGC}" "${SHADER_SRC_DIR}/${src_file}"
                ${SLANG_COMMON_FLAGS}
                -entry ${entry_name} -stage ${stage}
                -target spirv -profile glsl_450
                -o "${spv_path}"
        DEPENDS "${SHADER_SRC_DIR}/${src_file}"
                "${SHADER_SRC_DIR}/common.slang"
        COMMENT "Slang: ${src_file} [${entry_name}] → ${var_name}.spv"
        VERBATIM
    )
    list(APPEND SHADER_SPV_FILES "${spv_path}")

    # WGSL variant for the WebGPU backend. Only emitted when WDX_ENABLE_WEBGPU
    # is on — otherwise the embed script's stub keeps the consumer code
    # type-checking without dragging the slangc invocation into every build.
    if(WDX_ENABLE_WEBGPU)
        set(wgsl_path "${SHADER_OUT_DIR}/${var_name}.wgsl")
        add_custom_command(
            OUTPUT  "${wgsl_path}"
            COMMAND "${SLANGC}" "${SHADER_SRC_DIR}/${src_file}"
                    ${SLANG_COMMON_FLAGS}
                    -entry ${entry_name} -stage ${stage}
                    -target wgsl
                    -o "${wgsl_path}"
            DEPENDS "${SHADER_SRC_DIR}/${src_file}"
                    "${SHADER_SRC_DIR}/common.slang"
            COMMENT "Slang: ${src_file} [${entry_name}] → ${var_name}.wgsl"
            VERBATIM
        )
        list(APPEND SHADER_WGSL_FILES "${wgsl_path}")
    endif()
endforeach()

# Embed all DXBC blobs into a single C++ header
set(COMPILED_SHADERS_H "${SHADER_OUT_DIR}/compiled_shaders.h")

add_custom_command(
    OUTPUT  "${COMPILED_SHADERS_H}"
    COMMAND "${CMAKE_COMMAND}"
            "-DSHADER_DIR=${SHADER_OUT_DIR}"
            "-DOUTPUT=${COMPILED_SHADERS_H}"
            -P "${SHADER_SRC_DIR}/embed_shaders.cmake"
    DEPENDS ${SHADER_DXBC_FILES} ${SHADER_SPV_FILES} ${SHADER_WGSL_FILES}
            "${SHADER_SRC_DIR}/embed_shaders.cmake"
    COMMENT "Embedding compiled shaders into compiled_shaders.h"
    VERBATIM
)

add_custom_target(WhiteoutFlakesShaders DEPENDS "${COMPILED_SHADERS_H}")

# ============================================================================
# Embedded resources (build-time: binary file → C++ header with byte array)
# ============================================================================

set(EMBED_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/embedded")
file(MAKE_DIRECTORY "${EMBED_OUT_DIR}")

set(TEAM_GLOW_TGA "${CMAKE_CURRENT_SOURCE_DIR}/resources/teamglow.tga")
set(TEAM_GLOW_H   "${EMBED_OUT_DIR}/team_glow_embedded.h")

add_custom_command(
    OUTPUT  "${TEAM_GLOW_H}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${TEAM_GLOW_TGA}"
            "-DOUTPUT=${TEAM_GLOW_H}"
            "-DVAR_NAME=kTeamGlowTGA"
            "-DNAMESPACE=whiteout::flakes::io"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/resources/embed_binary.cmake"
    DEPENDS "${TEAM_GLOW_TGA}"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/embed_binary.cmake"
    COMMENT "Embedding teamglow.tga into team_glow_embedded.h"
    VERBATIM
)

# Roboto-Medium for the ImGui host theme — hinted TTF (vs. ProggyClean bitmap)
# so DPI-scaled UIs stay crisp instead of bilinear-upscaled.
set(ROBOTO_TTF "${CMAKE_CURRENT_SOURCE_DIR}/externals/imgui/misc/fonts/Roboto-Medium.ttf")
set(ROBOTO_H   "${EMBED_OUT_DIR}/roboto_medium_embedded.h")

add_custom_command(
    OUTPUT  "${ROBOTO_H}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${ROBOTO_TTF}"
            "-DOUTPUT=${ROBOTO_H}"
            "-DVAR_NAME=kRobotoMediumTTF"
            "-DNAMESPACE=whiteout::flakes::ui"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/resources/embed_binary.cmake"
    DEPENDS "${ROBOTO_TTF}"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/embed_binary.cmake"
    COMMENT "Embedding Roboto-Medium.ttf into roboto_medium_embedded.h"
    VERBATIM
)

add_custom_target(WhiteoutFlakesEmbedded DEPENDS "${TEAM_GLOW_H}" "${ROBOTO_H}")

# ============================================================================
# Wc3Shaders — opt-in build of the externals/Wc3Shaders Slang reconstruction.
#
# When WDX_BUILD_WC3_SHADERS=ON, drives the submodule's two-step Python
# pipeline (compile_all_slang.py → DXBC + DXIL, then build_bls.py → .bls)
# and copies the resulting BLS bundles next to the standalone exe under
# Shaders/{ps,vs}/. Packing requires the shipped war3.w3mod/shaders/ tree
# (binding-metadata templates), supplied via -DWDX_SHADER_TEMPLATES=<path>.
# Both DXBC (sm 5.0, primary — what the BLS loader reads) and DXIL (sm 6.0,
# kept as raw .dxil under wc3_shaders/slang_out/ for future use) targets
# are produced.
# ============================================================================

option(WDX_BUILD_WC3_SHADERS
    "Compile the externals/Wc3Shaders Slang sources and pack BLS bundles"
    OFF)

# When ON (and WDX_BUILD_WC3_SHADERS is too), a second pass builds a
# parallel set of BLS bundles with full debug symbols (slangc -g2 -O0,
# +emit-spirv-directly) and stages them into standalone/debug_shaders/.
# The renderer loads those instead of standalone/shaders/ when graphics
# debug is on. Roughly doubles the Wc3Shaders compile time and the
# emitted bundles weigh ~865 MB — opt in only when actively debugging
# shaders. Not committed under prebuilt/ for the same size reason.
option(WDX_BUILD_WC3_DEBUG_SHADERS
    "Also build debug-symbol BLS bundles into debug_shaders/"
    OFF)

# Use the pre-baked .bls bundles committed under prebuilt/shaders/ instead of
# re-running slangc. Mutually exclusive with WDX_BUILD_WC3_SHADERS — the
# auto-detect at the bottom of this option block flips it on by default
# whenever the prebuilt dir exists *and* shader compilation is off, which
# covers both the GitHub CI case (slangc isn't installed) and a fresh clone
# without the Vulkan SDK.
option(WDX_USE_PREBUILT_SHADERS
    "Stage the prebuilt/shaders pack instead of running the Slang toolchain"
    OFF)
if(NOT WDX_USE_PREBUILT_SHADERS AND NOT WDX_BUILD_WC3_SHADERS
        AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/shaders")
    set(WDX_USE_PREBUILT_SHADERS ON CACHE BOOL
        "Stage the prebuilt/shaders pack instead of running the Slang toolchain"
        FORCE)
    message(STATUS "Wc3Shaders: WDX_USE_PREBUILT_SHADERS auto-enabled (prebuilt/shaders found)")
endif()
if(WDX_BUILD_WC3_SHADERS AND WDX_USE_PREBUILT_SHADERS)
    message(FATAL_ERROR
        "WDX_BUILD_WC3_SHADERS and WDX_USE_PREBUILT_SHADERS are mutually exclusive — "
        "one runs slangc, the other copies pre-baked bundles. Pick one.")
endif()

if(WDX_USE_PREBUILT_SHADERS)
    set(WDX_PREBUILT_SHADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/shaders"
        CACHE PATH "Pre-baked .bls staging tree (mirrors standalone/shaders/ layout)")
    if(NOT EXISTS "${WDX_PREBUILT_SHADERS_DIR}")
        message(FATAL_ERROR
            "WDX_USE_PREBUILT_SHADERS=ON but ${WDX_PREBUILT_SHADERS_DIR} is missing. "
            "Run cmake --build build --target update-prebuilt-shaders against a "
            "tree where WDX_BUILD_WC3_SHADERS=ON to populate it.")
    endif()
    # Stage prebuilt → standalone/shaders so the runtime exe-dir lookup
    # finds them with no extra path config (matches the WDX_BUILD_WC3_SHADERS
    # codegen layout exactly).
    set(_prebuilt_stage "${CMAKE_BINARY_DIR}/standalone/shaders")
    set(_prebuilt_stamp "${CMAKE_CURRENT_BINARY_DIR}/wc3_prebuilt.stamp")
    add_custom_command(
        OUTPUT  "${_prebuilt_stamp}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_prebuilt_stage}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${WDX_PREBUILT_SHADERS_DIR}" "${_prebuilt_stage}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${_prebuilt_stamp}"
        # Re-stage when any prebuilt file changes (developer pulled new
        # prebuilt/ from git).
        DEPENDS "${WDX_PREBUILT_SHADERS_DIR}"
        COMMENT "Wc3Shaders[prebuilt]: staging ${WDX_PREBUILT_SHADERS_DIR} -> ${_prebuilt_stage}"
        VERBATIM)
    add_custom_target(Wc3Shaders ALL DEPENDS "${_prebuilt_stamp}")

    # debug_shaders/ is intentionally not prebuilt — the .bls bundles
    # weigh ~865 MB and bloat the repo. Developers who need them flip
    # WDX_BUILD_WC3_SHADERS=ON locally (which also honors
    # WDX_BUILD_WC3_DEBUG_SHADERS=ON to produce standalone/debug_shaders/).
endif()

if(WDX_BUILD_WC3_SHADERS)
    set(WC3_SHADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/externals/Wc3Shaders")

    if(NOT EXISTS "${WC3_SHADERS_DIR}/compile_all_slang.py")
        message(FATAL_ERROR
            "WDX_BUILD_WC3_SHADERS=ON but externals/Wc3Shaders/ is empty. "
            "Initialize the submodule with: git submodule update --init externals/Wc3Shaders")
    endif()

    # Default to the locally-vendored war3.w3mod/shaders/ tree at the
    # project root (gitignored). Users can still override with
    # -DWDX_SHADER_TEMPLATES=<path> when their templates live elsewhere.
    if(NOT DEFINED WDX_SHADER_TEMPLATES OR WDX_SHADER_TEMPLATES STREQUAL "")
        set(WDX_SHADER_TEMPLATES "${CMAKE_CURRENT_SOURCE_DIR}/war3.w3mod/shaders"
            CACHE PATH "Path to extracted war3.w3mod/shaders/ BLS templates")
    endif()
    if(NOT EXISTS "${WDX_SHADER_TEMPLATES}")
        message(FATAL_ERROR
            "WDX_SHADER_TEMPLATES does not exist: ${WDX_SHADER_TEMPLATES}\n"
            "Point at an extracted war3.w3mod/shaders/ directory containing "
            "the shipped {ps,vs}/*.bls templates, or place the tree at "
            "${CMAKE_CURRENT_SOURCE_DIR}/war3.w3mod/shaders/ to use the default.")
    endif()

    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    message(STATUS "Wc3Shaders: Python = ${Python3_EXECUTABLE}")
    message(STATUS "Wc3Shaders: BLS templates = ${WDX_SHADER_TEMPLATES}")

    # The Python scripts hardcode their output under <repo>/slang_out and
    # <repo>/bls_out (both gitignored inside the submodule). We use a stamp
    # file pattern for dependency tracking — globs catch source / config
    # changes; the slangc the renderer's debug shaders found is forwarded
    # so we don't need to re-resolve.
    file(GLOB_RECURSE WC3_SLANG_SOURCES
        "${WC3_SHADERS_DIR}/wc3_shaders/*.slang"
        "${WC3_SHADERS_DIR}/custom_shaders/*.slang")
    set(WC3_BUILD_SCRIPTS
        "${WC3_SHADERS_DIR}/compile_all_slang.py"
        "${WC3_SHADERS_DIR}/build_bls.py"
        "${WC3_SHADERS_DIR}/shader_config.py"
        "${WC3_SHADERS_DIR}/wc3_shaders.json"
        "${WC3_SHADERS_DIR}/custom_shaders.json")

    # Optional cache var: semicolon-separated list of shader families to
    # compile. Defaults to empty → compile every family. The compile
    # script's --family flag accepts the same names; only the listed
    # families get re-slang'd, every other family reuses the previous
    # slang_out/<target>/<family>/ output (and BLS bundles).
    set(WDX_WC3_FAMILIES "" CACHE STRING "Wc3Shaders families to re-compile (empty = all)")
    set(WC3_FAMILY_ARGS "")
    if(WDX_WC3_FAMILIES)
        foreach(fam IN LISTS WDX_WC3_FAMILIES)
            list(APPEND WC3_FAMILY_ARGS --family "${fam}")
        endforeach()
        message(STATUS "Wc3Shaders: limiting compile to families: ${WDX_WC3_FAMILIES}")
    endif()

    # Build one variant of the Wc3Shaders pipeline end to end:
    #   compile Slang -> {DXBC, DXIL, SPIR-V}  (compile_all_slang.py)
    #   pack .bls bundles                      (build_bls.py)
    #   stage into the standalone exe's <stage_subdir>/ tree
    #
    # The release variant compiles optimised bytecode into slang_out/ and
    # packs it (with --strip) into shaders/. The debug variant passes
    # --debug to compile_all_slang.py (slangc -g2 -O0, +emit-spirv-directly;
    # output diverted to slang_out_debug/), then packs *without* --strip —
    # stripping would trim the input-signature/debug data we want kept —
    # into debug_shaders/. Both reuse the same .slang sources, the same
    # WC3_FAMILY_ARGS filter, and the same v1.8/v1.14 → flat layout
    # flattening so the renderer's BLS cache only knows two roots:
    # shaders/ and debug_shaders/.
    #
    # Sets ${out_stamp_var} in the caller's scope to the final stage stamp.
    function(wc3_shader_pipeline variant is_debug stage_subdir out_stamp_var)
        if(is_debug)
            set(_compile_extra --debug)
            set(_slang_out "${WC3_SHADERS_DIR}/slang_out_debug")
            set(_bls_out   "${WC3_SHADERS_DIR}/bls_out_debug")
            set(_bls_slang_out_arg --slang-out "${_slang_out}")
            set(_bls_strip_arg)            # debug: keep signatures/debug info
        else()
            set(_compile_extra)            # release: no extra flag
            set(_slang_out "${WC3_SHADERS_DIR}/slang_out")
            set(_bls_out   "${WC3_SHADERS_DIR}/bls_out")
            set(_bls_slang_out_arg)        # build_bls.py defaults to slang_out
            set(_bls_strip_arg --strip)
        endif()

        set(_dxbc  "${CMAKE_CURRENT_BINARY_DIR}/wc3_${variant}_dxbc.stamp")
        set(_dxil  "${CMAKE_CURRENT_BINARY_DIR}/wc3_${variant}_dxil.stamp")
        set(_spirv "${CMAKE_CURRENT_BINARY_DIR}/wc3_${variant}_spirv.stamp")
        set(_wgsl  "${CMAKE_CURRENT_BINARY_DIR}/wc3_${variant}_wgsl.stamp")
        set(_bls   "${CMAKE_CURRENT_BINARY_DIR}/wc3_${variant}_bls.stamp")
        set(_stage "${CMAKE_CURRENT_BINARY_DIR}/wc3_${variant}_stage.stamp")

        add_custom_command(
            OUTPUT  "${_dxbc}"
            COMMAND "${Python3_EXECUTABLE}" "${WC3_SHADERS_DIR}/compile_all_slang.py"
                    --target d3d11 --slangc "${SLANGC}" ${_compile_extra} ${WC3_FAMILY_ARGS}
            COMMAND "${CMAKE_COMMAND}" -E touch "${_dxbc}"
            WORKING_DIRECTORY "${WC3_SHADERS_DIR}"
            DEPENDS ${WC3_SLANG_SOURCES} ${WC3_BUILD_SCRIPTS}
            COMMENT "Wc3Shaders[${variant}]: compiling Slang -> DXBC (d3d11)"
            VERBATIM)

        add_custom_command(
            OUTPUT  "${_dxil}"
            COMMAND "${Python3_EXECUTABLE}" "${WC3_SHADERS_DIR}/compile_all_slang.py"
                    --target d3d12 --slangc "${SLANGC}" ${_compile_extra} ${WC3_FAMILY_ARGS}
            COMMAND "${CMAKE_COMMAND}" -E touch "${_dxil}"
            WORKING_DIRECTORY "${WC3_SHADERS_DIR}"
            DEPENDS ${WC3_SLANG_SOURCES} ${WC3_BUILD_SCRIPTS}
            COMMENT "Wc3Shaders[${variant}]: compiling Slang -> DXIL (d3d12)"
            VERBATIM)

        add_custom_command(
            OUTPUT  "${_spirv}"
            COMMAND "${Python3_EXECUTABLE}" "${WC3_SHADERS_DIR}/compile_all_slang.py"
                    --target vulkan --slangc "${SLANGC_VULKAN}" ${_compile_extra} ${WC3_FAMILY_ARGS}
            COMMAND "${CMAKE_COMMAND}" -E touch "${_spirv}"
            WORKING_DIRECTORY "${WC3_SHADERS_DIR}"
            DEPENDS ${WC3_SLANG_SOURCES} ${WC3_BUILD_SCRIPTS}
            COMMENT "Wc3Shaders[${variant}]: compiling Slang -> SPIR-V (vulkan)"
            VERBATIM)

        # WGSL for the WebGPU backend — only emitted when WDX_ENABLE_WEBGPU
        # is on (the consumer side). compile_all_slang.py writes
        # slang_out/webgpu/<family>/*.wgsl; build_bls.py picks them up
        # automatically and packs them into bls_out_1_14/shaders/{pixel,
        # vertex}/wgsl_1_0/.
        add_custom_command(
            OUTPUT  "${_wgsl}"
            COMMAND "${Python3_EXECUTABLE}" "${WC3_SHADERS_DIR}/compile_all_slang.py"
                    --target webgpu --slangc "${SLANGC_VULKAN}" ${_compile_extra} ${WC3_FAMILY_ARGS}
            COMMAND "${CMAKE_COMMAND}" -E touch "${_wgsl}"
            WORKING_DIRECTORY "${WC3_SHADERS_DIR}"
            DEPENDS ${WC3_SLANG_SOURCES} ${WC3_BUILD_SCRIPTS}
            COMMENT "Wc3Shaders[${variant}]: compiling Slang -> WGSL (webgpu)"
            VERBATIM)

        # Upstream build_bls.py writes two trees:
        #   <output>_1_8/{ps,vs,mtlfs,mtlvs}/*.bls            (Wc3 v1.8 layout)
        #   <output>_1_14/shaders/{pixel,vertex}/<api>/*.bls  (v1.14, WoW-style)
        # where <api> ∈ {dx_5_0, dx_6_0, spv_1_3, …}. The SPIR-V subdir name
        # encodes the slangc-emitted SPIR-V version (1.3 for glsl_450 →
        # Vulkan 1.3).
        set(_bls_out_18 "${_bls_out}_1_8")
        set(_bls_out_14 "${_bls_out}_1_14")
        add_custom_command(
            OUTPUT  "${_bls}"
            COMMAND "${Python3_EXECUTABLE}" "${WC3_SHADERS_DIR}/build_bls.py"
                    --templates "${WDX_SHADER_TEMPLATES}"
                    --output    "${_bls_out}"
                    ${_bls_slang_out_arg}
                    ${_bls_strip_arg}
            COMMAND "${CMAKE_COMMAND}" -E touch "${_bls}"
            WORKING_DIRECTORY "${WC3_SHADERS_DIR}"
            DEPENDS "${_dxbc}" "${_dxil}" "${_spirv}" "${_wgsl}" ${WC3_BUILD_SCRIPTS}
            COMMENT "Wc3Shaders[${variant}]: packing .bls (v1.8 DX + v1.14 DX/SPIR-V/WGSL)"
            VERBATIM)

        # Stage the freshly packed bundles into the standalone exe's output
        # directory the moment Wc3Shaders rebuilds — independently of
        # whether the exe relinks (POST_BUILD on the exe wouldn't fire on a
        # BLS-only change). The standalone exe lands at
        # ${CMAKE_BINARY_DIR}/standalone, so the path is known up front. We
        # flatten the upstream layout into the renderer's path scheme
        # (<root>/{ps,vs,d3d12/ps,d3d12/vs,vulkan/ps,vulkan/vs}/) so the BLS
        # cache only needs to know the two roots shaders/ and debug_shaders/.
        set(_stage_dir "${CMAKE_BINARY_DIR}/standalone/${stage_subdir}")
        add_custom_command(
            OUTPUT  "${_stage}"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${_bls_out_18}/ps"                       "${_stage_dir}/ps"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${_bls_out_18}/vs"                       "${_stage_dir}/vs"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${_bls_out_14}/shaders/pixel/dx_6_0"     "${_stage_dir}/d3d12/ps"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${_bls_out_14}/shaders/vertex/dx_6_0"    "${_stage_dir}/d3d12/vs"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${_bls_out_14}/shaders/pixel/spv_1_3"    "${_stage_dir}/vulkan/ps"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${_bls_out_14}/shaders/vertex/spv_1_3"   "${_stage_dir}/vulkan/vs"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${_bls_out_14}/shaders/pixel/wgsl_1_0"   "${_stage_dir}/webgpu/ps"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${_bls_out_14}/shaders/vertex/wgsl_1_0"  "${_stage_dir}/webgpu/vs"
            COMMAND "${CMAKE_COMMAND}" -E touch "${_stage}"
            DEPENDS "${_bls}"
            COMMENT "Wc3Shaders[${variant}]: staging BLS bundles into ${_stage_dir}"
            VERBATIM)

        set(${out_stamp_var} "${_stage}" PARENT_SCOPE)
    endfunction()

    wc3_shader_pipeline(release FALSE "shaders" WC3_RELEASE_STAGE_STAMP)
    set(WC3_ALL_STAGE_STAMPS "${WC3_RELEASE_STAGE_STAMP}")
    if(WDX_BUILD_WC3_DEBUG_SHADERS)
        wc3_shader_pipeline(debug TRUE "debug_shaders" WC3_DEBUG_STAGE_STAMP)
        list(APPEND WC3_ALL_STAGE_STAMPS "${WC3_DEBUG_STAGE_STAMP}")
    endif()

    add_custom_target(Wc3Shaders ALL DEPENDS ${WC3_ALL_STAGE_STAMPS})

    # `cmake --build build --target update-prebuilt-shaders` re-emits the
    # prebuilt/shaders pack from the freshly-built BLS bundles. Run
    # after touching .slang sources, then `git add prebuilt/` and commit
    # so CI / fresh clones can use WDX_USE_PREBUILT_SHADERS=ON without
    # invoking slangc themselves. debug_shaders/ is not mirrored — they
    # weigh ~865 MB and are local-only.
    set(_prebuilt_dir "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt")
    add_custom_target(update-prebuilt-shaders
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_prebuilt_dir}/shaders"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_BINARY_DIR}/standalone/shaders" "${_prebuilt_dir}/shaders"
        COMMENT "Updating ${_prebuilt_dir}/shaders from latest Wc3Shaders build"
        DEPENDS Wc3Shaders
        VERBATIM)
endif()

# ============================================================================
# Dear ImGui — optional debug UI integration.
#
# When WDX_ENABLE_IMGUI=ON, builds the ImGui core sources as a static lib and
# adds the engine-side BLS-backed renderer adapter (src/renderer/imgui/...) to
# WhiteoutFlakesLib. The renderer uses the engine's imgui.bls shader pair
# (from externals/Wc3Shaders/wc3_shaders/imgui_{vs,ps}.slang) — we do NOT use
# ImGui's own d3d11 / d3d12 / vulkan backends, only its platform backend
# (imgui_impl_glfw) for input.
#
# Hosts that don't call ImGui::CreateContext() pay only the link-time cost of
# the static lib; the engine's per-frame draw hook is a no-op when there is no
# ImGui context. The Max plugin falls in this bucket.
# ============================================================================

option(WDX_ENABLE_IMGUI "Build with Dear ImGui debug UI integration" ON)

if(WDX_ENABLE_IMGUI)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/externals/imgui/imgui.cpp")
        message(FATAL_ERROR
            "WDX_ENABLE_IMGUI=ON but externals/imgui/ is empty. "
            "Initialize the submodule with: git submodule update --init externals/imgui")
    endif()

    add_library(imgui STATIC
        externals/imgui/imgui.cpp
        externals/imgui/imgui_draw.cpp
        externals/imgui/imgui_tables.cpp
        externals/imgui/imgui_widgets.cpp
        externals/imgui/imgui_demo.cpp
    )
    target_include_directories(imgui PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/externals/imgui"
        "${CMAKE_CURRENT_SOURCE_DIR}/externals/imgui/backends"
    )
    set_target_properties(imgui PROPERTIES
        CXX_STANDARD          17
        CXX_STANDARD_REQUIRED ON
    )
    if(MSVC)
        # ImGui targets warning-clean upstream but the engine compiles at /W3
        # with /wd4244 etc. Keep the same suppressions for visual parity.
        target_compile_options(imgui PRIVATE /W3 /EHsc /wd4244 /wd4267)
    endif()

    # ImGui's GLFW platform backend is a separate translation unit so the
    # engine library (which links imgui core) doesn't drag in GLFW. Only
    # the basic_viewer compiles it via add_library below.
endif()

# ---- Static library: GFX Service (backend-agnostic abstraction) ----
# The Vulkan backend compiles in whenever the Vulkan SDK is installed
# (find_package(Vulkan) succeeds). It targets Vulkan 1.3 + dynamic
# rendering and pulls VMA from the SDK's bundled vk_mem_alloc.h.
find_package(Vulkan QUIET)

# WebGPU/Dawn backend — opt-in (Dawn is a multi-minute first build).
# See WebGPU.md for the design. When ON, fetch a pinned Dawn revision via
# FetchContent and compile in src/gfx/webgpu/*.cpp.
option(WDX_ENABLE_WEBGPU "Enable WebGPU/Dawn backend (fetches Dawn via FetchContent)" OFF)

# Native Metal backend — macOS only. Default ON on Apple platforms so a
# stock checkout on macOS gets a native gfx path without manual flags.
# Forced OFF elsewhere because Metal.framework only ships with Xcode.
if(APPLE)
    option(WDX_ENABLE_METAL "Enable native Metal backend (macOS only)" ON)
else()
    set(WDX_ENABLE_METAL OFF CACHE BOOL "Enable native Metal backend (macOS only)" FORCE)
endif()

set(WHITEOUTFLAKES_GFX_SOURCES
    src/gfx/gfx_factory.cpp
)
# D3D11/D3D12 are Windows-only.
if(WIN32)
    list(APPEND WHITEOUTFLAKES_GFX_SOURCES
        src/gfx/d3d11/d3d11_device.cpp
        src/gfx/d3d11/d3d11_command_list.cpp
        src/gfx/d3d12/d3d12_device.cpp
        src/gfx/d3d12/d3d12_command_list.cpp
        src/gfx/d3d12/d3d12_resources.cpp
    )
endif()
if(Vulkan_FOUND)
    list(APPEND WHITEOUTFLAKES_GFX_SOURCES
        src/gfx/vulkan/vulkan_device.cpp
        src/gfx/vulkan/vulkan_init.cpp
        src/gfx/vulkan/vulkan_buffer.cpp
        src/gfx/vulkan/vulkan_texture.cpp
        src/gfx/vulkan/vulkan_pipeline.cpp
        src/gfx/vulkan/vulkan_command_list.cpp
        src/gfx/vulkan/vulkan_delete_queue.cpp
        src/gfx/vulkan/vulkan_swap_chain.cpp
        src/gfx/vulkan/vulkan_vma.cpp
    )
endif()
if(WDX_ENABLE_WEBGPU)
    list(APPEND WHITEOUTFLAKES_GFX_SOURCES
        src/gfx/webgpu/webgpu_device.cpp
        src/gfx/webgpu/webgpu_init.cpp
        src/gfx/webgpu/webgpu_buffer.cpp
        src/gfx/webgpu/webgpu_texture.cpp
        src/gfx/webgpu/webgpu_pipeline.cpp
        src/gfx/webgpu/webgpu_command_list.cpp
        src/gfx/webgpu/webgpu_delete_queue.cpp
        src/gfx/webgpu/webgpu_swap_chain.cpp
    )
    if(APPLE)
        # ObjC++ shim that hands Dawn a CAMetalLayer attached to GLFW's
        # NSWindow contentView (Dawn's SurfaceSourceMetalLayer can't be
        # populated from pure C++). Cocoa + QuartzCore are linked further
        # down where the WebGPU target wires up.
        list(APPEND WHITEOUTFLAKES_GFX_SOURCES
            src/gfx/webgpu/webgpu_surface_macos.mm
        )
    endif()
endif()

# Metal backend (Apple only). All Obj-C++ — pure C++ TUs see only the
# PIMPL'd MetalDevice. Framework linkage happens just below where the
# WhiteoutFlakesGfx target is finalized.
set(WHITEOUTFLAKES_GFX_METAL_MM_SOURCES)
if(WDX_ENABLE_METAL AND APPLE)
    set(WHITEOUTFLAKES_GFX_METAL_MM_SOURCES
        src/gfx/metal/metal_device.mm
        src/gfx/metal/metal_init.mm
        src/gfx/metal/metal_command_list.mm
        src/gfx/metal/metal_swap_chain.mm
        src/gfx/metal/metal_surface_macos.mm
        src/gfx/metal/metal_buffer.mm
        src/gfx/metal/metal_texture.mm
        src/gfx/metal/metal_pipeline.mm
        src/gfx/metal/metal_delete_queue.mm
    )
    list(APPEND WHITEOUTFLAKES_GFX_SOURCES
        ${WHITEOUTFLAKES_GFX_METAL_MM_SOURCES}
    )
endif()

add_library(WhiteoutFlakesGfx STATIC ${WHITEOUTFLAKES_GFX_SOURCES})

set_target_properties(WhiteoutFlakesGfx PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(WhiteoutFlakesGfx PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(WhiteoutFlakesGfx PUBLIC whiteout_lib)
# D3D libs are Windows-only.
if(WIN32)
    target_link_libraries(WhiteoutFlakesGfx PRIVATE d3d11.lib d3d12.lib dxgi.lib)
endif()

# Tracy lives on WhiteoutFlakesGfx PUBLIC so consumers of the gfx
# library (the renderer, tools) pick up TRACY_ENABLE transitively and
# their ZoneScoped / TracyVkZone macros expand the same way as ours.
if(WDX_TRACY_LIB)
    target_link_libraries(WhiteoutFlakesGfx PUBLIC ${WDX_TRACY_LIB})
endif()

if(Vulkan_FOUND)
    message(STATUS "WhiteoutFlakesGfx: Vulkan = ${Vulkan_LIBRARIES} "
                   "(SDK=${Vulkan_INCLUDE_DIRS})")
    target_link_libraries(WhiteoutFlakesGfx PRIVATE Vulkan::Vulkan)
    target_compile_definitions(WhiteoutFlakesGfx PRIVATE
        WDX_HAS_VULKAN=1
        # vulkan.hpp config: no exceptions, default-dispatch (the dynamic
        # dispatcher would let us use the loader without linking
        # vulkan-1.lib, but we already link it). Result-based error
        # handling keeps with the codebase's no-throw style.
        VULKAN_HPP_NO_EXCEPTIONS
        VULKAN_HPP_NO_SMART_HANDLE
        # Removes the explicit constructors on vk::*CreateInfo / Desc
        # structs so they become aggregates — designated initializers
        # (`{ .field = value }`) become legal. sType is still default-
        # initialised correctly via the in-class initializer.
        VULKAN_HPP_NO_CONSTRUCTORS
    )
    # Platform-specific surface extensions: Win32 surface on Windows; on Linux
    # we drive the surface from GLFW (glfwCreateWindowSurface) which auto-
    # picks XCB/Xlib/Wayland — no platform define needed in the engine itself.
    if(WIN32)
        target_compile_definitions(WhiteoutFlakesGfx PRIVATE
            VK_USE_PLATFORM_WIN32_KHR=1
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
    endif()
    # VMA ships under <Vulkan SDK>/Include/vma/vk_mem_alloc.h. The single
    # vulkan_vma.cpp TU defines VMA_IMPLEMENTATION before including it;
    # everywhere else we include without the implementation macro.
    target_include_directories(WhiteoutFlakesGfx PRIVATE
        "${Vulkan_INCLUDE_DIRS}/vma"
    )
else()
    message(STATUS "WhiteoutFlakesGfx: Vulkan SDK not found — Vulkan backend disabled")
    target_compile_definitions(WhiteoutFlakesGfx PRIVATE WDX_HAS_VULKAN=0)
endif()

# ---- WebGPU / Dawn backend ----
#
# Dawn is fetched through eliemichel/WebGPU-distribution rather than
# upstream dawn.googlesource.com. Upstream Dawn runs
# `tools/fetch_dawn_dependencies.py` at configure time, which reads
# Dawn's DEPS file and tries to clone everything — including Chromium
# tooling on chrome-internal.googlesource.com (Google-only). That repo
# triggers a Git Credential Manager prompt and ultimately fails for
# non-Googlers. WebGPU-distribution sidesteps the whole mess by
# downloading prebuilt Dawn binaries (DLLs + headers).
#
# Headers exposed (the prebuilt zip includes all of Dawn's public
# headers + eliemichel's wrapper):
#   <webgpu/webgpu.h>     — Dawn-flavored C API (full surface incl. extensions)
#   <webgpu/webgpu_cpp.h> — Dawn's PascalCase C++ wrapper (the one we use)
#   <webgpu/webgpu.hpp>   — eliemichel's camelCase wrapper (unused)
# We pick webgpu_cpp.h to keep parity with upstream Dawn samples and the
# original FetchContent-from-source attempt — see webgpu_init.cpp for
# the async RequestAdapter / RequestDevice pattern we use instead of
# Dawn's private (and unshipped) dawn::native sync helpers.
if(WDX_ENABLE_WEBGPU)
    if(EMSCRIPTEN)
        # Emscripten ships emdawnwebgpu — Dawn-flavoured WebGPU bindings
        # for the browser. Same <webgpu/webgpu_cpp.h> surface our backend
        # already targets, so the gfx sources compile unchanged modulo the
        # surface-creation and device-acquisition #ifdefs in webgpu_init.cpp
        # and webgpu_swap_chain.cpp. No GLFW link here — JS owns the canvas.
        target_compile_options(WhiteoutFlakesGfx PUBLIC "--use-port=emdawnwebgpu")
        target_link_options(WhiteoutFlakesGfx PUBLIC "--use-port=emdawnwebgpu")
        target_compile_definitions(WhiteoutFlakesGfx PUBLIC WDX_HAS_WEBGPU=1)
        message(STATUS "WhiteoutFlakesGfx: WebGPU enabled "
                       "(Emscripten emdawnwebgpu port)")
    else()
        include(FetchContent)
        # Pin to a specific commit — README warns against tracking branch tip.
        # Bump deliberately; see WebGPU.md §13.
        set(WDX_WEBGPU_DIST_TAG "main" CACHE STRING
            "WebGPU-distribution revision (commit or tag)")
        FetchContent_Declare(webgpu
            GIT_REPOSITORY https://github.com/eliemichel/WebGPU-distribution
            GIT_TAG        ${WDX_WEBGPU_DIST_TAG}
            GIT_SHALLOW    TRUE
        )
        # DAWN backend, prebuilt — no fetch script, no compile.
        set(WEBGPU_BACKEND          DAWN CACHE STRING "" FORCE)
        set(WEBGPU_BUILD_FROM_SOURCE OFF  CACHE BOOL   "" FORCE)
        FetchContent_MakeAvailable(webgpu)

        message(STATUS "WhiteoutFlakesGfx: WebGPU enabled "
                       "(eliemichel/WebGPU-distribution ${WDX_WEBGPU_DIST_TAG}, prebuilt Dawn)")
        target_link_libraries(WhiteoutFlakesGfx PRIVATE
            webgpu
            # GLFW: the WebGPU swap-chain pulls native handles (Display/Window
            # pair on X11, wl_display/wl_surface on Wayland, NSWindow on
            # macOS) via glfw3native.h. The target is added later via
            # add_subdirectory; CMake defers symbol resolution to generate
            # time so the forward reference resolves cleanly.
            glfw
        )
        target_compile_definitions(WhiteoutFlakesGfx PUBLIC WDX_HAS_WEBGPU=1)
        if(WIN32)
            target_compile_definitions(WhiteoutFlakesGfx PRIVATE
                NOMINMAX
                WIN32_LEAN_AND_MEAN
            )
        elseif(APPLE)
            # SurfaceSourceMetalLayer needs the CAMetalLayer that the .mm
            # shim attaches to the NSWindow contentView — pull in Cocoa for
            # NSWindow/NSView and QuartzCore for CAMetalLayer.
            target_link_libraries(WhiteoutFlakesGfx PRIVATE
                "-framework Cocoa"
                "-framework QuartzCore"
            )
        endif()
    endif()
else()
    target_compile_definitions(WhiteoutFlakesGfx PUBLIC WDX_HAS_WEBGPU=0)
endif()

# ---- Metal backend wiring ----
# Frameworks: Metal (id<MTLDevice> + all rendering APIs), QuartzCore
# (CAMetalLayer for the swap chain), Foundation/Cocoa (NSWindow handle
# from GLFW), MetalKit reserved for future MTKTextureLoader use.
# `-fobjc-arc` is scoped to the .mm files only via set_source_files_properties
# so the pure-C++ TUs (factory, etc.) don't see the flag.
if(WDX_ENABLE_METAL AND APPLE)
    target_link_libraries(WhiteoutFlakesGfx PRIVATE
        "-framework Metal"
        "-framework MetalKit"
        "-framework QuartzCore"
        "-framework Foundation"
        "-framework Cocoa"
    )
    target_compile_definitions(WhiteoutFlakesGfx PUBLIC WDX_HAS_METAL=1)
    if(WHITEOUTFLAKES_GFX_METAL_MM_SOURCES)
        set_source_files_properties(${WHITEOUTFLAKES_GFX_METAL_MM_SOURCES}
            PROPERTIES COMPILE_FLAGS "-fobjc-arc"
        )
    endif()
    message(STATUS "WhiteoutFlakesGfx: Metal backend enabled")
else()
    target_compile_definitions(WhiteoutFlakesGfx PUBLIC WDX_HAS_METAL=0)
endif()

if(MSVC)
    target_compile_options(WhiteoutFlakesGfx PRIVATE
        /W3 /EHsc
        /wd4100 /wd4127 /wd4244 /wd4267
    )
endif()

# ---- Renderer-native coordinate system (compile-time knob) ----
set(WDX_DEFAULT_COORD_SPACE "Blizzard" CACHE STRING
    "Renderer-native coordinate system. Valid values: Blizzard, Max.")
set_property(CACHE WDX_DEFAULT_COORD_SPACE PROPERTY STRINGS "Blizzard" "Max")

# ---- Static library: standalone renderer + MDX adapter ----
add_library(WhiteoutFlakesLib STATIC
    src/renderer/animation/actor_eval.cpp
    src/renderer/animation/animation_driver.cpp
    src/renderer/frame_ticker.cpp
    src/renderer/model/model_loader.cpp
    src/renderer/model/model_template.cpp
    src/renderer/model/model_template_manager.cpp
    src/renderer/model/render_model.cpp
    src/renderer/render_pipeline.cpp
    src/renderer/frame_capture.cpp
    src/renderer/render_service.cpp
    src/renderer/render_detail.cpp
    src/renderer/render_target.cpp
    src/renderer/scene_manager.cpp
    src/renderer/coordinate_system.cpp
    src/renderer/assets/sampler_asset_manager.cpp
    src/renderer/assets/texture_asset_manager.cpp
    src/renderer/assets/replaceable_texture_manager.cpp
    src/renderer/assets/asset_manager.cpp
    src/renderer/debug/debug_renderer.cpp
    src/renderer/effects/event_emitter_pool.cpp
    src/renderer/effects/pe1_system.cpp
    src/renderer/effects/ribbon.cpp
    src/renderer/effects/spn_spawner.cpp
    src/renderer/corn_effects/corn_effects_emitter.cpp
    src/renderer/corn_effects/corn_effects_gfx_backend.cpp
    src/renderer/corn_effects/corn_effects_service.cpp
    ${CORNFLAKES_SOURCES}
    src/renderer/particle/rnd_seed.cpp
    src/renderer/particle/particle_key.cpp
    src/renderer/particle/particle_pool.cpp
    src/renderer/particle/particle2_emitter.cpp
    src/renderer/particle/plane_emitter.cpp
    src/renderer/particle/particle_geometry.cpp
    src/renderer/particle/particle_service.cpp
    src/renderer/particle/splat_service.cpp
    src/renderer/dnc/dnc_asset.cpp
    src/renderer/dnc/dnc_cache.cpp
    src/renderer/dnc/dnc_service.cpp
    src/renderer/shadow/shadow_service.cpp
    src/renderer/shadow/shadow_pass.cpp
    src/renderer/bls/bls_container.cpp
    src/renderer/bls/bls_shader_cache.cpp
    src/renderer/bls/bls_permuter.cpp
    src/renderer/bls/bls_program.cpp
    src/renderer/bls/bls_pso_builder.cpp
    src/renderer/bls/bls_pso_trace.cpp
    src/renderer/bls/bls_mat_params.cpp
    src/renderer/bls/bls_frame.cpp
    src/renderer/bls/bls_draw_helpers.cpp
    src/renderer/ibl/split_sum.cpp
    src/renderer/ibl/env_probe.cpp
    src/io/mdx_animation.cpp
    src/io/mdx_model_adapter.cpp
    src/io/team_glow_data.cpp
    src/io/file_resolver.cpp
    src/io/file_content_provider.cpp
    src/io/fetch_content_provider.cpp
    src/io/content_provider.cpp
    src/io/path_utf8.cpp
    src/io/replaceable_paths.cpp
    src/io/event_data.cpp
    src/io/slk.cpp
    src/io/texture_image_usage.cpp
    src/public_api/renderer_api.cpp
)

if(WDX_ENABLE_IMGUI)
    target_sources(WhiteoutFlakesLib PRIVATE
        src/renderer/imgui/imgui_renderer.cpp
    )
endif()

add_dependencies(WhiteoutFlakesLib WhiteoutFlakesShaders WhiteoutFlakesEmbedded)

set_target_properties(WhiteoutFlakesLib PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(WhiteoutFlakesLib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/io"
    "${SHADER_OUT_DIR}"
    "${EMBED_OUT_DIR}"
)
# cornflakes module include layout:
#   src/renderer/corn_effects          → so <cornflakes/interface/...> resolves
#                                    (public headers used by corn-fx host code).
#   cornflakes/internal           → so <cornflakes/...> resolves to the
#                                    private impl-side headers (e.g.
#                                    cornflakes/asset/field_defs.hpp lives at
#                                    cornflakes/internal/cornflakes/asset/...).
target_include_directories(WhiteoutFlakesLib PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/corn_effects"
    "${CORNFLAKES_MODULE_DIR}/internal"
)

# cornflakes' core/determinism.hpp asserts CORNFLAKES_DETERMINISM_FLAGS_APPLIED
# is defined as 1 in every TU that includes it. Upstream's INTERFACE target
# also pins MSVC FP semantics to /fp:precise /fp:except- to keep corn-fx
# evaluation bit-deterministic. Since cornflakes is now part of WhiteoutFlakesLib,
# we apply the same flags to the whole library.
target_compile_definitions(WhiteoutFlakesLib PRIVATE
    CORNFLAKES_DETERMINISM_FLAGS_APPLIED=1
)
if(MSVC)
    target_compile_options(WhiteoutFlakesLib PRIVATE /fp:precise /fp:except-)
endif()

target_link_libraries(WhiteoutFlakesLib PUBLIC
    whiteout_lib
    WhiteoutFlakesGfx
)

if(WIN32)
    target_link_libraries(WhiteoutFlakesLib PUBLIC user32.lib gdi32.lib comdlg32.lib)
endif()

target_compile_definitions(WhiteoutFlakesLib PUBLIC
    WDX_DEFAULT_COORD_SPACE=${WDX_DEFAULT_COORD_SPACE}
)

if(WDX_ENABLE_IMGUI)
    target_link_libraries(WhiteoutFlakesLib PUBLIC imgui)
    target_compile_definitions(WhiteoutFlakesLib PUBLIC WDX_ENABLE_IMGUI=1)
else()
    target_compile_definitions(WhiteoutFlakesLib PUBLIC WDX_ENABLE_IMGUI=0)
endif()

if(TARGET whiteout_casc)
    target_link_libraries(WhiteoutFlakesLib PUBLIC whiteout_casc)
endif()

if(TARGET whiteout_mpq)
    target_link_libraries(WhiteoutFlakesLib PUBLIC whiteout_mpq)
endif()

if(MSVC)
    target_compile_options(WhiteoutFlakesLib PRIVATE
        /W3 /EHsc
        /wd4100 /wd4127 /wd4244 /wd4267
    )
endif()

# ---- Throwaway: dump a BLS permute's disassembly ----
# Windows-only: links d3d11.lib + d3dcompiler.lib + includes <d3d11.h>,
# none of which exist outside the Win32 SDK. Skipped under Emscripten and
# on non-Windows hosts so the macOS/Linux gfx build succeeds.
if(WIN32 AND NOT EMSCRIPTEN AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/bls/bls_reflect_tool.cpp")
  add_executable(BlsReflect
      src/renderer/bls/bls_reflect_tool.cpp
      src/renderer/bls/bls_container.cpp
  )
  set_target_properties(BlsReflect PROPERTIES
      CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON
      RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/standalone"
      RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/standalone"
  )
  target_include_directories(BlsReflect PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/src"
      "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer")
  target_link_libraries(BlsReflect PRIVATE whiteout_lib d3d11.lib d3dcompiler.lib)
  if(MSVC)
    target_compile_options(BlsReflect PRIVATE /W3 /EHsc /wd4100 /wd4127 /wd4244 /wd4267)
  endif()
endif()

# ---- Standalone executable: basic MDX viewer ----
#
# The viewer drives the engine through GLFW (window + input) and Dear ImGui
# (every toolbar / menu / settings widget that used to be a Win32 control).
# Both must be enabled — WDX_ENABLE_IMGUI=OFF would leave the viewer with no
# UI surface at all. The entire block is gated on `NOT EMSCRIPTEN` (no
# native windowing/audio/UI in the web build); the closing `endif()` sits
# just before the Linux AppImage section.
if(NOT EMSCRIPTEN)

if(NOT WDX_ENABLE_IMGUI)
    message(FATAL_ERROR
        "WhiteoutFlakesStandalone requires WDX_ENABLE_IMGUI=ON; the viewer's UI "
        "is entirely Dear ImGui. Disable the viewer (don't add it to the build) "
        "if you want WDX_ENABLE_IMGUI=OFF.")
endif()

# GLFW — windowing + input. Bundled docs / tests / examples are off so the
# library compiles in a few seconds and doesn't drop binaries into the
# build tree.
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
add_subdirectory(externals/glfw EXCLUDE_FROM_ALL)

# NFD — native file open / save dialogs. Linux builds want GTK/portal; on
# Windows it's IFileOpenDialog and has no extra deps.
add_subdirectory(externals/nativefiledialog-extended EXCLUDE_FROM_ALL)

# cubeb — cross-platform audio output (WASAPI / PulseAudio / AudioUnit).
# Drives CubebSoundEmitter, shared by the viewer and the Max plugin. The
# bundled tests / tools / sanitizer wiring are forced off so it builds as
# a lean static lib; the Rust backends stay off (the C++ WASAPI backend
# covers Windows). The `cubeb` target's bundled speex resampler is linked
# PRIVATE inside cubeb itself — CubebSoundEmitter does its own (linear)
# resampling so it doesn't reach into cubeb internals.
set(BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(BUILD_TOOLS     OFF CACHE BOOL "" FORCE)
set(BUILD_RUST_LIBS OFF CACHE BOOL "" FORCE)
set(USE_SANITIZERS  OFF CACHE BOOL "" FORCE)
add_subdirectory(externals/cubeb EXCLUDE_FROM_ALL)

set(WHITEOUTFLAKES_VIEWER_SOURCES
    tools/basic_viewer/test_main.cpp
    tools/basic_viewer/viewer_app.cpp
    tools/basic_viewer/viewer_ui.cpp
    tools/basic_viewer/settings_ini.cpp
    # Shared host utilities live in tools/common (used by the Max plugin too).
    tools/common/cubeb_sound_emitter.cpp
    tools/common/imgui_theme.cpp
    # ImGui's GLFW platform backend lives with the viewer rather than the
    # imgui core lib because it pulls in <GLFW/glfw3.h> — the engine library
    # itself only links the renderer-agnostic ImGui core.
    externals/imgui/backends/imgui_impl_glfw.cpp
)
# Win32 .rc — embeds resources/flakes_icon.ico as IDI_WHITEOUT_ICON so the
# viewer can hand it to the GLFW HWND via WM_SETICON. On Linux we instead
# load the same icon via glfwSetWindowIcon at runtime (see ViewerApp::Open).
if(WIN32)
    list(APPEND WHITEOUTFLAKES_VIEWER_SOURCES tools/basic_viewer/resource.rc)
endif()
# macOS: ObjC++ shim that creates the CAMetalLayer + Vulkan surface in
# the order MoltenVK expects (wantsLayer=YES before setLayer:). Replaces
# glfwCreateWindowSurface, which trips VK_ERROR_INITIALIZATION_FAILED on
# macOS 13+ / MoltenVK 1.4. cocoa_window_macos.mm tints the NSWindow's
# title bar to match the ImGui MenuBarBg — Cocoa parity for the Windows
# DwmSetWindowAttribute path in viewer_app.cpp.
if(APPLE)
    list(APPEND WHITEOUTFLAKES_VIEWER_SOURCES
        tools/basic_viewer/vulkan_surface_macos.mm
        tools/basic_viewer/cocoa_window_macos.mm
    )
endif()
add_executable(WhiteoutFlakesStandalone ${WHITEOUTFLAKES_VIEWER_SOURCES})

set_target_properties(WhiteoutFlakesStandalone PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    OUTPUT_NAME           "WhiteoutFlakes"
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/standalone"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/standalone"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/standalone"
)

target_include_directories(WhiteoutFlakesStandalone PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/basic_viewer"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/common"
    "${CMAKE_CURRENT_SOURCE_DIR}/externals/dr_libs"
)

target_link_libraries(WhiteoutFlakesStandalone PRIVATE
    WhiteoutFlakesLib
    glfw
    nfd
    imgui
    cubeb
)

# ---- Animated WebP export (viewer only) ----
# libwebp is fetched and built at configure time, and linked into the viewer
# alone — no other WhiteoutFlakes target (engine, gfx, lib) depends on it. Only
# libwebp's encoder + mux libraries are built; its command-line tools are
# disabled, so it needs no libpng/libjpeg/libtiff. The viewer's WebP code is
# gated on WDX_HAVE_WEBP and falls back to a clear error when this is OFF.
option(WDX_ENABLE_WEBP
    "Enable animated WebP export in the viewer (fetches libwebp)" ON)
if(WDX_ENABLE_WEBP)
    include(FetchContent)
    set(WEBP_BUILD_ANIM_UTILS OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_CWEBP      OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_DWEBP      OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_GIF2WEBP   OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_IMG2WEBP   OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_VWEBP      OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_WEBPINFO   OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_WEBPMUX    OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_EXTRAS     OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_FUZZTEST   OFF CACHE BOOL "" FORCE)
    set(WEBP_BUILD_LIBWEBPMUX ON  CACHE BOOL "" FORCE) # WebPAnimEncoder lives here
    FetchContent_Declare(libwebp
        GIT_REPOSITORY https://github.com/webmproject/libwebp.git
        GIT_TAG        v1.5.0
        GIT_SHALLOW    TRUE
    )
    # Build libwebp as STATIC libraries regardless of the project-wide
    # BUILD_SHARED_LIBS, so the viewer embeds the encoder/mux directly and
    # ships no libwebp.dll. BUILD_SHARED_LIBS is shadowed as a normal variable
    # for libwebp's add_subdirectory scope, then restored.
    set(_wdx_prev_bsl_set FALSE)
    if(DEFINED BUILD_SHARED_LIBS)
        set(_wdx_prev_bsl_set TRUE)
        set(_wdx_prev_bsl "${BUILD_SHARED_LIBS}")
    endif()
    set(BUILD_SHARED_LIBS OFF)

    # libwebp registers install() rules for every one of its targets with no
    # opt-out. FetchContent leaves the ones we don't link (webpdecoder,
    # webpdemux) out of the build, so `cmake --install` would then fail trying
    # to stage artefacts that were never built. We link libwebp statically
    # into the viewer alone and never want it in WhiteoutFlakes' install tree,
    # so neutralise install() for the duration of libwebp's add_subdirectory.
    # Single override → _install is reliably the builtin; a flag re-enables it
    # afterwards so WhiteoutFlakes' own install rules still run.
    set(_wdx_suppress_install TRUE)
    function(install)
        if(NOT _wdx_suppress_install)
            _install(${ARGV})
        endif()
    endfunction()

    FetchContent_MakeAvailable(libwebp)

    set(_wdx_suppress_install FALSE)
    if(_wdx_prev_bsl_set)
        set(BUILD_SHARED_LIBS "${_wdx_prev_bsl}")
    else()
        unset(BUILD_SHARED_LIBS)
    endif()
    target_link_libraries(WhiteoutFlakesStandalone PRIVATE webp libwebpmux)
    target_include_directories(WhiteoutFlakesStandalone SYSTEM PRIVATE
        "${libwebp_SOURCE_DIR}/src")
    target_compile_definitions(WhiteoutFlakesStandalone PRIVATE WDX_HAVE_WEBP=1)
    message(STATUS
        "WhiteoutFlakesStandalone: animated WebP export enabled (libwebp v1.5.0)")
endif()

# ---- metal_smoke ----
# Tiny diagnostic tool that opens a GLFW window, asks gfx for a Metal
# device + swap chain, and runs a magenta clear loop. Bypasses the
# renderer entirely so it can run before the BLS / pipeline / asset
# wiring is finished. Used to verify the Metal backend's swap-chain +
# render-pass + Present path during bring-up.
if(WDX_ENABLE_METAL AND APPLE)
    add_executable(metal_smoke tools/metal_smoke/metal_smoke.cpp)
    set_target_properties(metal_smoke PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/standalone"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/standalone"
    )
    target_link_libraries(metal_smoke PRIVATE WhiteoutFlakesGfx glfw)
endif()

# Viewer includes <vulkan/vulkan.h> unconditionally (for the Linux surface
# path via glfwCreateWindowSurface). On Windows the include compiles but
# the Vulkan-using code is gated behind `#if !defined(_WIN32)`; we still
# need the headers + import lib on PATH so the TU compiles.
if(Vulkan_FOUND)
    target_link_libraries(WhiteoutFlakesStandalone PRIVATE Vulkan::Vulkan)
elseif(UNIX AND NOT APPLE)
    message(FATAL_ERROR
        "WhiteoutFlakesStandalone requires the Vulkan SDK on Linux (it's the "
        "only swap-chain backend on this platform). Install vulkan-sdk or "
        "libvulkan-dev + headers and re-run CMake.")
endif()

# WebGPU-distribution ships prebuilt Dawn as a DLL — copy it next to the
# exe so the loader finds it at runtime. The helper is a no-op when the
# backend is built statically; see WebGPU.md §10.
if(WDX_ENABLE_WEBGPU)
    target_copy_webgpu_binaries(WhiteoutFlakesStandalone)
endif()

if(MSVC)
    target_compile_options(WhiteoutFlakesStandalone PRIVATE
        /W3 /EHsc
        /wd4100 /wd4127 /wd4244 /wd4267
    )
endif()

if(WDX_BUILD_WC3_SHADERS OR WDX_USE_PREBUILT_SHADERS)
    add_dependencies(WhiteoutFlakesStandalone Wc3Shaders)
endif()

# imgui_theme.cpp is compiled directly into the viewer and pulls in the
# embedded Roboto header generated by WhiteoutFlakesEmbedded.
add_dependencies(WhiteoutFlakesStandalone WhiteoutFlakesEmbedded)

# ---- Optional: 3ds Max plugin (.dlx) ----
option(WDX_BUILD_MAX_PLUGIN
    "Build the WhiteoutFlakes 3ds Max plugin (requires Max SDK + -DMAX_VERSION)"
    OFF)

if(WDX_BUILD_MAX_PLUGIN)
    if(NOT WIN32)
        message(FATAL_ERROR "WDX_BUILD_MAX_PLUGIN=ON requires Windows (3ds Max SDK is Windows-only).")
    endif()
    add_subdirectory(tools/max_plugin)
endif()

# ---- CASC dev server -----------------------------------------------------
# Opt-in Crow HTTP server that fronts WhiteoutLib's CASC reader, used to
# feed real WC3 assets to the WASM viewer over a local origin (imitates
# Hiveworkshop's CASC-backed asset delivery). The Crow dep is fetched
# inside the subdirectory so it's scoped to this target only.
option(WDX_BUILD_CASC_SERVER
    "Build the wf_casc_server tool (Crow HTTP server + WhiteoutLib CASC)"
    OFF)
if(WDX_BUILD_CASC_SERVER)
    if(NOT WHITEOUT_ENABLE_CASC)
        message(FATAL_ERROR
            "WDX_BUILD_CASC_SERVER=ON requires WHITEOUT_ENABLE_CASC=ON.")
    endif()
    # Lives under tools/web_viewer/ since it exists to feed the web viewer
    # — CASC-backed loose-file delivery imitating Hiveworkshop's service.
    add_subdirectory(tools/web_viewer/casc_server)
endif()

endif()  # NOT EMSCRIPTEN — standalone viewer block

# ---- Web viewer: WASM module + JS facade (Emscripten only) ----
if(EMSCRIPTEN)
    add_subdirectory(tools/web_viewer)
endif()

# ============================================================================
# Linux AppImage staging
#
# `cmake --install build --prefix AppDir/usr` populates the AppDir layout
# linuxdeploy expects. The AppImage runs the binary from AppDir/usr/bin and
# the engine's exe-dir search root picks up the staged BLS files next to it
# (no special XDG lookup needed). The .desktop file + icon get installed to
# AppDir/usr/share/{applications,icons/hicolor/256x256/apps}.
# ============================================================================
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    include(GNUInstallDirs)

    install(TARGETS WhiteoutFlakesStandalone
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

    # BLS shader bundles staged by the Wc3Shaders custom target sit at
    # ${CMAKE_BINARY_DIR}/standalone/shaders (and optionally debug_shaders).
    # Install them next to the binary so FileContentProvider's exe-dir
    # search root resolves them with no path config. Both WDX_BUILD_WC3_SHADERS
    # and WDX_USE_PREBUILT_SHADERS produce the same staging layout, so this
    # install fires for either source.
    if(WDX_BUILD_WC3_SHADERS OR WDX_USE_PREBUILT_SHADERS)
        install(DIRECTORY "${CMAKE_BINARY_DIR}/standalone/shaders/"
                DESTINATION ${CMAKE_INSTALL_BINDIR}/shaders
                OPTIONAL)
        if(WDX_BUILD_WC3_DEBUG_SHADERS)
            install(DIRECTORY "${CMAKE_BINARY_DIR}/standalone/debug_shaders/"
                    DESTINATION ${CMAKE_INSTALL_BINDIR}/debug_shaders
                    OPTIONAL)
        endif()
    endif()

    # AppImage / XDG installs need the icon basename to match the .desktop's
    # `Icon=WhiteoutFlakes` directive, so we rename the source file
    # (flakes_icon_linux.png is the right size for this slot) at install time.
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/flakes_icon_linux.png"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
            RENAME WhiteoutFlakes.png)

    # Pre-baked PSO replay trace: seeds the Vulkan pipeline cache on first
    # launch so the user doesn't pay the cold-build hitch. Lives next to the
    # binary inside the AppImage; test_main copies it into $XDG_CACHE_HOME/
    # WhiteoutFlakes/ if no per-user trace exists yet (subsequent runs save
    # their own additions there).
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/pso_trace.bin")
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/pso_trace.bin"
                DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/packaging/linux/WhiteoutFlakes.desktop"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif()

# ============================================================================
# macOS .app bundle staging
#
# `cmake --install build --prefix <stagedir>` produces
# <stagedir>/WhiteoutFlakes.app/ with the standard layout:
#   Contents/MacOS/WhiteoutFlakes        ← executable
#   Contents/Resources/WhiteoutFlakes.icns
#   Contents/Resources/shaders/...        ← prebuilt BLS bundle
#   Contents/Resources/pso_trace.bin
#   Contents/Frameworks/                  ← MoltenVK + libvulkan + ICD
#   Contents/Info.plist
# Dylib + ICD bundling lives in the GitHub Actions YAML (dylibbundler /
# install_name_tool / hand-copy from the LunarG SDK) since it depends on
# the runner's SDK install path. The CMake side just builds the skeleton.
# ============================================================================
if(APPLE)
    # Mark the executable as a macOS bundle. CMake then arranges the build
    # output as `WhiteoutFlakes.app/Contents/MacOS/WhiteoutFlakes` directly
    # (no manual mkdir / mv needed).
    set_target_properties(WhiteoutFlakesStandalone PROPERTIES
        MACOSX_BUNDLE                          TRUE
        MACOSX_BUNDLE_BUNDLE_NAME              "WhiteoutFlakes"
        MACOSX_BUNDLE_GUI_IDENTIFIER           "com.whiteoutflakes.viewer"
        MACOSX_BUNDLE_BUNDLE_VERSION           "0.1.0"
        MACOSX_BUNDLE_SHORT_VERSION_STRING     "0.1.0"
        MACOSX_BUNDLE_ICON_FILE                "WhiteoutFlakes.icns"
        MACOSX_BUNDLE_INFO_PLIST
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/Info.plist.in"
        # Bundled dylibs live next to the executable in
        # Contents/Frameworks; @executable_path is the bundle's MacOS dir
        # at runtime, so ../Frameworks resolves correctly. The plain
        # @executable_path entry covers libwebgpu_dawn.dylib, which Dawn's
        # WebGPU-distribution helper drops as a sibling of the exe via
        # target_copy_webgpu_binaries rather than moving it to Frameworks/.
        INSTALL_RPATH                          "@executable_path;@executable_path/../Frameworks"
        BUILD_WITH_INSTALL_RPATH               TRUE
    )

    # Generate WhiteoutFlakes.icns from the iconset committed under
    # resources/FlakesMacOS.iconset/. Apple's iconutil only exists on
    # macOS hosts; we use it as a build-time codegen step.
    set(_iconset_src "${CMAKE_CURRENT_SOURCE_DIR}/resources/FlakesMacOS.iconset")
    set(_icns_out    "${CMAKE_CURRENT_BINARY_DIR}/WhiteoutFlakes.icns")
    add_custom_command(
        OUTPUT  "${_icns_out}"
        COMMAND iconutil -c icns -o "${_icns_out}" "${_iconset_src}"
        DEPENDS "${_iconset_src}"
        COMMENT "macOS: iconutil ${_iconset_src} -> WhiteoutFlakes.icns"
        VERBATIM)
    add_custom_target(WhiteoutFlakesIcns ALL DEPENDS "${_icns_out}")
    add_dependencies(WhiteoutFlakesStandalone WhiteoutFlakesIcns)

    # ---- Install rules -------------------------------------------------
    # Bundle goes at the install root (`WhiteoutFlakes.app/`). Everything
    # except the executable itself lives under Contents/Resources/ —
    # macOS treats anything in Contents/MacOS/ as code and `codesign
    # --force` fails on non-Mach-O files (`.bls` etc.) shipped there.
    # The engine's DiscoverExecutableDirectory() has a .app-aware path:
    # when the exe sits at Contents/MacOS/X, the search root resolves
    # to Contents/Resources/ instead.
    install(TARGETS WhiteoutFlakesStandalone
            BUNDLE  DESTINATION .)

    install(FILES "${_icns_out}"
            DESTINATION WhiteoutFlakes.app/Contents/Resources)

    if(WDX_BUILD_WC3_SHADERS OR WDX_USE_PREBUILT_SHADERS)
        install(DIRECTORY "${CMAKE_BINARY_DIR}/standalone/shaders/"
                DESTINATION WhiteoutFlakes.app/Contents/Resources/shaders
                OPTIONAL)
    endif()

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/pso_trace.bin")
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/pso_trace.bin"
                DESTINATION WhiteoutFlakes.app/Contents/Resources)
    endif()

    # Self-contained bundle: stage the Vulkan loader + MoltenVK driver and
    # rewrite the ICD JSON's library_path to bundle-relative. test_main.cpp
    # sets VK_ICD_FILENAMES to <bundle>/Contents/Resources/vulkan/icd.d/
    # MoltenVK_icd.json at startup, so the loader finds MoltenVK without
    # any system Vulkan SDK on the user's machine — the .app is launchable
    # via Finder/double-click with no env-var prep.
    #
    # Python is needed by packaging/macos/rewrite_icd.py — find it lazily
    # here since WDX_BUILD_WC3_SHADERS already pulled it in earlier if on,
    # but we want this path to work in the common WDX_USE_PREBUILT_SHADERS
    # case too.
    if(NOT Python3_EXECUTABLE)
        find_package(Python3 QUIET COMPONENTS Interpreter)
    endif()
    #
    # Source paths: Vulkan_LIBRARY points at libvulkan.dylib (a symlink to
    # libvulkan.1.dylib on every modern SDK). libMoltenVK + its ICD JSON
    # live next to the loader, but the exact layout differs between LunarG
    # SDK (Frameworks/lib + share/) and Homebrew (split across opt/
    # vulkan-loader and opt/molten-vk). Glob both layouts so a local dev
    # build with brew, and the CI runner with the LunarG SDK installer,
    # both produce the same staged bundle.
    if(Vulkan_FOUND)
        # libvulkan: resolve symlinks so we install the real dylib, not
        # the version-suffixed symlink Homebrew ships at lib/libvulkan.dylib.
        get_filename_component(_wdx_vk_loader_real "${Vulkan_LIBRARY}" REALPATH)
        # libMoltenVK + ICD JSON: walk candidate layouts.
        set(_wdx_mvk_lib "")
        set(_wdx_mvk_icd "")
        foreach(_root
                "/opt/homebrew/opt/molten-vk"                       # brew arm64
                "/usr/local/opt/molten-vk"                          # brew x86_64
                "$ENV{VULKAN_SDK}"                                  # LunarG SDK
                "${Vulkan_INCLUDE_DIR}/.."                          # SDK-style
                "/opt/homebrew/opt/vulkan-loader/.."                # brew sibling
            )
            if(_root AND EXISTS "${_root}")
                if(NOT _wdx_mvk_lib AND EXISTS "${_root}/lib/libMoltenVK.dylib")
                    set(_wdx_mvk_lib "${_root}/lib/libMoltenVK.dylib")
                endif()
                if(NOT _wdx_mvk_icd AND EXISTS "${_root}/share/vulkan/icd.d/MoltenVK_icd.json")
                    set(_wdx_mvk_icd "${_root}/share/vulkan/icd.d/MoltenVK_icd.json")
                endif()
                if(NOT _wdx_mvk_icd AND EXISTS "${_root}/etc/vulkan/icd.d/MoltenVK_icd.json")
                    set(_wdx_mvk_icd "${_root}/etc/vulkan/icd.d/MoltenVK_icd.json")
                endif()
            endif()
        endforeach()
        if(_wdx_mvk_lib AND _wdx_mvk_icd)
            # Order matters: every file the .app should ship must land
            # *before* the bundle-wrapper codesign at the end of this block.
            # If we sign the wrapper and then drop a file inside (e.g. the
            # ICD JSON), the sealed CodeResources manifest no longer matches
            # the on-disk contents and `codesign --verify` reports
            # "a sealed resource is missing or invalid".
            install(CODE "
                # ---- Stage Vulkan loader + MoltenVK ----
                # Force the loader filename to libvulkan.1.dylib so the
                # @rpath/libvulkan.1.dylib load command resolves regardless
                # of whether the source path was a versioned symlink or the
                # canonical Cellar realpath.
                file(INSTALL \"${_wdx_vk_loader_real}\"
                     DESTINATION \"\${CMAKE_INSTALL_PREFIX}/WhiteoutFlakes.app/Contents/Frameworks\"
                     RENAME \"libvulkan.1.dylib\")
                file(INSTALL \"${_wdx_mvk_lib}\"
                     DESTINATION \"\${CMAKE_INSTALL_PREFIX}/WhiteoutFlakes.app/Contents/Frameworks\")
                execute_process(COMMAND install_name_tool -id @rpath/libvulkan.1.dylib
                    \"\${CMAKE_INSTALL_PREFIX}/WhiteoutFlakes.app/Contents/Frameworks/libvulkan.1.dylib\")
                execute_process(COMMAND install_name_tool -id @rpath/libMoltenVK.dylib
                    \"\${CMAKE_INSTALL_PREFIX}/WhiteoutFlakes.app/Contents/Frameworks/libMoltenVK.dylib\")

                # ---- Rewrite the exe's libvulkan load command ----
                # The linker bakes the dylib's LC_ID_DYLIB install_name into
                # the load command, which may be neither Vulkan_LIBRARY nor
                # its realpath — brew's libvulkan ships with install_name =
                # /opt/homebrew/opt/vulkan-loader/lib/libvulkan.1.dylib, the
                # LunarG SDK ships @rpath/... already. Read the actual load
                # command with otool and rewrite whatever's there to @rpath.
                execute_process(
                    COMMAND otool -L
                        \"\${CMAKE_INSTALL_PREFIX}/WhiteoutFlakes.app/Contents/MacOS/WhiteoutFlakes\"
                    OUTPUT_VARIABLE _wdx_otool_out OUTPUT_STRIP_TRAILING_WHITESPACE)
                string(REGEX MATCH \"[^ \\t\\r\\n]+libvulkan\\\\.1\\\\.dylib\"
                    _wdx_exe_libvk \"\${_wdx_otool_out}\")
                message(STATUS \"macOS bundle: exe libvulkan load path = '\${_wdx_exe_libvk}'\")
                if(_wdx_exe_libvk AND NOT _wdx_exe_libvk MATCHES \"^@rpath\")
                    execute_process(COMMAND install_name_tool -change
                        \"\${_wdx_exe_libvk}\" @rpath/libvulkan.1.dylib
                        \"\${CMAKE_INSTALL_PREFIX}/WhiteoutFlakes.app/Contents/MacOS/WhiteoutFlakes\")
                endif()

                # ---- Rewrite the MoltenVK ICD JSON's library_path ----
                # MUST happen before the wrapper codesign so the JSON is
                # part of the sealed bundle manifest.
                file(MAKE_DIRECTORY
                    \"\${CMAKE_INSTALL_PREFIX}/WhiteoutFlakes.app/Contents/Resources/vulkan/icd.d\")
                execute_process(COMMAND \"${Python3_EXECUTABLE}\"
                    \"${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/rewrite_icd.py\"
                    \"${_wdx_mvk_icd}\"
                    \"\${CMAKE_INSTALL_PREFIX}/WhiteoutFlakes.app/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json\"
                    RESULT_VARIABLE _icd_rc)
                if(NOT _icd_rc EQUAL 0)
                    message(FATAL_ERROR \"rewrite_icd.py failed (rc=\${_icd_rc})\")
                endif()

                # ---- Ad-hoc codesign ----
                # Nested binaries first (signature freezes), then the exe,
                # then the bundle wrapper. macOS 14+ rejects code-signed
                # dylibs whose load commands have been edited post-sign, so
                # the install_name_tool steps above must precede this.
                execute_process(COMMAND codesign --force --sign -
                    \"\${CMAKE_INSTALL_PREFIX}/WhiteoutFlakes.app/Contents/Frameworks/libvulkan.1.dylib\")
                execute_process(COMMAND codesign --force --sign -
                    \"\${CMAKE_INSTALL_PREFIX}/WhiteoutFlakes.app/Contents/Frameworks/libMoltenVK.dylib\")
                execute_process(COMMAND codesign --force --sign -
                    \"\${CMAKE_INSTALL_PREFIX}/WhiteoutFlakes.app/Contents/MacOS/WhiteoutFlakes\")
                execute_process(COMMAND codesign --force --sign -
                    \"\${CMAKE_INSTALL_PREFIX}/WhiteoutFlakes.app\")
            ")
            message(STATUS "macOS bundle: bundling MoltenVK from ${_wdx_mvk_lib}")
        else()
            message(STATUS
                "macOS bundle: MoltenVK not found in standard layouts; "
                "user will need a system Vulkan install at runtime "
                "(checked: brew molten-vk, VULKAN_SDK, brew vulkan-loader sibling)")
        endif()
    endif()
endif()
