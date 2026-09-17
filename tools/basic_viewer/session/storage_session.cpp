#include "session/storage_session.h"

#include "app/viewer_tuning.h"
#include "documents/model_formats.h"
#include "session/game_profiles.h"
#include "io/file_content_provider.h"
#include "io/load_task.h"
#include "io/wem/wem_profiles.h"
#include "renderer/model/model_loader.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "settings_ini.h"
#include "string_util.h"
#include "whiteout/flakes/util/path_utf8.h"
#if WDX_ENABLE_M2
#include "renderer/profiles/wow/wow_character_appearance.h"
#include "renderer/profiles/wow/wow_replaceable_textures.h"
#endif
#if WDX_ENABLE_M3
#include "renderer/profiles/sc2_heroes/sc2_model_catalog.h"
#endif

#include <cstdio>
#include <string>
#include <system_error>
#include <thread>

namespace whiteout::flakes {

namespace wem = ::whiteout::models::wem;

namespace {

// For the progress modal's title: the names the Settings profile list uses.
const char* GameDisplayName(ProductId game) {
    return GameProfileOf(game).displayName;
}

// Progress weights: what each part of a document preload actually costs. The
// install open is the long pole; the fourteen character-customisation tables
// and the skin tables' four follow it.
constexpr u64 kPreloadStorageWeight = 70;
constexpr u64 kPreloadCharacterTablesWeight = 22;
constexpr u64 kPreloadSkinTablesWeight = 8;
constexpr u64 kPrewarmCharacterTablesWeight = 3;
constexpr u64 kPrewarmSkinTablesWeight = 1;

} // namespace

StorageSession::StorageSession(renderer::RenderService& service, io::LoadTaskRunner& tasks)
    : service_(service), tasks_(tasks) {
    // The game the user was last working with. An ini read and nothing more:
    // which product the provider serves is decided by applying the same value,
    // and by content loaded later.
    settingsProfile_ = LoadIoProduct();
}

io::FileContentProvider& StorageSession::Provider() {
    return service_.DefaultScene().GetContentProvider();
}

std::shared_ptr<io::IContentProvider> StorageSession::SharedProvider() {
    if (!sharedProvider_) {
        // Aliased without ownership (a no-op deleter): every document scene
        // shares this one provider and its CASC/MPQ/install configuration.
        io::IContentProvider* p = &Provider();
        sharedProvider_ = std::shared_ptr<io::IContentProvider>(p, [](io::IContentProvider*) {});
    }
    return sharedProvider_;
}

void StorageSession::SetSettingsProfile(ProductId game) {
    if (settingsProfile_ == game)
        return;
    settingsProfile_ = game;
    // Persisted so the next launch comes back here, and applied to nothing: a
    // profile's storage opens when that profile is needed, not when its
    // settings page is opened.
    SaveIoProduct(game);
}

void StorageSession::ApplyProfile(ProductId game, bool force) {
    auto& provider = Provider();
    if (!force && appliedProfiles_.count(game)) {
        // The slot already holds this profile's settings — and whatever the
        // session added to them, like keys AdoptNearbyWowKeys found beside a
        // model. Re-applying the ini would write its empty listfile back over
        // that, and a changed value invalidates the slot: the provider drops the
        // last reference to the install, the registry holds it only by weak_ptr,
        // and the next read re-parses indices, manifest and a 144 MB listfile.
        // Switching between two open models is supposed to be a pointer move.
        provider.SetGame(game);
        return;
    }
    // A different install means different tables.
    if (game == ProductId::Wow)
        wowTablesPrewarmed_ = false;
    if (game == ProductId::Sc2)
        sc2CatalogPrewarmed_ = false;
    if (onProfileApplying_)
        onProfileApplying_(game);
    ApplyIoPathOverrides(provider, game);
    appliedProfiles_.insert(game);
}

// Which game the provider serves decides which storage resolves a document's
// textures, and every document shares one provider whose game comes from
// Settings. So an `.m2` opened while that says Warcraft III loads its geometry —
// the `.skin` sits beside it on disk — and silently loses every texture, which
// are fileDataIDs with no WoW storage open.
//
// Warcraft III follows too, and not as a formality: with the panel left on
// StarCraft II or World of Warcraft an `.mdx` loses its CASC textures, the
// day/night rig, the IBL probes and every event SLK — all read during the load,
// and switching back does not repair them, because those are load-time decisions.
//
// Not persisted: the user opened a file, they did not pick a profile. The
// Settings panel reads its selection off the provider, so it still shows the
// truth for the session.
void StorageSession::FollowModelGame(const std::filesystem::path& path, wem::ProfileId wemProfile) {
    const ModelFormat* format = FindModelFormat(path);
    if (!format)
        return;
    // One `.wem` opens as any profile it carries, and the profile decides which
    // game's storage its textures resolve against. A glTF import derives into
    // Reforged, so its answer is Warcraft III.
    const ProductId game =
        IsInterchangeKind(format->kind) ? io::ProductForWemProfile(wemProfile) : format->game;
    if (game == ProductId::Neutral)
        return;

    // Content is what makes a profile *needed*, and this is the only path that
    // moves the provider onto one: configuring a game is no reason to open it.
    settingsProfile_ = game;
    if (Provider().Game() != game) {
        ApplyProfile(game, /*force=*/false);
        // The load that follows demands this storage anyway, and assets that
        // missed under the old game get their chance under the new one.
        service_.RetryUnloadedAssets();
    }
    if (game == ProductId::Wow)
        AdoptNearbyWowKeys(path);
}

// A loose `.m2` tree was extracted from an id-keyed root by something that
// needed a listfile, so one usually sits a directory or two above the model,
// next to the TACT key list the same extraction needed. Adopting them turns the
// extraction back into a storage that knows its names and can read the client
// databases. Session-only, like the game follow; Settings > IO always wins.
void StorageSession::AdoptNearbyWowKeys(const std::filesystem::path& modelPath) {
    auto& provider = Provider();
    bool wantListfile = provider.ListfilePath().empty();
    bool wantKeys = provider.TactKeyPath().empty();

    std::error_code ec;
    std::filesystem::path dir = modelPath.parent_path();
    for (i32 up = 0; up < tuning::kNearbyKeySearchLevels && (wantListfile || wantKeys); ++up) {
        for (const auto& entry : std::filesystem::directory_iterator(
                 dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            const std::string name = tools::ToLowerAscii(io::PathToUtf8(entry.path().filename()));
            const bool isListfile =
                wantListfile && name.ends_with(".csv") && name.find("listfile") != std::string::npos;
            const bool isKeys = wantKeys && name.ends_with(".txt") && name.find("tactkey") != std::string::npos;
            if (!isListfile && !isKeys)
                continue;
            std::fprintf(stderr, "[viewer] adopting %s beside the content: %s\n",
                         isListfile ? "listfile" : "TACT keys", io::PathToUtf8(entry.path()).c_str());
            if (isListfile) {
                provider.SetListfilePath(entry.path());
                wantListfile = false;
            } else {
                provider.SetTactKeyPath(entry.path());
                wantKeys = false;
            }
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir)
            break;
        dir = parent;
    }
}

void StorageSession::RunStorageOpenTask(io::FileContentProvider& provider,
                                        std::function<void(bool ok)> onDone) {
    // Already open, or failed and not worth retrying: answer now rather than
    // flashing a modal for a task with nothing to do.
    const io::StorageState state = provider.StoragesState();
    if (state == io::StorageState::Open || state == io::StorageState::Failed) {
        if (onDone)
            onDone(state == io::StorageState::Open);
        return;
    }
    io::FileContentProvider* p = &provider;

    if (state == io::StorageState::Opening) {
        // A read on a provider worker already started one. Its progress belongs
        // to that call, but waiting behind an indeterminate bar beats answering
        // "not open" and letting the caller block the host thread on it.
        tasks_.Run(
            std::string("Opening ") + GameDisplayName(provider.Game()),
            [p](io::ProgressMonitor& m) {
                m.Begin("Waiting for storage"); // no total: not our open to count
                while (p->StoragesState() == io::StorageState::Opening) {
                    if (m.Cancelled())
                        return io::TaskResult::Fail("Cancelled");
                    std::this_thread::sleep_for(tuning::kStorageOpenPollInterval);
                }
                return io::TaskResult::Ok();
            },
            [p, onDone = std::move(onDone)](const io::TaskOutcome&) {
                if (onDone)
                    onDone(p->StoragesState() == io::StorageState::Open);
            },
            // Not cancellable, and saying so: the open belongs to another
            // caller, and stopping this wait would not stop it.
            /*cancellable=*/false);
        return;
    }

    tasks_.Run(
        std::string("Opening ") + GameDisplayName(provider.Game()),
        [p](io::ProgressMonitor& m) {
            // The demanded path, so it retries a previously cancelled open —
            // the point of the user asking again after pressing Cancel.
            return p->OpenStorages(&m) ? io::TaskResult::Ok() : io::TaskResult::Fail("No storage opened");
        },
        [onDone = std::move(onDone)](const io::TaskOutcome& out) {
            if (onDone)
                onDone(out.ok);
        });
}

void StorageSession::OpenStoragesAsync(std::function<void(bool ok)> onDone) {
    RunStorageOpenTask(Provider(), [this, onDone = std::move(onDone)](bool ok) {
        if (ok) {
            // Assets that missed while nothing was open get another chance.
            service_.RetryUnloadedAssets();
            // Chained rather than kicked in parallel: both want the task thread,
            // and the tables cannot be read before the storage holding them is
            // up. At most one of the two does anything.
            PrewarmWowTablesAsync();
            PrewarmSc2CatalogAsync();
        }
        if (onDone)
            onDone(ok);
    });
}

bool StorageSession::DocumentOpenWouldWait() {
    io::FileContentProvider& provider = Provider();
    const io::StorageState state = provider.StoragesState();
    const bool needStorage = state != io::StorageState::Open && state != io::StorageState::Failed;
    // The databases count too: the spawn reads them synchronously through
    // WowReplaceableTextures::Apply, so a document opened before they are in hand
    // freezes the host thread for fourteen CASC reads.
    const bool needTables = provider.Game() == ProductId::Wow && !wowTablesPrewarmed_;
    return needStorage || needTables;
}

void StorageSession::PreloadForDocumentAsync(std::function<void()> then) {
    io::FileContentProvider& provider = Provider();

    // Resolved on the host thread, not in the body: both are lazily constructed,
    // and the task thread must not be what constructs them.
    renderer::profiles::wow::WowReplaceableTextures* skins = nullptr;
    renderer::profiles::wow::WowCharacterAppearance* chars = nullptr;
#if WDX_ENABLE_M2
    if (provider.Game() == ProductId::Wow && !wowTablesPrewarmed_) {
        skins = &service_.Loader().WowReplaceables();
        chars = &service_.Loader().WowCharacters();
        skins->SetContentProvider(&provider);
        chars->SetContentProvider(&provider);
    }
#endif

    io::FileContentProvider* p = &provider;
    const bool tables = chars != nullptr;
    tasks_.Run(
        std::string("Opening ") + GameDisplayName(provider.Game()),
        [p, skins, chars, tables](io::ProgressMonitor& m) {
            m.Begin("Preparing", tables ? kPreloadStorageWeight + kPreloadCharacterTablesWeight +
                                              kPreloadSkinTablesWeight
                                        : kPreloadStorageWeight);
            {
                io::ProgressMonitor step = m.Split(kPreloadStorageWeight);
                step.Begin("Waiting for storage");
                // A read on a provider worker may already be opening it: wait that
                // out rather than queueing a second open behind it, and report
                // while waiting, which is the whole point.
                while (p->StoragesState() == io::StorageState::Opening) {
                    if (m.Cancelled())
                        return io::TaskResult::Fail("Cancelled");
                    std::this_thread::sleep_for(tuning::kStorageOpenPollInterval);
                }
                // Demanded, so a previously cancelled open is retried. Its own plan
                // replaces the placeholder stage above.
                p->OpenStorages(&step);
            }
            if (m.Cancelled())
                return io::TaskResult::Fail("Cancelled");

            if (chars) {
                {
                    io::ProgressMonitor step = m.Split(kPreloadCharacterTablesWeight);
                    chars->Prewarm(&step);
                }
                if (!m.Cancelled()) {
                    io::ProgressMonitor step = m.Split(kPreloadSkinTablesWeight);
                    skins->Prewarm(&step);
                }
            }
            // Always Ok. A storage that would not open and an install that cannot
            // serve the databases are normal states; the document opens either
            // way and reports its own miss.
            return io::TaskResult::Ok();
        },
        [this, tables, then = std::move(then)](const io::TaskOutcome& out) {
            // Tried, whether or not it worked — see wowTablesPrewarmed_.
            if (tables && !out.cancelled)
                wowTablesPrewarmed_ = true;
            service_.RetryUnloadedAssets();
            if (then)
                then();
        });
}

void StorageSession::PrewarmWowTablesAsync() {
#if WDX_ENABLE_M2
    io::FileContentProvider& provider = Provider();
    if (provider.Game() != ProductId::Wow)
        return;
    // Only once the storage is up. Kicked against a pending storage, the task
    // thread would trigger the open itself, behind a bar that claims to be
    // reading databases.
    if (provider.StoragesState() != io::StorageState::Open)
        return;
    auto& replaceables = service_.Loader().WowReplaceables();
    auto& characters = service_.Loader().WowCharacters();
    if (replaceables.Table().Loaded() && characters.Tables().Loaded())
        return;

    replaceables.SetContentProvider(&provider);
    characters.SetContentProvider(&provider);
    tasks_.Run(
        "Reading client databases",
        [&replaceables, &characters](io::ProgressMonitor& m) {
            m.Begin("Client databases", kPrewarmCharacterTablesWeight + kPrewarmSkinTablesWeight);
            {
                io::ProgressMonitor step = m.Split(kPrewarmCharacterTablesWeight);
                characters.Prewarm(&step);
            }
            if (m.Cancelled())
                return io::TaskResult::Fail("Cancelled");
            io::ProgressMonitor step = m.Split(kPrewarmSkinTablesWeight);
            replaceables.Prewarm(&step);
            // Always Ok: an install that cannot serve these tables (no listfile,
            // no keys, a classic client) is normal, and an error box would stand
            // in front of a user who asked for nothing.
            return io::TaskResult::Ok();
        },
        [this](const io::TaskOutcome& out) {
            if (!out.cancelled)
                wowTablesPrewarmed_ = true;
            // Re-apply to what is already loaded: the restyle runs exactly the
            // passes the tables feed.
            if (out.ok && onWowTablesReady_)
                onWowTablesReady_();
        },
        /*cancellable=*/true,
        // NOT modal. Nobody asked for these tables; a model spawned before they
        // arrive shows its default look and is restyled when they land.
        /*modal=*/false);
#endif
}

void StorageSession::PrewarmSc2CatalogAsync() {
#if WDX_ENABLE_M3
    io::FileContentProvider& provider = Provider();
    if (provider.Game() != ProductId::Sc2 || sc2CatalogPrewarmed_)
        return;
    // Same rule as the World of Warcraft tables: only once the storage is up.
    if (provider.StoragesState() != io::StorageState::Open)
        return;
    auto& catalog = service_.Loader().Sc2Catalog();
    if (catalog.Loaded())
        return;

    catalog.SetContentProvider(&provider);
    tasks_.Run(
        "Reading the model catalog",
        [&catalog](io::ProgressMonitor& m) {
            catalog.Prewarm(&m);
            // Always Ok: an install with no GameData in it is a normal state.
            return io::TaskResult::Ok();
        },
        [this](const io::TaskOutcome& out) {
            if (!out.cancelled)
                sc2CatalogPrewarmed_ = true;
        },
        /*cancellable=*/true,
        // NOT modal, and nothing to re-apply: a model that loaded first built the
        // index inside its own load; this only spares the ones after it.
        /*modal=*/false);
#endif
}

} // namespace whiteout::flakes
