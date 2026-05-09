#include "renderer/render_service.h"
#include "renderer/render_service_impl.h"
#include "renderer/render_pipeline.h"
#include "debug/debug_renderer.h"
#include "renderer/scene_manager.h"
#include "renderer/model/model_template_manager.h"
#include "renderer/assets/sampler_asset_manager.h"
#include "renderer/assets/texture_asset_manager.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/dnc/dnc_service.h"
#include "renderer/shadow/shadow_service.h"

namespace whiteout::flakes::renderer {

using namespace ::whiteout::flakes::renderer::model;
using namespace ::whiteout::flakes::renderer::animation;
using namespace ::whiteout::flakes::renderer::effects;
using namespace ::whiteout::flakes::renderer::assets;
using namespace ::whiteout::flakes::renderer::debug;
using namespace ::whiteout::flakes::renderer::particle;
using namespace ::whiteout::flakes::renderer::shadow;
using namespace ::whiteout::flakes::renderer::dnc;

RenderService::RenderService(SceneManager& scene)
    : impl_(std::make_unique<Impl>()) {
    impl_->scene_       = &scene;
    impl_->debug_       = std::make_unique<DebugRenderer>(*this);
    impl_->pipeline_    = std::make_unique<RenderPipeline>(*this);
    impl_->spnSpawner_  = std::make_unique<SpnSpawner>(*this);
    impl_->ticker_      = std::make_unique<FrameTicker>(*this);
    // ModelLoader must be constructed BEFORE the texture-cache lambda below
    // is installed, since the lambda dispatches into impl_->loader_.
    impl_->loader_      = std::make_unique<ModelLoader>(*this);
    impl_->soundEmitter_= MakeNullSoundEmitter();
    impl_->soundEmitter_->SetVolume(0.2f);
    impl_->scene_->Templates().SetTextureCacheQuery(
        [this](std::string_view k) { return impl_->loader_->IsTextureCached(k); });
}

RenderService::~RenderService() = default;

// ---- Out-of-line accessors (bodies live here so Impl is complete) ----
SceneManager&       RenderService::Scene()               { return *impl_->scene_; }
const SceneManager& RenderService::Scene() const         { return *impl_->scene_; }
TextureAssetManager& RenderService::Textures()           { return *impl_->textures_; }
SamplerAssetManager& RenderService::Samplers()           { return *impl_->samplers_; }
ReplaceableTextureManager& RenderService::Replaceables() { return *impl_->replaceables_; }
DebugRenderer& RenderService::Debug()                    { return *impl_->debug_; }

particle::ParticleService& RenderService::Particles()    { return impl_->particleService_; }
particle::SplatService&    RenderService::Splats()       { return impl_->splatService_; }
SpnSpawner&                RenderService::Spn()          { return *impl_->spnSpawner_; }

dnc::DncService*       RenderService::GetDncService()       { return impl_->dncService_.get(); }
const dnc::DncService* RenderService::GetDncService() const { return impl_->dncService_.get(); }
shadow::ShadowService*       RenderService::GetShadowService()       { return impl_->shadowService_.get(); }
const shadow::ShadowService* RenderService::GetShadowService() const { return impl_->shadowService_.get(); }

RenderSettings&       RenderService::Settings()       { return impl_->settings_; }
const RenderSettings& RenderService::Settings() const { return impl_->settings_; }

FrameTicker&          RenderService::Ticker()         { return *impl_->ticker_; }

ModelLoader&          RenderService::Loader()         { return *impl_->loader_; }
const ModelLoader&    RenderService::Loader() const   { return *impl_->loader_; }

RenderPipeline&       RenderService::Pipeline()       { return *impl_->pipeline_; }
const RenderPipeline& RenderService::Pipeline() const { return *impl_->pipeline_; }

ISoundEmitter&       RenderService::Sound()       { return *impl_->soundEmitter_; }
const ISoundEmitter& RenderService::Sound() const { return *impl_->soundEmitter_; }

ActorEvalContext RenderService::MakeActorEvalContext() {
    ActorEvalContext ctx;
    ctx.camPos               = impl_->scene_->Camera().GetSource();
    ctx.sceneAnimationTimeMs = impl_->scene_->GetAnimationTime();
    ctx.fireEvents           = impl_->settings_.ShowEvents();
    ctx.scene                = impl_->scene_;
    ctx.particles            = &impl_->particleService_;
    ctx.splats               = &impl_->splatService_;
    ctx.spnSpawner           = impl_->spnSpawner_.get();
    ctx.sound                = impl_->soundEmitter_.get();
    return ctx;
}

bool RenderService::HasCachedTexture(std::string_view key) const {
    return impl_->textures_ && impl_->textures_->IsCachedShared(key);
}

bool RenderService::HasDeviceAssetManagers() const {
    return impl_->samplers_ && impl_->textures_ && impl_->replaceables_;
}

void RenderService::CreateDeviceAssetManagers(gfx::IGFXDevice& gfx) {
    impl_->samplers_     = std::make_unique<SamplerAssetManager>(gfx);
    impl_->textures_     = std::make_unique<TextureAssetManager>(gfx);
    impl_->replaceables_ = std::make_unique<ReplaceableTextureManager>(gfx, *impl_->textures_);
}

void RenderService::ResetDeviceAssetManagers() {
    if (impl_->replaceables_) impl_->replaceables_->Shutdown();
    impl_->replaceables_.reset();
    impl_->samplers_.reset();
    impl_->textures_.reset();
}

dnc::DncService& RenderService::EnsureDncService() {
    if (!impl_->dncService_)
        impl_->dncService_ = std::make_unique<dnc::DncService>(impl_->scene_->ActiveContentProvider());
    return *impl_->dncService_;
}

shadow::ShadowService& RenderService::EnsureShadowService(gfx::IGFXDevice& gfx) {
    if (!impl_->shadowService_)
        impl_->shadowService_ = std::make_unique<shadow::ShadowService>(&gfx);
    return *impl_->shadowService_;
}

void RenderService::SwapSoundEmitter(std::unique_ptr<ISoundEmitter> emitter) {
    const f32 carry = impl_->soundEmitter_ ? impl_->soundEmitter_->GetVolume() : 1.0f;
    impl_->soundEmitter_ = emitter ? std::move(emitter) : MakeNullSoundEmitter();
    impl_->soundEmitter_->SetVolume(carry);
}

}
