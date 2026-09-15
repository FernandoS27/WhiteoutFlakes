#include "renderer/particle/d3_emitter.h"

#include "renderer/particle/d3_channel_sampler.h"
#include "renderer/particle/particle_output.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle::d3 {

namespace {

// The five `dwPrtFlags` bits (`PrtFlag`), and what each one does here:
//
// `Persistent` (bit 0) — the system runs until it is told to stop.
// `ParticleSystem_TickEmitter` @0x71000AEA20 branches its WHOLE timing model on
// this. Clear (12,331 files): the emitter channels are sampled at
// `elapsed / tmLifetime` in time mode 0 — no wrap, one pass — and the system is
// released the moment `elapsed >= tmLifetime`. Set (9,262 files): time mode 1,
// so the same quotient WRAPS into the path's loop sub-range, and the release
// test is not run at all. `tmLifetime` is 60 frames on 6,884 files, so reading
// the bit as "expires anyway" stops a third of every shipped effect after
// exactly one second.
//
// `BirthAtEmitter` (bit 8) — birth AT the emitter, not along its path. Clear
// (the common case) makes `Particle_InitLifeAndSize` draw a uniform `u` and
// place the particle at `lerp(prevPos, pos, u)`: a random point on the segment
// the emitter travelled this frame, which is what stops a fast emitter stamping
// a whole frame's particles at one spot. Set skips both the lerp AND THE DRAW.
// This build gated it on the system type instead, which is neither the same
// condition nor the same draw count. The same bit does a second job
// `ParticleSystem_SetEmitterTransform` reads (G-D3P-21): it CARRIES the live
// particles when the emitter moves. Between the two the bit means "this system
// is emitter-local", and reading it as the birth rule alone leaves an attached
// effect trailing behind the thing it is on.
//
// `ParticleUnwrapped` (bit 10) — the PARTICLE channels are sampled unwrapped.
// `Particle_BuildEvalContext` @0x710037AD70 opens on `sys+13 & 4`, which is this
// bit of the same word: set, the particle's channels use time mode 0 and play
// their curve once across the particle's life; clear, mode 1 wraps the quotient
// into `[loopStart, loopEnd]` and the curve repeats. 18,415 of 21,593 files set
// it, so mode 1 is the exception and not the rule.
//
// `ClampDistanceEmission` (bit 28) ENABLES the 300 u/s clamp on distance
// emission.
//
// `CarryWithoutRotation` (bit 29) — carry by translation only, never by
// rotation. Only consulted when bit 8 is set. With it clear a turning emitter
// rotates every live particle's offset by `newQ * conj(oldQ)`; with it set the
// offsets are translated and the rotation is dropped.

const std::shared_ptr<const EmitterDesc>& DefaultD3Desc() {
    static const std::shared_ptr<const EmitterDesc> d = std::make_shared<EmitterDesc>();
    return d;
}

} // namespace

Emitter::Emitter() : d3desc_(DefaultD3Desc()) {}

void Emitter::SetD3Desc(std::shared_ptr<const EmitterDesc> desc) {
    d3desc_ = desc ? std::move(desc) : DefaultD3Desc();
    Restart();
}

void Emitter::Restart() {
    // The children go with it. The ENGINE does not do this — a spawned ACD
    // outlives the system that made it, and `ParticleSystem_ReleaseAttachments`
    // frees the link nodes and nothing else — but a viewer replaying a clip
    // would then stack one model per lap until the instance cap. Same call
    // `D3AttachmentPool` makes for a TriggerEvent child, for the same reason.
    children_.DeathAll();

    systemAge_ = 0.0f;
    emitAccum_ = 0.0f;
    emittedLastUpdate_ = 0;
    Pool().Clear();
    lifetimeScale_ = LifetimeScale();
    preSimPending_ = d3desc_->preSimulate > 0.0f;

    // Where the emitter was goes too, as `Emitter2::ResetParticles` forgets it
    // for the other dialects: a restart is usually a scrub, and the first
    // distance-rate emit after it must not measure a jump the emitter never
    // travelled, nor carry live particles across it. The draw stream carries
    // on, as every dialect's does.
    placement_.Unseed();
    carrySeeded_ = false;
}

