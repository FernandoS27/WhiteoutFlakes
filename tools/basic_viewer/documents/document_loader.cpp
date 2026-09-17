#include "documents/document_loader.h"

#include "documents/document_manager.h"
#include "documents/model_formats.h"
#include "documents/playback_controller.h"
#include "session/storage_session.h"

#include "io/file_content_provider.h"
#include "io/storage/storage_paths.h"
#include "io/wem/wem_import.h"
#include "io/wem/wem_profiles.h"
#include "renderer/effects/splat_service.h"
#include "renderer/model/corn_effect_source.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_loader.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "whiteout/flakes/event_data.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <whiteout/models/mdx/mdx.h>

#include <cstdio>
#include <exception>
#include <filesystem>

namespace whiteout::flakes {

namespace fs = std::filesystem;
namespace model = renderer::model;
using ::whiteout::models::wem::ProfileId;

DocumentLoader::DocumentLoader(renderer::RenderService& service, DocumentManager& documents,
                               PlaybackController& playback, StorageSession& session)
    : service_(service), documents_(documents), playback_(playback), session_(session) {}

bool DocumentLoader::LoadModel(const fs::path& path) {
    // A standalone effect has no animation list; File > Open, the CLI and the
    // startup picker all accept one transparently.
    if (IsModelKind(path, ModelKind::Effect))
        return LoadEffect(path);

    // A path with no file behind it may still be a storage-internal one — the
    // CLI names `units/human/footman/footman.mdx` and the shared provider's game
    // storage resolves it. The open itself is the probe: the spawn fails cleanly
    // when nothing resolves, and only then is "file not found" the truth.
    const bool onDisk = fs::exists(path);

    // A `.wem` with nobody to ask: the CLI, the startup picker and a drop all
    // reach here with no dialog in front of them, so the document's own default
    // profile answers — settled before the document opens (see OpenWemAs).
    const ModelFormat* format = FindModelFormat(path);
    if (onDisk && format && IsInterchangeKind(format->kind) && !pendingWemDocument_)
        return OpenWemAs(path, nullptr, ProfileId::Count);

    if (OpenDocument(path, /*effect=*/false))
        return true;
    if (!onDisk)
        std::fprintf(stderr, "[viewer] file not found: %s\n", io::PathToUtf8(path).c_str());
    return false;
}

bool DocumentLoader::OpenModelAsync(const fs::path& path) {
    if (!fs::exists(path)) {
        std::fprintf(stderr, "[viewer] file not found: %s\n", io::PathToUtf8(path).c_str());
        return false;
    }
    const bool effect = IsModelKind(path, ModelKind::Effect);

    // The game FIRST: which storage this load needs is decided by the file, so
    // asking about the slot before this would ask about the wrong one.
    session_.FollowModelGame(path, pendingWemProfile_);
    if (!session_.DocumentOpenWouldWait())
        return OpenDocument(path, effect);

    session_.PreloadForDocumentAsync([this, path, effect] {
        // Opened, failed or cancelled, the document opens either way: a model
        // that needs no storage still loads, and one that does reports its own
        // miss rather than being silently dropped.
        OpenDocument(path, effect);
    });
    return true;
}

bool DocumentLoader::LoadEffect(const fs::path& path) {
    if (!fs::exists(path)) {
        std::fprintf(stderr, "[viewer] file not found: %s\n", io::PathToUtf8(path).c_str());
        return false;
    }
    return OpenDocument(path, /*effect=*/true);
}

void DocumentLoader::QueueInitialOpen(const fs::path& path) {
    pendingInitialOpens_.push_back(path);
}

void DocumentLoader::OpenNextQueued(bool tasksBusy) {
    // One per frame and only while nothing else loads, so each gets its own bar
    // instead of racing the one before it.
    if (pendingInitialOpens_.empty() || tasksBusy)
        return;
    const fs::path next = pendingInitialOpens_.front();
    pendingInitialOpens_.erase(pendingInitialOpens_.begin());
    OpenModelAsync(next);
}

// ---- WEM interchange ----------------------------------------------------------------

std::shared_ptr<io::WemDocument> DocumentLoader::PeekWemDocument(const fs::path& path) {
    const ModelFormat* format = FindModelFormat(path);
    if (format && format->kind == ModelKind::Gltf)
        return io::ParseGltfFile(path);
    if (!format || format->kind != ModelKind::Wem)
        return nullptr;
    return io::ParseWemFile(path);
}

bool DocumentLoader::OpenWemAs(const fs::path& path, std::shared_ptr<io::WemDocument> document,
                               ProfileId profile) {
    if (!document)
        document = PeekWemDocument(path);
    if (!document) {
        std::fprintf(stderr, "[viewer] '%s' is not a WEM file this build can read\n",
                     io::PathToUtf8(path).c_str());
        return false;
    }
    if (profile == ProfileId::Count)
        profile = preferredWemProfile_;
    if (profile == ProfileId::Count)
        profile = io::DefaultWemProfile(document->document);

    // Both before OpenDocument: the game follow inside it needs the profile to
    // know which storage to point the shared provider at. A `.wem` whose textures
    // are fileDataIDs and a provider left on Warcraft III is a silent white load.
    pendingWemDocument_ = std::move(document);
    pendingWemProfile_ = profile;
    const bool ok = OpenDocument(path, /*effect=*/false);
    pendingWemDocument_.reset();
    return ok;
}

// ---- Documents ------------------------------------------------------------------------

bool DocumentLoader::OpenDocument(const fs::path& path, bool effect) {
    session_.FollowModelGame(path, pendingWemProfile_);
    // All documents share one configured game provider, so the CASC/MPQ/install
    // set is identical across tabs.
    return documents_.Open(session_.SharedProvider(), io::PathToUtf8(path.stem()), [&](Document& doc) {
        // SetPE1BasePath on a scene with an external provider updates only the
        // template cache's base path, not the shared provider's: set its local
        // file root directly so sibling textures resolve.
        session_.Provider().SetBasePath(path.parent_path());
        return effect ? LoadEffectInto(doc, path) : LoadModelInto(doc, path);
    });
}

bool DocumentLoader::OpenStorageDocument(const std::string& archivePath, bool effect,
                                         std::shared_ptr<io::IContentProvider> provider) {
    // The Storage Explorer's provider opens its storage on first read, and for a
    // double-clicked model that read is this load: put a bar in front of it.
    if (auto* fp = dynamic_cast<io::FileContentProvider*>(provider.get())) {
        const io::StorageState state = fp->StoragesState();
        if (state != io::StorageState::Open && state != io::StorageState::Failed) {
            session_.RunStorageOpenTask(*fp, [this, archivePath, effect, provider](bool) {
                OpenStorageDocumentNow(archivePath, effect, provider);
            });
            return true;
        }
    }
    return OpenStorageDocumentNow(archivePath, effect, std::move(provider));
}

bool DocumentLoader::OpenStorageDocumentNow(const std::string& archivePath, bool effect,
                                            std::shared_ptr<io::IContentProvider> provider) {
    const fs::path apath = io::FsPathFromUtf8(archivePath);
    // The document reads through the EXPLORER's provider, so the model resolves
    // from the storage the user is browsing.
    return documents_.Open(provider, io::PathToUtf8(apath.stem()), [&](Document& doc) {
        service_.Scene().SetPE1BasePath(apath.parent_path());

        // Mode and tier, armed before the spawn so the parse, the SLK loads and
        // the textures resolve through them. An effect has no layers to probe and
        // takes its tier from where it lives; a model's layers and its location
        // decide both (io::Wc3TierForModel).
        Wc3ArtTier tier = io::Wc3TierOfPath(archivePath).value_or(Wc3ArtTier::Classic);
        RenderMode mode = tier == Wc3ArtTier::Classic ? RenderMode::SD : RenderMode::HD;
        if (!effect && provider && IsModelKind(apath, ModelKind::Wc3Model)) {
            mode = ProbeWc3RenderMode(*provider, archivePath);
            tier = io::Wc3TierForModel(archivePath, mode == RenderMode::HD);
        }
        ApplyRenderMode(mode);
        service_.Scene().SetArtTier(tier);

        service_.Loader().RequestClearAll();
        doc.state.modelPath = apath;
        model::Actor* hero =
            effect ? service_.Loader().SpawnUnitFromSource(std::make_shared<model::CornEffectSource>(archivePath))
                   : service_.Loader().SpawnUnit(archivePath);
        if (!hero) {
            std::fprintf(stderr, "[viewer] storage open FAILED for %s\n", archivePath.c_str());
            return false;
        }
        if (effect)
            playback_.OnEffectLoaded(doc.state, hero);
        else
            playback_.OnModelLoaded(doc.state, hero, apath);
        // Assets the browse queued may have drained through the WRONG provider —
        // with no document open, the main viewport pumps the shared needs queue
        // through the default scene's, which on a fresh session has no storage
        // and no HD overlay — and a failed drain is dropped for good. Re-queue
        // them now that this scene is what pumps next.
        service_.RetryUnloadedAssets();
        return true;
    });
}

namespace {

// Any layer whose ShaderType is not `SD` is a Reforged HD layer (shipping models
// tag classic-on-HD as `SDOnHD`, which counts).
RenderMode RenderModeOfLayers(const whiteout::mdx::Model& model) {
    for (const auto& mat : model.materials)
        for (const auto& layer : mat.layers)
            if (layer.shader != whiteout::mdx::Layer::ShaderType::SD)
                return RenderMode::HD;
    return RenderMode::SD;
}

} // namespace

RenderMode DocumentLoader::ProbeWc3RenderMode(const fs::path& path) const {
    // The mode has to be settled before the spawn, which primes SLK loads, splat
    // prefetches and the BLS shader path under it. Parsing the MDX through
    // WhiteoutLib is enough.
    try {
        whiteout::mdx::Parser parser;
        return RenderModeOfLayers(parser.parse(io::PathToUtf8(path)));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[viewer] HD-probe parse FAILED for %s: %s (continuing in SD)\n",
                     io::PathToUtf8(path).c_str(), e.what());
    }
    return RenderMode::SD;
}

