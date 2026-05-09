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

#include "common_types.h"
#include "types.h"  // Vector3f

namespace whiteout::flakes::renderer {
    class SceneManager;
    class ISoundEmitter;
}
namespace whiteout::flakes::renderer::effects { class SpnSpawner; }
namespace whiteout::flakes::renderer::particle {
    class ParticleService;
    class SplatService;
}
namespace whiteout::flakes::renderer::corn_effects { class CornEffectsService; }

namespace whiteout::flakes::renderer::animation {

struct ActorEvalContext {
    Vector3f                   camPos              = {0, 0, 0};
    i32                        sceneAnimationTimeMs = 0;
    bool                       fireEvents          = false;
    SceneManager*              scene               = nullptr;
    particle::ParticleService* particles           = nullptr;
    particle::SplatService*    splats              = nullptr;
    corn_effects::CornEffectsService*   cornEffects             = nullptr;
    effects::SpnSpawner*       spnSpawner          = nullptr;
    ISoundEmitter*             sound               = nullptr;
};

}
