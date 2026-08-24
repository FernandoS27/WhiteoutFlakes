#pragma once

// Small value types shared by the Diablo III adapter and the diablo3 profile.
//
// Split out so `d3_surface_table.h` can take the adapter's emission order and
// canonical texture list without including the adapter (and the content
// provider behind it), and so neither side can drift a field.

#include "whiteout/flakes/types.h"

namespace whiteout::flakes::io {

/// @brief One referenced texture, in canonical (first-seen, deduped) order.
struct D3TextureRef {
    i32 snoId = -1;
    /// @brief Wanted as a cubemap. Always false in v1: no slot the shading
    ///        model consumes samples a cube, and D3 states cube-ness inside the
    ///        `.tex` rather than on the material — so asking here would mean
    ///        reading every texture once to learn something nothing uses.
    bool cube = false;
};

/// @brief Does this material texture entry name a texture THIS material owns?
///
/// `dwTextureType == 0` is Render_ResolveMaterialTextureStages' default branch —
/// "the entry's own texture" — and measurement says it is the only per-material
/// one: for Barbarian_Male, `oneBatch_mat` names texture 95384 through it and
/// `Skeleton_mat` names 48525. The named types (2 Lightmap, 3 NormalMap, 8
/// Irradiance, ...) arrive as a model-wide block of core-asset ids that is
/// byte-identical across every material in the file.
///
/// Lives here rather than in the surface table because the adapter's canonical
/// texture list and the table's slot resolution have to agree on it *by
/// construction*: a list that collected the shared block would acquire forty
/// GPU textures per model that no slot ever samples.
inline bool D3EntryOwnsTexture(i32 textureType) {
    return textureType == 0;
}

/// @brief Where an emitted geoset's SubObject lives.
///
/// `geosetId` is an index into the adapter's emission order and this is what it
/// names. Carried rather than re-derived so every per-geoset accessor takes the
/// same skips.
struct D3SubObjectRef {
    u32 geoSet = 0; ///< 0 = tGeoSet0, 1 = tGeoSet1.
    u32 index = 0;
};

} // namespace whiteout::flakes::io
