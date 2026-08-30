#pragma once

// ============================================================================
// The Export Animation window.
//
// A recipe has about forty fields, and the previous dialog — eight controls in
// an AlwaysAutoResize popup — was already at its limit. So this is arranged
// around what people actually do rather than around the struct:
//
//   1. The common case is one click. "Current animation" reproduces the old
//      export exactly, so an existing workflow costs nothing.
//   2. Presets fill the whole recipe; you tweak from there.
//   3. Progressive disclosure, with the state still visible — a collapsed
//      section shows a one-line summary of what is inside it.
//   4. The footer always says exactly what will happen: length, frames,
//      resolution, estimated size, and the resolved output path.
//   5. Warnings appear next to their cause and never block.
//
// A normal window, not a modal: the Viewport camera mode is "whatever you are
// framing right now" and the timeline scrubber poses the model live, so both
// need the viewport reachable while the dialog is open.
// ============================================================================

#include "export_recipe.h"

#include <string>
#include <vector>

namespace whiteout::flakes {

class ViewerApp;

class ExportWindow {
public:
    explicit ExportWindow(ViewerApp& app);

    bool IsOpen() const {
        return open_;
    }
    /// @param seedSequence the sequence the toolbar is on, used to seed an
    ///        empty queue so opening the window and pressing Export does what
    ///        the old dialog did.
    void Open(i32 seedSequence);
    void Close();

    /// Build the window. No-op when closed.
    void Build();

    /// The model (or its sequence list) changed: re-resolve the queue against
    /// the new names and drop the preview.
    void OnModelChanged();

    /// Persist the recipe. Called on close and after an export.
    void Save();

    /// Advance the live preview, if one is running. Called once a frame from
    /// the viewer's tick, outside the ImGui frame.
    void Tick(f32 dtSec);

private:
    // ---- sections ----
    void BuildPresetRow();
    void BuildQueue();
    void BuildTimeline();
    void BuildLength();
    void BuildCamera();
    void BuildOutput();
    void BuildAdvanced();
    void BuildFooter();
    void BuildAddClipPopup();

    // ---- helpers ----
    void LoadIfNeeded();
    // Rebuild `schedule_` from the current recipe. Called once per frame: four
    // sections read it, and Build() is O(frames) for a fill that repeats a
    // short clip, so rebuilding per section made a long recording sluggish to
    // even look at.
    void RebuildSchedule();
    void ApplyPreset(i32 preset);
    void MarkCustom();
    std::string ResolvedOutputPath() const;
    const char* SequenceLabel(const ExportClip& clip) const;

    ViewerApp& app_;
    ExportRecipe recipe_;
    ExportSchedule schedule_;
    bool open_ = false;
    bool loaded_ = false;

    // Which preset row entry is lit. Any edit moves it to Custom, so the row
    // never claims a configuration it no longer describes.
    i32 preset_ = 0;

    std::string folderBuf_;
    std::string nameBuf_;
    i32 resMode_ = 0; ///< 0 = current view, 1 = custom

    // Add-animations popup.
    bool openAddPopup_ = false;
    std::string addSearch_;
    std::vector<char> addSelected_;

    // Timeline scrub + preview.
    i32 scrubFrame_ = 0;
    bool previewing_ = false;
    f32 previewAccum_ = 0.0f;
    i32 previewFrame_ = 0;

    // Row expanders — one flag per queue row, parallel to recipe_.clips.
    std::vector<char> rowExpanded_;

    // Height the footer took last frame, which is what the scroll region
    // reserves for it. Measured rather than assumed: the footer grows by a
    // wrapped line per warning, and a fixed reservation pushed the Export
    // button off the bottom of the window exactly when there was something
    // worth reading above it.
    f32 footerHeight_ = 0.0f;

    // Last run, shown in the footer until the next one.
    bool hasReport_ = false;
    bool reportOk_ = false;
    std::string reportLine_;
    std::string reportPath_;

    // The sequence list the queue was last resolved against, so a model
    // change is noticed without the app having to announce it.
    std::vector<std::string> resolvedAgainst_;
};

} // namespace whiteout::flakes
