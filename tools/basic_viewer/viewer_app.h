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

// Output format for an animation export. The enum order is the canonical
// order used by the UI format dropdown and by GetExportFormatInfo().
enum class ExportFormat {
    PngFrames, ///< One <model>_<anim>_<id>.png per frame.
    Gif,       ///< A single animated <model>_<anim>.gif.
    Apng,      ///< A single animated <model>_<anim>.apng (APNG).
    Webp,      ///< A single animated <model>_<anim>.webp.
};
constexpr i32 kExportFormatCount = 4;

// Static description of an ExportFormat — its human label and, for the
// single-file animated formats, the output file extension. PngFrames has an
// empty extension since it writes a numbered PNG per frame instead.
struct ExportFormatInfo {
    const char* label;     ///< e.g. "Animated WebP".
    const char* extension; ///< Single-file extension incl. dot; "" = per-frame PNGs.
};

// Look up the description of a format. Indexed by ExportFormat.
const ExportFormatInfo& GetExportFormatInfo(ExportFormat format);

// True for the animated single-file formats (GIF/APNG/WebP); false for PngFrames.
inline bool IsSingleFileFormat(ExportFormat format) {
    return GetExportFormatInfo(format).extension[0] != '\0';
}

// Parameters for an animation-frame export.
struct AnimationExportParams {
    i32 sequenceIndex = 0;
    i32 fps = 30;
    ExportFormat format = ExportFormat::PngFrames;
    // Transparent background: each frame is rendered twice (black + white
    // backdrop) and keyed, recovering a real alpha channel. PNG and APNG
    // carry it directly; GIF falls back to 1-bit transparency.
    bool transparentBackground = false;
    // Capture the viewer's ImGui UI overlay into each exported frame.
    bool captureUi = false;
    // Render resolution; 0×0 means "use the current view size".
    i32 width = 0;
    i32 height = 0;
    std::filesystem::path outputFolder;
};

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

    // ---- Animation frame export ----
    // Queue an animation export (see AnimationExportParams). Deferred: the UI
    // calls this from inside the ImGui frame, and Tick() runs the actual
    // render loop on the next tick (outside ImGui frame building).
    void RequestAnimationExport(AnimationExportParams params);

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

    // Runs a queued animation export synchronously: drives the focus actor
    // through the sequence one frame at a time, captures each composited
    // frame, and writes it as PNG frames or a GIF.
    void RunAnimationExport(const AnimationExportParams& params);

    void OnFramebufferResize(i32 w, i32 h);
    void OnMouseButton(i32 button, i32 action);
    void OnCursorPos(f64 x, f64 y);
    void OnScroll(f64 yoffset);
    void UpdateCameraPresetAnimator();
    // Frames the orbital camera on a freshly-loaded model — targets the centre
    // of its bounding box, three-quarter angle, distance to fit.
    void FrameCameraToModel(model::Actor* hero);

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

    // Pending animation export — filled by RequestAnimationExport, consumed
    // by the next Tick().
    bool exportPending_ = false;
    AnimationExportParams pendingExport_;

    friend class ViewerUI;
};

} // namespace whiteout::flakes
