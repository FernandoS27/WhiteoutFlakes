#pragma once

// ============================================================================
// MaxPluginUI — every ImGui widget for the Max plugin's preview window.
//
// Counterpart to basic_viewer's ViewerUI, minus the bits that don't fit
// the Max workflow (File > Open MDX, Exit, Loop-NonLooping policy — Max owns
// the model + the timeline). Owns no engine state; reads / mutates
// RenderService and RenderWindow's host snapshot accessors.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <string>

namespace whiteout::flakes {

class RenderWindow;

class MaxPluginUI {
public:
    explicit MaxPluginUI(RenderWindow& win);

    // Called every frame between ImGui::NewFrame() and ImGui::Render().
    void BuildFrame();

private:
    void BuildMenuBar();
    void BuildToolbar();
    void BuildSettingsWindow();
    void BuildViewCubeWidget();

    RenderWindow& win_;

    bool settingsOpen_ = false;

    // Mirror of DncService.UnitMdlPath() — InputText needs a writable
    // buffer the UI owns across frames.
    std::string dncPathBuf_;

    // IO tab edit buffers (Settings > IO). Seeded from the provider on first
    // display; installPathBuf_ commits on IsItemDeactivatedAfterEdit, the
    // MPQ-list scratch is mutated inline by the add/remove/reorder buttons.
    std::string installPathBuf_;
    std::string newMpqEntryBuf_;
    bool ioBufsInitialised_ = false;
};

} // namespace whiteout::flakes
