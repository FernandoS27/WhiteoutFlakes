# ============================================================================
# WhiteoutFlakes — 3ds Max plugin (.dlx)
#
# Builds a Max plugin DLL (`WhiteoutFlakes.dlx`) that hosts the WhiteoutFlakes
# renderer inside 3ds Max. Provides MaxScript entry points (ndxStart / ndxStop
# / ndxRefreshMaterials).
#
# Configure with:
#   -DWDX_BUILD_MAX_PLUGIN=ON
#   -DMAX_VERSION=2026         (or 2024/2025/...; selects the Autodesk SDK)
#   -DMAXSDK_PATH=<path>       (optional; auto-derived from MAX_VERSION)
# ============================================================================

# ---- Max SDK location ------------------------------------------------------

if(NOT DEFINED MAX_VERSION)
    message(FATAL_ERROR
        "MAX_VERSION not set. Pass -DMAX_VERSION=<year> when configuring.\n"
        "Example: cmake -DWDX_BUILD_MAX_PLUGIN=ON -DMAX_VERSION=2026 ..")
endif()

if(NOT DEFINED MAXSDK_PATH)
    set(MAXSDK_PATH "C:/Program Files/Autodesk/3ds Max ${MAX_VERSION} SDK/maxsdk")
endif()

if(NOT EXISTS "${MAXSDK_PATH}/include/max.h")
    message(FATAL_ERROR
        "3ds Max ${MAX_VERSION} SDK not found at: ${MAXSDK_PATH}\n"
        "Override with -DMAXSDK_PATH=<path-to-maxsdk>.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "3ds Max plugins require 64-bit builds only.")
endif()

message(STATUS "WhiteoutFlakes Max plugin: SDK = ${MAXSDK_PATH} (Max ${MAX_VERSION})")

# Reusable interface target carrying Max SDK include path, link dirs, and
# the boilerplate Win32 macros every Max plugin needs.
add_library(MaxSDK INTERFACE)
target_include_directories(MaxSDK INTERFACE "${MAXSDK_PATH}/include")
target_link_directories   (MaxSDK INTERFACE "${MAXSDK_PATH}/lib/x64/Release")
target_compile_definitions(MaxSDK INTERFACE
    WIN32 _WIN64 _WINDOWS _USRDLL
    UNICODE _UNICODE
    NOMINMAX WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

# ---- Plugin DLL ------------------------------------------------------------

set(BASIC_VIEWER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../basic_viewer")
set(TOOLS_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common")

add_library(WhiteoutFlakesMaxPlugin SHARED
    dllmain.cpp
    max_scene_adapter.cpp
    WhiteoutDexExtractor.def
    resource.rc

    # Win32 host window — Max-only after the basic_viewer moved to GLFW + ImGui.
    render_window.cpp

    # INI loader is still shared with the standalone viewer; the cubeb sound
    # emitter is a shared host utility under tools/common.
    "${BASIC_VIEWER_DIR}/settings_ini.cpp"
    "${TOOLS_COMMON_DIR}/cubeb_sound_emitter.cpp"
)

set_target_properties(WhiteoutFlakesMaxPlugin PROPERTIES
    CXX_STANDARD          20
    CXX_STANDARD_REQUIRED ON
    SUFFIX                ".dlx"
    OUTPUT_NAME           "WhiteoutFlakes"
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/plugins"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/plugins"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/plugins"
)

target_include_directories(WhiteoutFlakesMaxPlugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${BASIC_VIEWER_DIR}"
    "${TOOLS_COMMON_DIR}"
    "${CMAKE_SOURCE_DIR}/externals/dr_libs"
)

target_compile_definitions(WhiteoutFlakesMaxPlugin PRIVATE
    NDX_MAX_VERSION=${MAX_VERSION}
)

target_link_libraries(WhiteoutFlakesMaxPlugin PRIVATE
    WhiteoutFlakesLib
    MaxSDK
    cubeb
    core.lib
    maxutil.lib
    geom.lib
    mesh.lib
    bmm.lib
    paramblk2.lib
    maxscrpt.lib
    comdlg32
)

if(MSVC)
    target_compile_options(WhiteoutFlakesMaxPlugin PRIVATE
        /W3 /EHsc
        /wd4100 /wd4127 /wd4244 /wd4267
        /permissive
        /Zc:strictStrings-
    )
endif()
