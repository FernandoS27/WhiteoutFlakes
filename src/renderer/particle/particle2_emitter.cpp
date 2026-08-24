#include "renderer/particle/particle2_emitter.h"

#include "whiteout/flakes/model_types.h" // FrameState::ParticleFrameState
#include "whiteout/flakes/util/coordinate_system.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

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
    // adding 0.0f to -0.0f is not the identity. What the guard tests is spelled
    // out on PathOffsetsSpawn.
    if (PathOffsetsSpawn()) {
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

void Emitter2::AddTrail(std::unique_ptr<Emitter2> trail) {
    if (!trail || trails_.size() >= kMaxTrails)
        return;
    // Both bits the adoption is responsible for: the enable the client ORs in
    // (`RecursiveEmitterModelLoaded` @0x1016a6b60), and the marker that keeps it
    // from being cleared again — nothing animates a trail, so nothing would
    // ever set it a second time.
    trail->flags_ |= kFlagTrail | kFlagVisible;
    // A trail never emits from its own position, so a squirt it inherited from
    // its record would fire once, from wherever it happens to sit. The client
    // cannot reach that either: the squirt bit is written by AnimateParticleST,
    // which never runs for a trail.
    trail->flags_ &= ~kFlagNeedSquirt;
    trails_.push_back(std::move(trail));
}

void Emitter2::GrowPool(u32 capacity) {
    const usize before = pool_.Capacity();
    pool_.Sync(capacity);
    if (pool_.Capacity() != before) {
        if (desc_->UsesMultiTexLayers())
            multiTex_.resize(pool_.Capacity());
        OnPoolResized(pool_.Capacity());
    }
}

void Emitter2::SeedMultiTex(u32 poolIndex) {
    if (!desc_->UsesMultiTexLayers() || poolIndex >= multiTex_.size())
        return;
    MultiTexState& m = multiTex_[poolIndex];
    // Three draws per layer, in the client's order: the UV origin's u, its v,
    // then ONE symmetric draw that scales both components of the scroll rate.
    // The single shared draw is the client's, not a simplification — a layer's
    // u and v rates are perfectly correlated because of it.
    for (usize layer = 0; layer < 2; ++layer) {
        m.uv[layer].x = CRandom::real_(randSeed_);
        m.uv[layer].y = CRandom::real_(randSeed_);
        const f32 r = CRandom::reals_(randSeed_);
        m.scroll[layer] = {desc_->multiTexScrollMid[layer].x + r * desc_->multiTexScrollRange[layer].x,
                           desc_->multiTexScrollMid[layer].y + r * desc_->multiTexScrollRange[layer].y};
    }
}

void Emitter2::AdvanceMultiTex(u32 poolIndex, f32 dt) {
    if (!desc_->UsesMultiTexLayers() || poolIndex >= multiTex_.size())
        return;
    MultiTexState& m = multiTex_[poolIndex];
    for (usize layer = 0; layer < 2; ++layer) {
        const f32 u = m.uv[layer].x + m.scroll[layer].x * dt;
        const f32 v = m.uv[layer].y + m.scroll[layer].y * dt;
        // floor, not fmod: the client wraps with `x - floorf(x)`, which keeps a
        // negative scroll rate inside [0,1) instead of walking into -1.
        m.uv[layer] = {u - std::floor(u), v - std::floor(v)};
    }
}

void Emitter2::Sync() {
    if (emissionRate_ <= 0.0f || desc_->lifeSpan <= 0.0f)
        return;
    // The lifespan that actually decides how long a slot stays occupied. Under
    // WoW that is the ANIMATED track plus the record's variance, not the desc's
    // constant: EffectiveLifeSpan reads `lifeSpan_`, so sizing off
    // `desc_->lifeSpan` undercounts every emitter whose track rises above its
    // first key and pins the pool permanently full.
    const f32 life = behavior_.perParticleLifespan
                         ? ((std::max)(lifeSpan_, 0.0f) + std::fabs(desc_->lifespanVariation))
                         : desc_->lifeSpan;
    // Headroom over the steady-state population (rate x lifespan) so a rate
    // spike does not immediately starve the free list.
    const u32 capacity = static_cast<u32>(1.15f * emissionRate_ * life);
    GrowPool(capacity);

    // A trail is driven once per live particle of ours, so it needs room for
    // its own steady-state population times ours — the client's own product,
    // clamped the same way (`Sync` @0x10169f860). Both pools only ever grow,
    // here and in the client (`SyncSize` @0x10169e8c0 "never shrinks"), so the
    // trail's own Sync cannot undo this.
    for (auto& t : trails_) {
        const f32 tLife = (std::max)(t->lifeSpan_, 0.0f) + std::fabs(t->desc_->lifespanVariation);
        const u32 own = static_cast<u32>(1.15f * t->emissionRate_ * tLife);
        const u64 product = static_cast<u64>(capacity) * static_cast<u64>(own);
        t->GrowPool(static_cast<u32>(product > 4096u ? 4096u : product));
    }
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
            SeedMultiTex(idx);
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
        // Drained here, not by however many the pool could actually serve.
        // `EmitNewParticles` @0x1016a5c90 decrements m_numNew once per loop
        // iteration with the decrement OUTSIDE the buffer guard, so its carry
        // always falls below 1 — a step it could not spawn is dropped, not
        // banked. Subtracting only what we emitted lets the carry grow without
        // bound whenever the pool is saturated, and `carried` is what sets the
        // spawn's phase along the emitter path below: at carry 300 the first
        // spawn lands 300 emission-periods of travel behind the emitter, which
        // reads as the whole effect slowly separating from the model.
        if (behavior_.dropUnservedEmission)
            numNew_ -= static_cast<f32>(planned);

        u32 emitted = 0;
        while (planned > 0 && !pool_.DeadEmpty()) {
            if (behavior_.emitAlongPath) {
                if (desc_->randomEmissionSpacing) {
                    // The client draws the number and then throws the position
                    // away. Both branches of `EmitNewParticles` @0x1016a5c90
                    // copy the spawn matrix to the stack and overwrite its
                    // translation with the position they computed, but only the
                    // even-spacing branch hands that copy to `CreateParticle`
                    // (`lea rdx,[rbp+var_68]` @0x1016a64ca). The random branch
                    // passes the UNMODIFIED matrix instead (`mov
                    // rdx,[rbp+var_80]` @0x1016a6492, spilled @0x1016a5ee3), so
                    // the particle is born at the emitter's current position and
                    // the randomised point is dead. The draw is not: it advances
                    // the stream every other spawn is sequenced against.
                    (void)CRandom::real_(randSeed_);
                } else {
                    const f32 k = (1.0f - carried) + static_cast<f32>(emitted);
                    const Vector3f pos{prevWorldPos_.x + step.x * k, prevWorldPos_.y + step.y * k,
                                       prevWorldPos_.z + step.z * k};
                    spawnOffset_ = {pos.x - worldPos_.x, pos.y - worldPos_.y, pos.z - worldPos_.z};
                }
            }
            u32 idx = pool_.PopDead();
            pool_.PushAlive(idx);
            CreateParticle(pool_[idx], elapsed);
            SeedMultiTex(idx);
            OnParticleBorn(idx);
            ++emitted;
            --planned;
        }
        if (!behavior_.dropUnservedEmission)
            numNew_ -= static_cast<f32>(emitted);
    }
}

