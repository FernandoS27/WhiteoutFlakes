#pragma once

// ============================================================================
// FrameTicker — per-frame scene-update orchestration extracted from RenderService.
//
// The application calls Tick(dt) once per frame BEFORE RenderFrame. Tick walks
// the scene's actors and advances their animation, attachments, particles,
// PE1 children, and ribbons. GPU upload work (staged textures/geometry) lives
// on RenderService::RenderFrame; FrameTicker is pure scene-update logic plus
// bone-palette CB writes.
//
// FrameTicker is a friend of RenderService so it can read/write the shared
// state in RenderService::Impl directly. This mirrors how other in-tree
// renderer subsystems (DebugRenderer, SpnSpawner, shadow::ShadowPass) reach
// into RenderService.
// ============================================================================

#include "common_types.h"

namespace whiteout::flakes::renderer {

class RenderService;

class FrameTicker {
public:
    explicit FrameTicker(RenderService& rs) : rs_(rs) {}

    // Called once per frame from the application loop.
    void Tick(f32 dt);

    // Externally-driven actors (Max plugin) bypass Tick and use
    // Actor::EvaluateAndApply directly.

private:
    void UpdateAttachments();
    void EvaluateTopLevelActors();
    void EvaluatePE1Children();
    void UpdateAnimation();
    void UpdateParticles(f32 dt);
    void UpdatePE1(f32 dt);
    void UpdateRibbons(f32 dt);

    RenderService& rs_;
};

}
