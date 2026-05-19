#include "viewer_app.h"

#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/camera.h"
#include "renderer/debug/debug_renderer.h"
#include "renderer/frame_ticker.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_loader.h"
#include "renderer/model/model_template.h"
#include "renderer/particle/splat_service.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#if defined(_WIN32)
#include "resource.h" // IDI_WHITEOUT_ICON
#endif
#include "imgui_theme.h"
#include "viewer_ui.h"
#include "whiteout/flakes/util/path_utf8.h"

#include "gfx/gfx.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>

// We use glfwCreateWindowSurface (cross-platform) for the Vulkan backend.
// On Windows, the D3D backends still want a raw HWND, so we pull in
// glfw3native there. Including <vulkan/vulkan.h> *before* <GLFW/glfw3.h>
// makes glfwCreateWindowSurface visible without GLFW pulling in its own
// vulkan header copy.
#include <vulkan/vulkan.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
// Newer DWM attributes — define locally so we don't depend on the SDK version.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
// Per-monitor V2 DPI context — declared in Win10 1607+ SDKs. Older SDKs need
// the cast-from-int fallback (same trick the SDK header itself uses).
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace whiteout::flakes {

namespace {

const char* kWindowTitle = "WhiteoutFlakes";

bool ContainsCi(const std::string& hay, const char* needle) {
    const usize hn = hay.size();
    const usize nn = std::strlen(needle);
    if (nn == 0 || hn < nn)
        return false;
    for (usize i = 0; i + nn <= hn; ++i) {
        bool ok = true;
        for (usize j = 0; j < nn; ++j) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(hay[i + j])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) {
                ok = false;
                break;
            }
        }
        if (ok)
            return true;
    }
    return false;
}

} // namespace

ViewerApp::ViewerApp(RenderService& service) : service_(service) {
    ui_ = std::make_unique<ViewerUI>(*this);
}

ViewerApp::~ViewerApp() {
    Close();
}

bool ViewerApp::Open(i32 width, i32 height, gfx::GfxApi api) {
    backend_ = api;

#if defined(_WIN32)
    // Opt into per-monitor V2 awareness before any HWND is created. Without
    // this, Windows bitmap-stretches the whole window on non-100% displays
    // (4K / scaled laptop panels), which is what makes ImGui glyphs look
    // soft and wavy. SetProcessDpiAwarenessContext exists on Windows 10
    // 1703+; pre-Win10 falls back through the older SetProcessDpiAware.
    {
        using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
        auto setCtx =
            u32 ? reinterpret_cast<SetCtxFn>(::GetProcAddress(u32, "SetProcessDpiAwarenessContext"))
                : nullptr;
        if (!setCtx || !setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            ::SetProcessDPIAware();
    }
#endif

    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit FAILED\n");
        return false;
    }

    // No OpenGL context — we drive the swap chain through the engine's gfx
    // layer, which talks directly to d3d11 / d3d12 / vulkan.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    // The caller's width/height are logical (96-DPI) pixels — pre-scale them
    // for the primary monitor so a 1280x720 viewer doesn't shrink to a quarter
    // of the screen on a 4K 200% display now that we're DPI-aware.
    float initialDpiScale = 1.0f;
    if (GLFWmonitor* primary = glfwGetPrimaryMonitor()) {
        float xs = 1.0f;
        float ys = 1.0f;
        glfwGetMonitorContentScale(primary, &xs, &ys);
        if (xs > 0.0f)
            initialDpiScale = xs;
    }
    const i32 scaledW = static_cast<i32>(static_cast<f32>(width) * initialDpiScale);
    const i32 scaledH = static_cast<i32>(static_cast<f32>(height) * initialDpiScale);

    window_ = glfwCreateWindow(scaledW, scaledH, kWindowTitle, nullptr, nullptr);
    if (!window_) {
        std::fprintf(stderr, "glfwCreateWindow FAILED\n");
        glfwTerminate();
        return false;
    }
    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, &ViewerApp::FramebufferSizeCallback);
    glfwSetMouseButtonCallback(window_, &ViewerApp::MouseButtonCallback);
    glfwSetCursorPosCallback(window_, &ViewerApp::CursorPosCallback);
    glfwSetScrollCallback(window_, &ViewerApp::ScrollCallback);

