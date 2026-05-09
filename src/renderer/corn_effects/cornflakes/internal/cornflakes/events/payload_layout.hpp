#pragma once

/// @file
/// @brief Byte-layout constants for payload cache blobs.

#include <cornflakes/interface/core/types.hpp>

#include <cstddef>

namespace whiteout::cornflakes {

inline constexpr std::size_t kPayloadCacheHeaderBytes = 16U;  ///< Header bytes before element 0.
inline constexpr std::size_t kPayloadCacheElementBytes = 24U; ///< Per-element stride.

/// @brief Total blob size needed for `elementCount` payload elements.
constexpr std::size_t computePayloadCacheByteCount(std::size_t elementCount) noexcept {
    return (kPayloadCacheElementBytes * elementCount) + kPayloadCacheHeaderBytes;
}

} // namespace whiteout::cornflakes
