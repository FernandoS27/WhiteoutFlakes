#pragma once

#include <string_view>

namespace whiteout::cornflakes {

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
};

Version libraryVersion() noexcept;

std::string_view libraryVersionString() noexcept;

}
