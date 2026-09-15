#pragma once

// ============================================================================
// Per-particle motion. Gravity is a vector (WC3's scalar leaves x and y exactly
// zero). One integrator per dialect, because WoW applies its terms in a
// different order; see core/particle_dialect.h's ParticleForceModel and
// M2_PARTICLE_DESIGN.md §11.6.
// ============================================================================

#include "renderer/particle/base/particle2.h"
#include "types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::particle {

// The animated half of motion. WC3 rewrites `gravity` every frame from its
// FrameState; drag and wind stay at their desc defaults.
struct MotionParams {
    Vector3f gravity{0, 0, 0};
    Vector3f wind{0, 0, 0};
    f32 drag = 0.0f; // per-second velocity damping; 0 = none
};

// Immutable motion configuration. Seeds MotionParams at registration; formats
// that animate a field overwrite it in ApplyState.
// Gravity is not here: every format animates it, so it arrives in ApplyState.
struct MotionDesc {
    Vector3f wind{0, 0, 0};
    f32 drag = 0.0f;
};

// Semi-implicit Euler with the position's quadratic acceleration term, as WC3
// does on its single axis. Gravity only; M2_PARTICLE_DESIGN.md §11.6.
inline void IntegrateWc3(Particle2& p, const MotionParams& m, f32 dt) {
    p.position.x += p.velocity.x * dt + 0.5f * m.gravity.x * dt * dt;
    p.position.y += p.velocity.y * dt + 0.5f * m.gravity.y * dt * dt;
    p.position.z += p.velocity.z * dt + 0.5f * m.gravity.z * dt * dt;

    p.velocity.x += m.gravity.x * dt;
    p.velocity.y += m.gravity.y * dt;
    p.velocity.z += m.gravity.z * dt;
}

// ---------------------------------------------------------------------------
// The WoW force model: `CalculateForces` @0x1016a6710 bakes the frame's forces
// once, `MoveParticle` @0x1016a1360 applies them in THIS order — wind into the
// velocity, displacement from that pre-gravity velocity, gravity (velocity and
// position terms), then drag clamped to 1. Swapping any two moves the trajectory.
// ---------------------------------------------------------------------------

struct ParticleForces {
    Vector3f windVel{0, 0, 0};  // wind * dt, an instantaneous velocity add
    Vector3f accel{0, 0, 0};    // gravity * dt
    Vector3f posDelta{0, 0, 0}; // 0.5 * gravity * dt^2
    f32 drag = 0.0f;            // min(dragCoeff * dt, 1)
};

inline ParticleForces CalculateForcesWow(const MotionParams& m, f32 dt) {
    ParticleForces f;
    f.windVel = {m.wind.x * dt, m.wind.y * dt, m.wind.z * dt};
    f.accel = {m.gravity.x * dt, m.gravity.y * dt, m.gravity.z * dt};
    const f32 half = 0.5f * dt * dt;
    f.posDelta = {m.gravity.x * half, m.gravity.y * half, m.gravity.z * half};
    // The clamp is the client's: an unclamped `1 - drag*dt` goes negative for a
    // large step and flips the particle's direction instead of stopping it.
    f.drag = (m.drag == 0.0f) ? 0.0f : ((m.drag * dt < 1.0f) ? m.drag * dt : 1.0f);
    return f;
}

/// @brief Advance one particle. Returns false when the implosion filter wants
///        it dead — the caller kills it rather than this mutating the pool.
inline bool MoveParticleWow(Particle2& p, const ParticleForces& f, f32 dt, bool implosionKill,
                            const Vector3f& center) {
    p.velocity.x += f.windVel.x;
    p.velocity.y += f.windVel.y;
    p.velocity.z += f.windVel.z;

    // Displacement is taken from the velocity BEFORE gravity is applied.
    const Vector3f disp = {p.velocity.x * dt, p.velocity.y * dt, p.velocity.z * dt};

    p.velocity.x += f.accel.x;
    p.velocity.y += f.accel.y;
    p.velocity.z += f.accel.z;

    p.position.x += disp.x + f.posDelta.x;
    p.position.y += disp.y + f.posDelta.y;
    p.position.z += disp.z + f.posDelta.z;

    const f32 k = 1.0f - f.drag;
    p.velocity.x *= k;
    p.velocity.y *= k;
    p.velocity.z *= k;

    if (!implosionKill)
        return true;

    // Kill anything whose step carried it outward from the centre.
    const f32 away = disp.x * (p.position.x - center.x) + disp.y * (p.position.y - center.y) +
                     disp.z * (p.position.z - center.z);
    return away <= 0.0f;
}

} // namespace whiteout::flakes::renderer::particle
