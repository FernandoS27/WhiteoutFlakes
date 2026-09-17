#pragma once

// ============================================================================
// ViewerApp — the viewer's composition root and frame loop.
//
// It owns the subsystems and wires them in its constructor: the window, the
// camera input, the storage session, the documents with their loader and
// playback, the embedded Storage Explorer and the UI. Tick runs one frame on
// the calling thread; the caller owns the outer loop.
//
// ============================================================================

#include "app/orbit_camera_input.h"
#include "app/platform_window.h"
#include "app/storage_explorer_host.h"
#include "capture/capture_controller.h"
#include "documents/document_loader.h"
#include "documents/document_manager.h"
#include "documents/playback_controller.h"
#include "export/model_export_service.h"
#include "features/game_features.h"
#include "io/load_task.h"
#include "session/storage_session.h"
#include "whiteout/flakes/enums.h"
#include "whiteout/flakes/gfx_types.h"
#include "whiteout/flakes/types.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes {

class ViewerUI;

class ViewerApp {
public:
    explicit ViewerApp(renderer::RenderService& service);
    ~ViewerApp();

    ViewerApp(const ViewerApp&) = delete;
    ViewerApp& operator=(const ViewerApp&) = delete;

    /// `visible == false` creates the window hidden: the full app runs (device,
    /// swap chain, tick loop) but nothing appears on screen — what scripted
    /// export and attach runs use so a corpus sweep never flashes windows.
    bool Open(i32 width, i32 height, gfx::GfxApi api, bool visible = true);
    /// The Storage Explorer (its thumbnail scenes and offscreen targets) goes
    /// first, while the device is still alive; then ImGui, the device, the window.
    void Close();

    /// Pin the DPI scale instead of asking the monitor. Before Open; the
    /// `--ui-shot` harness uses it so a capture does not depend on the display.
    void SetDpiScaleOverride(f32 scale) {
        dpiScaleOverride_ = scale;
    }
    /// Draw the empty default scene under the UI instead of the document's. The
    /// `--ui-shot` harness captures the UI alone: a cloth solver keeps stepping
    /// while paused, and those pixels are not what the shot is for.
    void SetSceneHiddenForCapture(bool hidden) {
        sceneHiddenForCapture_ = hidden;
    }

    bool ShouldClose() const;
    void Tick(f32 dt);

    // ---- Subsystems ----
    renderer::RenderService& Service() {
        return service_;
    }
    const renderer::RenderService& Service() const {
        return service_;
    }
    PlatformWindow& Window() {
        return window_;
    }
    /// Where every load long enough to freeze the window goes. One thread, tasks
    /// in submission order; the UI polls it once a frame and draws the modal.
    io::LoadTaskRunner& Tasks() {
        return tasks_;
    }
    DocumentManager& Documents() {
        return documents_;
    }
    const DocumentManager& Documents() const {
        return documents_;
    }
    PlaybackController& Playback() {
        return playback_;
    }
    const PlaybackController& Playback() const {
        return playback_;
    }
    DocumentLoader& Loader() {
        return loader_;
    }
    const DocumentLoader& Loader() const {
        return loader_;
    }
    StorageSession& Session() {
        return session_;
    }
    StorageExplorerHost& Explorer() {
        return explorer_;
    }
    /// Each null when its format is compiled out.
    const GameFeatures& Features() const {
        return features_;
    }
    ModelExportService& Exports() {
        return exports_;
    }
    CaptureController& Capture() {
        return capture_;
    }
    ViewerUI& Ui() {
        return *ui_;
    }

    // ---- Tabs ----
    /// Make document @p index active and re-apply the render mode its scene
    /// carries; the splat and event-data hand-over runs only when it changed.
    void ActivateDocument(i32 index);
    /// Destroy document @p index. When it was active a neighbour takes over;
    /// closing the last tab leaves the viewer empty.
    void CloseDocument(i32 index);

private:
    // Tick's stages after the UI frame.
    void UpdateEngine();
    void RenderViewport();
    void UpdateTitle(f32 dt);

    renderer::RenderService& service_;
    std::optional<f32> dpiScaleOverride_;
    bool sceneHiddenForCapture_ = false;

    // Declaration order is construction order, and each subsystem takes the
    // ones above it. Destruction runs the other way.
    PlatformWindow window_;
    StorageSession session_;
    DocumentManager documents_;
    PlaybackController playback_;
    DocumentLoader loader_;
    GameFeatures features_;
    ModelExportService exports_;
    CaptureController capture_;
    OrbitCameraInput cameraInput_;
    StorageExplorerHost explorer_;
    std::unique_ptr<ViewerUI> ui_;

    // The title bar's frame counter.
    f64 fpsAccum_ = 0.0;
    i32 fpsFrames_ = 0;

    // LAST member, deliberately. Members are destroyed in reverse declaration
    // order, so declaring it here is what makes it the FIRST thing torn down —
    // and its destructor cancels the running task and joins the thread. A task
    // body captures the Storage Explorer, the provider and this object; every
    // one of them is still alive while that join happens, because every one of
    // them is declared above.
    io::LoadTaskRunner tasks_;
};

} // namespace whiteout::flakes