EmitterDrawHeader Emitter::DrawHeader() const {
    EmitterDrawHeader h;
    h.output = d3desc_->SpawnsChildActors() ? ParticleOutput::ChildModel
                                            : ParticleOutput::Billboard;
    h.priorityPlane = d3desc_->priorityPlane;
    h.material = &d3desc_->material;
    return h;
}

void Emitter::GrowPool(u32 capacity) {
    const usize before = pool_.Capacity();
    pool_.Sync(capacity);
    if (pool_.Capacity() != before)
        states_.resize(pool_.Capacity());
}

bool Emitter::EmissionFinished() const {
    const EmitterDesc& d = *d3desc_;
    // A persistent system has no expiry at all: the engine's release test sits
    // inside the branch this bit skips, and a viewer never sends the stop that
    // would start the wind-down. See kFlagPersistent.
    if (d.Has(PrtFlag::Persistent))
        return false;
    if (d.lifetime <= 0.0f)
        return false;
    return systemAge_ >= d.lifetime * lifetimeScale_;
}

void Emitter::RunPreSimulate() {
    constexpr f32 kStep = kFrameSeconds;
    // Round rather than truncate: the desc holds `tmPreSimulate * (1/60)` and
    // dividing that back by the same float leaves 0.5 s at 29.999998 steps,
    // one short of the 30 the engine's countdown takes.
    const f32 secs = std::min(d3desc_->preSimulate, kMaxPreSimulateSeconds);
    const i32 steps = static_cast<i32>(std::lround(secs * kFramesPerSecond));
    for (i32 i = 0; i < steps; ++i)
        Update(kStep, 1.0f);
}

f32 Emitter::LifetimeScale() const {
    const Driver& r = d3desc_->lifetimeRandom;
    // Mode 10 is the one driver that needs nothing but a number: the engine
    // takes `Rand_MWC_Next(global) * 2^-32` and maps it through lo..hi, so a
    // `(0.75, 1.25)` system lives between three quarters and a quarter over its
    // authored length. 47 shipped files ask for it. Every other non-zero mode —
    // 54 files, modes 1, 2, 4 and 8 — reads an actor attribute or a game
    // scalar, and behaves here the way mode 0 does: the scale stays 1.
    if (r.mode != 10)
        return 1.0f;
    // The draw itself is replaced by its midpoint, the same treatment
    // `D3EffectResolver` gives `nChance` and the weighted group modes: the
    // engine draws from a GLOBAL stream, so reproducing it would need state a
    // scrubbed or reloaded timeline cannot keep stable, and a viewer that
    // re-rolled would show a different length every replay.
    return 0.5f * (r.lo + r.hi);
}

EvalCtx Emitter::EmitterCtx() const {
    const EmitterDesc& d = *d3desc_;
    EvalCtx c;
    // `tmLifetime` is the emitter clock's period in BOTH branches — the
    // quotient is `elapsed / lifetime` and only the wrap differs. `tmEmissionPeriod`
    // is not a loop length: it is the wind-down the engine runs AFTER a stop
    // request (ParticleSystem_RequestStop sets sys+236 and the emitter fades over it), and
    // normalising against it here made every channel run 1/2 to 1/6 of its
    // authored length.
    c.timeMode = d.Has(PrtFlag::Persistent) ? TimeMode::Looped : TimeMode::Raw;
    c.time = systemAge_;
    // A zero period divides by zero in NormalisedTime; the engine's own guard
    // is the `period == 0` early-out, which yields t = 0. The randomiser scales
    // the PERIOD, the way ParticleSystem_Spawn does — it multiplies sys+232.
    const f32 life = d.lifetime * lifetimeScale_;
    c.period = (life > 0.0f) ? life : 0.0f;
    return c;
}

