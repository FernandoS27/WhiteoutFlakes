#pragma once

// ============================================================================
// The strip under the menu bar: transport, the sequence and camera pickers,
// team colour, the per-game look controls, and the lighting mode.
// ============================================================================

#include "ui/d3_equip_popup.h"

namespace whiteout::flakes {

class AnimationWindow;
class WowAppearance;
struct UiContext;

class Toolbar {
public:
    Toolbar(UiContext& ctx, AnimationWindow& animationWindow);

    void Build();

    /// The `--ui-shot` harness: hold one of the toolbar's popups open
    /// ("##d3equip", "##customize") — re-requested every frame, because a
    /// window appearing takes focus and closes popups.
    void HoldPopupOpen(const char* id) {
        shotPopupId_ = id;
    }

private:
    void BuildTransport();
    void BuildSequencePicker();
    void BuildCameraPicker();
    void BuildTeamColor();
    void BuildWowControls(WowAppearance& wow);
    void BuildD3Equip();
    void BuildLightingMode();
    bool ShotHolds(const char* popupId) const;

    UiContext& ctx_;
    AnimationWindow& animationWindow_;
    D3EquipPopup d3Equip_;
    const char* shotPopupId_ = nullptr;
};

} // namespace whiteout::flakes
