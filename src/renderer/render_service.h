#pragma once

#include "gfx/gfx.h"
#include "types.h"
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/replaceable_paths.h"

#include "animation/actor_eval_context.h"
#include "model/model_instance.h"
#include "render_settings.h"
#include "render_target.h"
#include "scene_manager.h"
#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/sound_emitter.h"

// Tool code (and a few subsystem .cpps) historically reaches DncService /
// ShadowService through this header. The renderer's own private state
// (Pimpl) doesn't need the full types here, but downstream consumers do —
// keep these includes so the public API of RenderService (which returns
// pointers to these services) is usable without each tool adding its own
// transitive include.
#include "dnc/dnc_service.h"
#include "imgui/imgui_renderer.h"
#include "shadow/shadow_service.h"
#include "dof/dof_service.h"
#include "gtao/gtao_service.h"
#include "post_process/post_process_service.h"

#include <memory>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout::flakes::renderer {

class SceneManager;
class FrameTicker;
class RenderPipeline;
struct SceneServices;

// Identifies one scene within a RenderService. A RenderService always has a
// default scene (DefaultSceneId); additional scenes are created via
// CreateScene() and rendered into their own viewports/targets. 0 is reserved
// "none/default".
using SceneId = u32;

namespace assets {
class AssetManager;
class SamplerAssetManager;
class TextureAssetManager;
class ReplaceableTextureManager;
} // namespace assets
namespace model {
class ModelTemplateManager;
struct ModelTemplate;
class ModelLoader;
} // namespace model
namespace debug {
class DebugRenderer;
}
namespace bls {
class BlsShaderCache;
} // namespace bls
namespace effects {
class SpnSpawner;
}
namespace particle {
class ParticleService;
class SplatService;
} // namespace particle
namespace corn_effects {
class CornEffectsService;
}

struct LineVertex {
    Vector3f position;
    Vector4f color;
};

// Pure helpers shared with render_detail.cpp / shadow_pass.cpp.
// Free-standing because they touch no RenderService state.
inline bool GeosetPassesLod(u32 geosetLod, i32 selectedLod) {
    return geosetLod == 0xFFFFFFFFu || (i32)geosetLod == selectedLod;
}

class RenderService {
public:
    explicit RenderService(SceneManager& scene);
    ~RenderService();

    // ---- GPU pipeline (device lifecycle, render targets, frame loop, stats) ----
    // Tools call service.Pipeline().InitDevice / RenderFrame / Present / etc.
    RenderPipeline& Pipeline();
    const RenderPipeline& Pipeline() const;

    // ---- Model loading & state ----
    // Loader() owns model creation, staging, and GPU upload. Hosts compose
    // multi-actor scenes via Loader().SpawnUnit / SpawnUnitFromSource /
    // SpawnChild / DestroyActor / Clear. See model_loader.h.
    model::ModelLoader& Loader();
    const model::ModelLoader& Loader() const;

    // Build the per-frame context an Actor needs to call EvaluateAndApply.
    // The application (Max plugin / basic_viewer) hands this to one or more
    // actors; multiple actors can be evaluated independently.
    animation::ActorEvalContext MakeActorEvalContext();

    // ---- Scene registry (multi-scene) ----
    // A RenderService hosts N independent scenes, each with its own actors,
    // clock, camera set, and effect services (particles/splats/corn/SPN). The
    // ctor registers the passed SceneManager as the default scene; additional
    // scenes are created with CreateScene() (owned by the RenderService).
    //
    // The pass pipeline / ticker / loader read the *active* scene via Scene()
    // and the effect-service accessors below — RenderViewport publishes the
    // viewport's scene as active for the duration of the render, and the ticker
    // publishes the scene it is ticking. Outside those scopes the active scene
    // is the default scene.
    SceneId CreateScene();
    void DestroyScene(SceneId id);
    SceneManager& SceneAt(SceneId id);   // unknown id ⇒ default scene
    SceneManager& DefaultScene();
    SceneId DefaultSceneId() const;
    // Publish/clear the active scene (used by RenderViewport + FrameTicker).
    // SetActiveScene(0) restores the default scene.
    void SetActiveScene(SceneId id);
    void SetActiveScene(SceneManager& scene); // by-reference overload
    SceneManager& ActiveScene();

