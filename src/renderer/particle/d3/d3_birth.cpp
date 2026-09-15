#include "renderer/particle/d3/d3_emitter.h"

#include "renderer/animation/anim_math.h"
#include "renderer/particle/d3/d3_channel_sampler.h"
#include "renderer/particle/d3/d3_emitter_math.h"
#include "renderer/particle/d3/d3_orientation.h"
#include "renderer/particle/output/particle_output.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle::d3 {

using detail::kAzimuthTwoPi;

namespace {

/// A uniform turn about @p axis, `(axis * sin h, cos h)` for h = U·2π·½, taking
/// one draw.
Quaternion RandomTurn(MwcRng& rng, const Vector3f& axis) {
    const f32 half = (rng.NextUnit() * kAzimuthTwoPi) * 0.5f;
    const f32 s = std::sin(half);
    return Quaternion{axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
}

/// `q * t`, summed in the order `ParticleSystem_EmitParticle` @0x71000B1F48
/// sums it.
Quaternion Compose(const Quaternion& q, const Quaternion& t) {
    return Quaternion{((t.x * q.w + t.w * q.x) + t.z * q.y) - t.y * q.z,
                      ((t.y * q.w + t.w * q.y) + t.x * q.z) - t.z * q.x,
                      (t.y * q.x + (t.z * q.w + t.w * q.z)) - t.x * q.y,
                      ((t.w * q.w - t.x * q.x) - t.y * q.y) - t.z * q.z};
}

/// `ParticleSystem_EmitParticle`'s write straight after the birth record, on
/// both of its branches: an authored axis that never varies replaces the drawn
/// one, normalised, unless it is degenerate.
void AdoptConstantSpinAxis(const EmitterDesc& d, const ChannelSampler& ch, ParticleState& st) {
    if (!d.Cap(kCapSpinAxis) || !d.spinAxisConstant)
        return;
    const Vector3f a = ch.EvalVector(kChSpinAxis);
    const f32 len = std::sqrt((a.x * a.x + a.y * a.y) + a.z * a.z);
    if (len > kEpsilon) {
        const f32 inv = 1.0f / len;
        st.spinAxis = {a.x * inv, a.y * inv, a.z * inv};
    }
}

/// @p v rotated by @p q, summed as `Particle_InitLifeAndSize` @0x71000B6F14 sums
/// it for the birth unit axis.
Vector3f RotateAsBirth(const Quaternion& q, const Vector3f& v) {
    const Vector3f t{(q.y * v.z - q.z * v.y) + q.w * v.x, v.y * q.w + (q.z * v.x - v.z * q.x),
                     v.z * q.w + (v.y * q.x - q.y * v.x)};
    const f32 dot = v.z * q.z + (q.y * v.y + v.x * q.x);
    return {q.w * t.x + (q.x * dot + (q.y * t.z - q.z * t.y)),
            q.w * t.y + (q.y * dot + (q.z * t.x - q.x * t.z)),
            q.w * t.z + (q.z * dot + (q.x * t.y - q.y * t.x))};
}

} // namespace

void InitLifeAndSize(MwcRng& rng, const EmitterDesc& d, const EvalCtx& ctx, const Vector3f& prev,
                     const Vector3f& now, f32 invUnit, f32 dt, const Quaternion& emitterQuat,
                     ParticleState& st) {
    const ChannelSampler ch{d, st.seed, ctx};
    st.baseSize = ch.Scalar(kChBirthSize, 1.0f);
    // A TimePath is an INT channel in frames, and a missing one is the literal
    // 1/60: one frame, not one second.
    st.lifetime = kFrameSeconds;
    if (ch.Has(kChParticleLife))
        st.lifetime = static_cast<f32>(ch.EvalInt(kChParticleLife)) * kFrameSeconds;

    if (ch.Has(kChSpeedLifeCut)) {
        const f32 cut = ch.EvalScalar(kChSpeedLifeCut);
        if (cut > kEpsilon) {
            const f32 inv = 1.0f / dt;
            const Vector3f v{inv * ((now.x - prev.x) * invUnit), inv * ((now.y - prev.y) * invUnit),
                             inv * ((now.z - prev.z) * invUnit)};
            const f32 speed = std::sqrt((v.x * v.x + v.y * v.y) + v.z * v.z);
            const f32 life = st.lifetime * (1.0f - (speed * cut) * dt);
            const f32 shorter = (life > st.lifetime) ? st.lifetime : life;
            st.lifetime = (life < kFrameSeconds) ? kFrameSeconds : shorter;
        }
    }

    if (d.Cap(kCapSpin))
        st.spinAxis = SamplePointOnSphere(rng, 1.0f);
    st.birthEmitterQuat = emitterQuat;
    // The orbit phase is seeded at birth, not derived per frame, so a particle
    // keeps its place on the ring for its whole life.
    if (d.Cap(kCapOrbit)) {
        const f32 phi = rng.NextUnit() * kAzimuthTwoPi;
        st.orbitDir = {std::cos(phi), std::sin(phi)};
    }
    st.axisUnit = RotateAsBirth(emitterQuat, kWorldX);
    if (d.Has(PrtFlag::RandomRoll))
        st.rollAngle = rng.NextUnit() * kAzimuthTwoPi;
    // The engine then adds pi to that roll when the system's spawn target is
    // type 15 and the LOCAL PLAYER's hero kind is 3 under game option 10
    // (@0x71000B7010). A viewer has no player, so the branch never runs.
}

Vector3f Emitter::BirthVelocity(u32 seed, const EvalCtx& ectx) {
    const EmitterDesc& d = *d3desc_;
    if (!d.Has(kChInitialVelocity) && !d.Has(kChWorldVelocity))
        return {0, 0, 0};
    const ChannelSampler ch{d, seed, ectx};

    // Evaluated even when only the world half exists: an absent path's value,
    // not an assumed zero.
    Vector3f v = ch.EvalVector(kChInitialVelocity);
    v = {v.x * kFramesPerSecond, v.y * kFramesPerSecond, v.z * kFramesPerSecond};
    if (ch.Has(kChSpreadAngle)) {
        // Channel 39 is seeded from the SYSTEM: one cone angle per emitter
        // (§22.4). The draw is gated on an exact `!= 0` angle AND the velocity's
        // length, so a degenerate velocity costs no random (§30.11).
        const f32 cone = ChannelSampler{d, emitterSeed_, ectx}.EvalScalar(kChSpreadAngle);
        const f32 mag = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (cone != 0.0f && mag > kEpsilon)
            v = d3::ConeSpread(v, cone, sysRng_.NextUnit() * kAzimuthTwoPi);
    }
    v = emitterQuat_.rotate_vector(v);
    if (ch.Has(kChWorldVelocity)) {
        const Vector3f w = ch.EvalVector(kChWorldVelocity);
        v = {v.x + w.x * kFramesPerSecond, v.y + w.y * kFramesPerSecond,
             v.z + w.z * kFramesPerSecond};
    }
    return v;
}

bool Emitter::BirthParticle(f32 dt, EmitContext& ec, const EmitterFrame& f) {
    const EmitterDesc& d = *d3desc_;

    if (Pool().DeadEmpty()) {
        const u32 want = static_cast<u32>(
            std::min<usize>(kMaxLiveParticles, std::max<usize>(kMinPoolGrowth, Pool().Capacity() * 2)));
        if (want <= Pool().Capacity())
            return false;
        GrowPool(want);
    }

    const u32 idx = Pool().PopDead();
    Particle2& p = Pool()[idx];
    ParticleState& st = states_[idx];
    st = ParticleState{};

    // The seed off the system stream, and the base on the segment the emitter
    // travelled this frame.
    const BirthDraw draw = DrawSeedAndBase(sysRng_, placement_.prevWorldPos, f.sysPos,
                                           d.Has(PrtFlag::BirthAtEmitter));
    st.seed = draw.seed;
    Vector3f base = draw.base;
    // System type 10 overrides whichever of the two it just computed.
    if (d.systemType == SystemType::WorldAnchored)
        base = {0, 0, 0};

    p.position = SampleShape(ec, base);
    p.velocity = {0, 0, 0};
    p.age = 0.0f;
    if (!PlaceOnGround(p.position)) {
        Pool().PushDead(idx);
        return false;
    }

    InitLifeAndSize(sysRng_, d, f.ctx, placement_.prevWorldPos, f.sysPos, f.invUnit, dt,
                    emitterQuat_, st);

    // Only the three solver-body types compute the birth velocity (§5); with no
    // solver here it parks in Particle2::velocity and drifts them (§15.7, §30.11).
    if (OwnsSolverBody(d.systemType))
        p.velocity = BirthVelocity(st.seed, f.ctx);

    AdoptConstantSpinAxis(d, ChannelSampler{d, st.seed, f.ctx}, st);
    // particle+216 (@0x71000B2310): the emitter quaternion, the identity in
    // render mode 1, and for a spinning system the camera-facing frame.
    st.orientation =
        (d.renderMode == PrtRenderMode::Unoriented) ? Quaternion::identity() : emitterQuat_;
    if (d.Cap(kCapSpin))
        SpinBirthSeat(camForward_, st.axisUnit, st.orientation);
    SeedUvStates(st);

    // The one-step update below kills a non-positive lifetime before the
    // particle is counted; every draw of the birth has been taken by then.
    if (st.lifetime <= 0.0f) {
        Pool().PushDead(idx);
        return false;
    }

    Pool().PushAlive(idx);

    // Newborns are advanced by the REMAINDER of the frame, not a whole one, so
    // a particle created mid-frame is not a frame behind. This is also what
    // gives frame 0 fully evaluated channels instead of zeros. The step can
    // kill it too, over no ground.
    if (!StepParticle(idx, dt, f)) {
        Pool().RemoveAliveAtOrdered(Pool().AliveCount() - 1);
        Pool().PushDead(idx);
        return false;
    }

    // Foliage turns each newborn about world Z (@0x7100E5DAD0), and that
    // quaternion's w is its gust phase (see `StepWindSpring`).
    if (UsesWindSpring(d.systemType))
        states_[idx].birthEmitterQuat = RandomTurn(sysRng_, kWorldUp);
    return true;
}

void Emitter::TickEmit(f32 dt, f32 emissionScaler, const EmitterFrame& f) {
    const EmitterDesc& d = *d3desc_;
    // `ParticleSystem_TickEmitter` returns before the accumulator for these
    // two: `if (eSystemType - 7 < 2) return 0`. Static clutter and the wind
    // foliage that carries it place their instances at load, not per frame.
    if (SkipsEmission(d.systemType))
        return;
    const ChannelSampler sys{d, emitterSeed_, f.ctx};

    // The three drivers, combined into one accumulator.
    const f32 rate = sys.PerSecond(kChEmissionRate);
    emitAccum_ += rate * dt * emissionScaler;

    if (sys.Has(kChDistanceRate)) {
        // The move is in renderer units but `perUnit` and the 300 clamp are
        // authored `.prt` speeds: convert back, as distNorm does, or a worldScale
        // 17 model over-emits seventeenfold (§30.11).
        const f32 inv = f.invUnit;
        const Vector3f now = f.sysPos;
        const Vector3f prev = placement_.prevWorldPos;
        const Vector3f delta{(now.x - prev.x) * inv, (now.y - prev.y) * inv,
                             (now.z - prev.z) * inv};
        const f32 speed =
            (dt > 0.0f)
                ? std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z) / dt
                : 0.0f;
        const bool clamped =
            (speed >= kDistanceEmissionMaxSpeed) && d.Has(PrtFlag::ClampDistanceEmission);
        if (speed > kEpsilon && !clamped) {
            const f32 perUnit = sys.EvalScalar(kChDistanceRate) * kFramesPerSecond;
            // The dt^2 is the engine's, and it is dimensionally odd: the term
            // is `speed * (rate*dt) * dt`, so a trail's density is frame-rate
            // coupled by design. Reproduced as measured.
            emitAccum_ += speed * (perUnit * dt) * dt * emissionScaler;
        }
    }

