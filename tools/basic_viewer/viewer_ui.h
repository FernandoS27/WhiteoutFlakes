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
    bool showDemo_ = false; // toggle from menu for ImGui demo

    // DNC path edit buffer (ImGui InputText needs a writable buffer the UI
    // owns; we sync from DncService.UnitMdlPath() on each frame so external
    // updates take effect).
    std::string dncPathBuf_;
};

} // namespace whiteout::flakes