RenderMode DocumentLoader::ProbeWc3RenderMode(io::IContentProvider& provider,
                                              const std::string& archivePath) const {
    const auto bytes = provider.ReadFile(archivePath);
    if (!bytes) {
        std::fprintf(stderr, "[viewer] HD-probe read FAILED for %s (continuing in SD)\n",
                     archivePath.c_str());
        return RenderMode::SD;
    }
    try {
        const auto format = io::GetLowerExtension(archivePath) == ".mdl"
                                ? whiteout::mdx::MDLXFormat::MDL
                                : whiteout::mdx::MDLXFormat::MDX;
        whiteout::mdx::Parser parser;
        return RenderModeOfLayers(parser.parse(std::span<const u8>(*bytes), format));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[viewer] HD-probe parse FAILED for %s: %s (continuing in SD)\n",
                     archivePath.c_str(), e.what());
    }
    return RenderMode::SD;
}

bool DocumentLoader::LoadModelInto(Document& doc, const fs::path& path) {
    doc.state.modelPath = path;
    service_.Scene().SetPE1BasePath(path.parent_path());

    const ModelFormat* format = FindModelFormat(path);
    if (format && IsInterchangeKind(format->kind)) {
        // The mode comes from the PROFILE, not a probe: "is this Reforged" is
        // which Warcraft III profile was picked. Every other profile is SD for
        // the reason `.m2` and `.m3` are — its frame is the scene product's.
        ApplyRenderMode((forceHd_ || io::WemProfileIsHd(pendingWemProfile_)) ? RenderMode::HD
                                                                            : RenderMode::SD);
        service_.Loader().RequestClearAll();

        std::shared_ptr<io::WemDocument> document = pendingWemDocument_;
        if (!document)
            document = format->kind == ModelKind::Gltf ? io::ParseGltfFile(path) : io::ParseWemFile(path);
        if (!document) {
            std::fprintf(stderr, "[viewer] '%s' is not a file this build can read\n",
                         io::PathToUtf8(path).c_str());
            return false;
        }
        model::Actor* hero = service_.Loader().SpawnWemDocument(*document, pendingWemProfile_);
        if (!hero) {
            std::fprintf(stderr, "[viewer] SpawnWemDocument FAILED for %s\n", io::PathToUtf8(path).c_str());
            return false;
        }
        playback_.OnModelLoaded(doc.state, hero, path);
        return true;
    }

    if (format && format->kind == ModelKind::ForeignModel) {
        // No BLS layers to probe: HD is a Warcraft III distinction, and a foreign
        // model's frame is chosen by the product its magic names.
        ApplyRenderMode(RenderMode::SD);
    } else {
        const RenderMode probed = ProbeWc3RenderMode(path);
        ApplyRenderMode(forceHd_ ? RenderMode::HD : probed);
    }

    service_.Loader().RequestClearAll();
    model::Actor* hero = service_.Loader().SpawnUnit(io::PathToUtf8(path));
    if (!hero) {
        std::fprintf(stderr, "[viewer] SpawnUnit FAILED for %s\n", io::PathToUtf8(path).c_str());
        // The likeliest cause for a format that can be compiled out, and one
        // "SpawnUnit FAILED" alone sends the reader looking for a corrupt file.
        if (format && !format->compiled)
            std::fprintf(stderr, "[viewer]   %.*s support is not compiled in — configure with -D%.*s=ON\n",
                         static_cast<int>(format->extension.size()), format->extension.data(),
                         static_cast<int>(format->buildOption.size()), format->buildOption.data());
        return false;
    }
    playback_.OnModelLoaded(doc.state, hero, path);
    return true;
}

