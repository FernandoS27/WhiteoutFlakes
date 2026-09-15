#pragma once

// ============================================================================
// D3ParticleState — the live per-particle state a D3 particle needs beyond the
// 32-byte `Particle2`.
//
// `Particle2` is frozen at 32 bytes because it mirrors the engine's
// `CParticle2` for the WC3/WoW dialect, so this rides alongside the pool as a
// parallel array, the way `MultiTexState` does. Position, velocity and age
// stay in `Particle2`; everything below is D3's.
//
// The five `prev*` fields are what make the differentiation idiom in
// D3_PARTICLE_DESIGN.md §5 work: every `(offset, rate)` channel pair combines
// as `rate*60 + (now - prev)/dt`, so an authored offset curve produces
// momentum rather than a teleport.
// ============================================================================

#include "d3_channels.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <array>
#include <cmath>

namespace whiteout::flakes::renderer::particle::d3 {

struct ParticleState {
    /// particle+4. Every `Eval` on this particle derives its random stream
    /// from `(channelId * seed, 666)`, so this must never change after birth.
    u32 seed = 0;

    f32 lifetime = 1.0f; ///< particle+40, seconds (ch 29)
    f32 baseSize = 1.0f; ///< particle+24 (ch 28)

    Quaternion orientation = Quaternion::identity();      ///< particle+216
    Quaternion birthEmitterQuat = Quaternion::identity(); ///< particle+184, triple B's frame

    /// pool+444 and pool+456 — the frame's displacement and the last non-zero
    /// direction it had. `ParticleSystem_UpdateParticles` @0x71000BEB00 stores
    /// the raw vector every frame and the unit one only when the step cleared
    /// the frame epsilon, so a particle that has stopped keeps pointing where it
    /// was going. Read by the orientation frame and by nothing else — this is a
    /// particle's "axis", and it is its direction of motion.
    Vector3f axis{0, 0, 0};
    Vector3f axisUnit{0, 0, 0};

    /// pool+560, and the XY it was sampled at (pool+548/552). Only render modes
    /// 9 and 10 read it; the engine re-casts only when the particle's XY has
    /// moved, so a stationary one samples once and a still system costs nothing.
    Vector3f groundNormal{0, 0, 1};
    Vector2f groundAt{0, 0};
    bool groundSeeded = false;

    Vector3f spinAxis{0, 1, 0}; ///< particle+172, normalised ch 23
    Vector3f radialDir{0, 0, 0}; ///< particle+48, cached at first use
    Vector2f orbitDir{1, 0};     ///< particle+60, seeded (cos phi, sin phi) at birth

    // Kinematic triple A — world space. `prevOffsetA` is the differentiation
    // cache; `accelVelA` is the running integral of the acceleration channel.
    Vector3f prevOffsetA{0, 0, 0}; ///< particle+76
    Vector3f accelVelA{0, 0, 0};   ///< particle+88

    // Kinematic triple B — emitter-local. Same shape, plus the running total
    // of the local displacement the engine keeps at particle+124 (which it
    // does NOT use for motion; the per-frame delta is what gets rotated).
    Vector3f prevOffsetB{0, 0, 0}; ///< particle+100
    Vector3f accelVelB{0, 0, 0};   ///< particle+112
    Vector3f localDispB{0, 0, 0};  ///< particle+124

    f32 prevOrbitRadius = 0.0f; ///< particle+68  (ch 7)
    f32 prevRadial = 0.0f;      ///< particle+72  (ch 11)
    f32 prevSeek = 0.0f;        ///< particle+136 (ch 14)
    f32 prevSpinAngle = 0.0f;   ///< particle+164 (ch 16)
    f32 prevRoll = 0.0f;        ///< particle+200 (ch 24)

    f32 rollAngle = 0.0f; ///< particle+204, wrapped into [0, 2pi]

    // Per-frame appearance, refreshed by the step and read by the builder.
    f32 size = 1.0f;   ///< particle+212, clamped to [1e-4, 999]
    /// particle+0xD0 — ch5 x ch35. An OPACITY, not a size: see @ref D3OpacityByte.
    f32 opacity = 1.0f;
    /// particle+0xF0 — ch2. The quad's HEIGHT ratio: `Particle_WriteQuadVertices`
    /// @0x71000BC4E4 builds the vertical half-extent as `aspect * halfWidth * this`
    /// and the horizontal one without it.
    f32 heightRatio = 1.0f;
    /// particle+232 (ch3). The w is the authored colour's own alpha and the
    /// builder discards it: the engine overwrites that byte with the opacity.
    Vector4f color{1, 1, 1, 1};
    /// particle+236 — ch6, the whole of COLOR1. Named for its only consumer,
    /// `ps_legacy`'s erosion tail; 21,328 of 21,593 files leave it at 1.0.
    f32 dissolve = 1.0f;

