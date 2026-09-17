#pragma once

// ============================================================================
// The viewer's ribbon: the rail down the left edge (the File tile and the mode
// tabs), the menu row along the top, and the groups of commands under it —
// what used to be the main menu bar plus the toolbar strip.
//
// The widget itself is host-agnostic and lives in tools/common/imgui_ribbon.h;
// this is only what the viewer puts in it.
// ============================================================================

#include "ui/d3_equip_popup.h"

namespace whiteout::flakes {

class AnimationWindow;
class ExportDialogs;
class ExportWindow;
class MenuBar;
class OpenDialog;
class SettingsWindow;
class WowAppearance;
struct UiContext;

/// What the rail's tabs select. One for now: everything the viewer does today
/// is the Preview. A second mode is a row in kModes plus a case in BuildBody.
enum class ViewerMode { Preview };

class Ribbon {
public:
    Ribbon(UiContext& ctx, MenuBar& menus, AnimationWindow& animationWindow, OpenDialog& openDialog,
           ExportDialogs& exportDialogs, ExportWindow& exportWindow, SettingsWindow& settings);

    void Build();

    ViewerMode Mode() const {
        return mode_;
    }

    /// The `--ui-shot` harness: hold one of the ribbon's popups open
    /// ("##file", "##export", "##d3equip", "##customize") — re-requested every
    /// frame, because a window appearing takes focus and closes popups.
    void HoldPopupOpen(const char* id) {
        shotPopupId_ = id;
    }

private:
    void BuildRail();
    void BuildDocumentGroup();
    void BuildPlaybackGroup();
    void BuildSceneGroup();
    void BuildLookGroup();
    void BuildToolsGroup();
    void BuildWowControls(WowAppearance& wow);
    void BuildD3Equip();
    /// The widest caption of the rows in one group, so its controls line up.
    f32 RowLabelWidth(const char* const* keys, i32 count) const;
    bool ShotHolds(const char* popupId) const;

    UiContext& ctx_;
    MenuBar& menus_;
    AnimationWindow& animationWindow_;
    OpenDialog& openDialog_;
    ExportDialogs& exportDialogs_;
    ExportWindow& exportWindow_;
    SettingsWindow& settings_;
    D3EquipPopup d3Equip_;
    ViewerMode mode_ = ViewerMode::Preview;
    const char* shotPopupId_ = nullptr;
};

} // namespace whiteout::flakes
