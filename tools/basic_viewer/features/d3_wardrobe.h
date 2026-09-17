#pragma once

// ============================================================================
// What a Diablo III player character on screen is wearing, and its ragdoll.
//
// A `.m2` character leaves its geosets and body sheet blank for the game to
// fill; a `.app` ships every armour variant at once for the game to pick
// between. So this offers pieces out of a wardrobe rather than choices out of a
// database — by look (the manual wardrobe), and by in-game item through the
// GameBalance item registry (the outfit). Every change restyles the actor where
// it stands.
//
// Addressed through the focus actor rather than through a path: an outfit is
// keyed on the *appearance* — 594 actors name one `.app` and ModelLoader hands
// them one shared drawable, so the file asked for is not what is being dressed.
//
// Abstract so a build without `.acr` support has no implementation to link:
// MakeD3Wardrobe returns null there.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout::flakes::io {
class LoadTaskRunner;
}
namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes {

class DocumentLoader;
class DocumentManager;

class D3Wardrobe {
public:
    virtual ~D3Wardrobe() = default;

    // ---- The manual wardrobe: looks the appearance carries ----
    struct CharacterSlot {
        std::string name;               ///< "Torso", "Legs", "Boots", "Gloves", "Hair".
        i32 slot = 0;                   ///< native::LookSlot, opaque to the UI.
        std::vector<std::string> items; ///< "Naked", "Heavy A", "Medium B (CLS)".
        u32 selectedItem = 0;
        u32 lookIndex = 0; ///< Index into LookNames().
        /// The slots the item registry dresses (everything but Hair). With the
        /// registry live these rows restate the outfit rows and the UI hides them.
        bool registryDriven = false;
    };
    /// Empty when the focus actor is not a player character.
    virtual std::vector<CharacterSlot> CharacterSlots() const = 0;
    virtual void SetCharacterItem(i32 slot, u32 itemIndex) = 0;
    virtual void SetCharacterSlotLook(i32 slot, u32 lookIndex) = 0;
    /// The material sets the appearance ships — "A", "Unique25", "A_skeleton".
    /// An equipped item names one through tag 0x10401, so one on every slot at
    /// once is what wearing an armour *set* means.
    virtual std::vector<std::string> LookNames() const = 0;
    virtual void SetCharacterLookForAll(u32 lookIndex) = 0;

    /// Sub-objects no equipment slot claims: death bodies, skill meshes, the
    /// merged `oneBatch` LOD. Hidden unless asked for.
    struct CharacterExtra {
        std::string name;
        u32 geoset = 0;
        bool shown = false;
    };
    virtual std::vector<CharacterExtra> CharacterExtras() const = 0;
    virtual void SetCharacterExtra(u32 geoset, bool shown) = 0;

    // ---- The item registry ----
    struct RegistryState {
        enum class Stage : u8 { NotStarted, Building, Ready };
        Stage stage = Stage::NotStarted;
        /// Ready, but the storage ships no GameBalance tables.
        bool empty = false;
        /// While Building, and only once the runner is on this build rather
        /// than on whatever it is queued behind: its step and counts.
        bool progressKnown = false;
        std::string step;
        u64 current = 0;
        u64 total = 0;
        f32 fraction = 0.0f;
        bool indeterminate = true;
    };
    /// Start building the registry (~3.4k Actor reads, once per storage) as a
    /// background task, unless it is built or building. Not cancellable: the
    /// batched build is seconds, and a cancelled half-registry would need a
    /// retry state machine the popup has no honest way to draw.
    virtual void EnsureItemRegistry() = 0;
    virtual RegistryState ItemRegistry() const = 0;
    /// The Diablo III profile's install settings are being re-applied: the
    /// registry belongs to the old install. Never cleared mid-build — the
    /// registry is the task thread's then; the next apply clears it.
    virtual void OnProfileApplying() = 0;

    // ---- The outfit: dressing by in-game item ----
    struct OutfitRow {
        std::string name;          ///< "Torso", "Right hand", ...
        i32 visualSlot = 0;        ///< native::EVisualSlot, opaque to the UI.
        std::string equipped;      ///< The item's record stem; empty when nothing is.
        std::string equippedLabel; ///< Its display name; the stem when unnamed.
        i32 dye = 0;
        bool armour = false; ///< The four rows that dress the body model.
    };
    /// Empty unless the focus actor is a player character and the registry is
    /// Ready with items.
    virtual std::vector<OutfitRow> OutfitSlots() const = 0;

    /// One picker row: the display name to show and the record stem that equips
    /// it. The stem is the key everywhere that persists; the label is only shown.
    struct ItemEntry {
        std::string label;
        std::string stem;
    };
    /// Up to @p max items for @p visualSlot whose display name or stem contains
    /// @p filter (case-insensitive), sorted by label. Restricted to what the
    /// focused character's class can wear — class-neutral items always pass —
    /// unless @p allClasses. One display name is one row: the art-test dupes ship
    /// a name on many records, and a group's survivor is the equipped record when
    /// one is, else the first stem.
    virtual std::vector<ItemEntry> ItemEntries(i32 visualSlot, std::string_view filter, usize max,
                                               bool allClasses) const = 0;

    struct SetEntry {
        std::string label; ///< "Firebird's Finery".
        std::string key;   ///< The ItemSets.stl key — the stable identity.
        usize pieces = 0;  ///< Wearable pieces (rings and amulets excluded).
    };
    /// The item sets whose pieces the focused class can wear, same rules.
    virtual std::vector<SetEntry> SetEntries(std::string_view filter, bool allClasses) const = 0;
    /// Every wearable piece of set @p key into its slot; weapons fill right hand
    /// then left, other slots keep what they wore. False when no set has the key.
    virtual bool EquipSet(std::string_view key) = 0;
    /// Equip @p itemName (empty unequips). False when the name is unknown.
    virtual bool SetOutfitItem(i32 visualSlot, std::string_view itemName) = 0;
    virtual void SetOutfitDye(i32 visualSlot, i32 dye) = 0;
    /// Weapons in the sheath hardpoints rather than the hands.
    virtual bool OutfitSheathed() const = 0;
    virtual void SetOutfitSheathed(bool sheathed) = 0;
    /// The picker's tooltip: stem, type, set, class lock, gbid and actor SNO.
    /// Empty for an unknown name.
    virtual std::string ItemTip(std::string_view itemName) const = 0;

    /// Whole outfits by name in `d3_outfits.ini` beside the viewer settings.
    virtual std::vector<std::string> PresetNames() const = 0;
    virtual bool SavePreset(std::string_view name) = 0;
    /// Unknown item names are reported and skipped. False when nothing equipped.
    virtual bool LoadPreset(std::string_view name) = 0;

    // ---- Ragdoll ----
    //
    // D3 has no model-level "collapse now": the client builds the rig on a
    // gameplay event and stops the actor animating in the same call. Nothing in
    // a model file carries that event, so the host supplies it.
    /// Whether the focus actor has a rig that could collapse at all.
    virtual bool HasRagdoll() const = 0;
    virtual bool Ragdoll() const = 0;
    virtual void SetRagdoll(bool on) = 0;
};

/// Null without `.acr` support.
std::unique_ptr<D3Wardrobe> MakeD3Wardrobe(renderer::RenderService& service, io::LoadTaskRunner& tasks,
                                           DocumentManager& documents, DocumentLoader& loader);

} // namespace whiteout::flakes