    // Wind spring (system types 6 and 8). Unused by every other type, and
    // eleven floats is cheap next to giving foliage its own emitter class.
    Vector2f swayOffset{0, 0};
    Vector2f swayVelocity{0, 0};
    Vector2f swayForce{0, 0};
    f32 swayPhase = 0.0f;

    /// @brief The four UV animation states, one per texture stage.
    ///
    /// `particle+16` in the engine is an array of FOUR 72-byte states, indexed
    /// by the POSITIONAL uv set (0 type 1, 1 type 19, 2 type 12, 3 type 14) with
    /// a hole where a type is absent. `ParticleSystem_EmitParticle` seeds each
    /// through `MatTex_InitUvState` at birth and `MatTex_TickUvStateEntry`
    /// advances each per frame, so **every scalar here belongs to the particle
    /// and not to the emitter**. That is not a nicety: 12,361 of the corpus's
    /// 13,897 uv mode 2 entries draw their initial U/V phase at RANDOM and 9,235
    /// carry a per-instance rate jitter, so a system evaluated on one shared
    /// clock draws every particle of a puff on the identical tile of its sheet.
    ///
    /// Six scalars for the scroll and rotation (`state+4/+8/+12/+16/+20/+24`)
    /// and two for the flip-book player at `state+32` (`+4` the cursor, `+20`
    /// the rate; the integer frame is `(int)cursor` and the length is the
    /// material's).
    struct UvState {
        f32 u = 0.0f, v = 0.0f;
        f32 uRate = 0.0f, vRate = 0.0f;
        f32 rot = 0.0f, rotRate = 0.0f;
        f32 cursor = 0.0f, cursorRate = 0.0f;
    };
    std::array<UvState, 4> uv{};

