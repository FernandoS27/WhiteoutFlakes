#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/schema/handles.hpp>
#include <cornflakes/sim/particle_page.hpp>

#include <vector>

namespace whiteout::cornflakes {

struct MediumState {
    EmitterId emitter;
    LayerId layer;
    u32 effectIdValue = 0;
    u32 randomSeedModifier = 0;
    u64 nextSelfId = 1;

    std::vector<ParticlePage> pages;
};

}
