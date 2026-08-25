#pragma once

// ============================================================================
// D3CharacterAppearance — dressing a Diablo III player character.
//
// The Diablo III sibling of WowCharacterAppearance, and the same shape: choices
// in, a dressed adapter out, the choice kept per model so a host can step an
// option and re-apply without re-spawning.
//
// ---------------------------------------------------------------------------
// The one fact that organises all of this
//
// A player Appearance already contains every armour variant as sub-objects.
// Equipping an item loads no geometry and no material asset: it resolves to two
// small integers on the item's Actor tag map, and those flip visibility bits on
// sub-objects that were there all along.
//
//     look VALUE (tag 0x10400) + the slot's category  ->  which MESH draws
//     look NAME  (tag 0x10401) -> an index into arLooks  ->  which MATERIAL
//
// See `whiteout/sno/d3/native/character.h`, which holds the rules; this file
// holds the renderer's use of them. `ActorModel_ApplyLook` (0x7100222000) is
// the pass being reproduced.
//
// So a D3 character is *undressable* rather than incomplete. Nothing here fills
// a blank the way WoW's composite textures do — there are no blanks. It picks,
// out of a wardrobe the file already ships, which pieces draw.
//
// ---------------------------------------------------------------------------
// Why this is not optional
//
// Without it every player Appearance draws its whole wardrobe at once: the
// Barbarian's baseline trace is 30 sub-objects per frame, which is naked plus
// light plus medium plus heavy plus the decapitated, dismembered and skeletal
// bodies, all interpenetrating. A dressed character is the correct picture and
// the current one is the accident.
//
// ---------------------------------------------------------------------------
// Where the wardrobe comes from, and what is still open
//
// 2.6.2 packs a sub-object's geoset descriptor into a dword — bit 1 takes part
// in look switching, byte 2 is the look category, byte 3 the look value — but
// the shipped v260 files carry `szName[128]` at that offset instead and say the
// same thing in the Maya shape name (`N_TRS_HVY_AShape_Barb_F_HVY_mat_001`).
// `native::parseGeosetName` recovers slot / weight / variant from it, over 436
// of the 441 sub-objects the fourteen player Appearances hold.
//
// What is NOT settled, and is deliberately not guessed at here: which engine
// look *category* each of torso / legs / boots / gloves is.
// `g_EquipSlotToLookCategory` holds four (visual slot, category) pairs and
// nothing yet pins a body part to one. It does not matter for this: a category
// only says which slot a value applies to, and the slot is what the shape name
// already states. So the selection is keyed on `native::LookSlot` and the
// engine ordinals stay unclaimed rather than being invented.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <whiteout/sno/d3/native/character.h>
#include <whiteout/sno/d3/native/d3_native.h>

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::io {
class D3ModelAdapter;
} // namespace whiteout::flakes::io

namespace whiteout::flakes::renderer::profiles::diablo3 {

namespace d3n = ::whiteout::sno::d3::native;

/// One wearable piece: everything that draws when this armour weight is worn
/// in this slot.
///
/// Several sub-objects, usually: a `_Cloth` companion is part of the same piece
/// rather than a piece of its own, and grouping them is what keeps a skirt from
/// being a separate wardrobe entry from the legs it hangs off.
struct D3WardrobeItem {
    std::string label;    ///< "Naked", "Heavy A", "Medium B (CLS)".
    std::string token;    ///< The shape-name token, e.g. `N_TRS_CLS_MED_B`.
    d3n::ArmourWeight weight = d3n::ArmourWeight::Unknown;
    char variant = 0;
    /// @brief The engine look value that selects this piece, where the value
    ///        space determines it.
    ///
    /// Only the weight's base value is a recovered fact (Naked 0, Light 1,
    /// Medium 3, Heavy 5). Which of a group's two spare values a `B` or a `C`
    /// takes is not: the fallback is symmetric, so the corpus cannot tell 2
    /// from 7. A non-base variant therefore reports its group's base — the
    /// value it degrades to — rather than a number nothing supports.
    u32 lookValue = 0;
    std::vector<u32> geosets; ///< Emitted geoset ids, ascending.
};

/// One equipment slot and the pieces it offers.
struct D3WardrobeSlot {
    d3n::LookSlot slot = d3n::LookSlot::Unknown;
    std::string name; ///< "Torso", "Legs", "Boots", "Gloves", "Hair".
    std::vector<D3WardrobeItem> items;
    u32 selectedItem = 0;
    /// @brief Which look — which material set — this slot wears.
    ///
    /// Per slot and not per model, because the original reads a look name off
    /// each equipped item. One material serves a whole weight class, so a heavy
    /// chest and heavy boots from two different sets are one material read at
    /// two variant indices.
    u32 lookIndex = 0;
};

/// A sub-object no equipment slot claims: a death body, a skill mesh, the
/// merged `oneBatch` LOD.
///
/// Hidden by default and offered one by one. These are not switched by
/// ActorModel_ApplyLook at all — gameplay decides when a decapitated body
/// replaces a live one — so there is no rule to reproduce, only a choice to
/// expose.
struct D3WardrobeExtra {
    std::string name;
    u32 geoset = 0;
    bool shown = false;
};

class D3CharacterAppearance {
public:
    /// @brief Is @p adapter a player character — does it ship a wardrobe?
    ///
    /// Measured, not name-matched: an appearance qualifies when at least two of
    /// the four armour slots offer at least two weights. A creature with one
    /// body has neither, and matching on `playerAppearanceStem` would miss
    /// every `_characterSelect` and `_FrontEnd` variant of the same rig.
    bool IsCharacter(const io::D3ModelAdapter& adapter) const;

