#pragma once

// ============================================================================
// ViewerUI — the viewer's ImGui frame. Owns the panels and builds them in a
// fixed order each frame; the panels drive the app's subsystems, never engine
// internals.
// ============================================================================

#include "capture/export_window.h"
#include "ui/animation_window.h"
#include "ui/export_dialogs.h"
#include "ui/menu_bar.h"
#include "ui/open_dialog.h"
#include "ui/settings_window.h"
#include "ui/ribbon.h"
#include "ui/ui_context.h"

#include <string_view>

namespace whiteout::flakes {

class ViewerApp;

class ViewerUI {
public:
    explicit ViewerUI(ViewerApp& app);

    /// The Export Animation window, so the host can advance its live preview
    /// outside the ImGui frame.
    ExportWindow& Export() {
        return exportWindow_;
    }

    /// Between ImGui::NewFrame() and ImGui::Render(). Writes the settings file
    /// once at the end when a panel changed a persisted setting.
    void BuildFrame();

    /// The `--ui-shot` harness: put the UI in the state that shows @p panel (see
    /// harness/ui_shot.h for the names). False for an unknown name or one the
    /// loaded model cannot show. Nothing is persisted.
    bool OpenPanelForShot(std::string_view panel);

private:
    /// A host-side widget the renderer has no notion of (imgui_viewcube.h),
    /// offset below the ribbon and, with documents open, the tab strip.
    void BuildViewCube();

    // Declaration order is construction order: each panel takes the ones above it.
    UiContext ctx_;
    /// A window rather than a modal: its Viewport camera mode and its timeline
    /// scrubber both need the viewport reachable while it is open.
    ExportWindow exportWindow_;
    OpenDialog openDialog_;
    ExportDialogs exportDialogs_;
    SettingsWindow settings_;
    AnimationWindow animationWindow_;
    MenuBar menuBar_;
    /// Last: it hosts the menus and drives every panel above it.
    Ribbon ribbon_;
};

} // namespace whiteout::flakes