EmitterFrame Emitter::BuildEmitterFrame() const {
    const EmitterDesc& d = *d3desc_;
    EmitterFrame f;
    f.ctx = EmitterCtx();
    f.sysPos = WorldPosition();
    f.unit = UnitScale();
    f.invUnit = 1.0f / f.unit;
    // Both off the emitter's own seed, both 1.0 when the path is absent — and
    // they drive DIFFERENT things: `particle+0xD4 = ch1 * (birthSize *
    // sys+0x134)` @0x71000BEF7C is the quad's width, while `particle+0xD0 =
    // ch5 * sys+0x12C` @0x71000BEE28 is the opacity byte. Only the first is a
    // size.
    const ChannelSampler sys{d, emitterSeed_, f.ctx};
    f.sizeScale = sys.Scalar(kChSizeScale, 1.0f);
    f.hasEffectScale = sys.Has(kChEffectScale);
    if (f.hasEffectScale)
        f.effectScale = sys.EvalScalar(kChEffectScale);
    return f;
}

EvalCtx Emitter::ParticleCtx(const ParticleState& st, const Vector3f& pos, f32 age,
                             const EmitterFrame& f) const {
    const EmitterDesc& d = *d3desc_;
    EvalCtx c;
    // Mode 0 plays the curve once across the particle's life; mode 1 wraps it
    // into the path's own loop sub-range, so a channel whose `loopEnd` is 0.5
    // runs twice. See `PrtFlag::ParticleUnwrapped` — the bit picks between them
    // and 85% of shipped files ask for mode 0.
    c.timeMode = d.Has(PrtFlag::ParticleUnwrapped) ? TimeMode::Raw : TimeMode::Looped;
    c.time = age;
    c.period = st.lifetime;

    // The two driver sources a viewer can actually supply. `flMaxDistance` and
    // `flCameraDistScale` do double duty here: they are the kill radius and
    // the camera placement scale, AND the normalising divisors for modes 3
    // and 6.
    // In `.prt` units, not renderer units: both divisors are authored numbers
    // and the separation they normalise came off a scaled world matrix.
    const Vector3f rel{(pos.x - f.sysPos.x) * f.invUnit, (pos.y - f.sysPos.y) * f.invUnit,
                       (pos.z - f.sysPos.z) * f.invUnit};
    if (d.maxDistance > kEpsilon) {
        const f32 len = std::sqrt(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
        c.driver.distNorm = len / d.maxDistance;
    }
    // `> 1e-6`, not `|x| > 1e-6`: a negative camera scale leaves the height
    // driver unset in the engine rather than dividing by it.
    if (d.cameraDistScale > kEpsilon)
        c.driver.heightNorm = rel.z / d.cameraDistScale;
    return c;
}

void Emitter::CollectOutputEvents(std::vector<ChildModelEvent>& out) {
    // Births and deaths only. The engine pushes no per-frame transform to a
    // spawned actor — it is a free ACD with its own animation from the moment
    // it exists — so neither does this.
    children_.Drain(out);
}

/// `ParticleSystem_SetEmitterTransform` @0x71000AFBE0, measured by G-D3P-21.
///
/// `dwPrtFlags` bit 8 is not only "birth at the emitter": it also carries the
/// LIVE particles when the emitter moves, which is what makes a system
/// emitter-local rather than world-space. Bit 29 restricts that to translation.
/// Types 2 and 3 never carry whatever the flags say, and type 9 leaves the
/// function before anything happens.
///
/// A carried move also refreshes each particle's `birthEmitterQuat`. That is the
/// frame the emitter-local kinematic triple rotates its displacement by, and it
/// is NOT frozen at birth as §5.1 had it: an emitter that turns re-frames every
/// particle it still owns.
void Emitter::CarryWithEmitter() {
    const EmitterDesc& d = *d3desc_;
    const Vector3f now = WorldPosition();
    if (!carrySeeded_) {
        carryPos_ = now;
        carryQuat_ = emitterQuat_;
        carrySeeded_ = true;
        return;
    }
    const CarryMove move =
        PlanCarry(d.systemType, d.Has(PrtFlag::BirthAtEmitter),
                  d.Has(PrtFlag::CarryWithoutRotation), carryPos_, carryQuat_, now, emitterQuat_);
    carryPos_ = now;
    carryQuat_ = emitterQuat_;
    if (!move.carries)
        return;

    for (usize i = 0; i < Pool().AliveCount(); ++i) {
        const u32 idx = Pool().AliveAt(i);
        Particle2& p = Pool()[idx];
        p.position = CarryPoint(move, p.position);
        // On the rotating arm the engine also rotates the particle's own axis
        // at pool+456 — the emitter-rotated world axis `Particle_InitLifeAndSize`
        // writes at birthRecord+152 and `Particle_BuildOrientationBasis` takes as
        // its fallback. This build carries no such field (`spinAxis` is a
        // different offset, birthRecord+172), so that half is recorded, not
        // reproduced.
        states_[idx].birthEmitterQuat = emitterQuat_;
    }
}

void Emitter::Update(f32 elapsed, f32 emissionScaler) {
    const EmitterDesc& d = *d3desc_;
    if (elapsed <= 0.0f)
        return;
    // `ParticleSystem_Spawn` runs the system forward before anything sees it:
    // `tmPreSimulate / 60` seconds of TickEmitter(forceEmit) + UpdateAndCull at
    // a FIXED 1/60 step, clamped to 1000 s (@0x71000ADF84). 2,532 of 21,593
    // shipped files ask for it and they are the ambient set -- a torch fire
    // authored with 15 frames is meant to be already burning the first time it
    // is drawn, not building up from nothing. Deferred to here rather than done
    // in Restart because the engine spawns after the attach point is resolved,
    // and a system simulated before its transform arrives lays its particles
    // down at the origin.
    if (preSimPending_) {
        preSimPending_ = false;
        RunPreSimulate();
    }
    emittedLastUpdate_ = 0;
    CarryWithEmitter();

    // Types 4, 5 and 7 — light shafts and static ground clutter. The engine
    // returns before doing anything at all, so they are one static frame.
    if (!SimulatesParticles(d.systemType)) {
        if (UsesWindSpring(d.systemType)) {
            // Foliage and clutter. Emit once so there is something to sway,
            // then run only the spring: no channels, no emission accumulator,
            // no motion models.
            if (Pool().AliveCount() == 0 && Visible()) {
                const EmitterFrame frame = BuildEmitterFrame();
                EmitContext ec = BuildEmitContext(frame.ctx);
                const ChannelSampler sys{d, emitterSeed_, frame.ctx};
                const i32 target = sys.Has(kChTargetCount) ? sys.EvalInt(kChTargetCount) : 0;
                const i32 n = std::clamp(target, 0, kMaxWindSpringPopulation);
                ec.emitCount = n;
                for (i32 i = 0; i < n; ++i) {
                    ec.emitIndex = i;
                    if (BirthParticle(elapsed, ec, frame))
                        ++emittedLastUpdate_;
                }
            }
            StepWindSpring(elapsed);
        }
        return;
    }

    systemAge_ += elapsed;
    const EmitterFrame frame = BuildEmitterFrame();

    // Age, kill, then move. The kill test is the particle's own lifetime, plus
    // the system's kill radius (`flMaxDistance`), which is what fires the 3501
    // triggered event in the engine.
    const Vector3f& sysPos = frame.sysPos;
    // The radius is authored, the separation is in renderer units; square the
    // conversion into the threshold rather than the distance.
    const f32 killR2 = d.maxDistance * d.maxDistance * frame.unit * frame.unit;
    for (usize i = 0; i < Pool().AliveCount();) {
        const u32 idx = Pool().AliveAt(i);
        Particle2& p = Pool()[idx];
        p.age += elapsed;

        bool dead = p.age >= states_[idx].lifetime;
        if (!dead && d.maxDistance > kEpsilon) {
            const f32 dx = p.position.x - sysPos.x;
            const f32 dy = p.position.y - sysPos.y;
            dead = (dx * dx + dy * dy) > killR2;
        }
        if (dead) {
            // Ordered, not swap-with-last: the engine compacts the live list and
            // renumbers the survivors (G-D3P-20), so emission order survives
            // every death. A swap shuffles the list on each one, and for an
            // alpha-blended system that is the draw order.
            Pool().RemoveAliveAtOrdered(i);
            Pool().PushDead(idx);
            continue;
        }
        StepParticle(idx, elapsed, frame);
        ++i;
    }

    if (Visible() && !EmissionFinished())
        TickEmit(elapsed, emissionScaler, frame);
}

} // namespace whiteout::flakes::renderer::particle::d3
