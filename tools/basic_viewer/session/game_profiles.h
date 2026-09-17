#pragma once

// ============================================================================
// The games the viewer has a settings profile for, in the Settings window's
// order: the name shown for each (a game name, so not localised — the same rule
// the backend list follows) and whether its storage is CASC alone.
//
// StarCraft II and Heroes of the Storm are one entry because they are one
// ProductId: they share a render profile, and the provider opens both.
// ============================================================================

#include "whiteout/flakes/enums.h"

#include <array>

namespace whiteout::flakes {

struct GameProfile {
    ProductId product;
    const char* displayName;
    /// Never shipped an MPQ, so its IO page offers CASC roots and nothing else.
    bool cascOnly;
};

inline constexpr std::array<GameProfile, 4> kGameProfiles = {{
    {ProductId::Wc3, "Warcraft III", false},
    {ProductId::Sc2, "StarCraft II / Storm", true},
    {ProductId::Wow, "World of Warcraft", false},
    {ProductId::D3, "Diablo III", true},
}};

/// @p game's row; Warcraft III's for a product with none.
constexpr const GameProfile& GameProfileOf(ProductId game) {
    for (const GameProfile& p : kGameProfiles)
        if (p.product == game)
            return p;
    return kGameProfiles[0];
}

} // namespace whiteout::flakes
