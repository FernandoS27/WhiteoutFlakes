#include <cornflakes/interface/sim/particle_pool.hpp>

namespace whiteout::cornflakes {

void ParticlePool::resize(std::size_t count) {
    particles_.resize(count);
}

void ParticlePool::resizeForLayer(const LayerProgram& layer) {
    for (auto& p : particles_) {
        p.resizeForLayer(layer);
    }
}

bool ParticlePool::initBatch(const LayerProgram& layer, u32 baseSeed, IArena& arena,
                             IssueBag& issues) {
    for (std::size_t i = 0; i < particles_.size(); ++i) {
        particles_[i].setRngSeed(baseSeed + static_cast<u32>(i));
        if (!particles_[i].initParticle(layer, arena, issues)) {
            return false;
        }
    }
    return true;
}

bool ParticlePool::initRange(const LayerProgram& layer, u32 baseSeed, std::size_t startIdx,
                             std::size_t count, IArena& arena, IssueBag& issues) {
    if (startIdx >= particles_.size() || count == 0U) {
        return true;
    }
    const std::size_t end = std::min(startIdx + count, particles_.size());
    for (std::size_t i = startIdx; i < end; ++i) {
        particles_[i].setRngSeed(baseSeed + static_cast<u32>(i));
        if (!particles_[i].initParticle(layer, arena, issues)) {
            return false;
        }
    }
    return true;
}

bool ParticlePool::tickBatch(const LayerProgram& layer, IArena& arena, IssueBag& issues) {
    for (auto& p : particles_) {

        if (p.wasDeadAtFrameStart()) {
            continue;
        }
        if (!p.tick(layer, arena, issues)) {
            return false;
        }
    }
    return true;
}

} // namespace whiteout::cornflakes
