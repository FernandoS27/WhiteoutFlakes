#pragma once

/// @file path_utf8.h
/// @brief UTF-8 ↔ `std::filesystem::path` helpers that keep host code
///        free of platform-specific wide-char conversions.

#include "../types.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace whiteout::flakes::io {

/// @brief Build an `fs::path` from a UTF-8 string view.
///
/// On Windows this widens via `MultiByteToWideChar(CP_UTF8, …)`; on POSIX
/// it's a direct pass-through. Defined out-of-line in `path_utf8.cpp` so
/// this header doesn't have to drag `<windows.h>` into every TU.
std::filesystem::path FsPathFromUtf8(std::string_view utf8);

/// @brief Render any `fs::path` as a UTF-8 `std::string`.
///
/// Wraps `path::u8string()` and re-interprets the resulting `char8_t`
/// buffer as `char` for compatibility with APIs that take plain
/// `std::string`.
inline std::string PathToUtf8(const std::filesystem::path& p) {
    auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

} // namespace whiteout::flakes::io

namespace whiteout::flakes {
using ::whiteout::flakes::io::FsPathFromUtf8;
using ::whiteout::flakes::io::PathToUtf8;
} // namespace whiteout::flakes
