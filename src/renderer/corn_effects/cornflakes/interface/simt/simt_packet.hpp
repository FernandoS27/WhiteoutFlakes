#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/vm/bytecode_exec_context.hpp>

#include <cstddef>
#include <span>

namespace whiteout::cornflakes::simt {

#ifndef CORNFLAKES_SIMT_LANES
#define CORNFLAKES_SIMT_LANES 32
#endif
inline constexpr std::size_t kSimtLanes = static_cast<std::size_t>(CORNFLAKES_SIMT_LANES);
static_assert(kSimtLanes >= 1U && kSimtLanes <= 32U, "LaneMask is u32");

using LaneMask = u32;

inline constexpr LaneMask kAllLanesLive =
    kSimtLanes >= 32U ? 0xFFFFFFFFU : static_cast<LaneMask>((1ULL << kSimtLanes) - 1ULL);

inline constexpr LaneMask maskForLiveCount(std::size_t count) noexcept {
    if (count >= kSimtLanes) {
        return kAllLanesLive;
    }
    if (count == 0U) {
        return 0U;
    }
    return static_cast<LaneMask>((1ULL << count) - 1ULL);
}

inline constexpr bool laneIsLive(LaneMask mask, std::size_t lane) noexcept {
    return lane < kSimtLanes && ((mask >> lane) & 1U) != 0U;
}

inline constexpr std::size_t liveLaneCount(LaneMask mask) noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < kSimtLanes; ++i) {
        n += ((mask >> i) & 1U);
    }
    return n;
}

struct SimtPacket {
    std::span<BytecodeExecContext* const> laneContexts;

    LaneMask executionMask = kAllLanesLive;

    std::size_t baseParticleIndex = 0U;

    std::span<const std::size_t> laneParticles;

    static constexpr std::size_t kNoParticle = static_cast<std::size_t>(-1);

    bool isLive(std::size_t lane) const noexcept {
        return laneIsLive(executionMask, lane) && lane < laneContexts.size() &&
               laneContexts[lane] != nullptr;
    }

    LaneMask liveMask() const noexcept {
        LaneMask m = 0U;
        for (std::size_t lane = 0; lane < kSimtLanes; ++lane) {
            if (isLive(lane)) {
                m |= (1U << lane);
            }
        }
        return m;
    }
};

}
