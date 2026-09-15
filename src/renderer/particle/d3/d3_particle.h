#pragma once

// ============================================================================
// D3ParticleState — the live per-particle state a D3 particle needs beyond the
// 32-byte `Particle2` (which keeps position, velocity and age), riding the pool
// as a parallel array like `MultiTexState`. The five `prev*` fields drive the
// differentiation idiom `rate*60 + (now - prev)/dt` of D3_PARTICLE_DESIGN.md §5.
// See §30.3.
// ============================================================================

#include "renderer/particle/d3/d3_channels.h"
#include "renderer/particle/d3/d3_path.h" // MwcRng
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

    /// particle+216, the quaternion the vertex packs. Seated at birth (the emitter
    /// quaternion; identity in render mode 1; the camera-facing frame for a
    /// spinning system), turned by spin, and overwritten at draw by every mode
    /// that builds a frame.
    Quaternion orientation = Quaternion::identity();
    /// particle+184, triple B's frame. For foliage (types 6 and 8) it is a random
    /// turn about world Z instead, and its w is the sway phase.
    Quaternion birthEmitterQuat = Quaternion::identity();

    /// pool+444 and pool+456 — the frame's displacement (raw, every frame) and its
    /// last unit direction (only past the frame epsilon), per
    /// `ParticleSystem_UpdateParticles` @0x71000BEB00: the direction of motion.
    /// The birth seeds the unit one with the emitter's X. See §30.3.
    Vector3f axis{0, 0, 0};
    Vector3f axisUnit{0, 0, 0};

    /// pool+560, and the XY it was sampled at (pool+548/552). Only render modes
    /// 9 and 10 read it; the engine re-casts only when the particle's XY has
    /// moved, so a stationary one samples once and a still system costs nothing.
    Vector3f groundNormal{0, 0, 1};
    Vector2f groundAt{0, 0};
    bool groundSeeded = false;

    Vector3f spinAxis{0, 1, 0}; ///< particle+172: the birth draw, or normalised ch 23
    Vector3f radialDir{0, 0, 0}; ///< particle+48, cached at first use
    Vector2f orbitDir{1, 0};     ///< particle+60, seeded (cos phi, sin phi) at birth

    // Kinematic triple A — world space. `prevOffsetA` is the differentiation
    // cache; `accelVelA` is the running integral of the acceleration channel.
    Vector3f prevOffsetA{0, 0, 0}; ///< particle+76
    Vector3f accelVelA{0, 0, 0};   ///< particle+88

    // Kinematic triple B — emitter-local. Same shape. The engine also keeps a
    // running total of the local displacement at particle+124, which it does
    // NOT use for motion — the per-frame delta is what gets rotated — so it is
    // not kept here.
    Vector3f prevOffsetB{0, 0, 0}; ///< particle+100
    Vector3f accelVelB{0, 0, 0};   ///< particle+112

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

    /// @brief The four UV animation states, one per POSITIONAL uv set, and every
    ///        scalar belongs to the particle, not the emitter: seeded by
    ///        `MatTex_InitUvState`, advanced by `MatTex_TickUvStateEntry`. Six
    ///        scroll/rotation scalars, two for the flip-book player (§27.3, §30.3).
    struct UvState {
        f32 u = 0.0f, v = 0.0f;
        f32 uRate = 0.0f, vRate = 0.0f;
        f32 rot = 0.0f, rotRate = 0.0f;
        f32 cursor = 0.0f, cursorRate = 0.0f;
    };
    std::array<UvState, 4> uv{};
};

/// The five `.prt` fields the wind spring reads, at SNO+176..192.
struct WindSpringRig {
    f32 frequency = 1.0f;
    f32 damping = 0.3f;
    f32 maxOffset = 1.0f;
    f32 gustAmount = 1.25f;
    f32 baseAmount = 0.0f;
};

