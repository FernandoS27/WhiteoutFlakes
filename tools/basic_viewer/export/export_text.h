#pragma once

// ============================================================================
// The string handling the converters share: case folding, the fixed-width
// field trim, slash spelling. Their own, not tools/common's, so the conversion
// library depends on nothing else under tools/.
// ============================================================================

#include <algorithm>
#include <cctype>
#include <string>

namespace whiteout::flakes::export_text {

inline std::string Lower(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return value;
}

/// An `.m3` string is a fixed-width field, so a path arrives with its NULs.
inline std::string TrimFixedWidth(std::string value) {
    while (!value.empty() && (value.back() == '\0' || value.back() == ' ')) {
        value.pop_back();
    }
    return value;
}

/// Backslashes as forward slashes: the spelling `std::filesystem` splits on.
inline std::string ForwardSlashes(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

} // namespace whiteout::flakes::export_text
