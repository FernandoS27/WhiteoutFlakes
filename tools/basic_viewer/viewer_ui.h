#pragma once

// ============================================================================
// ViewerUI — every ImGui widget for the basic viewer.
//
// Owns no engine state; reads / mutates RenderService and ViewerApp host
// state. Built on top of the engine-side ImGui adapter (RenderService::ImGui())
// which handles the actual draw submission.
// ============================================================================

#include "whiteout/flakes/enums.h" // ProductId
#include "whiteout/flakes/types.h"

#include <string>
#include <vector>

namespace whiteout::flakes::io {
class FileContentProvider;
} // namespace whiteout::flakes::io

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
    // Settings is a game picker (left panel) plus that game's pages. `game` is
    // the picked profile, which is also the shared provider's active game —
    // one source of truth rather than a selection to keep in sync.
    void BuildSettingsWindow();
    void BuildSettingsGeneralTab(ProductId game);
    void BuildSettingsIoTab(io::FileContentProvider& provider, ProductId game);
    // Warcraft III and World of Warcraft share this page: one install root,
    // the per-storage ignore switches, an editable MPQ load order. What
    // differs is the data — WoW adds a listfile and scans its archive names.
    void BuildIoArchivePage(io::FileContentProvider& provider, ProductId game);
    // StarCraft II / Heroes: two CASC roots and no MPQs, ever.
    void BuildIoCascPage(io::FileContentProvider& provider);
    // Repoint the shared provider at `game` and apply that game's saved IO
    // setup. What the left panel does when a row is clicked.
    void SelectSettingsProfile(ProductId game);
    void BuildViewCubeWidget();
    // Renders the deferred Save As options modal (MDL dialect + texture export)
    // when a model save is pending. No-op otherwise.
    void BuildSaveOptionsPopup();
    // Renders the "Export Animation Frames" modal (animation + FPS + folder).
    void BuildExportPopup();

    void OpenFileDialog();
    // Pops a multi-select `.m3a` picker and attaches each pick to the focus
    // actor. StarCraft II only — the toolbar hides the button otherwise.
    void AttachAnimationDialog();
    // Save As entry point — pops the native save dialog, then defers to the
    // options modal (dialect for MDL, texture export for both).
    void SaveAsDialog();

    ViewerApp& app_;

    // Last `.m3a` pick the attach refused, shown in the Anims popup until
    // the next attempt. Empty when the last attempt succeeded.
    std::string animAttachError_;

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

    // IO tab edit buffers, mirroring the live FileContentProvider state.
    // Seeded from the provider on first display of Settings (and after a
    // Reset). installPathBuf_ commits to the provider + ini on
    // IsItemDeactivatedAfterEdit; the MPQ-list scratch is committed inline
    // by the add/remove/reorder buttons.
    std::string installPathBuf_;
    std::string hotsPathBuf_;  // StarCraft II page: the Heroes root
    std::string listfileBuf_;  // World of Warcraft page: the `id;path` CSV
    std::string tactKeyBuf_;   // World of Warcraft page: the TACT key list
    std::string newMpqEntryBuf_;
    bool ioBufsInitialised_ = false;
    // Which game the buffers above hold. They are re-seeded when the profile
    // panel selects a different one.
    ProductId ioBufsGame_ = ProductId::Wc3;

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
