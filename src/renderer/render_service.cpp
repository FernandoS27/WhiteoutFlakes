#include "debug/debug_renderer.h"
#include "renderer/assets/asset_manager.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/assets/sampler_asset_manager.h"
#include "renderer/assets/texture_asset_manager.h"
#include "renderer/bls/bls_shader_cache.h"
#include "renderer/core/render_profile.h"
#include "renderer/corn_effects/corn_effects_service.h"
#include "renderer/dnc/dnc_service.h"
#include "renderer/physics/ground_plane.h"
#include "renderer/imgui/imgui_renderer.h"
#include "renderer/model/model_loader.h"
#include "renderer/model/model_source_utils.h"
#include "renderer/model/model_template_manager.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/render_service_impl.h"
#include "renderer/scene_manager.h"
#include "renderer/shadow/shadow_service.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/event_data.h"
#include "whiteout/flakes/util/replaceable_paths.h"
#include "whiteout/flakes/util/texture_image_usage.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace whiteout::flakes::renderer {

RenderService::Impl::~Impl() {
    // `scenes_` holds every SceneManager the service knows about — the
    // borrowed default plus anything from CreateScene — so one pass covers
    // them all. The scenes themselves are not touched; only the actors,
    // whose destructors reach back into the asset managers.
    for (auto& [id, scene] : scenes_) {
        if (scene)
            scene->Actors().Clear();
    }
}


using namespace ::whiteout::flakes::renderer::model;
using namespace ::whiteout::flakes::renderer::animation;
using namespace ::whiteout::flakes::renderer::effects;
using namespace ::whiteout::flakes::renderer::assets;
using namespace ::whiteout::flakes::renderer::debug;
using namespace ::whiteout::flakes::renderer::particle;
using namespace ::whiteout::flakes::renderer::shadow;
using namespace ::whiteout::flakes::renderer::dnc;

RenderService::RenderService(SceneManager& scene) : impl_(std::make_unique<Impl>()) {
    // Register the passed scene as the default scene (id 0). The host owns it
    // (borrowed); scenes created later via CreateScene() are owned by us.
    impl_->scenes_[impl_->defaultSceneId_] = &scene;
    impl_->sceneServices_[impl_->defaultSceneId_] = std::make_unique<SceneServices>(*this);
    impl_->activeSceneId_ = impl_->defaultSceneId_;
    impl_->activeScene_ = &scene;
    impl_->activeServices_ = impl_->sceneServices_[impl_->defaultSceneId_].get();

    impl_->debug_ = std::make_unique<DebugRenderer>(*this);
    impl_->pipeline_ = std::make_unique<RenderPipeline>(*this);
    impl_->ticker_ = std::make_unique<FrameTicker>(*this);
    // ModelLoader must be constructed BEFORE the texture-cache lambda below
    // is installed, since the lambda dispatches into impl_->loader_.
    impl_->loader_ = std::make_unique<ModelLoader>(*this);
    impl_->soundEmitter_ = MakeNullSoundEmitter();
    // Default sound volume for a fresh run (no INI yet). SwapSoundEmitter
    // carries this onto the real backend, and LoadSettingsIni overrides it
    // when a persisted SoundVolume exists.
    impl_->soundEmitter_->SetVolume(0.5f);
}

RenderService::~RenderService() = default;

// ---- Scene registry ----
SceneId RenderService::CreateScene() {
    const SceneId id = impl_->nextSceneId_++;
    auto scene = std::make_unique<SceneManager>();
    impl_->scenes_[id] = scene.get();
    impl_->ownedScenes_.push_back(std::move(scene));
    impl_->sceneServices_[id] = std::make_unique<SceneServices>(*this);
    // Apply whatever device-dependent wiring is already available (corn-fx
    // backend, splat→AssetManager hook) to the new scene's bundle.
    InitSceneServices(*impl_->sceneServices_[id]);
    return id;
}

void RenderService::DestroyScene(SceneId id) {
    if (id == impl_->defaultSceneId_)
        return; // the default scene lives for the RenderService's lifetime
    auto sit = impl_->scenes_.find(id);
    if (sit == impl_->scenes_.end())
        return;
    SceneManager* scene = sit->second;

    // Release this scene's GPU state before its objects go away: corn-fx
    // buffers/emitters and each actor's GPU resources. The device is still
    // alive here (DestroyScene is a runtime call, not teardown).
    if (auto svcIt = impl_->sceneServices_.find(id); svcIt != impl_->sceneServices_.end()) {
        svcIt->second->corn.Clear();
        svcIt->second->corn.ReleaseGpuResources();
    }
    if (scene) {
        // Unregister every actor from the GLOBAL replaceable-texture manager
        // (keyed by Actor*) before the scene's actors are freed — otherwise its
        // dirty-actor list keeps dangling pointers and the next RebakeDirtyActors
        // dereferences freed memory. Per-scene services (particle/corn/splat/spn)
        // are destroyed with the scene, so they need no explicit unregister.
        if (impl_->replaceables_) {
            for (auto& [h, miPtr] : scene->Actors().All())
                impl_->replaceables_->UnregisterModel(*miPtr);
        }
        if (impl_->pipeline_ && impl_->pipeline_->IsDeviceReady()) {
            if (auto* gfx = impl_->pipeline_->Gfx()) {
                for (auto& [h, miPtr] : scene->Actors().All())
                    miPtr->ReleaseGPU(*gfx);
                scene->Templates().ReleaseAllGPU(*gfx);
            }
            scene->Templates().Clear();
        }
    }

    // If this was the active scene, fall back to the default before erasing.
    if (impl_->activeSceneId_ == id)
        SetActiveScene(impl_->defaultSceneId_);

    impl_->sceneServices_.erase(id);
    impl_->scenes_.erase(id);
    impl_->ownedScenes_.erase(
        std::remove_if(impl_->ownedScenes_.begin(), impl_->ownedScenes_.end(),
                       [scene](const std::unique_ptr<SceneManager>& p) { return p.get() == scene; }),
        impl_->ownedScenes_.end());
}

