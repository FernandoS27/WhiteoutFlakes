#pragma once

#include "render_service.h"

#include "animation/animation.h"
#include "camera.h"
#include "corn_effects/corn_effects_service.h"
#include "dnc/dnc_service.h"
#include "effects/ribbon.h"
#include "effects/spn_spawner.h"
#include "file_content_provider.h"
#include "frame_ticker.h"
#include "imgui/imgui_renderer.h"
#include "model/actor_manager.h"
#include "model/model_instance.h"
#include "model/model_loader.h"
#include "particle.h"
#include "particle/particle_service.h"
#include "particle/splat_service.h"
#include "post_process/post_process_service.h"
#include "render_pipeline.h"
#include "render_settings.h"
#include "render_target.h"
#include "scene_manager.h"
#include "shadow/shadow_service.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/sound_emitter.h"

#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::renderer::assets {
class AssetManager;
class SamplerAssetManager;
class TextureAssetManager;
class ReplaceableTextureManager;
} // namespace whiteout::flakes::renderer::assets
namespace whiteout::flakes::renderer::debug {
class DebugRenderer;
}

namespace whiteout::flakes::renderer {

// The per-scene effect services. One bundle per scene so two scenes can hold
// independent live particle/corn/splat/SPN state at once (e.g. a grid of live
// model/effect thumbnails). CornEffectsService is declared LAST so it destroys
// FIRST — its emitters' RuntimeBundles hold gfx + AssetManager refs that must
// still be alive on teardown (same contract the single global service had).
struct SceneServices {
    explicit SceneServices(RenderService& rs)
        : spn(std::make_unique<effects::SpnSpawner>(rs)) {}

    particle::ParticleService particles;
    particle::SplatService splats;
    std::unique_ptr<effects::SpnSpawner> spn;
    corn_effects::CornEffectsService corn; // declared last → destroyed first
};

struct RenderService::Impl {
    // Actors must not outlive the AssetManager. `Actor` → `RenderModel` →
    // `TextureAssetManager::ModelScope` releases its asset slots from its
    // destructor, and the scenes holding those actors are destroyed after
    // `assets_` — the default scene is owned by RendererImpl (destroyed after
    // the whole service) and `ownedScenes_` is declared before the asset
    // managers, so both lose that race. Clearing every scene's actors here,
    // before any member destructs, is the one place that covers both.
    ~Impl();

    // ---- Destruction-order contract (members destroy in REVERSE
    //      declaration order, so LATER-declared = destroyed EARLIER) ----
    //
    //   1. CornEffectsEmitter dtors run when cornEffectsService_ dies.
    //      Each emitter's RuntimeBundle holds a CornEffectsGfxBackend
    //      that calls into the gfx device + AssetManager during
    //      destruction. ⇒ cornEffectsService_ must die BEFORE
    //      pipeline_ (gfx) and BEFORE assets_.
    //   2. Other subsystems (debug renderer, ImGui adapter) also need
    //      the gfx device alive on teardown. ⇒ pipeline_ must die
    //      AFTER imgui_/debug_/sound but BEFORE the asset managers.
    //   3. The asset managers must outlive every consumer (cornflakes,
    //      model instances, ImGui). ⇒ asset managers declared earliest
    //      among the "lives during gfx" members, so they're destroyed
    //      last among them.
    //
    // Symptom when violated: ~CornEffectsEmitter → assets_.Release on
    // a destroyed AssetManager hits a dead std::mutex (EINVAL →
    // std::system_error → std::terminate → abort).

    // ---- Scene registry (multi-scene) ----
    // `scenes_` maps every SceneId (default + created) to its SceneManager
    // (borrowed default, or owned in ownedScenes_). `activeScene_`/
    // `activeServices_` are published by RenderViewport / FrameTicker and read
    // by Scene()/Particles()/etc.; they default to the default scene.
    std::unordered_map<SceneId, SceneManager*> scenes_;
    std::vector<std::unique_ptr<SceneManager>> ownedScenes_;
    SceneId nextSceneId_ = 1;
    SceneId defaultSceneId_ = 0;
    SceneId activeSceneId_ = 0;
    SceneManager* activeScene_ = nullptr;
    SceneServices* activeServices_ = nullptr;
    // Applier the pipeline installs to wire each scene's corn-fx GPU backend.
    std::function<void(corn_effects::CornEffectsService&)> cornInitApplier_;

    // ---- Asset managers (destroyed LAST) ----
    // `assets_` FIRST, so it is destroyed LAST. TextureAssetManager's
    // ModelScope::Clear calls assets_->Release from its own destructor; with
    // assets_ declared after it, reverse-order destruction killed the
    // AssetManager first and the Release walked a freed hash map. That is the
    // exact symptom described at the top of this struct — it applied to the
    // asset managers among themselves, not just to their consumers.
    std::unique_ptr<assets::AssetManager> assets_;
    std::unique_ptr<assets::SamplerAssetManager> samplers_;
    std::unique_ptr<assets::TextureAssetManager> textures_;
    std::unique_ptr<assets::ReplaceableTextureManager> replaceables_;

    // ---- App-tunable knobs ----
    RenderSettings settings_;

    // ---- Gfx device + subsystems that hold gfx handles ----
    std::unique_ptr<RenderPipeline> pipeline_;
    std::unique_ptr<dnc::DncService> dncService_;
    std::unique_ptr<shadow::ShadowService> shadowService_;
    std::unique_ptr<gtao::GtaoService> gtaoService_;
    std::unique_ptr<dof::DofService> dofService_;
    std::unique_ptr<post_process::PostProcessService> postProcessService_;
    std::unique_ptr<FrameTicker> ticker_;
    std::unique_ptr<model::ModelLoader> loader_;
    std::unique_ptr<ISoundEmitter> soundEmitter_;
    std::unique_ptr<debug::DebugRenderer> debug_;

    // ---- Per-scene effect services (must die BEFORE the gfx device + assets) ----
    // Each scene's SceneServices holds a CornEffectsService whose emitters hold
    // gfx handles + AssetManager slot refs released on teardown — both must
    // still be alive when these run. Declared LAST so the whole map is destroyed
    // FIRST in reverse member order. GPU teardown paths additionally Clear()/
    // ReleaseGpuResources() every scene's corn service explicitly (via
    // ForEachCornService) before the device/assets go away.
    std::unordered_map<SceneId, std::unique_ptr<SceneServices>> sceneServices_;

#if WDX_ENABLE_IMGUI
    // ImGui adapter is built lazily once the gfx device + BLS shader cache
    // are alive (RenderPipeline::InitBlsShaders calls EnsureImGui). The
    // host owns the ImGui context — the adapter only manages the GPU side.
    std::unique_ptr<dear_imgui::ImGuiRenderer> imgui_;
#endif
};

} // namespace whiteout::flakes::renderer
