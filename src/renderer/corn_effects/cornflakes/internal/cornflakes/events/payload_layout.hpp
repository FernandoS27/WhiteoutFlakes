#pragma once

#include <cornflakes/interface/core/types.hpp>

#include <cstddef>

namespace whiteout::cornflakes {

inline constexpr std::size_t kPayloadCacheHeaderBytes = 16U;
inline constexpr std::size_t kPayloadCacheElementBytes = 24U;

constexpr std::size_t computePayloadCacheByteCount(std::size_t elementCount) noexcept {
    return (kPayloadCacheElementBytes * elementCount) + kPayloadCacheHeaderBytes;
}

}
