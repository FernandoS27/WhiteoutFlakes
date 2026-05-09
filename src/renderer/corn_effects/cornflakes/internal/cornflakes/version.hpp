#pragma once

/// @file
/// @brief Library version reporting (major/minor/patch + a string form).

#include <string_view>

namespace whiteout::cornflakes {

/// @brief Semantic version triple.
struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

Version libraryVersion() noexcept;

std::string_view libraryVersionString() noexcept;

} // namespace whiteout::cornflakes
