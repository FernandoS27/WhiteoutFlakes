#pragma once

#include "whiteout/flakes/types.h"
#include "types.h"
#include "gfx/gfx.h"
#include "whiteout/flakes/util/replaceable_paths.h"

#include "animation/actor_eval_context.h"
#include "model/model_instance.h"
#include "whiteout/flakes/model_types.h"
#include "render_settings.h"
#include "whiteout/flakes/sound_emitter.h"
#include "render_target.h"
#include "scene_manager.h"
#include "whiteout/flakes/model_source.h"

// Tool code (and a few subsystem .cpps) historically reaches DncService /
// ShadowService through this header. The renderer's own private state
// (Pimpl) doesn't need the full types here, but downstream consumers do —
// keep these includes so the public API of RenderService (which returns
// pointers to these services) is usable without each tool adding its own
// transitive include.
#include "dnc/dnc_service.h"
#include "shadow/shadow_service.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout::flakes::renderer {

class SceneManager;
class FrameTicker;
class RenderPipeline;

namespace assets {
    class SamplerAssetManager;
    class TextureAssetManager;
    class ReplaceableTextureManager;
}
namespace model {
    class ModelTemplateManager;
    struct ModelTemplate;
    class ModelLoader;
}
namespace debug { class DebugRenderer; }
namespace effects { class SpnSpawner; }
namespace particle {
    class ParticleService;
    class SplatService;
}
namespace corn_effects { class CornEffectsService; }

struct LineVertex {
    Vector3f position;
    Vector4f color;
};

// Pure helpers shared with render_detail.cpp / shadow_pass.cpp.
// Free-standing because they touch no RenderService state.
inline bool GeosetPassesLod(u32 geosetLod, i32 selectedLod) {
    return geosetLod == 0xFFFFFFFFu || (i32)geosetLod == selectedLod;
}

inline i32 GeosetRenderOrder(i32 filterMode) {
    switch (filterMode) {
        case model::FILTER_NONE:        return 1;
        case model::FILTER_TRANSPARENT: return 2;
        case model::FILTER_BLEND:       return 3;
        default:                        return 4;
    }
}

class RenderService {
public:
    explicit RenderService(SceneManager& scene);
    ~RenderService();

    // ---- GPU pipeline (device lifecycle, render targets, frame loop, stats) ----
    // Tools call service.Pipeline().InitDevice / RenderFrame / Present / etc.
    RenderPipeline&       Pipeline();
    const RenderPipeline& Pipeline() const;

    // ---- Model loading & state ----
    // Loader() owns model creation, staging, and GPU upload. Hosts compose
    // multi-actor scenes via Loader().SpawnUnit / SpawnUnitFromSource /
    // SpawnChild / DestroyActor / Clear. See model_loader.h.
    model::ModelLoader&       Loader();
    const model::ModelLoader& Loader() const;

    // Build the per-frame context an Actor needs to call EvaluateAndApply.
    // The application (Max plugin / basic_viewer) hands this to one or more
    // actors; multiple actors can be evaluated independently.
    animation::ActorEvalContext MakeActorEvalContext();

    // ---- Scene & asset accessors ----
    SceneManager&                       Scene();
    const SceneManager&                 Scene() const;
    assets::TextureAssetManager&        Textures();
    assets::SamplerAssetManager&        Samplers();
    assets::ReplaceableTextureManager&  Replaceables();
    debug::DebugRenderer&               Debug();
    dnc::DncService*                    GetDncService();
    const dnc::DncService*              GetDncService() const;
    shadow::ShadowService*              GetShadowService();
    const shadow::ShadowService*        GetShadowService() const;

    // ---- Per-actor effect services ----
    particle::ParticleService&   Particles();
    particle::SplatService&      Splats();
    corn_effects::CornEffectsService&     CornEffects();
    effects::SpnSpawner&         Spn();

    // ---- App-tunable knobs ----
    RenderSettings&              Settings();
    const RenderSettings&        Settings() const;

    // ---- Per-frame scene update orchestrator ----
    FrameTicker&                 Ticker();

    // ---- Sound ----
    // Volume / mute lives on the emitter itself: callers use Sound().SetVolume(v).
    // SwapSoundEmitter installs a different backend (e.g., basic_viewer plugs in
    // WindowsSoundEmitter at startup) and carries over the previous volume.
    ISoundEmitter&       Sound();
    const ISoundEmitter& Sound() const;
    void                 SwapSoundEmitter(std::unique_ptr<ISoundEmitter> emitter);

    // Null-tolerant texture cache probe. The template manager installs a
    // texture-cache lambda during RenderService construction — before
    // InitDevice has created the texture manager — so the loader's probe
    // path needs a safe accessor that handles the pre-init window.
    bool HasCachedTexture(std::string_view key) const;

    // Load (or fetch from cache) a corn-fx-referenced diffuse texture.
    // The .pkb's renderer property block carries paths that aren't part
    // of the model's MDX texture list, so the corn fx backend's resolver
    // can't just hit the existing shared cache — this helper reads the
    // file via the active content provider, decodes it (BLP/DDS/TGA),
    // and inserts under the normalised path key. Subsequent lookups via
    // TextureAssetManager::LookupShared then hit. Returns Invalid on
    // miss (no provider, file not found, or decode failure).
    gfx::TextureHandle LoadCornEffectsTexture(std::string_view path);

    // ---- Pipeline-coordination hooks ----
    // RenderService owns the asset managers; RenderPipeline drives their
    // lifecycle as it brings the GPU device up and down.
    bool HasDeviceAssetManagers() const;
    void CreateDeviceAssetManagers(gfx::IGFXDevice& gfx);
    void ResetDeviceAssetManagers();
    dnc::DncService&    EnsureDncService();
    shadow::ShadowService& EnsureShadowService(gfx::IGFXDevice& gfx);

private:
    // ---- Pimpl: all instance state lives here. ----
    // Subsystems reach RenderService state through the public accessors above
    // (Scene/Textures/Samplers/Replaceables/Particles/Splats/Spn/Settings/
    //  Pipeline/Loader/Ticker/Sound). No friend declarations needed.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
