#pragma once

// ============================================================================
// The toolbar's Equip popup for a Diablo III player character: the outfit by
// in-game item (with presets, sets and dyes) once the item registry is up, and
// the manual wardrobe of looks the appearance carries — all of it before the
// registry lands, Hair and the extras after.
// ============================================================================

#include "features/d3_wardrobe.h"

#include <string>
#include <vector>

namespace whiteout::flakes {

class D3EquipPopup {
public:
    /// Inside the popup's Begin/End. @p slots is the manual wardrobe, read once
    /// by the toolbar to decide whether the button shows at all.
    void Build(D3Wardrobe& d3, const std::vector<D3Wardrobe::CharacterSlot>& slots);

private:
    void BuildOutfit(D3Wardrobe& d3, const std::vector<D3Wardrobe::OutfitRow>& rows);

    /// One filter shared by every item combo: one popup is ever open, and a set
    /// is usually searched for once and equipped piecewise.
    std::string itemFilter_;
    std::string presetName_;
    /// Offer the whole registry rather than what the focused class can wear.
    bool allClasses_ = false;
};

} // namespace whiteout::flakes
