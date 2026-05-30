# wf_casc_server — Crow-based local replacement for Hive's CASC.
# Crow is fetched at this scope only. Gated by WDX_BUILD_CASC_SERVER.

include(FetchContent)

# Asio (standalone, header-only). SOURCE_SUBDIR=no-such-dir skips its
# CMakeLists (examples/tests we don't need); Crow finds it via ASIO_INCLUDE_DIR.
FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-38-0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  no-such-dir
)
FetchContent_MakeAvailable(asio)
# Skip the asio/include POSIX-symlink that doesn't resolve on Windows
# without Developer Mode; the top-level include/ works everywhere.
set(ASIO_INCLUDE_DIR "${asio_SOURCE_DIR}/include"
    CACHE PATH "Standalone Asio header location (for Crow)" FORCE)

# Disable Crow's examples/tests + install rules.
set(CROW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CROW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
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

target_link_libraries(wf_casc_server PRIVATE
    whiteout_lib   # findBlizzardGames (auto-discover WC3 install)
    whiteout_casc
    Crow::Crow
)

# Crow doesn't propagate ASIO_INCLUDE_DIR to consumers — set explicitly.
target_include_directories(wf_casc_server PRIVATE "${ASIO_INCLUDE_DIR}")
target_compile_definitions(wf_casc_server PRIVATE ASIO_STANDALONE)
if(WIN32)
    # Asio warns + falls back to Win7 without this; target Win10/11.
    target_compile_definitions(wf_casc_server PRIVATE _WIN32_WINNT=0x0A00)
endif()
