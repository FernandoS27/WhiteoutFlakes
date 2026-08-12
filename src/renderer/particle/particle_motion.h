#pragma once

// ============================================================================
// Per-particle motion.
//
// WC3 only ever accelerates straight down, encoded as a scalar it animates per
// frame. Expressing that as a gravity *vector* costs nothing (the extra
// components are exactly zero) and is what M2 needs for its directional gravity
// and wind, and M3 for drag.
// ============================================================================

#include "particle2.h"
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
struct MotionDesc {
    Vector3f gravity{0, 0, 0};
    Vector3f wind{0, 0, 0};
    f32 drag = 0.0f;
    f32 mass = 1.0f; // M3; unused until a format populates it
};

// Semi-implicit Euler with the position's quadratic acceleration term, matching
// what WC3 does on its single axis. With zero drag and wind the extra terms are
// exact no-ops (adding 0, multiplying by 1), so the WC3 path is untouched
// rather than approximated.
inline void Integrate(Particle2& p, const MotionParams& m, f32 dt) {
    p.position.x += p.velocity.x * dt + 0.5f * m.gravity.x * dt * dt;
    p.position.y += p.velocity.y * dt + 0.5f * m.gravity.y * dt * dt;
    p.position.z += p.velocity.z * dt + 0.5f * m.gravity.z * dt * dt;

    p.velocity.x += m.gravity.x * dt;
    p.velocity.y += m.gravity.y * dt;
    p.velocity.z += m.gravity.z * dt;

    if (m.drag != 0.0f) {
        const f32 k = 1.0f - m.drag * dt;
        p.velocity.x *= k;
        p.velocity.y *= k;
        p.velocity.z *= k;
    }
    if (m.wind.x != 0.0f || m.wind.y != 0.0f || m.wind.z != 0.0f) {
        p.velocity.x += m.wind.x * dt;
        p.velocity.y += m.wind.y * dt;
        p.velocity.z += m.wind.z * dt;
    }
}

} // namespace whiteout::flakes::renderer::particle
