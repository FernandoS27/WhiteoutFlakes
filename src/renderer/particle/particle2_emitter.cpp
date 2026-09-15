#include "renderer/particle/particle2_emitter.h"

#include "renderer/particle/particle_constants.h"
#include "renderer/particle/particle_geometry.h"
#include "renderer/particle/sc2_runtime.h"

#include "whiteout/flakes/model_types.h" // FrameState::ParticleFrameState
#include "whiteout/flakes/util/coordinate_system.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle {

namespace {

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

Emitter2::~Emitter2() {
    if (sc2_ && sc2PendingList_) {
        std::erase_if(*sc2PendingList_, [rt = sc2_.get()](const Sc2PendingModel& entry) {
            return entry.runtime == rt;
        });
    }
}

void Emitter2::SetDesc(std::shared_ptr<const EmitterDesc> desc) {
    // A shapeless WC3/M2 desc is replaced, because `CreateParticle`
    // dereferences `desc_->shape` unconditionally and a null there is a crash.
    // An SC2 desc legitimately has none: its emission shape is `sc2.emit.shape`,
    // a NUMBER the SC2 spawn kernels read, and `InternalUpdate` returns before
    // `EmitStep` for an SC2 emitter so that dereference is unreachable.
    //
    // Without the family term this line silently swapped every SC2 desc for the
    // default — which also reset `family` to Wc3, so the runtime below was never
    // created either. Nothing crashed and every gate stayed green, because the
    // emitters had become inert WC3 emitters that emit nothing.
    const bool usable = desc && (desc->shape || desc->family == EmitterDesc::Family::Sc2);
    desc_ = usable ? std::move(desc) : DefaultDesc();

    // The one place `family` is read. Everything else tests `sc2_` — so a
    // re-describe from one family to the other cannot leave a stale runtime
    // behind, and no other touch point has to know the enum exists.
    if (desc_->family == EmitterDesc::Family::Sc2)
        DescribeSc2();
    else
        sc2_.reset();

    spawn_.longitude = desc_->longitude;
    motion_.wind = desc_->motion.wind;
    motion_.drag = desc_->motion.drag;
    wow_.lifeSpan = desc_->lifeSpan;
    if (desc_->emission.squirtAtStart)
        run_.squirtOwed = true;
}

void Emitter2::ResetParticles() {
    // Through the death hook, not straight into the pool: an output can own
    // something per particle — a PE1 child actor — and dropping the pool blind
    // would leave it standing in the scene with nothing driving it.
    for (usize i = 0; i < pool_.AliveCount(); ++i)
        OnParticleDied(pool_.AliveAt(i));
    pool_.Clear();

    // The one-shot burst is owed again, exactly as SetDesc armed it at
    // registration: this emitter has not fired in the run that starts now.
    run_ = Wc3RunState{.numNew = 0.0f, .squirtOwed = desc_->emission.squirtAtStart};
    wow_.motion = WowMotion{};
    // Unseeded, so the next SetWorldPosition re-seeds prev and curr together
    // and no spawn is spread across a path the emitter never travelled.
    placement_.Unseed();

    // After the shared death walk, not instead of it: model particles are
    // child actors, and they are released by the OnParticleDied above.
    if (sc2_)
        RewindSc2();

    trails_.Reset();
}

EmitterDrawHeader Emitter2::DrawHeader() const {
    EmitterDrawHeader h;
    h.output = desc_->output;
    h.priorityPlane = desc_->priorityPlane;
    h.material = &desc_->material;
    h.refraction = desc_->refraction;
    h.multiTexture = desc_->multiTexture;
    return h;
}

i32 Emitter2::TotalAlive() const {
    // An SC2 emitter keeps its elements in `Sc2ParticleStore`, never in
    // `pool_` — reading the pool alone reported every SC2 emitter as empty in
    // the viewer's frame stats, which is indistinguishable from an emitter that
    // is not running.
    if (sc2_)
        return static_cast<i32>(sc2_->store.AliveCount());
    return static_cast<i32>(pool_.AliveCount());
}

f32 Emitter2::EffectiveLifeSpan(const Particle2& p) const {
    if (!behavior_.perParticleLifespan)
        return desc_->lifeSpan;
    // Re-read against the CURRENT animated lifespan, not the one in force when
    // the particle was born — the client does this every frame, so a lifespan
    // keyframe resizes particles already in flight.
    const f32 v = p.LifespanVariance();
    return (std::max)(v * desc_->lifespanVariation + wow_.lifeSpan, kMinLifespan);
}

void Emitter2::ApplyState(const model::FrameState::ParticleFrameState& st) {
    ApplyAnimated(st);
    if (sc2_)
        ApplySc2Frame(st.sc2);
}

template <class State>
void Emitter2::ApplyAnimated(const State& st) {
    emissionRate_ = st.emissionRate;
    placement_.unitScale = (st.unitScale > 0.0f) ? st.unitScale : 1.0f;
    wow_.modelAlpha = st.modelAlpha;

    // A world-space emitter stamps its particles into renderer units at birth,
    // so the forces pushing them have to be in renderer units too — and the M2
    // tracks are authored in model units. A model-space emitter integrates in
    // model units and is scaled at draw, so converting there would double it.
    const f32 forceScale =
        (behavior_.forceModel == core::ParticleForceModel::WowForces && !desc_->modelSpace)
            ? placement_.unitScale
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
        wow_.lifeSpan = st.lifeSpan;
    // SC2 always needs it: the spawn sweep distributes a frame's particles
    // along `prevPos → curPos` unconditionally, so the two WC3-family reasons
    // to track the emitter's travel are not the only ones. P1 also copies the
    // sampled `PAR_` tracks into the runtime's frame block here.
    if (sc2_ || behavior_.emitAlongPath || behavior_.inheritEmitterVelocity)
        SetWorldPosition(st.worldPosition);

    SetVisible(st.visibility > 0.0f && !st.squirting);
    placement_.modelToWorld = CoordinateSystem::ConvertTransform(CoordinateSystem::Default(),
                                                                 desc_->coordSpace, st.transform);
}

// The two states the animated half is applied from: the model's own tracks,
// and a trail's record patched with its owner's placement.
template void Emitter2::ApplyAnimated(const model::FrameState::ParticleFrameState&);
template void Emitter2::ApplyAnimated(const TrailState&);

void Emitter2::CreateParticle(Particle2& p, f32 elapsed) {
    if (behavior_.birthDrawsVarianceAndSeed) {
        // Three draws where WC3 takes one, in the client's order: a sub-frame
        // age numerator, the quantised lifespan variance, then the particle's
        // own render seed.
        const f32 ageNum = CRandom::reals_(randSeed_) * elapsed;
        const f32 r = CRandom::reals_(randSeed_);
        const i16 varQ = QuantizeLifespanVariance(r);
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
    spawn_.boneTable = wow_.boneSpawns;
    desc_->shape->Sample(s, spawn_, randSeed_);

    if (desc_->modelSpace) {
        p.position = s.localPos;
        p.velocity = s.localVel;
    } else {
        p.position = whiteout::transform_point(s.localPos, placement_.modelToWorld);
        p.velocity = whiteout::transform_normal(s.localVel, placement_.modelToWorld);
    }

    // Guarded rather than "add zero": the WC3 trace compares exact bits, and
    // adding 0.0f to -0.0f is not the identity. What the guard tests is spelled
    // out on PathOffsetsSpawn.
    if (PathOffsetsSpawn()) {
        p.position.x += wow_.motion.spawnOffset.x;
        p.position.y += wow_.motion.spawnOffset.y;
        p.position.z += wow_.motion.spawnOffset.z;
    }
    if (behavior_.inheritEmitterVelocity) {
        const f32 scale = CRandom::reals_(randSeed_) * desc_->inheritVelocityScale + 1.0f;
        p.velocity.x += wow_.motion.emitterVelocity.x * scale;
        p.velocity.y += wow_.motion.emitterVelocity.y * scale;
        p.velocity.z += wow_.motion.emitterVelocity.z * scale;
    }
}

void Emitter2::BirthOne(f32 elapsed) {
    const u32 idx = pool_.PopDead();
    pool_.PushAlive(idx);
    CreateParticle(pool_[idx], elapsed);
    SeedMultiTex(idx);
    OnParticleBorn(idx);
}

void Emitter2::KillAt(usize alivePos, u32 idx) {
    OnParticleDied(idx);
    pool_.PushDead(idx);
    pool_.RemoveAliveAt(alivePos);
}

void Emitter2::AddTrail(std::unique_ptr<Emitter2> trail) {
    trails_.Adopt(std::move(trail));
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
    // Never reached by an SC2 emitter: InternalUpdate hands it to its own tick
    // first, and its store is sized from the authored `maxParticles` cap at
    // describe (`Sc2Runtime::Arm`) — not from `1.15 * rate * lifespan`, which
    // would shrink an emitter whose rate is momentarily zero to nothing.
    if (emissionRate_ <= 0.0f || desc_->lifeSpan <= 0.0f)
        return;
    // The lifespan that actually decides how long a slot stays occupied. Under
    // WoW that is the ANIMATED track plus the record's variance, not the desc's
    // constant: EffectiveLifeSpan reads `wow_.lifeSpan`, so sizing off
    // `desc_->lifeSpan` undercounts every emitter whose track rises above its
    // first key and pins the pool permanently full.
    const f32 life = behavior_.perParticleLifespan
                         ? ((std::max)(wow_.lifeSpan, 0.0f) +
                            std::fabs(desc_->lifespanVariation))
                         : desc_->lifeSpan;
    // Headroom over the steady-state population (rate x lifespan) so a rate
    // spike does not immediately starve the free list.
    const u32 capacity = static_cast<u32>(kPoolHeadroom * emissionRate_ * life);
    GrowPool(capacity);
    trails_.GrowPools(capacity);
}

void Emitter2::SetSeed(u32 seed) {
    randSeed_.SetSeed(MixSeed(seed));
    // Housekeeping (pool compaction) draws from its own stream so it can never
    // perturb the spawn stream — spawn reproducibility is what the trace diff
    // compares, and it must not depend on how often the pool happened to empty.
    compactSeed_.SetSeed(MixSeed(seed ^ kCompactSeedSalt));

    // An SC2 emitter draws from its runtime's `sc2::Rng`, which this seeds too,
    // so two actors of one model do not emit one cloud. Retail shares a global
    // generator whose position differs per emitter; a stream per emitter
    // seeded from the actor gives the same difference reproducibly.
    if (sc2_) {
        const u32 s = MixSeed(seed ^ kSc2SeedSalt);
        sc2_->rng.SetState(s, MixSeed(s));
    }

    // RandFlipbookStart is resolved once, off the emitter's own stream, the way
    // the loader does it (`SetRandFlipBookStart` @0x1016a6ad0 calls dice_ with
    // the emitter seed). It therefore consumes a draw before any particle is
    // born — which is why registration sets the desc before the seed, and why
    // no WC3 emitter can reach it.
    wow_.baseCell = 0;
    if (desc_->randFlipbookStart) {
        const u32 cells = desc_->sheet.rows * desc_->sheet.cols;
        wow_.baseCell = static_cast<u16>(CRandom::dice_(cells, randSeed_));
    }
}

void Emitter2::EmitStep(f32 elapsed, f32 emissionScaler) {
    const bool squirtPending = run_.squirtOwed;
    const bool enabled = Visible();

    // WC3 emits at exactly its animated rate. WoW re-jitters every frame and
    // fades the rate with distance, both of which have to happen before the
    // burst count is taken — and the jitter draw itself is observable, which is
    // why it is gated on the dialect rather than on a zero variation.
    f32 rate = emissionRate_;
    if (behavior_.rateJitterPerFrame)
        rate += CRandom::reals_(randSeed_) * desc_->emissionRateVariation;
    if (behavior_.lodEmissionScale && !desc_->lodIgnoreDistance) {
        const f32 raw = (kLodNearDistance - wow_.viewDistance) * kLodFalloffPerUnit + 1.0f;
        rate *= (raw < kLodMinScale) ? kLodMinScale : ((raw > 1.0f) ? 1.0f : raw);
    }

    if (squirtPending) {
        i32 numToEmit = static_cast<i32>(rate * emissionScaler);
        while (numToEmit > 0 && !pool_.DeadEmpty()) {
            BirthOne(0.0f);
            --numToEmit;
        }
        run_.squirtOwed = false;
    }

    if (enabled) {
        const f32 carried = run_.numNew;
        run_.numNew += elapsed * rate * emissionScaler;

        // Spawn positions walk the segment the emitter travelled this step.
        // `step` is one emission period's worth of that travel and `carried` is
        // last step's leftover fraction, so the phase is continuous across
        // steps rather than restarting at the emitter each time.
        Vector3f travel{0, 0, 0};
        Vector3f step{0, 0, 0};
        if (behavior_.emitAlongPath) {
            travel = {placement_.worldPos.x - placement_.prevWorldPos.x, placement_.worldPos.y - placement_.prevWorldPos.y,
                      placement_.worldPos.z - placement_.prevWorldPos.z};
            const f32 emitThisStep = elapsed * rate * emissionScaler;
            if (emitThisStep > kEmissionEpsilon) {
                const f32 inv = 1.0f / emitThisStep;
                step = {travel.x * inv, travel.y * inv, travel.z * inv};
            }
        }

        u32 planned = static_cast<u32>(run_.numNew);
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
            run_.numNew -= static_cast<f32>(planned);

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
                    const Vector3f pos{placement_.prevWorldPos.x + step.x * k, placement_.prevWorldPos.y + step.y * k,
                                       placement_.prevWorldPos.z + step.z * k};
                    wow_.motion.spawnOffset = {pos.x - placement_.worldPos.x, pos.y - placement_.worldPos.y, pos.z - placement_.worldPos.z};
                }
            }
            BirthOne(elapsed);
            ++emitted;
            --planned;
        }
        if (!behavior_.dropUnservedEmission)
            run_.numNew -= static_cast<f32>(emitted);
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
                                : Vector3f{placement_.worldPos.x, placement_.worldPos.y,
                                           placement_.worldPos.z};

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
            KillAt(i, idx);
            continue;
        }

        // Each feature below answers to its own flag, not to the force model:
        // an `.m2` run under a behaviour that integrates the WC3 way still
        // scrolls its texture layers and drives its trails. Under the two
        // shipped presets the order is the client's either way.
        if (behavior_.followPosition && desc_->followPosition && (elapsed + elapsed) < p.age) {
            p.position.x += wow_.motion.followDelta.x;
            p.position.y += wow_.motion.followDelta.y;
            p.position.z += wow_.motion.followDelta.z;
        }
        // The extra texture layers scroll BEFORE the move, and before the move
        // can kill the particle — `UpdateLiveParticle<CMultiTexParticle>`
        // @0x1016a9d30 advances them at the top and only then calls
        // MoveParticle. A particle that dies this step still scrolled.
        AdvanceMultiTex(idx, elapsed);
        if (!wowForces) {
            IntegrateWc3(p, motion_, elapsed);
        } else {
            const bool implode = behavior_.implosionKill && desc_->implosionFilter;
            if (!MoveParticleWow(p, forces, elapsed, implode, center)) {
                KillAt(i, idx);
                continue;
            }
        }
        // Only a particle that survived its move drives anything: the client
        // drives its children from the true half of `MoveParticle`'s result and
        // nowhere else. No MDX record can name a trail model, so a WC3 emitter
        // has none to drive.
        if (!trails_.Empty())
            trails_.Drive(*this, p, elapsed, emissionScaler);
        ++i;
    }
}

