#pragma once

// ============================================================================
// ActorEvalContext — bundle of renderer-wide state needed to drive a single
// Actor's per-frame evaluation. Built by RenderService::MakeActorEvalContext()
// and passed into Actor::EvaluateAndApply() / Actor::ApplyFrameState().
//
// Decouples Actor from RenderService: Actor owns its evaluation logic but
// reads camera position / scene time / external services through this struct
// instead of holding a back-pointer to the renderer.
// ============================================================================

#include "types.h" // Vector3f
#include "whiteout/flakes/types.h"

#include <functional>

namespace whiteout::flakes::renderer {
class SceneManager;
class ISoundEmitter;
} // namespace whiteout::flakes::renderer
namespace whiteout::flakes::renderer::effects {
class SpnSpawner;
}
namespace whiteout::flakes::renderer::particle {
class ParticleService;
class SplatService;
} // namespace whiteout::flakes::renderer::particle
namespace whiteout::flakes::renderer::ribbon {
class RibbonService;
}
namespace whiteout::flakes::renderer::corn_effects {
class CornEffectsService;
}

namespace whiteout::flakes::renderer::animation {

struct ActorEvalContext {
    Vector3f camPos = {0, 0, 0};
    i32 sceneAnimationTimeMs = 0;
    bool fireEvents = false;

    /// @brief Run the per-actor pose stages (terrain IK, turret) this frame.
    ///
    /// Off unless the host turns it on, which mirrors StarCraft II gating IK on
    /// a world flag rather than per model. Off means byte-identical output, so
    /// the gates that assert "nothing moved" keep meaning something.
    bool poseStagesEnabled = false;
    /// @brief Real frame delta, for the IK goal's rate limit and the turret's
    ///        slew. Real time, not animation time: a paused actor's feet still
    ///        settle onto the ground.
    i32 frameDtMs = 0;
    /// @brief Host-supplied ground height. Absent ⇒ no terrain IK runs; the
    ///        renderer has no terrain of its own to guess from.
    std::function<bool(const Vector3f& pos, f32 up, f32 down, f32& outZ)> queryGround;
    SceneManager* scene = nullptr;
    particle::ParticleService* particles = nullptr;
    particle::SplatService* splats = nullptr;
    ribbon::RibbonService* ribbons = nullptr;
    corn_effects::CornEffectsService* cornEffects = nullptr;
    effects::SpnSpawner* spnSpawner = nullptr;
    ISoundEmitter* sound = nullptr;
};

} // namespace whiteout::flakes::renderer::animation
