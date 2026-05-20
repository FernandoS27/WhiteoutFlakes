#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

// clang-format off
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
// clang-format on
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

#include "imgui_theme.h"
#include "max_plugin_ui.h"
#include "render_window.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/camera.h"
#include "renderer/debug/debug_renderer.h"
#include "renderer/frame_ticker.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "resource.h"
#include "settings_ini.h"
#include "whiteout/flakes/sound_emitter.h"
#include "whiteout/flakes/types.h"

#include <imgui.h>
#include <imgui_impl_win32.h>

#if defined(TRACY_ENABLE)
#include <tracy/Tracy.hpp>
#endif

// imgui_impl_win32.h asks callers to forward-declare this — see the comment
// at the top of that header.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam,
                                                             LPARAM lParam);

namespace whiteout::flakes {

using namespace whiteout::flakes::renderer;
using namespace whiteout::flakes::renderer::model;

static const wchar_t* WINDOW_CLASS = L"WhiteoutDexRendererClass";
static const wchar_t* WINDOW_TITLE = L"WhiteoutFlakes";

RenderWindow::RenderWindow(RenderService& service) : service_(service) {}

RenderWindow::~RenderWindow() {
    Close();
    Destroy();
}

bool RenderWindow::Open(i32 w, i32 h, gfx::GfxApi api) {
    if (running_)
        return true;

    if (renderThread_.joinable())
        renderThread_.join();
    running_ = true;
    initialized_ = false;
    renderThread_ = std::thread(&RenderWindow::ThreadFunc, this, w, h, api);
    for (i32 i = 0; i < 500 && !initialized_ && running_; ++i)
        Sleep(10);
    return initialized_;
}

void RenderWindow::Close() {
    running_ = false;
    if (renderThread_.joinable()) {
        if (hwnd_)
            PostMessage(hwnd_, WM_CLOSE, 0, 0);
        renderThread_.join();
    }
}

bool RenderWindow::IsOpen() const {
    return running_ && initialized_;
}

void RenderWindow::ThreadFunc(i32 w, i32 h, gfx::GfxApi api) {
    if (!Create(w, h)) {
        running_ = false;
        return;
    }

    InitImGui();

    if (!service_.Pipeline().InitDevice(api)) {
        ShutdownImGui();
        running_ = false;
        Destroy();
        return;
    }

    targetId_ = service_.Pipeline().CreateSwapChainTarget(static_cast<void*>(hwnd_), w, h);
    if (targetId_ == 0) {
        service_.Pipeline().Shutdown();
        ShutdownImGui();
        running_ = false;
        Destroy();
        return;
    }
    service_.Pipeline().SetPrimaryTarget(targetId_);
    lastFbW_ = w;
    lastFbH_ = h;

    Show();
    initialized_ = true;

    LARGE_INTEGER freq, lastTime, now, fpsTimer;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&lastTime);
    fpsTimer = lastTime;
    i32 frameCount = 0;
    i32 lastParentTimeMs = service_.Scene().GetAnimationTime();

