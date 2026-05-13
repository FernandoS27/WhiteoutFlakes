#pragma once

// ============================================================================
// ViewerApp — GLFW + Dear ImGui frontend for WhiteoutFlakes.
//
// Replaces the previous Win32 + Common Controls RenderWindow. Single-threaded:
// `Tick(dt)` polls GLFW, builds the ImGui frame, runs the engine's per-frame
// update + RenderFrame + Present, all on the calling thread. test_main owns
// the outer loop.
//
// All host-side UI policy (camera presets dropdown, sequence dropdown, focus
// actor, tileset radio) lives in ViewerUI (sibling .cpp), keeping ViewerApp
// focused on lifetime + dispatch.
// ============================================================================

#include "model/actor_manager.h"
#include "render_target.h"
#include "whiteout/flakes/gfx_types.h"
#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

namespace whiteout::flakes::renderer {
class RenderService;
} // namespace whiteout::flakes::renderer

namespace whiteout::flakes {

using namespace whiteout::flakes::renderer;
using namespace whiteout::flakes::renderer::model;

class ViewerUI;

class ViewerApp {
public:
    explicit ViewerApp(RenderService& service);
    ~ViewerApp();

    ViewerApp(const ViewerApp&) = delete;
    ViewerApp& operator=(const ViewerApp&) = delete;

    bool Open(i32 width, i32 height, gfx::GfxApi api);
    void Close();

    bool ShouldClose() const;
    void Tick(f32 dt);

    // Load an MDX from disk and make it the focus actor. Clears any
    // previously-loaded scene. Safe to call repeatedly (used by File > Open).
    bool LoadModel(const std::filesystem::path& path);

    // ---- Host policy (toggled by the UI, read by the per-frame tick) ----
    bool LoopNonLoopingPolicy() const {
        return loopNonLoopingPolicy_;
    }
    void SetLoopNonLoopingPolicy(bool on);

    // ---- Camera presets ----
    void ActivateCameraPreset(i32 idx);
    i32 ActiveCameraPresetIdx() const {
        return activeCameraPresetIdx_;
    }
    const std::vector<CameraPreset>& CameraPresets() const {
        return cameraPresets_;
    }
    // UTF-8 view of CameraPreset::name (which is std::wstring), refreshed
    // on every LoadModel. ImGui takes UTF-8 only, so the UI consumes this
    // mirror instead of re-converting per frame.
    const std::vector<std::string>& CameraPresetNamesUtf8() const {
        return cameraPresetNamesUtf8_;
    }
    bool CameraLocked() const {
        return cameraLocked_;
    }

    // ---- Sequences (per focus actor) ----
    const std::vector<std::string>& SequenceNames() const {
        return sequenceNames_;
    }
    const std::vector<SequenceInfo>& SequenceRanges() const {
        return sequenceRanges_;
    }

    // ---- Focus actor (the one driven by the sequence dropdown, team
    //      colour swatch, etc.) ----
    ActorId FocusActor() const {
        return focusActor_;
    }
    model::Actor* FocusActorPtr() const;

    RenderService& Service() {
        return service_;
    }
    const RenderService& Service() const {
        return service_;
    }

    GLFWwindow* Window() const {
        return window_;
    }
    gfx::GfxApi Backend() const {
        return backend_;
    }

    // Set by test_main right after LoadModel so the parent path becomes the
    // PE1 scan root (matches the old `scene.SetPE1BasePath(mdxPath.parent_path())`).
    void SetCurrentModelPath(std::filesystem::path p) {
        currentModelPath_ = std::move(p);
    }
    const std::filesystem::path& CurrentModelPath() const {
        return currentModelPath_;
    }

private:
    void InitImGui();
    void ShutdownImGui();

    void OnFramebufferResize(i32 w, i32 h);
    void OnMouseButton(i32 button, i32 action);
    void OnCursorPos(f64 x, f64 y);
    void OnScroll(f64 yoffset);
    void UpdateCameraPresetAnimator();

    static void FramebufferSizeCallback(GLFWwindow* w, int width, int height);
    static void MouseButtonCallback(GLFWwindow* w, int button, int action, int mods);
    static void CursorPosCallback(GLFWwindow* w, double x, double y);
    static void ScrollCallback(GLFWwindow* w, double xoff, double yoff);

    RenderService& service_;
    GLFWwindow* window_ = nullptr;
    gfx::GfxApi backend_ = gfx::GfxApi::D3D12;
    RenderTargetId targetId_ = 0;
    bool imguiInitialised_ = false;

    i32 lastFbW_ = 0;
    i32 lastFbH_ = 0;

    std::unique_ptr<ViewerUI> ui_;

    // ---- Host state ----
    bool loopNonLoopingPolicy_ = true;
    ActorId focusActor_ = 0;
    std::vector<CameraPreset> cameraPresets_;
    std::vector<std::string> cameraPresetNamesUtf8_;
    std::vector<std::string> sequenceNames_;
    std::vector<SequenceInfo> sequenceRanges_;
    i32 activeCameraPresetIdx_ = -1;
    bool cameraLocked_ = false;
    std::filesystem::path currentModelPath_;

    // ---- Input state ----
    bool lmbDown_ = false;
    bool rmbDown_ = false;
    bool mmbDown_ = false;
    f64 lastMouseX_ = 0.0;
    f64 lastMouseY_ = 0.0;

    // FPS counter (title bar)
    f64 fpsAccum_ = 0.0;
    i32 fpsFrames_ = 0;

    // Walk-drift state — used by Tick to advance the actor along
    // walk-cycle sequences, mirroring the old test_main logic.
    i32 walkDriftPrevSeqIdx_ = -1;
    f32 walkDriftAccumulated_ = 0.0f;

    // Last animation-time sample, for computing parentDt without a
    // duplicate animation-clock tick.
    i32 lastParentTimeMs_ = 0;

    friend class ViewerUI;
};

} // namespace whiteout::flakes