SceneManager& RenderService::SceneAt(SceneId id) {
    auto it = impl_->scenes_.find(id);
    if (it != impl_->scenes_.end())
        return *it->second;
    return *impl_->scenes_[impl_->defaultSceneId_];
}

SceneManager& RenderService::DefaultScene() {
    return *impl_->scenes_[impl_->defaultSceneId_];
}

SceneId RenderService::DefaultSceneId() const {
    return impl_->defaultSceneId_;
}

void RenderService::SetActiveScene(SceneId id) {
    auto it = impl_->scenes_.find(id);
    if (it == impl_->scenes_.end())
        id = impl_->defaultSceneId_;
    impl_->activeSceneId_ = id;
    impl_->activeScene_ = impl_->scenes_[id];
    impl_->activeServices_ = impl_->sceneServices_[id].get();
    // Impose this scene's effective art tier on the provider it reads through.
    // Scenes legitimately share one provider (explorer cells, viewer
    // documents), so the tier is whatever the last writer left it — this is
    // the one place that re-arms it, replacing the save/restore dances the
    // hosts used to carry. Cheap (an atomic store) and non-creating: a scene
    // without a provider skips it entirely.
    if (auto* p = impl_->activeScene_->ActiveContentProviderIfAny())
        p->SetArtTier(EffectiveArtTier());
}

void RenderService::SetActiveScene(SceneManager& scene) {
    for (auto& [id, s] : impl_->scenes_) {
        if (s == &scene) {
            SetActiveScene(id);
            return;
        }
    }
    SetActiveScene(impl_->defaultSceneId_);
}

SceneManager& RenderService::ActiveScene() {
    return *impl_->activeScene_;
}

RenderMode RenderService::EffectiveRenderMode() {
    return EffectiveRenderMode(*impl_->activeScene_);
}

RenderMode RenderService::EffectiveRenderMode(const SceneManager& scene) {
    return scene.RenderModeOverride().value_or(impl_->settings_.GetRenderMode());
}

Wc3ArtTier RenderService::EffectiveArtTier() {
    return EffectiveArtTier(*impl_->activeScene_);
}

// A scene's own tier wins; failing that the global setting; failing that the
// tier the effective render mode implies. That last step is what keeps a host
// which only ever set a render mode reading the same art it always did — see
// SceneManager::ImpliedArtTier.
Wc3ArtTier RenderService::EffectiveArtTier(const SceneManager& scene) {
    if (auto own = scene.ArtTierOverride())
        return *own;
    if (auto global = impl_->settings_.GetArtTier())
        return *global;
    return SceneManager::ImpliedArtTier(EffectiveRenderMode(scene));
}

bool RenderService::EffectiveSceneHdrInSd() {
    return EffectiveSceneHdrInSd(*impl_->activeScene_);
}

bool RenderService::EffectiveSceneHdrInSd(const SceneManager& scene) {
    return scene.SceneHdrInSdOverride().value_or(impl_->settings_.SceneHdrInSd());
}

void RenderService::TickScenes(f32 dt) {
    // Advance each scene's wall clock + actors, then run the per-scene frame
    // update against it. Snapshot ids first so the loop is stable if a tick
    // mutates the registry.
    std::vector<SceneId> ids;
    ids.reserve(impl_->scenes_.size());
    for (auto& [id, s] : impl_->scenes_)
        ids.push_back(id);
    for (SceneId id : ids) {
        auto it = impl_->scenes_.find(id);
        if (it == impl_->scenes_.end())
            continue;
        it->second->Update(dt);
        impl_->ticker_->Tick(*it->second, dt);
    }
}

// Splats and SPN instances are spawned by playback and mean nothing once the
// clock has moved elsewhere, so they go outright. Particles, ribbons and corn-fx
// are held by emitters the model owns, so those are restarted in place.
void RenderService::DropTransientEffects() {
    // Reset, not Clear, for the two that hold emitters: those are registered
    // when the actor spawns, so clearing the maps would leave the model with no
    // particles and no ribbons until it was reloaded.
    Particles().ResetEmitters();
    Ribbons().ResetTrails();
    Splats().Clear();
    Spn().Clear();
    CornEffects().ResetRuntimes();
}