#if defined(_WIN32)
    // GLFW's cross-platform glfwSetWindowIcon takes RGBA pixels — we'd need
    // a decoder to feed it an .ico. On Windows the embedded Win32 resource
    // is already in the right format for WM_SETICON, so we use it directly.
    // On Linux the window manager picks a default icon for now.
    {
        HWND hwnd = glfwGetWin32Window(window_);
        HMODULE hMod = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                             reinterpret_cast<LPCWSTR>(&ViewerApp::FramebufferSizeCallback),
                             &hMod);
        HINSTANCE hInst = hMod ? reinterpret_cast<HINSTANCE>(hMod) : ::GetModuleHandle(nullptr);
        HICON hIcon = ::LoadIconW(hInst, MAKEINTRESOURCEW(IDI_WHITEOUT_ICON));
        if (hIcon) {
            ::SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
            ::SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
        }
        // Match the title bar + thin window border to the ImGui MenuBarBg so
        // the OS chrome blends with the menu strip below it. Silently ignored
        // on older Windows.
        const BOOL useDark = TRUE;
        const COLORREF chrome = RGB(38, 45, 56);
        ::DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
        ::DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &chrome, sizeof(chrome));
        ::DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &chrome, sizeof(chrome));
    }
#endif

    // ImGui context must exist *before* Pipeline.InitDevice() because
    // InitBlsShaders calls RenderService::EnsureImGui, which constructs
    // ImGuiRenderer and (on the first Render() call) queries io.Fonts.
    InitImGui();

    if (!service_.Pipeline().InitDevice(api)) {
        std::fprintf(stderr, "Pipeline().InitDevice FAILED\n");
        Close();
        return false;
    }

    i32 fbW = width;
    i32 fbH = height;
    glfwGetFramebufferSize(window_, &fbW, &fbH);
    if (fbW <= 0)
        fbW = width;
    if (fbH <= 0)
        fbH = height;

    // Build the swap-chain handle the gfx layer expects per backend.
    //   • d3d11/d3d12: HWND
    //   • vulkan on Windows: HWND (gfx creates the Win32 surface internally)
    //   • vulkan on Linux: a pre-built VkSurfaceKHR from glfwCreateWindowSurface
    //     (so gfx doesn't have to branch on xcb / xlib / wayland itself)
    //   • webgpu on non-Windows: the GLFWwindow* itself — the WebGPU
    //     backend pulls the platform-specific handles (Display+Window /
    //     wl_display+wl_surface / NSWindow) via glfw3native.h, so the
    //     viewer doesn't have to duplicate the X11/Wayland/Cocoa branches.
    void* swapHandle = nullptr;
#if !defined(_WIN32)
    if (api == gfx::GfxApi::Vulkan) {
        gfx::IGFXDevice* dev = service_.Pipeline().Gfx();
        VkInstance instance = dev ? static_cast<VkInstance>(dev->GetNativeInstance()) : nullptr;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        if (!instance ||
            glfwCreateWindowSurface(instance, window_, nullptr, &surface) != VK_SUCCESS) {
            std::fprintf(stderr, "glfwCreateWindowSurface FAILED\n");
            Close();
            return false;
        }
        // VkSurfaceKHR is a 64-bit non-dispatchable handle on x86_64; pack it
        // into the void* the gfx interface expects.
        std::memcpy(&swapHandle, &surface, sizeof(swapHandle));
    } else if (api == gfx::GfxApi::WebGPU) {
        swapHandle = static_cast<void*>(window_);
    } else {
        std::fprintf(stderr, "Backend not supported on this platform\n");
        Close();
        return false;
    }
