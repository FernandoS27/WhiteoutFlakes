#pragma once

// ============================================================================
// The `M2Texture::type` values a World of Warcraft client database fills, and
// the slot each one occupies in a resolved look.
//
// A WoW model leaves a texture blank by giving it a non-zero `type` and a zero
// TXID: the file that belongs there is a property of what the model was spawned
// *as*, not of the model, so a viewer that opens the file and nothing else
// binds the white default. Two different tables answer, for two different kinds
// of model — CreatureDisplayInfo for a creature, ItemDisplayInfo for a weapon
// or a piece of armour — and both hand back the same shape: one texture per
// slot.
//
// One order for both, so a look is one array whichever table filled it:
//
//   slot 0..3   CreatureDisplayInfo::TextureVariation[0..3], in its own order.
//               Types 11, 12, 13 and *5* — not contiguous; see
//               creature_skin_table.h for where the fourth was measured.
//   slot 4..7   The types ItemDisplayInfoModelMatRes tags a material with:
//               2 (the object's own skin), 3 and 4 (a weapon's blade and
//               handle), 24. Measured across the shipped table — see
//               item_appearance_table.h.
//
// Slot 3 belongs to both: type 5 is the creature array's fourth entry and also
// appears in 72 of the item table's 141309 rows. A model is one kind or the
// other, so the two never write it in the same look.
//
// Character customisation (types 1, 6, 8, 19) is not here. Those slots are not
// filled with a file at all — they are composited at load time from many, which
// is a different answer of a different shape. See chr_customization_table.h.
// ============================================================================

#include "whiteout/flakes/types.h"

namespace whiteout::flakes::io::wow {

inline constexpr u32 kReplaceableSlots = 8;
inline constexpr u32 kReplaceableTypes[kReplaceableSlots] = {11, 12, 13, 5, 2, 3, 4, 24};

/// The highest type any slot fills, so a by-type array can be sized once.
inline constexpr u32 kMaxReplaceableType = 24;

/// Which slot fills texture type @p type, or -1 for a type no database touches.
constexpr i32 ReplaceableSlotOfType(u32 type) {
    for (u32 slot = 0; slot < kReplaceableSlots; ++slot)
        if (kReplaceableTypes[slot] == type)
            return static_cast<i32>(slot);
    return -1;
}

} // namespace whiteout::flakes::io::wow