void Emitter2::StepOnce(f32 dt, f32 emissionScaler) {
    // A trail's own emission is suppressed for the whole of its life, which is
    // the same thing the client's `InternalUpdate(dt, 1)` recursion says: that
    // second argument is `suppressEmit`, and being driven as a child is the
    // only way a trail is ever updated.
    if (!isTrail_)
        EmitStep(dt, emissionScaler);
    AdvanceStep(dt, emissionScaler);

    trails_.Step(dt, emissionScaler);
}

void Emitter2::TickEmitterVelocity(f32 dt) {
    wow_.motion.velocityTimer += dt;
    if (wow_.motion.velocityTimer <= kVelocitySampleSeconds)
        return;
    const f32 accumulated = wow_.motion.velocityTimer;
    wow_.motion.velocityTimer = 0.0f;
    // Only an EMPTY pool refreshes the velocity; with particles alive the tick
    // zeroes it instead. So a burst inherits the emitter's motion at onset and
    // a steady stream stops inheriting after the first tick — verified at
    // instruction level, and the opposite of what the RE notes said.
    if (pool_.AliveCount() != 0) {
        wow_.motion.emitterVelocity = {0, 0, 0};
        return;
    }
    const f32 scale = (kVelocitySampleSeconds / accumulated) * desc_->inheritVelocityScale *
                      MotionToParticleSpace();
    wow_.motion.emitterVelocity = {(placement_.worldPos.x - placement_.prevWorldPos.x) * scale,
                                   (placement_.worldPos.y - placement_.prevWorldPos.y) * scale,
                                   (placement_.worldPos.z - placement_.prevWorldPos.z) * scale};
}