#else
    swapHandle = static_cast<void*>(glfwGetWin32Window(window_));
#endif

    targetId_ = service_.Pipeline().CreateSwapChainTarget(swapHandle, fbW, fbH);
    if (targetId_ == 0) {
        std::fprintf(stderr, "CreateSwapChainTarget FAILED\n");
        Close();
        return false;
    }
    service_.Pipeline().SetPrimaryTarget(targetId_);
    lastFbW_ = fbW;
    lastFbH_ = fbH;

    return true;
}

void ViewerApp::Close() {
    if (imguiInitialised_) {
        ShutdownImGui();
    }
    if (service_.Pipeline().IsDeviceReady()) {
        service_.Pipeline().Shutdown();
    }
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
        glfwTerminate();
    }
    targetId_ = 0;
}

bool ViewerApp::ShouldClose() const {
    return !window_ || glfwWindowShouldClose(window_);
}

void ViewerApp::InitImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // imgui.ini lives alongside the exe (next to settings ini) — letting
    // ImGui pick up its own default `imgui.ini` in CWD is fine for now.

    ApplyImGuiTheme();

    // Scale fonts + style sizes to the current monitor's content scale.
    // Glyphs rasterise at scaled pixel size (via style.FontScaleDpi) instead
    // of being bilinear-upsampled, which is what fixes "weird" text on 4K
    // and 1080p-scaled displays.
    float xs = 1.0f;
    float ys = 1.0f;
    glfwGetWindowContentScale(window_, &xs, &ys);
    ApplyImGuiDpiScale(xs);

    // GLFW backend handles input only; the engine adapter draws.
    ImGui_ImplGlfw_InitForOther(window_, true);
    imguiInitialised_ = true;
}

void ViewerApp::ShutdownImGui() {
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    imguiInitialised_ = false;
}

void ViewerApp::SetLoopNonLoopingPolicy(bool on) {
    loopNonLoopingPolicy_ = on;
    for (auto& [h, mi] : service_.Scene().Actors().All()) {
        if (mi->IsChild())
            continue;
        mi->ignoreNonLooping = on;
    }
}

model::Actor* ViewerApp::FocusActorPtr() const {
    return service_.Scene().Actors().Find(focusActor_);
}

void ViewerApp::FramebufferSizeCallback(GLFWwindow* w, int width, int height) {
    auto* self = static_cast<ViewerApp*>(glfwGetWindowUserPointer(w));
    if (self)
        self->OnFramebufferResize(width, height);
}
void ViewerApp::MouseButtonCallback(GLFWwindow* w, int button, int action, int /*mods*/) {
    auto* self = static_cast<ViewerApp*>(glfwGetWindowUserPointer(w));
    if (self)
        self->OnMouseButton(button, action);
}
void ViewerApp::CursorPosCallback(GLFWwindow* w, double x, double y) {
    auto* self = static_cast<ViewerApp*>(glfwGetWindowUserPointer(w));
    if (self)
        self->OnCursorPos(x, y);
}
void ViewerApp::ScrollCallback(GLFWwindow* w, double /*xoff*/, double yoff) {
    auto* self = static_cast<ViewerApp*>(glfwGetWindowUserPointer(w));
    if (self)
        self->OnScroll(yoff);
}

void ViewerApp::OnFramebufferResize(i32 w, i32 h) {
    if (w <= 0 || h <= 0)
        return;
    if (!service_.Pipeline().IsDeviceReady())
        return;
    if (w == lastFbW_ && h == lastFbH_)
        return;
    service_.Pipeline().ResizePrimaryTarget(w, h);
    lastFbW_ = w;
    lastFbH_ = h;
}

