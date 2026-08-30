#pragma once

#include <vector>
#include "particle2.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::particle {

class ParticlePool {
public:
    // Grow to hold `capacity` particles. The pool no longer derives that from
    // the emission model — how many particles an emitter needs is the
    // emitter's business, not the container's.
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
    /// @brief Remove the live entry at @p i, keeping the others in order.
    ///
    /// `ParticleSystem_FreeParticle` @0x71000AF790 memmoves the pointers above
    /// the hole down and rewrites each survivor's index (G-D3P-20), so a D3
    /// system's emission order survives every death and the draw order that
    /// follows from it is stable. @ref RemoveAliveAt is the swap-with-last the
    /// other profiles use; whether their clients compact is a separate question
    /// nothing here has measured, so this is a second entry point rather than a
    /// change to theirs.
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
