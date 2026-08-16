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

class WowCharacterAppearance {
public:
    /// The tables belong to the install they were read from, so pointing the
    /// provider somewhere else drops them.
    void SetContentProvider(io::IContentProvider* provider);

    /// Dress @p adapter, which was parsed from @p modelRef. Returns true when
    /// the model is a character model — whether or not the databases resolved,
    /// because a character model with no tables in reach still needs its
    /// geosets culled down to one per group.
    ///
    /// Called before Build(): GetTextures and GetMeshes both read what this
    /// sets.
    bool Apply(io::M2ModelAdapter& adapter, const ContentRef& modelRef);

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
