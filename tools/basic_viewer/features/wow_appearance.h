#pragma once

// ============================================================================
// World of Warcraft looks on the active `.m2`: a creature's skin and a
// character's customisation.
//
// Both are host decisions. A skin is a property of the display record a
// creature was spawned with, not of the model; a character model leaves its
// geosets and body texture for the player to choose. Changing either restyles
// the actor where it stands (see DocumentLoader::RestyleOrReload).
//
// Abstract so a build without `.m2` support has no implementation to link:
// MakeWowAppearance returns null there.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <memory>
#include <string>
#include <vector>

namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes {

class DocumentLoader;
class DocumentManager;

class WowAppearance {
public:
    virtual ~WowAppearance() = default;

    /// "Skin Color", "Hair Style": one entry per ChrCustomizationOption the
    /// client databases list for the model.
    struct CharacterOption {
        std::string name;
        u32 optionId = 0;
        u32 choiceCount = 0;
        u32 selected = 0;
    };

    /// The skins the active model can wear. Empty when it is not a creature.
    virtual std::vector<std::string> SkinNames() const = 0;
    virtual u32 Skin() const = 0;
    virtual void SetSkin(u32 skin) = 0;

    /// Empty when the model is not a character or the databases are out of reach.
    virtual std::vector<CharacterOption> CharacterOptions() const = 0;
    virtual void SetCharacterChoice(u32 optionId, u32 choiceIndex) = 0;

    /// Re-apply the look passes to the active document's model, when the client
    /// databases land behind a model spawned before them. The ACTIVE document
    /// only: the restyle resolves its actor through the active scene, so
    /// background tabs pick the tables up on their next restyle instead.
    virtual void RestyleActiveModel() = 0;
};

/// Null without `.m2` support.
std::unique_ptr<WowAppearance> MakeWowAppearance(renderer::RenderService& service, DocumentManager& documents,
                                                 DocumentLoader& loader);

} // namespace whiteout::flakes
