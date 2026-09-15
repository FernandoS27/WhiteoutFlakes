#include "renderer/particle/d3_emitter.h"

#include "io/d3/d3_sno_cache.h"
#include "renderer/animation/anim_math.h"
#include "renderer/particle/d3_orientation.h"
#include "renderer/particle/particle_geometry.h"
#include "renderer/particle/particle_output.h"
#include "whiteout/flakes/model_types.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle::d3 {

namespace {

/// @brief `ch5 * ch35` as the byte the draw is gated on.
///
/// `Particle_PrepareDrawFrame` @0x71000BCC48 loads `particle+0xD0` and turns it
/// into an integer: below 1e-6 (@0x7100E3BEA0) the result is zero and
/// `TST W27,#0xFF / B.EQ` @0x71000BCD54 skips `Particle_WriteQuadVertices`
/// outright; at or above 0.999999 (@0x7100E3BFF8) it is opaque; in between it is
/// `FCVTPS(s * 255)` @0x71000BCC80, i.e. a ceil.
///
/// This is why neither channel belongs in the quad's extent. `arScalePath`'s
/// shipped shape is a 0 -> 1 -> 0 ramp over the particle's life, which is a fade,
/// and `arEffectScalePath`'s range never exceeds 1.0 on any of the 21,593 files,
/// which is an attenuation.
u8 D3OpacityByte(f32 s) {
    if (!(s >= kEpsilon))
        return 0;
    if (s >= kNearlyOne)
        return 255;
    return static_cast<u8>(std::ceil(s * 255.0f));
}
constexpr f32 kHalfPi = 1.57079632679489661923f;
/// @brief The engine's OTHER two-pi, and the one every azimuth is built from.
///
/// `6.28318452835083` @0x7100E3BFD0, two ulps below the correctly-rounded 2pi it
/// keeps at 0x7100E3BEE4 for angle wrapping. Both print as `6.2832` in a
/// decompile, so nothing but running the code separates them. The three shape
/// samplers multiply their draw by THIS one; `WrapAngle` and `AsinFast` use the
/// real one. Measured as ~4e-7 relative on a sphere sample's x and y.
constexpr f32 kAzimuthTwoPi = 6.28318452835083f;
/// @brief The engine's 8x2pi clamp, applied before it wraps an angle.
///
/// `50.26548386`, which is `8 * 2pi` in single precision — NOT the `50.265` a
/// decompile prints. Hex-Rays rounds a float constant to five significant digits
/// when it displays it, so a constant read out of pseudocode is a rounded
/// reading of the real one; this is the same trap that had AsinFast's whole
/// polynomial off by 3e-5. Kept because the clamp changes the result for a large
/// angle, not just the speed.
constexpr f32 kAngleClamp = kTwoPi * 8.0f;
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

Vector3f Normalized(const Vector3f& v, const Vector3f& fallback) {
    const f32 l2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (l2 <= kEpsilon * kEpsilon)
        return fallback;
    const f32 inv = 1.0f / std::sqrt(l2);
    return {v.x * inv, v.y * inv, v.z * inv};
}

f32 WrapAngle(f32 a) {
    // The engine clamps to +/-8pi first and only then wraps, so an angle that
    // ran away is pinned rather than folded from wherever it reached.
    a = std::clamp(a, -kAngleClamp, kAngleClamp);
    while (a < 0.0f)
        a += kTwoPi;
    while (a > kTwoPi)
        a -= kTwoPi;
    return a;
}

/// `Math_AsinFast` @0x7100979640 — the polynomial arcsine the sphere samplers
/// use. Its negative branch returns the angle WRAPPED INTO [0, 6.2832] rather
/// than in [-pi/2, 0], through the same clamp-then-wrap `WrapAngle` reproduces.
///
/// This build used to return `-r` on the grounds that only sin and cos are ever
/// taken of the result. That is nearly true and measurably not: the engine wraps
/// by its own rounded 6.2832, so `-r` and `-r + 6.2832` differ by 1.5e-5 rad.
/// G-D3P-01 measured 3.3e-5 on the sine before this matched the original.
f32 AsinFast(f32 x) {
    // Abramowitz & Stegun 4.4.45, at the FULL precision the constant pool holds
    // (@0x7100E3C394..0x7100E3C3A0 and pi/2 @0x7100E3BF6C). The decompile prints
    // these as 0.018729 / 0.074261 / 0.21211 / 1.5707 / 1.5708; transcribing
    // those cost up to 3.2e-5 on the result, which G-D3P-01 caught by running
    // the original against ours.
    const f32 a = std::fmin(std::fabs(x), 1.0f);
    const f32 poly = ((a * -0.0187293f + 0.0742610f) * a - 0.2121144f) * a + 1.5707288f;
    const f32 r = kHalfPi - poly * std::sqrt(1.0f - a);
    return (x < 0.0f) ? WrapAngle(-r) : r;
}

} // namespace

// ---------------------------------------------------------------------------
// Shape samplers. Split out of the emitter so the shape gate can drive them
// directly — the distributions are the part most likely to be got wrong.
// ---------------------------------------------------------------------------

f32 SampleRadiusInAnnulus(MwcRng& rng, f32 inner, f32 thickness) {
    const f32 outer = inner + thickness;
    if (outer < kEpsilon)
        return 0.0f;
    const f32 f = inner / outer;
    const f32 f2 = f * f;
    const f32 rem = 1.0f - f2;
    // NO DRAW when the annulus has zero width. The stream is positional, so
    // consuming one here would desynchronise every later channel.
    const f32 t = (rem != 0.0f) ? (f2 + rem * rng.NextUnit()) : f2;
    return outer * std::sqrt(std::fmax(t, 0.0f));
}

Vector3f SamplePointOnSphere(MwcRng& rng, f32 radius) {
    const f32 u = rng.NextUnit();
    const f32 elev = AsinFast(u + u - 1.0f);
    const f32 phi = rng.NextUnit() * kAzimuthTwoPi;
    const f32 ce = std::cos(elev);
    return {std::sin(phi) * radius * ce, std::cos(phi) * radius * ce, std::sin(elev) * radius};
}

Vector3f SamplePointOnHemisphere(MwcRng& rng, f32 radius) {
    // The ONLY difference from the sphere: the elevation draw is [0,1) rather
    // than [-1,1), so the result never leaves the +Z half.
    const f32 elev = AsinFast(rng.NextUnit());
    const f32 phi = rng.NextUnit() * kAzimuthTwoPi;
    const f32 ce = std::cos(elev);
    return {std::sin(phi) * radius * ce, std::cos(phi) * radius * ce, std::sin(elev) * radius};
}

Vector3f SamplePointOnCircleXY(MwcRng& rng, f32 radius) {
    const f32 phi = rng.NextUnit() * kAzimuthTwoPi;
    return {std::cos(phi) * radius, std::sin(phi) * radius, 0.0f};
}

// ---------------------------------------------------------------------------
// The capability mask. ParticleSystem_Spawn asks each channel whether its value
// range is non-trivial and the per-frame step branches on the answer; without
// it every asset integrates five motion models per particle. `IsInert` is the
// same "one node, start == end, and that value is the identity" test the
// engine's constancy mask uses, applied to the channels that GATE a model
// rather than to all twenty-four.
// ---------------------------------------------------------------------------

void EmitterDesc::DeriveCapabilities() {
    caps = 0;
    auto live = [&](i32 id, f32 identity) {
        const Path& p = Channel(id);
        return !p.nodes.empty() && !p.IsInert(identity);
    };

    // All THREE orbit channels feed the bit, not just the radial pair: Spawn
    // ORs the ranges of ch 7, ch 8 and ch 9 together, so an asset that only
    // spins (angular speed, no radial motion) still enables the model.
    if (live(kChOrbitRadSpeed, 0.0f) || live(kChOrbitRadius, 0.0f) ||
        live(kChOrbitAngSpeed, 0.0f))
        caps |= kCapOrbit;
    if (live(kChRadialSpeed, 0.0f) || live(kChRadialOffset, 0.0f))
        caps |= kCapRadial;
    if (live(kChOffsetA, 0.0f) || live(kChVelocityA, 0.0f) || live(kChAccelA, 0.0f))
        caps |= kCapTripleA;
    if (live(kChOffsetB, 0.0f) || live(kChVelocityB, 0.0f) || live(kChAccelB, 0.0f))
        caps |= kCapTripleB;
    if (live(kChSpinRate, 0.0f) || live(kChSpinAngle, 0.0f))
        caps |= kCapSpin;
    if (live(kChSpinAxis, 0.0f))
        caps |= kCapSpinAxis;
    if (live(kChSeekSpeed, 0.0f) || live(kChSeekOffset, 0.0f))
        caps |= kCapSeek;
    // Rotation is FORCED OFF for the two foliage types, whatever the channels
    // say — the engine skips the whole block for them.
    if (!UsesWindSpring(systemType) && (live(kChRollRate, 0.0f) || live(kChRollAngle, 0.0f)))
        caps |= kCapRoll;
    if (EmitsActors(systemType))
        caps |= kCapRibbon;
}