    // Tick every scene's actors/clock/effect services once this frame (the
    // multi-scene per-frame entry; single-scene hosts can keep calling
    // Ticker().Tick(dt)).
    void TickScenes(f32 dt);

    // ---- Scene & asset accessors ----
    // Scene() returns the ACTIVE scene (default outside a render/tick scope).
    SceneManager& Scene();
    const SceneManager& Scene() const;
    assets::TextureAssetManager& Textures();
    assets::SamplerAssetManager& Samplers();
    assets::ReplaceableTextureManager& Replaceables();
    /// @brief Unified push-based asset registry (slots + needs queue +
    ///        Apply pipeline). Phase-1 skeleton; later phases route
    ///        texture / .pkb / child-MDX through this in place of the
    ///        per-kind caches.
    assets::AssetManager& Assets();
    const assets::AssetManager& Assets() const;

    /// @brief Re-queue every asset that failed to load (still on the placeholder)
    ///        so the next provider pump re-fetches it. Hosts call this after the
    ///        content provider's sources change (an IO-settings edit) to refresh
    ///        textures that couldn't be found before, without a restart.
    ///        Forwards to AssetManager::RetryUnloaded so hosts needn't pull in
    ///        the asset-manager (and its engine-internal) headers.
    void RetryUnloadedAssets();

    debug::DebugRenderer& Debug();
    dnc::DncService* GetDncService();
    const dnc::DncService* GetDncService() const;
    shadow::ShadowService* GetShadowService();
    const shadow::ShadowService* GetShadowService() const;
    gtao::GtaoService* GetGtaoService();
    const gtao::GtaoService* GetGtaoService() const;
    dof::DofService* GetDofService();
    const dof::DofService* GetDofService() const;
    post_process::PostProcessService* GetPostProcessService();
    const post_process::PostProcessService* GetPostProcessService() const;

    // Engine-side Dear ImGui adapter. Returns nullptr when WDX_ENABLE_IMGUI
    // is off at compile time, or before InitBlsShaders has had a chance to
    // call EnsureImGui (e.g. during early service construction). The host
    // is responsible for ImGui::CreateContext / ImGui::NewFrame /
    // ImGui::Render — the engine only consumes the resulting ImDrawData
    // inside RenderFrame.
    dear_imgui::ImGuiRenderer* ImGui();
    const dear_imgui::ImGuiRenderer* ImGui() const;
    // Lazily build the adapter once a device is available. Idempotent.
    void EnsureImGui(gfx::IGFXDevice& gfx, bls::BlsShaderCache& shaderCache, gfx::Format rtvFormat,
                     gfx::Format dsvFormat);
    // Tear down before the gfx device goes away.
    void ShutdownImGui();

    // ---- Per-actor effect services (of the ACTIVE scene) ----
    particle::ParticleService& Particles();
    particle::SplatService& Splats();
    corn_effects::CornEffectsService& CornEffects();
    effects::SpnSpawner& Spn();

    // The corn-fx GPU backend init (device + BLS program + asset hooks) is built
    // by RenderPipeline once the device is up. It must reach EVERY scene's corn
    // service — including scenes created later. The pipeline registers an applier
    // here; RenderService runs it for all existing scenes immediately and for
    // each new scene at CreateScene time.
    void SetCornBackendInitApplier(std::function<void(corn_effects::CornEffectsService&)> fn);
    // Run `fn` for each scene's corn service (used by GPU teardown paths that
    // must Clear()/ReleaseGpuResources() every scene before the device dies).
    void ForEachCornService(const std::function<void(corn_effects::CornEffectsService&)>& fn);
    // Run `fn` for each scene (used by GPU teardown paths that must release
    // every scene's actor GPU state + template caches before the device dies).
    void ForEachScene(const std::function<void(SceneManager&)>& fn);

