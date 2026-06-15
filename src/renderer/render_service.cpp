#include "debug/debug_renderer.h"
#include "renderer/assets/asset_manager.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/assets/sampler_asset_manager.h"
#include "renderer/assets/texture_asset_manager.h"
#include "renderer/bls/bls_shader_cache.h"
#include "renderer/corn_effects/corn_effects_service.h"
#include "renderer/dnc/dnc_service.h"
#include "renderer/imgui/imgui_renderer.h"
#include "renderer/model/model_source_utils.h"
#include "renderer/model/model_template_manager.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/render_service_impl.h"
#include "renderer/scene_manager.h"
#include "renderer/shadow/shadow_service.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/util/texture_image_usage.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace whiteout::flakes::renderer {

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

void RenderService::SetCornBackendInitApplier(
    std::function<void(corn_effects::CornEffectsService&)> fn) {
    impl_->cornInitApplier_ = std::move(fn);
    InitAllSceneServices();
}

void RenderService::InitSceneServices(SceneServices& svc) {
    // Each step is idempotent and self-guards on its input being ready, so this
    // can run any number of times as inputs (assets, corn backend) come online.
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
DebugRenderer& RenderService::Debug() {
    return *impl_->debug_;
}

particle::ParticleService& RenderService::Particles() {
    return impl_->activeServices_->particles;
}
particle::SplatService& RenderService::Splats() {
    return impl_->activeServices_->splats;
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
    return impl_->dncService_.get();
}
const dnc::DncService* RenderService::GetDncService() const {
    return impl_->dncService_.get();
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
    ctx.sceneAnimationTimeMs = scene->GetAnimationTime();
    ctx.fireEvents = impl_->settings_.ShowEvents();
    ctx.scene = scene;
    ctx.particles = &svc->particles;
    ctx.splats = &svc->splats;
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
    // ChildModel parsing lives on ModelTemplateManager (so we don't drag
    // the MDX parser into AssetManager's translation unit). Install a
    // builder that wraps BuildFromBytes — AssetManager.ApplyPrepared
    // (ChildModel) calls it with the pre-fetched bytes.
    impl_->assets_->SetChildModelBuilder(
        [this](std::string_view path, std::span<const u8> bytes,
               std::string_view foundExt) -> std::shared_ptr<model::ModelTemplate> {
            // Child MDX templates build into the active scene's cache (the
            // scene whose load triggered the asset apply).
            return Scene().Templates().BuildFromBytes(std::string(path), bytes, foundExt);
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
        if (kind != AssetKind::Particle)
            return;
        const auto* model = impl_->assets_->ParticleAssetOf(slot);
        if (!model) return;
        auto paths = corn_effects::CornEffectsService::ExtractDiffuseTexturePaths(*model);
        for (const auto& p : paths) {
            const auto dep = impl_->assets_->Acquire(AssetKind::Texture, p);
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
    if (!impl_->dncService_)
        impl_->dncService_ =
            std::make_unique<dnc::DncService>(Scene().ActiveContentProvider());
    return *impl_->dncService_;
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

dof::DofService& RenderService::EnsureDofService(gfx::IGFXDevice& gfx, gfx::GfxApi api,
                                                 bls::BlsShaderCache& cache,
                                                 gfx::BufferHandle spriteVb) {
    if (!impl_->dofService_) {
        impl_->dofService_ = std::make_unique<dof::DofService>();
        impl_->dofService_->Init(gfx, api, cache, spriteVb);
    }
    return *impl_->dofService_;
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
        // CASC TVFS stack (_hd overrides win first, then _deprecated).
        static constexpr std::string_view kModPrefixes[] = {
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

    impl_->assets_->DrainNeeds([&](assets::AssetKind kind, std::string_view path) {
        std::vector<u8> bytes;
        std::string ext;
        if (kind == assets::AssetKind::Texture) {
            if (!readWithModPrefixFallback(path, bytes, ext))
                return;
        } else if (kind == assets::AssetKind::Particle) {
            if (!readParticleWithAltExt(path, bytes, ext))
                return;
        } else if (kind == assets::AssetKind::ChildModel) {
            // MDX child reads ride the same content-provider path the
            // top-level SpawnUnit uses — alt-extension synonyms (.mdx
            // ↔ .mdl) are already handled inside FileContentProvider.
            if (!readWithModPrefixFallback(path, bytes, ext))
                return;
        } else {
            return;
        }
        impl_->assets_->ApplyPrepared(
            kind, path,
            std::span<const u8>(bytes.data(), bytes.size()),
            ext);
    });
    impl_->assets_->CommitPrepared();
#endif
}

} // namespace whiteout::flakes::renderer
