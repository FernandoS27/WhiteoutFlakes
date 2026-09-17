#pragma once

// ============================================================================
// Diablo III outfit presets: whole outfits by name in `d3_outfits.ini`, beside
// the viewer settings. Items are stored by record NAME, so a preset survives a
// re-parse of the item tables. Device-free.
//
//   [<name>]
//   Slot<n>=<item stem>      one per equipped visual slot
//   Dye<n>=<row>             only when dyed
//   Sheathed=0|1
// ============================================================================

#include "features/d3_visual_slots.h"
#include "whiteout/flakes/types.h"

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout::flakes {

struct D3OutfitPreset {
    /// Per visual slot. Empty when the preset names nothing there, which
    /// unequips the slot on load.
    std::array<std::optional<std::string>, kD3VisualSlots.size()> items;
    /// 0 is undyed, and is not written.
    std::array<i32, kD3VisualSlots.size()> dyes{};
    /// Absent in the file leaves the current sheathe alone.
    std::optional<bool> sheathed;
};

/// The names of the presets in @p file, sorted.
std::vector<std::string> ListD3OutfitPresets(const std::filesystem::path& file);
/// @p name's preset. A name the file does not hold reads as an empty preset.
D3OutfitPreset ReadD3OutfitPreset(const std::filesystem::path& file, std::string_view name);
/// Replace @p name's section in @p file, keeping every other preset.
void WriteD3OutfitPreset(const std::filesystem::path& file, std::string_view name, const D3OutfitPreset& preset);

} // namespace whiteout::flakes
