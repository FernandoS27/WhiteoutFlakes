#pragma once

// ============================================================================
// ViewerUI — every ImGui widget for the basic viewer.
//
// Owns no engine state; reads / mutates RenderService and ViewerApp host
// state. Built on top of the engine-side ImGui adapter (RenderService::ImGui())
// which handles the actual draw submission.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <string>
#include <vector>

namespace whiteout::flakes {

class ViewerApp;

class ViewerUI {
public:
    explicit ViewerUI(ViewerApp& app);

    // Called every frame between ImGui::NewFrame() and ImGui::Render() to
    // build all the windows / menus the viewer exposes.
    void BuildFrame();

private:
    void BuildMenuBar();
    void BuildToolbar();
    void BuildSettingsWindow();
    void BuildViewCubeWidget();
    // Renders the deferred "pick MDL dialect" modal when an MDL Save As is
    // pending. No-op otherwise.
    void BuildSaveAsPopup();
    // Renders the "Export Animation Frames" modal (animation + FPS + folder).
    void BuildExportPopup();

    void OpenFileDialog();
    // Save As entry point — pops the native save dialog, then either writes
    // immediately (MDX) or defers to the dialect modal (MDL).
    void SaveAsDialog();

    ViewerApp& app_;

    bool settingsOpen_ = false;

    // Save As state. `pendingSaveMdlPath_` is non-empty only between the user
    // choosing an .mdl target and picking a dialect in the modal.
    std::string pendingSaveMdlPath_;
    bool openDialectPopup_ = false;

    // DNC path edit buffer (ImGui InputText needs a writable buffer the UI
    // owns; we sync from DncService.UnitMdlPath() on each frame so external
    // updates take effect).
    std::string dncPathBuf_;

    // IO tab edit buffers, mirroring the live FileContentProvider state.
    // Seeded from the provider on first display of Settings (and after a
    // Reset). installPathBuf_ commits to the provider + ini on
    // IsItemDeactivatedAfterEdit; the MPQ-list scratch is committed inline
    // by the add/remove/reorder buttons.
    std::string installPathBuf_;
    std::string newMpqEntryBuf_;
    bool ioBufsInitialised_ = false;

    // Export Animation Frames modal state.
    bool openExportPopup_ = false;
    i32 exportSeqIdx_ = 0;
    i32 exportFps_ = 30;
    i32 exportFormat_ = 0; // 0 = PNG frames, 1 = animated GIF
    std::string exportFolder_;
};

} // namespace whiteout::flakes
