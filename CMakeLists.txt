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

    set(WC3_SLANG_OUT  "${WC3_SHADERS_DIR}/slang_out")
    set(WC3_BLS_OUT    "${WC3_SHADERS_DIR}/bls_out")

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

    set(WC3_DXBC_STAMP   "${CMAKE_CURRENT_BINARY_DIR}/wc3_shaders_dxbc.stamp")
    set(WC3_DXIL_STAMP   "${CMAKE_CURRENT_BINARY_DIR}/wc3_shaders_dxil.stamp")
    set(WC3_SPIRV_STAMP  "${CMAKE_CURRENT_BINARY_DIR}/wc3_shaders_spirv.stamp")
    set(WC3_BLS_STAMP    "${CMAKE_CURRENT_BINARY_DIR}/wc3_shaders_bls.stamp")

    add_custom_command(
        OUTPUT  "${WC3_DXBC_STAMP}"
        COMMAND "${Python3_EXECUTABLE}" "${WC3_SHADERS_DIR}/compile_all_slang.py"
                --target d3d11 --slangc "${SLANGC}" ${WC3_FAMILY_ARGS}
        COMMAND "${CMAKE_COMMAND}" -E touch "${WC3_DXBC_STAMP}"
        WORKING_DIRECTORY "${WC3_SHADERS_DIR}"
        DEPENDS ${WC3_SLANG_SOURCES} ${WC3_BUILD_SCRIPTS}
        COMMENT "Wc3Shaders: compiling Slang -> DXBC (d3d11)"
        VERBATIM)

    add_custom_command(
        OUTPUT  "${WC3_DXIL_STAMP}"
        COMMAND "${Python3_EXECUTABLE}" "${WC3_SHADERS_DIR}/compile_all_slang.py"
                --target d3d12 --slangc "${SLANGC}" ${WC3_FAMILY_ARGS}
        COMMAND "${CMAKE_COMMAND}" -E touch "${WC3_DXIL_STAMP}"
        WORKING_DIRECTORY "${WC3_SHADERS_DIR}"
        DEPENDS ${WC3_SLANG_SOURCES} ${WC3_BUILD_SCRIPTS}
        COMMENT "Wc3Shaders: compiling Slang -> DXIL (d3d12)"
        VERBATIM)

    add_custom_command(
        OUTPUT  "${WC3_SPIRV_STAMP}"
        COMMAND "${Python3_EXECUTABLE}" "${WC3_SHADERS_DIR}/compile_all_slang.py"
                --target vulkan --slangc "${SLANGC_VULKAN}" ${WC3_FAMILY_ARGS}
        COMMAND "${CMAKE_COMMAND}" -E touch "${WC3_SPIRV_STAMP}"
        WORKING_DIRECTORY "${WC3_SHADERS_DIR}"
        DEPENDS ${WC3_SLANG_SOURCES} ${WC3_BUILD_SCRIPTS}
        COMMENT "Wc3Shaders: compiling Slang -> SPIR-V (vulkan)"
        VERBATIM)

    # Upstream build_bls.py writes two trees:
    #   <output>_1_8/{ps,vs,mtlfs,mtlvs}/*.bls                 (template-faithful Wc3 v1.8)
    #   <output>_1_14/shaders/{pixel,vertex}/<api>/*.bls       (v1.14, WoW-style layout)
    # where <api> ∈ {dx_5_0, dx_6_0, mtl_1_1, spv_1_3, …}. The SPIR-V
    # subdir name encodes the slangc-emitted SPIR-V version (1.3 for the
    # `glsl_450` profile targeting Vulkan 1.3). The `--build_extra` flag
    # was retired upstream — every backend with slangc output is packed
    # unconditionally.
    set(WC3_BLS_OUT_18 "${WC3_BLS_OUT}_1_8")
    set(WC3_BLS_OUT_14 "${WC3_BLS_OUT}_1_14")
    add_custom_command(
        OUTPUT  "${WC3_BLS_STAMP}"
        COMMAND "${Python3_EXECUTABLE}" "${WC3_SHADERS_DIR}/build_bls.py"
                --templates "${WDX_SHADER_TEMPLATES}"
                --output    "${WC3_BLS_OUT}"
                --strip
        COMMAND "${CMAKE_COMMAND}" -E touch "${WC3_BLS_STAMP}"
        WORKING_DIRECTORY "${WC3_SHADERS_DIR}"
        DEPENDS "${WC3_DXBC_STAMP}" "${WC3_DXIL_STAMP}" "${WC3_SPIRV_STAMP}"
                ${WC3_BUILD_SCRIPTS}
        COMMENT "Wc3Shaders: packing .bls (v1.8 DX + v1.14 DX/SPIR-V)"
        VERBATIM)

    # Stage the freshly packed bundles into the standalone exe's output
    # directory the moment Wc3Shaders rebuilds — independently of whether
    # the exe itself relinks. POST_BUILD on the exe target wouldn't fire
    # on a BLS-only change. The standalone exe lands at
    # ${CMAKE_BINARY_DIR}/standalone (set via RUNTIME_OUTPUT_DIRECTORY
    # below), so we know the path up-front. We flatten the upstream
    # layout into the renderer's existing path scheme
    # (shaders/{ps,vs,d3d12/ps,d3d12/vs,vulkan/ps,vulkan/vs}/) so the
    # BLS shader cache doesn't need to learn the new tree.
    set(WC3_STAGE_DIR   "${CMAKE_BINARY_DIR}/standalone/shaders")
    set(WC3_STAGE_STAMP "${CMAKE_CURRENT_BINARY_DIR}/wc3_shaders_stage.stamp")
    add_custom_command(
        OUTPUT  "${WC3_STAGE_STAMP}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${WC3_BLS_OUT_18}/ps"                          "${WC3_STAGE_DIR}/ps"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${WC3_BLS_OUT_18}/vs"                          "${WC3_STAGE_DIR}/vs"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${WC3_BLS_OUT_14}/shaders/pixel/dx_6_0"        "${WC3_STAGE_DIR}/d3d12/ps"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${WC3_BLS_OUT_14}/shaders/vertex/dx_6_0"       "${WC3_STAGE_DIR}/d3d12/vs"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${WC3_BLS_OUT_14}/shaders/pixel/spv_1_3"       "${WC3_STAGE_DIR}/vulkan/ps"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${WC3_BLS_OUT_14}/shaders/vertex/spv_1_3"      "${WC3_STAGE_DIR}/vulkan/vs"
        COMMAND "${CMAKE_COMMAND}" -E touch "${WC3_STAGE_STAMP}"
        DEPENDS "${WC3_BLS_STAMP}"
        COMMENT "Wc3Shaders: staging BLS bundles into ${WC3_STAGE_DIR}"
        VERBATIM)

    add_custom_target(Wc3Shaders ALL DEPENDS "${WC3_STAGE_STAMP}")