bool DocumentLoader::LoadEffectInto(Document& doc, const fs::path& path) {
    doc.state.modelPath = path;
    // PopcornFX is Reforged-only content: always HD, whatever the previous
    // document or the Reforged Graphics toggle.
    ApplyRenderMode(RenderMode::HD);
    // Textures the effect references resolve against its own directory.
    service_.Scene().SetPE1BasePath(path.parent_path());

    service_.Loader().RequestClearAll();
    auto source = std::make_shared<model::CornEffectSource>(io::PathToUtf8(path));
    model::Actor* hero = service_.Loader().SpawnUnitFromSource(source);
    if (!hero) {
        std::fprintf(stderr, "[viewer] effect spawn FAILED for %s\n", io::PathToUtf8(path).c_str());
        return false;
    }
    playback_.OnEffectLoaded(doc.state, hero);
    return true;
}

bool DocumentLoader::ReloadActiveModel() {
    Document* doc = documents_.Active();
    if (!doc || doc->state.modelPath.empty())
        return false;
    const fs::path path = doc->state.modelPath;
    return LoadModelInto(*doc, path);
}

void DocumentLoader::RestyleOrReload(const std::function<bool(u32 focusActor)>& restyle) {
    if (!restyle(documents_.ActiveState().focusActor))
        ReloadActiveModel();
}

