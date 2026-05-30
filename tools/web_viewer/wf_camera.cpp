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

void wf_camera_reset(WfRenderer* h) {
    if (!h) return;
    h->renderer.Camera().Reset();
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
