#pragma once

// ============================================================================
// Animation capture for the viewer: the queued run, its outcome for the
// dialog, and the live preview that poses the model from the same schedule.
//
// A run is requested from inside the UI frame and runs on the next tick,
// outside it: it owns the frame loop for its duration (export_runner.h).
// ============================================================================

#include "capture/export_recipe.h"
#include "capture/export_runner.h"
#include "whiteout/flakes/types.h"

#include <functional>

namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes {

class DocumentManager;
class PlatformWindow;
class PlaybackController;

class CaptureController {
public:
    CaptureController(renderer::RenderService& service, PlatformWindow& window, DocumentManager& documents,
                      PlaybackController& playback);

    /// The viewer's own UI frame, for a capture that composites the overlay.
    void SetUiFrameBuilder(std::function<void()> build) {
        buildUiFrame_ = std::move(build);
    }

    /// Queue @p recipe for the next tick.
    void Request(ExportRecipe recipe);
    /// Run the queued capture, if any. True when one ran: it owned the frame.
    bool RunPending();

    /// True while a run owns the frame loop. The run builds the viewer's UI frame
    /// when capturing the overlay, so anything in it that would drive the model
    /// has to stand down meanwhile.
    bool IsRunning() const {
        return running_;
    }
    const ExportReport& LastReport() const {
        return lastReport_;
    }
    /// True exactly once per finished run, so the dialog latches the report.
    bool ConsumeFinished();

    /// Pose the model (and, in orbit mode, the camera) at frame @p frameIndex of
    /// @p recipe: the dialog's scrubber and live preview, from the schedule the
    /// recording uses.
    bool Scrub(const ExportRecipe& recipe, i32 frameIndex, bool applyCamera);

private:
    ExportHost MakeHost();
    void Run(const ExportRecipe& recipe);

    renderer::RenderService& service_;
    PlatformWindow& window_;
    DocumentManager& documents_;
    PlaybackController& playback_;
    std::function<void()> buildUiFrame_;

    bool pending_ = false;
    ExportRecipe pendingRecipe_;
    ExportReport lastReport_;
    bool finished_ = false;
    bool running_ = false;
};

} // namespace whiteout::flakes
