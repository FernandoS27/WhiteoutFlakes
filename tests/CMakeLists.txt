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
wdx_add_test(content_ref_test   content_ref_test.cpp)
# Per-product storage routing. Policy assertions run everywhere; the parts
# that need a real WoW / SC2 / Heroes install skip without one.
wdx_add_test(content_provider_games_test content_provider_games_test.cpp)
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
             "${CMAKE_SOURCE_DIR}/tools/basic_viewer/settings_io.cpp")
target_include_directories(settings_ini_test PRIVATE "${CMAKE_SOURCE_DIR}/tools/basic_viewer")
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
    wdx_add_test(m2_lighting_test m2_lighting_test.cpp)
    wdx_add_test(wow_character_test wow_character_test.cpp)
    # Corpus only: needs the `dbfilesclient/` dump beside the models.
    wdx_add_test(wow_creature_skin_test wow_creature_skin_test.cpp)
endif()
if(WDX_ENABLE_M3)
    wdx_add_test(m3_geometry_test m3_geometry_test.cpp)
endif()
# The baked-vertex-buffer description check. Compiled unconditionally — it
# #ifdefs its two halves internally, so a build with neither format still
# links and reports honestly rather than vanishing from the suite.
if(WDX_ENABLE_M2 OR WDX_ENABLE_M3)
    wdx_add_test(mesh_buffer_test mesh_buffer_test.cpp)
endif()

# ---- renderer ----
wdx_add_test(coordinate_system_test coordinate_system_test.cpp)
wdx_add_test(pose_request_test       pose_request_test.cpp)
wdx_add_test(draw_trace_test        draw_trace_test.cpp)
wdx_add_test(render_profile_test    render_profile_test.cpp)
wdx_add_test(wc3_classify_test      wc3_classify_test.cpp)
wdx_add_test(particle_curve_test    particle_curve_test.cpp)
wdx_add_test(particle_shape_test    particle_shape_test.cpp)
wdx_add_test(particle_sim_test      particle_sim_test.cpp)
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
