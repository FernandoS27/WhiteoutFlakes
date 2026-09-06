#pragma once

// ============================================================================
// StarCraft II -> Warcraft III Reforged, the material half.
//
// The two games light a surface from different quantities. StarCraft II states
// a **specular colour** and a **gloss**; Reforged wants a **roughness** and a
// **metalness**, packed with an occlusion term and a team-colour mask into one
// map it calls the ORM. `DeriveProfile` cannot make that crossing on its own —
// it moves slot references around and never opens a texture, and there is no
// slot in an `.m3` whose *pixels* are a Reforged ORM. So the crossing is a
// bake, and a bake needs a content provider, which is why it lives in the host
// beside "Export to MDX" rather than in WhiteoutLib beside the converters.
//
// What crosses, and how:
//
//   * **emissive -> emissive** is already a rename; `DeriveProfile` does it.
//   * **normal -> normal**, but restated. StarCraft II packs DXT5nm — x in
//     alpha, y in green — and Reforged ships BC5, which is x in red. A
//     channel move and nothing else: the axes agree between the engines; see
//     `kNormalRestatement`.
//   * **specular + gloss + the exponent + AO + the team mask -> ORM**, baked by
//     `textures::pbr::BakeOrm`. The roughness comes from the material's
//     `specularExponent` and not from the map — the map's job is the metalness,
//     which in Reforged is the specular knob and not a classification.
//   * **diffuse -> base colour, rewritten**, by `textures::pbr::BakeBaseColor`.
//     Reforged reads `F0 = metalness * albedo`, so the metalness above is paid
//     for out of the albedo and the albedo has to be raised by the same amount
//     or the model comes back darker than it went in. The team mask lightens it
//     in the same pass.
//   * **the base colour's alpha is REPLACED**, because in StarCraft II it is
//     not opacity: it is the team-colour mask, and it has just been written
//     into the ORM's alpha where Reforged reads one. What goes in its place is
//     the coverage the source actually blends and alpha-tests by — the two
//     alpha-mask layers, composed (`cFinal.a = mask1.a * mask2.a`) — which no
//     unbaked export could carry, Reforged having no slot for a mask texture.
//     With no masks the alpha is opaque.
//
// `pbr_bake.h` carries the algebra and the measurements behind every number in
// it; this file's job is to read StarCraft II's fields correctly and hand them
// over.
//
// The team-colour SWATCH the mask selects is not here: `mdx_core::exportPbr`
// puts replaceable 1 in every Reforged material's slot 4, the way all 12,893
// shipped HD layers do, because the stock ORM's own alpha is 0 and a material
// with no mask is therefore tinted nowhere. Nothing StarCraft II-specific was
// ever needed for it.
//
// Nothing is written to disk here. The bake hands back decoded textures keyed
// by *document* texture index and `ExportTextures` writes them through the same
// encoder every other texture goes through — one writer, one naming rule, and
// a baked map that lands beside its model looking like every other file there.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <whiteout/models/wem/diagnostics.h>
#include <whiteout/models/wem/document.h>
#include <whiteout/textures/pbr_bake.h>
#include <whiteout/textures/texture.h>

#include <map>
#include <vector>

namespace whiteout::flakes {

namespace io {
class IContentProvider;
}

namespace wem = ::whiteout::models::wem;
namespace tx = ::whiteout::textures;

/// One texture the export has to write out of nothing a file holds.
struct BakedTexture {
    tx::Texture texture;
    /// The container it wants. BC5 for a two-channel normal — which is what
    /// Reforged's own normal maps are — and BC3 for an ORM, whose alpha is the
    /// team-colour mask and so cannot be dropped.
    tx::PixelFormat format = tx::PixelFormat::BC3;
};

struct Sc2PbrBakeResult {
    /// Keyed by index into `Document::textures`. `toMdx` writes one MDX texture
    /// per document texture in order, so the same index addresses both.
    std::map<u32, BakedTexture> baked;

    int ormBaked = 0;
    int normalsRestated = 0;
    /// Base colours rewritten under a team mask: lightened where the mask
    /// selects, and made opaque — or, where the material carries alpha-mask
    /// layers, given the composed cutout instead (see `coverageComposed`).
    int baseColorsCleared = 0;
    /// Of those, how many carried a real per-texel coverage — StarCraft II's
    /// alpha-mask layers composed into the base colour's alpha, encoded BC3.
    int coverageComposed = 0;
    /// Materials whose source layers could not be resolved or decoded. Their
    /// slots are left exactly as the derive wrote them.
    int materialsSkipped = 0;

    bool empty() const {
        return baked.empty();
    }
};

/// @brief Give @p document a Warcraft III Reforged material set with its PBR
///        slots filled from StarCraft II's specular/gloss ones.
///
/// Derives the Reforged set if the document does not already carry it — the
/// same derive `StageWemDocument` would have run, done early so the slots this
/// fills are there to fill. A document carrying no StarCraft II set is left
/// alone and the result comes back empty, which is not an error.
///
/// @param provider  where a texture reference resolves. Null does nothing.
/// @param normal    the two conventions the normal-map packing does not settle.
Sc2PbrBakeResult BakeSc2AsReforgedPbr(wem::Document& document, io::IContentProvider* provider,
                                      const tx::pbr::NormalRestatement& normal,
                                      wem::Diagnostics& out);

} // namespace whiteout::flakes
