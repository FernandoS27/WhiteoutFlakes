#include "renderer/particle/d3/d3_emitter.h"

#include "renderer/particle/d3/d3_channel_sampler.h"
#include "renderer/particle/output/particle_output.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle::d3 {

namespace {

// The five `dwPrtFlags` bits this emitter reads are `PrtFlag` (d3_channels.h);
// what each does is D3_PARTICLE_DESIGN.md §30.5.

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
    // The children go with it — a deliberate deviation: the engine's spawned
    // ACD outlives its system, but a replaying viewer would stack one per lap
    // (§17.2, §30.5).
    children_.DeathAll();

    systemAge_ = 0.0f;
    emitAccum_ = 0.0f;
    emittedLastUpdate_ = 0;
    Pool().Clear();
    lifetimeScale_ = LifetimeScale();
    preSimPending_ = d3desc_->preSimulate > 0.0f;

    // Where the emitter was goes too (a restart is usually a scrub): the next
    // distance-rate emit or carry must not measure a jump never travelled. The
    // draw stream carries on, as every dialect's does (§30.5).
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
    // Mode 10 (uniform random) is the one driver answerable without an actor;
    // every other mode leaves the scale at 1, as mode 0 does (§4.1, §30.5).
    if (r.mode != 10)
        return 1.0f;
    // The draw is replaced by its midpoint: the engine's comes off a GLOBAL
    // stream a scrubbed or reloaded timeline cannot keep stable (§30.5).
    return 0.5f * (r.lo + r.hi);
}

EvalCtx Emitter::EmitterCtx() const {
    const EmitterDesc& d = *d3desc_;
    EvalCtx c;
    // `tmLifetime` is the emitter clock's period in BOTH branches; only the wrap
    // differs. `tmEmissionPeriod` is the post-stop wind-down, not a loop (§19.1,
    // §30.5).
    c.timeMode = d.Has(PrtFlag::Persistent) ? TimeMode::Looped : TimeMode::Raw;
    c.time = systemAge_;
    // A zero period takes NormalisedTime's `period == 0` early-out (t = 0). The
    // randomiser scales the PERIOD, as ParticleSystem_Spawn multiplies sys+232.
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
    // Both off the emitter's seed, 1.0 when absent, and DIFFERENT terms: ch 34
    // feeds the quad's width (`particle+0xD4` @0x71000BEF7C), ch 35 the opacity
    // byte (`particle+0xD0` @0x71000BEE28). See §28.3, §30.5.
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
    // `PrtFlag::ParticleUnwrapped` picks mode 0 (the curve once across the life)
    // over mode 1 (wrapped into the loop sub-range); see §19.2.
    c.timeMode = d.Has(PrtFlag::ParticleUnwrapped) ? TimeMode::Raw : TimeMode::Looped;
    c.time = age;
    c.period = st.lifetime;

    // Driver modes 3 and 6, the two a viewer can supply, normalised by
    // `flMaxDistance` / `flCameraDistScale` in `.prt` units (§2.4, §18.1, §30.5).
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

/// `ParticleSystem_SetEmitterTransform` @0x71000AFBE0 (G-D3P-21): carry the live
/// particles under `PlanCarry`'s rule and refresh each one's `birthEmitterQuat`,
/// triple B's frame, which is not frozen at birth (§5.1, §22.3).
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
        // The engine's rotating arm also rotates the particle's axis at
        // pool+456; that half is recorded, not reproduced (§30.5).
        states_[idx].birthEmitterQuat = emitterQuat_;
    }
}

void Emitter::Update(f32 elapsed, f32 emissionScaler) {
    const EmitterDesc& d = *d3desc_;
    if (elapsed <= 0.0f)
        return;
    // `ParticleSystem_Spawn`'s pre-simulate at a FIXED 1/60 step (@0x71000ADF84,
    // §4.1), deferred to the first Update so it runs after the attach transform
    // arrives; run earlier, it lays the particles down at the origin (§30.5).
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
        // The step itself kills a particle that follows the ground off it.
        if (dead || !StepParticle(idx, elapsed, frame)) {
            // Ordered, not swap-with-last: the engine compacts the live list
            // (G-D3P-20), so emission order — the draw order — survives (§22.8).
            Pool().RemoveAliveAtOrdered(i);
            Pool().PushDead(idx);
            continue;
        }
        ++i;
    }

    if (Visible() && !EmissionFinished())
        TickEmit(elapsed, emissionScaler, frame);
}

} // namespace whiteout::flakes::renderer::particle::d3