// ---- Render mode ------------------------------------------------------------------------

void DocumentLoader::SetForceHd(bool on) {
    if (forceHd_ == on)
        return;
    forceHd_ = on;
    // Forcing a mode IS scripting it: while the force stands, the loader must not
    // true the scene up to the parsed template.
    service_.Settings().SetFollowModelRenderMode(!on);
    // Reload so the model re-probes and its dependencies re-resolve under the new
    // overlay. An effect cannot be re-probed as MDX, and a foreign model renders
    // through its own profile — both reloads would change nothing.
    const fs::path& path = documents_.ActiveState().modelPath;
    if (path.empty() || IsModelKind(path, ModelKind::Effect) || IsModelKind(path, ModelKind::ForeignModel))
        return;
    ReloadActiveModel();
}

void DocumentLoader::SetArtTier(std::optional<Wc3ArtTier> tier) {
    if (service_.Settings().GetArtTier() == tier)
        return;
    service_.Settings().SetArtTier(tier);
    // A storage browse may have pinned the scene's own tier; clear it so the new
    // global takes effect.
    service_.Scene().ClearArtTier();
    if (auto* p = service_.Scene().ActiveContentProviderIfAny())
        p->SetArtTier(service_.EffectiveArtTier());
    // Reload so every dependent read re-resolves, with SetForceHd's exemption for
    // a foreign model.
    const fs::path path = documents_.ActiveState().modelPath;
    if (path.empty() || IsModelKind(path, ModelKind::ForeignModel))
        return;
    if (IsModelKind(path, ModelKind::Effect))
        LoadEffect(path);
    else
        ReloadActiveModel();
}

void DocumentLoader::ApplyRenderMode(RenderMode wanted) {
    const bool modeFlipped = service_.EffectiveRenderMode() != wanted;
    // The ACTIVE SCENE owns its mode: the pipeline, the loader's latches and the
    // CASC overlay all read it there. The global follows as the fallback for
    // unsettled scenes and as the ini-persisted preference.
    service_.Scene().SetRenderMode(wanted);
    service_.Settings().SetRenderMode(wanted);
    if (!modeFlipped)
        return;

    // Splat and SLK caches keyed under the old mode move before any event-data
    // fetch resolves under the new one. Nothing cached means nothing to move —
    // forcing the tables in here would open Warcraft III's install for a flip.
    auto* p = service_.Scene().ActiveContentProvider();
    if (!p || !io::IsSplCachePopulated())
        return;
    // Live splats hold refcounts on slots keyed by the old-mode texture.
    service_.Splats().Clear();
    // The tables are kept per mode, so flipping back re-reads no SLKs — only the
    // textures, whose slots are keyed by path alone.
    io::SyncEventDataMode(p, service_.Assets());
}

} // namespace whiteout::flakes
