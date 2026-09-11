#pragma once

// ============================================================================
// ViewerUI — every ImGui widget for the basic viewer.
//
// Owns no engine state; reads / mutates RenderService and ViewerApp host
// state. Built on top of the engine-side ImGui adapter (RenderService::ImGui())
// which handles the actual draw submission.
// ============================================================================

#include "export_window.h"
#include "io/wem/wem_profiles.h"   // WemProfileOption — the picker's rows
#include "whiteout/flakes/enums.h" // ProductId
#include "whiteout/flakes/types.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace whiteout::flakes::io {
class FileContentProvider;
struct WemDocument;
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
    // The settings no game owns: the frame the renderer draws for all of them
    // and the knobs the next launch reads. Its own left-panel row, above the
    // games, because picking a game to change the exposure was the wrong
    // question to answer.
    void BuildSettingsGeneralPage();
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

    // Export the model on screen as a `.wem`. One dialog and no options modal:
    // which converter runs is decided by what the model IS, not by anything the
    // user could pick here (WEM_INTEGRATION_DESIGN.md §5).
    void ExportWemDialog();

    // Export the model on screen as a Warcraft III `.mdx`. Pops the native save
    // dialog and then the options modal below — unlike the `.wem` export, this
    // one has choices to make: which Warcraft III generation, and whether the
    // textures come with it.
    void ExportMdxDialog();

    // The deferred "Export to MDX" options modal. Renders when
    // `pendingMdxPath_` is set; a no-op otherwise.
    void BuildMdxExportPopup();

    // Export the model on screen as a StarCraft II `.m3` — the MDX export one
    // game over. Pops the native save dialog, then the options modal below.
    void ExportM3Dialog();

    // The deferred "Export to M3" options modal. Renders when `pendingM3Path_`
    // is set; a no-op otherwise.
    void BuildM3ExportPopup();

    // Export the model on screen as glTF 2.0 — the export pointed out of the
    // Blizzard family. Pops the native save dialog, then the modal below.
    void ExportGltfDialog();

    // The deferred "Export to glTF" options modal. Renders when
    // `pendingGltfPath_` is set; a no-op otherwise.
    void BuildGltfExportPopup();

    // Save As for a model that IS a StarCraft II `.m3`: the native save dialog,
    // then the options modal below. Separate from SaveAsDialog's MDX/MDL path
    // because it shares neither the writer nor a single option with it.
    void SaveM3Dialog();

    // The deferred "Save M3" options modal. Renders when `pendingM3SavePath_`
    // is set; a no-op otherwise.
    void BuildM3SavePopup();

    // The profile picker a `.wem` open goes through. Renders when
    // `wemOpenDocument_` is set and loads the document on confirm; a no-op
    // otherwise. Not a question a default can answer: a document carrying two
    // material sets over one geometry has two right answers and the file does
    // not say which the user meant (§3).
    void BuildWemProfilePopup();

    ViewerApp& app_;

    // Last `.m3a` pick the attach refused, shown in the Anims popup until
    // the next attempt. Empty when the last attempt succeeded.
    std::string animAttachError_;

    bool settingsOpen_ = false;
    // Which left-panel row is picked: the shared page, or the game that
    // ViewerApp::SettingsProfile() names. Not persisted — the shared page is
    // where the window opens.
    bool settingsGlobalPage_ = true;
    bool animWindowOpen_ = false;
    bool showViewCube_ = true;    // View > View Cube toggle
    bool showLogConsole_ = false; // Debug > Log Console toggle

    // The `.wem` waiting on a profile: the parsed document, where it came from,
    // the rows it offers and which one is selected. All cleared when the popup
    // closes, either way.
    std::shared_ptr<io::WemDocument> wemOpenDocument_;
    std::filesystem::path wemOpenPath_;
    std::vector<io::WemProfileOption> wemOpenOptions_;
    i32 wemOpenSelection_ = 0;
    bool openWemProfilePopup_ = false;

    // Save As state. `pendingSavePath_` is non-empty only between the user
    // choosing a target and confirming in the options modal.
    std::string pendingSavePath_;
    bool pendingSaveIsMdl_ = false; // target is .mdl → show dialect choice
    bool openSaveOptionsPopup_ = false;
    i32 saveDialect_ = 0;             // 0 = Warcraft III, 1 = Hiveworkshop
    bool saveExportTextures_ = false; // export used textures next to the model
    i32 saveTexFormatIdx_ = 0;        // index into kExportFormats (0 = keep original)

    // Export to MDX state. `pendingMdxPath_` is non-empty only between the user
    // choosing a target and confirming in the options modal.
    //
    // `mdxExportProfile_` is Reforged and is drawn disabled: the classic derive
    // is a different material vocabulary (§7.2.1) and nothing has measured what
    // it costs yet, so offering the choice would be offering an untested one.
    // The row is drawn rather than hidden because the file it writes IS
    // generation-specific and a user should see which one they are getting.
    std::string pendingMdxPath_;
    bool openMdxExportPopup_ = false;
    i32 mdxExportProfile_ = 1;      // 0 = classic, 1 = Reforged
    bool mdxExportTextures_ = true; // convert and write the textures too

    // Export to M3 state, the block above one game over. Both rows are live,
    // unlike the MDX popup's: the two games share the container and the
    // version field is the difference (v29 imports back as StarCraft II,
    // v30 as Heroes of the Storm).
    std::string pendingM3Path_;
    bool openM3ExportPopup_ = false;
    i32 m3ExportProfile_ = 0;      // 0 = StarCraft II, 1 = Heroes of the Storm
    bool m3ExportTextures_ = true; // write the `.dds` textures beside it
    bool m3ExactPasses_ = false;   // Warcraft III: a composite for every approximate fold
    bool m3SharpenTeamKey_ = false; // Warcraft III: bake keyed alpha binary over a plate
    bool m3War3ModTextures_ = false; // Warcraft III: name War3 (Mod)'s copies, write fewer

    // Export-to-glTF state. The container follows the picked filename
    // (`.glb` = one self-contained file, `.gltf` = JSON + `.bin` + images).
    std::string pendingGltfPath_;
    bool openGltfExportPopup_ = false;
    bool gltfExportTextures_ = true; // resolve + embed the textures as PNG

    // Save M3 state. No option is the file's own shape: the merge folds in
    // `.m3a` files the `.m3` never named, the conversion lowers a Heroes
    // model to what StarCraft II's loader accepts, and the texture copy adds
    // files beside it. All default off, so the plain save writes the model as
    // it stands.
    std::string pendingM3SavePath_;
    bool openM3SavePopup_ = false;
    bool m3SaveMergeAnims_ = false;
    bool m3SaveConvertSc2_ = false;
    bool m3SaveExportTextures_ = false; // copy the referenced textures beside it
    // Why the last attempt wrote nothing, kept so the modal can stay open and
    // say so. A Heroes material the standard form cannot represent blocks the
    // conversion, and that is the user's cue to leave the box unticked.
    std::string m3SaveError_;

    // IO tab edit buffers: the whole of one profile's settings. They are the
    // page's state, not a mirror of the provider — the provider only ever holds
    // the ACTIVE profile, and these have to be able to hold any of them (see
    // SeedIoBuffers / CommitIoProfile).
    std::string installPathBuf_;
    std::string hotsPathBuf_; // StarCraft II page: the Heroes root
    std::string listfileBuf_; // World of Warcraft page: the `id;path` CSV
    std::string tactKeyBuf_;  // World of Warcraft page: the TACT key list
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
