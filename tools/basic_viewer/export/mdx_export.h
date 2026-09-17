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

#include "export_common.h"
#include "export_options.h"

#include <whiteout/models/wem/profile.h>

namespace whiteout::flakes {

/// @brief One export.
struct MdxExportRequest {
    ExportSubject subject;
    MdxExportOptions options;
};

/// @brief What one export did.
struct MdxExportReport : ConversionReport {
    /// `inWar3Mod` stays zero: War3 (Mod) is StarCraft II's.
    TextureExportCounters textures;
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
const char* Wc3TextureExtension(::whiteout::models::wem::ProfileId profile);

} // namespace whiteout::flakes
