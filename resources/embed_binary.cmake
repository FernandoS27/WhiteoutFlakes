# ============================================================================
# embed_binary.cmake — Convert a binary file into a single-array C++ header.
#
# Inputs (set via -D on the cmake command line):
#   INPUT     — path to the binary file
#   OUTPUT    — path to the output .h file
#   VAR_NAME  — name of the constexpr uint8_t[] to emit
#   NAMESPACE — fully-qualified namespace (e.g. whiteout::flakes::io)
# ============================================================================
cmake_minimum_required(VERSION 3.16)

if(NOT EXISTS "${INPUT}")
    message(FATAL_ERROR "embed_binary: INPUT does not exist: ${INPUT}")
endif()

file(READ "${INPUT}" hex HEX)
file(SIZE "${INPUT}" bytesize)

string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " hex "${hex}")
string(REGEX REPLACE ", $" "" hex "${hex}")

get_filename_component(srcname "${INPUT}" NAME)

set(content "#pragma once
// ============================================================================
// Auto-generated from ${srcname} — do not edit.
// Re-run CMake and rebuild to regenerate.
// ============================================================================
#include <cstdint>

namespace ${NAMESPACE} {

// ${VAR_NAME} — ${bytesize} bytes
inline constexpr uint8_t ${VAR_NAME}[] = {
    ${hex}
};

} // namespace ${NAMESPACE}
")

file(WRITE "${OUTPUT}" "${content}")
message(STATUS "Generated ${OUTPUT} (${bytesize} bytes from ${srcname})")
