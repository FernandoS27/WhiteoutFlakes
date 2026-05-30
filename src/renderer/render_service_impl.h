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
#include <memory>
#include <unordered_map>

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

struct RenderService::Impl {
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

    // ---- Scene ----
    SceneManager* scene_ = nullptr;

    // ---- Asset managers (destroyed LAST) ----
    std::unique_ptr<assets::SamplerAssetManager> samplers_;
    std::unique_ptr<assets::TextureAssetManager> textures_;
    std::unique_ptr<assets::ReplaceableTextureManager> replaceables_;
    std::unique_ptr<assets::AssetManager> assets_;

    // ---- App-tunable knobs ----
    RenderSettings settings_;

    // ---- Background services that don't hold gfx state ----
    particle::ParticleService particleService_;
    particle::SplatService splatService_;

    // ---- Gfx device + subsystems that hold gfx handles ----
    std::unique_ptr<RenderPipeline> pipeline_;
    std::unique_ptr<dnc::DncService> dncService_;
    std::unique_ptr<shadow::ShadowService> shadowService_;
    std::unique_ptr<effects::SpnSpawner> spnSpawner_;
    std::unique_ptr<FrameTicker> ticker_;
    std::unique_ptr<model::ModelLoader> loader_;
    std::unique_ptr<ISoundEmitter> soundEmitter_;
    std::unique_ptr<debug::DebugRenderer> debug_;

    // ---- Subsystems that must die BEFORE the gfx device + assets ----
    // CornEffectsService is declared LAST so it's destroyed FIRST in
    // reverse order. Its emitters' RuntimeBundles hold gfx handles +
    // AssetManager slot refs that they release on teardown — both
    // must still be alive when ~CornEffectsService runs.
    corn_effects::CornEffectsService cornEffectsService_;

#if WDX_ENABLE_IMGUI
    // ImGui adapter is built lazily once the gfx device + BLS shader cache
    // are alive (RenderPipeline::InitBlsShaders calls EnsureImGui). The
    // host owns the ImGui context — the adapter only manages the GPU side.
    std::unique_ptr<dear_imgui::ImGuiRenderer> imgui_;
#endif
};

} // namespace whiteout::flakes::renderer
