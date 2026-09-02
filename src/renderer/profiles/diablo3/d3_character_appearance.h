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
// The engine look categories are settled now (retail 2.8 name tables:
// TRS 2, GLV 5, BTS 7, LEG 9 -- `EEquipmentSlot` ordinals), and so is the
// engine's matching rule: a case-sensitive substring search for "<CAT>" and
// "<CAT>_<VALUE>", never a parse (`ActorModel_ApplyLook`, retail 0x7544A0).
// Membership below therefore goes through `native::matchesLook` -- the same
// substrings the engine tests -- so labelling (`parseGeosetName`) and
// selection can no longer disagree on an oddly-spelled name. The API stays
// keyed on `native::LookSlot`; the category is an implementation detail of
// the matching.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <whiteout/sno/d3/native/character.h>
#include <whiteout/sno/d3/native/d3_native.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::io {
class D3ModelAdapter;
struct D3ItemRecord;
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
    std::string token;    ///< The engine's match pattern, e.g. `TRS_CLS_MED_B`
                          ///< (hair keeps its whole shape-name token).
    d3n::ArmourWeight weight = d3n::ArmourWeight::Unknown;
    char variant = 0;
    /// @brief The engine look value that selects this piece — exact, now that
    ///        `g_LookValueNames` (retail 0x14777E0) names all nineteen: the
    ///        second spare of each weight group is the `C` variant, and the
    ///        `CLS_*` family sits at 10..18. An item whose Actor carries this
    ///        value in tag 0x10400 draws exactly this piece.
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

/// One of the engine's nine visual-equipment slots, as an outfit stores it:
/// an item plus a dye. The armour slots (Torso/Feet/Hands/Legs) resolve into
/// the wardrobe selection above through `resolveEquip`; the attachment slots
/// (Head/hands/Shoulders) carry child models. -1 = nothing equipped.
struct D3OutfitSlot {
    i32 itemGbid = -1;
    i32 dyeType = 0; ///< 0 undyed, 1 hidden, 2..22 a dye_ramp row.
};

/// What a character is wearing, the engine way: one record per visual slot.
/// Kept per appearance SNO next to the wardrobe selection it drives.
struct D3Outfit {
    D3OutfitSlot slots[8]; ///< EVisualSlot 0..7; slot 8 (cosmetics) out of scope.
    bool sheathed = false;
    // Who is wearing it — only per-class/per-gender ATTACHMENT art reads
    // these (armour tags are class-blind). Supplied by the host, which knows
    // what it loaded; `playerFromAppearanceStem` is the usual derivation.
    d3n::PlayerClass cls = d3n::PlayerClass::Barbarian;
    d3n::Gender gender = d3n::Gender::Male;
};

/// One child model an equipped item wants attached: the realisable half of a
/// resolveEquip, ready for the loader to spawn. Shoulders yield two of these.
struct D3OutfitAttachment {
    i32 visualSlot = 0;
    i32 itemGbid = -1;
    i32 actorSno = -1;                ///< what to spawn; -1 = nothing
    std::string_view hardpoint;       ///< where; static storage, always valid
    i32 dyeType = 0;
    bool sheathed = false;            ///< riding a sheath hardpoint
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

    /// @name The outfit: dressing by ITEM rather than by piece
    ///
    /// `SetOutfitItem` is `ActorModel_ApplyItemLookForSlot` (retail 0x750DE0)
    /// for the four armour slots: the item's Actor supplies the two look tags
    /// through `resolveEquip`, the engine's fallback chain lands them on a
    /// wardrobe piece, and the result is written into the same per-slot
    /// selection the manual picker drives — so the two stay one mechanism and
    /// the manual picker keeps working. Attachment slots are stored but not
    /// yet realised (their child models are the next phase).
    /// @{

