# ============================================================================
# wf_casc_server — HTTP server (Crow) that imitates Hiveworkshop's CASC
# delivery for the WhiteoutFlakes web viewer.
#
# Pulls Crow in via FetchContent so the dep is scoped to this target only —
# WhiteoutFlakesLib, the desktop viewer, and the WASM build never see it.
# Activated by the root CMakeLists.txt's WDX_BUILD_CASC_SERVER option.
# ============================================================================

include(FetchContent)

# Asio (standalone, header-only). Crow's bundled Findasio.cmake searches
# for the headers via the `ASIO_INCLUDE_DIR` cache variable — we fetch the
# Asio source, skip its own add_subdirectory (it's all examples/tests we
# don't need), and just point Crow at the include dir.
#
# The `SOURCE_SUBDIR no-such-dir` trick makes FetchContent_MakeAvailable
# do the download without invoking Asio's CMakeLists.txt — cleaner than the
# now-deprecated FetchContent_Populate path.
FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-38-0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  no-such-dir
)
FetchContent_MakeAvailable(asio)
# The Asio repo has `asio/include` and `asio/src` as POSIX symlinks
# (pointing at the top-level `include/` and `src/`). Those don't resolve
# on Windows git checkouts unless core.symlinks is enabled and the user
# has Developer Mode on. Point at the top-level `include/` directly —
# canonical on Linux/macOS too.
set(ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/include"
    CACHE PATH "Standalone Asio header location (for Crow)" FORCE)

# Crow ships its own CMake config. Disable its bundled examples/tests so it
# only builds the Crow target itself.
set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
# Crow's CMake exposes a few "install" rules we don't want polluting the
# WhiteoutFlakes install tree.
set(CROW_INSTALL        OFF CACHE BOOL "" FORCE)

FetchContent_Declare(crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG        v1.3.2
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(crow)

add_executable(wf_casc_server main.cpp)

set_target_properties(wf_casc_server PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/casc_server"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/casc_server"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/casc_server"
)

# Crow's target is `Crow::Crow` (provided by its installed CMake config).
# whiteout_casc is exported by externals/WhiteoutLib when WHITEOUT_ENABLE_CASC=ON,
# which the root CMakeLists guarantees for desktop builds.
target_link_libraries(wf_casc_server PRIVATE
    whiteout_lib   # for utils::findBlizzardGames (auto-discover WC3 install)
    whiteout_casc
    Crow::Crow
)

# Crow's INTERFACE include path doesn't propagate Asio's location to
# downstream consumers — Findasio.cmake just sets ASIO_INCLUDE_DIR as a
# cache var for Crow's own build. Add it explicitly here so <crow.h>'s
# `#include <asio.hpp>` resolves.
target_include_directories(wf_casc_server PRIVATE "${ASIO_INCLUDE_DIR}")
target_compile_definitions(wf_casc_server PRIVATE ASIO_STANDALONE)
if(WIN32)
    # Asio probes _WIN32_WINNT to pick the right sockets API surface and
    # falls back to Windows-7 with a warning if it's unset. Target Win10/11.
    target_compile_definitions(wf_casc_server PRIVATE _WIN32_WINNT=0x0A00)
endif()
