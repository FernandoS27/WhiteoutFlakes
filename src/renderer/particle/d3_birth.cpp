#include "renderer/particle/d3_emitter.h"

#include "renderer/animation/anim_math.h"
#include "renderer/particle/d3_channel_sampler.h"
#include "renderer/particle/d3_emitter_math.h"
#include "renderer/particle/d3_orientation.h"
#include "renderer/particle/particle_output.h"

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

} // namespace

void InitLifeAndSize(MwcRng& rng, const EmitterDesc& d, const EvalCtx& ctx, const Vector3f& prev,
                     const Vector3f& now, f32 invUnit, f32 dt, ParticleState& st) {
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
    // The orbit phase is seeded at birth, not derived per frame, so a particle
    // keeps its place on the ring for its whole life.
    if (d.Cap(kCapOrbit)) {
        const f32 phi = rng.NextUnit() * kAzimuthTwoPi;
        st.orbitDir = {std::cos(phi), std::sin(phi)};
    }
    if (d.Has(PrtFlag::RandomRoll))
        st.rollAngle = rng.NextUnit() * kAzimuthTwoPi;
    // Not reproduced: the engine then adds pi to the roll when the system's
    // spawn target is an actor of kind 3, which a viewer never binds.
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
        // Seeded from the SYSTEM, not the particle: channel 39 is drawn once
        // per system, so every particle of one emitter shares its cone angle
        // and only the azimuth varies. The gates on the draw are an exact
        // `!= 0` on the angle AND a length test on the velocity — a degenerate
        // velocity costs no random at all, which is what keeps the stream
        // aligned for everything emitted after it.
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

    InitLifeAndSize(sysRng_, d, f.ctx, placement_.prevWorldPos, f.sysPos, f.invUnit, dt, st);
    st.birthEmitterQuat = emitterQuat_;
    st.orientation = Quaternion::identity();

    // Channel 38/39/40 build a birth velocity, and only the three system types
    // that own a body in the world-collision solver compute it — an ordinary
    // particle moves purely by its channels, and takes no cone draw. Those
    // three have no solver here, so it parks in Particle2::velocity and drifts
    // them: a better degradation than a freeze.
    if (OwnsSolverBody(d.systemType))
        p.velocity = BirthVelocity(st.seed, f.ctx);

    AdoptConstantSpinAxis(d, ChannelSampler{d, st.seed, f.ctx}, st);
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
    // gives frame 0 fully evaluated channels instead of zeros.
    StepParticle(idx, dt, f);

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
        // WorldPosition is in renderer units — the emitter matrix carries the
        // model's worldScale — but `perUnit` and the 300 clamp below are both
        // authored `.prt` speeds. Convert the move back with inv-UnitScale, the
        // same conversion the distNorm driver applies, or a scaled-up model
        // (worldScale 17) over-emits by that factor: the fog cloud that trails a
        // walking hero.
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
    // The default is ONE, not zero: `ParticleSystem_TickEmitter` initialises the
    // target to 1 and only overwrites it when the count path exists. So a system
    // with neither a rate nor a count still puts one particle on the screen — and
    // treating an absent count as zero is what makes such an asset invisible.
    // The path itself is an IntPath, evaluated in integers; rounding a float
    // sample gives a different population wherever the curve is between counts.
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

/// @brief One emission of a system whose particles are models.
///
/// `ParticleSystem_EmitParticle`'s first branch. Its birth record is the ordinary
/// path's, draw for draw — the same `Particle_InitLifeAndSize` over a particle on
/// the stack — and its one-step update can still reject it. The scale is the
/// birth size channel times the ACTOR's own: `Actor_SpawnFromSno` reads tags 65543
/// and 65544 off the `.acr` and the emit path pre-computes the same product,
/// which is why it passes spawn flag bit 2 to stop the spawn doing it twice.
/// The actor's half is not available here (it is a tag map on the `.acr`, not
/// a field), so what this carries is the `.prt`'s half and 1.0 for the rest —
/// which also means tag 65544's random width takes no draw.
///
/// The child is then FORGOTTEN by the engine: no transform is ever pushed to
/// it, nothing kills it when the system ends, and `ReleaseAttachments` frees
/// the link node alone. So a `cos_wings_*` system spawns its wings once, lives
/// its authored second, and the wings stay — which is the behaviour, not a gap
/// in the reading.
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
    const Vector3f pos = SampleShape(ec, draw.base);
    InitLifeAndSize(sysRng_, d, f.ctx, placement_.prevWorldPos, f.sysPos, f.invUnit, dt, st);
    // Type 3 also owns a solver body, which takes its velocity here.
    if (OwnsSolverBody(d.systemType))
        BirthVelocity(st.seed, f.ctx);

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
    const f32 size = st.baseSize;
    if (!(size > 0.0f))
        return false;

    // A new slot per birth: the children are never killed one at a time.
    const u32 handle = children_.Birth(static_cast<u32>(children_.SlotCount()), [&](ChildModelEvent& ev) {
        // The rotation half of `Actor_SpawnFromSno`'s transform. The engine builds
        // a scratch particle for this emission, steps it once and runs the whole
        // render-mode switch on it before spawning; what the gated arm writes is
        // what the actor is born holding. Everything the switch can read is in
        // `.prt` units and only ever as a direction, so no unit conversion belongs
        // here.
        d3::FrameInput fi;
        fi.camForward = camForward_;
        // The engine's axis is that one step's displacement. Its direction is the
        // birth velocity's — which is also the vector the spawn passes the actor —
        // and the candidate and the fallback are the same thing on a particle that
        // has never moved.
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
