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
// Only the creature slots are resolved here — the four
// CCharacterComponent::ReplaceMonsterSkin fills from
// CreatureDisplayInfo::TextureVariation, which are types 11, 12, 13 and 5 in
// that order (creature_skin_table.h has the measurement for the fourth). That
// is what leaves a creature `.m2` rendering flat white in a model viewer,
// because a viewer opens a file and never picks a display record. Character
// customisation (types 1..9) is a different set of tables and is not handled;
// those slots keep the white default they have today.
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
//     needs no configuration at all.
//
// One manager per RenderService, holding one parsed copy of the tables. The
// tables belong to the install they were read from, so pointing the provider
// at another one drops them.
// ============================================================================

#include "io/wow/creature_skin_table.h"
#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/types.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::io {
class IContentProvider;
class M2ModelAdapter;
} // namespace whiteout::flakes::io

namespace whiteout::flakes::renderer::profiles::wow {

/// One skin a creature can wear: a texture key per monster-skin slot, and a
/// name for a host offering a picker. An empty key leaves that slot white.
struct SkinVariation {
    std::string label;
    /// One per variation slot, in `io::wow::kMonsterSkinTypes` order.
    std::string texture[io::wow::kMonsterSkinSlots];
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

    void Clear() {
        table_.Clear();
        byModel_.clear();
    }

    const io::wow::CreatureSkinTable& Table() const noexcept {
        return table_;
    }

private:
    /// The fileDataID naming @p ref, which is the key both tables join on.
    /// Zero when nothing can say — a loose file with no listfile behind it.
    u32 ModelFileId(const ContentRef& ref) const;

    /// The skins for @p modelRef, from the tables when they can name it and
    /// from its `.blp` siblings when they cannot. @p slots is the sorted set of
    /// monster-skin types the model declares — a skin fills all of them, not
    /// just the first, which is what pairs a mount's body with its saddle.
    std::vector<SkinVariation> FindVariations(const ContentRef& modelRef,
                                              const std::vector<u32>& slots);

    io::IContentProvider* provider_ = nullptr;
    io::wow::CreatureSkinTable table_;
    // Keyed by ContentRef::Describe, so a host with several models open can
    // ask about any of them. Filled by Apply, which is also the only thing
    // that knows a model has a slot worth looking for at all.
    std::unordered_map<std::string, std::vector<SkinVariation>> byModel_;
    u32 variation_ = 0;
};

} // namespace whiteout::flakes::renderer::profiles::wow
