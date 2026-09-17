#include "capture/capture_controller.h"

#include "app/platform_window.h"
#include "documents/document_manager.h"
#include "documents/playback_controller.h"
#include "ui/ui_metrics.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <imgui.h>

#include <cstdio>

namespace whiteout::flakes {

CaptureController::CaptureController(renderer::RenderService& service, PlatformWindow& window,
                                     DocumentManager& documents, PlaybackController& playback)
    : service_(service), window_(window), documents_(documents), playback_(playback) {}

void CaptureController::Request(ExportRecipe recipe) {
    pendingRecipe_ = std::move(recipe);
    pending_ = true;
}

bool CaptureController::RunPending() {
    if (!pending_)
        return false;
    pending_ = false;
    Run(pendingRecipe_);
    return true;
}

bool CaptureController::ConsumeFinished() {
    const bool was = finished_;
    finished_ = false;
    return was;
}

bool CaptureController::Scrub(const ExportRecipe& recipe, i32 frameIndex, bool applyCamera) {
    return PreviewExportFrame(recipe, MakeHost(), frameIndex, applyCamera);
}

// Everything the runner needs that belongs to the viewer, handed over as data
// and callbacks rather than the runner reaching back in.
ExportHost CaptureController::MakeHost() {
    const DocumentState& state = documents_.ActiveState();
    ExportHost host;
    host.service = &service_;
    host.scene = documents_.ActiveScene();
    host.target = window_.Target();
    host.hero = playback_.FocusActor();
    host.sequenceNames = state.sequenceNames;
    host.modelPath = state.modelPath;
    // An animated camera preset keeps tracking the sequence for every captured
    // frame, as normal playback does.
    host.applyCameraPreset = [this] { playback_.UpdateCameraPresetAnimator(); };
    host.activateCameraPreset = [this](i32 idx) { playback_.ActivateCameraPreset(idx); };
    host.currentCameraPreset = [this] { return playback_.ActiveCameraPreset().value_or(-1); };
    // The ImGui draw data RenderFrame composites: the live overlay when asked
    // for, otherwise an empty frame.
    host.buildUiFrame = [this] {
        window_.BeginImGuiFrame();
        if (buildUiFrame_)
            buildUiFrame_();
        ImGui::Render();
    };
    host.buildEmptyFrame = [this] {
        window_.BeginImGuiFrame();
        ImGui::Render();
    };
    host.reassertAnimTracks = [this] { playback_.ReassertTracks(); };
    host.onProgress = [this](i32 frame, i32 total) {
        // An in-viewport overlay would be composited into the frame being
        // captured, so progress goes where it costs nothing.
        if (!window_.Handle())
            return;
        char title[128];
        std::snprintf(title, sizeof(title), "%s - exporting %d/%d (%d%%)", ui::kWindowTitle, frame, total,
                      total > 0 ? (frame * 100) / total : 0);
        window_.SetTitle(title);
    };
    host.pollCancel = [this] { return window_.PollEscape(); };
    return host;
}

void CaptureController::Run(const ExportRecipe& recipe) {
    const ExportHost host = MakeHost();
    if (!host.hero) {
        lastReport_ = {};
        lastReport_.error = "no animated model loaded";
        finished_ = true;
        std::fprintf(stderr, "[viewer] Export: no animated model loaded\n");
        return;
    }

    running_ = true;
    lastReport_ = RunExport(recipe, host);
    running_ = false;
    finished_ = true;
    const ExportReport& r = lastReport_;

    window_.SetTitle(ui::kWindowTitle);

    if (r.ok) {
        std::fprintf(stderr, "[viewer] Exported %d frame(s) in %.1fs (%s) to %s\n", r.framesCaptured, r.elapsedSec,
                     r.files.empty() ? "" : io::PathToUtf8(r.files.front().filename()).c_str(),
                     io::PathToUtf8(r.folder).c_str());
    } else if (r.cancelled) {
        std::fprintf(stderr, "[viewer] Export cancelled after %d frame(s)\n", r.framesCaptured);
    } else {
        std::fprintf(stderr, "[viewer] Export failed: %s\n", r.error.empty() ? "unknown error" : r.error.c_str());
    }
}

} // namespace whiteout::flakes
