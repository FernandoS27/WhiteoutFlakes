#pragma once

/// @file
/// @brief Fixed-capacity SoA particle page used by the medium-based simulation path.

#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/service/service_types.hpp>

#include <span>

namespace whiteout::cornflakes {

/// @brief Index of the always-present per-particle streams.
enum class BuiltinStream : u32 {
    SelfId = 0,
    EffectId = 1,
    ParentId = 2,
    RandState = 3,
    LifeRatio = 4,
    MetaData = 5,

    Position = 6,
    Count,
};

/// @brief One SoA chunk of `capacity` particle slots, of which `particleCount` are alive.
struct ParticlePage {
    u32 particleCount = 0;
    u32 capacity = 0;

    std::span<u64> selfIds;
    std::span<u32> effectIds;
    std::span<u64> parentIds;
    std::span<u32> randStates;
    std::span<f32> lifeRatios;
    std::span<u32> metaData;

    std::span<Float3> positions;

    Float3 emitterPosition;
};

/// @brief Allocate a zero-initialised page of `capacity` slots in `pageArena`.
ParticlePage allocateParticlePage(IArena& pageArena, u32 capacity);

} // namespace whiteout::cornflakes
