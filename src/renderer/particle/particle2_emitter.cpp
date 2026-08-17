#include "renderer/particle/particle2_emitter.h"

#include "whiteout/flakes/model_types.h" // FrameState::ParticleFrameState
#include "whiteout/flakes/util/coordinate_system.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle {

namespace {

// Below this, one step's worth of emission is too small to divide the emitter's
// travel by — the client's own guard against a 1/x blowup.
constexpr f32 kEmissionEpsilon = 2.3841858e-7f;

// WoW resamples the emitter's velocity on a 1/30 s cadence rather than every
// frame, so the value a particle inherits is frame-rate coupled by design.
constexpr f32 kVelocitySampleSeconds = 1.0f / 30.0f;

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
    lifeSpan_ = desc_->lifeSpan;
    if (desc_->emission.squirtAtStart)
        flags_ |= kFlagNeedSquirt;
}

void Emitter2::SetWorldPosition(const Vector3f& p) {
    // The first frame has no previous position, so seeding both suppresses one
    // spurious spawn-spread across whatever distance the emitter was placed at.
    if (!worldPosSeeded_) {
        prevWorldPos_ = p;
        worldPosSeeded_ = true;
    } else {
        prevWorldPos_ = worldPos_;
    }
    worldPos_ = p;
}

f32 Emitter2::EffectiveLifeSpan(const Particle2& p) const {
    if (!behavior_.perParticleLifespan)
        return desc_->lifeSpan;
    // Re-read against the CURRENT animated lifespan, not the one in force when
    // the particle was born — the client does this every frame, so a lifespan
    // keyframe resizes particles already in flight.
    const f32 v = static_cast<f32>(p.LifespanVarQ()) * (1.0f / 32768.0f);
    return (std::max)(v * desc_->lifespanVariation + lifeSpan_, 0.001f);
}

void Emitter2::ApplyState(const model::FrameState::ParticleFrameState& st) {
    emissionRate_ = st.emissionRate;
    unitScale_ = (st.unitScale > 0.0f) ? st.unitScale : 1.0f;
    modelAlpha_ = st.modelAlpha;

    // A world-space emitter stamps its particles into renderer units at birth,
    // so the forces pushing them have to be in renderer units too — and the M2
    // tracks are authored in model units. A model-space emitter integrates in
    // model units and is scaled at draw, so converting there would double it.
    const f32 forceScale =
        (behavior_.forceModel == core::ParticleForceModel::WowForces && !desc_->modelSpace)
            ? unitScale_
            : 1.0f;

    if (st.hasGravityVector) {
        // M2 animates a gravity direction, not just a magnitude.
        motion_.gravity = {st.gravityVector.x * forceScale, st.gravityVector.y * forceScale,
                           st.gravityVector.z * forceScale};
        motion_.wind = {desc_->motion.wind.x * forceScale, desc_->motion.wind.y * forceScale,
                        desc_->motion.wind.z * forceScale};
    } else {
        // WC3 animates a downward acceleration magnitude; the vector form keeps
        // the other two components exactly zero.
        motion_.gravity = {0.0f, 0.0f, -st.gravity};
    }

    spawn_.speed.base = st.speed;
    spawn_.speed.variance = st.variation;
    spawn_.latitude = st.coneAngle;
    spawn_.width = st.width;
    spawn_.height = st.length;
    // Ignored by the WC3 shapes, so unconditional is safe and keeps the branch
    // count down.
    spawn_.horizontalRange = st.horizontalRange;
    spawn_.zSource = st.zSource;

    if (behavior_.perParticleLifespan)
        lifeSpan_ = st.lifeSpan;
    if (behavior_.emitAlongPath || behavior_.inheritEmitterVelocity)
        SetWorldPosition(st.worldPosition);

    SetVisible(st.visibility > 0.0f && !st.squirting);
    modelToWorld_ = CoordinateSystem::ConvertTransform(CoordinateSystem::Default(),
                                                       desc_->coordSpace, st.transform);
}

