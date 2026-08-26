#pragma once

// ============================================================================
// WowReplaceableTextures — World of Warcraft's half of "the model does not
// name this texture; the game does".
//
// Warcraft III spells that as `ReplaceableId N` on a layer, and
// assets::ReplaceableTextureManager answers it with a team-colour swatch, a
// synthesized team glow, or one canonical file per id. World of Warcraft
// spells the same idea as `M2Texture::type`, and answers it from the client
// databases: a non-zero type is a slot, and which file fills it depends on
// what the model was spawned *as*, not on the model.
//
// Two families of slot are resolved here, because two families of model leave
// one blank:
//
//   * A creature's, the four CCharacterComponent::ReplaceMonsterSkin fills
//     from CreatureDisplayInfo::TextureVariation — types 11, 12, 13 and 5
//     in that order (creature_skin_table.h has the measurement for the
//     fourth).
//   * An item's, the ones ItemDisplayInfoModelMatRes names — type 2 for an
//     object's own skin, 3 and 4 for a weapon's blade and handle, and 24
//     (item_appearance_table.h has the population).
//
// Either is what leaves a `.m2` rendering flat white in a model viewer,
// because a viewer opens a file and never picks a display record. A model
// belongs to one family or the other, so only one set of tables is ever read.
//
// Character customisation (types 1, 6, 8, 19) is a different set of tables and
// is not handled here: those slots are not filled with a file at all but
// composited from many. See chr_customization_table.h.
//
// Two ways to find the skins, because a viewer opens models from both kinds of
// place:
//
//   * The client databases, when the storage can name the model — a CASC root
//     read by fileDataID, or a loose tree with a listfile behind it. This is
//     the client's own answer, in the client's own order.
//   * The model's own directory otherwise. ReplaceMonsterSkin builds the skin
//     path by truncating the model path to its folder and appending the
//     variation name, so a creature's skins are always its `.blp` siblings —
//     which is the whole answer for a loose extraction with no listfile, and
//     needs no configuration at all. Items are laid out the same way — a
//     shield's four looks are four `.blp`s beside it — so the fallback
//     serves both.
//
// One manager per RenderService, holding one parsed copy of the tables. The
// tables belong to the install they were read from, so pointing the provider
// at another one drops them.
// ============================================================================

#include "io/wow/creature_skin_table.h"
#include "io/wow/item_appearance_table.h"
#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/types.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::io {
class IContentProvider;
class M2ModelAdapter;
class ProgressMonitor;
} // namespace whiteout::flakes::io

namespace whiteout::flakes::renderer::profiles::wow {

/// One look a model can wear: a texture key per replaceable slot, and a name
/// for a host offering a picker. An empty key leaves that slot white.
struct SkinVariation {
    std::string label;
    /// One per slot, in `io::wow::kReplaceableTypes` order — whose first
    /// four entries are the creature ones, so a creature look still indexes
    /// 0..3.
    std::string texture[io::wow::kReplaceableSlots];
};

class WowReplaceableTextures {
public:
    /// The tables are read through this, and dropped when it changes — a
    /// second install has its own fileDataIDs and its own display rows.
    void SetContentProvider(io::IContentProvider* provider);

    /// Which variation a creature wears when it is spawned. Out-of-range wraps
    /// per model, so a host can step the value without knowing how many a
    /// model has.
    ///
    /// Read at spawn time. `.m2` actors are built from a freshly parsed model
    /// rather than from the template cache, so changing this and re-spawning is
    /// enough to see another skin.
    void SetVariation(u32 variation) {
        variation_ = variation;
    }
    u32 Variation() const noexcept {
        return variation_;
    }

    /// Bind @p adapter's replaceable slots for the model named by @p modelRef.
    /// Returns how many of its textures now have a file behind them.
    ///
    /// Does nothing — and reads nothing — for a model with no replaceable
    /// slot, which is most of them.
    usize Apply(io::M2ModelAdapter& adapter, const ContentRef& modelRef);

    /// The skins @p modelRef can wear, in pick order, for a host offering a
    /// picker. Answered from what Apply already found — a host with several
    /// models open asks about whichever one it is showing, and a model that
    /// has not been through Apply, or has no replaceable slot, has none.
    const std::vector<SkinVariation>& Variations(const ContentRef& modelRef) const;

    /// Read the client tables now, off the thread that draws.
    ///
    /// They are per-install and read once, but they are read from inside a
    /// model load — so the first World of Warcraft model of a session used to
    /// pay for fourteen CASC reads and a third of a million rows on the render
    /// thread, with the window already up. Running this as a background task
    /// instead means a model that arrives first simply shows its default look;
    /// re-applying it afterwards is what ModelLoader::RestyleWowModel is for.
    ///
    /// Safe on any thread, provided the host keeps pumping the provider (see
    /// io/load_task.h). Returns whether the tables ended up loaded.
    bool Prewarm(io::ProgressMonitor* progress = nullptr);

    void Clear() {
        table_.Clear();
        items_.Clear();
        byModel_.clear();
    }

    const io::wow::CreatureSkinTable& Table() const noexcept {
        return table_;
    }
    const io::wow::ItemAppearanceTable& ItemTable() const noexcept {
        return items_;
    }

private:
    /// The fileDataID naming @p ref, which is the key every one of these
    /// tables joins on. Zero when nothing can say — a loose file with no
    /// listfile behind it.
    u32 ModelFileId(const ContentRef& ref) const;

    /// The looks for @p modelRef, from the tables when they can name it and
    /// from its `.blp` siblings when they cannot. @p slots is the sorted set of
    /// replaceable types the model declares — which decides both which family
    /// of table to read and what a look has to fill, since it fills all of
    /// them and not just the first. That is what pairs a mount's body with its
    /// saddle, and a weapon's blade with its handle.
    std::vector<SkinVariation> FindVariations(const ContentRef& modelRef,
                                              const std::vector<u32>& slots);

    io::IContentProvider* provider_ = nullptr;
    io::wow::CreatureSkinTable table_;
    io::wow::ItemAppearanceTable items_;
    // Keyed by ContentRef::Describe, so a host with several models open can
    // ask about any of them. Filled by Apply, which is also the only thing
    // that knows a model has a slot worth looking for at all.
    std::unordered_map<std::string, std::vector<SkinVariation>> byModel_;
    u32 variation_ = 0;
};

} // namespace whiteout::flakes::renderer::profiles::wow