    i32 n = 0;
    while (emitAccum_ >= 1.0f) {
        emitAccum_ -= 1.0f;
        ++n;
    }

    // `sys+408 + sys+376` — child actors AND particles. A child-actor system
    // pools no particle at all, so for it the population IS the child count,
    // and that is what makes a target of 1 mean one model rather than one
    // model per frame forever.
    const i32 alive = static_cast<i32>(Pool().AliveCount()) + ChildCount();
    // The default target is ONE, not zero (§20.4, §30.11). The count path is
    // an IntPath; a rounded float sample is a different population.
    i32 target = 1;
    if (sys.Has(kChTargetCount))
        target = sys.EvalInt(kChTargetCount);
    n = std::max(n, target - alive);
    n = std::min(n, kMaxLiveParticles - alive);
    if (n <= 0)
        return;

    EmitContext ec = BuildEmitContext(f.ctx);
    ec.emitCount = n;
    const bool actors = d.SpawnsChildActors();
    for (i32 i = 0; i < n; ++i) {
        ec.emitIndex = i;
        if (actors ? SpawnChildActor(dt, ec, f) : BirthParticle(dt, ec, f))
            ++emittedLastUpdate_;
    }
}

// ---------------------------------------------------------------------------
// Child actors — eSystemType 1, 3 and 4
// ---------------------------------------------------------------------------