void ViewerApp::OnMouseButton(i32 button, i32 action) {
    // ImGui's GLFW backend already routes events into ImGui IO. We gate
    // camera input on WantCaptureMouse so clicks inside an ImGui window
    // don't double up.
    if (ImGui::GetCurrentContext()) {
        if (ImGui::GetIO().WantCaptureMouse) {
            // Forget any in-flight drag so releasing the button outside an
            // ImGui window doesn't snap the camera.
            lmbDown_ = rmbDown_ = mmbDown_ = false;
            return;
        }
    }

    f64 mx = 0.0, my = 0.0;
    glfwGetCursorPos(window_, &mx, &my);
    const bool pressed = (action == GLFW_PRESS);

    // ViewCube clicks no longer get a dedicated branch here — the host
    // overlays an invisible ImGui button on the cube region (see
    // ViewerUI::BuildViewCubeWidget). When that button is hovered ImGui's
    // WantCaptureMouse short-circuits the camera handler above, and the
    // widget itself performs the hit-test + camera snap.
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        lmbDown_ = pressed;
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        rmbDown_ = pressed;
    } else if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        mmbDown_ = pressed;
    }
    lastMouseX_ = mx;
    lastMouseY_ = my;
}

void ViewerApp::OnCursorPos(f64 x, f64 y) {
    const f64 dx = x - lastMouseX_;
    const f64 dy = y - lastMouseY_;
    lastMouseX_ = x;
    lastMouseY_ = y;

    if (cameraLocked_)
        return;
    if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse)
        return;

    auto& cam = service_.Scene().Camera();
    if (lmbDown_)
        cam.Rotate(static_cast<i32>(dx), static_cast<i32>(dy));
    if (rmbDown_)
        cam.Pan(static_cast<i32>(-dx), static_cast<i32>(dy));
    if (mmbDown_)
        cam.ZoomSmooth(static_cast<f32>(dy) * cam.GetDistance() / Camera::kFactorRelDist);
}

void ViewerApp::OnScroll(f64 yoffset) {
    if (cameraLocked_)
        return;
    if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse)
        return;
    service_.Scene().Camera().Zoom(static_cast<i32>(yoffset * 30.0));
}

bool ViewerApp::LoadModel(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        std::fprintf(stderr, "[viewer] file not found: %s\n", io::PathToUtf8(path).c_str());
        return false;
    }

    service_.Scene().SetPE1BasePath(path.parent_path());

    service_.Loader().RequestClearAll();
    model::Actor* hero = service_.Loader().SpawnUnit(io::PathToUtf8(path));
    if (!hero) {
        std::fprintf(stderr, "[viewer] SpawnUnit FAILED for %s\n", io::PathToUtf8(path).c_str());
        return false;
    }
    focusActor_ = hero->handle;
    hero->ignoreNonLooping = loopNonLoopingPolicy_;
    service_.Settings().SetRenderMode(hero->PreferredRenderMode());

    auto sequences = hero->animation.Sequences();
    sequenceNames_.clear();
    sequenceRanges_.clear();
    sequenceNames_.reserve(sequences.size());
    sequenceRanges_.reserve(sequences.size());
    for (auto& s : sequences) {
        sequenceNames_.push_back(s.name);
        sequenceRanges_.push_back(s);
    }
    if (!sequences.empty())
        hero->animation.SetActiveSequenceIndex(0);

    service_.Scene().Camera().SetPitch(30.0f);
    service_.Scene().Camera().SetYaw(45.0f);
    service_.Scene().Camera().SetDistance(300.0f);
    service_.Scene().Camera().SetTarget(0, 0, 50.0f);

    cameraPresets_.clear();
    if (hero->sourceTemplate)
        cameraPresets_ = hero->sourceTemplate->cameraPresets;
    cameraPresetNamesUtf8_.clear();
    cameraPresetNamesUtf8_.reserve(cameraPresets_.size());
    for (const auto& p : cameraPresets_)
        cameraPresetNamesUtf8_.push_back(p.name); // CameraPreset::name is already UTF-8.
    activeCameraPresetIdx_ = -1;
    cameraLocked_ = false;
    walkDriftPrevSeqIdx_ = -1;
    walkDriftAccumulated_ = 0.0f;

    currentModelPath_ = path;
    return true;
}

