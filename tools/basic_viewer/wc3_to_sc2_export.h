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
//    roughness come back apart — `spec = metalness · albedo` and the diffuse
//    what the metal leaves, `(1 − metalness) · albedo` (Reforged has no
//    dielectric F0, so both are exact; under the team mask the metal stays
//    diffuse, as StarCraft II cannot tint a reflection), the exponent through
//    `GlossCeilingExponent`, the amplitude at the 2 StarCraft II's own
//    roughness-simulating materials use. The team share
//    crosses through `tx::pbr::TeamReplaceFromBlend`: Reforged BLENDS the
//    team hue in and keeps the art's brightness, StarCraft II REPLACES the
//    texel where the diffuse alpha is LOW (both measured), so the share
//    becomes the texel's brightness and the shading painted under the mask
//    comes through as shades of the team colour. That alpha carries nothing
//    else: a keyed material's cutout stays in the source's own alpha, named
//    by the mask layer. The reflection follows the
//    StarTools "Simulate Roughness" recipe, which is how StarCraft II's own
//    PBR-styled art is built: the F0 map doubles as the RGB environment
//    mask, the gloss (`1 - roughness`) rides its alpha, the material sets
//    `SimulateRoughness` so the engine blurs the cube by it, and the cube
//    itself is the tileset probe Reforged really reflects, its pre-filtered
//    chain kept (`tx::env::CubeFromCube`).
// ============================================================================

#include "sc2_pbr_export.h" // BakedTexture

#include "whiteout/flakes/util/replaceable_paths.h" // Tileset

#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>

#include <map>
#include <string>
#include <vector>

namespace whiteout::flakes {

namespace io {
class IContentProvider;
}

namespace wem = ::whiteout::models::wem;

/// The Classic arm's knobs (WC3_SD_MATERIAL_TO_SC2_DESIGN.md §5).
struct Wc3ToSc2Options {
    /// Which tileset resolves the replaceables the game keys on one (11 the
    /// cliff texture, 31–37 the trees). Cursor (21) has no file and stays out.
    io::Tileset tileset = io::Tileset::LordaeronSummer;
    /// Bake a keyed pass's alpha to 0/255 where it sits over another pass, so
    /// the StarCraft II lerp reproduces Warcraft III's 0.75 key instead of
    /// softening it. Moves pixels; off by default, as Blizzard's own
    /// conversions did not.
    bool sharpenTeamKey = false;
    /// A composite section for every pass the one material can only
    /// approximate (`m3_core::Context::exactPasses`).
    bool exactPasses = false;
    /// Decode every texture once for its alpha class, which the fold uses to
    /// tell a covering base from a partial one and to drop a team plate no
    /// texel reveals. Costs one decode per texture.
    bool classifyTextures = true;
    /// Rename attachment points to StarCraft II's `Ref_` names (§4.1).
    bool attachmentNames = true;
    /// Cross the particle and ribbon emitters into `PAR_` / `RIB_`
    /// (WC3_TO_SC2_COMPLETION_PLAN.md C5, C7). Off writes the model's nodes
    /// and nothing they emit.
    bool effects = true;
    /// Add the references StarCraft II looks for and a Warcraft III model does
    /// not name -- `Ref_Origin`, `Ref_Overhead`, `Ref_Center`, `Ref_Target` --
    /// and the `Vol_Target` targeting volume (WC3_TO_SC2_COMPLETION_PLAN.md
    /// C3.2-C3.3).
    bool standardRefs = true;
};

/// What the pass produced beyond the document edits.
struct Wc3ToSc2Result {
    /// Textures the bake invented, keyed by `Document::textures` index — the
    /// export's texture writer consumes them in place of a provider read.
    std::map<u32, BakedTexture> baked;
    int materialsRestated = 0; ///< HD materials rebuilt as spec/gloss.
    int materialsSkipped = 0;  ///< HD materials whose sources would not decode.
    /// Per `Document::textures` entry, an `m3_core::TextureAlphaClass` byte
    /// (0 unknown, 1 opaque, 2 keyed, 3 gradient); empty when none decoded.
    std::vector<u8> textureAlphaClasses;
    int texturesSharpened = 0; ///< Keyed passes whose alpha was baked binary.
    /// The `Vol_Target` node the standard set added, and the scale its bone
    /// takes (StarCraft II axis order) -- a rest scale no retarget keeps, so
    /// the export states it on the bone. Invalid when none was added.
    u32 volTargetNode = ~0u;
    Vector3f volTargetScale{1.0f, 1.0f, 1.0f};
};

/// @brief Restate a Warcraft III document's conventions for a StarCraft II
///        export, in place. A no-op for a document that is not Warcraft III.
Wc3ToSc2Result RestateWc3AsSc2(wem::Document& document, io::IContentProvider* provider,
                               wem::Diagnostics& diagnostics,
                               const Wc3ToSc2Options& options = {});

} // namespace whiteout::flakes
