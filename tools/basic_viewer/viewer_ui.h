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
    // Strip of one tab per open document (model/effect), each with a close (x)
    // button. Selecting a tab activates that document; closing it unloads it.
    void BuildTabBar();
    void BuildSettingsWindow();
    void BuildViewCubeWidget();
    // Renders the deferred Save As options modal (MDL dialect + texture export)
    // when a model save is pending. No-op otherwise.
    void BuildSaveOptionsPopup();
    // Renders the "Export Animation Frames" modal (animation + FPS + folder).
    void BuildExportPopup();

    void OpenFileDialog();
    // Save As entry point — pops the native save dialog, then defers to the
    // options modal (dialect for MDL, texture export for both).
    void SaveAsDialog();

    ViewerApp& app_;

    bool settingsOpen_ = false;
    bool showViewCube_ = true;    // View > View Cube toggle
    bool showLogConsole_ = false; // Debug > Log Console toggle

    // Save As state. `pendingSavePath_` is non-empty only between the user
    // choosing a target and confirming in the options modal.
    std::string pendingSavePath_;
    bool pendingSaveIsMdl_ = false;    // target is .mdl → show dialect choice
    bool openSaveOptionsPopup_ = false;
    i32 saveDialect_ = 0;              // 0 = Warcraft III, 1 = Hiveworkshop
    bool saveExportTextures_ = false;  // export used textures next to the model
    i32 saveTexFormatIdx_ = 0;         // index into kExportFormats (0 = keep original)

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
    i32 exportFormat_ = 0; // 0 = PNG frames, 1 = GIF, 2 = APNG, 3 = WebP
    bool exportTransparent_ = false;
    bool exportCaptureUi_ = false;
    i32 exportResMode_ = 0; // 0 = current view, 1 = custom
    i32 exportWidth_ = 1280;
    i32 exportHeight_ = 960;
    std::string exportFolder_;
};

} // namespace whiteout::flakes