void ViewerApp::ActivateCameraPreset(i32 idx) {
    auto& cam = service_.Scene().Camera();

    if (idx < 0 || idx >= static_cast<i32>(cameraPresets_.size())) {
        activeCameraPresetIdx_ = -1;
        cameraLocked_ = false;
        cam.SetOrbitalMode();
        cam.SetFovDiagonal(Camera::kDefaultFovDiagonal);
        cam.SetClip(Camera::kDefaultNearZ, Camera::kDefaultFarZ);
        return;
    }

    activeCameraPresetIdx_ = idx;
    const CameraPreset& preset = cameraPresets_[idx];
    cameraLocked_ = preset.isLive;

    Vector3f pos = preset.position;
    Vector3f tgt = preset.target;
    f32 roll = preset.staticRoll;
    if (preset.animator) {
        i32 seqStart = 0;
        i32 seqEnd = 0;
        model::Actor* focus = FocusActorPtr();
        const i32 seqIdx = focus ? focus->animation.ActiveSequenceIndex() : 0;
        if (seqIdx >= 0 && seqIdx < static_cast<i32>(sequenceRanges_.size())) {
            seqStart = sequenceRanges_[seqIdx].startMs;
            seqEnd = sequenceRanges_[seqIdx].endMs;
        }
        if (seqStart == 0 && seqEnd == 0)
            seqEnd = 1 << 30;
        const i32 sampleMs = focus ? focus->animation.TimeMs() : service_.Scene().GetAnimationTime();
        preset.animator(pos, tgt, roll, sampleMs, seqStart, seqEnd);
    }
    cam.SetDirectPose(pos, tgt, roll);
    const f32 fov = (preset.fovDiagonal > 1e-3f) ? preset.fovDiagonal : Camera::kDefaultFovDiagonal;
    cam.SetFovDiagonal(fov);
    cam.SetClip(preset.zNear, preset.zFar);
}

void ViewerApp::UpdateCameraPresetAnimator() {
    if (activeCameraPresetIdx_ < 0 ||
        activeCameraPresetIdx_ >= static_cast<i32>(cameraPresets_.size()))
        return;
    const CameraPreset& preset = cameraPresets_[activeCameraPresetIdx_];
    if (!preset.animator)
        return;

    model::Actor* focus = FocusActorPtr();
    const i32 seqIdx = focus ? focus->animation.ActiveSequenceIndex() : 0;
    i32 seqStart = 0;
    i32 seqEnd = 0;
    if (seqIdx >= 0 && seqIdx < static_cast<i32>(sequenceRanges_.size())) {
        seqStart = sequenceRanges_[seqIdx].startMs;
        seqEnd = sequenceRanges_[seqIdx].endMs;
    }
    if (seqStart == 0 && seqEnd == 0)
        seqEnd = 1 << 30;

    Vector3f pos = preset.position;
    Vector3f tgt = preset.target;
    f32 roll = preset.staticRoll;
    const i32 sampleMs = focus ? focus->animation.TimeMs() : service_.Scene().GetAnimationTime();
    preset.animator(pos, tgt, roll, sampleMs, seqStart, seqEnd);
    service_.Scene().Camera().SetDirectPose(pos, tgt, roll);
}

