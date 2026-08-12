#include "renderer/particle/particle2_emitter.h"

#include "whiteout/flakes/model_types.h" // FrameState::ParticleFrameState
#include "whiteout/flakes/util/coordinate_system.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle {

namespace {

constexpr f32 kMaxDt = 0.5f;

// Seed an emitter starts with when the caller never calls SetSeed. Fixed rather
// than counter-derived so an un-seeded emitter is still reproducible.
constexpr u32 kDefaultEmitterSeed = 0x1234567u;

// Keeps desc_ (and desc_->shape) non-null so every accessor and CreateParticle
// can dereference unconditionally.
const std::shared_ptr<const EmitterDesc>& DefaultDesc() {
    static const std::shared_ptr<const EmitterDesc> d = [] {
        auto e = std::make_shared<EmitterDesc>();
        e->shape = std::make_shared<PlaneShape>();
        return e;
    }();
    return d;
}

} // namespace

Emitter2::Emitter2() : desc_(DefaultDesc()) {
    SetSeed(kDefaultEmitterSeed);
}

void Emitter2::SetDesc(std::shared_ptr<const EmitterDesc> desc) {
    desc_ = (desc && desc->shape) ? std::move(desc) : DefaultDesc();
    spawn_.longitude = desc_->longitude;
    motion_.wind = desc_->motion.wind;
    motion_.drag = desc_->motion.drag;
    if (desc_->emission.squirtAtStart)
        flags_ |= kFlagNeedSquirt;
}

void Emitter2::ApplyState(const model::FrameState::ParticleFrameState& st) {
    emissionRate_ = st.emissionRate;
    // WC3 animates a downward acceleration magnitude; the vector form keeps the
    // other two components exactly zero.
    motion_.gravity = {0.0f, 0.0f, -st.gravity};

    spawn_.speed.base = st.speed;
    spawn_.speed.variance = st.variation;
    spawn_.latitude = st.coneAngle;
    spawn_.width = st.width;
    spawn_.height = st.length;

    SetVisible(st.visibility > 0.0f && !st.squirting);
    modelToWorld_ = CoordinateSystem::ConvertTransform(CoordinateSystem::Default(),
                                                       desc_->coordSpace, st.transform);
}

void Emitter2::CreateParticle(Particle2& p, f32 elapsed) {
    const f32 r = CRandom::real_(randSeed_);
    p.keyFrame = 0;
    p.age = elapsed * r;

    SpawnSample s;
    desc_->shape->Sample(s, spawn_, randSeed_);

    if (desc_->modelSpace) {
        p.position = s.localPos;
        p.velocity = s.localVel;
    } else {
        p.position = whiteout::transform_point(s.localPos, modelToWorld_);
        p.velocity = whiteout::transform_normal(s.localVel, modelToWorld_);
    }
}

void Emitter2::Sync() {
    if (emissionRate_ <= 0.0f || desc_->lifeSpan <= 0.0f)
        return;
    // Headroom over the steady-state population (rate x lifespan) so a rate
    // spike does not immediately starve the free list.
    const u32 capacity = static_cast<u32>(1.15f * emissionRate_ * desc_->lifeSpan);
    const usize before = pool_.Capacity();
    pool_.Sync(capacity);
    if (pool_.Capacity() != before)
        OnPoolResized(pool_.Capacity());
}

void Emitter2::SetSeed(u32 seed) {
    randSeed_.SetSeed(MixSeed(seed));
    // Housekeeping (pool compaction) draws from its own stream so it can never
    // perturb the spawn stream — spawn reproducibility is what the trace diff
    // compares, and it must not depend on how often the pool happened to empty.
    compactSeed_.SetSeed(MixSeed(seed ^ 0x5BF03635u));
}

void Emitter2::InternalUpdate(f32 elapsed, f32 emissionScaler) {

    if (elapsed < 0.0f)
        elapsed = 0.0f;
    if (elapsed > kMaxDt)
        elapsed = kMaxDt;

    const bool squirtPending = (flags_ & kFlagNeedSquirt) != 0;
    const bool enabled = Visible();

    if (enabled || squirtPending) {
        Sync();
    }

    if (squirtPending) {
        i32 numToEmit = static_cast<i32>(emissionRate_ * emissionScaler);
        while (numToEmit > 0 && !pool_.DeadEmpty()) {
            u32 idx = pool_.PopDead();
            pool_.PushAlive(idx);
            CreateParticle(pool_[idx], 0.0f);
            OnParticleBorn(idx);
            --numToEmit;
        }
        flags_ &= ~kFlagNeedSquirt;
    }

    if (enabled) {
        numNew_ += elapsed * emissionRate_ * emissionScaler;
        u32 planned = static_cast<u32>(numNew_);
        u32 emitted = 0;
        while (planned > 0 && !pool_.DeadEmpty()) {
            u32 idx = pool_.PopDead();
            pool_.PushAlive(idx);
            CreateParticle(pool_[idx], elapsed);
            OnParticleBorn(idx);
            ++emitted;
            --planned;
        }
        numNew_ -= static_cast<f32>(emitted);
    }

    const auto& colorCurve = desc_->curves.color;
    const u32 lastSegment = static_cast<u32>(colorCurve.SegmentCount());
    const f32 ooLifeSpan = (desc_->lifeSpan > 0.0f) ? (1.0f / desc_->lifeSpan) : 0.0f;

    for (usize i = 0; i < pool_.AliveCount();) {
        u32 idx = pool_.AliveAt(i);
        Particle2& p = pool_[idx];

        p.age += elapsed;

        // keyFrame is now purely a curve-segment cursor: a hint that saves the
        // geometry builder a search. Death is age against lifespan and nothing
        // else — it used to also trigger on running out of keys, which is what
        // made an emitter with no keys kill every particle on its first update.
        const f32 u = ooLifeSpan * p.age;
        while (p.keyFrame + 1 < lastSegment && u > colorCurve.KeyTime(p.keyFrame + 1)) {
            ++p.keyFrame;
        }

        if (desc_->lifeSpan <= p.age) {
            OnParticleDied(idx);
            pool_.PushDead(idx);
            pool_.RemoveAliveAt(i);

        } else {
            Integrate(p, motion_, elapsed);
            ++i;
        }
    }

    if (pool_.AliveCount() == 0 && CRandom::dice_(32, compactSeed_) == 0) {
        pool_.Compact();
    }

    flags_ &= ~kFlagVisible;
}

void Emitter2::Update(f32 elapsed, f32 emissionScaler) {
    InternalUpdate(elapsed, emissionScaler);
}

} // namespace whiteout::flakes::renderer::particle
