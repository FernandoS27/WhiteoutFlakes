# cornflakes_core — static runtime library.
#
# Module subdirectories (asset, binding, vm, …) will plug in as sources get
# filled in during later checkpoints. CP0 only ships the version shim so
# the target links cleanly and proves the build pipeline end-to-end.

add_library(cornflakes_core STATIC
    version.cpp
    core/arena.cpp
    diagnostics/issue.cpp
    scheduler/serial_worker_pool.cpp
    service/cornflakes_service.cpp
    asset/asset_reader.cpp
    asset/pkb_reader.cpp
    asset/text_pkfx_reader.cpp
    asset/war3_compatibility_validator.cpp
    asset/object_accessors.cpp
    binding/effect_binder.cpp
    binding/program_binder.cpp
    binding/renderer_binder.cpp
    binding/sampler_binder.cpp
    binding/external_binding.cpp
    binding/ir_to_cbem_lowerer.cpp
    binding/sampler_resource.cpp
    binding/spatial_layer_resource.cpp
    vm/bytecode_trace.cpp
    vm/cbem/cbem_interpreter.cpp
    vm/cbem/cbem_ir_ops.cpp
    vm/cbem/cbem_cbem_ops.cpp
    vm/cbem/cbem_dispatchers.cpp
    vm/math_functions.cpp
    vm/bytecode_decoder.cpp
    sampler/shape.cpp
    sampler/curve.cpp
    sampler/texture.cpp
    sampler/turbulence.cpp
    sim/particle_page.cpp
    sim/spawn_processor.cpp
    sim/spawn_evaluator_driver.cpp
    sim/evolve_page_task.cpp
    sim/simulation_runtime.cpp
    sim/layer_tick_harness.cpp
    sim/particle_pool.cpp
    sim/effect_runtime.cpp
    events/payload_cache_store.cpp
    events/payload_builder.cpp
    events/event_generator.cpp
    events/event_router.cpp
    events/kick_processor.cpp
    render/semantic_slot_reader.cpp
    render/renderer_class_bucket.cpp
    render/render_extractor.cpp
    render/pool_extractor.cpp
    render/ribbon_geometry.cpp
    render/shader_perm.cpp
    render/instance_stream.cpp
    diagnostics/frame_trace.cpp
    validation/batch_resource_tracker.cpp
)
add_library(cornflakes::core ALIAS cornflakes_core)

# Generated PKB/PKFX handler field tables. Same scope as the target so
# CMake's add_custom_command -> target_sources hookup works correctly.
cornflakes_add_field_defs_target(cornflakes_core)

target_include_directories(cornflakes_core
    PUBLIC  "${CMAKE_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(cornflakes_core
    PUBLIC  cornflakes::determinism
    PRIVATE cornflakes::warnings
)

set_target_properties(cornflakes_core PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
