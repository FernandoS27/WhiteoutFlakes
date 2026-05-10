# ============================================================================
# embed_shaders.cmake — Convert compiled DXBC blobs into a C++ header
#
# Inputs (set via -D on the cmake command line):
#   SHADER_DIR — directory containing the .dxbc files
#   OUTPUT     — path to the output .h file
# Variable names are derived from filenames (e.g. kLineVS.dxbc → kLineVS).
# ============================================================================
cmake_minimum_required(VERSION 3.16)

file(GLOB dxbc_files "${SHADER_DIR}/*.dxbc")
file(GLOB spv_files  "${SHADER_DIR}/*.spv")
list(SORT dxbc_files)
list(SORT spv_files)

list(LENGTH dxbc_files dxbc_count)
list(LENGTH spv_files  spv_count)
if(dxbc_count EQUAL 0)
    message(FATAL_ERROR "No .dxbc files found in ${SHADER_DIR}")
endif()

# Header preamble
set(header [=[#pragma once
// ============================================================================
// Auto-generated from Slang shader compilation — do not edit.
// Re-run CMake and rebuild to regenerate.
// ============================================================================
#include <cstdint>

namespace whiteout::flakes::Shaders {

]=])

# DXBC blobs (consumed by D3D11 / D3D12 backends): emitted as `<varname>`.
foreach(binpath IN LISTS dxbc_files)
    get_filename_component(varname "${binpath}" NAME_WE)

    file(READ "${binpath}" hex HEX)
    file(SIZE "${binpath}" bytesize)

    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " hex "${hex}")
    string(REGEX REPLACE ", $" "" hex "${hex}")

    string(APPEND header "// ${varname} — ${bytesize} bytes (DXBC sm_5_0)\n")
    string(APPEND header "inline constexpr uint8_t ${varname}[] = {\n    ${hex}\n};\n\n")
endforeach()

# SPIR-V blobs (consumed by the Vulkan backend): emitted as `<varname>Spv`.
# Vulkan callers pick the `*Spv` variant; the existing D3D call sites
# stay unchanged.
foreach(binpath IN LISTS spv_files)
    get_filename_component(varname "${binpath}" NAME_WE)

    file(READ "${binpath}" hex HEX)
    file(SIZE "${binpath}" bytesize)

    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " hex "${hex}")
    string(REGEX REPLACE ", $" "" hex "${hex}")

    string(APPEND header "// ${varname}Spv — ${bytesize} bytes (SPIR-V)\n")
    string(APPEND header "inline constexpr uint8_t ${varname}Spv[] = {\n    ${hex}\n};\n\n")
endforeach()

string(APPEND header "} // namespace whiteout::flakes::Shaders\n")

file(WRITE "${OUTPUT}" "${header}")
message(STATUS "Generated compiled_shaders.h (${dxbc_count} dxbc + ${spv_count} spv)")
