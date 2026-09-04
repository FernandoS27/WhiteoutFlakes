# ============================================================================
# embed_shaders.cmake — Convert compiled DXBC blobs into a C++ header
#
# Inputs (set via -D on the cmake command line):
#   SHADER_DIR — directory containing the .dxbc files
#   OUTPUT     — path to the output .h file
#   EMBED_SPV  — ON (default) to embed the SPIR-V bytes, OFF to stub them.
#                A build with no Vulkan backend still needs the `*Spv` symbols
#                to exist for the per-backend selectors, but not their ~780 KB
#                of payload: the WebGPU-only wasm carried a quarter of its size
#                in blobs nothing could reach.
# Variable names are derived from filenames (e.g. kLineVS.dxbc → kLineVS).
# ============================================================================
cmake_minimum_required(VERSION 3.16)

file(GLOB dxbc_files "${SHADER_DIR}/*.dxbc")
file(GLOB spv_files  "${SHADER_DIR}/*.spv")
file(GLOB wgsl_files "${SHADER_DIR}/*.wgsl")
file(GLOB mtl_files  "${SHADER_DIR}/*.metallib")
list(SORT dxbc_files)
list(SORT spv_files)
list(SORT wgsl_files)
list(SORT mtl_files)

list(LENGTH dxbc_files dxbc_count)
list(LENGTH spv_files  spv_count)
list(LENGTH wgsl_files wgsl_count)
list(LENGTH mtl_files  mtl_count)
# DXBC compilation is Windows-only (slangc -target dxbc needs fxc.exe). On
# Linux/macOS the dxbc list is empty; the consumer code's `vk ? *Spv : *`
# expressions still need the DXBC symbols to exist, so we emit 1-byte stubs
# below. SPV is mandatory on every platform — bail if even those are missing.
if(spv_count EQUAL 0)
    message(FATAL_ERROR "No .spv files found in ${SHADER_DIR}")
endif()

# The .spv list stays the master enumeration of shader names either way — the
# DXBC and WGSL stub passes below key off it — so SPIR-V is always compiled and
# only its bytes are optional.
if(NOT DEFINED EMBED_SPV)
    set(EMBED_SPV ON)
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

    if(EMBED_SPV)
        file(READ "${binpath}" hex HEX)
        file(SIZE "${binpath}" bytesize)

        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " hex "${hex}")
        string(REGEX REPLACE ", $" "" hex "${hex}")

        string(APPEND header "// ${varname}Spv — ${bytesize} bytes (SPIR-V)\n")
        string(APPEND header "inline constexpr uint8_t ${varname}Spv[] = {\n    ${hex}\n};\n\n")
    else()
        string(APPEND header
            "// ${varname}Spv — stub (no Vulkan path on this build)\n"
            "inline constexpr uint8_t ${varname}Spv[] = { 0x00 };\n\n")
    endif()

    list(FIND real_dxbc_names "${varname}" found_idx)
    if(found_idx EQUAL -1)
        string(APPEND header
            "// ${varname} — DXBC stub (no D3D path on this build)\n"
            "inline constexpr uint8_t ${varname}[] = { 0x00 };\n\n")
    endif()
endforeach()

