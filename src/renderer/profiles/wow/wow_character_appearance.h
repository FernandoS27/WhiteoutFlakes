#pragma once

// ============================================================================
// WowCharacterAppearance — the spawn-time half of character support.
//
// WowReplaceableTextures answers "what texture does this creature wear". This
// answers the two questions a *character* model asks instead, both of which the
// client answers from CCharacterComponent and neither of which a file can
// answer alone:
//
//   which of its 113 submeshes draw   → M2ModelAdapter::SetVisibleGeosets
//   what its blank texture slots hold → M2ModelAdapter::SetComposedTextures
//
// The databases are read once per install, lazily, off the first character
// model spawned — a session that only ever opens creatures never touches them.
//
// The chosen appearance is kept per model rather than per actor, so a host can
// step "Hair Style" and re-Apply to see the next one, exactly as
// WowReplaceableTextures::SetVariation works for creature skins. Re-applying
// need not mean re-spawning — see ModelLoader::RestyleWowModel.
// ============================================================================

#include "io/wow/character_appearance.h"
#include "io/wow/chr_customization_table.h"
#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/types.h"

#include <whiteout/models/m2/m2.h>

#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::io {
class IContentProvider;
class M2ModelAdapter;
} // namespace whiteout::flakes::io

namespace whiteout::flakes::renderer::profiles::wow {

/// What a host needs to offer a customisation picker for one model.
struct CharacterOptionView {
    std::string name;
    u32 optionId = 0;
    u32 choiceCount = 0;
    u32 selected = 0; ///< Index into the option's choice list.
};

/// Pair @p child's bones to @p parent's by key bone id — the only thing the two
/// rigs agree on, since a collections model carries thirteen bones where the
/// character carries hundreds and nothing lines up by index.
///
/// Returns one entry per @p child bone: the parent bone driving it, or -1 for
/// one no key bone pairs (an intermediate link, which then composes onto its
/// paired ancestor as usual). What `Actor::skinnedParentBone` holds.
std::vector<i32> PairBonesByKeyBone(const ::whiteout::m2::Model& parent,
                                    const ::whiteout::m2::Model& child);

class WowCharacterAppearance {
public:
    /// The tables belong to the install they were read from, so pointing the
    /// provider somewhere else drops them.
    void SetContentProvider(io::IContentProvider* provider);

    /// A second model the chosen appearance puts on the character, and which of
    /// its geosets to draw. Several entries can name one file — that is one
    /// model wearing two of its parts, not two models.
    struct SkinnedModel {
        u32 fileId = 0;
        std::vector<u16> geosets;
    };

    /// Dress @p adapter, which was parsed from @p modelRef. Returns true when
    /// the model is a character model — whether or not the databases resolved,
    /// because a character model with no tables in reach still needs its
    /// geosets culled down to one per group.
    ///
    /// @p outSkinned, when given, receives the collections models this
    /// appearance calls for — a Dracthyr's horns are geosets of a *different*
    /// `.m2`, and the caller is the one that can spawn it. Empty for every
    /// model that asks for none, which is most of them.
    ///
    /// Called before Build(): GetTextures and GetMeshes both read what this
    /// sets.
    bool Apply(io::M2ModelAdapter& adapter, const ContentRef& modelRef,
               std::vector<SkinnedModel>* outSkinned = nullptr);

    /// The options @p modelRef offers, in the client's own order. Empty for a
    /// model that has not been through Apply, is not a character, or resolved
    /// no ChrModel.
    std::vector<CharacterOptionView> Options(const ContentRef& modelRef) const;

    /// Choose @p choiceIndex for @p optionId on @p modelRef. Takes effect on
    /// the next Apply — the next spawn, or sooner if the host re-dresses a live
    /// actor through ModelLoader::RestyleWowModel.
    void SetChoice(const ContentRef& modelRef, u32 optionId, u32 choiceIndex);

    void Clear() {
        tables_.Clear();
        byModel_.clear();
    }

    const io::wow::ChrCustomizationTable& Tables() const noexcept {
        return tables_;
    }

private:
    /// The chosen choice index per option, for one model.
    struct Selection {
        u32 chrModelId = 0;
        std::vector<u32> choiceIndex; ///< Parallel to ChrModelInfo::options.
    };

    /// The fileDataID naming @p ref — the key every table joins on. Zero when
    /// nothing can say, which is a loose extraction with no listfile behind it.
    u32 ModelFileId(const ContentRef& ref) const;

    Selection& SelectionFor(const ContentRef& ref, const io::wow::ChrModelInfo& model);

    io::IContentProvider* provider_ = nullptr;
    io::wow::ChrCustomizationTable tables_;
    // Keyed by ContentRef::Describe, like WowReplaceableTextures::byModel_, so
    // a host with several documents open can ask about any of them.
    std::unordered_map<std::string, Selection> byModel_;
};

} // namespace whiteout::flakes::renderer::profiles::wow
