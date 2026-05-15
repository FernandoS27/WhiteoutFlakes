# ============================================================================
# WhiteoutFlakes — standalone renderer + basic_viewer tool.
# ============================================================================

cmake_minimum_required(VERSION 3.20)
project(WhiteoutFlakes LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Pull in WhiteoutLib (provides whiteout_lib; optionally whiteout_casc, whiteout_mpq).
set(WHITEOUT_ENABLE_CASC ON CACHE BOOL "Enable CASC archive support" FORCE)
set(WHITEOUT_ENABLE_MPQ  ON CACHE BOOL "Enable MPQ archive support"  FORCE)
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
# Only line.slang (debug wires) and viewcube.slang (debug orientation
# cube) remain as Slang sources; every MDX-material draw goes through
# BLS, and the tonemap pass uses the engine's sprite.bls VS directly.
set(SHADER_ENTRIES
    "line.slang|vsLine|vertex|kLineVS"
    "line.slang|psLine|fragment|kLinePS"
    "viewcube.slang|vsViewCube|vertex|kViewCubeVS"
    "viewcube.slang|psViewCube|fragment|kViewCubePS"
)

set(SHADER_DXBC_FILES "")
set(SHADER_SPV_FILES  "")

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
endforeach()

# Embed all DXBC blobs into a single C++ header
set(COMPILED_SHADERS_H "${SHADER_OUT_DIR}/compiled_shaders.h")

add_custom_command(
    OUTPUT  "${COMPILED_SHADERS_H}"
    COMMAND "${CMAKE_COMMAND}"
            "-DSHADER_DIR=${SHADER_OUT_DIR}"
            "-DOUTPUT=${COMPILED_SHADERS_H}"
            -P "${SHADER_SRC_DIR}/embed_shaders.cmake"
    DEPENDS ${SHADER_DXBC_FILES} ${SHADER_SPV_FILES}
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

add_custom_target(WhiteoutFlakesEmbedded DEPENDS "${TEAM_GLOW_H}")

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
# debug is on. Roughly doubles the Wc3Shaders compile time — turn off
# while iterating if that hurts.
option(WDX_BUILD_WC3_DEBUG_SHADERS
    "Also build debug-symbol BLS bundles into debug_shaders/"
    ON)

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

    # Optional: stage debug shaders the same way if they exist.
    if(WDX_BUILD_WC3_DEBUG_SHADERS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/debug_shaders")
        set(_prebuilt_debug_stage "${CMAKE_BINARY_DIR}/standalone/debug_shaders")
        set(_prebuilt_debug_stamp "${CMAKE_CURRENT_BINARY_DIR}/wc3_prebuilt_debug.stamp")
        add_custom_command(
            OUTPUT  "${_prebuilt_debug_stamp}"
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_prebuilt_debug_stage}"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/debug_shaders"
                    "${_prebuilt_debug_stage}"
            COMMAND "${CMAKE_COMMAND}" -E touch "${_prebuilt_debug_stamp}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt/debug_shaders"
            COMMENT "Wc3Shaders[prebuilt-debug]: staging debug_shaders/"
            VERBATIM)
        add_custom_target(Wc3ShadersDebug ALL DEPENDS "${_prebuilt_debug_stamp}")
        add_dependencies(Wc3Shaders Wc3ShadersDebug)
    endif()
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
            DEPENDS "${_dxbc}" "${_dxil}" "${_spirv}" ${WC3_BUILD_SCRIPTS}
            COMMENT "Wc3Shaders[${variant}]: packing .bls (v1.8 DX + v1.14 DX/SPIR-V)"
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
    # prebuilt/shaders pack (and prebuilt/debug_shaders if WDX_BUILD_WC3_DEBUG_SHADERS
    # is on) from the freshly-built BLS bundles. Run after touching .slang
    # sources, then `git add prebuilt/` and commit so CI / fresh clones can
    # use WDX_USE_PREBUILT_SHADERS=ON without invoking slangc themselves.
    set(_prebuilt_dir "${CMAKE_CURRENT_SOURCE_DIR}/prebuilt")
    add_custom_target(update-prebuilt-shaders
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_prebuilt_dir}/shaders"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${CMAKE_BINARY_DIR}/standalone/shaders" "${_prebuilt_dir}/shaders"
        COMMENT "Updating ${_prebuilt_dir}/shaders from latest Wc3Shaders build"
        DEPENDS Wc3Shaders
        VERBATIM)
    if(WDX_BUILD_WC3_DEBUG_SHADERS)
        add_custom_command(TARGET update-prebuilt-shaders POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_prebuilt_dir}/debug_shaders"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "${CMAKE_BINARY_DIR}/standalone/debug_shaders"
                    "${_prebuilt_dir}/debug_shaders"
            COMMENT "Updating ${_prebuilt_dir}/debug_shaders from latest debug build"
            VERBATIM)
    endif()
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
    src/renderer/render_service.cpp
    src/renderer/render_detail.cpp
    src/renderer/render_target.cpp
    src/renderer/scene_manager.cpp
    src/renderer/coordinate_system.cpp
    src/renderer/assets/sampler_asset_manager.cpp
    src/renderer/assets/texture_asset_manager.cpp
    src/renderer/assets/replaceable_texture_manager.cpp
    src/renderer/assets/viewcube_atlas.cpp
    src/renderer/debug/debug_renderer.cpp
    src/renderer/effects/event_emitter_pool.cpp
    src/renderer/effects/pe1_system.cpp
    src/renderer/effects/ribbon.cpp
    src/renderer/effects/spn_spawner.cpp
    src/renderer/corn_effects/corn_effects_asset_cache.cpp
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
    user32.lib
    gdi32.lib
    comdlg32.lib
)

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
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/renderer/bls/bls_reflect_tool.cpp")
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
# UI surface at all.
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

if(MSVC)
    target_compile_options(WhiteoutFlakesStandalone PRIVATE
        /W3 /EHsc
        /wd4100 /wd4127 /wd4244 /wd4267
    )
endif()

if(WDX_BUILD_WC3_SHADERS OR WDX_USE_PREBUILT_SHADERS)
    add_dependencies(WhiteoutFlakesStandalone Wc3Shaders)
endif()

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

# ============================================================================
# Linux AppImage staging
#
# `cmake --install build --prefix AppDir/usr` populates the AppDir layout
# linuxdeploy expects. The AppImage runs the binary from AppDir/usr/bin and
# the engine's exe-dir search root picks up the staged BLS files next to it
# (no special XDG lookup needed). The .desktop file + icon get installed to
# AppDir/usr/share/{applications,icons/hicolor/256x256/apps}.
# ============================================================================
if(UNIX AND NOT APPLE)
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

    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/resources/WhiteoutFlakes.png"
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps)

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
