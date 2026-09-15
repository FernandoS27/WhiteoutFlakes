#pragma once

// ============================================================================
// The state only the WoW dialect of `Emitter2` reads, grouped by lifetime.
//
// Inert under the WC3 dialect: every member keeps its default there, and no
// WC3 stage consults one. `WowMotion` is what a rewind throws away; the rest
// are inputs the host pushes and a rewind keeps.
// ============================================================================

#include "types.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <vector>

namespace whiteout::flakes::renderer::particle {

/// The emitter-motion accumulators. Reset whole by a rewind.
struct WowMotion {
    /// The velocity a newly born particle inherits, sampled from the
    /// emitter's travel while its pool is empty.
    Vector3f emitterVelocity{0, 0, 0};
    /// Where along the travelled segment the spawn being made sits, relative
    /// to the emitter.
    Vector3f spawnOffset{0, 0, 0};
    /// The share of the emitter's travel a trailing particle inherits this
    /// sub-step.
    Vector3f followDelta{0, 0, 0};
    f32 velocityTimer = 0.0f;
};

struct WowRuntime {
    WowMotion motion;

    /// The animated lifespan. WoW drives this from an M2 track and re-reads it
    /// every frame for every live particle; WC3 has no such track, so its
    /// emitters leave it at the desc value and nothing consults it.
    f32 lifeSpan = 0.0f;

    /// Distance from the camera, for the emission-rate falloff. Pushed by the
    /// service once per frame.
    f32 viewDistance = 0.0f;

    /// The model's own fade, which WoW multiplies into particle alpha instead
    /// of gating the emitter with it.
    f32 modelAlpha = 1.0f;

    /// The model's bone-emitter table, for a bone generator. Copied rather
    /// than referenced: the FrameState it comes from is rebuilt every
    /// evaluation and the emitter reads this during its own update, later.
    std::vector<model::FrameState::BoneSpawn> boneSpawns;

    /// The emitter's random flipbook offset, drawn once from its seed.
    u16 baseCell = 0;
};

} // namespace whiteout::flakes::renderer::particle