// ---------------------------------------------------------------------------

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
    for (u32 h : childHandles_) {
        ChildModelEvent ev;
        ev.kind = ChildModelEvent::Kind::Death;
        ev.owner = childOwner_;
        ev.emitterId = childEmitterId_;
        ev.childHandle = h;
        childPending_.push_back(ev);
    }
    childHandles_.clear();

    systemAge_ = 0.0f;
    emitAccum_ = 0.0f;
    emittedLastUpdate_ = 0;
    Pool().Clear();
    lifetimeScale_ = LifetimeScale();
    preSimPending_ = d3desc_->preSimulate > 0.0f;
}

EmitterDrawHeader Emitter::DrawHeader() const {
    EmitterDrawHeader h;
    h.output = d3desc_->SpawnsChildActors() ? ParticleOutput::ChildModel
                                            : ParticleOutput::Billboard;
    h.priorityPlane = d3desc_->priorityPlane;
    h.material = &d3desc_->material;
    h.materialTimeSec = MaterialTimeSec();
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

void Emitter::RefreshGroundNormal(ParticleState& st, const Vector3f& pos) const {
    // `Particle_UpdateGroundNormal` re-casts only when the particle's XY has
    // moved (the engine keys on pool+548/552), so a resting decal samples once.
    if (st.groundSeeded && st.groundAt.x == pos.x && st.groundAt.y == pos.y)
        return;
    st.groundAt = {pos.x, pos.y};
    st.groundSeeded = true;
    // World up is the engine's own answer when the raycast misses, so it is
    // also the right thing to hold when there is no query at all.
    st.groundNormal = kWorldUp;
    if (!surface_.groundQuery)
        return;

    // The query answers with a height, not a normal, so take two tangents a
    // step apart and cross them. On the grid every sample is kGroundZ and this
    // is exactly (0,0,1); a host with real terrain gets the real slope.
    constexpr f32 kStep = 1.0f;
    // How far up and down a particle will look for ground. Wide, because a
    // `.prt` decal can sit well above its floor and the engine's raycast has no
    // comparable window — a host that wants a tighter one answers false, which
    // lands on the miss default above.
    constexpr f32 kReach = 1000.0f;
    f32 z0 = 0.0f, zx = 0.0f, zy = 0.0f;
    if (!surface_.groundQuery(pos, kReach, kReach, z0) ||
        !surface_.groundQuery({pos.x + kStep, pos.y, pos.z}, kReach, kReach, zx) ||
        !surface_.groundQuery({pos.x, pos.y + kStep, pos.z}, kReach, kReach, zy))
        return;
    const Vector3f n = FrameCross({kStep, 0.0f, zx - z0}, {0.0f, kStep, zy - z0});
    const f32 len = FrameLength(n);
    if (len > kFrameEpsilon)
        st.groundNormal = FrameNormalise(n, len);
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

EvalCtx Emitter::ParticleCtx(const ParticleState& st, const Vector3f& pos, f32 age) const {
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
    const Vector3f sysPos = WorldPosition();
    const f32 inv = 1.0f / UnitScale();
    const Vector3f rel{(pos.x - sysPos.x) * inv, (pos.y - sysPos.y) * inv,
                       (pos.z - sysPos.z) * inv};
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

// ---------------------------------------------------------------------------
// Emission
// ---------------------------------------------------------------------------

Emitter::EmitContext Emitter::BuildEmitContext() const {
    const EmitterDesc& d = *d3desc_;
    const EvalCtx ec = EmitterCtx();

    EmitContext c;
    c.shape = d.shape;

    switch (d.shape) {
    case Shape::SphereShell:
    case Shape::HemisphereShell:
        d.shapeExtent0.ScalarEndpoints(ec, c.ext0Lo, c.ext0Hi);
        break;
    case Shape::Cylinder:
    case Shape::Ring:
        d.shapeExtent0.ScalarEndpoints(ec, c.ext0Lo, c.ext0Hi);
        d.shapeExtent1.ScalarEndpoints(ec, c.ext1Lo, c.ext1Hi);
        break;
    case Shape::Box: {
        // The extent-2 VECTOR path evaluated at r = (0,0,0) and r = (1,1,1),
        // which recovers the low and high corner of the node's random range.
        const Vector4f lo = SampleAt(d.shapeExtent2, {0, 0, 0, 0}, ec);
        const Vector4f hi = SampleAt(d.shapeExtent2, {1, 1, 1, 1}, ec);
        c.boxLo = {lo.x, lo.y, lo.z};
        c.boxHi = {hi.x, hi.y, hi.z};
        break;
    }
    default:
        // Point, and the three mesh shapes. A mesh shape with no bound actor
        // falls through to the point case in the engine too, so this is not a
        // simplification — it is the same branch.
        break;
    }
    return c;
}

// `base` arrives in renderer units (it came off the emitter's world matrix);
// everything this function computes is authored, so it leaves in `.prt` units
// and is converted on the way out. See SetUnitScale.
Vector3f Emitter::SkinEmitMeshVertex(const EmitMesh& m, u32 vertex) const {
    const Vector3f& rest = m.rest[vertex];
    if (m.bones.empty() || surface_.pose.empty() || surface_.invBind.empty())
        return rest;

    const auto& b = m.bones[vertex];
    const auto& lane = m.weights[vertex];
    Vector3f acc{0, 0, 0};
    f32 sum = 0.0f;
    // Four slots since the promotion out of d3::; a `.prt` never fills the
    // fourth, and the weight test below is what makes that free rather than
    // a behaviour change.
    for (usize k = 0; k < kEmitMeshBones; ++k) {
        if (!(lane[k] > 0.0f) || b[k] < 0 || b[k] >= static_cast<i32>(surface_.pose.size()) ||
            b[k] >= static_cast<i32>(surface_.invBind.size()))
            continue;
        const Vector3f p = whiteout::transform_point(
            rest, surface_.invBind[static_cast<usize>(b[k])] * surface_.pose[static_cast<usize>(b[k])]);
        acc = {acc.x + p.x * lane[k], acc.y + p.y * lane[k], acc.z + p.z * lane[k]};
        sum += lane[k];
    }
    if (!(sum > kEpsilon))
        return rest;
    const f32 inv = 1.0f / sum;
    return {acc.x * inv, acc.y * inv, acc.z * inv};
}

bool Emitter::SampleEmitMeshPoint(bool sequential, u32 sequence, Vector3f& out) {
    if (!surface_.mesh || surface_.mesh->Empty())
        return false;
    const EmitMesh& m = *surface_.mesh;

    // Which sub-object. The engine reads the system's own index and draws
    // uniformly when it is -1, with the modulo shortcut it uses everywhere a
    // count might be a power of two; nothing in a viewer ever sets one, so
    // this is always the draw.
    const u32 subCount = static_cast<u32>(m.subs.size());
    const u32 r = sysRng_.Next();
    const EmitMesh::SubMesh& sm =
        m.subs[(((subCount - 1) & subCount) != 0) ? (r % subCount) : (r & (subCount - 1))];
    if (sm.triCount == 0)
        return false;

    u32 tri;
    if (sequential) {
        // Shape 11 walks its triangles in order off a counter that is never
        // reset, so consecutive emissions spread over the surface instead of
        // clustering the way a random draw does.
        tri = sm.firstTri + (sequence % sm.triCount);
    } else {
        // Area-uniform, from the running sum. One draw, because the engine
        // takes one — the stream is positional and an extra draw here shifts
        // every value after it.
        const u32 last = sm.firstTri + sm.triCount - 1;
        const f32 pick = sysRng_.NextUnit() * m.areaCdf[last];
        tri = last;
        for (u32 i = sm.firstTri; i <= last; ++i) {
            if (m.areaCdf[i] >= pick) {
                tri = i;
                break;
            }
        }
    }

    const Vector3f p0 = SkinEmitMeshVertex(m, m.tris[tri * 3 + 0]);
    const Vector3f p1 = SkinEmitMeshVertex(m, m.tris[tri * 3 + 1]);
    const Vector3f p2 = SkinEmitMeshVertex(m, m.tris[tri * 3 + 2]);

    // Two draws folded into the triangle: when they land outside it the SECOND
    // is mirrored and the first's complement is taken, which is the engine's
    // arithmetic rather than the usual `if (a+b>1) { a=1-a; b=1-b; }`.
    const f32 a = sysRng_.NextUnit();
    f32 b = sysRng_.NextUnit();
    f32 w1 = a;
    if (a + b > 1.0f) {
        b = 1.0f - b;
        w1 = 1.0f - a;
    }
    const f32 w0 = (1.0f - w1) - b;
    const Vector3f local{p0.x * w0 + p1.x * w1 + p2.x * b, p0.y * w0 + p1.y * w1 + p2.y * b,
                         p0.z * w0 + p1.z * w1 + p2.z * b};
    out = whiteout::transform_point(local, surface_.toWorld);
    return true;
}

Vector3f Emitter::SampleShape(EmitContext& ec, const Vector3f& base) {
    Vector3f local{0, 0, 0};
    const f32 u = UnitScale();

    switch (ec.shape) {
    case Shape::SphereShell: {
        const f32 r = SampleRadiusInAnnulus(sysRng_, ec.ext0Lo, ec.ext0Hi - ec.ext0Lo);
        local = SamplePointOnSphere(sysRng_, r);
        break;
    }
    case Shape::HemisphereShell: {
        const f32 r = SampleRadiusInAnnulus(sysRng_, ec.ext0Lo, ec.ext0Hi - ec.ext0Lo);
        local = SamplePointOnHemisphere(sysRng_, r);
        break;
    }
    case Shape::Cylinder: {
        const f32 r = SampleRadiusInAnnulus(sysRng_, ec.ext0Lo, ec.ext0Hi - ec.ext0Lo);
        local = SamplePointOnCircleXY(sysRng_, r);
        local.z = ec.ext1Lo;
        if (ec.ext1Hi - ec.ext1Lo != 0.0f)
            local.z += (ec.ext1Hi - ec.ext1Lo) * sysRng_.NextUnit();
        break;
    }
    case Shape::Ring: {
        const f32 r = SampleRadiusInAnnulus(sysRng_, ec.ext0Lo, ec.ext0Hi - ec.ext0Lo);
        // Not random: the azimuth is the emit index spread evenly across the
        // particles being emitted THIS TICK, starting from one random phase
        // drawn at index 0. n spokes, not n scattered points.
        f32 phi;
        if (ec.emitIndex != 0 && ec.emitCount > 0) {
            phi = ec.ringPhase +
                  // The SPACING keeps the real 2pi -- SampleEmitterShape loads the
                  // azimuth constant exactly once, for the index-0 draw above.
                  (static_cast<f32>(ec.emitIndex) * kTwoPi) / static_cast<f32>(ec.emitCount);
            phi = WrapAngle(phi);
        } else {
            phi = sysRng_.NextUnit() * kAzimuthTwoPi;
            ec.ringPhase = phi;
        }
        local = {std::cos(phi) * r, std::sin(phi) * r, 0.0f};
        local.z = ec.ext1Lo;
        if (ec.ext1Hi - ec.ext1Lo != 0.0f)
            local.z += (ec.ext1Hi - ec.ext1Lo) * sysRng_.NextUnit();
        break;
    }
    case Shape::Box: {
        f32 v[3];
        const f32 lo[3] = {ec.boxLo.x, ec.boxLo.y, ec.boxLo.z};
        const f32 hi[3] = {ec.boxHi.x, ec.boxHi.y, ec.boxHi.z};
        for (i32 k = 0; k < 3; ++k) {
            const f32 span = hi[k] - lo[k];
            v[k] = lo[k];
            if (span != 0.0f)
                v[k] += span * sysRng_.NextUnit();
        }
        // The box case adds the base but NOT the emit-context offset. It is
        // the only shape that skips it; reproduced, not tidied.
        return {base.x + v[0] * u, base.y + v[1] * u, base.z + v[2] * u};
    }
    case Shape::MeshRandom:
    case Shape::MeshActorKind4:
    case Shape::MeshSequential: {
        // The counter advances whichever of the three this is — only shape 11
        // reads it, and the engine increments it in TickEmitter before the
        // dispatch rather than inside the sampler.
        const u32 seq = emitSequence_++;
        Vector3f p;
        if (SampleEmitMeshPoint(ec.shape == Shape::MeshSequential, seq, p))
            return p; // a surface point REPLACES the base; it is not an offset
        break;        // no surface bound: the engine's own point-case fallback
    }
    case Shape::Point:
    default:
        break;
    }

    return {base.x + (ec.offset.x + local.x) * u, base.y + (ec.offset.y + local.y) * u,
            base.z + (ec.offset.z + local.z) * u};
}

Vector3f Emitter::BirthVelocity(u32 seed, const EvalCtx& ectx) {
    const EmitterDesc& d = *d3desc_;
    if (!d.Has(kChInitialVelocity) && !d.Has(kChWorldVelocity))
        return {0, 0, 0};

    Vector3f v = d.Channel(kChInitialVelocity).EvalVector(seed, kChInitialVelocity, ectx);
    v = {v.x * kFramesPerSecond, v.y * kFramesPerSecond, v.z * kFramesPerSecond};
    if (d.Has(kChSpreadAngle)) {
        // Seeded from the SYSTEM, not the particle: channel 39 is drawn once
        // per system, so every particle of one emitter shares its cone angle
        // and only the azimuth varies. The gates on the draw are an exact
        // `!= 0` on the angle AND a length test on the velocity — a degenerate
        // velocity costs no random at all, which is what keeps the stream
        // aligned for everything emitted after it.
        const f32 cone = d.Channel(kChSpreadAngle).EvalScalar(emitterSeed_, kChSpreadAngle, ectx);
        const f32 mag = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (cone != 0.0f && mag > kEpsilon)
            v = d3::ConeSpread(v, cone, sysRng_.NextUnit() * kAzimuthTwoPi);
    }
    v = emitterQuat_.rotate_vector(v);
    if (d.Has(kChWorldVelocity)) {
        const Vector3f w = d.Channel(kChWorldVelocity).EvalVector(seed, kChWorldVelocity, ectx);
        v = {v.x + w.x * kFramesPerSecond, v.y + w.y * kFramesPerSecond,
             v.z + w.z * kFramesPerSecond};
    }
    return v;
}

bool Emitter::BirthParticle(f32 dt, EmitContext& ec) {
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

    // The particle's seed, drawn from the system stream. The engine nudges the
    // two values that would collide with its own sentinels; reproduce it so a
    // seed-for-seed trace matches.
    const u32 raw = sysRng_.Next();
    st.seed = (raw >= kFirstSentinelSeed) ? (raw + 2u) : raw;

    // Sub-frame emission interpolation: the base is a random point on the
    // segment the emitter travelled this frame, not its current position. One
    // draw, and it is what keeps a fast-moving emitter from stamping all of a
    // frame's particles at one spot.
    const Vector3f now = WorldPosition();
    Vector3f base = now;
    if (!d.Has(PrtFlag::BirthAtEmitter)) {
        const Vector3f prev = placement_.prevWorldPos;
        const f32 u = sysRng_.NextUnit();
        base = {prev.x + u * (now.x - prev.x), prev.y + u * (now.y - prev.y),
                prev.z + u * (now.z - prev.z)};
    }
    // System type 10 overrides whichever of the two it just computed.
    if (d.systemType == SystemType::WorldAnchored)
        base = {0, 0, 0};

    p.position = SampleShape(ec, base);
    p.velocity = {0, 0, 0};
    p.age = 0.0f;

    // Lifetime and base size, in the engine's order: an asset that asks for a
    // non-positive lifetime hands the slot straight back.
    const EvalCtx ectx = EmitterCtx();
    // A TimePath is an INT channel in FRAMES. With no path at all the engine uses
    // ONE FRAME, not one second — `Particle_InitLifeAndSize` writes the literal
    // 1/60 — and a system whose lifetime path is missing is meant to be a
    // one-frame flash rather than a second of particles.
    st.lifetime = kFrameSeconds;
    if (d.Has(kChParticleLife)) {
        const i32 frames = d.Channel(kChParticleLife).EvalInt(st.seed, kChParticleLife, ectx);
        st.lifetime = static_cast<f32>(frames) * kFrameSeconds;
    }
    if (st.lifetime <= 0.0f) {
        Pool().PushDead(idx);
        return false;
    }
    st.baseSize = d.Has(kChBirthSize)
                      ? d.Channel(kChBirthSize).EvalScalar(st.seed, kChBirthSize, ectx)
                      : 1.0f;

    st.birthEmitterQuat = emitterQuat_;
    st.orientation = Quaternion::identity();

    // The orbit phase is seeded at birth, not derived per frame, so a particle
    // keeps its place on the ring for its whole life.
    if (d.Cap(kCapOrbit)) {
        const f32 phi = sysRng_.NextUnit() * kAzimuthTwoPi;
        st.orbitDir = {std::cos(phi), std::sin(phi)};
        st.orbitSeeded = true;
    }

    // Channel 38/39/40 build a birth velocity, but ONLY the three system types
    // that own a body in the world-collision solver ever consult it — an
    // ordinary particle moves purely by its channels. We compute it anyway and
    // park it in Particle2::velocity, because those three types have no solver
    // here and a straight-line drift is a better degradation than a freeze.
    p.velocity = BirthVelocity(st.seed, ectx);

    st.swayPhase = sysRng_.NextUnit();
    SeedUvStates(st);

    Pool().PushAlive(idx);

    // Newborns are advanced by the REMAINDER of the frame, not a whole one, so
    // a particle created mid-frame is not a frame behind. This is also what
    // gives frame 0 fully evaluated channels instead of zeros.
    StepParticle(idx, dt);
    return true;
}

/// @brief Seed one particle's four UV animation states, `MatTex_InitUvState`
///        @0x71000F7C60 once per stage, as `ParticleSystem_EmitParticle` does.
///
/// Everything here belongs to the PARTICLE. The engine draws the initial phase,
/// the rate jitter and the flip-book's start frame "so instances of the same
/// effect are desynchronised" — and for a particle system the instances are the
/// particles, one 72-byte state each at `particle+16 + 72*stage`. Without the
/// draws every member of a puff sits on the same tile of the same sheet, which
/// is what 12,361 of the corpus's 13,897 mode-2 entries are authored against.
///
/// The draws come off the PARTICLE's own stream rather than the engine's global
/// one, which is a deliberate difference: the engine's is a process-wide counter
/// no trace could reproduce, and the particle's is already the seed every
/// channel of this particle uses. The ORDER is the engine's — stages ascending,
/// and within a mode-2 stage the U phase, the V phase, then the U, V and
/// rotation rate jitters — so a stage that takes no draw does not shift the
/// ones after it.
void Emitter::SeedUvStates(ParticleState& st) const {
    const MaterialDesc& m = d3desc_->d3mat;
    MwcRng rng = MwcRng::Seed(st.seed ^ kUvSeedSalt);
    for (u32 s = 0; s < MaterialDesc::kMaxLayers; ++s) {
        if (m.setLayer[s] < 0)
            continue;
        const MaterialLayer& L = m.layers[static_cast<usize>(m.setLayer[s])];
        ParticleState::UvState& u = st.uv[s];
        if (L.uv.mode == ::whiteout::flakes::io::D3UvMode::Anim2D) {
            if (!L.atlas || L.atlas->frames.empty())
                continue;
            const i32 count = static_cast<i32>(L.atlas->frames.size());
            const u32 span = static_cast<u32>(L.atlasFrameRange) + 1u;
            i32 frame = L.atlasFrameBase + static_cast<i32>(rng.Next() % span);
            // `>=`, not `>`: the engine clamps to count-1 with a `>=` test, so a
            // range that overruns the sheet lands on the last frame rather than
            // wrapping.
            if (frame >= count - 1)
                frame = count - 1;
            if (frame < 0)
                frame = 0;
            u.cursor = static_cast<f32>(frame);
            u.cursorRate = L.atlasRate;
            if (L.atlasRateJitter != 0.0f)
                u.cursorRate += L.atlasRateJitter * rng.NextUnit();
        } else if (L.uv.mode == ::whiteout::flakes::io::D3UvMode::ScaleRotateScroll) {
            u.u = L.uv.randomPhaseU ? rng.NextUnit() : L.uv.offset.x;
            u.v = L.uv.randomPhaseV ? rng.NextUnit() : L.uv.offset.y;
            u.uRate = L.uv.scrollPerSec.x;
            if (L.uv.scrollJitter.x != 0.0f)
                u.uRate += L.uv.scrollJitter.x * rng.NextUnit();
            u.vRate = L.uv.scrollPerSec.y;
            if (L.uv.scrollJitter.y != 0.0f)
                u.vRate += L.uv.scrollJitter.y * rng.NextUnit();
            u.rotRate = L.uv.rotatePerSec;
            if (L.uv.rotateJitter != 0.0f)
                u.rotRate += L.uv.rotateJitter * rng.NextUnit();
            // Already clamped to 8x2pi and folded into [0, 2pi] by D3ReadUvXform,
            // which is what the engine does before the angle reaches the state.
            u.rot = L.uv.rotate;
        }
    }
}

/// @brief Advance them, `MatTex_TickUvStateEntry` @0x71000F8310 and
///        `Anim2D_AdvanceCursor` @0x710033EE90.
///
/// Mode 2 integrates the scroll and either WRAPS it into [0,1] or, with
/// `tAnim3.flAmount` set, CLAMPS it there — which turns a loop into a one-way
/// reveal. The angle is clamped to 8x2pi and folded the same way. Mode 3 steps
/// its flip-book: the length is `count - 0.0001`, which is what lets the last
/// frame be reached while a loop still wraps before `count`; a once-shot clamps
/// and zeroes its own rate.
void Emitter::StepUvStates(ParticleState& st, f32 dt) const {
    const MaterialDesc& m = d3desc_->d3mat;
    for (u32 s = 0; s < MaterialDesc::kMaxLayers; ++s) {
        if (m.setLayer[s] < 0)
            continue;
        const MaterialLayer& L = m.layers[static_cast<usize>(m.setLayer[s])];
        ParticleState::UvState& u = st.uv[s];
        if (L.uv.mode == ::whiteout::flakes::io::D3UvMode::Anim2D) {
            // `fabsf(rate) < 1e-6`, not `!= 0`: the engine's early-out is on the
            // magnitude, so a NEGATIVE rate plays the sheet backwards. One
            // shipped entry authors -30 fps.
            if (!L.atlas || std::fabs(u.cursorRate) < kEpsilon)
                continue;
            const f32 length = static_cast<f32>(L.atlas->frames.size()) - 0.0001f;
            if (length <= 0.0f)
                continue;
            f32 c = u.cursor + u.cursorRate * dt;
            if (L.atlasLoops) {
                while (c > length)
                    c -= length;
            } else if (c > length) {
                c = length;
                u.cursorRate = 0.0f;
            }
            u.cursor = c;
        } else if (L.uv.mode == ::whiteout::flakes::io::D3UvMode::ScaleRotateScroll) {
            u.u += u.uRate * dt;
            u.v += u.vRate * dt;
            if (L.uv.clampUv) {
                u.u = std::clamp(u.u, 0.0f, 1.0f);
                u.v = std::clamp(u.v, 0.0f, 1.0f);
            } else {
                u.u = ::whiteout::flakes::io::D3FoldUv(u.u);
                u.v = ::whiteout::flakes::io::D3FoldUv(u.v);
            }
            u.rot = ::whiteout::flakes::io::D3WrapAngle(u.rot + u.rotRate * dt);
        }
    }
}

void Emitter::TickEmit(f32 dt, f32 emissionScaler) {
    const EmitterDesc& d = *d3desc_;
    // `ParticleSystem_TickEmitter` returns before the accumulator for these
    // two: `if (eSystemType - 7 < 2) return 0`. Static clutter and the wind
    // foliage that carries it place their instances at load, not per frame.
    if (SkipsEmission(d.systemType))
        return;
    const EvalCtx ectx = EmitterCtx();

    // The three drivers, combined into one accumulator.
    f32 rate = 0.0f;
    if (d.Has(kChEmissionRate))
        rate = d.Channel(kChEmissionRate).EvalScalar(emitterSeed_, kChEmissionRate, ectx) *
               kFramesPerSecond;
    emitAccum_ += rate * dt * emissionScaler;

    if (d.Has(kChDistanceRate)) {
        // WorldPosition is in renderer units — the emitter matrix carries the
        // model's worldScale — but `perUnit` and the 300 clamp below are both
        // authored `.prt` speeds. Convert the move back with inv-UnitScale, the
        // same conversion the distNorm driver applies, or a scaled-up model
        // (worldScale 17) over-emits by that factor: the fog cloud that trails a
        // walking hero.
        const f32 inv = 1.0f / UnitScale();
        const Vector3f now = WorldPosition();
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
            const f32 perUnit =
                d.Channel(kChDistanceRate).EvalScalar(emitterSeed_, kChDistanceRate, ectx) *
                kFramesPerSecond;
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
    if (d.Has(kChTargetCount))
        target = d.Channel(kChTargetCount).EvalInt(emitterSeed_, kChTargetCount, ectx);
    n = std::max(n, target - alive);
    n = std::min(n, kMaxLiveParticles - alive);
    if (n <= 0)
        return;

    EmitContext ec = BuildEmitContext();
    ec.emitCount = n;
    const bool actors = d.SpawnsChildActors();
    for (i32 i = 0; i < n; ++i) {
        ec.emitIndex = i;
        if (actors ? SpawnChildActor(ec) : BirthParticle(dt, ec))
            ++emittedLastUpdate_;
    }
}

// ---------------------------------------------------------------------------
// Child actors — eSystemType 1, 3 and 4
// ---------------------------------------------------------------------------

/// @brief One emission of a system whose particles are models.
///
/// `ParticleSystem_EmitParticle`'s first branch, which shares the emitter half
/// with the ordinary path and none of the rest: the shape sampler places it,
/// `Particle_InitLifeAndSize` can still reject it, and the scale is the birth
/// size channel times the ACTOR's own — `Actor_SpawnFromSno` reads tags 65543
/// and 65544 off the `.acr` and the emit path pre-computes the same product,
/// which is why it passes spawn flag bit 2 to stop the spawn doing it twice.
/// The actor's half is not available here (it is a tag map on the `.acr`, not
/// a field), so what this carries is the `.prt`'s half and 1.0 for the rest.
///
/// The child is then FORGOTTEN by the engine: no transform is ever pushed to
/// it, nothing kills it when the system ends, and `ReleaseAttachments` frees
/// the link node alone. So a `cos_wings_*` system spawns its wings once, lives
/// its authored second, and the wings stay — which is the behaviour, not a gap
/// in the reading.
bool Emitter::SpawnChildActor(EmitContext& ec) {
    const EmitterDesc& d = *d3desc_;
    if (!allocHandle_)
        return false;

    const u32 raw = sysRng_.Next();
    const u32 seed = (raw >= kFirstSentinelSeed) ? (raw + 2u) : raw;

    // The same sub-frame interpolation an ordinary birth uses: the emitter may
    // have moved this frame and a burst must not stamp every model on one spot.
    const Vector3f now = WorldPosition();
    const f32 u = sysRng_.NextUnit();
    const Vector3f base{placement_.prevWorldPos.x + u * (now.x - placement_.prevWorldPos.x),
                        placement_.prevWorldPos.y + u * (now.y - placement_.prevWorldPos.y),
                        placement_.prevWorldPos.z + u * (now.z - placement_.prevWorldPos.z)};
    const Vector3f pos = SampleShape(ec, base);

    // `Particle_InitLifeAndSize` still runs, and still rejects: a non-positive
    // lifetime hands the slot back before anything is spawned.
    const EvalCtx ectx = EmitterCtx();
    if (d.Has(kChParticleLife)) {
        const f32 frames = d.Channel(kChParticleLife).EvalScalar(seed, kChParticleLife, ectx);
        if (std::round(frames) * kFrameSeconds <= 0.0f)
            return false;
    }
    const f32 size = d.Has(kChBirthSize)
                         ? d.Channel(kChBirthSize).EvalScalar(seed, kChBirthSize, ectx)
                         : 1.0f;
    if (!(size > 0.0f))
        return false;

    const u32 handle = allocHandle_();
    if (handle == 0)
        return false;
    childHandles_.push_back(handle);

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
    fi.axis = BirthVelocity(seed, ectx);
    fi.axisUnit = fi.axis;
    const Vector3f sysPos = WorldPosition();
    fi.fromSystem = {pos.x - sysPos.x, pos.y - sysPos.y, pos.z - sysPos.z};
    fi.emitterQuat = emitterQuat_;
    if (ConformsToGround(d.renderMode)) {
        ParticleState ground;
        RefreshGroundNormal(ground, pos);
        fi.groundNormal = ground.groundNormal;
    }
    // False leaves the emitter's own quaternion standing, which is what the
    // engine leaves in the slot when the mode writes nothing.
    Quaternion orient = emitterQuat_;
    d3::BuildChildOrientation(d.renderMode, fi, orient);

    ChildModelEvent ev;
    ev.kind = ChildModelEvent::Kind::Birth;
    ev.owner = childOwner_;
    ev.emitterId = childEmitterId_;
    ev.childHandle = handle;
    ev.route = ChildModelEvent::Route::D3Actor;
    ev.snoActor = d.snoActor;
    ev.transform = renderer::animation::ComposePivotSRT(pos, orient, {size, size, size},
                                                        {0.0f, 0.0f, 0.0f});
    childPending_.push_back(ev);
    return true;
}

void Emitter::CollectOutputEvents(std::vector<ChildModelEvent>& out) {
    // Births and deaths only. The engine pushes no per-frame transform to a
    // spawned actor — it is a free ACD with its own animation from the moment
    // it exists — so neither does this.
    for (auto& ev : childPending_)
        out.push_back(ev);
    childPending_.clear();
}

// ---------------------------------------------------------------------------
// Per-particle step
// ---------------------------------------------------------------------------

void Emitter::StepParticle(u32 idx, f32 dt) {
    const EmitterDesc& d = *d3desc_;
    Particle2& p = Pool()[idx];
    ParticleState& st = states_[idx];

    const Vector3f sysPos = WorldPosition();
    const EvalCtx ctx = ParticleCtx(st, p.position, p.age);
    const f32 invDt = (dt > kEpsilon) ? (1.0f / dt) : 0.0f;

    // The four UV states run on the same tick as the motion, ahead of it, the
    // way ParticleSystem_ForEachParticle steps all four before it calls
    // ParticleSystem_UpdateParticles on the survivor.
    StepUvStates(st, dt);

    // The whole of this function works in `.prt` units — every speed, offset
    // and acceleration below is a number straight out of the file, and the two
    // models that read the particle's own position (orbit, seek) divide it in
    // on the way. `disp` is converted once at the end, which is the only place
    // renderer units appear.
    const f32 u = UnitScale();
    const f32 inv = 1.0f / u;

    Vector3f disp{0, 0, 0};

    // ---- cylindrical orbit: axis (ch 10), radius (7), radial speed (8),
    //      angular speed (9) ------------------------------------------------
    if (d.Cap(kCapOrbit)) {
        const Vector3f axis =
            d.Has(kChOrbitAxis)
                ? Normalized(d.Channel(kChOrbitAxis).EvalVector(st.seed, kChOrbitAxis, ctx),
                             {0, 0, 1})
                : Vector3f{0, 0, 1};
        const Quaternion q = d3::OrientationFromAxes({0, 0, 1}, axis);
        const Vector3f rel{(p.position.x - sysPos.x) * inv, (p.position.y - sysPos.y) * inv,
                           (p.position.z - sysPos.z) * inv};
        const Vector3f local = q.inverse().rotate_vector(rel);

        const f32 planarLen = std::sqrt(local.x * local.x + local.y * local.y);
        if (planarLen > kEpsilon)
            st.orbitDir = {local.x / planarLen, local.y / planarLen};

        f32 radialSpeed = 0.0f;
        if (d.Has(kChOrbitRadSpeed))
            radialSpeed =
                d.Channel(kChOrbitRadSpeed).EvalScalar(st.seed, kChOrbitRadSpeed, ctx) *
                kFramesPerSecond;
        if (d.Has(kChOrbitRadius)) {
            const f32 now = d.Channel(kChOrbitRadius).EvalScalar(st.seed, kChOrbitRadius, ctx);
            radialSpeed += (now - st.prevOrbitRadius) * invDt;
            st.prevOrbitRadius = now;
        }
        f32 angular = 0.0f;
        if (d.Has(kChOrbitAngSpeed))
            angular =
                d.Channel(kChOrbitAngSpeed).EvalScalar(st.seed, kChOrbitAngSpeed, ctx) *
                kFramesPerSecond;

        const f32 a = angular * dt;
        const f32 ca = std::cos(a), sa = std::sin(a);
        const Vector3f rotated{local.x * ca - local.y * sa, local.x * sa + local.y * ca, 0.0f};
        const Vector3f planar{rotated.x - local.x + st.orbitDir.x * radialSpeed * dt,
                              rotated.y - local.y + st.orbitDir.y * radialSpeed * dt, 0.0f};
        const Vector3f world = q.rotate_vector(planar);
        disp = {disp.x + world.x, disp.y + world.y, disp.z + world.z};
    }

    // ---- radial push away from the system origin (ch 11 offset, 12 speed) --
    if (d.Cap(kCapRadial)) {
        const Vector3f rel{p.position.x - sysPos.x, p.position.y - sysPos.y,
                           p.position.z - sysPos.z};
        const f32 len = std::sqrt(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
        if (len > kEpsilon)
            st.radialDir = {rel.x / len, rel.y / len, rel.z / len};

        f32 speed = 0.0f;
        if (d.Has(kChRadialSpeed))
            speed = d.Channel(kChRadialSpeed).EvalScalar(st.seed, kChRadialSpeed, ctx) *
                    kFramesPerSecond;
        if (d.Has(kChRadialOffset)) {
            const f32 now = d.Channel(kChRadialOffset).EvalScalar(st.seed, kChRadialOffset, ctx);
            speed += (now - st.prevRadial) * invDt;
            st.prevRadial = now;
        }
        disp = {disp.x + st.radialDir.x * speed * dt, disp.y + st.radialDir.y * speed * dt,
                disp.z + st.radialDir.z * speed * dt};
    }

    // ---- kinematic triple A (17/18/19), WORLD space -----------------------
    if (d.Cap(kCapTripleA)) {
        Vector3f v{0, 0, 0};
        if (d.Has(kChVelocityA)) {
            const Vector3f s = d.Channel(kChVelocityA).EvalVector(st.seed, kChVelocityA, ctx);
            v = {s.x * kFramesPerSecond, s.y * kFramesPerSecond, s.z * kFramesPerSecond};
        }
        if (d.Has(kChOffsetA)) {
            const Vector3f now = d.Channel(kChOffsetA).EvalVector(st.seed, kChOffsetA, ctx);
            v = {v.x + (now.x - st.prevOffsetA.x) * invDt,
                 v.y + (now.y - st.prevOffsetA.y) * invDt,
                 v.z + (now.z - st.prevOffsetA.z) * invDt};
            st.prevOffsetA = now;
        }
        if (d.Has(kChAccelA)) {
            const Vector3f a = d.Channel(kChAccelA).EvalVector(st.seed, kChAccelA, ctx);
            st.accelVelA = {st.accelVelA.x + a.x * kFramesPerSecondSq * dt,
                            st.accelVelA.y + a.y * kFramesPerSecondSq * dt,
                            st.accelVelA.z + a.z * kFramesPerSecondSq * dt};
        }
        disp = {disp.x + (v.x + st.accelVelA.x) * dt, disp.y + (v.y + st.accelVelA.y) * dt,
                disp.z + (v.z + st.accelVelA.z) * dt};
    }

    // ---- kinematic triple B (20/21/22), EMITTER-LOCAL ----------------------
    if (d.Cap(kCapTripleB)) {
        Vector3f v{0, 0, 0};
        if (d.Has(kChVelocityB)) {
            const Vector3f s = d.Channel(kChVelocityB).EvalVector(st.seed, kChVelocityB, ctx);
            v = {s.x * kFramesPerSecond, s.y * kFramesPerSecond, s.z * kFramesPerSecond};
        }
        if (d.Has(kChOffsetB)) {
            const Vector3f now = d.Channel(kChOffsetB).EvalVector(st.seed, kChOffsetB, ctx);
            v = {v.x + (now.x - st.prevOffsetB.x) * invDt,
                 v.y + (now.y - st.prevOffsetB.y) * invDt,
                 v.z + (now.z - st.prevOffsetB.z) * invDt};
            st.prevOffsetB = now;
        }
        if (d.Has(kChAccelB)) {
            const Vector3f a = d.Channel(kChAccelB).EvalVector(st.seed, kChAccelB, ctx);
            st.accelVelB = {st.accelVelB.x + a.x * kFramesPerSecondSq * dt,
                            st.accelVelB.y + a.y * kFramesPerSecondSq * dt,
                            st.accelVelB.z + a.z * kFramesPerSecondSq * dt};
        }
        // The PER-FRAME delta is what gets rotated and applied; the running
        // total the engine also keeps is not what drives motion.
        const Vector3f local{(v.x + st.accelVelB.x) * dt, (v.y + st.accelVelB.y) * dt,
                             (v.z + st.accelVelB.z) * dt};
        st.localDispB = {st.localDispB.x + local.x, st.localDispB.y + local.y,
                         st.localDispB.z + local.z};
        const Vector3f world = st.birthEmitterQuat.rotate_vector(local);
        disp = {disp.x + world.x, disp.y + world.y, disp.z + world.z};
    }

    // ---- target seek (ch 13 speed, 14 offset) ------------------------------
    if (d.Cap(kCapSeek) && hasSeekTarget_) {
        Vector3f dir{seekTarget_.x - p.position.x, seekTarget_.y - p.position.y,
                     seekTarget_.z - p.position.z};
        const f32 len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
        if (len > kEpsilon)
            dir = {dir.x / len, dir.y / len, dir.z / len};

        f32 s = 0.0f;
        if (d.Has(kChSeekSpeed))
            s = d.Channel(kChSeekSpeed).EvalScalar(st.seed, kChSeekSpeed, ctx) * kFramesPerSecond;
        if (d.Has(kChSeekOffset)) {
            const f32 now = d.Channel(kChSeekOffset).EvalScalar(st.seed, kChSeekOffset, ctx);
            const Vector3f toTarget{(seekTarget_.x - sysPos.x) * inv,
                                    (seekTarget_.y - sysPos.y) * inv,
                                    (seekTarget_.z - sysPos.z) * inv};
            const f32 spread = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y +
                                         toTarget.z * toTarget.z);
            s = spread * (s + (now - st.prevSeek) * invDt);
            st.prevSeek = now;
        }
        disp = {disp.x + dir.x * s * dt, disp.y + dir.y * s * dt, disp.z + dir.z * s * dt};
    }

    // Types 2, 3 and 9 own a body in a shared world-collision solver we do not
    // have. Drifting them along their birth velocity is not what the engine
    // does — it is the honest degradation, and it keeps rain falling instead
    // of hanging in the air.
    if (OwnsSolverBody(d.systemType)) {
        disp = {disp.x + p.velocity.x * dt, disp.y + p.velocity.y * dt,
                disp.z + p.velocity.z * dt};
    }

    p.position = {p.position.x + disp.x * u, p.position.y + disp.y * u,
                  p.position.z + disp.z * u};

    // The same vector the engine adds to the position it also keeps as the
    // particle's axis, raw at pool+444 and normalised at pool+456 — and the
    // normalise is skipped, not zeroed, on a step too short to measure. In
    // `.prt` units, like the displacement itself: the orientation frame only
    // ever asks for a direction, and the epsilon is the engine's own.
    st.axis = disp;
    if (const f32 l2 = disp.x * disp.x + disp.y * disp.y + disp.z * disp.z;
        l2 > kFrameEpsilon) {
        const f32 invLen = 1.0f / std::sqrt(l2);
        st.axisUnit = {disp.x * invLen, disp.y * invLen, disp.z * invLen};
    }

    // Render modes 9 and 10 conform the quad to the ground under it. Gated on
    // the mode because it is a query per moved particle and 422 files want it.
    if (ConformsToGround(d.renderMode))
        RefreshGroundNormal(st, p.position);

    // ---- orientation -------------------------------------------------------
    if (d.Cap(kCapRoll)) {
        f32 w = 0.0f;
        if (d.Has(kChRollRate))
            w = d.Channel(kChRollRate).EvalScalar(st.seed, kChRollRate, ctx) * kFramesPerSecond;
        if (d.Has(kChRollAngle)) {
            const f32 now = d.Channel(kChRollAngle).EvalScalar(st.seed, kChRollAngle, ctx);
            w += (now - st.prevRoll) * invDt;
            st.prevRoll = now;
        }
        st.rollAngle = WrapAngle(st.rollAngle + w * dt);
    }

    if (d.Cap(kCapSpin)) {
        // Read whenever the channel exists. `kCapSpinAxis` means "the axis
        // is not constant", which is a re-evaluation hint — gating the READ on
        // it would leave every constant non-default axis at (0,1,0).
        if (d.Has(kChSpinAxis)) {
            st.spinAxis =
                Normalized(d.Channel(kChSpinAxis).EvalVector(st.seed, kChSpinAxis, ctx), {0, 1, 0});
        }
        f32 w = 0.0f;
        if (d.Has(kChSpinRate))
            w = d.Channel(kChSpinRate).EvalScalar(st.seed, kChSpinRate, ctx) * kFramesPerSecond;
        if (d.Has(kChSpinAngle)) {
            const f32 now = d.Channel(kChSpinAngle).EvalScalar(st.seed, kChSpinAngle, ctx);
            // prev MINUS now. Every other pair in the system is now - prev;
            // this one was re-checked at instruction level and really is
            // reversed. Reproduce it, do not repair it.
            w += (st.prevSpinAngle - now) * invDt;
            st.prevSpinAngle = now;
        }
        if (std::fabs(w) > kEpsilon) {
            st.orientation = st.orientation * Quaternion::from_axis_angle(st.spinAxis, w * dt);
            st.orientation.normalize();
        }
    }

    // ---- appearance --------------------------------------------------------
    if (d.Has(kChColor)) {
        const Vector4f c = d.Channel(kChColor).EvalColor(st.seed, kChColor, ctx);
        st.color = {c.x, c.y, c.z, c.w};
    }
    // ch6 is NOT this colour's alpha. `ParticleSystem_UpdateParticles` quantises
    // it and replicates the byte into all four lanes of a SECOND dword
    // (`MOV W9,#0x1010101` @0x71000BEF20 -> particle+0xEC), which
    // `Particle_PrepareDrawFrame` @0x71000BCDC0 forwards as the vertex's COLOR1
    // while COLOR0's own alpha byte is overwritten by the opacity. The only
    // program that reads COLOR1 is `Billboard.fx__ps_legacy`'s erosion tail,
    // `alpha = min(1, pow(alpha, 10 * COLOR1.a))`.
    st.dissolve = d.Has(kChAlpha)
                      ? std::clamp(d.Channel(kChAlpha).EvalScalar(st.seed, kChAlpha, ctx), 0.0f,
                                   1.0f)
                      : 1.0f;

    st.opacity = d.Has(kChScale) ? d.Channel(kChScale).EvalScalar(st.seed, kChScale, ctx) : 1.0f;

    f32 sizeCh = d.Has(kChSize) ? d.Channel(kChSize).EvalScalar(st.seed, kChSize, ctx) : 1.0f;
    f32 emitterSize = 1.0f;
    // The two EMITTER-wide terms, both sampled once per tick off the emitter's
    // own seed by `ParticleSystem_TickEmitter` (ch 34 -> sys+0x134 @0x71000AEF48,
    // ch 35 -> sys+0x12C @0x71000AEF64), both defaulting to 1.0 when the path is
    // absent, and they drive DIFFERENT things: `particle+0xD4 = ch1 * (birthSize
    // * sys+0x134)` @0x71000BEF7C is the quad's width, while `particle+0xD0 =
    // ch5 * sys+0x12C` @0x71000BEE28 is the opacity byte. Only the first is a
    // size.
    if (d.Has(kChSizeScale) || d.Has(kChEffectScale)) {
        const EvalCtx ectx = EmitterCtx();
        if (d.Has(kChSizeScale))
            emitterSize = d.Channel(kChSizeScale).EvalScalar(emitterSeed_, kChSizeScale, ectx);
        if (d.Has(kChEffectScale))
            st.opacity *=
                d.Channel(kChEffectScale).EvalScalar(emitterSeed_, kChEffectScale, ectx);
    }
    st.size = std::clamp(sizeCh * st.baseSize * emitterSize, kMinParticleSize, kMaxParticleSize);

    // ch2 -> `particle+0xF0`, default 1.0 @0x71000BEF98. Only the quad's HEIGHT
    // reads it; a channel named for a size that scales one axis is why this sat
    // parsed-but-unread until the frost weapons drew twice as tall as the blade.
    if (d.Has(kChHeightRatio))
        st.heightRatio = d.Channel(kChHeightRatio).EvalScalar(st.seed, kChHeightRatio, ctx);
}

// ---------------------------------------------------------------------------
// The wind spring — system types 6 and 8 only, and INSTEAD of everything above
// ---------------------------------------------------------------------------

void Emitter::StepWindSpring(f32 dt) {
    const EmitterDesc& d = *d3desc_;
    const WindSpringRig rig{d.swayFrequency, d.swayDamping, d.swayMaxOffset, d.swayGustAmount,
                            d.swayBaseAmount};
    for (usize i = 0; i < Pool().AliveCount(); ++i) {
        const u32 idx = Pool().AliveAt(i);
        d3::StepWindSpring(states_[idx], rig, {windDir_.x, windDir_.y}, windStrength_,
                           windPhase_, dt);
    }
}

// ---------------------------------------------------------------------------

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
    const Vector3f old = carryPos_;
    const Quaternion oldQ = carryQuat_;
    carryPos_ = now;
    carryQuat_ = emitterQuat_;

    if (d.systemType == SystemType::Weather || d.systemType == SystemType::Swarm ||
        d.systemType == SystemType::RibbonPhysics || d.systemType == SystemType::Ribbon ||
        !d.Has(PrtFlag::BirthAtEmitter))
        return;
    const Vector3f delta{old.x - now.x, old.y - now.y, old.z - now.z};
    const bool moved = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z != 0.0f;

    const bool sameQuat = emitterQuat_.x == oldQ.x && emitterQuat_.y == oldQ.y &&
                          emitterQuat_.z == oldQ.z && emitterQuat_.w == oldQ.w;
    if (d.Has(PrtFlag::CarryWithoutRotation) || sameQuat) {
        // The exact `!= 0.0` displacement guard belongs to THIS arm alone. The
        // rotating arm below runs on a turn in place, which is the whole point
        // of it — an emitter that spins without moving still sweeps its
        // particles round.
        if (!moved)
            return;
        for (usize i = 0; i < Pool().AliveCount(); ++i) {
            const u32 idx = Pool().AliveAt(i);
            Particle2& p = Pool()[idx];
            p.position = {now.x + (p.position.x - old.x), now.y + (p.position.y - old.y),
                          now.z + (p.position.z - old.z)};
            states_[idx].birthEmitterQuat = emitterQuat_;
        }
        return;
    }
    const Quaternion dq = emitterQuat_ * oldQ.conjugate();
    for (usize i = 0; i < Pool().AliveCount(); ++i) {
        const u32 idx = Pool().AliveAt(i);
        Particle2& p = Pool()[idx];
        const Vector3f rel{p.position.x - old.x, p.position.y - old.y, p.position.z - old.z};
        const Vector3f r = dq.rotate_vector(rel);
        p.position = {now.x + r.x, now.y + r.y, now.z + r.z};
        // The engine also rotates the particle's own axis at pool+456 — the
        // emitter-rotated world axis `Particle_InitLifeAndSize` writes at
        // birthRecord+152 and `Particle_BuildOrientationBasis` takes as its
        // fallback. This build carries no such field (`spinAxis` is a different
        // offset, birthRecord+172), so that half is recorded, not reproduced.
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
                EmitContext ec = BuildEmitContext();
                const i32 target =
                    d.Has(kChTargetCount)
                        ? d.Channel(kChTargetCount).EvalInt(emitterSeed_, kChTargetCount,
                                                            EmitterCtx())
                        : 0;
                const i32 n = std::clamp(target, 0, kMaxWindSpringPopulation);
                ec.emitCount = n;
                for (i32 i = 0; i < n; ++i) {
                    ec.emitIndex = i;
                    if (BirthParticle(elapsed, ec))
                        ++emittedLastUpdate_;
                }
            }
            StepWindSpring(elapsed);
        }
        return;
    }

    systemAge_ += elapsed;

    // Age, kill, then move. The kill test is the particle's own lifetime, plus
    // the system's kill radius (`flMaxDistance`), which is what fires the 3501
    // triggered event in the engine.
    const Vector3f sysPos = WorldPosition();
    // The radius is authored, the separation is in renderer units; square the
    // conversion into the threshold rather than the distance.
    const f32 killR2 = d.maxDistance * d.maxDistance * UnitScale() * UnitScale();
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
        StepParticle(idx, elapsed);
        ++i;
    }

    if (Visible() && !EmissionFinished())
        TickEmit(elapsed, emissionScaler);
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

i32 Emitter::BuildGeometry(const BuildGeometryInput& in, std::vector<Vertex>& out) const {
    if (!in.worldToView || Pool().AliveCount() == 0)
        return 0;

    const Matrix44f& view = *in.worldToView;
    // What `Particle_PrepareDrawFrame` hands the frame builder as its last
    // argument: `view+0x28C`, the same vector the particle draw list sorts on.
    const Vector3f camForward{-view.data[0][2], -view.data[1][2], -view.data[2][2]};
    const Vector3f sysPos = WorldPosition();

    const EmitterDesc& d = *d3desc_;
    const i32 before = static_cast<i32>(out.size());
    Vector3f normal{0.0f, 0.0f, 1.0f};

    // The flip-book, if this material has one. The tile SIZE is constant — the
    // engine reads it off frame 0 and applies it to every frame — and rides the
    // layer's UV transform in the constant buffer; only the tile ORIGIN varies
    // per particle, and that goes in the vertex, which is what `normal` is for
    // here. Nothing in the D3 particle program reads a normal (the family is
    // unlit; `Particle_DrawBatch` uploads no light constants for it), so the
    // three floats were already dead weight in this stream.
    // The four POSITIONAL uv sets, and the base RECTANGLE each one's coordinates
    // are built over. `Particle_WriteQuadVertices` takes that rectangle off
    // **stage 0's** sheet and no other's, and off the SHEET rather than off the
    // flip-book: it reads the frame table before it has looked at the entry's uv
    // mode at all, so a type-1 layer whose texture carries frames shapes the
    // quad even when its entry is mode 0 or 2, while a flip-book on any later
    // stage gets the unit square (and has to carry its own scale — see
    // D3UvXform::atlasScale). A set with no layer bakes the identity rectangle.
    struct UvSet {
        const MaterialLayer* layer = nullptr;
        f32 extU = 1.0f;
        f32 extV = 1.0f;
    };
    UvSet sets[MaterialDesc::kMaxLayers];
    for (u32 s = 0; s < MaterialDesc::kMaxLayers; ++s) {
        if (d.d3mat.setLayer[s] < 0)
            continue;
        sets[s].layer = &d.d3mat.layers[static_cast<usize>(d.d3mat.setLayer[s])];
        if (s == 0 && sets[s].layer->atlas) {
            const Vector2f tile = sets[s].layer->atlas->TileSize();
            sets[s].extU = tile.x;
            sets[s].extV = tile.y;
        }
    }
    // A non-square tile makes a non-square quad: the engine divides the frame's
    // V extent in PIXELS by its U extent in pixels and scales the quad's
    // vertical half-extent by it. Same sheet as the base rectangle — stage 0's.
    // Square tiles, which is most of the corpus, leave this at 1.
    f32 atlasAspect = 1.0f;
    if (sets[0].layer && sets[0].layer->atlas && sets[0].layer->atlas->width > 0 &&
        sets[0].layer->atlas->height > 0) {
        const f32 wpx = sets[0].extU * static_cast<f32>(sets[0].layer->atlas->width);
        const f32 hpx = sets[0].extV * static_cast<f32>(sets[0].layer->atlas->height);
        if (wpx > kEpsilon && hpx > kEpsilon)
            atlasAspect = hpx / wpx;
    }
    // Renderer units per `.prt` unit. Positions are already in renderer units;
    // the size channels and the wind spring's offset are not.
    const f32 u = UnitScale();

    // The quad's corner signs and its UVs, matching the WC3/WoW builder so the
    // two streams stay interchangeable at the dispatcher.
    static constexpr f32 kCorner[4][2] = {{-1, 1}, {-1, -1}, {1, 1}, {1, -1}};
    static constexpr f32 kUV[4][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};

    for (usize i = 0; i < Pool().AliveCount(); ++i) {
        const u32 idx = Pool().AliveAt(i);
        const Particle2& p = Pool()[idx];
        const ParticleState& st = states_[idx];

        const Vector3f pos = p.position;
        // The sway offset BENDS the quad: `Particle_WriteQuadVertices` adds it to
        // the two +v corners and leaves the two -v corners on the particle's own
        // position (G-D3P-12), so foliage leans from a base that stays put.
        // Translating the whole particle by it slides the plant across the ground.
        Vector3f sway{0, 0, 0};
        if (UsesWindSpring(d.systemType))
            sway = {st.swayOffset.x * u, st.swayOffset.y * u, 0.0f};

        // `Particle_BuildOrientationBasis`, read out as a right/up pair — see
        // `d3_orientation.h`. When a mode writes nothing (0 ungated, 1 and 8 —
        // 60.7% of the corpus) or the frame comes out degenerate, the engine
        // leaves the emitter's frozen quaternion standing and the GPU builds the
        // quad from THAT, not from the camera: `Particle_WriteQuadVertices`
        // carries a packed quaternion and no view (G-D3P-11/12), and the modes
        // that do want the camera fold `camForward` in themselves (6, 13,
        // 0-gated). So the fallback is the emitter frame's own right/up — a
        // ground effect like player_fogRipple (mode 1) then lies flat under an
        // upright emitter instead of standing up model-tall to face the viewer.
        Vector3f right = st.birthEmitterQuat.rotate_vector(Vector3f{1.0f, 0.0f, 0.0f});
        Vector3f up = st.birthEmitterQuat.rotate_vector(Vector3f{0.0f, 1.0f, 0.0f});
        if (QuadFrame frame; BuildQuadFrame(d.renderMode,
                                           {camForward,
                                            st.axis,
                                            st.axisUnit,
                                            {pos.x - sysPos.x, pos.y - sysPos.y,
                                             pos.z - sysPos.z},
                                            st.groundNormal,
                                            st.birthEmitterQuat},
                                           frame)) {
            right = frame.right;
            up = frame.up;
        }

        // Roll spins the quad in its own plane; the spin quaternion rotates the
        // plane itself.
        if (st.rollAngle != 0.0f) {
            const f32 c = std::cos(st.rollAngle), s = std::sin(st.rollAngle);
            const Vector3f r2{right.x * c + up.x * s, right.y * c + up.y * s,
                              right.z * c + up.z * s};
            const Vector3f u2{up.x * c - right.x * s, up.y * c - right.y * s,
                              up.z * c - right.z * s};
            right = r2;
            up = u2;
        }
        if (d.Cap(kCapSpin)) {
            right = st.orientation.rotate_vector(right);
            up = st.orientation.rotate_vector(up);
        }

        // Width is `particle+0xD4` ALONE — `Particle_PrepareDrawFrame` stores it
        // to drawDesc+4 @0x71000BCD60 and `Particle_WriteQuadVertices` halves it
        // @0x71000BC4C4. The opacity term never reaches either extent. The
        // height takes the same half-width through the sheet aspect and then
        // ch2: `v55 = (v11 * v23) * drawDesc[2]` @0x71000BC4E4.
        const f32 half = st.size * 0.5f * u;
        if (!(half > 0.0f))
            continue;
        const u8 op = D3OpacityByte(st.opacity);
        if (op == 0)
            continue;
        const f32 halfV = half * atlasAspect * st.heightRatio;

        // This particle's four texcoord transforms, one per set. Everything
        // that varies is read out of the particle's own UV state, which is the
        // whole reason these are baked per vertex rather than uploaded per
        // draw: the scroll phase and its rate were drawn at emit, the rotation
        // has been accumulating since, and each set walks its own flip-book.
        f32 aff[MaterialDesc::kMaxLayers][6];
        for (u32 s = 0; s < MaterialDesc::kMaxLayers; ++s) {
            f32(&a)[6] = aff[s];
            a[0] = 1.0f; a[1] = 0.0f; a[2] = 0.0f;
            a[3] = 0.0f; a[4] = 1.0f; a[5] = 0.0f;
            const MaterialLayer* L = sets[s].layer;
            if (!L)
                continue;
            if (L->uv.mode == ::whiteout::flakes::io::D3UvMode::Anim2D) {
                // `MatTex_BuildUvAffine2x3` case 3: a translation to frame k's
                // origin, plus frame ZERO's far corner as a scale when the entry
                // asks. Never frame k's corner — only the origin varies.
                if (L->atlas && !L->atlas->frames.empty()) {
                    const auto& frames = L->atlas->frames;
                    const i32 k = std::clamp(static_cast<i32>(st.uv[s].cursor), 0,
                                             static_cast<i32>(frames.size()) - 1);
                    a[2] = frames[static_cast<usize>(k)].x;
                    a[5] = frames[static_cast<usize>(k)].y;
                    if (L->uv.atlasScale) {
                        a[0] = frames[0].z;
                        a[4] = frames[0].w;
                    }
                }
            } else {
                ::whiteout::flakes::io::D3UvAffineAt(L->uv, st.uv[s].u, st.uv[s].v,
                                                     st.uv[s].rot, a);
            }
            // Fold the base rectangle in, so the quad's own coordinates stay the
            // unit square and the corner below is one multiply-add.
            a[0] *= sets[s].extU;
            a[3] *= sets[s].extU;
            a[1] *= sets[s].extV;
            a[4] *= sets[s].extV;
        }

        // Straight, not premultiplied: the blend factors come from the `.prt`'s
        // own RenderPass and 169 of the corpus's 223 particle passes already
        // source SrcAlpha, so folding alpha into the colour here would apply it
        // twice. Same convention the WC3/WoW builder uses.
        //
        // The alpha is the opacity byte ALONE. Whatever `arColorPath` authored
        // in its own alpha lane is overwritten @0x71000BCCAC, and ch6 rides
        // COLOR1 instead.
        Vector4f vcol = st.color;
        vcol.w = static_cast<f32>(op) * (1.0f / 255.0f);
        if (in.fogEnabled && in.fogSampler) {
            const ImVector fog = in.fogSampler(pos);
            const Vector4f f = fog.ToVec4();
            vcol = {vcol.x * f.x, vcol.y * f.y, vcol.z * f.z, vcol.w};
        }

        Vertex v[4];
        Vector2f tc[4][MaterialDesc::kMaxLayers];
        for (i32 c = 0; c < 4; ++c) {
            const f32 sx = kCorner[c][0] * half;
            const f32 sy = kCorner[c][1] * halfV;
            const Vector3f base = kCorner[c][1] > 0.0f ? Vector3f{pos.x + sway.x, pos.y + sway.y,
                                                                 pos.z + sway.z}
                                                       : pos;
            v[c].position = {base.x + right.x * sx + up.x * sy, base.y + right.y * sx + up.y * sy,
                             base.z + right.z * sx + up.z * sy};
            v[c].normal = normal;
            v[c].color = vcol;
            v[c].uv = {kUV[c][0], kUV[c][1]};
            for (u32 s = 0; s < MaterialDesc::kMaxLayers; ++s) {
                const f32* a = aff[s];
                tc[c][s] = {a[0] * kUV[c][0] + a[1] * kUV[c][1] + a[2],
                            a[3] * kUV[c][0] + a[4] * kUV[c][1] + a[5]};
            }
        }
        static constexpr i32 kWind[6] = {0, 1, 2, 3, 2, 1};
        for (const i32 c : kWind)
            out.push_back(v[c]);
        if (in.d3Uv01 && in.d3Uv23) {
            for (const i32 c : kWind) {
                in.d3Uv01->push_back({tc[c][0].x, tc[c][0].y, tc[c][1].x, tc[c][1].y});
                in.d3Uv23->push_back({tc[c][2].x, tc[c][2].y, tc[c][3].x, tc[c][3].y});
            }
        }
        if (in.d3Color1) {
            for (i32 c = 0; c < 6; ++c)
                in.d3Color1->push_back(st.dissolve);
        }
    }

    return static_cast<i32>(out.size()) - before;
}

} // namespace whiteout::flakes::renderer::particle::d3
