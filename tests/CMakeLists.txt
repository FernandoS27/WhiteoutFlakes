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

# ---- renderer ----
wdx_add_test(coordinate_system_test coordinate_system_test.cpp)
wdx_add_test(draw_trace_test        draw_trace_test.cpp)
wdx_add_test(render_profile_test    render_profile_test.cpp)
wdx_add_test(wc3_classify_test      wc3_classify_test.cpp)
wdx_add_test(particle_curve_test    particle_curve_test.cpp)
wdx_add_test(particle_shape_test    particle_shape_test.cpp)
wdx_add_test(particle_sim_test      particle_sim_test.cpp)
wdx_add_test(particle_trace_test    particle_trace_test.cpp)

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
