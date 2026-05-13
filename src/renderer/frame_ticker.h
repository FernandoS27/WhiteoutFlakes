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

namespace whiteout::flakes::renderer::animation {
struct ActorEvalContext;
}

namespace whiteout::flakes::renderer {

class RenderService;

namespace model {
struct Actor;
}

class FrameTicker {
public:
    explicit FrameTicker(RenderService& rs) : rs_(rs) {}

    // Called once per frame from the application loop.
    void Tick(f32 dt);

    // Externally-driven actors (Max plugin) bypass Tick and use
    // Actor::EvaluateAndApply directly.

private:
    void UpdateAttachments();
    void EvaluateActorTree();
    void EvaluateActorTreeRec(model::Actor& actor, const animation::ActorEvalContext& ctx,
                              i32 ancestorClock);
    void SilenceCornEmittersRec(model::Actor& actor);
    void UpdateAnimation();
    void UpdateParticles(f32 dt);
    void UpdatePE1(f32 dt);
    void UpdateRibbons(f32 dt);

    RenderService& rs_;
};

} // namespace whiteout::flakes::renderer