    /// @brief Dress @p adapter with its current selection.
    ///
    /// Returns false — and touches nothing — for a model with no wardrobe, so
    /// a creature keeps the "everything draws" default rather than being
    /// stripped to nothing by a rule that does not apply to it.
    ///
    /// Called before the surface table is built: the table resolves each
    /// geoset's material at the look this sets for it.
    bool Apply(io::D3ModelAdapter& adapter);

    /// @brief The slots @p adapter offers, in torso / legs / boots / gloves /
    ///        hair order, with the current selection filled in. Empty for a
    ///        model that is not a character.
    std::vector<D3WardrobeSlot> Slots(const io::D3ModelAdapter& adapter) const;

    /// @brief The sub-objects no slot claims, with their current state.
    std::vector<D3WardrobeExtra> Extras(const io::D3ModelAdapter& adapter) const;

    /// @brief Choose a piece for one slot. Takes effect on the next Apply.
    void SetItem(const io::D3ModelAdapter& adapter, d3n::LookSlot slot, u32 itemIndex);

    /// @brief Choose the material set one slot wears.
    void SetSlotLook(const io::D3ModelAdapter& adapter, d3n::LookSlot slot, u32 lookIndex);

    /// @brief Put every slot on one look — an armour *set* rather than a piece.
    void SetLookForAll(const io::D3ModelAdapter& adapter, u32 lookIndex);

    /// @brief Show or hide one unclaimed sub-object.
    void SetExtra(const io::D3ModelAdapter& adapter, u32 geoset, bool shown);

    void Clear() {
        byAppearance_.clear();
    }

private:
    /// What one Appearance offers, derived once. Immutable: it is a reading of
    /// the file, and the file does not change under us.
    struct Wardrobe {
        bool isCharacter = false;
        std::vector<D3WardrobeSlot> slots; ///< `selectedItem` / `lookIndex` unused here.
        std::vector<D3WardrobeExtra> extras;
    };

    /// What the host has chosen for it.
    struct Selection {
        std::vector<u32> item;  ///< Parallel to Wardrobe::slots.
        std::vector<u32> look;  ///< Parallel to Wardrobe::slots.
        std::vector<u8> extra;  ///< Parallel to Wardrobe::extras.
    };

    struct Entry {
        Wardrobe wardrobe;
        Selection selection;
    };

    /// Keyed on the *appearance* SNO and not on the file that was asked for:
    /// 594 actors name one appearance, ModelLoader shares one drawable between
    /// them, and keying on the `.acr` would give each of them a private outfit
    /// for a model they are all looking at through the same adapter.
    Entry* EntryFor(const io::D3ModelAdapter& adapter) const;

    /// Index of @p slot in the wardrobe, or -1.
    static i32 SlotIndex(const Wardrobe& w, d3n::LookSlot slot);

    mutable std::unordered_map<i32, Entry> byAppearance_;
};

} // namespace whiteout::flakes::renderer::profiles::diablo3