# WGSL blobs (consumed by the WebGPU backend): emitted as `<varname>Wgsl`.
# WGSL is UTF-8 text — we emit it as a byte array (matching the SPV / DXBC
# pattern) with a NUL terminator so the WebGPU backend can treat the buffer
# as a C string when populating ShaderModuleWGSLDescriptor::code. The
# consumer chooses `*Wgsl` for WebGPU and stubs for every other backend so
# existing call sites stay valid even when WGSL emission is off.
#
# Slangc emits the entry function under its source name (vsLine, psLine,
# vsViewCube, …). The WebGPU backend always asks for "main", so we
# rewrite each .wgsl in place to rename the @vertex / @fragment / @compute
# function to `main`. The regex is idempotent (already-renamed files are
# no-ops on re-embed), and applies only to the function immediately
# following an entry-stage attribute, so helper functions emitted by
# slangc are left alone.
set(real_wgsl_names "")
foreach(srcpath IN LISTS wgsl_files)
    get_filename_component(varname "${srcpath}" NAME_WE)
    list(APPEND real_wgsl_names "${varname}")

    file(READ "${srcpath}" wgsl_text)
    string(REGEX REPLACE
        "(@(vertex|fragment|compute)[ \t\r\n]+fn[ \t]+)[A-Za-z_][A-Za-z0-9_]*"
        "\\1main"
        wgsl_text "${wgsl_text}")
    file(WRITE "${srcpath}" "${wgsl_text}")

    file(READ "${srcpath}" hex HEX)
    file(SIZE "${srcpath}" bytesize)

    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " hex "${hex}")
    # Trailing NUL so the WGSL string is null-terminated when CreateShader
    # treats `bytecode` as a UTF-8 source pointer.
    string(APPEND hex "0x00")
    math(EXPR bytesize_plus1 "${bytesize} + 1")

    string(APPEND header "// ${varname}Wgsl — ${bytesize_plus1} bytes (WGSL UTF-8 + NUL)\n")
    string(APPEND header "inline constexpr uint8_t ${varname}Wgsl[] = {\n    ${hex}\n};\n\n")
endforeach()

# Stub any WGSL symbol that wasn't produced, so the per-backend selector
# `case WebGPU: bytes = kFooWgsl` still type-checks on platforms where
# WGSL emission is skipped.
set(stub_names "")
foreach(name IN LISTS real_dxbc_names)
    list(APPEND stub_names "${name}")
endforeach()
foreach(srcpath IN LISTS spv_files)
    get_filename_component(varname "${srcpath}" NAME_WE)
    list(APPEND stub_names "${varname}")
endforeach()
list(REMOVE_DUPLICATES stub_names)
foreach(name IN LISTS stub_names)
    list(FIND real_wgsl_names "${name}" found_idx)
    if(found_idx EQUAL -1)
        string(APPEND header
            "// ${name}Wgsl — stub (no WebGPU path on this build)\n"
            "inline constexpr uint8_t ${name}Wgsl[] = { 0x00 };\n\n")
    endif()
endforeach()

# Metal library blobs (consumed by the native Metal backend): emitted as
# `<varname>Mtl`. The Metal CreateShader path wraps these bytes in a
# dispatch_data + newLibraryWithData; the entry-point name is resolved
# at runtime by querying [lib functionNames] for the matching stage, so
# the embedded blob doesn't need any name rewriting. macOS-only.
set(real_mtl_names "")
foreach(binpath IN LISTS mtl_files)
    get_filename_component(varname "${binpath}" NAME_WE)
    list(APPEND real_mtl_names "${varname}")

    file(READ "${binpath}" hex HEX)
    file(SIZE "${binpath}" bytesize)

    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " hex "${hex}")
    string(REGEX REPLACE ", $" "" hex "${hex}")

    string(APPEND header "// ${varname}Mtl — ${bytesize} bytes (Metal library binary)\n")
    string(APPEND header "inline constexpr uint8_t ${varname}Mtl[] = {\n    ${hex}\n};\n\n")
endforeach()

# Stub any Mtl symbol that wasn't produced — same pattern as WGSL stubs
# above so non-macOS builds still type-check the
# `case Metal: bytes = kFooMtl` consumer arm.
foreach(name IN LISTS stub_names)
    list(FIND real_mtl_names "${name}" found_idx)
    if(found_idx EQUAL -1)
        string(APPEND header
            "// ${name}Mtl — stub (no Metal path on this build)\n"
            "inline constexpr uint8_t ${name}Mtl[] = { 0x00 };\n\n")
    endif()
endforeach()

string(APPEND header "} // namespace whiteout::flakes::Shaders\n")

file(WRITE "${OUTPUT}" "${header}")
if(EMBED_SPV)
    set(spv_note "${spv_count} spv")
else()
    set(spv_note "${spv_count} spv STUBBED")
endif()
message(STATUS
    "Generated compiled_shaders.h "
    "(${dxbc_count} dxbc + ${spv_note} + ${wgsl_count} wgsl + ${mtl_count} metallib)")