// Rewind to the first frame. Two things have to happen and both matter: the
// clocks go back to zero, and whatever the effect systems have spawned since is
// thrown away. Skipping the second leaves particles frozen in mid-air from the
// previous run, which reads as a bug the moment you press play again.
void RenderService::RewindScene() {
    SceneManager& scene = ActiveScene();
    scene.SetAnimationTime(0);

    for (auto& [h, mi] : scene.Actors().All()) {
        mi->cursor = model::Actor::Cursor{};
        // A play remembers the clock value it started at, so a rewind that
        // skipped this would leave every play — the model's own global loops
        // included — stamped in the future, and the actor would sit on its
        // first frame for as long as the rewind was long.
        mi->animation.Playlist().Restart(0);
        mi->animation.SetTimeMs(0);
        // Children (PE1 / SPN / attachment) derive their cursor from
        // wall-clock minus birth, so a birth time left in the future would
        // make them evaluate at a negative age until the clock caught up.
        mi->animation.SetBirthTimeMs(0);
    }

    DropTransientEffects();
}

void RenderService::SetCornBackendInitApplier(
    std::function<void(corn_effects::CornEffectsService&)> fn) {
    impl_->cornInitApplier_ = std::move(fn);
    InitAllSceneServices();
}

void RenderService::InitSceneServices(SceneServices& svc) {
    // Each step is idempotent and self-guards on its input being ready, so this
    // can run any number of times as inputs (assets, corn backend) come online.
    //
    // Deliberately does NOT wire the corn-fx content provider here: this runs
    // from CreateScene, before the host installs the scene's provider, and
    // SceneManager::ActiveContentProvider() force-creates the internal
    // FileContentProvider (opening a CASC and spawning IO threads) when none is
    // set yet. That would give every tab — and every explorer thumbnail cell —
    // a throwaway CASC. The pipeline pushes it per frame instead.
    if (impl_->assets_)
        svc.splats.Configure(impl_->assets_.get());
    if (impl_->cornInitApplier_)
        impl_->cornInitApplier_(svc.corn);
}

void RenderService::InitAllSceneServices() {
    for (auto& [id, svc] : impl_->sceneServices_)
        InitSceneServices(*svc);
}

void RenderService::ForEachCornService(
    const std::function<void(corn_effects::CornEffectsService&)>& fn) {
    for (auto& [id, svc] : impl_->sceneServices_)
        fn(svc->corn);
}

void RenderService::ForEachScene(const std::function<void(SceneManager&)>& fn) {
    for (auto& [id, scene] : impl_->scenes_)
        fn(*scene);
}

// ---- Out-of-line accessors (bodies live here so Impl is complete) ----
SceneManager& RenderService::Scene() {
    return *impl_->activeScene_;
}
const SceneManager& RenderService::Scene() const {
    return *impl_->activeScene_;
}
TextureAssetManager& RenderService::Textures() {
    return *impl_->textures_;
}
SamplerAssetManager& RenderService::Samplers() {
    return *impl_->samplers_;
}
ReplaceableTextureManager& RenderService::Replaceables() {
    return *impl_->replaceables_;
}
AssetManager& RenderService::Assets() {
    return *impl_->assets_;
}
const AssetManager& RenderService::Assets() const {
    return *impl_->assets_;
}
void RenderService::RetryUnloadedAssets() {
    impl_->assets_->RetryUnloaded();
#if WDX_ENABLE_D3
    // The one host-agnostic funnel every provider reconfiguration already goes
    // through, which is exactly what the Diablo III SNO cache needs: a sno id
    // means something different in another install, so a cache that survives a
    // re-point serves one install's geometry against another's textures.
    // Handed-out shared_ptrs stay valid; only the cache's own references go.
    Loader().D3Cache().Clear();
#endif
}

void RenderService::EnsureWc3GameData() {
    auto* cp = Scene().ActiveContentProvider();
    if (!cp)
        return;
    // The day/night rig and the IBL probes are Warcraft III content too, and
    // unlike the tables below the rig is per scene — so it runs every time
    // rather than behind the once-per-session gate, and is cheap once
    // satisfied because it is already in hand.
    EnsureDncService().RealiseAsset();
    // The probes are per device, and applying the mode is a destroy and reload
    // of both cube maps — not the free flag-flip this used to assume. This
    // runs on every model load, and a grid of thumbnails is one model load per
    // cell, so ask first. Still marked while they are missing, which is what
    // lets a session that had no install reachable pick them up later.
    if (!Pipeline().HasIblProbes())
        impl_->settings_.MarkIblModeDirty();
    // The splat table standing in for all of them: the loaders below fill the
    // tables together, and each one early-returns after a successful pass, so
    // this is what "already loaded" looks like from outside. It also makes a
    // session with no Warcraft III install retry on the next model rather than
    // give up for good, which is what should happen when the user is still
    // pointing the settings panel at one.
    if (!impl_->assets_ || io::IsSplCachePopulated())
        return;
    io::LoadGameDataFiles(cp);
    io::LoadEventDataFiles(cp);
    // Acquire every SPL/UBR texture and SPN child-model slot the tables name,
    // so they are resident before the first splat is born rather than being
    // fetched during it.
    if (io::IsSplCachePopulated())
        io::PrefetchEventAssetSlots(*impl_->assets_);
}

