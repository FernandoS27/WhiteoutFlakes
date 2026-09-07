#pragma once

// ============================================================================
// Sc2ModelCatalog — StarCraft II and Heroes of the Storm's half of "the model
// does not name this; the game does".
//
// An `.m3` carries no path to its external animation files. The chunk table
// holds no field for one, and the two `MODL` hash words that look like they
// should be it are not: measured over the 18,409 shipped Heroes models, *zero*
// have a non-empty `m3aAnimHashes`, and the 663 `.m3a` carry only 532 distinct
// `m3aAnimHash` values between them (212 of them below 0x10000). See
// HOTS_MODEL_CATALOG_DESIGN.md §2.1 for the full measurement.
//
// What the game actually reads is the model's catalog entry, in the GameData
// XML that ships beside the assets:
//
//   <CModel id="HeroZagara" parent="HeroModelParent" Race="Zerg">
//       <Model value="Assets\Units\Heroes\Storm_Hero_Zagara_Base\...m3"/>
//       <RequiredAnims value="Assets\Units\Heroes\...RequiredAnims.m3a"/>
//       <RequiredAnims value="Assets\Portraits\...PortraitAnims.m3a"/>
//   </CModel>
//
// That matters because a hero's `.m3` is nearly animation-free on its own —
// Stand, Walk, Attack, the facial set and the portrait set are all in separate
// files — so a viewer that opens one and stops shows a model in bind pose. It
// is not a small slice of the library either: 1,331 Heroes models and 333
// StarCraft II ones name at least one animation file this way.
//
// Two things the catalog does that a naive read of it would miss:
//
//   * `parent=` is real inheritance. 110 of the 881 shipped entries that carry
//     RequiredAnims declare no <Model> of their own — they are abstract parents
//     (`HeroAnubarakCommon`) whose children supply it.
//   * `index=` REPLACES a slot rather than appending. `MuradinMarauder`
//     overrides index 0 of the three files `HeroMuradinCommon` set, so it gets
//     its own body animation and keeps the shared portrait and facial sets.
//     Appending everything would give it two Muradins.
//
// Deliberately no filename-convention fallback. `X_RequiredAnims.m3a` beside
// `X_Base.m3` would reach some of the 76 Heroes / 167 StarCraft II files the
// catalog never names, and would also attach animations to models the game
// never attaches them to — and once a sequence is merged it is indistinguishable
// from a shipped one. Where the catalog says nothing, nothing is attached.
// ============================================================================

#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/types.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace whiteout::flakes::io {
class IContentProvider;
class M3ModelAdapter;
class ProgressMonitor;
} // namespace whiteout::flakes::io

namespace whiteout::flakes::renderer::profiles::sc2_heroes {

/// @brief The `.m3` → `.m3a` map the GameData catalog spells, for one install.
///
/// Serves StarCraft II and Heroes of the Storm both: they share
/// @ref ProductId::Sc2 because they share a render profile, and they spell this
/// identically — only the mod names and the volume differ.
class Sc2ModelCatalog {
public:
    /// The catalog is read through this. A second install has its own mods and
    /// its own entries, so switching to one drops the index — but an index
    /// already built for the install the new provider points at is REUSED.
    ///
    /// That last part is what makes the Storage Explorer usable inside the
    /// viewer: the panel's thumbnails read through a provider of their own and
    /// the host's documents through the game's, so the two alternate on one
    /// ModelLoader. Keyed on the install rather than the pointer, each is built
    /// once instead of every time the user looks away from the grid.
    void SetContentProvider(io::IContentProvider* provider);

    /// @brief Read the catalog now, off the thread that draws.
    ///
    /// It is per-install and read once, but it is ~5,700 XML files and about a
    /// second and a half of CASC reads, and the alternative is paying for it
    /// inside the first model load of a session with the window already up.
    /// A model that arrives first simply shows its own animations, which is
    /// what it did before this class existed.
    ///
    /// Safe on any thread, provided the host keeps pumping the provider (see
    /// io/load_task.h). Returns whether an index ended up built.
    bool Prewarm(io::ProgressMonitor* progress = nullptr);

    /// @brief Attach every `.m3a` the catalog names for @p modelRef.
    ///
    /// Reads nothing for a model the catalog does not name, which is most of
    /// them. Builds the index on first use if @ref Prewarm has not run, so a
    /// host that never prewarms still gets the right answer — just on this
    /// thread.
    ///
    /// @return How many files are now attached (0 when the model names none, or
    ///         when they were already attached — @ref AttachAnimationFile
    ///         rejects a repeat by label).
    usize Apply(io::M3ModelAdapter& adapter, const ContentRef& modelRef);

    /// @brief The animation files @p modelRef names, in catalog order, for a
    ///        host that wants to show or undo what Apply did.
    ///
    /// Storage-relative `assets/...` paths, readable through the provider as
    /// they stand (CascSource retries a relative asset path under every mod
    /// root). Empty for a model the catalog does not name — and for any model
    /// at all until the index is built, since this one never builds it.
    const std::vector<std::string>& AnimationsFor(const ContentRef& modelRef) const;

    /// Whether the index has been built. False is a normal state: a Warcraft III
    /// install has no catalog to read, and so is an install whose GameData is
    /// missing.
    bool Loaded() const noexcept {
        return loaded_;
    }

    /// How many models the index names, for a host reporting what it read.
    usize ModelCount() const noexcept {
        return byModel_.size();
    }

    void Clear();

private:
    /// Build @ref byModel_ from the provider's GameData. Idempotent; does
    /// nothing once @ref loaded_ is set.
    bool Build(io::ProgressMonitor* progress);

    /// Keyed on the `assets/...` tail of the model path, lowercased with `/`
    /// separators. The catalog names models mod-relative and a browsed file
    /// arrives as a full storage path, and this is the form both reduce to.
    /// Plus one entry per unambiguous basename, for a flat extraction with no
    /// mod path to key on.
    using Index = std::unordered_map<std::string, std::vector<std::string>>;

    io::IContentProvider* provider_ = nullptr;
    /// Which install @ref byModel_ was built for — the provider's install
    /// paths and product, which is what decides the answer. Empty for a
    /// provider that cannot say, and then nothing is cached.
    std::string installKey_;
    Index byModel_;
    bool loaded_ = false;
    /// Indexes already built, newest first. Small and fixed: a session reaches
    /// two of these (the host's provider and the Storage Explorer's), and the
    /// cap is what stops a host that repoints providers from growing it without
    /// bound. Shared so adopting one is a pointer copy.
    std::vector<std::pair<std::string, std::shared_ptr<const Index>>> cache_;
};

} // namespace whiteout::flakes::renderer::profiles::sc2_heroes
