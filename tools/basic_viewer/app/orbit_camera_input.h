#pragma once

// ============================================================================
// Mouse and scroll input steering the active scene's orbital camera: left drag
// rotates, right drag pans, middle drag and the wheel zoom.
//
// ImGui's GLFW backend already routes the same events into ImGui, so anything
// ImGui wants (a click in a panel, the ViewCube's invisible button) never
// reaches the camera. A live camera preset locks it.
// ============================================================================

#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes {

class PlaybackController;

class OrbitCameraInput {
public:
    OrbitCameraInput(renderer::RenderService& service, const PlaybackController& playback);

    /// @p button and @p action are GLFW's.
    void OnMouseButton(i32 button, i32 action, f64 x, f64 y);
    void OnCursorPos(f64 x, f64 y);
    void OnScroll(f64 yoffset);

private:
    renderer::RenderService& service_;
    const PlaybackController& playback_;
    bool lmbDown_ = false;
    bool rmbDown_ = false;
    bool mmbDown_ = false;
    f64 lastMouseX_ = 0.0;
    f64 lastMouseY_ = 0.0;
};

} // namespace whiteout::flakes
