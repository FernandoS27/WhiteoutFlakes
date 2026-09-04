// Orbital + preset camera controls. CameraView handles the math; this
// is just dispatch from JS pointer/wheel events.

#include "wf_web_internal.h"

#include <cstdint>

using wf_web::WfRenderer;

extern "C" {

void wf_camera_rotate(WfRenderer* h, int dx, int dy) {
    if (!h) return;
    h->renderer.Camera().Rotate(dx, dy);
}

void wf_camera_pan(WfRenderer* h, int dx, int dy) {
    if (!h) return;
    h->renderer.Camera().Pan(dx, dy);
}

void wf_camera_zoom(WfRenderer* h, int wheelDelta) {
    if (!h) return;
    h->renderer.Camera().Zoom(wheelDelta);
}

// Continuous zoom by a ratio. A wheel detent is a discrete step, but a
// pinch is a spread ratio between two fingers, so touch hosts hand that
// ratio over directly: >1 (fingers apart) pulls the camera in.
// CameraView::SetDistance clamps, so a runaway gesture can't escape.
void wf_camera_zoom_scale(WfRenderer* h, float scale) {
    if (!h || !(scale > 0.0f)) return;
    auto cam = h->renderer.Camera();
    cam.SetDistance(cam.GetDistance() / scale);
}

void wf_camera_reset(WfRenderer* h) {
    if (!h) return;
    h->renderer.Camera().Reset();
}

// Frame the orbital camera on a standalone effect's LIVE particle cloud.
// A `.pkb` has no mesh, so there is nothing to frame until the sim has
// actually emitted; returns 0 while the cloud is still empty and the host
// retries next frame. Same math as tools/common/thumbnail_framing.cpp's
// FrameCameraToEffect, which the web build can't call — it takes an
// internal RenderService&.
int wf_camera_frame_effect(WfRenderer* h, uint32_t actor) {
    if (!h) return 0;
    auto av = h->renderer.Actor(actor);
    if (!av.IsValid()) return 0;
    whiteout::flakes::Vector3f lo{}, hi{};
    if (!av.EffectBounds(/*emitterId*/ 0, lo, hi)) return 0;

    const whiteout::flakes::Vector3f center{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f,
                                            (lo.z + hi.z) * 0.5f};
    float maxAxis = hi.x - lo.x;
    if (hi.y - lo.y > maxAxis) maxAxis = hi.y - lo.y;
    if (hi.z - lo.z > maxAxis) maxAxis = hi.z - lo.z;
    if (maxAxis < 30.0f) maxAxis = 30.0f; // floor for tiny / point effects

    auto cam = h->renderer.Camera();
    cam.SetTarget(center.x, center.y, center.z);
    cam.SetDistance(maxAxis * 1.3f); // a little margin around the cloud
    return 1;
}

// idx<0 = Reset to orbital. Stashes {actor, idx} so wf_tick re-evals
// animated presets as the animation cursor advances.
void wf_camera_activate_preset(WfRenderer* h, uint32_t actor, int idx) {
    if (!h) return;
    auto cam = h->renderer.Camera();
    if (idx < 0) {
        h->cameraPresetActor = 0;
        h->cameraPresetIdx   = -1;
        cam.SetOrbitalMode();
        cam.SetFovDiagonal(whiteout::flakes::CameraView::kDefaultFovDiagonal);
        cam.SetClip(whiteout::flakes::CameraView::kDefaultNearZ,
                    whiteout::flakes::CameraView::kDefaultFarZ);
        cam.Reset();
        return;
    }
    auto av = h->renderer.Actor(actor);
    if (!av.IsValid()) return;
    const auto presets = av.CameraPresets();
    if (idx >= static_cast<int>(presets.size())) return;
    const auto& p = presets[idx];
    cam.SetFovDiagonal(p.fovDiagonal > 1e-3f
                       ? p.fovDiagonal
                       : whiteout::flakes::CameraView::kDefaultFovDiagonal);
    cam.SetClip(p.zNear, p.zFar);
    cam.SetDirectPose(p.position, p.target, p.staticRoll);

    // Stash before animator eval so the next wf_tick sees consistent state.
    h->cameraPresetActor = actor;
    h->cameraPresetIdx   = idx;
    wf_web::UpdateAnimatedCameraPreset(h);
}

} // extern "C"