    while (running_) {
        {
#if defined(TRACY_ENABLE)
            ZoneScopedN("PumpMessages");
#endif
            if (!PumpMessages()) {
                running_ = false;
                break;
            }
        }

        // Skip the frame entirely while minimised — swap chains hate zero
        // extents, and ImGui's Win32 backend reads the current client rect
        // each NewFrame so we'd hit a zero display size too.
        RECT cr{};
        if (hwnd_ && GetClientRect(hwnd_, &cr)) {
            const i32 cw = cr.right - cr.left;
            const i32 ch = cr.bottom - cr.top;
            if (cw <= 0 || ch <= 0) {
                Sleep(16);
                continue;
            }
        }

        QueryPerformanceCounter(&now);
        lastTime = now;

        i32 curParentMs = service_.Scene().GetAnimationTime();
        i32 parentDtMs = curParentMs - lastParentTimeMs;
        if (parentDtMs < 0)
            parentDtMs = 0;
        if (parentDtMs > 100)
            parentDtMs = 100;
        lastParentTimeMs = curParentMs;
        f32 parentDt = (f32)parentDtMs / 1000.0f;

        // Drain content-provider completions (texture stubs → real pixels,
        // MDX Wait() wake-ups, etc.) on the render thread before per-frame
        // asset access reads the results.
        if (auto* cp = service_.Scene().ActiveContentProvider())
            cp->Pump();

        UpdateCameraPresetAnimator();
        (void)service_.Replaceables().ConsumeDirty();
        (void)service_.Settings().ConsumeRenderModeDirty();

        // Camera pose → sound emitter, before the tick fires SND events.
        {
            const auto& cam = service_.Scene().Camera();
            const Vector3f eye = cam.GetSource();
            const Vector3f fwd = cam.GetTarget() - eye;
            service_.Sound().SetListener(eye, fwd, cam.GetUp());
        }

        // ---- ImGui frame ----
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (ui_)
            ui_->BuildFrame();
        ImGui::Render();

        {
#if defined(TRACY_ENABLE)
            ZoneScopedN("Ticker.Tick");
#endif
            service_.Ticker().Tick(parentDt);
        }
        {
#if defined(TRACY_ENABLE)
            ZoneScopedN("Pipeline.RenderFrame");
#endif
            service_.Pipeline().RenderFrame(targetId_);
        }
        {
#if defined(TRACY_ENABLE)
            ZoneScopedN("Pipeline.Present");
#endif
            service_.Pipeline().Present(targetId_);
        }
        frameCount++;

        f64 fpsDt = (f64)(now.QuadPart - fpsTimer.QuadPart) / freq.QuadPart;
        if (fpsDt >= 1.0) {
            i32 nGeo = 0, nTex = 0, nNodes = 0, nParts = 0, nSegs = 0;
            service_.Pipeline().GetFrameStats(nGeo, nTex, nNodes, nParts, nSegs);
            wchar_t title[300];
            swprintf_s(
                title,
                L"WhiteoutFlakes — %d FPS | %d geo, %d tex, %d nodes, %d parts, %d segs",
                frameCount, nGeo, nTex, nNodes, nParts, nSegs);
            SetTitle(title);
            frameCount = 0;
            fpsTimer = now;
        }
    }

    service_.Pipeline().Shutdown();
    ShutdownImGui();
    Destroy();
    initialized_ = false;
}

bool RenderWindow::Create(i32 w, i32 h) {
    HMODULE hMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&RenderWindow::WndProc,
                       &hMod);
    HINSTANCE hInst = hMod ? (HINSTANCE)hMod : GetModuleHandle(nullptr);

    icon_ = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_WHITEOUT_ICON));

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = RenderWindow::WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = icon_;
    wc.hIconSm = icon_;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WINDOW_CLASS;
    if (!RegisterClassExW(&wc))
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return false;

    RECT adj = {0, 0, w, h};
    AdjustWindowRect(&adj, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd_ = CreateWindowExW(0, WINDOW_CLASS, WINDOW_TITLE, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, adj.right - adj.left, adj.bottom - adj.top, nullptr,
                            nullptr, hInst, this);
    if (hwnd_) {
        // Match the title bar + thin window border to the ImGui MenuBarBg so
        // the OS chrome blends with the menu strip below it. Silently ignored
        // on older Windows.
        const BOOL useDark = TRUE;
        const COLORREF chrome = RGB(38, 45, 56);
        DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
        DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR, &chrome, sizeof(chrome));
        DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &chrome, sizeof(chrome));
    }
    return hwnd_ != nullptr;
}

void RenderWindow::Destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (icon_) {
        DestroyIcon(icon_);
        icon_ = nullptr;
    }
    UnregisterClassW(WINDOW_CLASS, GetModuleHandle(nullptr));
}

void RenderWindow::Show() {
    if (hwnd_) {
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
    }
}

void RenderWindow::InitImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ApplyImGuiTheme();

    // 3ds Max marks the process per-monitor DPI-aware, so the HWND already
    // sits at native pixel density — we just need to scale fonts + style to
    // match. GetDpiForWindow requires Win10 1607+, which any modern Max ships
    // on; the fallback covers older SDKs / Max versions.
    UINT dpi = 96;
    using GetDpiFn = UINT(WINAPI*)(HWND);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    auto getDpi =
        u32 ? reinterpret_cast<GetDpiFn>(GetProcAddress(u32, "GetDpiForWindow")) : nullptr;
    if (getDpi)
        dpi = getDpi(hwnd_);
    ApplyImGuiDpiScale(static_cast<float>(dpi) / 96.0f);

    ImGui_ImplWin32_Init(hwnd_);
    ui_ = std::make_unique<MaxPluginUI>(*this);
    imguiInitialised_ = true;
}