u64 RenderService::AssetActivityCounter() const {
    const auto st = impl_->assets_->GetStats();
    return static_cast<u64>(st.totalAcquires) + AssetArrivalCounter();
}

u64 RenderService::AssetArrivalCounter() const {
    const auto st = impl_->assets_->GetStats();
    return static_cast<u64>(st.totalApplies) +
           static_cast<u64>(impl_->assets_->PendingNeedsCount());
}

namespace {

// Extensions each asset kind can decode — used to filter a directory
// listing down to the files the requested kind actually understands.
// Matches the texture/model synonym sets FileContentProvider resolves.
bool ExtensionSuitsKind(std::string_view path, assets::AssetKind kind) {
    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos) return false;
    const std::string_view ext = path.substr(dot);
    switch (kind) {
    case assets::AssetKind::Texture:
        return ext == ".blp" || ext == ".dds" || ext == ".tga" || ext == ".png" || ext == ".tif";
    case assets::AssetKind::Effect:
        return ext == ".pkb" || ext == ".pkfx";
    case assets::AssetKind::Model:
        return ext == ".mdx" || ext == ".mdl";
    case assets::AssetKind::Data:
        // No WC3 use, and no extension set that would mean anything across
        // products — an M2's `.skin` and an M3's blob share nothing.
        return false;
    }
    return false;
}

} // namespace

assets::AssetPreload RenderService::PreloadAssets(assets::AssetKind kind,
                                                  assets::AssetSubKind subKind,
                                                  std::span<const ContentRef> refs) {
    if (!impl_->assets_) return {};
    return impl_->assets_->Preload(kind, subKind, refs);
}

assets::AssetPreload RenderService::PreloadAssetDirectory(assets::AssetKind kind,
                                                          assets::AssetSubKind subKind,
                                                          std::string_view directory,
                                                          bool recursive) {
    if (!impl_->assets_ || !impl_->activeScene_) return {};
    auto* provider = Scene().ActiveContentProvider();
    if (!provider) return {};

    std::vector<std::string> listing = provider->ListFiles(std::string(directory), recursive);
    std::vector<ContentRef> refs;
    refs.reserve(listing.size());
    for (const std::string& p : listing) {
        if (ExtensionSuitsKind(p, kind))
            refs.push_back(ContentRef::FromPath(p));
    }
    return impl_->assets_->Preload(kind, subKind, refs);
}

DebugRenderer& RenderService::Debug() {
    return *impl_->debug_;
}

particle::ParticleService& RenderService::Particles() {
    return impl_->activeServices_->particles;
}
particle::SplatService& RenderService::Splats() {
    return impl_->activeServices_->splats;
}
ribbon::RibbonService& RenderService::Ribbons() {
    return impl_->activeServices_->ribbons;
}
corn_effects::CornEffectsService& RenderService::CornEffects() {
    return impl_->activeServices_->corn;
}
bool RenderService::ComputeEffectWorldBounds(u32 actor, i32 emitterId, Vector3f& outMin,
                                             Vector3f& outMax) {
    return impl_->activeServices_->corn.ComputeWorldParticleBounds(actor, emitterId, outMin,
                                                                   outMax);
}
SpnSpawner& RenderService::Spn() {
    return *impl_->activeServices_->spn;
}

dnc::DncService* RenderService::GetDncService() {
    return impl_->activeServices_ ? impl_->activeServices_->dnc.get() : nullptr;
}
const dnc::DncService* RenderService::GetDncService() const {
    return impl_->activeServices_ ? impl_->activeServices_->dnc.get() : nullptr;
}
shadow::ShadowService* RenderService::GetShadowService() {
    return impl_->shadowService_.get();
}
const shadow::ShadowService* RenderService::GetShadowService() const {
    return impl_->shadowService_.get();
}
gtao::GtaoService* RenderService::GetGtaoService() {
    return impl_->gtaoService_.get();
}
const gtao::GtaoService* RenderService::GetGtaoService() const {
    return impl_->gtaoService_.get();
}
dof::DofService* RenderService::GetDofService() {
    return impl_->dofService_.get();
}
const dof::DofService* RenderService::GetDofService() const {
    return impl_->dofService_.get();
}
refraction::RefractionService* RenderService::GetRefractionService() {
    return impl_->refractionService_.get();
}
const refraction::RefractionService* RenderService::GetRefractionService() const {
    return impl_->refractionService_.get();
}
distortion::DistortionService* RenderService::GetDistortionService() {
    return impl_->distortionService_.get();
}
const distortion::DistortionService* RenderService::GetDistortionService() const {
    return impl_->distortionService_.get();
}
#if WDX_ENABLE_M3
sc2::M3DeferredLightService* RenderService::GetM3DeferredLightService() {
    return impl_->m3DeferredLightService_.get();
}
const sc2::M3DeferredLightService* RenderService::GetM3DeferredLightService() const {
    return impl_->m3DeferredLightService_.get();
}
#endif
post_process::PostProcessService* RenderService::GetPostProcessService() {
    return impl_->postProcessService_.get();
}
const post_process::PostProcessService* RenderService::GetPostProcessService() const {
    return impl_->postProcessService_.get();
}

