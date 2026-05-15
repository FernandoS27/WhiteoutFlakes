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
# DXBC compilation is Windows-only (slangc -target dxbc needs fxc.exe). On
# Linux/macOS the dxbc list is empty; the consumer code's `vk ? *Spv : *`
# expressions still need the DXBC symbols to exist, so we emit 1-byte stubs
# below. SPV is mandatory on every platform — bail if even those are missing.
if(spv_count EQUAL 0)
    message(FATAL_ERROR "No .spv files found in ${SHADER_DIR}")
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
# Track which symbols got real DXBC bytes so the SPV pass below can stub the
# rest (Linux/macOS: no DXBC at all, so every name is stubbed).
set(real_dxbc_names "")
foreach(binpath IN LISTS dxbc_files)
    get_filename_component(varname "${binpath}" NAME_WE)
    list(APPEND real_dxbc_names "${varname}")

    file(READ "${binpath}" hex HEX)
    file(SIZE "${binpath}" bytesize)

    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " hex "${hex}")
    string(REGEX REPLACE ", $" "" hex "${hex}")

    string(APPEND header "// ${varname} — ${bytesize} bytes (DXBC sm_5_0)\n")
    string(APPEND header "inline constexpr uint8_t ${varname}[] = {\n    ${hex}\n};\n\n")
endforeach()

# SPIR-V blobs (consumed by the Vulkan backend): emitted as `<varname>Spv`.
# Vulkan callers pick the `*Spv` variant; the existing D3D call sites
# stay unchanged. While iterating, emit a 1-byte DXBC stub for any varname
# that didn't get a real DXBC blob above (Linux/macOS path) so the consumer
# code's `vk ? *Spv : <varname>` expression still type-checks.
foreach(binpath IN LISTS spv_files)
    get_filename_component(varname "${binpath}" NAME_WE)

    file(READ "${binpath}" hex HEX)
    file(SIZE "${binpath}" bytesize)

    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " hex "${hex}")
    string(REGEX REPLACE ", $" "" hex "${hex}")

    string(APPEND header "// ${varname}Spv — ${bytesize} bytes (SPIR-V)\n")
    string(APPEND header "inline constexpr uint8_t ${varname}Spv[] = {\n    ${hex}\n};\n\n")

    list(FIND real_dxbc_names "${varname}" found_idx)
    if(found_idx EQUAL -1)
        string(APPEND header
            "// ${varname} — DXBC stub (no D3D path on this build)\n"
            "inline constexpr uint8_t ${varname}[] = { 0x00 };\n\n")
    endif()
endforeach()

string(APPEND header "} // namespace whiteout::flakes::Shaders\n")

file(WRITE "${OUTPUT}" "${header}")
message(STATUS "Generated compiled_shaders.h (${dxbc_count} dxbc + ${spv_count} spv)")
