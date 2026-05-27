#include "debug/debug_renderer.h"
#include "renderer/assets/asset_manager.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/assets/sampler_asset_manager.h"
#include "renderer/assets/texture_asset_manager.h"
#include "renderer/bls/bls_shader_cache.h"
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
    impl_->scene_ = &scene;
    impl_->debug_ = std::make_unique<DebugRenderer>(*this);
    impl_->pipeline_ = std::make_unique<RenderPipeline>(*this);
    impl_->spnSpawner_ = std::make_unique<SpnSpawner>(*this);
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

// ---- Out-of-line accessors (bodies live here so Impl is complete) ----
SceneManager& RenderService::Scene() {
    return *impl_->scene_;
}
const SceneManager& RenderService::Scene() const {
    return *impl_->scene_;
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
    return impl_->particleService_;
}
particle::SplatService& RenderService::Splats() {
    return impl_->splatService_;
}
corn_effects::CornEffectsService& RenderService::CornEffects() {
    return impl_->cornEffectsService_;
}
SpnSpawner& RenderService::Spn() {
    return *impl_->spnSpawner_;
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
    ActorEvalContext ctx;
    ctx.camPos = impl_->scene_->Camera().GetSource();
    ctx.sceneAnimationTimeMs = impl_->scene_->GetAnimationTime();
    ctx.fireEvents = impl_->settings_.ShowEvents();
    ctx.scene = impl_->scene_;
    ctx.particles = &impl_->particleService_;
    ctx.splats = &impl_->splatService_;
    ctx.cornEffects = &impl_->cornEffectsService_;
    ctx.spnSpawner = impl_->spnSpawner_.get();
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
            return impl_->scene_->Templates().BuildFromBytes(
                std::string(path), bytes, foundExt);
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
            std::make_unique<dnc::DncService>(impl_->scene_->ActiveContentProvider());
    return *impl_->dncService_;
}

shadow::ShadowService& RenderService::EnsureShadowService(gfx::IGFXDevice& gfx) {
    if (!impl_->shadowService_)
        impl_->shadowService_ = std::make_unique<shadow::ShadowService>(&gfx);
    return *impl_->shadowService_;
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
    if (!impl_->assets_ || !impl_->scene_) return;
    auto* provider = impl_->scene_->ActiveContentProvider();
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