endif()

# ---- Static library: GFX Service (backend-agnostic abstraction) ----
# The Vulkan backend compiles in whenever the Vulkan SDK is installed
# (find_package(Vulkan) succeeds). It targets Vulkan 1.3 + dynamic
# rendering and pulls VMA from the SDK's bundled vk_mem_alloc.h.
find_package(Vulkan QUIET)

set(WHITEOUTFLAKES_GFX_SOURCES
    src/gfx/gfx_factory.cpp
    src/gfx/d3d11/d3d11_device.cpp
    src/gfx/d3d11/d3d11_command_list.cpp
    src/gfx/d3d12/d3d12_device.cpp
    src/gfx/d3d12/d3d12_command_list.cpp
    src/gfx/d3d12/d3d12_resources.cpp
)
if(Vulkan_FOUND)
    list(APPEND WHITEOUTFLAKES_GFX_SOURCES
        src/gfx/vulkan/vulkan_device.cpp
        src/gfx/vulkan/vulkan_command_list.cpp
        src/gfx/vulkan/vulkan_swap_chain.cpp
        src/gfx/vulkan/vulkan_pipeline.cpp
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

target_link_libraries(WhiteoutFlakesGfx
    PUBLIC  whiteout_lib
    PRIVATE d3d11.lib d3d12.lib dxgi.lib
)

if(Vulkan_FOUND)
    message(STATUS "WhiteoutFlakesGfx: Vulkan = ${Vulkan_LIBRARIES} "
                   "(SDK=${Vulkan_INCLUDE_DIRS})")
    target_link_libraries(WhiteoutFlakesGfx PRIVATE Vulkan::Vulkan)
    target_compile_definitions(WhiteoutFlakesGfx PRIVATE
        WDX_HAS_VULKAN=1
        VK_USE_PLATFORM_WIN32_KHR=1
        NOMINMAX
        WIN32_LEAN_AND_MEAN
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
    src/io/replaceable_paths.cpp
    src/io/event_data.cpp
    src/io/slk.cpp
    src/io/texture_image_usage.cpp
    src/public_api/renderer_api.cpp
)

add_dependencies(WhiteoutFlakesLib WhiteoutFlakesShaders)

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
add_executable(WhiteoutFlakesStandalone
    tools/basic_viewer/test_main.cpp
    tools/basic_viewer/render_window.cpp
    tools/basic_viewer/settings_ini.cpp
    tools/basic_viewer/windows_sound_emitter.cpp
    tools/basic_viewer/resource.rc
)

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
    "${CMAKE_CURRENT_SOURCE_DIR}/externals/dr_libs"
)

target_link_libraries(WhiteoutFlakesStandalone PRIVATE
    WhiteoutFlakesLib
    comdlg32
)

if(MSVC)
    target_compile_options(WhiteoutFlakesStandalone PRIVATE
        /W3 /EHsc
        /wd4100 /wd4127 /wd4244 /wd4267
    )
endif()

if(WDX_BUILD_WC3_SHADERS)
    add_dependencies(WhiteoutFlakesStandalone Wc3Shaders)
endif()

# ---- Optional: 3ds Max plugin (.dlx) ----
option(WDX_BUILD_MAX_PLUGIN
    "Build the WhiteoutFlakes 3ds Max plugin (requires Max SDK + -DMAX_VERSION)"
    OFF)

if(WDX_BUILD_MAX_PLUGIN)
    add_subdirectory(tools/max_plugin)
endif()