RenderSettings& RenderService::Settings() {
    return impl_->settings_;
}
const RenderSettings& RenderService::Settings() const {
    return impl_->settings_;
}

FrameTicker& RenderService::Ticker() {
    return *impl_->ticker_;
}

ModelLoader& RenderService::Loader() {
    return *impl_->loader_;
}
const ModelLoader& RenderService::Loader() const {
    return *impl_->loader_;
}

RenderPipeline& RenderService::Pipeline() {
    return *impl_->pipeline_;
}
const RenderPipeline& RenderService::Pipeline() const {
    return *impl_->pipeline_;
}

ISoundEmitter& RenderService::Sound() {
    return *impl_->soundEmitter_;
}
const ISoundEmitter& RenderService::Sound() const {
    return *impl_->soundEmitter_;
}

ActorEvalContext RenderService::MakeActorEvalContext() {
    // Binds the ACTIVE scene + its effect services. The ticker publishes the
    // scene it is ticking, so each scene's actors evaluate against their own
    // services/camera.
    SceneManager* scene = impl_->activeScene_;
    SceneServices* svc = impl_->activeServices_;
    ActorEvalContext ctx;
    ctx.camPos = scene->Camera().GetSource();
    // The same accessor the geometry pass uses, so a screen-aligned bone
    // basis lands in the space the rasterizer actually draws in.
    ctx.view = scene->Camera().GetViewMatrix();
    ctx.sceneAnimationTimeMs = scene->GetAnimationTime();
    ctx.fireEvents = impl_->settings_.ShowEvents();
    ctx.poseStagesEnabled = impl_->settings_.PoseSolversEnabled();
    ctx.substepPhysics = impl_->settings_.PhysicsSubstepping();
    // The host's terrain if it registered one; otherwise the plane the physics
    // stages already stand on — the grid — so feet and ragdolls agree on where
    // the ground is without the host saying anything.
    ctx.queryGround = impl_->settings_.GetGroundQuery()
                          ? impl_->settings_.GetGroundQuery()
                          : RenderSettings::GroundQuery(&physics::FlatGroundQuery);
    // SC2 particles collide against the same ground. It is installed on the
    // scene's particle service, which forwards it to every emitter, once — and
    // again only when the host replaces it — rather than per frame, which would
    // copy a std::function into every emitter every frame (design §4.3, R9).
    // Nothing called the service's setter before this, so a CollideTerrain
    // emitter fell through the grid everywhere but in the tests.
    if (svc->particleGroundGeneration != impl_->settings_.GroundQueryGeneration()) {
        svc->particles.SetGroundQuery(ctx.queryGround);
        svc->particleGroundGeneration = impl_->settings_.GroundQueryGeneration();
    }
    ctx.scene = scene;
    ctx.particles = &svc->particles;
    ctx.splats = &svc->splats;
    ctx.ribbons = &svc->ribbons;
    ctx.cornEffects = &svc->corn;
    ctx.spnSpawner = svc->spn.get();
    ctx.sound = impl_->soundEmitter_.get();
    return ctx;
}

bool RenderService::HasDeviceAssetManagers() const {
    return impl_->samplers_ && impl_->textures_ && impl_->replaceables_;
}

