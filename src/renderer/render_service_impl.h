#pragma once

#include "render_service.h"

#include "camera.h"
#include "animation/animation.h"
#include "frame_ticker.h"
#include "model/model_loader.h"
#include "render_pipeline.h"
#include "particle.h"
#include "particle/particle_service.h"
#include "particle/splat_service.h"
#include "dnc/dnc_service.h"
#include "shadow/shadow_service.h"
#include "effects/ribbon.h"
#include "sound_emitter.h"
#include "effects/spn_spawner.h"
#include "model/model_types.h"
#include "model/model_instance.h"
#include "model/actor_manager.h"
#include "scene_manager.h"
#include "model/model_source.h"
#include "content_provider.h"
#include "file_content_provider.h"
#include "render_settings.h"
#include "render_target.h"

#include <atomic>
#include <memory>
#include <unordered_map>

namespace whiteout::flakes::renderer::assets {
    class SamplerAssetManager;
    class TextureAssetManager;
    class ReplaceableTextureManager;
}
namespace whiteout::flakes::renderer::debug { class DebugRenderer; }

namespace whiteout::flakes::renderer {

struct RenderService::Impl {
    // ---- Scene + subsystems ----
    SceneManager*                                 scene_ = nullptr;
    particle::ParticleService                     particleService_;
    particle::SplatService                        splatService_;
    std::unique_ptr<dnc::DncService>              dncService_;
    std::unique_ptr<shadow::ShadowService>        shadowService_;
    std::unique_ptr<effects::SpnSpawner>          spnSpawner_;
    std::unique_ptr<FrameTicker>                  ticker_;
    std::unique_ptr<model::ModelLoader>           loader_;
    std::unique_ptr<RenderPipeline>               pipeline_;
    std::unique_ptr<ISoundEmitter>                soundEmitter_;

    // ---- App-tunable knobs ----
    RenderSettings                                settings_;

    // ---- Asset managers ----
    std::unique_ptr<assets::SamplerAssetManager>       samplers_;
    std::unique_ptr<assets::TextureAssetManager>       textures_;
    std::unique_ptr<assets::ReplaceableTextureManager> replaceables_;
    std::unique_ptr<debug::DebugRenderer>              debug_;
};

}