    // World-space AABB of a corn-fx emitter's live particle cloud (game units),
    // or false if it has no live particles. Host-facing convenience so tools can
    // frame a standalone .pkb effect without pulling the cornflakes interface
    // headers (the corn↔game scale and packet layout stay inside the renderer).
    bool ComputeEffectWorldBounds(u32 actor, i32 emitterId, Vector3f& outMin, Vector3f& outMax);

    // ---- App-tunable knobs ----
    RenderSettings& Settings();
    const RenderSettings& Settings() const;

    // ---- Per-frame scene update orchestrator ----
    FrameTicker& Ticker();

    // ---- Sound ----
    // Volume / mute lives on the emitter itself: callers use Sound().SetVolume(v).
    // SwapSoundEmitter installs a different backend (e.g., the viewer + Max
    // plugin plug in CubebSoundEmitter at startup) and carries over the
    // previous volume.
    ISoundEmitter& Sound();
    const ISoundEmitter& Sound() const;
    void SwapSoundEmitter(std::unique_ptr<ISoundEmitter> emitter);

    /// @brief Drain the AssetManager needs queue and feed each entry
    ///        through the active content provider, then commit the
    ///        decoded results. On the desktop this is the synchronous
    ///        equivalent of the web build's JS-driven pump: ReadFile
    ///        returns bytes immediately for assets that live in CASC /
    ///        MPQ / disk, so the textures are ready by the time the
    ///        next render frame runs. No-op on Emscripten — the JS
    ///        host owns the drain there.
    void PumpAssetsViaProvider();

    // ---- Pipeline-coordination hooks ----
    // RenderService owns the asset managers; RenderPipeline drives their
    // lifecycle as it brings the GPU device up and down.
    bool HasDeviceAssetManagers() const;
    void CreateDeviceAssetManagers(gfx::IGFXDevice& gfx);
    void ResetDeviceAssetManagers();
    dnc::DncService& EnsureDncService();
    shadow::ShadowService& EnsureShadowService(gfx::IGFXDevice& gfx);
    gtao::GtaoService& EnsureGtaoService(gfx::IGFXDevice& gfx, gfx::GfxApi api);
    dof::DofService& EnsureDofService(gfx::IGFXDevice& gfx, gfx::GfxApi api,
                                      bls::BlsShaderCache& cache, gfx::BufferHandle spriteVb);
    post_process::PostProcessService& EnsurePostProcessService(gfx::IGFXDevice& gfx,
                                                               gfx::GfxApi api,
                                                               bls::BlsShaderCache& cache,
                                                               gfx::BufferHandle spriteVb);

private:
    // ---- Per-scene service initialization (current + future scenes) ----
    // A scene's SceneServices bundle needs device-dependent wiring (corn-fx GPU
    // backend, splat→AssetManager hook) that only becomes available once the
    // device is up. The inputs arrive at different times (assets at
    // CreateDeviceAssetManagers, corn backend at InitBlsShaders), and scenes can
    // be created before OR after either — so a single one-off configure() would
    // miss whichever side isn't ready yet, and every scene but the active one.
    // InitSceneServices is the one place that applies all currently-available
    // wiring to one bundle; InitAllSceneServices re-applies to every live scene
    // whenever a new input becomes available. Both steps are idempotent.
    void InitSceneServices(SceneServices& svc);
    void InitAllSceneServices();

    // ---- Pimpl: all instance state lives here. ----
    // Subsystems reach RenderService state through the public accessors above
    // (Scene/Textures/Samplers/Replaceables/Particles/Splats/Spn/Settings/
    //  Pipeline/Loader/Ticker/Sound). No friend declarations needed.
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace whiteout::flakes::renderer
