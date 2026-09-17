#pragma once

// ============================================================================
// "Export to M3" — writing the model on screen as a StarCraft II file.
//
// `mdx_export.h`'s twin, pointed the other way: a `.mdx` (Classic or
// Reforged), a `.m2` or a Diablo III `.app` is converted through WEM and
// written as `.m3`. The shape is identical — WEM export, a driver-side
// surface pass, the io conversion, then textures written beside the model and
// the references repointed — and the differences are exactly the two the
// formats dictate:
//
//   * **Textures live on layers.** An `.m3` has no texture table; every layer
//     carries its own path string. The repoint therefore happens on the WEM
//     document *before* the conversion — `toM3` builds its path table from
//     `document.textures` in order — rather than on the produced model.
//   * **One container.** StarCraft II reads `.dds` and nothing else this
//     export would write, so there is no per-profile encoder choice.
//
// The Warcraft III material conventions (team colour as a channel select, the
// 0.75 test as threshold 192, geoset animation as Color-flag layers) are the
// driver-side surface pass, landed by WC3_TO_SC2_DESIGN.md's phases; this
// file is the loop they hang off.
// ============================================================================

#include "export_common.h"
#include "export_options.h"

#include "whiteout/flakes/util/replaceable_paths.h" // Tileset

#include <string>

namespace whiteout::flakes {

/// @brief One export.
struct M3ExportRequest {
    ExportSubject subject;
    M3ExportOptions options;
    /// Warcraft III only: the tileset the replaceables the game keys on one
    /// resolve through (11 the cliff texture, 31–37 the trees).
    io::Tileset tileset = io::Tileset::LordaeronSummer;
    /// With `options.war3ModTextures`: the StarCraft II install War3 (Mod)'s
    /// copies are read from. The map must depend on War3 (Mod) to find them.
    std::string starCraft2Install;
};

/// @brief What one export did.
struct M3ExportReport : ConversionReport {
    /// Summed over the models the emitters spawn.
    TextureExportCounters textures;

    int particleRecords = 0; ///< `PAR_` crossed from Warcraft III emitters.
    int ribbonRecords = 0;   ///< `RIB_` crossed from Warcraft III ribbons.
    int cameraRecords = 0;   ///< `CAM_` aimed at their Warcraft III targets.
    int hitTests = 0;        ///< Fuzzy hit tests written for collision shapes.
    int modelParticles = 0;  ///< Of the `PAR_`, model particles from spawning emitters.
    int spawnedModels = 0;   ///< `.m3` written for the models they spawn, theirs included.
};

/// @brief Convert @p request's model to StarCraft II and write it.
M3ExportReport ExportModelAsM3(const M3ExportRequest& request);

} // namespace whiteout::flakes
