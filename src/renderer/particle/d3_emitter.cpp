#include "renderer/particle/d3_emitter.h"

#include "renderer/particle/particle_geometry.h"
#include "whiteout/flakes/model_types.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle::d3 {

namespace {

constexpr f32 kEpsilon = 1e-6f;
constexpr f32 kTwoPi = 6.28318530717958647692f;
/// The engine's own 8*pi clamp before it wraps an angle. Kept because the
/// clamp changes the result for a large angle, not just the speed.
constexpr f32 kAngleClamp = 50.265f;
/// `ParticleSystem_TickEmitter`'s hard ceiling on live particles.
constexpr i32 kMaxLiveParticles = 4096;
/// `dwPrtFlags` bit that ENABLES the 300 u/s clamp on distance emission.
constexpr u32 kFlagClampDistanceEmission = 0x10000000u;

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

/// Shortest-arc rotation taking +Z onto @p axis. This is what the orbit model
/// builds its plane from; `OrientationFromAxes(worldZ, axis)` in the engine.
Quaternion RotationFromZTo(const Vector3f& axis) {
    const Vector3f a = Normalized(axis, {0, 0, 1});
    const f32 d = a.z; // dot with (0,0,1)
    if (d > 1.0f - 1e-6f)
        return Quaternion::identity();
    if (d < -1.0f + 1e-6f)
        return Quaternion(1, 0, 0, 0); // pi about X
    const Vector3f c{-a.y, a.x, 0.0f}; // cross((0,0,1), a)
    const f32 s = std::sqrt((1.0f + d) * 2.0f);
    const f32 inv = 1.0f / s;
    Quaternion q(c.x * inv, c.y * inv, c.z * inv, s * 0.5f);
    q.normalize();
    return q;
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
/// use. Its negative branch returns the angle WRAPPED INTO [0, 2pi] rather
/// than in [-pi/2, 0]; sin and cos are unaffected, so this reproduces the
/// engine without reproducing the surprise.
f32 AsinFast(f32 x) {
    const f32 a = std::fmin(std::fabs(x), 1.0f);
    const f32 poly = ((a * -0.018729f + 0.074261f) * a - 0.21211f) * a + 1.5707f;
    const f32 r = 1.5708f - poly * std::sqrt(1.0f - a);
    return (x < 0.0f) ? -r : r;
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
    const f32 phi = rng.NextUnit() * kTwoPi;
    const f32 ce = std::cos(elev);
    return {std::sin(phi) * radius * ce, std::cos(phi) * radius * ce, std::sin(elev) * radius};
}

Vector3f SamplePointOnHemisphere(MwcRng& rng, f32 radius) {
    // The ONLY difference from the sphere: the elevation draw is [0,1) rather
    // than [-1,1), so the result never leaves the +Z half.
    const f32 elev = AsinFast(rng.NextUnit());
    const f32 phi = rng.NextUnit() * kTwoPi;
    const f32 ce = std::cos(elev);
    return {std::sin(phi) * radius * ce, std::cos(phi) * radius * ce, std::sin(elev) * radius};
}

Vector3f SamplePointOnCircleXY(MwcRng& rng, f32 radius) {
    const f32 phi = rng.NextUnit() * kTwoPi;
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
    if (systemType != 6 && systemType != 8 &&
        (live(kChRollRate, 0.0f) || live(kChRollAngle, 0.0f)))
        caps |= kCapRoll;
    if (systemType == 1 || systemType == 3 || systemType == 4)
        caps |= kCapRibbon;
}

// ---------------------------------------------------------------------------

Emitter::Emitter() : d3desc_(DefaultD3Desc()) {}

void Emitter::SetD3Desc(std::shared_ptr<const EmitterDesc> desc) {
    d3desc_ = desc ? std::move(desc) : DefaultD3Desc();

    // The base class still owns the draw list's material and priority, so give
    // it a desc carrying those and nothing else. Everything the WC3/WoW
    // simulation would read off it stays at its default and is never
    // consulted, because Update is overridden.
    auto base = std::make_shared<particle::EmitterDesc>();
    base->material = d3desc_->material;
    base->priorityPlane = d3desc_->priorityPlane;
    base->output = ParticleOutput::Billboard;
    base->hasHead = true;
    base->shape = std::make_shared<PlaneShape>();
    Emitter2::SetDesc(base);

    Restart();
}

void Emitter::Restart() {
    systemAge_ = 0.0f;
    emitElapsed_ = 0.0f;
    emitAccum_ = 0.0f;
    emittedLastUpdate_ = 0;
    Pool().Clear();
    // The lifetime randomiser is an InterpolationScalar, so with no driver it
    // resolves to 1.0 — which is every shipped asset in a viewer.
    lifetimeScale_ = 1.0f;
}

void Emitter::OnPoolResized(usize capacity) {
    states_.resize(capacity);
}

void Emitter::ApplyState(const model::FrameState::ParticleFrameState& st) {
    // D3 has no per-frame animated emitter parameters of the kind FrameState
    // carries: everything animated is a channel inside the `.prt`, sampled
    // against the system's own clock. So this takes only the placement and the
    // visibility and ignores the rest, rather than mapping fields that would
    // then fight the channels for the same quantity.
    SetVisible(st.visibility > 0.0f);
    SetModelToWorld(st.transform);
    SetWorldPosition(whiteout::transform_point({0, 0, 0}, st.transform));
}

bool Emitter::EmissionFinished() const {
    const EmitterDesc& d = *d3desc_;
    if (d.lifetime <= 0.0f)
        return false;
    return systemAge_ >= d.lifetime * lifetimeScale_;
}

EvalCtx Emitter::EmitterCtx() const {
    const EmitterDesc& d = *d3desc_;
    EvalCtx c;
    c.timeMode = 1;
    c.time = emitElapsed_;
    // A zero emission period would divide by zero in NormalisedTime; the
    // engine's own guard is the `period == 0` early-out, which yields t = 0.
    c.period = (d.emissionPeriod > 0.0f) ? d.emissionPeriod : 0.0f;
    return c;
}

EvalCtx Emitter::ParticleCtx(const ParticleState& st, const Vector3f& pos, f32 age) const {
    const EmitterDesc& d = *d3desc_;
    EvalCtx c;
    c.timeMode = 1;
    c.time = age;
    c.period = st.lifetime;

    // The two driver sources a viewer can actually supply. `flMaxDistance` and
    // `flCameraDistScale` do double duty here: they are the kill radius and
    // the camera placement scale, AND the normalising divisors for modes 3
    // and 6.
    const Vector3f sysPos = WorldPosition();
    const Vector3f rel{pos.x - sysPos.x, pos.y - sysPos.y, pos.z - sysPos.z};
    if (d.maxDistance > kEpsilon) {
        const f32 len = std::sqrt(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
        c.driver.distNorm = len / d.maxDistance;
    }
    if (std::fabs(d.cameraDistScale) > kEpsilon)
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
        d.shapeExtent0.ScalarEndpoints(c.ext0Lo, c.ext0Span);
        break;
    case Shape::Cylinder:
    case Shape::Ring:
        d.shapeExtent0.ScalarEndpoints(c.ext0Lo, c.ext0Span);
        d.shapeExtent1.ScalarEndpoints(c.ext1Lo, c.ext1Span);
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

Vector3f Emitter::SampleShape(EmitContext& ec, const Vector3f& base) {
    Vector3f local{0, 0, 0};

    switch (ec.shape) {
    case Shape::SphereShell: {
        const f32 r = SampleRadiusInAnnulus(sysRng_, ec.ext0Lo, ec.ext0Span);
        local = SamplePointOnSphere(sysRng_, r);
        break;
    }
    case Shape::HemisphereShell: {
        const f32 r = SampleRadiusInAnnulus(sysRng_, ec.ext0Lo, ec.ext0Span);
        local = SamplePointOnHemisphere(sysRng_, r);
        break;
    }
    case Shape::Cylinder: {
        const f32 r = SampleRadiusInAnnulus(sysRng_, ec.ext0Lo, ec.ext0Span);
        local = SamplePointOnCircleXY(sysRng_, r);
        local.z = ec.ext1Lo;
        if (ec.ext1Span != 0.0f)
            local.z += ec.ext1Span * sysRng_.NextUnit();
        break;
    }
    case Shape::Ring: {
        const f32 r = SampleRadiusInAnnulus(sysRng_, ec.ext0Lo, ec.ext0Span);
        // Not random: the azimuth is the emit index spread evenly across the
        // particles being emitted THIS TICK, starting from one random phase
        // drawn at index 0. n spokes, not n scattered points.
        f32 phi;
        if (ec.emitIndex != 0 && ec.emitCount > 0) {
            phi = ec.ringPhase +
                  (static_cast<f32>(ec.emitIndex) * kTwoPi) / static_cast<f32>(ec.emitCount);
            phi = WrapAngle(phi);
        } else {
            phi = sysRng_.NextUnit() * kTwoPi;
            ec.ringPhase = phi;
        }
        local = {std::cos(phi) * r, std::sin(phi) * r, 0.0f};
        local.z = ec.ext1Lo;
        if (ec.ext1Span != 0.0f)
            local.z += ec.ext1Span * sysRng_.NextUnit();
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
        return {base.x + v[0], base.y + v[1], base.z + v[2]};
    }
    case Shape::MeshRandom:
    case Shape::MeshActorKind4:
    case Shape::MeshSequential:
        ++emitSequence_;
        [[fallthrough]];
    case Shape::Point:
    default:
        break;
    }

    return {base.x + ec.offset.x + local.x, base.y + ec.offset.y + local.y,
            base.z + ec.offset.z + local.z};
}

bool Emitter::BirthParticle(f32 dt, EmitContext& ec) {
    const EmitterDesc& d = *d3desc_;

    if (Pool().DeadEmpty()) {
        const u32 want = static_cast<u32>(
            std::min<usize>(kMaxLiveParticles, std::max<usize>(64, Pool().Capacity() * 2)));
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
    st.seed = (raw >= 0xFFFFFFFEu) ? (raw + 2u) : raw;

    // Sub-frame emission interpolation: the base is a random point on the
    // segment the emitter travelled this frame, not its current position. One
    // draw, and it is what keeps a fast-moving emitter from stamping all of a
    // frame's particles at one spot.
    const Vector3f now = WorldPosition();
    Vector3f base = now;
    if (d.systemType == static_cast<i32>(SystemType::WorldAnchored)) {
        base = {0, 0, 0};
    } else {
        const Vector3f prev = prevWorldPos_;
        const f32 u = sysRng_.NextUnit();
        base = {prev.x + u * (now.x - prev.x), prev.y + u * (now.y - prev.y),
                prev.z + u * (now.z - prev.z)};
    }

    p.position = SampleShape(ec, base);
    p.velocity = {0, 0, 0};
    p.age = 0.0f;

    // Lifetime and base size, in the engine's order: an asset that asks for a
    // non-positive lifetime hands the slot straight back.
    const EvalCtx ectx = EmitterCtx();
    st.lifetime = 1.0f;
    if (d.Has(kChParticleLife)) {
        const f32 frames = d.Channel(kChParticleLife).EvalScalar(st.seed, kChParticleLife, ectx);
        st.lifetime = std::round(frames) * (1.0f / 60.0f);
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
        const f32 phi = sysRng_.NextUnit() * kTwoPi;
        st.orbitDir = {std::cos(phi), std::sin(phi)};
        st.orbitSeeded = true;
    }

    // Channel 38/39/40 build a birth velocity, but ONLY the three system types
    // that own a body in the world-collision solver ever consult it — an
    // ordinary particle moves purely by its channels. We compute it anyway and
    // park it in Particle2::velocity, because those three types have no solver
    // here and a straight-line drift is a better degradation than a freeze.
    if (d.Has(kChInitialVelocity) || d.Has(kChWorldVelocity)) {
        Vector3f v = d.Channel(kChInitialVelocity).EvalVector(st.seed, kChInitialVelocity, ectx);
        v = {v.x * 60.0f, v.y * 60.0f, v.z * 60.0f};
        if (d.Has(kChSpreadAngle)) {
            const f32 cone = d.Channel(kChSpreadAngle).EvalScalar(st.seed, kChSpreadAngle, ectx);
            if (std::fabs(cone) > kEpsilon) {
                const f32 azim = sysRng_.NextUnit() * kTwoPi;
                const Vector3f axis{std::cos(azim), std::sin(azim), 0.0f};
                v = Quaternion::from_axis_angle(axis, cone).rotate_vector(v);
            }
        }
        v = emitterQuat_.rotate_vector(v);
        if (d.Has(kChWorldVelocity)) {
            const Vector3f w =
                d.Channel(kChWorldVelocity).EvalVector(st.seed, kChWorldVelocity, ectx);
            v = {v.x + w.x * 60.0f, v.y + w.y * 60.0f, v.z + w.z * 60.0f};
        }
        p.velocity = v;
    }

    st.swayPhase = sysRng_.NextUnit();

    Pool().PushAlive(idx);
    OnParticleBorn(idx);

    // Newborns are advanced by the REMAINDER of the frame, not a whole one, so
    // a particle created mid-frame is not a frame behind. This is also what
    // gives frame 0 fully evaluated channels instead of zeros.
    StepParticle(idx, dt);
    return true;
}

void Emitter::TickEmit(f32 dt, f32 emissionScaler) {
    const EmitterDesc& d = *d3desc_;
    const EvalCtx ectx = EmitterCtx();

    // The three drivers, combined into one accumulator.
    f32 rate = 0.0f;
    if (d.Has(kChEmissionRate))
        rate = d.Channel(kChEmissionRate).EvalScalar(emitterSeed_, kChEmissionRate, ectx) * 60.0f;
    emitAccum_ += rate * dt * emissionScaler;

    if (d.Has(kChDistanceRate)) {
        const Vector3f now = WorldPosition();
        const Vector3f prev = prevWorldPos_;
        const Vector3f delta{now.x - prev.x, now.y - prev.y, now.z - prev.z};
        const f32 speed =
            (dt > 0.0f)
                ? std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z) / dt
                : 0.0f;
        const bool clamped =
            (speed >= 300.0f) && ((d.prtFlags & kFlagClampDistanceEmission) != 0);
        if (speed > kEpsilon && !clamped) {
            const f32 perUnit =
                d.Channel(kChDistanceRate).EvalScalar(emitterSeed_, kChDistanceRate, ectx) * 60.0f;
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

    const i32 alive = static_cast<i32>(Pool().AliveCount());
    if (d.Has(kChTargetCount)) {
        const f32 target = d.Channel(kChTargetCount).EvalScalar(emitterSeed_, kChTargetCount, ectx);
        n = std::max(n, static_cast<i32>(std::round(target)) - alive);
    }
    n = std::min(n, kMaxLiveParticles - alive);
    if (n <= 0)
        return;

    EmitContext ec = BuildEmitContext();
    ec.emitCount = n;
    for (i32 i = 0; i < n; ++i) {
        ec.emitIndex = i;
        if (BirthParticle(dt, ec))
            ++emittedLastUpdate_;
    }
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

    Vector3f disp{0, 0, 0};

    // ---- cylindrical orbit: axis (ch 10), radius (7), radial speed (8),
    //      angular speed (9) ------------------------------------------------
    if (d.Cap(kCapOrbit)) {
        const Vector3f axis =
            d.Has(kChOrbitAxis)
                ? Normalized(d.Channel(kChOrbitAxis).EvalVector(st.seed, kChOrbitAxis, ctx),
                             {0, 0, 1})
                : Vector3f{0, 0, 1};
        const Quaternion q = RotationFromZTo(axis);
        const Vector3f rel{p.position.x - sysPos.x, p.position.y - sysPos.y,
                           p.position.z - sysPos.z};
        const Vector3f local = q.inverse().rotate_vector(rel);

        const f32 planarLen = std::sqrt(local.x * local.x + local.y * local.y);
        if (planarLen > kEpsilon)
            st.orbitDir = {local.x / planarLen, local.y / planarLen};

        f32 radialSpeed = 0.0f;
        if (d.Has(kChOrbitRadSpeed))
            radialSpeed =
                d.Channel(kChOrbitRadSpeed).EvalScalar(st.seed, kChOrbitRadSpeed, ctx) * 60.0f;
        if (d.Has(kChOrbitRadius)) {
            const f32 now = d.Channel(kChOrbitRadius).EvalScalar(st.seed, kChOrbitRadius, ctx);
            radialSpeed += (now - st.prevOrbitRadius) * invDt;
            st.prevOrbitRadius = now;
        }
        f32 angular = 0.0f;
        if (d.Has(kChOrbitAngSpeed))
            angular =
                d.Channel(kChOrbitAngSpeed).EvalScalar(st.seed, kChOrbitAngSpeed, ctx) * 60.0f;

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
            speed = d.Channel(kChRadialSpeed).EvalScalar(st.seed, kChRadialSpeed, ctx) * 60.0f;
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
            v = {s.x * 60.0f, s.y * 60.0f, s.z * 60.0f};
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
            st.accelVelA = {st.accelVelA.x + a.x * 3600.0f * dt,
                            st.accelVelA.y + a.y * 3600.0f * dt,
                            st.accelVelA.z + a.z * 3600.0f * dt};
        }
        disp = {disp.x + (v.x + st.accelVelA.x) * dt, disp.y + (v.y + st.accelVelA.y) * dt,
                disp.z + (v.z + st.accelVelA.z) * dt};
    }

    // ---- kinematic triple B (20/21/22), EMITTER-LOCAL ----------------------
    if (d.Cap(kCapTripleB)) {
        Vector3f v{0, 0, 0};
        if (d.Has(kChVelocityB)) {
            const Vector3f s = d.Channel(kChVelocityB).EvalVector(st.seed, kChVelocityB, ctx);
            v = {s.x * 60.0f, s.y * 60.0f, s.z * 60.0f};
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
            st.accelVelB = {st.accelVelB.x + a.x * 3600.0f * dt,
                            st.accelVelB.y + a.y * 3600.0f * dt,
                            st.accelVelB.z + a.z * 3600.0f * dt};
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
            s = d.Channel(kChSeekSpeed).EvalScalar(st.seed, kChSeekSpeed, ctx) * 60.0f;
        if (d.Has(kChSeekOffset)) {
            const f32 now = d.Channel(kChSeekOffset).EvalScalar(st.seed, kChSeekOffset, ctx);
            const Vector3f toTarget{seekTarget_.x - sysPos.x, seekTarget_.y - sysPos.y,
                                    seekTarget_.z - sysPos.z};
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
    if ((d.systemType & ~1) == 2 || d.systemType == static_cast<i32>(SystemType::Weather)) {
        disp = {disp.x + p.velocity.x * dt, disp.y + p.velocity.y * dt,
                disp.z + p.velocity.z * dt};
    }

    p.position = {p.position.x + disp.x, p.position.y + disp.y, p.position.z + disp.z};

    // ---- orientation -------------------------------------------------------
    if (d.Cap(kCapRoll)) {
        f32 w = 0.0f;
        if (d.Has(kChRollRate))
            w = d.Channel(kChRollRate).EvalScalar(st.seed, kChRollRate, ctx) * 60.0f;
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
            w = d.Channel(kChSpinRate).EvalScalar(st.seed, kChSpinRate, ctx) * 60.0f;
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
        const Vector4f c = d.Channel(kChColor).Eval(st.seed, kChColor, ctx);
        st.color = {c.x, c.y, c.z, c.w};
    }
    f32 alpha = 1.0f;
    if (d.Has(kChAlpha))
        alpha = std::clamp(d.Channel(kChAlpha).EvalScalar(st.seed, kChAlpha, ctx), 0.0f, 1.0f);
    // The engine keeps the alpha as a separate dword replicated into all four
    // bytes (particle+236), i.e. it modulates every channel, not just alpha.
    st.color.w = alpha;

    st.scale = d.Has(kChScale) ? d.Channel(kChScale).EvalScalar(st.seed, kChScale, ctx) : 1.0f;

    f32 sizeCh = d.Has(kChSize) ? d.Channel(kChSize).EvalScalar(st.seed, kChSize, ctx) : 1.0f;
    f32 emitterSize = 1.0f;
    if (d.Has(kChSizeScale)) {
        const EvalCtx ectx = EmitterCtx();
        emitterSize = d.Channel(kChSizeScale).EvalScalar(emitterSeed_, kChSizeScale, ectx);
    }
    st.size = std::clamp(sizeCh * st.baseSize * emitterSize, 0.0001f, 999.0f);

    if (d.Has(kChChildScalar))
        st.childScalar = d.Channel(kChChildScalar).EvalScalar(st.seed, kChChildScalar, ctx);
}

// ---------------------------------------------------------------------------
// The wind spring — system types 6 and 8 only, and INSTEAD of everything above
// ---------------------------------------------------------------------------

void Emitter::StepWindSpring(f32 dt) {
    const EmitterDesc& d = *d3desc_;
    const f32 w = d.swayFrequency * kTwoPi;
    const f32 damp = w * (d.swayDamping + d.swayDamping);
    const f32 k = w * w;

    for (usize i = 0; i < Pool().AliveCount(); ++i) {
        const u32 idx = Pool().AliveAt(i);
        ParticleState& st = states_[idx];

        const f32 gust =
            d.swayBaseAmount -
            d.swayGustAmount * std::cos(st.swayPhase * kTwoPi + windPhase_) * windStrength_;
        st.swayForce = {windDir_.x * gust * (1.0f / 60.0f), windDir_.y * gust * (1.0f / 60.0f)};

        const f32 ax = -damp * st.swayVelocity.x - k * st.swayOffset.x + st.swayForce.x;
        const f32 ay = -damp * st.swayVelocity.y - k * st.swayOffset.y + st.swayForce.y;
        st.swayVelocity = {st.swayVelocity.x + ax * dt, st.swayVelocity.y + ay * dt};
        st.swayOffset = {st.swayOffset.x + st.swayVelocity.x * dt,
                         st.swayOffset.y + st.swayVelocity.y * dt};

        const f32 lim = d.swayMaxOffset * st.size;
        const f32 l2 = st.swayOffset.x * st.swayOffset.x + st.swayOffset.y * st.swayOffset.y;
        if (l2 > lim * lim) {
            const f32 l = std::sqrt(l2);
            if (l > kEpsilon) {
                const f32 s = (lim * 0.95f) / l;
                st.swayOffset = {st.swayOffset.x * s, st.swayOffset.y * s};
            }
            st.swayVelocity = {0, 0};
        }
    }
}

// ---------------------------------------------------------------------------

void Emitter::Update(f32 elapsed, f32 emissionScaler) {
    const EmitterDesc& d = *d3desc_;
    emittedLastUpdate_ = 0;
    if (elapsed <= 0.0f)
        return;

    // Types 4, 5 and 7 — light shafts and static ground clutter. The engine
    // returns before doing anything at all, so they are one static frame.
    if (!SimulatesParticles(d.systemType)) {
        if (UsesWindSpring(d.systemType)) {
            // Foliage and clutter. Emit once so there is something to sway,
            // then run only the spring: no channels, no emission accumulator,
            // no motion models.
            if (Pool().AliveCount() == 0 && Visible()) {
                EmitContext ec = BuildEmitContext();
                const f32 target =
                    d.Has(kChTargetCount)
                        ? d.Channel(kChTargetCount).EvalScalar(emitterSeed_, kChTargetCount,
                                                               EmitterCtx())
                        : 0.0f;
                const i32 n = std::clamp(static_cast<i32>(std::round(target)), 0, 256);
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
    const f32 killR2 = d.maxDistance * d.maxDistance;
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
            OnParticleDied(idx);
            Pool().RemoveAliveAt(i);
            Pool().PushDead(idx);
            continue;
        }
        StepParticle(idx, elapsed);
        ++i;
    }

    if (Visible() && !EmissionFinished()) {
        emitElapsed_ += elapsed;
        TickEmit(elapsed, emissionScaler);
    }
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

i32 Emitter::BuildGeometry(const BuildGeometryInput& in, std::vector<Vertex>& out) const {
    if (!in.worldToView || Pool().AliveCount() == 0)
        return 0;

    const Matrix44f& view = *in.worldToView;
    const Vector3f camRight{view.data[0][0], view.data[1][0], view.data[2][0]};
    const Vector3f camUp{view.data[0][1], view.data[1][1], view.data[2][1]};

    const EmitterDesc& d = *d3desc_;
    const i32 before = static_cast<i32>(out.size());
    const Vector3f normal{0.0f, 0.0f, 1.0f};

    // The quad's corner signs and its UVs, matching the WC3/WoW builder so the
    // two streams stay interchangeable at the dispatcher.
    static constexpr f32 kCorner[4][2] = {{-1, 1}, {-1, -1}, {1, 1}, {1, -1}};
    static constexpr f32 kUV[4][2] = {{0, 0}, {0, 1}, {1, 0}, {1, 1}};

    for (usize i = 0; i < Pool().AliveCount(); ++i) {
        const u32 idx = Pool().AliveAt(i);
        const Particle2& p = Pool()[idx];
        const ParticleState& st = states_[idx];

        Vector3f pos = p.position;
        if (UsesWindSpring(d.systemType)) {
            pos.x += st.swayOffset.x;
            pos.y += st.swayOffset.y;
        }

        // Render mode 13 flattens the frame onto XY (a ground quad) and modes
        // 1 and 8 leave the caller's frame alone; every other mode is a
        // camera-facing billboard here. The full fourteen-case basis needs the
        // per-view work `Particle_BuildOrientationBasis` does and lands with
        // the render-mode phase — this is the two cases that visibly differ.
        Vector3f right = camRight;
        Vector3f up = camUp;
        if (d.renderMode == 13) {
            right = {1, 0, 0};
            up = {0, 1, 0};
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

        const f32 half = st.size * st.scale * 0.5f;
        if (!(half > 0.0f))
            continue;

        Vector4f vcol{st.color.x * st.color.w, st.color.y * st.color.w, st.color.z * st.color.w,
                      st.color.w};
        if (in.fogEnabled && in.fogSampler) {
            const ImVector fog = in.fogSampler(pos);
            const Vector4f f = fog.ToVec4();
            vcol = {vcol.x * f.x, vcol.y * f.y, vcol.z * f.z, vcol.w};
        }

        Vertex v[4];
        for (i32 c = 0; c < 4; ++c) {
            const f32 sx = kCorner[c][0] * half;
            const f32 sy = kCorner[c][1] * half;
            v[c].position = {pos.x + right.x * sx + up.x * sy, pos.y + right.y * sx + up.y * sy,
                             pos.z + right.z * sx + up.z * sy};
            v[c].normal = normal;
            v[c].color = vcol;
            v[c].uv = {kUV[c][0], kUV[c][1]};
        }
        out.push_back(v[0]);
        out.push_back(v[1]);
        out.push_back(v[2]);
        out.push_back(v[3]);
        out.push_back(v[2]);
        out.push_back(v[1]);
    }

    return static_cast<i32>(out.size()) - before;
}

} // namespace whiteout::flakes::renderer::particle::d3
