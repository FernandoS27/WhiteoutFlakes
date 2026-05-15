#pragma once

// ============================================================================
// RenderWindow — Win32-hosted preview window for the 3ds Max plugin.
//
// Wraps a single Win32 HWND owned by a render thread (so Max's UI thread
// stays free for ndxStart / TimeChanged / material polling). The whole
// client area is the swap chain target; the toolbar / menus / settings
// panel are Dear ImGui widgets drawn by the engine's BLS-backed ImGui
// adapter, with input forwarded through imgui_impl_win32.
//
// Cross-thread access pattern matches the original Win32 build: Max-thread
// callers go through SetCameraPresets / SetSequences / SetFocusActor, all of
// which take hostMutex_ (or a relaxed atomic for focusActor_); the UI runs
// entirely on the render thread and snapshots host state under the same
// lock.
// ============================================================================

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "model/actor_manager.h"
#include "render_target.h"
#include "whiteout/flakes/gfx_types.h"
#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <windows.h>

namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes {

using namespace whiteout::flakes::renderer;
using namespace whiteout::flakes::renderer::model;

class MaxPluginUI;

class RenderWindow {
public:
    explicit RenderWindow(RenderService& service);
    ~RenderWindow();

    bool Open(i32 width, i32 height, gfx::GfxApi api = gfx::GfxApi::D3D12);
    void Close();
    bool IsOpen() const;

    bool Create(i32 width, i32 height);
    void Destroy();
    void Show();

    bool PumpMessages();

    // Host-side authorship — Max main thread pushes presets/sequences/focus
    // into the render thread. The UI on the render thread reads these under
    // hostMutex_ (or relaxed atomic for focusActor_).
    void SetCameraPresets(std::vector<CameraPreset> presets);
    void SetSequences(std::vector<std::string> names, std::vector<SequenceInfo> ranges);
    void SetFocusActor(ActorId h) {
        focusActor_.store(h, std::memory_order_relaxed);
    }
    ActorId FocusActor() const {
        return focusActor_.load(std::memory_order_relaxed);
    }

    HWND GetParentHWND() const {
        return hwnd_;
    }
    HWND GetRenderHWND() const {
        return hwnd_;
    }

    void SetTitle(const wchar_t* title);

    RenderService& Service() {
        return service_;
    }
    const RenderService& Service() const {
        return service_;
    }

    // ---- UI accessors (called by MaxPluginUI on the render thread) ----
    // Snapshots are taken under the lock to keep the UI's read consistent
    // even if the Max thread re-publishes mid-frame.
    std::vector<std::string> SequenceNamesSnapshot() const;
    std::vector<SequenceInfo> SequenceRangesSnapshot() const;
    std::vector<CameraPreset> CameraPresetsSnapshot() const;
    i32 ActiveCameraPresetIdx() const;
    bool CameraLocked() const;

    void ActivateCameraPreset(i32 idx);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void ThreadFunc(i32 width, i32 height, gfx::GfxApi api);

    void InitImGui();
    void ShutdownImGui();
    void UpdateCameraPresetAnimator();

    RenderService& service_;

    HWND hwnd_ = nullptr;

    HICON icon_ = nullptr;

    bool imguiInitialised_ = false;
    std::unique_ptr<MaxPluginUI> ui_;

    bool lmbDown_ = false, rmbDown_ = false, mmbDown_ = false;
    POINT lastMouse_ = {0, 0};

    // Cross-thread host state — Max main thread writes via the public
    // setters; render thread reads inside HandleMessage / MaxPluginUI.
    mutable std::mutex hostMutex_;
    std::vector<CameraPreset> cameraPresets_;
    i32 activeCameraPresetIdx_ = -1;
    bool cameraLocked_ = false;

    std::vector<std::string> sequenceNames_;
    std::vector<SequenceInfo> sequenceRanges_;

    std::atomic<ActorId> focusActor_{0};

    std::thread renderThread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> initialized_{false};
    RenderTargetId targetId_ = 0;

    i32 lastFbW_ = 0;
    i32 lastFbH_ = 0;
};

} // namespace whiteout::flakes
