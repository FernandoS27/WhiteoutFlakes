#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/sim/medium.hpp>
#include <cornflakes/sim/particle_page.hpp>

namespace whiteout::cornflakes {

inline constexpr u32 kRandStateSpawnAddend = 111U;

struct SpawnContext {

    u32 parentSeed = 0U;

    u32 count = 0U;
};

class SpawnProcessor {
public:
    bool setupStream(ParticlePage& page, MediumState& medium, const SpawnContext& ctx,
                     IssueBag& issues) const;
};

}
