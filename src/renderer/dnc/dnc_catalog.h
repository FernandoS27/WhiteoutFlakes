#pragma once

/// @file dnc_catalog.h
/// @brief The stock day/night-cycle light models, indexed by tileset and role.
///
/// Warcraft III picks a DNC model from two `UI\WorldEditData.txt` tables,
/// `[TerrainLights]` and `[UnitLights]`, keyed by the map's one-character
/// tileset code — `WEUDataGetTileSetLight(code, isUnit, …)`. Codes with no
/// entry fall back to Lordaeron, which is also what `CWorldFrameWar3` seeds
/// and what `CPortraitButton` hardcodes for the portrait rig.
///
/// Only six DNC families ship, so most tilesets share Lordaeron. Every family
/// exists in both SD and HD except the SD-only, unreferenced Lordaeron target
/// rig; @ref DncCatalogEntry records that so a host can offer the variants
/// that are actually installed.

#include "whiteout/flakes/enums.h" // Wc3ArtTier
#include "whiteout/flakes/types.h"

#include <span>
#include <string>
#include <string_view>

namespace whiteout::flakes::renderer::dnc {

/// @brief Which light rig a DNC model drives.
enum class DncRole : u8 {
    Unit,     ///< Lights units and doodads.
    Terrain,  ///< Lights the terrain mesh.
    Portrait, ///< Lights the portrait viewport.
    Target,   ///< Legacy rig, SD-only and unreferenced by the retail client.
};

/// @brief Which mod layer a DNC path is pinned to.
///
/// One value per Wc3ArtTier plus `Auto`, and in the same order, so the two
/// convert by cast. They stay separate types because a pin is not a tier: a
/// scene reading Definitive art can still want the classic rig's lighting, and
/// `Auto` — which has no tier — is the whole point of the setting.
enum class DncVariant : u8 {
    Auto, ///< Resolve through the content provider's own art-tier chain.
    Sd,   ///< Pin to `war3.w3mod:`.
    Hd,   ///< Pin to `war3.w3mod:_hd.w3mod:`.
    De,   ///< Pin to `war3.w3mod:_de.w3mod:`.
};

/// @brief The pin that names @p tier exactly.
constexpr DncVariant DncVariantForTier(Wc3ArtTier tier) {
    switch (tier) {
    case Wc3ArtTier::Definitive:
        return DncVariant::De;
    case Wc3ArtTier::Reforged:
        return DncVariant::Hd;
    default:
        return DncVariant::Sd;
    }
}

/// @brief One installed DNC model.
struct DncCatalogEntry {
    std::string_view family; ///< "Lordaeron", "Ashenvale", …
    DncRole role;
    /// Unpinned path, in the same `/`-separated `.mdl` spelling the game's
    /// own string table uses. CASC stores these as `.mdx`; the content
    /// provider tries both extensions.
    std::string_view path;
    bool hasSd;
    bool hasHd;
    /// Definitive ships every rig Reforged does — 13 of the 14 below, all but
    /// Lordaeron's SD-only legacy target — plus three campaign rigs
    /// (`DNCCampaign/Act2OutdoorDNC`, `CloudyDNC`, `DNCUndercityFinal`) that
    /// are not family/role shaped and so are not in this table.
    bool hasDe;
};

/// @brief One World Editor tileset and the DNC family it selects.
struct DncTilesetEntry {
    char code;               ///< `[TileSets]` key, e.g. `'L'`.
    std::string_view name;   ///< "Lordaeron Summer"
    std::string_view family; ///< DNC family both light tables map it to.
};

/// @brief Which of a tileset's two environment probes to resolve.
enum class EnvMapPhase : u8 { Day, Night };

/// @brief World IBL probe for @p tilesetCode.
///
/// `CWorldFrameWar3`'s ctor calls `WorldEnvironmentMapLoadForTileset`, which
/// reads `[DayEnvironmentMap]` / `[NightEnvironmentMap]` from
/// `UI\WorldEditData.txt` keyed by the same one-character tileset code as the
/// light tables, falling back to each table's `Default` row. This is where HD
/// ambient comes from — the HD DNC rigs ship `ambientIntensity = 0`, and
/// `CGxLightToShaderLight` only folds `ambColor` in on the SD-on-HD path, so
/// the probe is the *only* ambient the plain-HD shader sees.
std::string_view EnvMapForTileset(char tilesetCode, EnvMapPhase phase);

/// @brief Probe the portrait rig uses (`[PortraitEnvironmentMap]`).
///
/// `CPortraitButton` loads this one under its own `ScopedGxuLightContext` and
/// never touches the world tables — it is not a valid world probe.
std::string_view PortraitEnvMap();

/// @brief Every DNC model in a stock install, grouped by family.
std::span<const DncCatalogEntry> DncCatalog();

/// @brief The 18 World Editor tilesets, in `[TileSets]` order.
std::span<const DncTilesetEntry> DncTilesets();

/// @brief Unpinned path for @p tilesetCode / @p role, mirroring
///        `WEUDataGetTileSetLight`: unknown codes fall back to Lordaeron.
std::string_view DncPathForTileset(char tilesetCode, DncRole role);

/// @brief Rewrite @p path so it resolves from a specific mod layer.
std::string DncPathForVariant(std::string_view path, DncVariant variant);

/// @brief Index into @ref DncCatalog for @p path, ignoring any pinning,
///        separator and case differences. `-1` when @p path is not stock.
i32 DncCatalogIndexOf(std::string_view path);

/// @brief Which layer @p path is pinned to.
DncVariant DncVariantOf(std::string_view path);

/// @brief Display label for a catalog entry, e.g. "Lordaeron Unit".
std::string DncEntryLabel(const DncCatalogEntry& e);

} // namespace whiteout::flakes::renderer::dnc
