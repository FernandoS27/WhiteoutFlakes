#include "renderer/particle/d3_emitter.h"

#include "renderer/particle/d3_channel_sampler.h"
#include "renderer/particle/d3_emitter_math.h"
#include "renderer/particle/d3_orientation.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle::d3 {

namespace {

using detail::WrapAngle;

Vector3f Normalized(const Vector3f& v, const Vector3f& fallback) {
    const f32 l2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (l2 <= kEpsilon * kEpsilon)
        return fallback;
    const f32 inv = 1.0f / std::sqrt(l2);
    return {v.x * inv, v.y * inv, v.z * inv};
}

} // namespace

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

// ---------------------------------------------------------------------------
// Per-particle step
//
// Every motion model below works in `.prt` units — every speed, offset and
// acceleration is a number straight out of the file, and the two models that
// read the particle's own position (orbit, seek) divide it in on the way. Each
// returns its displacement for the step, and `StepParticle` adds them in the
// engine's order: a float sum does not reorder.
// ---------------------------------------------------------------------------

namespace {

/// Cylindrical orbit: axis (ch 10), radius (7), radial speed (8), angular
/// speed (9).
Vector3f OrbitStep(const ChannelSampler& ch, const Vector3f& position, ParticleState& st,
                   const EmitterFrame& f, f32 dt, f32 invDt) {
    const Vector3f& sysPos = f.sysPos;
    const f32 inv = f.invUnit;
    const Vector3f axis = ch.Has(kChOrbitAxis)
                              ? Normalized(ch.EvalVector(kChOrbitAxis), {0, 0, 1})
                              : Vector3f{0, 0, 1};
    const Quaternion q = d3::OrientationFromAxes({0, 0, 1}, axis);
    const Vector3f rel{(position.x - sysPos.x) * inv, (position.y - sysPos.y) * inv,
                       (position.z - sysPos.z) * inv};
    const Vector3f local = q.inverse().rotate_vector(rel);

    const f32 planarLen = std::sqrt(local.x * local.x + local.y * local.y);
    if (planarLen > kEpsilon)
        st.orbitDir = {local.x / planarLen, local.y / planarLen};

    const f32 radialSpeed =
        ch.Differentiated(kChOrbitRadSpeed, kChOrbitRadius, st.prevOrbitRadius, invDt);
    const f32 angular = ch.PerSecond(kChOrbitAngSpeed);

    const f32 a = angular * dt;
    const f32 ca = std::cos(a), sa = std::sin(a);
    const Vector3f rotated{local.x * ca - local.y * sa, local.x * sa + local.y * ca, 0.0f};
    const Vector3f planar{rotated.x - local.x + st.orbitDir.x * radialSpeed * dt,
                          rotated.y - local.y + st.orbitDir.y * radialSpeed * dt, 0.0f};
    return q.rotate_vector(planar);
}

/// Radial push away from the system origin (ch 11 offset, 12 speed).
Vector3f RadialStep(const ChannelSampler& ch, const Vector3f& position, ParticleState& st,
                    const Vector3f& sysPos, f32 dt, f32 invDt) {
    const Vector3f rel{position.x - sysPos.x, position.y - sysPos.y, position.z - sysPos.z};
    const f32 len = std::sqrt(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
    if (len > kEpsilon)
        st.radialDir = {rel.x / len, rel.y / len, rel.z / len};

    const f32 speed = ch.Differentiated(kChRadialSpeed, kChRadialOffset, st.prevRadial, invDt);
    return {st.radialDir.x * speed * dt, st.radialDir.y * speed * dt, st.radialDir.z * speed * dt};
}

/// One kinematic triple's displacement this step, `(v + a)·dt`: `v` the velocity
/// channel ×60 plus the offset channel differentiated, `a` the acceleration
/// channel's running integral. Triples A (17/18/19) and B (20/21/22) are this
/// one block, and differ only in the frame the result is applied in.
Vector3f TripleStep(const ChannelSampler& ch, i32 velocity, i32 offset, i32 accel,
                    Vector3f& prevOffset, Vector3f& accelVel, f32 dt, f32 invDt) {
    Vector3f v{0, 0, 0};
    if (ch.Has(velocity)) {
        const Vector3f s = ch.EvalVector(velocity);
        v = {s.x * kFramesPerSecond, s.y * kFramesPerSecond, s.z * kFramesPerSecond};
    }
    if (ch.Has(offset)) {
        const Vector3f now = ch.EvalVector(offset);
        v = {v.x + (now.x - prevOffset.x) * invDt, v.y + (now.y - prevOffset.y) * invDt,
             v.z + (now.z - prevOffset.z) * invDt};
        prevOffset = now;
    }
    if (ch.Has(accel)) {
        const Vector3f a = ch.EvalVector(accel);
        accelVel = {accelVel.x + a.x * kFramesPerSecondSq * dt,
                    accelVel.y + a.y * kFramesPerSecondSq * dt,
                    accelVel.z + a.z * kFramesPerSecondSq * dt};
    }
    return {(v.x + accelVel.x) * dt, (v.y + accelVel.y) * dt, (v.z + accelVel.z) * dt};
}

/// Target seek (ch 13 speed, 14 offset), toward @p target.
Vector3f SeekStep(const ChannelSampler& ch, const Vector3f& position, ParticleState& st,
                  const Vector3f& target, const EmitterFrame& f, f32 dt, f32 invDt) {
    const Vector3f& sysPos = f.sysPos;
    const f32 inv = f.invUnit;
    Vector3f dir{target.x - position.x, target.y - position.y, target.z - position.z};
    const f32 len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len > kEpsilon)
        dir = {dir.x / len, dir.y / len, dir.z / len};

    // Not `Differentiated`: the offset's term is scaled by the spread to
    // the target, and the rate rides inside that scale.
    f32 s = ch.PerSecond(kChSeekSpeed);
    if (ch.Has(kChSeekOffset)) {
        const f32 now = ch.EvalScalar(kChSeekOffset);
        const Vector3f toTarget{(target.x - sysPos.x) * inv, (target.y - sysPos.y) * inv,
                                (target.z - sysPos.z) * inv};
        const f32 spread = std::sqrt(toTarget.x * toTarget.x + toTarget.y * toTarget.y +
                                     toTarget.z * toTarget.z);
        s = spread * (s + (now - st.prevSeek) * invDt);
        st.prevSeek = now;
    }
    return {dir.x * s * dt, dir.y * s * dt, dir.z * s * dt};
}

/// Roll in the quad's own plane, and the spin of the plane itself.
void StepOrientation(const EmitterDesc& d, ParticleState& st, const ChannelSampler& ch, f32 dt,
                     f32 invDt) {
    if (d.Cap(kCapRoll)) {
        const f32 w = ch.Differentiated(kChRollRate, kChRollAngle, st.prevRoll, invDt);
        st.rollAngle = WrapAngle(st.rollAngle + w * dt);
    }

    if (d.Cap(kCapSpin)) {
        // Channel 23 is read here only when it is authored AND varies. A
        // constant one was adopted at birth, and an unauthored one leaves the
        // unit-sphere axis the birth drew. A degenerate sample keeps the last
        // axis.
        if (d.Cap(kCapSpinAxis) && !d.spinAxisConstant) {
            const Vector3f a = ch.EvalVector(kChSpinAxis);
            const f32 len = std::sqrt((a.x * a.x + a.y * a.y) + a.z * a.z);
            if (len > kEpsilon) {
                const f32 inv = 1.0f / len;
                st.spinAxis = {a.x * inv, a.y * inv, a.z * inv};
            }
        }
        // prev MINUS now: see `ChannelSampler::Differentiated`.
        const f32 w =
            ch.Differentiated(kChSpinRate, kChSpinAngle, st.prevSpinAngle, invDt, /*reversed=*/true);
        if (std::fabs(w) > kEpsilon) {
            st.orientation = st.orientation * Quaternion::from_axis_angle(st.spinAxis, w * dt);
            st.orientation.normalize();
        }
    }
}

/// Colour, dissolve, opacity, size and height ratio for this step.
void StepAppearance(ParticleState& st, const ChannelSampler& ch, const EmitterFrame& f) {
    if (ch.Has(kChColor)) {
        const Vector4f c = ch.EvalColor(kChColor);
        st.color = {c.x, c.y, c.z, c.w};
    }
    // ch6 is NOT this colour's alpha. `ParticleSystem_UpdateParticles` quantises
    // it and replicates the byte into all four lanes of a SECOND dword
    // (`MOV W9,#0x1010101` @0x71000BEF20 -> particle+0xEC), which
    // `Particle_PrepareDrawFrame` @0x71000BCDC0 forwards as the vertex's COLOR1
    // while COLOR0's own alpha byte is overwritten by the opacity. The only
    // program that reads COLOR1 is `Billboard.fx__ps_legacy`'s erosion tail,
    // `alpha = min(1, pow(alpha, 10 * COLOR1.a))`.
    st.dissolve = ch.Has(kChAlpha) ? std::clamp(ch.EvalScalar(kChAlpha), 0.0f, 1.0f) : 1.0f;

    st.opacity = ch.Scalar(kChScale, 1.0f);

    const f32 sizeCh = ch.Scalar(kChSize, 1.0f);
    // The two EMITTER-wide terms, sampled once this tick by `BuildEmitterFrame`:
    // ch 34 sizes the quad, ch 35 attenuates the opacity.
    if (f.hasEffectScale)
        st.opacity *= f.effectScale;
    st.size = std::clamp(sizeCh * st.baseSize * f.sizeScale, kMinParticleSize, kMaxParticleSize);

    // ch2 -> `particle+0xF0`, default 1.0 @0x71000BEF98. Only the quad's HEIGHT
    // reads it; a channel named for a size that scales one axis is why this sat
    // parsed-but-unread until the frost weapons drew twice as tall as the blade.
    if (ch.Has(kChHeightRatio))
        st.heightRatio = ch.EvalScalar(kChHeightRatio);
}

} // namespace