void Emitter2::DriveTrails(const Particle2& p, f32 dt, f32 emissionScaler) {
    // Where this particle is in world space. A world-space emitter stores that
    // directly; a model-space one stores model units and is transformed at
    // draw, so its trail's spawn point has to go the same way. No shipped
    // record carries a trail on a model-space emitter, so the second case is
    // reasoned, not measured.
    const Vector3f world = desc_->modelSpace
                               ? whiteout::transform_point(p.position, modelToWorld_)
                               : p.position;

    for (auto& t : trails_) {
        // The client stamps the particle's position into the translation row of
        // its OWN particle-to-world matrix, hands that matrix to the trail as
        // the spawn transform, and puts it back afterwards
        // (`UpdateLiveParticle` @0x1016aa080, disassembly at 0x1016aa113 and
        // 0x1016aa176). Restoring matters for the same reason it does there: a
        // trail's DRAW transform must stay the parent model's, not the last
        // particle's.
        const Matrix44f saved = t->modelToWorld_;
        t->modelToWorld_ = modelToWorld_;
        t->modelToWorld_.data[3][0] = world.x;
        t->modelToWorld_.data[3][1] = world.y;
        t->modelToWorld_.data[3][2] = world.z;

        // Seeded rather than left where it was. The client never writes a
        // trail's previous position unless the trail asked for random spacing —
        // no shipped one does — so its spawn segment runs from the world origin
        // to the particle and smears across the map. That is uninitialised
        // state, not a behaviour, and seeding prev := cur collapses the segment
        // to the point the feature is for. See M2_TRAIL_EMITTER_DESIGN.md D1.
        t->worldPos_ = world;
        t->prevWorldPos_ = world;
        t->worldPosSeeded_ = true;

        // The flag consulted is the TRAIL's, not ours.
        if (t->desc_->inheritVelocity) {
            const Vector3f v = desc_->modelSpace
                                   ? whiteout::transform_normal(p.velocity, modelToWorld_)
                                   : p.velocity;
            const f32 k = t->MotionToParticleSpace();
            t->emitterVelocity_ = {v.x * k, v.y * k, v.z * k};
        }

        t->EmitStep(dt, emissionScaler);
        t->modelToWorld_ = saved;
    }
}

