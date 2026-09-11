#pragma once

// ============================================================================
// "Export to MDX" — writing the model on screen as a Warcraft III file.
//
// Save As re-serialises a Warcraft III model back to its own format. This is
// the other direction: a `.m2`, a `.m3` or a Diablo III `.app` is converted
// through WEM and written as `.mdx`, which is the whole point of having an
// interchange format the four games share.
//
// Three things separate it from an in-memory cross-profile open (§12 of
// WEM_INTEGRATION_DESIGN.md), and all three are why this is not just
// `BuildWemSource` with a writer bolted on:
//
//   * **Scale.** WEM keeps geometry in the units it was authored in, and an
//     open compensates by stamping the actor's `worldScale`. A file has no host
//     to stamp: it has to arrive on Warcraft III's grid already the right size.
//   * **Textures.** A World of Warcraft or Diablo III model names its textures
//     by id. Nothing outside those games can resolve one, so the files are
//     written out beside the model and the references rewritten to name them.
//   * **Format.** Reforged reads `.dds`; classic Warcraft III reads BLP1 and
//     nothing else. The profile picks the container.
//
// Free of ImGui and of ViewerApp on purpose — the dialog is one caller, and a
// headless one (a CLI flag, a batch rip) is the other.
// ============================================================================

#include "io/wem/wem_export.h"
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
struct MdxExportRequest {
    /// The model on screen. Must be one `io::CanExportModelToWem` accepts.
    renderer::model::IModelSource* source = nullptr;
    /// Where every texture reference resolves. Null exports the model alone.
    io::IContentProvider* provider = nullptr;

    std::filesystem::path outPath;
    /// Names the model inside the file, and the fallback stem for a texture the
    /// source addressed by id and never named.
    std::string modelName;

    /// Which Warcraft III generation. Reforged today; the dialog offers classic
    /// disabled, because nothing has measured what a classic derive costs.
    wem::ProfileId profile = wem::ProfileId::Wc3Reforged;

    /// Write the model's textures beside it, converted to the profile's own
    /// container, and repoint the model at them.
    bool exportTextures = true;

    /// Diablo III only: which look's materials are written. Empty means the one
    /// the actor is wearing, which is what the user is looking at.
    std::string materialLook;
};

/// @brief What one export did, in the shape a log line and a dialog both want.
struct MdxExportReport {
    bool ok = false;
    std::string error;

    /// The converter the model came through — "m2", "m3", "d3", "mdx".
    std::string formatId;
    /// The factor the geometry was restated at: 100 for World of Warcraft and
    /// StarCraft II, 17 for Diablo III, 1 for a Warcraft III model.
    f32 scale = 1.0f;
    /// Whether the Warcraft III material set was derived rather than carried.
    bool derived = false;

    int texturesExported = 0;
    int texturesSkipped = 0; ///< Copies already present at the target (a bake is rewritten).
    int texturesFailed = 0;  ///< Unresolvable, undecodable or unwritable.

    /// Conversion + derive + rescale diagnostics. Never empty on success: every
    /// cross-format write has something to say about what it could not carry.
    wem::Diagnostics diagnostics;
};

/// @brief Convert @p request's model to Warcraft III and write it.
///
/// Returns a report rather than a bool for the reason every WEM operation does:
/// a conversion that succeeded and one that failed are different states, and so
/// are "wrote the model" and "wrote the model and could not resolve nine of its
/// textures".
MdxExportReport ExportModelAsMdx(const MdxExportRequest& request);

/// @brief The file extension @p profile's textures are written in — ".dds" for
///        Reforged, ".blp" (BLP1) for classic. Empty for anything else.
const char* Wc3TextureExtension(wem::ProfileId profile);

} // namespace whiteout::flakes