void Emitter::StepParticle(u32 idx, f32 dt, const EmitterFrame& f) {
    const EmitterDesc& d = *d3desc_;
    Particle2& p = Pool()[idx];
    ParticleState& st = states_[idx];

    const EvalCtx ctx = ParticleCtx(st, p.position, p.age, f);
    const ChannelSampler ch{d, st.seed, ctx};
    const f32 invDt = (dt > kEpsilon) ? (1.0f / dt) : 0.0f;

    // The four UV states run on the same tick as the motion, ahead of it, the
    // way ParticleSystem_ForEachParticle steps all four before it calls
    // ParticleSystem_UpdateParticles on the survivor.
    StepUvStates(st, dt);

    // The motion models, in the engine's order. `disp` is in `.prt` units and
    // is converted once, below, which is the only place renderer units appear.
    Vector3f disp{0, 0, 0};
    if (d.Cap(kCapOrbit)) {
        const Vector3f w = OrbitStep(ch, p.position, st, f, dt, invDt);
        disp = {disp.x + w.x, disp.y + w.y, disp.z + w.z};
    }
    if (d.Cap(kCapRadial)) {
        const Vector3f r = RadialStep(ch, p.position, st, f.sysPos, dt, invDt);
        disp = {disp.x + r.x, disp.y + r.y, disp.z + r.z};
    }
    // Kinematic triple A, WORLD space.
    if (d.Cap(kCapTripleA)) {
        const Vector3f step = TripleStep(ch, kChVelocityA, kChOffsetA, kChAccelA,
                                         st.prevOffsetA, st.accelVelA, dt, invDt);
        disp = {disp.x + step.x, disp.y + step.y, disp.z + step.z};
    }
    // Kinematic triple B, EMITTER-LOCAL. The PER-FRAME delta is what gets
    // rotated and applied; the running total the engine also keeps is not what
    // drives motion.
    if (d.Cap(kCapTripleB)) {
        const Vector3f local = TripleStep(ch, kChVelocityB, kChOffsetB, kChAccelB,
                                          st.prevOffsetB, st.accelVelB, dt, invDt);
        const Vector3f world = st.birthEmitterQuat.rotate_vector(local);
        disp = {disp.x + world.x, disp.y + world.y, disp.z + world.z};
    }
    if (d.Cap(kCapSeek) && hasSeekTarget_) {
        const Vector3f s = SeekStep(ch, p.position, st, seekTarget_, f, dt, invDt);
        disp = {disp.x + s.x, disp.y + s.y, disp.z + s.z};
    }
    // Types 2, 3 and 9 own a body in a shared world-collision solver we do not
    // have. Drifting them along their birth velocity is not what the engine
    // does — it is the honest degradation, and it keeps rain falling instead
    // of hanging in the air.
    if (OwnsSolverBody(d.systemType)) {
        disp = {disp.x + p.velocity.x * dt, disp.y + p.velocity.y * dt,
                disp.z + p.velocity.z * dt};
    }

    const f32 u = f.unit;
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

    StepOrientation(d, st, ch, dt, invDt);
    StepAppearance(st, ch, f);
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

} // namespace whiteout::flakes::renderer::particle::d3
