#pragma once

// ============================================================================
// WhiteoutFlakes — UTF-8 / std::filesystem::path helpers.
// ============================================================================

#include "../types.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace whiteout::flakes::io {

// Defined out-of-line in path_utf8.cpp so this header doesn't have to drag
// <windows.h> into every TU that just needs the conversion.
std::filesystem::path FsPathFromUtf8(std::string_view utf8);

inline std::string PathToUtf8(const std::filesystem::path& p) {
    auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

} // namespace whiteout::flakes::io

namespace whiteout::flakes {
using ::whiteout::flakes::io::FsPathFromUtf8;
using ::whiteout::flakes::io::PathToUtf8;
} // namespace whiteout::flakes
