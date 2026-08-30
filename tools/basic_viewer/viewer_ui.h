#pragma once

// ============================================================================
// ViewerUI — every ImGui widget for the basic viewer.
//
// Owns no engine state; reads / mutates RenderService and ViewerApp host
// state. Built on top of the engine-side ImGui adapter (RenderService::ImGui())
// which handles the actual draw submission.
// ============================================================================

#include "export_window.h"
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

    // The Export Animation window, so the host can advance its live preview
    // outside the ImGui frame.
    ExportWindow& Export() {
        return exportWindow_;
    }

    // Called every frame between ImGui::NewFrame() and ImGui::Render() to
    // build all the windows / menus the viewer exposes.
    void BuildFrame();

private:
    void BuildMenuBar();
    void BuildToolbar();
    // Strip of one tab per open document (model/effect), each with a close (x)
    // button. Selecting a tab activates that document; closing it unloads it.
    void BuildTabBar();
    // Settings is a profile picker (left panel) plus that profile's pages. The
    // picked profile is ViewerApp::SettingsProfile() — deliberately NOT the
    // provider's active game: editing a profile must not repoint the content
    // layer, let alone open its install.
    void BuildSettingsWindow();
    void BuildSettingsGeneralTab(ProductId game);
    void BuildSettingsIoTab(io::FileContentProvider& provider, ProductId game);
    // Warcraft III and World of Warcraft share this page: one install root,
    // the per-storage ignore switches, an editable MPQ load order. What
    // differs is the data — WoW adds a listfile and scans its archive names.
    void BuildIoArchivePage(io::FileContentProvider& provider, ProductId game);
    // StarCraft II / Heroes: two CASC roots and no MPQs, ever.
    void BuildIoCascPage(io::FileContentProvider& provider, ProductId game);
    // Live storage state for `game`, which only the active profile has.
    void BuildIoStorageStatus(io::FileContentProvider& provider, ProductId game);
    // Fill the edit buffers with `game`'s settings: from the live provider when
    // it is the profile being served, from the ini otherwise.
    void SeedIoBuffers(io::FileContentProvider& provider, ProductId game);
    // Write the buffers back as `game`'s profile — always to the ini, and to
    // the provider (reopening its storage) only when `game` is the one it is
    // serving. See the IO pages comment in viewer_ui.cpp.
    void CommitIoProfile(io::FileContentProvider& provider, ProductId game);
    // What the left panel does when a row is clicked: change which profile is
    // being edited, and nothing else.
    void SelectSettingsProfile(ProductId game);
    void BuildViewCubeWidget();
    // Renders the deferred Save As options modal (MDL dialect + texture export)
    // when a model save is pending. No-op otherwise.
    void BuildSaveOptionsPopup();
    // The Animation window: the model's global loops, the extra plays layered
    // under the sequence dropdown, and the attached `.m3a` files. StarCraft II
    // only — everything in it is a thing only an `.m3` has.
    void BuildAnimationWindow();
    // One row of the track table. Returns false when the row asked to be
    // removed, which the caller has to honour before touching the vector again.
    bool BuildAnimTrackRow(std::size_t index);

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
    bool animWindowOpen_ = false;
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

    // IO tab edit buffers: the whole of one profile's settings. They are the
    // page's state, not a mirror of the provider — the provider only ever holds
    // the ACTIVE profile, and these have to be able to hold any of them (see
    // SeedIoBuffers / CommitIoProfile).
    std::string installPathBuf_;
    std::string hotsPathBuf_;  // StarCraft II page: the Heroes root
    std::string listfileBuf_;  // World of Warcraft page: the `id;path` CSV
    std::string tactKeyBuf_;   // World of Warcraft page: the TACT key list
    std::string newMpqEntryBuf_;
    bool ioIgnoreCascBuf_ = false;
    bool ioIgnoreMpqBuf_ = false;
    std::vector<std::string> ioMpqListBuf_;
    bool ioBufsInitialised_ = false;
    // Which profile the buffers above hold, and which product the provider was
    // serving when they were filled. Either changing re-seeds them.
    ProductId ioBufsGame_ = ProductId::Wc3;
    ProductId ioBufsServing_ = ProductId::Wc3;

    // The Export Animation window. A window rather than a modal: its Viewport
    // camera mode and its timeline scrubber both need the viewport reachable
    // while it is open.
    ExportWindow exportWindow_;
};

} // namespace whiteout::flakes
