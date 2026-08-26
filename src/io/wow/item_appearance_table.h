#pragma once

// ============================================================================
// ItemAppearanceTable — what a weapon, shield or helm `.m2` is wearing.
//
// The same problem CreatureSkinTable solves, for the other family of models.
// An item's object component declares texture type 2 and leaves TXID zero,
// because which skin goes there is a property of the *appearance* the item was
// equipped as, not of the model: `shield_1h_artifactmagnar_d_03.m2` is one
// mesh with four looks, and the four `.blp`s beside it are not named in the
// file at all. Measured on a stock 11.x install, 562 of 593 sampled
// `item/objectcomponents/` models declare such a slot — which is why so many
// of them render flat grey in a viewer.
//
// The join, every position verified against the corpus dump:
//
//   ModelFileData        (id = the `.m2` fileDataID) → ModelResourcesID
//   ItemDisplayInfo      ModelResourcesID[0..1]      → the display rows using
//                                                      that model, and which
//                                                      of their two model
//                                                      slots it fills
//   ItemDisplayInfoModelMatRes (by display)          → (MaterialResourcesID,
//                                                      textureType, modelIndex)
//   TextureFileData      (by MaterialResourcesID)    → the `.blp` fileDataID
//
// `shield_1h_artifactmagnar_d_03.m2` (1241201) resolves through it to
// 1241203/04/05/06, which the community listfile confirms are its blue, green,
// red and yellow siblings.
//
// ---------------------------------------------------------------------------
// Why the relation table and not the inline array
//
// ItemDisplayInfo still carries `ModelMaterialResourcesID[2]` inline, and for
// a plain shield it agrees with the relation table. It is not enough on its
// own for two reasons, both measured:
//
//   * It has no room for a type. The inline array is type 2 by convention,
//     while ItemDisplayInfoModelMatRes tags every material with the type it
//     fills — and 30991 of its 141309 rows are types 3, 4, 5 or 24, which the
//     array cannot express. A weapon whose blade and handle are separate
//     textures needs those.
//   * It is not complete. 25483 (display, material) pairs exist only in the
//     relation table, against 47 that exist only inline.
//
// So the relation table is the source and the inline array is not read. The 47
// inline-only pairs are the cost, and no sampled model needed them.
//
// ---------------------------------------------------------------------------
// Keyed by ModelResourcesID, not by file
//
// Item models share a resource id far more than anything else does: 63711
// `item/objectcomponents/` files resolve to 8411 distinct ids, because every
// LOD and variant of one object is one resource. Grouping the looks by file
// would store each of them 7.6 times over.
// ============================================================================

#include "io/wow/replaceable_slots.h"
#include "whiteout/flakes/types.h"

#include <span>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::io {
class IContentProvider;
class ProgressMonitor;
}

namespace whiteout::flakes::io::wow {

/// One look one item model can wear: a texture fileDataID per replaceable slot
/// (see replaceable_slots.h). Zero means this appearance leaves that slot
/// alone, and it keeps the white default.
struct ItemAppearance {
    u32 displayId = 0;
    u32 texture[kReplaceableSlots] = {};
};

/// The four client tables, parsed and joined.
///
/// A value rather than a global, for the same reason CreatureSkinTable is: the
/// rows belong to the install they were read from.
class ItemAppearanceTable {
public:
    /// Parse and join. Cheap to call repeatedly — the first success latches,
    /// and so does the failure, because re-reading four tables and half a
    /// million rows on every spawn that is not an item is not a retry, it is a
    /// stall. Clear() when the provider changes.
    /// @param progress Optional; see ChrCustomizationTable::Load.
    bool Load(IContentProvider& provider, ProgressMonitor* progress = nullptr);

    void Clear();

    bool Loaded() const noexcept {
        return loaded_;
    }

    /// The looks @p modelFileDataId can wear, ordered by display id. Empty for
    /// anything that is not an item model, which is the common case and costs
    /// one hash lookup.
    std::span<const ItemAppearance> ForModel(u32 modelFileDataId) const;

    usize ModelCount() const noexcept {
        return resByFile_.size();
    }
    usize AppearanceCount() const noexcept {
        return appearances_.size();
    }

private:
    struct Range {
        u32 begin = 0;
        u32 count = 0;
    };

    bool loaded_ = false;
    bool loadFailed_ = false;
    /// `.m2` fileDataID → ModelResourcesID, for the files an item display
    /// actually names. A model nothing references is left out, so a miss here
    /// is the whole answer for every model that is not an item.
    std::unordered_map<u32, u32> resByFile_;
    std::unordered_map<u32, Range> byModelRes_;
    std::vector<ItemAppearance> appearances_;
};

} // namespace whiteout::flakes::io::wow
