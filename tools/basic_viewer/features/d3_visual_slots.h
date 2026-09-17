#pragma once

// ============================================================================
// Diablo III's equipment slots as the viewer names them: the equip popup's row
// labels, the `--d3-equip` / `--d3-dye` spellings, and which slots dress armour
// on the body rather than hang an attachment. Device-free, so the command-line
// parser shares it; `ordinal` is `native::EVisualSlot`, checked where the
// wardrobe converts it.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <array>
#include <optional>
#include <string_view>

namespace whiteout::flakes {

struct D3VisualSlot {
    i32 ordinal;
    std::string_view displayName;
    std::string_view cliName;
    /// Dresses the body model; the others are attachments (helm, weapons, pads).
    bool armour;
};

inline constexpr std::array<D3VisualSlot, 8> kD3VisualSlots = {{
    {0, "Head", "head", false},
    {1, "Torso", "torso", true},
    {2, "Feet", "feet", true},
    {3, "Hands", "hands", true},
    {4, "Right hand", "righthand", false},
    {5, "Left hand", "lefthand", false},
    {6, "Shoulders", "shoulders", false},
    {7, "Legs", "legs", true},
}};

/// A slot by its command-line name or its ordinal written in digits.
inline std::optional<i32> D3VisualSlotFromCliName(std::string_view name) {
    for (const D3VisualSlot& s : kD3VisualSlots)
        if (name == s.cliName)
            return s.ordinal;
    if (name.empty() || name.find_first_not_of("0123456789") != std::string_view::npos)
        return std::nullopt;
    i32 ordinal = 0;
    for (const char c : name) {
        ordinal = ordinal * 10 + (c - '0');
        if (ordinal >= static_cast<i32>(kD3VisualSlots.size()))
            return std::nullopt;
    }
    return ordinal;
}

} // namespace whiteout::flakes
