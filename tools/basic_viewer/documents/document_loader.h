#pragma once

// ============================================================================
// Opening files as documents: models, standalone effects, WEM/glTF
// interchange documents and paths inside a storage the Storage Explorer browses.
//
// Each open is a new tab on its own scene. The render mode is decided before
// the spawn (every SLK, splat and shader-path cache the spawn primes reads it),
// the provider follows the game the file belongs to, and a load that would wait
// on an install goes behind the progress modal first.
// ============================================================================

#include "whiteout/flakes/enums.h"
#include "whiteout/flakes/types.h"

#include <whiteout/models/wem/profile.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace whiteout::flakes::io {
class IContentProvider;
struct WemDocument;
} // namespace whiteout::flakes::io
namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes {

class DocumentManager;
class PlaybackController;
class StorageSession;
struct Document;

class DocumentLoader {
public:
    DocumentLoader(renderer::RenderService& service, DocumentManager& documents,
                   PlaybackController& playback, StorageSession& session);

    /// Synchronous open into a new tab: what the command line and the gates use,
    /// since their work follows immediately. `.pkb`/`.pkfx` go to LoadEffect, and
    /// a `.wem` opens at its own default profile.
    bool LoadModel(const std::filesystem::path& path);
    /// File > Open: LoadModel when nothing needs waiting for, otherwise the
    /// storage open and client tables first, behind the modal. False only for a
    /// path that cannot be opened at all.
    bool OpenModelAsync(const std::filesystem::path& path);
    bool LoadEffect(const std::filesystem::path& path);

    /// The startup picker runs before any frame exists, so its paths wait for the
    /// frame loop and open one per frame behind their own bar.
    void QueueInitialOpen(const std::filesystem::path& path);
    /// The next queued path, when nothing else is loading.
    void OpenNextQueued(bool tasksBusy);

    // ---- WEM interchange ----
    /// Parse @p path far enough to ask what profiles it offers. Null when it is
    /// not a WEM or glTF file. Hand the result to OpenWemAs to read it once.
    std::shared_ptr<io::WemDocument> PeekWemDocument(const std::filesystem::path& path);
    /// `--wem-profile`: the profile a `.wem` opens as when nobody picks one.
    void SetPreferredWemProfile(::whiteout::models::wem::ProfileId profile) {
        preferredWemProfile_ = profile;
    }
    /// Open @p document as @p profile; `Count` means the preferred profile, and
    /// failing that the document's own default.
    bool OpenWemAs(const std::filesystem::path& path, std::shared_ptr<io::WemDocument> document,
                   ::whiteout::models::wem::ProfileId profile);

    /// A path inside @p provider's storage (CASC `:` / `\` separators), as the
    /// Storage Explorer hands it over. Waits behind a bar when that storage is
    /// still to open.
    bool OpenStorageDocument(const std::string& archivePath, bool effect,
                             std::shared_ptr<io::IContentProvider> provider);

    // ---- Render mode ----
    /// "Reforged Graphics": HD for every model, whatever it would be detected as.
    /// Reloads the active document so it re-resolves under the new overlay.
    bool ForceHd() const {
        return forceHd_;
    }
    void SetForceHd(bool on);
    /// The Warcraft III art overlay reads resolve through; empty follows the
    /// render mode. Reloads the active document for the same reason.
    void SetArtTier(std::optional<Wc3ArtTier> tier);
    /// Point the active scene and the shared settings at @p wanted, running the
    /// splat and event-data hand-over only when the mode actually changes.
    void ApplyRenderMode(RenderMode wanted);

    /// Reload the active document's model in place — the fallback when a
    /// restyle cannot be done where the actor stands.
    bool ReloadActiveModel();
    /// Re-dress the active document's focus actor where it stands, and reload
    /// only when @p restyle says it cannot: a reload throws away the pose and
    /// the camera framing, and neither is a function of what the model wears.
    void RestyleOrReload(const std::function<bool(u32 focusActor)>& restyle);

private:
    bool OpenDocument(const std::filesystem::path& path, bool effect);
    bool OpenStorageDocumentNow(const std::string& archivePath, bool effect,
                                std::shared_ptr<io::IContentProvider> provider);
    bool LoadModelInto(Document& doc, const std::filesystem::path& path);
    bool LoadEffectInto(Document& doc, const std::filesystem::path& path);
    /// Whether a Warcraft III model has any HD layer, from a parse of the file.
    RenderMode ProbeWc3RenderMode(const std::filesystem::path& path) const;
    /// The same, for a model inside a storage.
    RenderMode ProbeWc3RenderMode(io::IContentProvider& provider, const std::string& archivePath) const;

    renderer::RenderService& service_;
    DocumentManager& documents_;
    PlaybackController& playback_;
    StorageSession& session_;

    bool forceHd_ = false;
    std::vector<std::filesystem::path> pendingInitialOpens_;
    /// The WEM document the next open spawns and the profile it was picked at —
    /// set before the game follow runs, because the profile is what tells it
    /// which game's storage to point at.
    std::shared_ptr<io::WemDocument> pendingWemDocument_;
    ::whiteout::models::wem::ProfileId pendingWemProfile_ = ::whiteout::models::wem::ProfileId::Count;
    ::whiteout::models::wem::ProfileId preferredWemProfile_ = ::whiteout::models::wem::ProfileId::Count;
};

} // namespace whiteout::flakes
