#pragma once

#include "common_types.h"
#include "particle2.h"
#include <vector>

namespace WhiteoutDex::particle {

class ParticlePool {
public:
    void Sync(f32 emissionRate, f32 lifeSpan);
    void Clear();
    void Compact();

    Particle2&       operator[](usize idx)       { return particles_[idx]; }
    const Particle2& operator[](usize idx) const { return particles_[idx]; }

    usize AliveCount() const { return alive_.size(); }
    u32   AliveAt(usize i) const { return alive_[i]; }
    void  RemoveAliveAt(usize i);

    bool DeadEmpty() const { return dead_.empty(); }
    u32  PopDead();
    void PushDead(u32 idx);
    void PushAlive(u32 idx) { alive_.push_back(idx); }

    usize Capacity() const { return particles_.size(); }

private:
    std::vector<Particle2> particles_;
    std::vector<u32>       alive_;
    std::vector<u32>       dead_;
};

}
