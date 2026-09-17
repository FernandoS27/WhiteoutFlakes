#include "app/orbit_camera_input.h"

#include "app/viewer_tuning.h"
#include "documents/playback_controller.h"
#include "renderer/camera.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"

#include <imgui.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace whiteout::flakes {

using renderer::Camera;

namespace {

bool ImGuiWantsMouse() {
    return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
}

} // namespace

OrbitCameraInput::OrbitCameraInput(renderer::RenderService& service, const PlaybackController& playback)
    : service_(service), playback_(playback) {}

void OrbitCameraInput::OnMouseButton(i32 button, i32 action, f64 x, f64 y) {
    if (ImGuiWantsMouse()) {
        // Forget any drag in flight, so releasing the button outside a panel
        // does not snap the camera.
        lmbDown_ = rmbDown_ = mmbDown_ = false;
        return;
    }
    const bool pressed = action == GLFW_PRESS;
    if (button == GLFW_MOUSE_BUTTON_LEFT)
        lmbDown_ = pressed;
    else if (button == GLFW_MOUSE_BUTTON_RIGHT)
        rmbDown_ = pressed;
    else if (button == GLFW_MOUSE_BUTTON_MIDDLE)
        mmbDown_ = pressed;
    lastMouseX_ = x;
    lastMouseY_ = y;
}

void OrbitCameraInput::OnCursorPos(f64 x, f64 y) {
    const f64 dx = x - lastMouseX_;
    const f64 dy = y - lastMouseY_;
    lastMouseX_ = x;
    lastMouseY_ = y;

    if (playback_.CameraLocked() || ImGuiWantsMouse())
        return;

    auto& cam = service_.Scene().Camera();
    if (lmbDown_)
        cam.Rotate(static_cast<i32>(dx), static_cast<i32>(dy));
    if (rmbDown_)
        cam.Pan(static_cast<i32>(-dx), static_cast<i32>(dy));
    if (mmbDown_)
        cam.ZoomSmooth(static_cast<f32>(dy) * cam.GetDistance() / Camera::kFactorRelDist);
}

void OrbitCameraInput::OnScroll(f64 yoffset) {
    if (playback_.CameraLocked() || ImGuiWantsMouse())
        return;
    service_.Scene().Camera().Zoom(static_cast<i32>(yoffset * tuning::kScrollZoomStep));
}

} // namespace whiteout::flakes