void Emitter2::InternalUpdate(f32 elapsed, f32 emissionScaler) {
    if (sc2_) {
        // The whole sub-step loop is SC2's own: its own clocks, its own
        // 60/30/15 Hz cadence capped at 100 steps, its own spawn sweep. None
        // of the dt policy below applies, so this returns rather than falling
        // through.
        TickSc2(elapsed, emissionScaler);
        return;
    }

    if (elapsed < 0.0f)
        elapsed = 0.0f;
    if (behavior_.dtPolicy == core::ParticleDtPolicy::ClampToMax && elapsed > behavior_.maxStepSeconds)
        elapsed = behavior_.maxStepSeconds;

    if (Visible() || run_.squirtOwed) {
        Sync();
    }

    if (behavior_.dtPolicy == core::ParticleDtPolicy::FixedSubSteps) {
        if (behavior_.inheritEmitterVelocity)
            TickEmitterVelocity(elapsed);

        // How much of the emitter's travel a trailing particle inherits: a
        // clamped line in the emitter's own speed, solved once per FRAME (the
        // speed is measured against the frame's dt, not a sub-step's) and only
        // then divided across the sub-steps.
        const Vector3f travel{placement_.worldPos.x - placement_.prevWorldPos.x,
                              placement_.worldPos.y - placement_.prevWorldPos.y,
                              placement_.worldPos.z - placement_.prevWorldPos.z};
        f32 follow = 0.0f;
        // The dt guard is the client's own: it bails out of Update before this
        // point, where we would divide by zero.
        if (behavior_.followPosition && desc_->followPosition && elapsed > kEmissionEpsilon) {
            const f32 distSq = travel.x * travel.x + travel.y * travel.y + travel.z * travel.z;
            // The record's two sample speeds are in model units per second, so
            // the measured speed has to be too — `travel` is renderer units
            // whatever space the particles live in.
            const f32 den =
                elapsed * ((placement_.unitScale > 0.0f) ? placement_.unitScale : 1.0f);
            const f32 speed = (distSq > 0.0f) ? (std::sqrt(distSq) / den) : 0.0f;
            const f32 raw = speed * desc_->followSlope + desc_->followBias;
            follow = (raw >= 0.0f) ? ((raw < 1.0f) ? raw : 1.0f) : 0.0f;
        }
        const f32 followScale = follow * MotionToParticleSpace();
        const Vector3f followed{travel.x * followScale, travel.y * followScale,
                                travel.z * followScale};

        const f32 stepSize = behavior_.subStepSeconds;
        if (elapsed <= stepSize) {
            wow_.motion.followDelta = followed;
            StepOnce(elapsed, emissionScaler);
        } else {
            // Fixed steps plus a remainder — NOT dt/n. When the lifespan cap
            // binds, the fixed steps deliberately cover less than `elapsed` and
            // the shortfall is dropped: nothing alive would have outlived it.
            const f32 whole = std::floor(elapsed / stepSize);
            const f32 lifeSteps = std::floor(wow_.lifeSpan / stepSize);
            const i32 n = static_cast<i32>((lifeSteps < whole) ? lifeSteps : whole);
            const f32 remainder = elapsed - stepSize * whole;
            const f32 invSub = 1.0f / static_cast<f32>(n + 1);
            wow_.motion.followDelta = {followed.x * invSub, followed.y * invSub,
                                       followed.z * invSub};
            for (i32 s = 0; s < n; ++s)
                StepOnce(stepSize, emissionScaler);
            StepOnce(remainder, emissionScaler);
        }
    } else {
        StepOnce(elapsed, emissionScaler);
    }

    if (pool_.AliveCount() == 0 && CRandom::dice_(kCompactOdds, compactSeed_) == 0) {
        pool_.Compact();
    }

    // Visibility is re-asserted every frame from the model's tracks — except
    // for a trail, which has no tracks and whose enable bit the client sets
    // once and never clears.
    if (!isTrail_)
        visible_ = false;
}

void Emitter2::Update(f32 elapsed, f32 emissionScaler) {
    trails_.ApplyOwner(*this);
    InternalUpdate(elapsed, emissionScaler);
}

i32 Emitter2::BuildGeometry(const BuildGeometryInput& in, std::vector<Vertex>& out) const {
    if (sc2_)
        return BuildSc2Geometry(*this, in, out);
    return BuildEmitterGeometry(*this, in, out);
}

} // namespace whiteout::flakes::renderer::particle
