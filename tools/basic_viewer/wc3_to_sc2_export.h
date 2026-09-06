#pragma once

// ============================================================================
// The Warcraft III → StarCraft II conventions — the driver-side half of
// WC3_TO_SC2_DESIGN.md, sitting between the WEM export and `ConvertWemToM3`
// exactly where `sc2_pbr_export.h` sits for the opposite direction.
//
// The format mechanics (slots, gate bones, blend modes) live in the library;
// what lives here is GAME knowledge: how StarCraft II spells a sequence name,
// which texture channel a team mask rides in, what Blizzard's own shipped
// conversion of this exact roster (`mods/war3.sc2mod` — 3,004 models, the
// answer key this file is written against) chose where a choice existed.
//
// Two arms share the pass:
//  * **Classic (SD)**: sequence renames and the team-glow texture path; the
//    material fold itself is library work (`m3_core::ExportMaterial`).
//  * **Reforged (HD)**: the inverse of the SC2→Reforged PBR bake. Metal and
//    roughness come back apart — `spec = metalness · albedo` (Reforged has
//    no dielectric F0, so this is exact), the exponent through
//    `ExponentFromRoughness` (the closed inverse of the forward map), the
//    amplitude parked in `hdrSpecularMultiplier = (n+2)/8` so the renderer's
//    peak-referenced `ReflectanceScale` cancels it exactly. The team share
//    INVERTS into the diffuse alpha (Reforged masks high = team, StarCraft
//    II shows team where the diffuse alpha is LOW — both measured), and the
//    forward bake's `lerp(white, albedo, 1-team)` whitening is divided back
//    out where the share leaves enough albedo to recover.
// ============================================================================

#include "sc2_pbr_export.h" // BakedTexture

#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>

#include <map>

namespace whiteout::flakes {

namespace io {
class IContentProvider;
}

namespace wem = ::whiteout::models::wem;

/// What the pass produced beyond the document edits.
struct Wc3ToSc2Result {
    /// Textures the bake invented, keyed by `Document::textures` index — the
    /// export's texture writer consumes them in place of a provider read.
    std::map<u32, BakedTexture> baked;
    int materialsRestated = 0; ///< HD materials rebuilt as spec/gloss.
    int materialsSkipped = 0;  ///< HD materials whose sources would not decode.
};

/// @brief Restate a Warcraft III document's conventions for a StarCraft II
///        export, in place. A no-op for a document that is not Warcraft III.
Wc3ToSc2Result RestateWc3AsSc2(wem::Document& document, io::IContentProvider* provider,
                               wem::Diagnostics& diagnostics);

} // namespace whiteout::flakes