void RenderService::CreateDeviceAssetManagers(gfx::IGFXDevice& gfx) {
    impl_->samplers_ = std::make_unique<SamplerAssetManager>(gfx);
    impl_->textures_ = std::make_unique<TextureAssetManager>(gfx);
    impl_->replaceables_ = std::make_unique<ReplaceableTextureManager>(gfx, *impl_->textures_);
    // AssetManager rides on top of TextureAssetManager — it borrows the
    // shared "white" handle as the placeholder texture for every Texture
    // slot until real bytes arrive. The .pkb / .pkfx parser also lives
    // on AssetManager, so corn-fx no longer needs its own content-
    // provider wire-up.
    impl_->assets_ = std::make_unique<AssetManager>(*impl_->textures_);
    impl_->assets_->SetGfxDevice(&gfx);
    // A gamma-space pipeline samples colour textures raw and multiplies them
    // against gamma geoset/light colours into a UNORM target; a linear one
    // needs them sRGB so the hardware linearises on sample. "Does the scene
    // land in the HDR target" is exactly that question, and the profile is
    // where it is answered — Wc3SdProfile already folds SceneHdrInSd into
    // SceneColorFormat, so this is an exact substitution for reading the mode
    // and the flag directly.
    //
    // Asking the profile is what makes it right for a product whose frame is
    // not WC3's. WowProfile is gamma with a UNORM target and no tonemap at any
    // setting, but the thumbnail grid turns SceneHdrInSd on globally while it
    // renders cells — so the old mode+flag read handed every texture acquired
    // during a browse an sRGB decode that WoW shading then never re-encodes.
    // The policy is latched per slot at acquire time, so those textures stayed
    // dark in the shared cache and followed the model into the document opened
    // from the browser.
    //
    // For WC3 SD the profile read alone is NOT enough: Wc3SdProfile folds
    // SceneHdrInSd in, so the same file legitimately wants both answers at
    // once — sRGB for the SD-HDR thumbnail still on screen, raw UNORM for the
    // gamma document just opened. That is resolved in AssetManager itself: the
    // latch is part of a Texture slot's identity, and the two contexts get two
    // slots instead of the browse poisoning the document's (or vice versa).
    impl_->assets_->SetGammaColorTexturesQuery([this]() {
        return Pipeline().LoadTimeProfile().SceneColorFormat() !=
               RenderPipeline::kHdrSceneFormat;
    });
    // The second acquire-time latch: which art tier the acquiring scene reads
    // through. Unlike gamma (a decode policy) this decides which BYTES the
    // fetch resolves, so the pump replays it per need — see
    // PumpAssetsViaProvider.
    impl_->assets_->SetArtTierQuery([this]() { return EffectiveArtTier(); });
    // Child-model parsing lives on ModelTemplateManager (so we don't drag
    // the MDX parser into AssetManager's translation unit). Install a
    // builder that wraps BuildFromBytes — AssetManager.ApplyPrepared
    // (Model) calls it with the pre-fetched bytes.
    impl_->assets_->SetChildModelBuilder(
        [this](const ContentRef& ref, std::span<const u8> bytes,
               std::string_view foundExt) -> std::shared_ptr<model::ModelTemplate> {
            // ModelTemplateManager keys its cache on a path string and picks
            // MDX vs MDL by extension, so it cannot cache an id-addressed
            // model at all. WC3 never hands it one; P9 generalises the cache
            // when M2 does.
            if (!ref.IsPath())
                return nullptr;
            // Child MDX templates build into the active scene's cache (the
            // scene whose load triggered the asset apply).
            return Scene().Templates().BuildFromBytes(ref.path, bytes, foundExt);
        });
    // The AssetManager is now live — wire every existing scene's SplatService to
    // it (new scenes pick it up at CreateScene). Without this, splats/ubersplats
    // in any scene but the one active at device-init draw the white placeholder.
    InitAllSceneServices();
    // When a .pkb arrives, walk its layer programs once and Acquire the
    // diffuse texture for each. The texture slots are tied to the parent
    // Particle slot via AddDependency, so they release together when the
    // emitter stops referencing the .pkb. Result: corn-fx textures load
    // eagerly the moment the .pkb commits, instead of waiting until the
    // emitter's first spawn.
    impl_->assets_->SetOnApplied([this](AssetManager::SlotId slot, AssetKind kind) {
        if (kind != AssetKind::Effect)
            return;
        const auto* model = impl_->assets_->ParticleAssetOf(slot);
        if (!model) return;
        auto paths = corn_effects::CornEffectsService::ExtractDiffuseTexturePaths(*model);
        for (const auto& p : paths) {
            const auto dep = impl_->assets_->Acquire(AssetKind::Texture, assets::kSoleSubKind, p);
            impl_->assets_->AddDependency(slot, dep);
        }
    });
}

void RenderService::ResetDeviceAssetManagers() {
    if (impl_->replaceables_)
        impl_->replaceables_->Shutdown();
    impl_->replaceables_.reset();
    if (impl_->assets_)
        impl_->assets_->SetGfxDevice(nullptr);
    impl_->assets_.reset();
    impl_->samplers_.reset();
    impl_->textures_.reset();
}

dnc::DncService& RenderService::EnsureDncService() {
    // Per active scene, bound to that scene's provider. A scene later pointed
    // at a different provider re-binds below, which drops the cached rig so
    // `Auto` re-resolves against the new mod chain.
    SceneServices& svc = *impl_->activeServices_;
    if (!svc.dnc) {
        svc.dnc = std::make_unique<dnc::DncService>(Scene().ActiveContentProvider());
        // A new scene opens looking like the one the host has been configuring
        // rather than snapping back to the built-in Lordaeron default — the
        // settings ini seeds the default scene, and tabs inherit from it.
        if (auto* seed = DefaultSceneDnc(); seed && seed != svc.dnc.get()) {
            svc.dnc->SetUnitMdl(seed->UnitMdlPath());
            svc.dnc->SetHoursPerDay(seed->GetHoursPerDay());
            svc.dnc->SetDayLengthSeconds(seed->GetDayLengthSeconds());
            svc.dnc->SetDawnHours(seed->GetDawnHours());
            svc.dnc->SetDuskHours(seed->GetDuskHours());
            svc.dnc->SetTimeOfDay(seed->GetTimeOfDay());
            svc.dnc->SetTodScale(seed->GetTodScale());
        }
    } else {
        svc.dnc->SetContentProvider(Scene().ActiveContentProvider());
    }
    // Which layer an unpinned path resolves from — the ACTIVE scene's own
    // effective mode, now that scenes carry it themselves.
    svc.dnc->SetArtTierPreference(EffectiveArtTier());
    return *svc.dnc;
}

