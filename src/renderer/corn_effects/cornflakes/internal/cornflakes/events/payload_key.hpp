#pragma once

/// @file
/// @brief Composite key (generator/payload/element) used to address payloads in the cache.

#include <cornflakes/interface/core/types.hpp>

namespace whiteout::cornflakes {

/// @brief Three-tier identifier for a payload blob in the cache.
struct PayloadKey {
    u32 generatorKey = 0;
    u32 payloadKey = 0;
    u32 elementKey = 0;

    constexpr bool operator==(const PayloadKey&) const = default;
};

} // namespace whiteout::cornflakes
