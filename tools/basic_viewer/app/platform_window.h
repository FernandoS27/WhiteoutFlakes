#pragma once

// ============================================================================
// The viewer's window: GLFW, DPI awareness, the icon and title-bar chrome, the
// ImGui input backend and the swap-chain target the frame is presented to.
//
// Windows runs a modal message loop while a border or the title bar is dragged,
// so the frame loop stops for the whole drag. The resize and refresh callbacks
// still fire inside it, so the window paints from there (`redraw`) — and never
// polls events from inside that dispatch.
// ============================================================================

#include "render_target.h"
#include "whiteout/flakes/gfx_types.h"
#include "whiteout/flakes/types.h"

#include <functional>
#include <optional>

struct GLFWwindow;

namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes {

struct WindowEvents {
    /// Paint one frame from inside a GLFW callback. Re-entrant calls are dropped.
    std::function<void()> redraw;
    std::function<void(i32 button, i32 action, f64 x, f64 y)> mouseButton;
    std::function<void(f64 x, f64 y)> cursorPos;
    std::function<void(f64 yoffset)> scroll;
};

class PlatformWindow {
public:
    explicit PlatformWindow(renderer::RenderService& service);
    ~PlatformWindow();

    PlatformWindow(const PlatformWindow&) = delete;
    PlatformWindow& operator=(const PlatformWindow&) = delete;

    /// `visible == false` creates the window hidden: the full app runs but
    /// nothing appears on screen. @p dpiScale pins the scale instead of asking
    /// the monitor. The device comes up here, after the ImGui context it needs.
    bool Open(i32 width, i32 height, gfx::GfxApi api, bool visible, std::optional<f32> dpiScale,
              WindowEvents events);
    /// ImGui, the device and the window, in that order.
    void Close();

    bool ShouldClose() const;
    void RequestClose();
    /// Dispatch pending events, unless this frame is being painted from inside a
    /// callback — polling again there would recurse through the same queue.
    void PollEvents();
    /// Match the swap chain to the framebuffer. The resize callback alone misses
    /// sizes during the maximise transition and on DPI changes. False while
    /// minimised: swap chains reject a zero extent.
    bool SyncFramebufferSize();
    void BeginImGuiFrame();
    /// Dispatch events and report whether Esc is down: how a capture that owns
    /// the frame loop is stopped, and what keeps the window alive meanwhile.
    bool PollEscape();
    void SetTitle(const char* title);

    GLFWwindow* Handle() const {
        return window_;
    }
    renderer::RenderTargetId Target() const {
        return target_;
    }
    gfx::GfxApi Backend() const {
        return backend_;
    }

private:
    void InitImGui(std::optional<f32> dpiScale);
    void OnFramebufferResize(i32 w, i32 h);
    void RedrawFromCallback();

    static void FramebufferSizeCallback(GLFWwindow* w, int width, int height);
    static void WindowRefreshCallback(GLFWwindow* w);
    static void MouseButtonCallback(GLFWwindow* w, int button, int action, int mods);
    static void CursorPosCallback(GLFWwindow* w, double x, double y);
    static void ScrollCallback(GLFWwindow* w, double xoff, double yoff);

    renderer::RenderService& service_;
    WindowEvents events_;
    GLFWwindow* window_ = nullptr;
    gfx::GfxApi backend_ = gfx::GfxApi::D3D12;
    renderer::RenderTargetId target_ = 0;
    bool imguiInitialised_ = false;
    i32 lastFbW_ = 0;
    i32 lastFbH_ = 0;
    bool inCallbackRedraw_ = false;
};

} // namespace whiteout::flakes
