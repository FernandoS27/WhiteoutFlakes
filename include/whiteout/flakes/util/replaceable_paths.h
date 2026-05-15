#pragma once

/// @file replaceable_paths.h
/// @brief Tileset enumeration + replaceable-texture path resolver.
///
/// WC3 models reference textures by *replaceable ID* — a u32 that maps to
/// a tileset-dependent on-disk path. This resolver mirrors the engine's
/// canonical mapping (terrain tiles, cliff sets, water, team-colour /
/// team-glow synthetics) and lets the texture manager swap entire tile
/// sets at runtime by changing `SetCurrentTileset`.

#include "../types.h"

namespace whiteout::flakes::io {
class IContentProvider;
}

namespace whiteout::flakes::io {

/// @brief The 16 canonical WC3 tilesets, ordered to match the in-game
///        World Editor enum so persisted indices round-trip.
enum class Tileset : u8 {
    LordaeronSummer = 0,
    Ashenvale,
    Barrens,
    Northrend,
    Felwood,
    Dungeon,
    Cityscape,
    LordaeronFall,
    LordaeronWinter,
    Outland,
    SunkenRuins,
    IcecrownGlacier,
    DalaranRuins,
    BlackCitadel,
    Underground,
    Village,
    Count, ///< Sentinel: number of valid tilesets, not a value itself.
};

/// @brief Display name for a tileset (English, used by host UI dropdowns).
const char* TilesetName(Tileset ts);

/// @brief Set the active tileset that subsequent `ReplaceableCanonicalPath`
///        calls resolve against. Process-global state.
void SetCurrentTileset(Tileset ts);
/// @brief Read the active tileset previously set via `SetCurrentTileset`.
Tileset GetCurrentTileset();

/// @brief Resolve a replaceable-ID to its canonical path under a specific
///        tileset.
/// @return Static string literal (never `nullptr`; unknown IDs return an
///         empty string).
const char* ReplaceableCanonicalPath(i32 replaceableId, Tileset ts);

/// @brief Convenience overload that uses `GetCurrentTileset()`.
const char* ReplaceableCanonicalPath(i32 replaceableId);

/// @brief Pre-warm engine-side tables (terrain SLK, cliff data, …) by
///        loading the necessary game-data files through @p cp.
/// @param cp     Content provider used for the read.
/// @param force  When `true`, re-load even if a previous successful load
///               cached the tables. Default `false`.
void LoadGameDataFiles(IContentProvider* cp, bool force = false);

} // namespace whiteout::flakes::io

namespace whiteout::flakes {
using ::whiteout::flakes::io::GetCurrentTileset;
using ::whiteout::flakes::io::LoadGameDataFiles;
using ::whiteout::flakes::io::ReplaceableCanonicalPath;
using ::whiteout::flakes::io::SetCurrentTileset;
using ::whiteout::flakes::io::Tileset;
using ::whiteout::flakes::io::TilesetName;
} // namespace whiteout::flakes
