#pragma once

#include <vector>
#include "renderer/particle/base/particle2.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::particle {

class ParticlePool {
public:
    // Grow to hold `capacity` particles; the emitter decides how many it needs.
    void Sync(u32 capacity);
    void Clear();
    void Compact();

    Particle2& operator[](usize idx) {
        return particles_[idx];
    }
    const Particle2& operator[](usize idx) const {
        return particles_[idx];
    }

    usize AliveCount() const {
        return alive_.size();
    }
    u32 AliveAt(usize i) const {
        return alive_[i];
    }
    void RemoveAliveAt(usize i);
    /// @brief Remove the live entry at @p i, keeping the others in order — the D3
    ///        client's `ParticleSystem_FreeParticle` @0x71000AF790. @ref RemoveAliveAt
    ///        is the other profiles' swap-with-last. M2_PARTICLE_DESIGN.md §11.13.
    void RemoveAliveAtOrdered(usize i);

    bool DeadEmpty() const {
        return dead_.empty();
    }
    u32 PopDead();
    void PushDead(u32 idx);
    void PushAlive(u32 idx) {
        alive_.push_back(idx);
    }

    usize Capacity() const {
        return particles_.size();
    }

private:
    std::vector<Particle2> particles_;
    std::vector<u32> alive_;
    std::vector<u32> dead_;
};

} // namespace whiteout::flakes::renderer::particle