void RenderWindow::ShutdownImGui() {
    if (!imguiInitialised_)
        return;
    ui_.reset();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    imguiInitialised_ = false;
}

bool RenderWindow::PumpMessages() {
    MSG msg = {};
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT)
            return false;
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return true;
}

LRESULT CALLBACK RenderWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    RenderWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        self = static_cast<RenderWindow*>(cs->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<RenderWindow*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }
    if (self)
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT RenderWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Forward every message to ImGui first. Returning TRUE means ImGui
    // wants to consume it (e.g. clicked inside an ImGui window).
    if (imguiInitialised_ && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return 0;

    const ImGuiIO* io = imguiInitialised_ ? &ImGui::GetIO() : nullptr;
    const bool imguiWantsMouse = io && io->WantCaptureMouse;
    const bool imguiWantsKeys = io && io->WantCaptureKeyboard;

    switch (msg) {
    case WM_SIZE: {
        i32 w = LOWORD(lParam), h = HIWORD(lParam);
        if (w > 0 && h > 0 && service_.Pipeline().IsDeviceReady() &&
            (w != lastFbW_ || h != lastFbH_)) {
            service_.Pipeline().ResizePrimaryTarget(w, h);
            lastFbW_ = w;
            lastFbH_ = h;
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (imguiWantsMouse)
            break;
        lmbDown_ = true;
        lastMouse_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        SetCapture(hwnd);
        return 0;
    case WM_LBUTTONUP:
        lmbDown_ = false;
        if (!rmbDown_ && !mmbDown_)
            ReleaseCapture();
        return 0;
    case WM_RBUTTONDOWN:
        if (imguiWantsMouse)
            break;
        rmbDown_ = true;
        lastMouse_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        SetCapture(hwnd);
        return 0;
    case WM_RBUTTONUP:
        rmbDown_ = false;
        if (!lmbDown_ && !mmbDown_)
            ReleaseCapture();
        return 0;
    case WM_MBUTTONDOWN:
        if (imguiWantsMouse)
            break;
        mmbDown_ = true;
        lastMouse_ = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        SetCapture(hwnd);
        return 0;
    case WM_MBUTTONUP:
        mmbDown_ = false;
        if (!lmbDown_ && !rmbDown_)
            ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE: {
        POINT cur = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        i32 dx = cur.x - lastMouse_.x, dy = cur.y - lastMouse_.y;
        lastMouse_ = cur;

        if (imguiWantsMouse)
            return 0;
        bool locked;
        {
            std::lock_guard<std::mutex> lk(hostMutex_);
            locked = cameraLocked_;
        }
        if (!locked) {
            auto& cam = service_.Scene().Camera();
            if (lmbDown_)
                cam.Rotate(dx, dy);
            if (rmbDown_)
                cam.Pan(-dx, dy);
            if (mmbDown_)
                cam.ZoomSmooth(f32(dy) * cam.GetDistance() /
                               whiteout::flakes::renderer::Camera::kFactorRelDist);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (imguiWantsMouse)
            return 0;
        bool locked;
        {
            std::lock_guard<std::mutex> lk(hostMutex_);
            locked = cameraLocked_;
        }
        if (!locked) {
            i32 delta = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            service_.Scene().Camera().Zoom(delta * 30);
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (imguiWantsKeys)
            break;
        return 0;
    case WM_DESTROY:
        hwnd_ = nullptr;
        running_ = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void RenderWindow::SetCameraPresets(std::vector<CameraPreset> presets) {
    std::lock_guard<std::mutex> lk(hostMutex_);
    cameraPresets_ = std::move(presets);
    activeCameraPresetIdx_ = -1;
    cameraLocked_ = false;
}

void RenderWindow::SetSequences(std::vector<std::string> names, std::vector<SequenceInfo> ranges) {
    std::lock_guard<std::mutex> lk(hostMutex_);
    sequenceNames_ = std::move(names);
    sequenceRanges_ = std::move(ranges);
}

std::vector<std::string> RenderWindow::SequenceNamesSnapshot() const {
    std::lock_guard<std::mutex> lk(hostMutex_);
    return sequenceNames_;
}

std::vector<SequenceInfo> RenderWindow::SequenceRangesSnapshot() const {
    std::lock_guard<std::mutex> lk(hostMutex_);
    return sequenceRanges_;
}

std::vector<CameraPreset> RenderWindow::CameraPresetsSnapshot() const {
    std::lock_guard<std::mutex> lk(hostMutex_);
    return cameraPresets_;
}

i32 RenderWindow::ActiveCameraPresetIdx() const {
    std::lock_guard<std::mutex> lk(hostMutex_);
    return activeCameraPresetIdx_;
}

bool RenderWindow::CameraLocked() const {
    std::lock_guard<std::mutex> lk(hostMutex_);
    return cameraLocked_;
}

void RenderWindow::ActivateCameraPreset(i32 idx) {
    auto& cam = service_.Scene().Camera();

    CameraPreset preset;
    bool haveValidPreset = false;
    {
        std::lock_guard<std::mutex> lk(hostMutex_);
        if (idx < 0 || idx >= (i32)cameraPresets_.size()) {
            activeCameraPresetIdx_ = -1;
            cameraLocked_ = false;
        } else {
            preset = cameraPresets_[idx];
            activeCameraPresetIdx_ = idx;
            cameraLocked_ = preset.isLive;
            haveValidPreset = true;
        }
    }

    if (!haveValidPreset) {
        cam.SetOrbitalMode();
        cam.SetFovDiagonal(Camera::kDefaultFovDiagonal);
        cam.SetClip(Camera::kDefaultNearZ, Camera::kDefaultFarZ);
        return;
    }

    Vector3f pos = preset.position;
    Vector3f tgt = preset.target;
    f32 roll = preset.staticRoll;
    if (preset.animator) {
        i32 seqStart = 0, seqEnd = 0;
        auto* focus = service_.Scene().Actors().Find(focusActor_.load(std::memory_order_relaxed));
        i32 seqIdx = focus ? focus->animation.ActiveSequenceIndex() : 0;
        {
            std::lock_guard<std::mutex> lk(hostMutex_);
            if (seqIdx >= 0 && seqIdx < (i32)sequenceRanges_.size()) {
                seqStart = sequenceRanges_[seqIdx].startMs;
                seqEnd = sequenceRanges_[seqIdx].endMs;
            }
        }
        if (seqStart == 0 && seqEnd == 0)
            seqEnd = 1 << 30;
        const i32 sampleMs =
            focus ? focus->animation.TimeMs() : service_.Scene().GetAnimationTime();
        preset.animator(pos, tgt, roll, sampleMs, seqStart, seqEnd);
    }

    cam.SetDirectPose(pos, tgt, roll);
    const f32 fov = (preset.fovDiagonal > 1e-3f) ? preset.fovDiagonal : Camera::kDefaultFovDiagonal;
    cam.SetFovDiagonal(fov);
    cam.SetClip(preset.zNear, preset.zFar);
}

void RenderWindow::UpdateCameraPresetAnimator() {
    CameraPreset preset;
    i32 seqStart = 0, seqEnd = 0;
    {
        std::lock_guard<std::mutex> lk(hostMutex_);
        if (activeCameraPresetIdx_ < 0 || activeCameraPresetIdx_ >= (i32)cameraPresets_.size())
            return;
        preset = cameraPresets_[activeCameraPresetIdx_];
        if (!preset.animator)
            return;

        auto* focus = service_.Scene().Actors().Find(focusActor_.load(std::memory_order_relaxed));
        const i32 seqIdx = focus ? focus->animation.ActiveSequenceIndex() : 0;
        if (seqIdx >= 0 && seqIdx < (i32)sequenceRanges_.size()) {
            seqStart = sequenceRanges_[seqIdx].startMs;
            seqEnd = sequenceRanges_[seqIdx].endMs;
        }
    }
    if (seqStart == 0 && seqEnd == 0)
        seqEnd = 1 << 30;

    auto* focus = service_.Scene().Actors().Find(focusActor_.load(std::memory_order_relaxed));
    Vector3f pos = preset.position;
    Vector3f tgt = preset.target;
    f32 roll = preset.staticRoll;
    const i32 sampleMs = focus ? focus->animation.TimeMs() : service_.Scene().GetAnimationTime();
    preset.animator(pos, tgt, roll, sampleMs, seqStart, seqEnd);
    service_.Scene().Camera().SetDirectPose(pos, tgt, roll);
}

void RenderWindow::SetTitle(const wchar_t* title) {
    if (hwnd_)
        SetWindowTextW(hwnd_, title);
}

} // namespace whiteout::flakes
