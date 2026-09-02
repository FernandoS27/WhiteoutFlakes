# ============================================================================
# Unit tests — Catch2 v3.
#
# Scope: the pure, headless parts of the engine — parsers, curves, spawn
# shapes, classification rules, path/format policy. No gfx device, no window,
# no game archives, so a test run is a plain executable and works on a CI
# box with no GPU.
#
# Every test links the real WhiteoutFlakesLib rather than recompiling the
# sources it exercises, so a behaviour change shows up here instead of in a
# screenshot diff.
#
# Catch2 is fetched at configure time (same version WhiteoutLib's own suite
# uses, so the two FetchContent declarations agree when both are enabled).
# ============================================================================

include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.7.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

# One executable per test file, auto-registered with CTest so `ctest` runs
# each TEST_CASE as its own test (and `-R` filters work on case names).
function(wdx_add_test TARGET)
    add_executable(${TARGET} ${ARGN})
    set_target_properties(${TARGET} PROPERTIES
        CXX_STANDARD          20
        CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/tests"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/tests"
    )
    target_link_libraries(${TARGET} PRIVATE WhiteoutFlakesLib Catch2::Catch2WithMain)
    if(MSVC)
        target_compile_options(${TARGET} PRIVATE
            /W3 /EHsc
            /wd4100 /wd4127 /wd4244 /wd4267
        )
    endif()
    catch_discover_tests(${TARGET} WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    set_property(GLOBAL APPEND PROPERTY WDX_TEST_TARGETS ${TARGET})
endfunction()

# ---- io ----
wdx_add_test(slk_test           slk_test.cpp)
wdx_add_test(path_utils_test    path_utils_test.cpp)
wdx_add_test(texture_usage_test texture_usage_test.cpp)
# The sphere-map -> cubemap projection StarCraft II's Spherical envio
# mappings are decoded through. Device-free, so it is unconditional.
wdx_add_test(env_cube_test     env_cube_test.cpp)
wdx_add_test(content_ref_test   content_ref_test.cpp)
# Event-data SLK tables, keyed per render mode so a flip back to a mode the
# session already parsed re-reads nothing.
wdx_add_test(event_data_mode_test event_data_mode_test.cpp)
# Per-product storage routing. Policy assertions run everywhere; the parts
# that need a real WoW / SC2 / Heroes install skip without one.
wdx_add_test(content_provider_games_test content_provider_games_test.cpp)
wdx_add_test(progress_test progress_test.cpp)
# The layer under it: path spelling, each game's archive rules, and what the
# builder assembles. Opens no storage, so it needs no install at all.
wdx_add_test(game_storage_test game_storage_test.cpp)
wdx_add_test(storage_browser_test storage_browser_test.cpp)
# The viewer's settings file, which now stores one IO section per game.
# Compiles the viewer's settings_io.cpp directly — it belongs to the viewer
# executable, not to WhiteoutFlakesLib. Its display-settings sibling
# (settings_ini.cpp) is deliberately NOT here: that one links RenderService,
# which would drag a gfx backend into a device-free test.
wdx_add_test(settings_ini_test settings_ini_test.cpp
             "${CMAKE_SOURCE_DIR}/tools/basic_viewer/settings_io.cpp"
             "${CMAKE_SOURCE_DIR}/tools/basic_viewer/storage_explorer_ini.cpp")
target_include_directories(settings_ini_test PRIVATE
                           "${CMAKE_SOURCE_DIR}/tools/basic_viewer"
                           "${CMAKE_SOURCE_DIR}/tools/model_explorer")
# The animation-export recipe: the schedule arithmetic (clip boundaries,
# fill-to-duration, drift compensation, loop closure), the name templates and
# the ini round trip. Same precedent as settings_ini_test above — both viewer
# TUs link nothing, so this is a G0 test with no device and no corpus.
# settings_io.cpp comes along for SettingsIniPath(), which export_ini.cpp uses.
wdx_add_test(export_recipe_test export_recipe_test.cpp
             "${CMAKE_SOURCE_DIR}/tools/basic_viewer/export_recipe.cpp"
             "${CMAKE_SOURCE_DIR}/tools/basic_viewer/export_ini.cpp"
             "${CMAKE_SOURCE_DIR}/tools/basic_viewer/settings_io.cpp")
target_include_directories(export_recipe_test PRIVATE
                           "${CMAKE_SOURCE_DIR}/tools/basic_viewer")
# Only when the format is compiled in — the adapter it exercises does not
# exist otherwise. The test itself skips without a configured WoW install.
if(WDX_ENABLE_M2)
    wdx_add_test(m2_geometry_test m2_geometry_test.cpp)
    # Half pure tables, half corpus sweep — see the file header.
    wdx_add_test(m2_animation_test m2_animation_test.cpp)
    # Corpus only: lazy `.anim` loading has to match the eager parse exactly.
    wdx_add_test(m2_lazy_animation_test m2_lazy_animation_test.cpp)
    # Pure tables — no device, no corpus. These are the ones that catch a
    # transcription slip in the shader-selection or material-state rules.
    wdx_add_test(m2_shader_select_test m2_shader_select_test.cpp)
    wdx_add_test(m2_material_test m2_material_test.cpp)
    # Bone billboards. Pure: a hand-built model through the real adapter,
    # with every assertion written on the view-space basis the client pins
    # down rather than on our model-space palette.
    wdx_add_test(m2_billboard_test m2_billboard_test.cpp)
    wdx_add_test(m2_lighting_test m2_lighting_test.cpp)
    wdx_add_test(wow_character_test wow_character_test.cpp)
    # `.m2` particle emitters. Half pure translation, half a corpus sweep — the
    # sweep is what stops a green render-diff M2 arm from being read as proof
    # that particles work when the corpus might carry none.
    wdx_add_test(m2_particle_test m2_particle_test.cpp)
    # Corpus only: needs the `dbfilesclient/` dump beside the models.
    wdx_add_test(wow_creature_skin_test wow_creature_skin_test.cpp)
    wdx_add_test(wow_item_appearance_test wow_item_appearance_test.cpp)
    # ParticleColor x particleColorIndex: the skin recolours emitters.
    wdx_add_test(wow_particle_color_test wow_particle_color_test.cpp)
    # `.phys` -> Snowball, end to end. Only exists when physics is compiled in;
    # without it there is no stage to test and the adapter returns nothing.
    if(WDX_ENABLE_PHYSICS)
        wdx_add_test(wow_physics_test wow_physics_test.cpp)
    endif()
endif()
if(WDX_ENABLE_M3)
    wdx_add_test(m3_geometry_test m3_geometry_test.cpp)
    # Half synthetic fixtures (m3_anim_builders.h), half corpus sweep — the
    # sweep is what checks shipped files agree with the animRef encoding the
    # reader indexes typed arrays with.
    wdx_add_test(m3_animation_test m3_animation_test.cpp)
    # Layered blending, asserted as poses through the real Evaluate path.
    wdx_add_test(m3_blend_test m3_blend_test.cpp)
    # Skeleton + palettes. Mostly guards the *absence* of a per-vertex decode.
    wdx_add_test(m3_skinning_test m3_skinning_test.cpp)
    # Non-bone channels: hierarchical bone visibility, geoset gating, lights.
    wdx_add_test(m3_billboard_test m3_billboard_test.cpp)
    wdx_add_test(m3_channels_test m3_channels_test.cpp)
    # Discrete keyframe events: the shared crossing arithmetic (previously
    # untested and private to EventEmitterPool) plus the `.m3` SDEV decode.
    wdx_add_test(m3_events_test m3_events_test.cpp)
    # Post-skeleton pose stages: terrain IK and the turret. Device-free —
    # a stage reads a FrameState and plain values, so the real solver runs
    # with no model, no adapter and no GPU.
    wdx_add_test(m3_solver_test m3_solver_test.cpp)
    # PHRB/PHYJ -> Snowball. Only exists when physics is compiled in; without
    # it the adapter appends no physics stage and there is nothing to test.
    if(WDX_ENABLE_PHYSICS)
        wdx_add_test(sc2_physics_test sc2_physics_test.cpp)
        # PHCL -> Snowball's cloth solver, and back into the skinning palette.
        # Unlike the rigid-body test this one reads the built records, so it
        # needs Snowball's headers — `Sc2ClothPiece` carries a `ClothDef` by
        # value rather than hiding one, because the records *are* the result.
        wdx_add_test(sc2_cloth_test sc2_cloth_test.cpp)
        target_link_libraries(sc2_cloth_test PRIVATE snowball)
    endif()
    # The simple material system's table: batch→MATM→material resolution,
    # the canonical texture set, layer fallbacks, classification. Synthetic
    # models, no device.
    wdx_add_test(m3_surface_table_test m3_surface_table_test.cpp)
endif()
if(WDX_ENABLE_D3)
    # Gates D3-G1 (geometry in range) and D3-G2 (the two bind poses, and the
    # divergent-bone COUNT that is the whole of why tTransform4 is the palette).
    # Offline: no CASC, no device.
    wdx_add_test(d3_geometry_test d3_geometry_test.cpp)
    # Rigid-first, global influence indices, and the palette read back off the
    # adapter on a model whose two bind poses actually differ.
    wdx_add_test(d3_skinning_test d3_skinning_test.cpp)
    # Looks, the material-name join, the EMaterialTextureType census the slot
    # mapping is corrected against, and the per-look sub-object visibility bit.
    wdx_add_test(d3_surface_table_test d3_surface_table_test.cpp)
    # Gate D3-G4: the signed quaternion decode, WITH the unsigned one failing
    # the same test — otherwise the gate is a formality.
    wdx_add_test(d3_animation_test d3_animation_test.cpp)
    # Gate D3-G5: read/parse counters over a stub provider. The one gate that
    # can prove "nothing is read twice" rather than measure that it got faster.
    wdx_add_test(d3_cache_test d3_cache_test.cpp)
    # `.tex` through the shared dispatcher, on the installed client.
    wdx_add_test(d3_texture_test d3_texture_test.cpp)
    # Gate D3-G6: a player Appearance ships its whole wardrobe and the file
    # never says which of it draws. Counts what dressing hides, and checks that
    # a model with no wardrobe is left completely alone.
    wdx_add_test(d3_character_test d3_character_test.cpp)
    # Gate D3-G7: equip by in-game name. The GameBalance registry over the
    # corpus snapshot, the item -> look resolver against measured tags, and
    # draw-set parity between item dressing and manual dressing.
    wdx_add_test(d3_item_equip_test d3_item_equip_test.cpp)
    # The rig census the CollisionShape renames are corrected against, plus the
    # physics gates. Registered outside the WDX_ENABLE_PHYSICS guard on purpose:
    # the census reads shipped records and needs no solver, so a build without
    # Snowball still gets to disagree with the decompile.
    wdx_add_test(d3_physics_test d3_physics_test.cpp)
    # Gates D3-P0..P4 of D3_PARTICLE_DESIGN.md: the path evaluator, the
    # nine random distributions, the emitter-shape samplers, the emission
    # accumulator, the five motion models and the orientation pair. The
    # corpus sweeps skip without an extracted D3 corpus; the rest are
    # synthetic and run everywhere.
    wdx_add_test(d3_particle_test d3_particle_test.cpp)
    # The same module measured against the ORIGINAL rather than against its own
    # description: `tools/d3_particle_oracle/` runs the real 2.6.2 Switch binary
    # under Unicorn and records what it computed; this replays those goldens.
    # No emulator and no binary needed to run it — that is the point.
    wdx_add_test(d3_particle_oracle_test d3_particle_oracle_test.cpp)
    target_compile_definitions(d3_particle_oracle_test PRIVATE
        WDX_D3_ORACLE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tools/d3_particle_oracle/golden")
    # How a `.prt` reaches an actor: the EffectGroup parser, the census that
    # ranks the three routes, and the resolver's item selection.
    wdx_add_test(d3_effect_test d3_effect_test.cpp)
    # The playback half of the same route: frames to milliseconds, and the pool
    # that fires them. Corpus-backed on purpose — the shortest path to a
    # ClipAttachments call runs through four assets, and faking them would be
    # faking the thing under test.
    wdx_add_test(d3_attachment_test d3_attachment_test.cpp)
    # The rig through Snowball. Only exists when physics is compiled in; without
    # it the adapter appends no stage and there is nothing to step.
    if(WDX_ENABLE_PHYSICS)
        wdx_add_test(d3_physics_sim_test d3_physics_sim_test.cpp)
        # Snowball's headers, because the joint mapping is checked directly
        # rather than through a settled pose: a frame pre-rotated by the wrong
        # quaternion still hangs, still collides and still settles.
        target_link_libraries(d3_physics_sim_test PRIVATE snowball)
        # Cloth. Under the same option and for the same reason as the profile
        # source -- it links no Snowball at all, but it is the simulation the
        # option turns off.
        wdx_add_test(d3_cloth_test d3_cloth_test.cpp)
    endif()
endif()

# The baked-vertex-buffer description check. Compiled unconditionally — it
# #ifdefs its two halves internally, so a build with neither format still
# links and reports honestly rather than vanishing from the suite.
if(WDX_ENABLE_M2 OR WDX_ENABLE_M3)
    wdx_add_test(mesh_buffer_test mesh_buffer_test.cpp)
    # Which way the normals point. mesh_buffer_test pins that they are unit
    # length; this pins the half that a length check cannot see, and measures
    # both formats together so the known-good one calibrates the other.
    wdx_add_test(normal_orientation_test normal_orientation_test.cpp)
endif()

# ---- renderer ----
wdx_add_test(coordinate_system_test coordinate_system_test.cpp)
wdx_add_test(pose_request_test       pose_request_test.cpp)
# The shared playback layer. Its parity case carries a transcription of the
# pre-refactor Actor::Advance — that copy is the point, see the file header.
wdx_add_test(clip_playlist_test     clip_playlist_test.cpp)
# Kernels shared between format adapters. Values pinned by hand, not captured
# from the copies the extraction replaced.
wdx_add_test(anim_math_test         anim_math_test.cpp)
wdx_add_test(draw_trace_test        draw_trace_test.cpp)
wdx_add_test(render_profile_test    render_profile_test.cpp)
wdx_add_test(wc3_classify_test      wc3_classify_test.cpp)
wdx_add_test(particle_curve_test    particle_curve_test.cpp)
# The WC3/WoW dialect POD. Pins Wc3()'s defaults, which are the MDX contract
# every "this did not move WC3" claim rests on.
wdx_add_test(particle_dialect_test  particle_dialect_test.cpp)
wdx_add_test(particle_shape_test    particle_shape_test.cpp)
wdx_add_test(particle_sim_test      particle_sim_test.cpp)
# Trail emitters (M2 RPID) and the random-spacing defect they rest on. No
# reference image of this feature exists, so the arithmetic is the contract.
wdx_add_test(particle_trail_test    particle_trail_test.cpp)
# Refraction emitters (M2 Refraction): the two scrolling texture layers and the
# routing that keeps a distortion mask out of the scene's colour.
wdx_add_test(particle_refraction_test particle_refraction_test.cpp)
wdx_add_test(particle_trace_test    particle_trace_test.cpp)
wdx_add_test(ribbon_sim_test        ribbon_sim_test.cpp)

# `cmake --build build --target wdx_tests` builds every test binary;
# `--target check` builds them and runs the suite through CTest.
get_property(_wdx_test_targets GLOBAL PROPERTY WDX_TEST_TARGETS)
add_custom_target(wdx_tests DEPENDS ${_wdx_test_targets})

add_custom_target(check
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure --build-config $<CONFIG>
    DEPENDS wdx_tests
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Running the WhiteoutFlakes unit tests"
    USES_TERMINAL
)
