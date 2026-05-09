#include <cornflakes/core/determinism.hpp>
#include <cornflakes/sim/particle_page.hpp>

namespace whiteout::cornflakes {

ParticlePage allocateParticlePage(IArena& pageArena, u32 capacity) {
    ParticlePage page;
    page.capacity = capacity;
    page.particleCount = 0;

    if (capacity == 0U) {
        return page;
    }

    page.selfIds = arenaArray<u64>(pageArena, capacity);
    page.effectIds = arenaArray<u32>(pageArena, capacity);
    page.parentIds = arenaArray<u64>(pageArena, capacity);
    page.randStates = arenaArray<u32>(pageArena, capacity);
    page.lifeRatios = arenaArray<f32>(pageArena, capacity);
    page.metaData = arenaArray<u32>(pageArena, capacity);
    page.positions = arenaArray<Float3>(pageArena, capacity);

    return page;
}

} // namespace whiteout::cornflakes
