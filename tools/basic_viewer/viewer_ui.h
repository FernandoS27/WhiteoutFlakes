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

    void OpenFileDialog();

    ViewerApp& app_;

    bool settingsOpen_ = false;

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
};

} // namespace whiteout::flakes