void Emitter2::AdvanceStep(f32 elapsed, f32 emissionScaler) {
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
        // The extra texture layers scroll BEFORE the move, and before the move
        // can kill the particle — `UpdateLiveParticle<CMultiTexParticle>`
        // @0x1016a9d30 advances them at the top and only then calls
        // MoveParticle. A particle that dies this step still scrolled.
        AdvanceMultiTex(idx, elapsed);
        const bool implode = behavior_.implosionKill && desc_->implosionFilter;
        if (!MoveParticleWow(p, forces, elapsed, implode, center)) {
            OnParticleDied(idx);
            pool_.PushDead(idx);
            pool_.RemoveAliveAt(i);
            continue;
        }
        // Only a particle that survived its move drives anything: the client
        // drives its children from the true half of `MoveParticle`'s result and
        // nowhere else. The WC3 branch above has no equivalent because no MDX
        // record can name a trail model.
        if (!trails_.empty())
            DriveTrails(p, elapsed, emissionScaler);
        ++i;
    }
}

void Emitter2::StepOnce(f32 dt, f32 emissionScaler) {
    // A trail's own emission is suppressed for the whole of its life, which is
    // the same thing the client's `InternalUpdate(dt, 1)` recursion says: that
    // second argument is `suppressEmit`, and being driven as a child is the
    // only way a trail is ever updated.
    if ((flags_ & kFlagTrail) == 0)
        EmitStep(dt, emissionScaler);
    AdvanceStep(dt, emissionScaler);

    // After the parent's own particles have moved and driven them, exactly
    // where `StepUpdate` @0x1016a95c0 recurses.
    for (auto& t : trails_)
        t->InternalUpdate(dt, emissionScaler);
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

    // Visibility is re-asserted every frame from the model's tracks — except
    // for a trail, which has no tracks and whose enable bit the client sets
    // once and never clears.
    if ((flags_ & kFlagTrail) == 0)
        flags_ &= ~kFlagVisible;
}

void Emitter2::Update(f32 elapsed, f32 emissionScaler) {
    // What the client's `Update` @0x1016a55a0 does before stepping: run
    // UpdateXform over its children with the parent's world matrix. Nothing
    // else reaches a trail, so the rest of the per-frame state a trail reads
    // has to arrive the same way — it belongs to the owning actor, not to the
    // model the trail's record came from.
    for (auto& t : trails_) {
        model::FrameState::ParticleFrameState st = t->trailState_;
        st.unitScale = unitScale_;
        st.modelAlpha = modelAlpha_;
        st.transform = modelToWorld_;
        st.worldPosition = worldPos_;
        t->ApplyState(st);
        // ApplyState is the animation's entry point and clears the enable bit
        // when the track says so; a trail has no track and stays on.
        t->flags_ |= kFlagVisible;
        t->viewDistance_ = viewDistance_;
    }
    InternalUpdate(elapsed, emissionScaler);
}

} // namespace whiteout::flakes::renderer::particle
