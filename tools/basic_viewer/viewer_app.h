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
#include "whiteout/flakes/enums.h" // RenderMode
#include "whiteout/flakes/gfx_types.h"
#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;

namespace whiteout::flakes::io {
class IContentProvider;
} // namespace whiteout::flakes::io

namespace whiteout::flakes::renderer {
class RenderService;
using SceneId = u32;
} // namespace whiteout::flakes::renderer

namespace whiteout::flakes::tools {
class StorageExplorer;
} // namespace whiteout::flakes::tools

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

    // Load an MDX from disk into a NEW document (tab) and make it active.
    // Each open document owns its own scene, so previously-loaded models stay
    // resident and switchable; nothing is cleared. Dispatches .pkb / .pkfx
    // paths to LoadEffect. Returns false (and opens no tab) on failure.
    bool LoadModel(const std::filesystem::path& path);

    // Load a standalone PopcornFX effect (.pkb / .pkfx) into a NEW document and
    // play it. Unlike a model, there's no animation list — the effect just
    // runs. Becomes the active document; other open documents are untouched.
    bool LoadEffect(const std::filesystem::path& path);

    // "Reforged Graphics": force the HD render pipeline for every model,
    // overriding the per-model SD/HD auto-detection (the LoadModel HD probe).
    // Toggling reloads the active document so its assets re-resolve under the
    // new (HD vs detected) CASC overlay.
    bool ForceHd() const {
        return forceHd_;
    }
    void SetForceHd(bool on);

    // ---- Open documents (tabs) ----
    // Each loaded file is one document, backed by its own RenderService scene.
    // The viewer renders / ticks only the active document; the rest stay frozen
    // but resident, so switching tabs is instant (no reload).
    i32 DocumentCount() const {
        return static_cast<i32>(documents_.size());
    }
    i32 ActiveDocumentIndex() const {
        return activeDoc_;
    }
    // Tab label for document `idx` (the file's stem). Empty for out-of-range.
    const std::string& DocumentTitle(i32 idx) const;
    // When the active document changes from app code (a file opened on the CLI /
    // via File > Open, or a neighbour taking over after a close), the ImGui tab
    // bar must be told which tab to select — otherwise it defaults to the first
    // tab and snaps the active document back. Returns that index once, then -1.
    i32 ConsumePendingTabSelect();
    // Make document `idx` active: publish its scene, restore its host state
    // (sequences, camera presets, focus actor) and re-apply its render mode.
    void SetActiveDocument(i32 idx);
    // Close document `idx`, destroying its scene + actors. If it was active,
    // a neighbouring tab becomes active; closing the last tab leaves the viewer
    // empty.
    void CloseDocument(i32 idx);

    // ---- Storage Explorer (Tools ▸ Storage Explorer) ----
    // The embedded CASC browser panel. Created lazily on first open; defaults to
    // the install path so it lands on the game storage without a folder pick.
    bool StorageExplorerOpen() const {
        return storageExplorerOpen_;
    }
    void SetStorageExplorerOpen(bool on);
    // Build the panel window inside the host's ImGui frame (called by ViewerUI).
    void BuildStorageExplorerWindow();

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

    // Path of the active document's model (or empty when no tab is open). The
    // load paths set this internally; Save As + the export naming read it back.
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

    // Frames the orbital camera on a standalone .pkb effect using its live
    // particle cloud (a .pkb has no mesh bounds). Returns true once it found
    // particles and reframed; false while the effect hasn't spawned any yet.
    // Driven by the deferred warm-up in Tick — particle spread isn't known
    // until the sim has run a few frames.
    bool FrameCameraToEffect();

    static void FramebufferSizeCallback(GLFWwindow* w, int width, int height);
    static void MouseButtonCallback(GLFWwindow* w, int button, int action, int mods);
    static void CursorPosCallback(GLFWwindow* w, double x, double y);
    static void ScrollCallback(GLFWwindow* w, double xoff, double yoff);

    // Per-document host state. The "flat" working members below mirror the
    // ACTIVE document; SaveActiveDocState / LoadActiveDocState shuttle that
    // mirror in and out of these slots on a tab switch. The scene itself
    // (actors, camera, clock) lives in the RenderService under `scene`.
    struct Document {
        SceneId scene = 0;
        std::string title;                      // tab label (file stem)
        std::filesystem::path modelPath;
        ActorId focusActor = 0;
        std::vector<std::string> sequenceNames;
        std::vector<SequenceInfo> sequenceRanges;
        std::vector<CameraPreset> cameraPresets;
        std::vector<std::string> cameraPresetNamesUtf8;
        i32 activeCameraPresetIdx = -1;
        bool cameraLocked = false;
        i32 walkDriftPrevSeqIdx = -1;
        f32 walkDriftAccumulated = 0.0f;
        i32 effectFrameTicks = -1;
        i32 lastParentTimeMs = 0;
        // Render mode is per scene: each document keeps the HD/SD mode it was
        // loaded under and re-applies it when it becomes active, since the
        // pipeline reads the (shared) RenderSettings mode at draw time.
        RenderMode renderMode = RenderMode::SD;
    };

    // Scene of the active document, or the default scene when none is open.
    SceneId ActiveSceneId() const;
    // Publish ActiveSceneId() as the RenderService's active scene so input
    // callbacks + every Scene() read this frame target the active document.
    void PublishActiveScene();
    // Lazily build (and return) the shared content provider every document
    // scene uses — the default scene's configured FileContentProvider, wrapped
    // in a non-owning shared_ptr so all tabs see the same CASC/MPQ/install set.
    std::shared_ptr<io::IContentProvider> SharedProvider();
    // Copy the flat working members into / out of documents_[activeDoc_].
    void SaveActiveDocState();
    void LoadActiveDocState();
    // Reset the flat working members to "no model loaded".
    void ClearWorkingState();
    // Point the shared RenderSettings (+ content-provider HD mode, splat /
    // event-data caches) at `wanted`, running the side effects only when the
    // mode actually changes. Called at load and whenever a document with a
    // different mode becomes active, so each scene draws in its own HD/SD mode.
    void ApplyRenderMode(RenderMode wanted);
    // Create a new document scene bound to `provider`, run `loadBody` (which
    // spawns into the now-active scene and fills the flat working state,
    // returning false on failure), and register it as the active tab. On failure
    // the scene is destroyed and the previously-active tab restored. The single
    // place that owns document create / rollback / register.
    bool OpenDocumentScene(std::shared_ptr<io::IContentProvider> provider, std::string title,
                           const std::function<bool()>& loadBody);
    // Restore the active document after a failed open: re-activate `prevDoc`
    // (its saved state + scene), or clear to the empty/default state if none.
    void RestoreActiveAfterFailedOpen(i32 prevDoc);

    // Shared body of LoadModel/LoadEffect: opens `path` (from the shared game
    // provider) as a new document. `effect` selects the .pkb path.
    bool OpenDocument(const std::filesystem::path& path, bool effect);

    // Open a model/effect from an arbitrary content provider (e.g. the Storage
    // Explorer's CASC provider) as a new document. Unlike OpenDocument, the doc
    // scene uses `provider` instead of the shared game provider, and HD-ness is
    // derived from the archive path (no filesystem MDX probe). `archivePath` is
    // a provider-native path (CASC ':'/'\\' separators).
    bool OpenStorageDocument(const std::string& archivePath, bool effect,
                             std::shared_ptr<io::IContentProvider> provider);
    // Common post-spawn flat-state fill (sequences, camera framing, presets)
    // shared by the model load paths. `hero` may be null (caller handles).
    void FillModelDocState(model::Actor* hero, const std::filesystem::path& path);
    // Post-spawn flat-state fill for a standalone effect (placeholder sequence,
    // provisional camera, deferred reframe). Caller sets currentModelPath_.
    void FillEffectDocState(model::Actor* hero);
    // Body of OpenDocument after a fresh scene is active: spawns into the
    // active scene and fills the flat working state. Returns false on spawn
    // failure (caller tears the scene down).
    bool LoadModelIntoActiveScene(const std::filesystem::path& path);
    bool LoadEffectIntoActiveScene(const std::filesystem::path& path);

    RenderService& service_;
    GLFWwindow* window_ = nullptr;
    gfx::GfxApi backend_ = gfx::GfxApi::D3D12;
    RenderTargetId targetId_ = 0;
    bool imguiInitialised_ = false;

    i32 lastFbW_ = 0;
    i32 lastFbH_ = 0;

    std::unique_ptr<ViewerUI> ui_;

    // "Reforged Graphics" — when set, every model loads/renders in HD
    // regardless of its detected SD/HD preference (see LoadModelIntoActiveScene
    // + SetForceHd).
    bool forceHd_ = false;

    // ---- Open documents (tabs) ----
    std::vector<Document> documents_;
    i32 activeDoc_ = -1; // index into documents_, or -1 when none are open
    // Tab index the UI should force-select next frame after an app-driven active
    // change, or -1 (see ConsumePendingTabSelect).
    i32 pendingTabSelect_ = -1;
    // Non-owning shared_ptr aliasing the default scene's content provider; one
    // instance, lazily built by SharedProvider(), handed to every document
    // scene so they all resolve assets through the same configured provider.
    std::shared_ptr<io::IContentProvider> sharedProvider_;

    // Embedded Storage Explorer panel (Tools ▸ Storage Explorer), created lazily.
    std::unique_ptr<tools::StorageExplorer> storageExplorer_;
    bool storageExplorerOpen_ = false;

    // ---- Host state (mirror of the ACTIVE document) ----
    bool loopNonLoopingPolicy_ = true;
    ActorId focusActor_ = 0;
    // Deferred camera-framing for a standalone .pkb: LoadEffect arms this, Tick
    // counts sim frames and reframes once the particle cloud has developed.
    // <0 means inactive.
    i32 effectFrameTicks_ = -1;
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
