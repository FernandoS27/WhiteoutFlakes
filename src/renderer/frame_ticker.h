#pragma once

// ============================================================================
// FrameTicker — per-frame scene-update orchestration.
//
// The application calls Tick(dt) once per frame BEFORE RenderFrame. Tick walks
// the scene's actors and advances their animation, attachments, particles,
// PE1 children, and ribbons. GPU upload work (staged textures/geometry) lives
// on RenderService::RenderFrame; FrameTicker is pure scene-update logic plus
// bone-palette CB writes.
//
// FrameTicker reaches the renderer's other subsystems through the public
// RenderService accessors (Scene, Replaceables, Particles, Splats, Spn,
// Loader, Pipeline) — no friend declarations.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <span>

namespace whiteout::flakes::renderer::animation {
struct ActorEvalContext;
}

namespace whiteout::flakes::renderer {

class RenderService;
class SceneManager;

namespace model {
struct Actor;
}

class FrameTicker {
public:
    explicit FrameTicker(RenderService& rs) : rs_(rs) {}

    // Called once per frame from the application loop. Ticks the default scene
    // (the legacy single-scene path).
    void Tick(f32 dt);

    // Ticks a SPECIFIC scene: publishes it as the active scene, then runs the
    // per-frame update (actors, animation, particles, PE1, ribbons) against it.
    // RenderService::TickScenes calls this for every scene each frame.
    void Tick(SceneManager& scene, f32 dt);

    // Externally-driven actors (Max plugin) bypass Tick and use
    // Actor::EvaluateAndApply directly.

private:
    void UpdateAttachments();
    void EvaluateActorTree(f32 dt);
    // @p parentBones is the caller's freshly evaluated bone matrices, which a
    // Skinned child poses from. Empty at the root and for a parent that
    // evaluated nothing.
    void EvaluateActorTreeRec(model::Actor& actor, const animation::ActorEvalContext& ctx,
                              i32 ancestorClock, std::span<const Matrix44f> parentBones = {});
    void SilenceCornEmittersRec(model::Actor& actor);
    void UpdateAnimation();
    void UpdateParticles(f32 dt);
    // Turn the particle service's child-model output (Birth / Transform /
    // Death) into actor spawns, transform writes and destroys.
    void DriveChildModels();
#if WDX_ENABLE_D3
    // Drains the child-model requests the Diablo III keyframed-attachment
    // pools raised during EvaluateActorTree. Separate for the same reason
    // DriveChildModels is: the pools tick inside the walk over the actor map
    // and cannot spawn into it.
    void DriveD3Attachments();
#endif
    void UpdateRibbons(f32 dt);

    RenderService& rs_;
};

} // namespace whiteout::flakes::renderer
