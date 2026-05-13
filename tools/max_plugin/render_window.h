#pragma once

#include <atomic>
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

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes {

// Tools live at `whiteout::flakes` while the renderer types live in nested
// sub-namespaces. Re-export the renderer's commonly-used types so the tool's
// existing flat references continue to compile.
using namespace whiteout::flakes::renderer;
using namespace whiteout::flakes::renderer::model;

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

    // Host-side authorship: the application pushes loaded presets/sequences
    // and the focus handle into the viewer, which owns the dropdowns and the
    // per-frame camera animator update. These setters are safe to call from
    // the host main thread while the render thread is running — internally
    // they synchronize via hostMutex_ (focusActor_ uses a relaxed atomic).
    void SetCameraPresets(std::vector<CameraPreset> presets);
    void SetSequences(std::vector<std::string> names, std::vector<SequenceInfo> ranges);
    void SetFocusActor(ActorId h) {
        focusActor_.store(h, std::memory_order_relaxed);
    }
    ActorId FocusActor() const {
        return focusActor_.load(std::memory_order_relaxed);
    }
    std::vector<SequenceInfo> SequenceRanges() const; // returns a snapshot copy

    void SyncViewMenuFromService();

    HWND GetParentHWND() const {
        return hwnd_;
    }
    HWND GetRenderHWND() const {
        return hwndRender_;
    }

    void SetTitle(const wchar_t* title);

    void InvalidateTeamColorSwatch();

    i32 GetActiveCameraIndex() const;

    // App-side policy — when true, freshly-loaded actors get
    // actor->ignoreNonLooping = true so non-looping sequences hold their
    // last frame instead of restarting. Toggled by the settings checkbox;
    // applied to all actors on toggle, and to newly-loaded actors by the
    // application (test_main applies after Loader().SpawnUnit).
    bool LoopNonLoopingPolicy() const {
        return loopNonLoopingPolicy_;
    }
    void SetLoopNonLoopingPolicy(bool on) {
        loopNonLoopingPolicy_ = on;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK RenderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleSettingsMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    void ThreadFunc(i32 width, i32 height, gfx::GfxApi api);

    void EnsureSettingsWindow();

    // Apply preset `idx` (or -1 for free orbital camera) to the renderer's
    // Camera. Mirrors what SceneManager::ActivateCameraPreset used to do; the
    // viewer owns the per-frame animator sample inside the render thread.
    void ActivateCameraPreset(i32 idx);
    void UpdateCameraPresetAnimator();

    RenderService& service_;

    HWND hwnd_ = nullptr;
    HWND hwndRender_ = nullptr;

    HWND btnTeamColor_ = nullptr;
    HWND cmbCamera_ = nullptr;
    HWND lblSequence_ = nullptr;
    HWND cmbSequence_ = nullptr;
    HWND cmbLighting_ = nullptr;
    HWND btnBgColor_ = nullptr;
    HWND sldExposure_ = nullptr;
    HWND lblExposure_ = nullptr;
    HWND sldSndVolume_ = nullptr;
    HWND lblSndVolume_ = nullptr;
    HWND chkLoopNonLoop_ = nullptr;
    HWND sldTimeOfDay_ = nullptr;
    HWND lblTimeOfDay_ = nullptr;
    HWND chkAnimateTod_ = nullptr;
    HWND editDncPath_ = nullptr;
    HWND btnDncReset_ = nullptr;
    HWND cmbIblMode_ = nullptr;
    HWND cmbShadows_ = nullptr;
    HWND cmbDefaultBackend_ = nullptr;
    HWND chkGraphicsDebug_ = nullptr;
    HWND cmbPreferredDev_ = nullptr;
    HMENU hMenuBar_ = nullptr;
    HMENU hMenuView_ = nullptr;
    HMENU hMenuTileset_ = nullptr;
    HMENU hMenuDebug_ = nullptr;
    HMENU hMenuDebugVis_ = nullptr;
    HMENU hMenuLod_ = nullptr;

    HWND hwndSettings_ = nullptr;

    enum : UINT {
        IDC_TEAMCOLOR = 1001,
        IDC_CAMERA,
        IDC_SEQUENCE,
        IDC_LIGHTING,
        IDC_BGCOLOR,
        IDC_EXPOSURE,
        IDC_SND_VOLUME,
        IDC_LOOP_NONLOOP,
        IDC_TIME_OF_DAY,
        IDC_ANIMATE_TOD,
        IDC_DNC_PATH,
        IDC_DNC_RESET,
        IDC_IBL_MODE,
        IDC_SHADOWS,
        IDC_DEFAULT_BACKEND,
        IDC_GRAPHICS_DEBUG,
        IDC_PREFERRED_DEVICE,

        IDM_VIEW_GRID = 1100,
        IDM_VIEW_PARTICLES,
        IDM_VIEW_RIBBONS,
        IDM_VIEW_EVENTS,

        IDM_DBG_COLLISIONS = 1200,
        IDM_DBG_LIGHTS,

        IDM_TILESET_BASE = 1310,
        IDM_TILESET_LAST = IDM_TILESET_BASE + 15,

        IDM_DBGVIS_BASE = 1400,
        IDM_DBGVIS_LAST = IDM_DBGVIS_BASE + 7,

        IDM_LOD_BASE = 1500,
        IDM_LOD_LAST = IDM_LOD_BASE + 4,

        IDM_SETTINGS = 1600,
    };

    bool lmbDown_ = false, rmbDown_ = false, mmbDown_ = false;
    POINT lastMouse_ = {0, 0};

    // App-side default for actor->ignoreNonLooping. UI checkbox toggles this
    // and re-applies to every actor in the scene. Settings ini persists it.
    bool loopNonLoopingPolicy_ = true;

    HICON icon_ = nullptr;

    // Cross-thread host state: the test_main main thread writes via
    // SetCameraPresets / SetSequences during init while the render thread
    // (this RenderWindow's ThreadFunc) reads in UpdateCameraPresetAnimator.
    // A single mutex serializes; UI dropdown manipulation happens before the
    // lock is acquired since SendMessageW is already serialized by the
    // message pump.
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

    static constexpr i32 kToolbarH = 28;
};

} // namespace whiteout::flakes
