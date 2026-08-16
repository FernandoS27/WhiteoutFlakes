#pragma once

// ============================================================================
// ChrCustomizationTable — the client databases that dress a character `.m2`.
//
// A character model leaves two things blank that only the game can fill: which
// of its geosets draw, and what its body texture looks like. In Warlords both
// came out of `CharSections`, which is what the 6.0.1 client reverse-engineered
// in character_geosets.h reads. That table was removed in Shadowlands and the
// whole system was rebuilt around per-model *options* and *choices*, which is
// what a retail install ships and therefore what this reads.
//
// The chain, every step verified against the corpus dump for human male:
//
//   ChrRaceXChrModel   (race, sex)          → ChrModelID
//   ChrModel                                → CharComponentTextureLayoutID,
//                                             DisplayID, SkeletonFileDataID
//   CreatureDisplayInfo → CreatureModelData → the `.m2` fileDataID
//   ChrCustomizationOption  (by ChrModelID) → "Skin Color", "Hair Style", …
//   ChrCustomizationChoice  (by option)     → the pickable values
//   ChrCustomizationElement (by choice)     → geoset row and/or material row
//   ChrCustomizationGeoset                  → (GeosetType, GeosetID)
//   ChrCustomizationMaterial                → (TextureTarget, MaterialResources)
//   TextureFileData         (by resources)  → the `.blp` fileDataID
//
// and, for where those textures go:
//
//   ChrModelMaterial     (by layout)        → one composite per M2 texture type
//   ChrModelTextureLayer (by layout)        → the layers of each composite
//   CharComponentTextureSections (by layout)→ the rect each section names
//
// Human male resolves to ChrModel 1, layout 103, a 2048×1024 type-1 composite,
// and `.m2` fileDataID 1011653 — which the community listfile confirms is
// `character/human/male/humanmale_hd.m2`.
//
// ---------------------------------------------------------------------------
// Two things this reader will not do
//
// **No Schema binding.** `Table::bind` matches every column one-to-one, so one
// added column in a later build takes the whole feature down. Every position
// below is a field index, checked against a shape only that layout has.
//
// **No path lookups.** Every join is by id, and every texture comes out as a
// fileDataID, because that is the only identity a retail root recognises.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::io {
class IContentProvider;
}

namespace whiteout::flakes::io::wow {

/// One rect of a composite sheet, from CharComponentTextureSections.
struct TextureSection {
    u32 sectionType = 0;
    u32 x = 0, y = 0, w = 0, h = 0;
};

/// One composite texture a model wants, from ChrModelMaterial. `textureType` is
/// the `M2Texture::type` it binds to — 1 for the body, 6 for hair, 19 for eyes.
struct CompositeTarget {
    u32 textureType = 0;
    u32 width = 0, height = 0;
};

/// One layer of one composite, from ChrModelTextureLayer. Ordered by `layer`.
///
/// `sectionMask` is a bit per section type. Exactly one bit means "this layer's
/// texture is that section's rect"; all-ones means "the whole sheet", which is
/// how the base skin arrives as a single 2048×1024 file rather than eight
/// pieces the way Warlords pasted it.
struct CompositeLayer {
    u32 textureType = 0;
    u32 layer = 0;
    u32 blendMode = 0;
    u32 target = 0; ///< ChrModelTextureTargetID; matched against a material.
    i64 sectionMask = -1;
};

/// One pickable value inside one option.
struct CustomizationChoice {
    u32 id = 0;
    u32 orderIndex = 0;
    std::string name; ///< Often empty — colour swatches are unnamed.
};

/// One customisation axis of one model: "Hair Style", "Skin Color".
struct CustomizationOption {
    u32 id = 0;
    u32 orderIndex = 0;
    std::string name;
    std::vector<CustomizationChoice> choices;
};

/// What one choice contributes. A choice usually has several, and each is
/// gated: `relatedChoiceId` non-zero means the element applies only when that
/// *other* choice is also selected. Face textures are per skin colour, and this
/// is how the table says so.
struct ChoiceElement {
    u32 relatedChoiceId = 0;
    i32 geoset = -1;               ///< skinSectionId, or -1.
    u32 materialTarget = 0;        ///< ChrModelTextureTargetID, or 0.
    u32 materialResourcesId = 0;
};

/// Everything the tables know about one character model.
struct ChrModelInfo {
    u32 id = 0;
    u32 raceId = 0; ///< From ChrRaceXChrModel; 0 when no race claims it.
    u32 sex = 0;
    u32 layoutId = 0;
    u32 displayId = 0;
    u32 modelFileId = 0;   ///< The `.m2` this model is.
    u32 skeletonFileId = 0;
    std::vector<CompositeTarget> composites;
    std::vector<CompositeLayer> layers; ///< Sorted by (textureType, layer).
    std::vector<TextureSection> sections;
    std::vector<CustomizationOption> options;

    const TextureSection* Section(u32 sectionType) const;
    const CompositeTarget* Composite(u32 textureType) const;
};

class ChrCustomizationTable {
public:
    /// Read every table through @p provider. Idempotent after success; retries
    /// after failure, since the install may have been configured in between.
    ///
    /// Synchronous, on the caller's thread, next to a `.m2` parse that already
    /// blocks on IO. Roughly 300k rows across twelve tables.
    bool Load(IContentProvider& provider);

    bool Loaded() const noexcept {
        return loaded_;
    }
    void Clear();

    /// The character model whose `.m2` is @p modelFileId, or null — which is
    /// the answer for every creature, doodad and item, and for a character
    /// model opened from a storage the tables did not come from.
    const ChrModelInfo* ModelForFile(u32 modelFileId) const;

    /// Every element of @p choiceId, in table order.
    std::span<const ChoiceElement> Elements(u32 choiceId) const;

    /// The `.blp` fileDataID behind @p materialResourcesId, preferring
    /// `UsageType == 0` (the diffuse sheet). Zero when nothing names it.
    u32 TextureFileFor(u32 materialResourcesId) const;

    usize ModelCount() const noexcept {
        return models_.size();
    }

private:
    bool loaded_ = false;
    /// One failed attempt is enough — see Load. Cleared with everything else.
    bool loadFailed_ = false;
    std::unordered_map<u32, ChrModelInfo> models_; ///< By ChrModelID.
    std::unordered_map<u32, u32> modelByFile_;     ///< `.m2` fileDataID → ChrModelID.
    // Flattened, one contiguous run per choice — the same shape
    // CreatureSkinTable uses, and for the same reason: 36k elements is a lot of
    // little vectors.
    std::vector<ChoiceElement> elements_;
    struct Range {
        u32 begin = 0;
        u32 count = 0;
    };
    std::unordered_map<u32, Range> elementsByChoice_;
    std::unordered_map<u32, u32> textureByResources_;
};

} // namespace whiteout::flakes::io::wow
