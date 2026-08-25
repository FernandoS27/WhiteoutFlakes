#pragma once

// ============================================================================
// CreatureDisplayInfo → the textures a creature `.m2` deliberately leaves
// blank.
//
// A creature model names its own textures in TXID, except the ones whose
// `M2Texture::type` is non-zero. Those are slots the *client* fills, and for a
// creature that is `CCharacterComponent::ReplaceMonsterSkin` (0x100345b60):
// it walks `CreatureDisplayInfo::TextureVariation` and, for each entry that
// names something, fills every texture carrying the matching type. Nothing in
// the model says which; the display record does.
//
// **There are four variations, not three.** 6.0.1 replaces types 11, 12 and 13;
// the shipped table's array is four wide and the fourth fills type *5* — the
// slot the wiki still calls "environment (OBSOLETE)". Measured over the retail
// `CreatureDisplayInfo`: 126 models fill the fourth entry, and in a sample of
// 40 of them 13 declare a type-5 texture, against 0 of 80 sampled from the
// models that fill only two or three. `sporebat3mount` names all four —
// body, bodyglow, saddle, saddleglow — and the saddleglow is its type 5.
//
// The viewer has no display id, because it opens a model file rather than
// spawning a creature. So the join runs backwards: `CreatureModelData` names
// the `.m2` by fileDataID, and every `CreatureDisplayInfo` row pointing at
// that model is one skin the model can wear. The first is the default; the
// rest are what a "skin" picker would offer.
// ============================================================================

#include "io/wow/replaceable_slots.h"
#include "whiteout/flakes/types.h"

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::io {
class IContentProvider;
}

namespace whiteout::flakes::io::wow {

/// How many texture slots a display row can fill, and which `M2Texture::type`
/// each one fills. Position is the meaning here — entry *i* is
/// `TextureVariation[i]` — so this is the one place the 11/12/13/5 order lives.
inline constexpr u32 kMonsterSkinSlots = 4;
inline constexpr u32 kMonsterSkinTypes[kMonsterSkinSlots] = {11, 12, 13, 5};

// These *are* the first four replaceable slots, and a resolved look indexes
// both by the same number — see replaceable_slots.h.
static_assert(kMonsterSkinSlots <= kReplaceableSlots);
static_assert(kMonsterSkinTypes[0] == kReplaceableTypes[0] &&
              kMonsterSkinTypes[1] == kReplaceableTypes[1] &&
              kMonsterSkinTypes[2] == kReplaceableTypes[2] &&
              kMonsterSkinTypes[3] == kReplaceableTypes[3]);

/// One display variation of one creature model: the fileDataIDs that fill the
/// slots above. Zero means the display leaves that slot alone.
struct MonsterSkin {
    u32 displayId = 0;
    u32 texture[kMonsterSkinSlots] = {0, 0, 0, 0};
};

/// The two client tables, parsed and joined.
///
/// Deliberately a value rather than a global: two scenes can be pointed at two
/// installs, and the tables belong to the install they were read from.
class CreatureSkinTable {
public:
    /// Read and join both tables through @p provider. Idempotent — a second
    /// call after a successful load does nothing, and after a failed one tries
    /// again (the install may have been configured in between).
    ///
    /// Synchronous, on the caller's Pump thread: this runs once per install
    /// off the first `.m2` load, next to a parse that already blocks on IO.
    bool Load(IContentProvider& provider);

    bool Loaded() const noexcept {
        return loaded_;
    }

    void Clear();

    /// Every display registered against the `.m2` with this fileDataID, in
    /// table order. Empty when the model is not a creature model, or when
    /// nothing has been loaded.
    std::span<const MonsterSkin> ForModel(u32 modelFileDataId) const;

    /// Rows read, for status display. Zero until a successful Load.
    usize ModelCount() const noexcept {
        return byModelFile_.size();
    }
    usize DisplayCount() const noexcept {
        return skins_.size();
    }

private:
    bool loaded_ = false;
    // Flattened: one contiguous run of skins per model, so ForModel hands out
    // a span into it rather than a vector per model.
    std::vector<MonsterSkin> skins_;
    struct Range {
        u32 begin = 0;
        u32 count = 0;
    };
    std::unordered_map<u32, Range> byModelFile_;
};

} // namespace whiteout::flakes::io::wow
