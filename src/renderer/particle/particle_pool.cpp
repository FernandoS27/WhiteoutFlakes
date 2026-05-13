#include "renderer/particle/particle_pool.h"

namespace whiteout::flakes::renderer::particle {

namespace {

u32 CeilPow2(u32 x) {
    if (x == 0)
        return 1;
    if ((x & (x - 1)) == 0)
        return x;
    u32 v = x - 1;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

} // namespace

void ParticlePool::Sync(f32 emissionRate, f32 lifeSpan) {
    if (emissionRate <= 0.0f || lifeSpan <= 0.0f)
        return;

    u32 arraySize = static_cast<u32>(1.15f * emissionRate * lifeSpan);
    if (arraySize == 0)
        return;
    u32 oldSize = static_cast<u32>(particles_.size());
    if (oldSize >= arraySize)
        return;

    u32 reserve = CeilPow2(arraySize);
    particles_.reserve(reserve);
    alive_.reserve(reserve);
    dead_.reserve(reserve);

    particles_.resize(arraySize);

    for (u32 u = oldSize; u < arraySize; ++u) {
        dead_.push_back(u);
    }
}

void ParticlePool::Clear() {
    alive_.clear();
    dead_.clear();

    dead_.reserve(particles_.size());
    for (u32 i = 0; i < particles_.size(); ++i) {
        dead_.push_back(i);
    }
}

void ParticlePool::Compact() {
    particles_.clear();
    particles_.shrink_to_fit();
    alive_.clear();
    alive_.shrink_to_fit();
    dead_.clear();
    dead_.shrink_to_fit();
}

void ParticlePool::RemoveAliveAt(usize i) {

    if (i + 1 < alive_.size()) {
        alive_[i] = alive_.back();
    }
    alive_.pop_back();
}

u32 ParticlePool::PopDead() {
    u32 idx = dead_.back();
    dead_.pop_back();
    return idx;
}

void ParticlePool::PushDead(u32 idx) {
    dead_.push_back(idx);
}

} // namespace whiteout::flakes::renderer::particle
