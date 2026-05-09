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
list(SORT dxbc_files)

list(LENGTH dxbc_files bin_count)
if(bin_count EQUAL 0)
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

foreach(binpath IN LISTS dxbc_files)
    get_filename_component(varname "${binpath}" NAME_WE)

    file(READ "${binpath}" hex HEX)
    file(SIZE "${binpath}" bytesize)

    # Format hex pairs as "0x??, " and remove trailing comma
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " hex "${hex}")
    string(REGEX REPLACE ", $" "" hex "${hex}")

    string(APPEND header "// ${varname} — ${bytesize} bytes\n")
    string(APPEND header "inline constexpr uint8_t ${varname}[] = {\n    ${hex}\n};\n\n")
endforeach()

string(APPEND header "} // namespace whiteout::flakes::Shaders\n")

file(WRITE "${OUTPUT}" "${header}")
message(STATUS "Generated compiled_shaders.h (${bin_count} shaders)")