    /// @brief Equip @p item (with its parsed Actor) into @p slot; null
    ///        unequips. Returns false when the adapter has no wardrobe or the
    ///        slot is out of range. A head item also drives the hair cutaway:
    ///        tag 0x10404 on its RESOLVED attach model (the per-class art —
    ///        retail reads it off ActorModel_GetItemAttachModelSno's result,
    ///        and 618 of the 699 helm-family actors carry it only there)
    ///        picks the `Hair_<style>` wardrobe entry the way
    ///        ActorModel_SetHairStyle does. A style with NO matching
    ///        `Hair_*` sub-object hides them all — ApplyHairStyle
    ///        (retail 0x764070) clears bit 1 on every non-match, which is
    ///        how a BALD helm removes a monk's beard. Unequipping restores
    ///        `Hair_NKD`; a later manual hair pick still wins until the next
    ///        head equip.
    bool SetOutfitItem(const io::D3ModelAdapter& adapter, d3n::EVisualSlot slot,
                       const io::D3ItemRecord* item,
                       std::shared_ptr<const d3n::Actor> itemActor);

    /// @brief How this object loads an Actor it discovers it needs — today
    ///        only the head slot's per-class attach model, for the hair tag.
    ///        ModelLoader wires this to its D3SnoCache; without it the hair
    ///        cutaway falls back to the item Actor's own (usually absent) tag.
    void SetActorSource(std::function<std::shared_ptr<const d3n::Actor>(i32)> source) {
        actorSource_ = std::move(source);
    }

    /// @brief Who wears the outfit — steers per-class attachment art only.
    void SetOutfitBody(const io::D3ModelAdapter& adapter, d3n::PlayerClass cls,
                       d3n::Gender gender);

    /// @brief Sheathe or draw the weapons. Attachments re-resolve their
    ///        hardpoints on the next OutfitAttachments read.
    void SetOutfitSheathed(const io::D3ModelAdapter& adapter, bool sheathed);

    /// @brief The child models the outfit's attachment slots want right now,
    ///        resolved with the stored body and sheathe state. The loader
    ///        diffs this against what it has spawned.
    std::vector<D3OutfitAttachment> OutfitAttachments(const io::D3ModelAdapter& adapter) const;

    /// @brief The dye stored for @p slot. For armour, dye 1 (hidden) dresses
    ///        the slot naked the way the engine does; colours wait on the
    ///        dye_ramp phase.
    void SetOutfitDye(const io::D3ModelAdapter& adapter, d3n::EVisualSlot slot, i32 dyeType);

    /// @brief The outfit as stored (gbids and dyes; -1 = empty slot).
    D3Outfit OutfitOf(const io::D3ModelAdapter& adapter) const;
    /// @}

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
        D3Outfit outfit;
        /// Each equipped item's parsed Actor, kept so dye flips, sheathe
        /// toggles and per-class art changes re-resolve without re-reading.
        /// Parallel to `outfit.slots`; null = slot empty. Cache-owned data;
        /// the shared_ptr keeps it alive past eviction.
        std::array<std::shared_ptr<const d3n::Actor>, 8> itemActors;
        /// The item's type hash, for the traits lookup at resolve time.
        std::array<u32, 8> itemTypes{};
    };

    /// Re-derive the wardrobe selection for one armour slot from its outfit
    /// entry and the stored resolution.
    void ApplyOutfitArmour(Entry& e, const io::D3ModelAdapter& adapter, d3n::EVisualSlot slot);

    /// Re-derive the hair cutaway from the head slot's item and dye.
    void ApplyOutfitHair(Entry& e);

    /// Keyed on the *appearance* SNO and not on the file that was asked for:
    /// 594 actors name one appearance, ModelLoader shares one drawable between
    /// them, and keying on the `.acr` would give each of them a private outfit
    /// for a model they are all looking at through the same adapter.
    Entry* EntryFor(const io::D3ModelAdapter& adapter) const;

    /// Index of @p slot in the wardrobe, or -1.
    static i32 SlotIndex(const Wardrobe& w, d3n::LookSlot slot);

    /// A selection.item value meaning "draw nothing for this slot" — how a
    /// hair style with no matching sub-object hides the hair, the way
    /// ApplyHairStyle's clear-every-non-match does.
    static constexpr u32 kNoneItem = 0xFFFFFFFFu;

    mutable std::unordered_map<i32, Entry> byAppearance_;
    std::function<std::shared_ptr<const d3n::Actor>(i32)> actorSource_;
};

} // namespace whiteout::flakes::renderer::profiles::diablo3
