#pragma once

/// @file
/// @brief Per-emitter "medium" — owns the page array, scratch counters, and seed offset.

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/schema/handles.hpp>
#include <cornflakes/sim/particle_page.hpp>

#include <vector>

namespace whiteout::cornflakes {

/// @brief Live state for one emitter/layer pairing — the container the simulation ticks into.
struct MediumState {
    EmitterId emitter;
    LayerId layer;
    u32 effectIdValue = 0;
    u32 randomSeedModifier = 0;
    u64 nextSelfId = 1; ///< Monotonic id allocator for newly spawned particles.

    std::vector<ParticlePage> pages;
};

} // namespace whiteout::cornflakes