/// @brief One emission of a system whose particles are models:
///        `ParticleSystem_EmitParticle`'s first branch, with the ordinary birth
///        record draw for draw. The scale is ch 28 times the actor's tags 65543
///        and 65544 (@0x71000B1E38), which the load path puts on the desc; the
///        engine then forgets the child. See §5.6a, §30.11.
bool Emitter::SpawnChildActor(f32 dt, EmitContext& ec, const EmitterFrame& f) {
    const EmitterDesc& d = *d3desc_;
    if (!children_.Bound())
        return false;

    // The ordinary birth's record, so a burst of models is spread along the
    // emitter's travel exactly as a burst of particles is.
    const BirthDraw draw =
        DrawSeedAndBase(sysRng_, placement_.prevWorldPos, f.sysPos, d.Has(PrtFlag::BirthAtEmitter));
    ParticleState st;
    st.seed = draw.seed;
    Vector3f pos = SampleShape(ec, draw.base);
    if (!PlaceOnGround(pos))
        return false;
    InitLifeAndSize(sysRng_, d, f.ctx, placement_.prevWorldPos, f.sysPos, f.invUnit, dt,
                    emitterQuat_, st);
    // Type 3 also owns a solver body, which takes its velocity here.
    if (OwnsSolverBody(d.systemType))
        BirthVelocity(st.seed, f.ctx);

    // The actor's scale, with one draw for its random width when it has one.
    f32 scale = d.actorScale;
    if (d.actorScaleRandom != 0.0f)
        scale = scale + d.actorScaleRandom * sysRng_.NextUnit();

    // The roll flag turns the actor too, about world X (@0x7100E5DAB8), with a
    // second draw of its own.
    const Quaternion born =
        d.Has(PrtFlag::RandomRoll) ? Compose(emitterQuat_, RandomTurn(sysRng_, kWorldX)) : emitterQuat_;

    // The one-step update kills a non-positive lifetime before anything spawns.
    if (st.lifetime <= 0.0f)
        return false;
    // `Particle_ComputeInitialVelocity` again, for the spawn: its direction is
    // also the orientation axis below.
    const Vector3f velocity = BirthVelocity(st.seed, f.ctx);
    const f32 size = scale * st.baseSize;
    if (!(size > 0.0f))
        return false;

    // A new slot per birth: the children are never killed one at a time.
    const u32 handle = children_.Birth(static_cast<u32>(children_.SlotCount()), [&](ChildModelEvent& ev) {
        // The rotation half of `Actor_SpawnFromSno`'s transform: the gated arm
        // of the render-mode switch (§30.1). It reads only directions, so no
        // unit conversion belongs here.
        d3::FrameInput fi;
        fi.camForward = camForward_;
        // The engine's axis is that one step's displacement, whose direction is
        // the birth velocity's; candidate and fallback coincide (§30.11).
        fi.axis = velocity;
        fi.axisUnit = fi.axis;
        fi.fromSystem = {pos.x - f.sysPos.x, pos.y - f.sysPos.y, pos.z - f.sysPos.z};
        fi.emitterQuat = emitterQuat_;
        if (ConformsToGround(d.renderMode)) {
            ParticleState ground;
            RefreshGroundNormal(ground, pos);
            fi.groundNormal = ground.groundNormal;
        }
        // False leaves the birth quaternion standing, which is what the engine
        // leaves in the slot when the mode writes nothing.
        Quaternion orient = born;
        d3::BuildChildOrientation(d.renderMode, fi, orient);

        ev.route = ChildModelEvent::Route::D3Actor;
        ev.snoActor = d.snoActor;
        ev.transform = renderer::animation::ComposePivotSRT(pos, orient, {size, size, size},
                                                            {0.0f, 0.0f, 0.0f});
    });
    return handle != 0;
}

} // namespace whiteout::flakes::renderer::particle::d3
