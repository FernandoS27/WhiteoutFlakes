#pragma once

/// @file
/// @brief Seeds and reserves slots in a `ParticlePage` for newly spawning particles.

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/sim/medium.hpp>
#include <cornflakes/sim/particle_page.hpp>

namespace whiteout::cornflakes {

/// @brief Engine-locked addend mixed into per-child seeds (`parentRandState + RSM + 111`).
inline constexpr u32 kRandStateSpawnAddend = 111U;

/// @brief Per-spawn-batch parameters consumed by `SpawnProcessor::setupStream`.
struct SpawnContext {

    u32 parentSeed = 0U;

    u32 count = 0U;
};

/// @brief Allocates new particle slots and seeds their RandState/SelfId/lifeRatio streams.
class SpawnProcessor {
public:
    bool setupStream(ParticlePage& page, MediumState& medium, const SpawnContext& ctx,
                     IssueBag& issues) const;
};

} // namespace whiteout::cornflakes
