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

#include "types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::particle::d3 {

struct ParticleState {
    /// particle+4. Every `Eval` on this particle derives its random stream
    /// from `(channelId * seed, 666)`, so this must never change after birth.
    u32 seed = 0;

    f32 lifetime = 1.0f; ///< particle+40, seconds (ch 29)
    f32 baseSize = 1.0f; ///< particle+24 (ch 28)

    Quaternion orientation = Quaternion::identity();      ///< particle+216
    Quaternion birthEmitterQuat = Quaternion::identity(); ///< particle+184, triple B's frame

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
    f32 scale = 1.0f;  ///< particle+208 (ch 5)
    f32 childScalar = 1.0f; ///< particle+240 (ch 2)
    Vector4f color{1, 1, 1, 1}; ///< particle+232 RGB, particle+236 alpha

    // Wind spring (system types 6 and 8). Unused by every other type, and
    // eleven floats is cheap next to giving foliage its own emitter class.
    Vector2f swayOffset{0, 0};
    Vector2f swayVelocity{0, 0};
    Vector2f swayForce{0, 0};
    f32 swayPhase = 0.0f;

    bool orbitSeeded = false;
    bool radialSeeded = false;
};

} // namespace whiteout::flakes::renderer::particle::d3