dnc::DncService* RenderService::DefaultSceneDnc() {
    auto it = impl_->sceneServices_.find(impl_->defaultSceneId_);
    return it != impl_->sceneServices_.end() ? it->second->dnc.get() : nullptr;
}

shadow::ShadowService& RenderService::EnsureShadowService(gfx::IGFXDevice& gfx) {
    if (!impl_->shadowService_)
        impl_->shadowService_ = std::make_unique<shadow::ShadowService>(&gfx);
    return *impl_->shadowService_;
}

gtao::GtaoService& RenderService::EnsureGtaoService(gfx::IGFXDevice& gfx, gfx::GfxApi api) {
    if (!impl_->gtaoService_) {
        impl_->gtaoService_ = std::make_unique<gtao::GtaoService>();
        impl_->gtaoService_->Init(gfx, api);
    }
    return *impl_->gtaoService_;
}

#if WDX_ENABLE_M3
sc2::M3DeferredLightService& RenderService::EnsureM3DeferredLightService(gfx::IGFXDevice& gfx,
                                                                         gfx::GfxApi api) {
    if (!impl_->m3DeferredLightService_) {
        impl_->m3DeferredLightService_ = std::make_unique<sc2::M3DeferredLightService>();
        impl_->m3DeferredLightService_->Init(gfx, api);
    }
    return *impl_->m3DeferredLightService_;
}
#endif

dof::DofService& RenderService::EnsureDofService(gfx::IGFXDevice& gfx, gfx::GfxApi api,
                                                 bls::BlsShaderCache& cache,
                                                 gfx::BufferHandle spriteVb) {
    if (!impl_->dofService_) {
        impl_->dofService_ = std::make_unique<dof::DofService>();
        impl_->dofService_->Init(gfx, api, cache, spriteVb);
    }
    return *impl_->dofService_;
}

refraction::RefractionService& RenderService::EnsureRefractionService(gfx::IGFXDevice& gfx,
                                                                     gfx::GfxApi api) {
    if (!impl_->refractionService_) {
        impl_->refractionService_ = std::make_unique<refraction::RefractionService>();
        impl_->refractionService_->Init(gfx, api);
    }
    return *impl_->refractionService_;
}

distortion::DistortionService& RenderService::EnsureDistortionService(gfx::IGFXDevice& gfx,
                                                                     gfx::GfxApi api) {
    if (!impl_->distortionService_) {
        impl_->distortionService_ = std::make_unique<distortion::DistortionService>();
        impl_->distortionService_->Init(gfx, api);
    }
    return *impl_->distortionService_;
}

post_process::PostProcessService& RenderService::EnsurePostProcessService(
    gfx::IGFXDevice& gfx, gfx::GfxApi api, bls::BlsShaderCache& cache,
    gfx::BufferHandle spriteVb) {
    if (!impl_->postProcessService_) {
        impl_->postProcessService_ = std::make_unique<post_process::PostProcessService>();
        impl_->postProcessService_->Init(gfx, api, cache, spriteVb);
    }
    return *impl_->postProcessService_;
}

dear_imgui::ImGuiRenderer* RenderService::ImGui() {
#if WDX_ENABLE_IMGUI
    return impl_->imgui_.get();
#else
    return nullptr;
#endif
}

const dear_imgui::ImGuiRenderer* RenderService::ImGui() const {
#if WDX_ENABLE_IMGUI
    return impl_->imgui_.get();
#else
    return nullptr;
#endif
}

void RenderService::EnsureImGui(gfx::IGFXDevice& gfx, bls::BlsShaderCache& shaderCache,
                                gfx::Format rtvFormat, gfx::Format dsvFormat) {
#if WDX_ENABLE_IMGUI
    if (!impl_->imgui_) {
        impl_->imgui_ =
            std::make_unique<dear_imgui::ImGuiRenderer>(gfx, shaderCache, rtvFormat, dsvFormat);
    }
#else
    (void)gfx;
    (void)shaderCache;
    (void)rtvFormat;
    (void)dsvFormat;
#endif
}

void RenderService::ShutdownImGui() {
#if WDX_ENABLE_IMGUI
    impl_->imgui_.reset();
#endif
}

void RenderService::SwapSoundEmitter(std::unique_ptr<ISoundEmitter> emitter) {
    const f32 carry = impl_->soundEmitter_ ? impl_->soundEmitter_->GetVolume() : 1.0f;
    impl_->soundEmitter_ = emitter ? std::move(emitter) : MakeNullSoundEmitter();
    impl_->soundEmitter_->SetVolume(carry);
}

