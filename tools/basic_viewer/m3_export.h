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

#include "io/wem/wem_export.h"
#include "wc3_to_sc2_export.h" // Wc3ToSc2Options
#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/types.h"

#include <whiteout/models/wem/diagnostics.h>

#include <filesystem>
#include <string>

namespace whiteout::flakes {

namespace io {
class IContentProvider;
}

namespace wem = ::whiteout::models::wem;

/// @brief One export.
struct M3ExportRequest {
    /// The model on screen. Must be one `io::CanExportModelToWem` accepts.
    renderer::model::IModelSource* source = nullptr;
    /// Where every texture reference resolves. Null exports the model alone.
    io::IContentProvider* provider = nullptr;

    std::filesystem::path outPath;
    /// Names the model inside the file, and the fallback stem for a texture
    /// the source addressed by id and never named.
    std::string modelName;

    /// `Sc2` (v29) or `Heroes` (v30).
    wem::ProfileId profile = wem::ProfileId::Sc2;

    /// Write the model's textures beside it as `.dds` and repoint the model
    /// at them. Only the ones the written model reads.
    bool exportTextures = true;

    /// Warcraft III to StarCraft II only: where StarCraft II's War3 (Mod)
    /// ships a texture as the same picture (`war3_<name>.dds`), name that copy
    /// instead of writing one. The map must depend on War3 (Mod) to find it.
    bool reuseWar3ModTextures = false;
    /// The StarCraft II install those copies are read from.
    std::string starCraft2Install;

    /// Diablo III only: which look's materials are written. Empty means the
    /// one the actor is wearing.
    std::string materialLook;

    /// Warcraft III only: the Classic arm's knobs (tileset, the key bake, the
    /// exact-passes composite).
    Wc3ToSc2Options wc3;
};

/// @brief What one export did, in the shape a log line and a dialog both want.
struct M3ExportReport {
    bool ok = false;
    std::string error;

    /// The converter the model came through — "mdx", "m2", "m3", "d3".
    std::string formatId;
    /// The factor the geometry was restated at: 1/100 for a Warcraft III
    /// model, 17/100 for Diablo III, 1 for StarCraft II's own.
    f32 scale = 1.0f;
    /// Whether the StarCraft II material set was derived rather than carried.
    bool derived = false;

    int texturesExported = 0;
    int texturesSkipped = 0;   ///< Copies already present at the target (a bake is rewritten).
    int texturesFailed = 0;    ///< Unresolvable, undecodable or unwritable.
    int texturesUnused = 0;    ///< Nothing the written model reads names them; not written.
    int texturesInWar3Mod = 0; ///< Named at War3 (Mod)'s own copy instead of written.

    /// Conversion + derive + rescale diagnostics. Never empty on success:
    /// every cross-format write has something to say about what it could not
    /// carry.
    wem::Diagnostics diagnostics;
};

/// @brief Convert @p request's model to StarCraft II and write it.
M3ExportReport ExportModelAsM3(const M3ExportRequest& request);

} // namespace whiteout::flakes
