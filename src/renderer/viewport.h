#pragma once

#include "render_service.h" // SceneId
#include "render_target.h"  // RenderTargetId
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer {

class Camera;

// A Viewport binds a camera to a render target: "render this scene, seen
// through this camera, into this target." It is the unit RenderPipeline draws
// — RenderPipeline::RenderViewport(const Viewport&) renders one. Multiple
// viewports can target one scene (each with its own camera) and a run can hold
// several; they render sequentially into their own targets in a frame.
//
// Today the camera is a borrowed pointer and the scene is implicit (the single
// SceneManager). Later phases move camera ownership into the scene's camera set
// (the viewport will select an active camera index), add a SceneId, and carry
// per-viewport look settings (render mode, exposure, debug viz). For now look
// state still comes from the global RenderSettings.
struct Viewport {
    // The scene to render. 0 = the RenderService's default scene (the legacy
    // single-scene path). RenderViewport publishes this as the active scene for
    // the duration of the render, so the pass pipeline / effect services see
    // only this scene's actors and emitters.
    SceneId scene = 0;

    // The target to render into. A target whose swap-chain is Invalid is
    // headless (rendered but not presented).
    RenderTargetId target = 0;

    // The camera this viewport renders from. Borrowed, must outlive the
    // RenderViewport call. Null falls back to the scene's camera so the
    // legacy single-camera path keeps working.
    const Camera* camera = nullptr;

    // Whether this viewport composites the host's ImGui draw data. The engine
    // draws ImGui inside the scene/tonemap pass, so a viewport that is NOT the
    // UI surface (e.g. an off-screen thumbnail) must set this false — otherwise
    // it redraws the host's live ImDrawData on top of its own scene (and, if
    // that UI samples this viewport's texture, feeds back recursively).
    bool drawImGui = true;
};

} // namespace whiteout::flakes::renderer