void ViewerApp::Tick(f32 dt) {
    glfwPollEvents();
    if (!window_ || glfwWindowShouldClose(window_))
        return;

    // Per-frame size sync. The framebuffer-size callback alone isn't
    // reliable — GLFW on Windows can swallow callbacks during the maximize
    // transition's modal sizing loop, and HiDPI display changes also slip
    // through. Comparing against the last sized value is cheap and catches
    // every missed event. Skip the rest of the frame when minimised
    // (width or height 0) — swap chains hate zero extents.
    {
        i32 fbW = 0;
        i32 fbH = 0;
        glfwGetFramebufferSize(window_, &fbW, &fbH);
        if (fbW <= 0 || fbH <= 0)
            return;
        if ((fbW != lastFbW_ || fbH != lastFbH_) && service_.Pipeline().IsDeviceReady()) {
            service_.Pipeline().ResizePrimaryTarget(fbW, fbH);
            lastFbW_ = fbW;
            lastFbH_ = fbH;
        }
    }

    // ---- Walk-drift along the camera's X axis (orbital mode only) ----
    constexpr f32 kDefaultWalkSpeed = 100.0f;
    auto effectiveMoveSpeed = [](const SequenceInfo& s) {
        if (!ContainsCi(s.name, "walk"))
            return 0.0f;
        return s.moveSpeed != 0.0f ? s.moveSpeed : kDefaultWalkSpeed;
    };

    auto* hero = FocusActorPtr();
    if (hero && service_.Scene().Camera().GetMode() == Camera::Mode::Orbital) {
        const i32 idx = hero->animation.ActiveSequenceIndex();
        f32 delta = 0.0f;
        if (idx != walkDriftPrevSeqIdx_) {
            delta = -walkDriftAccumulated_;
            walkDriftPrevSeqIdx_ = idx;
        } else if (idx >= 0 && idx < static_cast<i32>(sequenceRanges_.size())) {
            const f32 ms = effectiveMoveSpeed(sequenceRanges_[idx]);
            if (ms != 0.0f)
                delta = ms * dt;
        }
        if (delta != 0.0f) {
            walkDriftAccumulated_ += delta;
            hero->worldTransform.data[3][0] += delta;
            const auto t = service_.Scene().Camera().GetTarget();
            service_.Scene().Camera().SetTarget(t.x + delta, t.y, t.z);
        }
    }

    if (auto* dnc = service_.GetDncService())
        dnc->Advance(dt);

    // ---- ImGui frame ----
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ui_->BuildFrame();
    ImGui::Render();

    // ---- Per-frame engine update ----
    const i32 curParentMs = service_.Scene().GetAnimationTime();
    i32 parentDtMs = curParentMs - lastParentTimeMs_;
    if (parentDtMs < 0)
        parentDtMs = 0;
    if (parentDtMs > 100)
        parentDtMs = 100;
    lastParentTimeMs_ = curParentMs;
    const f32 parentDt = static_cast<f32>(parentDtMs) / 1000.0f;

    UpdateCameraPresetAnimator();
    (void)service_.Replaceables().ConsumeDirty();
    (void)service_.Settings().ConsumeRenderModeDirty();

    // Push the camera pose to the sound emitter before the tick fires SND
    // events, so 3D-positioned event objects pan / attenuate against where
    // the camera is this frame.
    {
        const auto& cam = service_.Scene().Camera();
        const Vector3f eye = cam.GetSource();
        const Vector3f fwd = cam.GetTarget() - eye;
        service_.Sound().SetListener(eye, fwd, cam.GetUp());
    }

    service_.Ticker().Tick(parentDt);
    service_.Pipeline().RenderFrame(targetId_);
    service_.Pipeline().Present(targetId_);

    // ---- FPS title-bar update ----
    fpsAccum_ += static_cast<f64>(dt);
    fpsFrames_ += 1;
    if (fpsAccum_ >= 1.0) {
        i32 nGeo = 0, nTex = 0, nNodes = 0, nParts = 0, nSegs = 0;
        service_.Pipeline().GetFrameStats(nGeo, nTex, nNodes, nParts, nSegs);
        char title[300];
        std::snprintf(title, sizeof(title),
                      "WhiteoutFlakes — %d FPS | %d geo, %d tex, %d nodes, %d parts, %d segs",
                      fpsFrames_, nGeo, nTex, nNodes, nParts, nSegs);
        glfwSetWindowTitle(window_, title);
        fpsAccum_ = 0.0;
        fpsFrames_ = 0;
    }
}

} // namespace whiteout::flakes