void Emitter2::CreateParticle(Particle2& p, f32 elapsed) {
    if (behavior_.birthDrawsVarianceAndSeed) {
        // Three draws where WC3 takes one, in the client's order: a sub-frame
        // age numerator, the quantised lifespan variance, then the particle's
        // own render seed.
        const f32 ageNum = CRandom::reals_(randSeed_) * elapsed;
        const f32 r = CRandom::reals_(randSeed_);
        const i16 varQ = (r < 1.0f) ? (r > -1.0f ? static_cast<i16>(r * 32767.0f + 0.5f)
                                                 : static_cast<i16>(-32767))
                                    : static_cast<i16>(32767);
        p.SetVarianceAndSeed(varQ, static_cast<u16>(CRandom::next_u32(randSeed_)));
        // fmod, not clamp — a negative draw gives a NEGATIVE age, and the
        // client lets that stand. Faithful; do not std::max it to zero.
        p.age = std::fmod(ageNum, EffectiveLifeSpan(p));
    } else {
        const f32 r = CRandom::real_(randSeed_);
        p.SetCursor(0);
        p.age = elapsed * r;
    }

    SpawnSample s;
    // Re-pointed here rather than once in the setter: `spawn_` holds a view into
    // a member vector, and an emitter that gets moved would otherwise carry a
    // span into its old storage.
    spawn_.boneTable = boneSpawns_;
    desc_->shape->Sample(s, spawn_, randSeed_);

    if (desc_->modelSpace) {
        p.position = s.localPos;
        p.velocity = s.localVel;
    } else {
        p.position = whiteout::transform_point(s.localPos, modelToWorld_);
        p.velocity = whiteout::transform_normal(s.localVel, modelToWorld_);
    }

    // Guarded rather than "add zero": the WC3 trace compares exact bits, and
    // adding 0.0f to -0.0f is not the identity.
    //
    // A model-space emitter is excluded because the client never offsets the
    // particle at all: `EmitNewParticles` @0x1016a5c90 rewrites the TRANSLATION
    // of the matrix it hands `CreateParticle`, and that matrix is only read on
    // the branch that bakes the spawn into world space. So the path exists for
    // a world-space emitter and does not exist for a local one — the two are
    // algebraically identical for the first (R*local + pathPos either way) and
    // nothing at all for the second.
    if (behavior_.emitAlongPath && !desc_->modelSpace) {
        p.position.x += spawnOffset_.x;
        p.position.y += spawnOffset_.y;
        p.position.z += spawnOffset_.z;
    }
    if (behavior_.inheritEmitterVelocity) {
        const f32 scale = CRandom::reals_(randSeed_) * desc_->inheritVelocityScale + 1.0f;
        p.velocity.x += emitterVelocity_.x * scale;
        p.velocity.y += emitterVelocity_.y * scale;
        p.velocity.z += emitterVelocity_.z * scale;
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

    // RandFlipbookStart is resolved once, off the emitter's own stream, the way
    // the loader does it (`SetRandFlipBookStart` @0x1016a6ad0 calls dice_ with
    // the emitter seed). It therefore consumes a draw before any particle is
    // born — which is why registration sets the desc before the seed, and why
    // no WC3 emitter can reach it.
    baseCell_ = 0;
    if (desc_->randFlipbookStart) {
        const u32 cells = desc_->sheet.rows * desc_->sheet.cols;
        baseCell_ = static_cast<u16>(CRandom::dice_(cells, randSeed_));
    }
}

void Emitter2::EmitStep(f32 elapsed, f32 emissionScaler) {
    const bool squirtPending = (flags_ & kFlagNeedSquirt) != 0;
    const bool enabled = Visible();

    // WC3 emits at exactly its animated rate. WoW re-jitters every frame and
    // fades the rate with distance, both of which have to happen before the
    // burst count is taken — and the jitter draw itself is observable, which is
    // why it is gated on the dialect rather than on a zero variation.
    f32 rate = emissionRate_;
    if (behavior_.rateJitterPerFrame)
        rate += CRandom::reals_(randSeed_) * desc_->emissionRateVariation;
    if (behavior_.lodEmissionScale && !desc_->lodIgnoreDistance) {
        const f32 raw = (50.0f - viewDistance_) * 0.02f + 1.0f;
        rate *= (raw < 0.25f) ? 0.25f : ((raw > 1.0f) ? 1.0f : raw);
    }

    if (squirtPending) {
        i32 numToEmit = static_cast<i32>(rate * emissionScaler);
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
        const f32 carried = numNew_;
        numNew_ += elapsed * rate * emissionScaler;

        // Spawn positions walk the segment the emitter travelled this step.
        // `step` is one emission period's worth of that travel and `carried` is
        // last step's leftover fraction, so the phase is continuous across
        // steps rather than restarting at the emitter each time.
        Vector3f travel{0, 0, 0};
        Vector3f step{0, 0, 0};
        if (behavior_.emitAlongPath) {
            travel = {worldPos_.x - prevWorldPos_.x, worldPos_.y - prevWorldPos_.y,
                      worldPos_.z - prevWorldPos_.z};
            const f32 emitThisStep = elapsed * rate * emissionScaler;
            if (emitThisStep > kEmissionEpsilon) {
                const f32 inv = 1.0f / emitThisStep;
                step = {travel.x * inv, travel.y * inv, travel.z * inv};
            }
        }

        u32 planned = static_cast<u32>(numNew_);
        u32 emitted = 0;
        while (planned > 0 && !pool_.DeadEmpty()) {
            if (behavior_.emitAlongPath) {
                Vector3f pos;
                if (desc_->randomEmissionSpacing) {
                    const f32 u = CRandom::real_(randSeed_);
                    pos = {prevWorldPos_.x + travel.x * u, prevWorldPos_.y + travel.y * u,
                           prevWorldPos_.z + travel.z * u};
                } else {
                    const f32 k = (1.0f - carried) + static_cast<f32>(emitted);
                    pos = {prevWorldPos_.x + step.x * k, prevWorldPos_.y + step.y * k,
                           prevWorldPos_.z + step.z * k};
                }
                spawnOffset_ = {pos.x - worldPos_.x, pos.y - worldPos_.y, pos.z - worldPos_.z};
            }
            u32 idx = pool_.PopDead();
            pool_.PushAlive(idx);
            CreateParticle(pool_[idx], elapsed);
            OnParticleBorn(idx);
            ++emitted;
            --planned;
        }
        numNew_ -= static_cast<f32>(emitted);
    }
}

void Emitter2::AdvanceStep(f32 elapsed) {
    const auto& colorCurve = desc_->curves.color;
    const u32 lastSegment = static_cast<u32>(colorCurve.SegmentCount());
    const f32 ooLifeSpan = (desc_->lifeSpan > 0.0f) ? (1.0f / desc_->lifeSpan) : 0.0f;

    // WoW's forces are computed once for the whole step, not per particle.
    const bool wowForces = behavior_.forceModel == core::ParticleForceModel::WowForces;
    const ParticleForces forces =
        wowForces ? CalculateForcesWow(motion_, elapsed) : ParticleForces{};
    const Vector3f center = desc_->modelSpace
                                ? Vector3f{0, 0, 0}
                                : Vector3f{worldPos_.x, worldPos_.y, worldPos_.z};

    for (usize i = 0; i < pool_.AliveCount();) {
        u32 idx = pool_.AliveAt(i);
        Particle2& p = pool_[idx];

        p.age += elapsed;

        // Under WC3 the aux word is a curve-segment cursor: a hint that saves
        // the geometry builder a search. Death is age against lifespan and
        // nothing else — it used to also trigger on running out of keys, which
        // is what made an emitter with no keys kill every particle on its first
        // update.
        //
        // Under WoW those same four bytes hold the lifespan variance and the
        // render seed, so advancing a "cursor" through them would corrupt both.
        if (!behavior_.birthDrawsVarianceAndSeed) {
            const f32 u = ooLifeSpan * p.age;
            while (p.Cursor() + 1 < lastSegment && u > colorCurve.KeyTime(p.Cursor() + 1)) {
                p.SetCursor(p.Cursor() + 1);
            }
        }

        if (EffectiveLifeSpan(p) <= p.age) {
            OnParticleDied(idx);
            pool_.PushDead(idx);
            pool_.RemoveAliveAt(i);
            continue;
        }

        if (!wowForces) {
            IntegrateWc3(p, motion_, elapsed);
            ++i;
            continue;
        }

        if (behavior_.followPosition && desc_->followPosition && (elapsed + elapsed) < p.age) {
            p.position.x += followDelta_.x;
            p.position.y += followDelta_.y;
            p.position.z += followDelta_.z;
        }
        const bool implode = behavior_.implosionKill && desc_->implosionFilter;
        if (!MoveParticleWow(p, forces, elapsed, implode, center)) {
            OnParticleDied(idx);
            pool_.PushDead(idx);
            pool_.RemoveAliveAt(i);
            continue;
        }
        ++i;
    }
}

void Emitter2::StepOnce(f32 dt, f32 emissionScaler) {
    EmitStep(dt, emissionScaler);
    AdvanceStep(dt);
}

void Emitter2::TickEmitterVelocity(f32 dt) {
    velocityTimer_ += dt;
    if (velocityTimer_ <= kVelocitySampleSeconds)
        return;
    const f32 accumulated = velocityTimer_;
    velocityTimer_ = 0.0f;
    // Only an EMPTY pool refreshes the velocity; with particles alive the tick
    // zeroes it instead. So a burst inherits the emitter's motion at onset and
    // a steady stream stops inheriting after the first tick — verified at
    // instruction level, and the opposite of what the RE notes said.
    if (pool_.AliveCount() != 0) {
        emitterVelocity_ = {0, 0, 0};
        return;
    }
    const f32 scale = (kVelocitySampleSeconds / accumulated) * desc_->inheritVelocityScale *
                      MotionToParticleSpace();
    emitterVelocity_ = {(worldPos_.x - prevWorldPos_.x) * scale,
                        (worldPos_.y - prevWorldPos_.y) * scale,
                        (worldPos_.z - prevWorldPos_.z) * scale};
}

void Emitter2::InternalUpdate(f32 elapsed, f32 emissionScaler) {

    if (elapsed < 0.0f)
        elapsed = 0.0f;
    if (behavior_.dtPolicy == core::ParticleDtPolicy::ClampToMax && elapsed > behavior_.maxStepSeconds)
        elapsed = behavior_.maxStepSeconds;

    if (Visible() || (flags_ & kFlagNeedSquirt) != 0) {
        Sync();
    }

    if (behavior_.dtPolicy == core::ParticleDtPolicy::FixedSubSteps) {
        if (behavior_.inheritEmitterVelocity)
            TickEmitterVelocity(elapsed);

        // How much of the emitter's travel a trailing particle inherits: a
        // clamped line in the emitter's own speed, solved once per FRAME (the
        // speed is measured against the frame's dt, not a sub-step's) and only
        // then divided across the sub-steps.
        const Vector3f travel{worldPos_.x - prevWorldPos_.x, worldPos_.y - prevWorldPos_.y,
                              worldPos_.z - prevWorldPos_.z};
        f32 follow = 0.0f;
        // The dt guard is the client's own: it bails out of Update before this
        // point, where we would divide by zero.
        if (behavior_.followPosition && desc_->followPosition && elapsed > kEmissionEpsilon) {
            const f32 distSq = travel.x * travel.x + travel.y * travel.y + travel.z * travel.z;
            // The record's two sample speeds are in model units per second, so
            // the measured speed has to be too — `travel` is renderer units
            // whatever space the particles live in.
            const f32 den = elapsed * ((unitScale_ > 0.0f) ? unitScale_ : 1.0f);
            const f32 speed = (distSq > 0.0f) ? (std::sqrt(distSq) / den) : 0.0f;
            const f32 raw = speed * desc_->followSlope + desc_->followBias;
            follow = (raw >= 0.0f) ? ((raw < 1.0f) ? raw : 1.0f) : 0.0f;
        }
        const f32 followScale = follow * MotionToParticleSpace();
        const Vector3f followed{travel.x * followScale, travel.y * followScale,
                                travel.z * followScale};

        const f32 stepSize = behavior_.subStepSeconds;
        if (elapsed <= stepSize) {
            followDelta_ = followed;
            StepOnce(elapsed, emissionScaler);
        } else {
            // Fixed steps plus a remainder — NOT dt/n. When the lifespan cap
            // binds, the fixed steps deliberately cover less than `elapsed` and
            // the shortfall is dropped: nothing alive would have outlived it.
            const f32 whole = std::floor(elapsed / stepSize);
            const f32 lifeSteps = std::floor(lifeSpan_ / stepSize);
            const i32 n = static_cast<i32>((lifeSteps < whole) ? lifeSteps : whole);
            const f32 remainder = elapsed - stepSize * whole;
            const f32 invSub = 1.0f / static_cast<f32>(n + 1);
            followDelta_ = {followed.x * invSub, followed.y * invSub, followed.z * invSub};
            for (i32 s = 0; s < n; ++s)
                StepOnce(stepSize, emissionScaler);
            StepOnce(remainder, emissionScaler);
        }
    } else {
        StepOnce(elapsed, emissionScaler);
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