void RenderService::PumpAssetsViaProvider() {
#ifndef __EMSCRIPTEN__
    if (!impl_->assets_ || !impl_->activeScene_) return;
    auto* provider = Scene().ActiveContentProvider();
    if (!provider) return;

    // ReadFile via the provider, with a fallback for corn-fx-style
    // paths that bake a mod-name prefix into the path (e.g.
    // "_hd.w3mod/Textures/FX/Flare/HeroGlow_BW.tif"). The desktop
    // CASC path resolver doesn't flip those into TVFS-chain form, so
    // we strip the prefix and retry — same logic LoadCornEffectsTexture
    // used to carry. AssetManager normalises to lowercase + forward
    // slashes before storing, so we only need to check the lowercase
    // forms here.
    auto readWithModPrefixFallback =
        [provider](std::string_view path,
                   std::vector<u8>& outBytes,
                   std::string& outExt) -> bool {
        std::string p(path);
        std::string ext;
        if (auto bytes = provider->ReadFile(p, &ext); bytes && !bytes->empty()) {
            outBytes = std::move(*bytes);
            outExt   = std::move(ext);
            return true;
        }
        // Try stripping a known mod-name prefix. Order matches the
        // CASC TVFS stack, newest overlay first. `_de.w3mod/` joined the list
        // in 3.0.0, where it carries 1,232 of the install's 2,165 .pkb files
        // — without it every Definitive effect resolved its textures by
        // accident or not at all.
        static constexpr std::string_view kModPrefixes[] = {
            "_de.w3mod/",
            "_hd.w3mod/",
            "_deprecated.w3mod/",
        };
        for (auto prefix : kModPrefixes) {
            if (p.size() > prefix.size() &&
                std::string_view(p).substr(0, prefix.size()) == prefix) {
                std::string stripped = p.substr(prefix.size());
                ext.clear();
                if (auto bytes = provider->ReadFile(stripped, &ext);
                    bytes && !bytes->empty()) {
                    outBytes = std::move(*bytes);
                    outExt   = std::move(ext);
                    return true;
                }
            }
        }
        return false;
    };

    // Particle assets often reference .pkb but ship as .pkfx (or vice
    // versa) — try both extensions before giving up.
    auto readParticleWithAltExt =
        [&](std::string_view origPath, std::vector<u8>& outBytes,
            std::string& outExt) -> bool {
        if (readWithModPrefixFallback(origPath, outBytes, outExt))
            return true;
        // Swap to the other extension (if any) and retry.
        std::string p(origPath);
        auto dot = p.find_last_of('.');
        if (dot == std::string::npos)
            return false;
        std::string altExt = (p.substr(dot) == ".pkb") ? ".pkfx" : ".pkb";
        std::string alt = p.substr(0, dot) + altExt;
        if (readWithModPrefixFallback(alt, outBytes, outExt)) {
            if (outExt.empty()) outExt = altExt;
            return true;
        }
        return false;
    };

    impl_->assets_->DrainNeedsEx([&](const assets::AssetManager::NeedInfo& need) {
        const assets::AssetKind kind = need.kind;
        const assets::AssetSubKind subKind = need.subKind;
        const ContentRef& ref = need.ref;
        // Fetch under the tier the acquiring scene read through. The pump
        // serves EVERY scene's pending needs through whichever scene happens
        // to be active, so the need's own tier — not the ambient one — is
        // what decides which bytes this path resolves to.
        if (provider->ArtTier() != need.tier)
            provider->SetArtTier(need.tier);
        std::vector<u8> bytes;
        std::string ext;
        if (ref.IsFileId()) {
            // A fileDataID resolves in the root manifest or not at all —
            // none of the fallbacks below mean anything for one. There is
            // no mod prefix to strip and no extension to swap, because
            // there is no name.
            auto data = provider->ReadFile(ref, &ext);
            if (!data || data->empty())
                return;
            bytes = std::move(*data);
        } else if (kind == assets::AssetKind::Texture) {
            if (!readWithModPrefixFallback(ref.path, bytes, ext))
                return;
        } else if (kind == assets::AssetKind::Effect) {
            if (!readParticleWithAltExt(ref.path, bytes, ext))
                return;
        } else if (kind == assets::AssetKind::Model) {
            // MDX child reads ride the same content-provider path the
            // top-level SpawnUnit uses — alt-extension synonyms (.mdx
            // ↔ .mdl) are already handled inside FileContentProvider.
            if (!readWithModPrefixFallback(ref.path, bytes, ext))
                return;
        } else {
            return;
        }
        impl_->assets_->ApplyPreparedFor(
            kind, subKind, ref, need.tier,
            std::span<const u8>(bytes.data(), bytes.size()),
            ext);
    });
    // Re-impose the active scene's tier after serving mixed-tier needs, so
    // reads outside the pump see the state SetActiveScene armed.
    provider->SetArtTier(EffectiveArtTier());
    impl_->assets_->CommitPrepared();
#endif
}

} // namespace whiteout::flakes::renderer