/// One step of `ParticleSystem_StepWindSpring` @0x71000BD610, per particle: a
/// damped oscillator in XY, bit-exact under G-D3P-14. The force enters the
/// velocity WHOLE (it already carries 1/60; only the spring term takes `dt`), and
/// `swayOffset` BENDS the quad's two +v corners rather than moving the particle.
/// @p reuseForce is the system+624 arm; the gust phase is the birth quaternion's w
/// (pool+500). See §21.3, §5.6 and §30.3.
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
                         rig.gustAmount * (std::cos(st.birthEmitterQuat.w * kTwoPi + windPhase) *
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

/// The first draws of a birth: the particle's seed, and where along the emitter's
/// frame it is born.
struct BirthDraw {
    u32 seed = 0;
    Vector3f base{0, 0, 0};
};

/// @brief `Particle_InitLifeAndSize`'s seed draw and sub-frame birth lerp
///        (G-D3P-A4). The seed is the raw draw nudged past the engine's two
///        sentinels; then, unless @p atEmitter, a second draw places the base
///        uniformly on @p prev → @p now. `PrtFlag::BirthAtEmitter` skips the lerp
///        AND ITS DRAW, which shifts every draw after it. See §30.3.
inline BirthDraw DrawSeedAndBase(MwcRng& rng, const Vector3f& prev, const Vector3f& now,
                                 bool atEmitter) {
    BirthDraw b;
    const u32 raw = rng.Next();
    b.seed = (raw >= kFirstSentinelSeed) ? (raw + 2u) : raw;
    b.base = now;
    if (!atEmitter) {
        const f32 u = rng.NextUnit();
        b.base = {prev.x + u * (now.x - prev.x), prev.y + u * (now.y - prev.y),
                  prev.z + u * (now.z - prev.z)};
    }
    return b;
}

/// What one move of the emitter does to its live particles.
struct CarryMove {
    bool carries = false; ///< any particle moves at all
    bool rotates = false; ///< about the emitter by @ref dq; else translated
    Vector3f from{0, 0, 0};
    Vector3f to{0, 0, 0};
    Quaternion dq = Quaternion::identity();
};

/// @brief `ParticleSystem_SetEmitterTransform` @0x71000AFBE0's rule (G-D3P-21),
///        for a move from (@p from, @p fromQ) to (@p to, @p toQ): bit 8
///        (@p carry) carries the live particles, bit 29 (@p withoutRotation) by
///        translation only. The exact `!= 0` displacement guard is the TRANSLATE
///        arm's alone. Types 2 and 3 never carry (`(type & ~1) == 2`
///        @0x71000AFC94) and type 9 returns at the top; type 1 pools nothing, so
///        its early-out here is free. See §22.3 and §30.3.
inline CarryMove PlanCarry(SystemType type, bool carry, bool withoutRotation, const Vector3f& from,
                           const Quaternion& fromQ, const Vector3f& to, const Quaternion& toQ) {
    CarryMove m;
    m.from = from;
    m.to = to;
    if (type == SystemType::Weather || type == SystemType::Swarm ||
        type == SystemType::RibbonPhysics || type == SystemType::Ribbon || !carry)
        return m;
    const Vector3f delta{from.x - to.x, from.y - to.y, from.z - to.z};
    const bool moved = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z != 0.0f;
    const bool sameQuat =
        toQ.x == fromQ.x && toQ.y == fromQ.y && toQ.z == fromQ.z && toQ.w == fromQ.w;
    if (withoutRotation || sameQuat) {
        m.carries = moved;
        return m;
    }
    m.carries = true;
    m.rotates = true;
    m.dq = toQ * fromQ.conjugate();
    return m;
}

/// Where a live particle at @p p lands under @p m, which must carry.
inline Vector3f CarryPoint(const CarryMove& m, const Vector3f& p) {
    if (!m.rotates)
        return {m.to.x + (p.x - m.from.x), m.to.y + (p.y - m.from.y), m.to.z + (p.z - m.from.z)};
    const Vector3f rel{p.x - m.from.x, p.y - m.from.y, p.z - m.from.z};
    const Vector3f r = m.dq.rotate_vector(rel);
    return {m.to.x + r.x, m.to.y + r.y, m.to.z + r.z};
}

/// @brief The emission cone, `sub_710097E390` as `Particle_ComputeInitialVelocity`
///        calls it (G-D3P-15): tilt by @p cone about `cross(dir, worldX)` —
///        `cross(dir, worldY)` when the RAW y and z are exactly zero — then spin
///        by @p azimuth about the velocity's own direction. See §22.4, §30.3.
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
///        @p a onto @p b, about `cross(a, b)`. Bit-exact under G-D3P-22. The
///        `±0.999` early-outs are cut-offs, not epsilons (exact identity or half
///        turn inside them), and the result is deliberately not normalised.
///        See §23.1, §30.3.
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
