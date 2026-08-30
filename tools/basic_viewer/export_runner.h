#pragma once

// ============================================================================
// Runs a recipe: drives the model through the schedule, captures every frame
// and hands it to a sink.
//
// Split out of ViewerApp so the loop is testable-adjacent and readable on its
// own, and so the ~600 lines of capture + encode stop growing viewer_app.cpp.
// The host stays in charge of *reaching* the actor and the ImGui frame — this
// takes those as callbacks rather than reaching back into ViewerApp.
// ============================================================================

#include "export_recipe.h"
#include "render_target.h"
#include "whiteout/flakes/types.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace whiteout::flakes::renderer {
class RenderService;
using SceneId = u32;
namespace model {
struct Actor;
}
} // namespace whiteout::flakes::renderer

namespace whiteout::flakes {

/// @brief What one export produced. Replaces the original exporter's
///        fprintf-only feedback, which said nothing a dialog could show.
struct ExportReport {
    bool ok = false;
    bool cancelled = false;
    i32 framesRequested = 0;
    i32 framesCaptured = 0;
    std::vector<std::filesystem::path> files;
    std::filesystem::path folder;
    std::string error;
    f64 elapsedSec = 0.0;
    u64 outputBytes = 0;
};

/// @brief Everything the runner needs from the viewer.
struct ExportHost {
    renderer::RenderService* service = nullptr;
    renderer::SceneId scene = 0;
    renderer::RenderTargetId target = 0;
    GLFWwindow* window = nullptr;
    renderer::model::Actor* hero = nullptr;
    std::vector<std::string> sequenceNames;
    std::filesystem::path modelPath;

    /// @brief Re-pose an animated camera preset, as normal playback does.
    std::function<void()> applyCameraPreset;
    /// @brief Select a camera preset by index (Preset mode).
    std::function<void(i32)> activateCameraPreset;
    /// @brief Which preset is active, or -1 for the free camera.
    ///
    /// Needed to put it back: a preset poses the camera DIRECTLY, so restoring
    /// the orbital yaw/pitch/distance would leave a viewer that was on a model
    /// camera looking somewhere else entirely.
    std::function<i32()> currentCameraPreset;
    /// @brief Build one ImGui frame with the viewer overlay in it.
    std::function<void()> buildUiFrame;
    /// @brief Build one empty ImGui frame — the composite still expects draw
    ///        data, it just has nothing in it.
    std::function<void()> buildEmptyFrame;
    /// @brief Re-apply the Animation window's layered plays after the run.
    std::function<void()> reassertAnimTracks;
    /// @brief Progress, for the window title and the log.
    std::function<void(i32 frame, i32 total)> onProgress;
};

/// @brief Hard ceiling on a single recording, so a mistyped duration cannot
///        fill a disk unattended.
constexpr i32 kMaxExportFrames = 100000;

/// @brief Run @p recipe against @p host. Synchronous: it owns the frame loop
///        for its duration, which is why the caller defers it out of the
///        ImGui frame.
ExportReport RunExport(const ExportRecipe& recipe, const ExportHost& host);

/// @brief Drive the recipe live in the viewport for one frame — the dialog's
///        Preview and its timeline scrubber.
///
/// Shares the schedule with the recording, so what the preview shows is what
/// the export will capture. `frameIndex` is a total index in the schedule.
/// Returns false when the recipe cannot be scheduled.
bool PreviewExportFrame(const ExportRecipe& recipe, const ExportHost& host, i32 frameIndex,
                        bool applyCamera);

} // namespace whiteout::flakes
