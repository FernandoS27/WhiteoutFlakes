#pragma once

// ============================================================================
// The game storages the viewer reads through, and the profile it is working in.
//
// A profile is a settings page and an ini section; nothing about it opens
// anything. The provider's active product is a separate fact, moved only by
// content that needs it (FollowModelGame). Storage opens and the client-table
// prewarms run on the task thread behind the progress UI.
// ============================================================================

#include "whiteout/flakes/enums.h"
#include "whiteout/flakes/types.h"

#include <whiteout/models/wem/profile.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <set>

namespace whiteout::flakes::io {
class FileContentProvider;
class IContentProvider;
class LoadTaskRunner;
} // namespace whiteout::flakes::io
namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes {

class StorageSession {
public:
    StorageSession(renderer::RenderService& service, io::LoadTaskRunner& tasks);

    /// The provider every document reads through: the default scene's.
    io::FileContentProvider& Provider();
    /// The same provider, aliased without ownership so every document scene
    /// shares one CASC/MPQ/install set.
    std::shared_ptr<io::IContentProvider> SharedProvider();

    // ---- Profile ----
    ProductId SettingsProfile() const {
        return settingsProfile_;
    }
    /// Picked in Settings: persisted, because the next launch should come back
    /// to it, and applied to nothing.
    void SetSettingsProfile(ProductId game);
    /// The page shown without persisting it (the `--ui-shot` harness).
    void ShowSettingsProfile(ProductId game) {
        settingsProfile_ = game;
    }
    /// Load @p game's ini settings into the provider's slot for it and serve that
    /// product. Once per product per session unless @p force: re-applying
    /// overwrites session-only state and invalidates a storage that is open and
    /// correct. `force` is for an explicit settings edit, which must reach the
    /// live storage so it reopens.
    void ApplyProfile(ProductId game, bool force);
    /// Runs when a profile's ini settings are about to be (re)applied to its
    /// slot: whatever was read from the previous install goes stale.
    void SetOnProfileApplying(std::function<void(ProductId)> fn) {
        onProfileApplying_ = std::move(fn);
    }

    /// Point the shared provider at the game a document belongs to, so its
    /// textures have a storage to resolve against. @p wemProfile decides for the
    /// interchange formats, whose suffix says nothing.
    void FollowModelGame(const std::filesystem::path& path, ::whiteout::models::wem::ProfileId wemProfile);

    // ---- Storage opens ----
    /// Open the active game's storages as a background task; a no-op when they
    /// are open. @p onDone runs on the host thread once they are up or failed.
    void OpenStoragesAsync(std::function<void(bool ok)> onDone = {});
    /// The open on its own, for any provider — the Storage Explorer has its own.
    void RunStorageOpenTask(io::FileContentProvider& provider, std::function<void(bool ok)> onDone);
    /// Whether a document open would wait: the storage is not settled, or World
    /// of Warcraft's client tables are still to read.
    bool DocumentOpenWouldWait();
    /// Everything a document needs before the host thread can open it without
    /// stalling, as ONE task so the bar covers both: the install, and for World
    /// of Warcraft the client tables the spawn reads synchronously.
    void PreloadForDocumentAsync(std::function<void()> then);

    // ---- Client-table prewarms ----
    /// World of Warcraft's client databases, so the first character of a session
    /// does not pay for fourteen CASC reads on the render thread. Restyles what
    /// is loaded when they land.
    void PrewarmWowTablesAsync();
    void SetOnWowTablesReady(std::function<void()> fn) {
        onWowTablesReady_ = std::move(fn);
    }
    /// StarCraft II / Heroes: which `.m3a` a model wants is in the GameData
    /// catalog, ~5,700 CASC reads.
    void PrewarmSc2CatalogAsync();

private:
    void AdoptNearbyWowKeys(const std::filesystem::path& modelPath);

    renderer::RenderService& service_;
    io::LoadTaskRunner& tasks_;
    std::shared_ptr<io::IContentProvider> sharedProvider_;

    /// Seeded from the ini at startup.
    ProductId settingsProfile_ = ProductId::Wc3;
    /// Products whose ini settings were loaded into their slot this session.
    std::set<ProductId> appliedProfiles_;
    std::function<void(ProductId)> onProfileApplying_;
    std::function<void()> onWowTablesReady_;

    /// The client tables have been read, or tried, for the install now
    /// configured. Not asked of the tables themselves: an install that cannot
    /// serve them leaves them un-Loaded forever, and that would re-run the
    /// prewarm on every model opened.
    bool wowTablesPrewarmed_ = false;
    bool sc2CatalogPrewarmed_ = false;
};

} // namespace whiteout::flakes