    bool orbitSeeded = false;
    bool radialSeeded = false;
};

/// The five `.prt` fields the wind spring reads, at SNO+176..192.
struct WindSpringRig {
    f32 frequency = 1.0f;
    f32 damping = 0.3f;
    f32 maxOffset = 1.0f;
    f32 gustAmount = 1.25f;
    f32 baseAmount = 0.0f;
};

/// One step of `ParticleSystem_StepWindSpring` @0x71000BD610, per particle.
///
/// A damped harmonic oscillator in XY, verified bit-exact by G-D3P-14. Two things
/// about it are easy to get wrong and both were:
///
/// * the forcing term enters the velocity WHOLE. Only the spring term is scaled by
///   `dt`. The force already carries a fixed 1/60, so the drive is frame-rate
///   independent while the integration is not — scaling it by `dt` as well leaves
///   the sway about sixty times too weak at 60 fps;
/// * the result is a quad BEND, not a translation. `Particle_WriteQuadVertices`
///   adds `swayOffset` to the quad's two +v corners only.
///
/// `reuseForce` is the engine's second arm, selected by the float at system+624: it
/// integrates with the force stored on the particle instead of recomputing it.
inline void StepWindSpring(ParticleState& st, const WindSpringRig& rig,
                           const Vector2f& windDir, f32 windStrength, f32 windPhase, f32 dt,
                           bool reuseForce = false) {
    /// Where an over-extended sway parks: this fraction of the limit, at rest.
    constexpr f32 kParkFraction = 0.95f;

    const f32 w = rig.frequency * kTwoPi;
    const f32 damp = w * (rig.damping + rig.damping);
    const f32 k = w * w;

    if (!reuseForce) {
        const f32 gust = rig.baseAmount -
                         rig.gustAmount * (std::cos(st.swayPhase * kTwoPi + windPhase) *
                                           windStrength);
        st.swayForce = {windDir.x * gust * kFrameSeconds, windDir.y * gust * kFrameSeconds};
    }

    st.swayVelocity = {
        st.swayVelocity.x +
            (st.swayForce.x + (st.swayVelocity.x * -damp - k * st.swayOffset.x) * dt),
        st.swayVelocity.y +
            (st.swayForce.y + (st.swayVelocity.y * -damp - k * st.swayOffset.y) * dt)};
    st.swayOffset = {st.swayOffset.x + st.swayVelocity.x * dt,
                     st.swayOffset.y + st.swayVelocity.y * dt};

    const f32 lim = rig.maxOffset * st.size;
    const f32 l2 = st.swayOffset.x * st.swayOffset.x + st.swayOffset.y * st.swayOffset.y;
    if (l2 > lim * lim) {
        const f32 l = std::sqrt(l2);
        if (l > kEpsilon) {
            const f32 inv = 1.0f / l;
            st.swayOffset = {inv * st.swayOffset.x, inv * st.swayOffset.y};
        }
        // The 0.95 park happens either way — a degenerate length skips the
        // normalise, not the scale.
        st.swayVelocity = {0, 0};
        st.swayOffset = {(lim * kParkFraction) * st.swayOffset.x,
                         (lim * kParkFraction) * st.swayOffset.y};
    }
}

/// @brief The emission cone, `sub_710097E390` as `Particle_ComputeInitialVelocity`
///        calls it. Verified bit-exact by G-D3P-15.
///
/// A cone about the velocity's OWN direction: tilt by @p cone about a
/// perpendicular, then spin by @p azimuth about the direction itself. The
/// perpendicular is `cross(dir, worldX)`, falling back to `cross(dir, worldY)`
/// when the velocity's y and z are both exactly zero — the raw components are
/// tested, before normalisation.
///
/// Rotating by @p cone about an axis in the XY plane instead only produces a cone
/// when the velocity happens to lie on Z; for a lateral one it opens the spread
/// in the wrong plane entirely. That is what this build did.
inline Vector3f ConeSpread(const Vector3f& v, f32 cone, f32 azimuth) {
    const f32 len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    Vector3f n = v;
    if (len > kEpsilon) {
        const f32 inv = 1.0f / len;
        n = {inv * v.x, inv * v.y, inv * v.z};
    }
    const bool onX = v.y == 0.0f && v.z == 0.0f;
    Vector3f perp = onX ? Vector3f{-n.z, 0.0f, n.x} : Vector3f{0.0f, n.z, -n.y};
    const f32 pl = std::sqrt(perp.z * perp.z + (perp.y * perp.y + perp.x * perp.x));
    if (pl > kEpsilon) {
        const f32 inv = 1.0f / pl;
        perp = {inv * perp.x, perp.y * inv, inv * perp.z};
    }
    const f32 h = cone * 0.5f;
    const f32 s1 = std::sin(h);
    const Quaternion tilt{perp.x * s1, perp.y * s1, perp.z * s1, std::cos(h)};
    const f32 h2 = azimuth * 0.5f;
    const f32 s2 = std::sin(h2);
    const Quaternion spin{s2 * n.x, s2 * n.y, s2 * n.z, std::cos(h2)};
    return (spin * tilt).rotate_vector(v);
}

/// @brief `Math_OrientationFromAxes` @0x71000B2EC0 — the shortest arc taking
///        @p a onto @p b, about `cross(a, b)`. Bit-exact under G-D3P-22.
///
/// Two thresholds decide the answer and neither is an epsilon: the near-parallel
/// early-out is `dot > 0.999` and the anti-parallel one `dot < -0.999`, which is
/// 2.56 degrees of arc either side. Everything inside that cone gets exact
/// identity (or an exact half turn about X); this build used `1 - 1e-6`, a
/// thirty-times narrower cone, and returned a small real rotation where the
/// engine returns none. The result is deliberately not normalised — it is unit
/// in exact arithmetic, and the extra step only moves the rounding.
inline Quaternion OrientationFromAxes(const Vector3f& a, const Vector3f& b) {
    /// The near-parallel and anti-parallel early-outs, 2.56° of arc either side.
    constexpr f32 kParallelDot = 0.999f;
    const f32 eps = kEpsilon;
    const f32 la = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    const f32 lb = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
    if (lb <= eps || la < eps)
        return {0, 0, 0, 1};
    const f32 ia = 1.0f / la, ib = 1.0f / lb;
    const Vector3f ua{ia * a.x, ia * a.y, ia * a.z};
    const Vector3f ub{ib * b.x, ib * b.y, ib * b.z};
    const f32 d = ub.z * ua.z + (ub.x * ua.x + ub.y * ua.y);
    if (d > kParallelDot)
        return {0, 0, 0, 1};
    if (d < -kParallelDot)
        return {1, 0, 0, 0};
    const Vector3f c{ub.z * ua.y - ub.y * ua.z, ub.x * ua.z - ub.z * ua.x,
                     ub.y * ua.x - ub.x * ua.y};
    const f32 s = (d + 1.0f) + (d + 1.0f);
    const f32 r = std::sqrt(s);
    const f32 inv = 1.0f / r;
    return {c.x * inv, c.y * inv, c.z * inv, r * 0.5f};
}

} // namespace whiteout::flakes::renderer::particle::d3
