#pragma once

#include "renderer/corn_effects/corn_effects_gfx_backend.h"
#include "renderer/types.h"
#include "whiteout/flakes/types.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace whiteout::cornflakes {
struct EffectAssetModel;
class EffectRuntime;
class ExpandingArena;
class IArena;
struct Mat4x3;
} // namespace whiteout::cornflakes

namespace whiteout::flakes::renderer::assets {
class AssetManager;
} // namespace whiteout::flakes::renderer::assets

namespace whiteout::flakes::renderer::corn_effects {

class CornEffectsService;

enum SimFlag : u32 {
    SimFlag_OwningAgentIsVisible = 0x00000001u,
    SimFlag_OwningAgentBecameVisible = 0x00000002u,
    SimFlag_RenderAttempted = 0x00000010u,
    SimFlag_UpdatedByAnim = 0x00000020u,
    SimFlag_Paused = 0x00000040u,
    SimFlag_SystemDead = 0x00001000u,
    SimFlag_KilledByEffect = 0x00002000u,
};

inline constexpr std::size_t kDefaultRenderPoolSize = 256;

class CornEffectsEmitter {
public:
    CornEffectsEmitter(assets::AssetManager& assets, std::string pkbPath,
                       std::string animVisibilityGuide, i32 replaceableId, bool cornEffectsScaling);
    ~CornEffectsEmitter();

    CornEffectsEmitter(const CornEffectsEmitter&) = delete;
    CornEffectsEmitter& operator=(const CornEffectsEmitter&) = delete;
    CornEffectsEmitter(CornEffectsEmitter&&) = delete;
    CornEffectsEmitter& operator=(CornEffectsEmitter&&) = delete;

    void SetModelToWorld(const Matrix44f& m) {
        modelToWorld_ = m;
    }
    void SetScale(f32 s) {
        hostScale_ = s;
    }
    void SetEmissionRateMultiplier(f32 v) {
        emissionRateMultiplier_ = v;
    }
    void SetLifeSpanMultiplier(f32 v) {
        lifeSpanMultiplier_ = v;
    }
    void SetSpeedMultiplier(f32 v) {
        speedMultiplier_ = v;
    }
    void SetColor(const Vector4f& rgba) {
        color_ = rgba;
    }
    void SetWeatherParams(const Vector3f& position, const Vector2f& size, f32 emissionRate);
    void SetReplaceableId(i32 id) {
        replaceableId_ = id;
    }
    void SetReplaceableColor(const Vector4f& rgba) {
        replaceableColor_ = rgba;
    }
    void SetOwningAgentVisibility(bool visible);
    void SetCurrentAnimationName(const char* name);

    // True when the emitter's visibility guide is the "single-shot per
    // cycle" shape — Always=Off with an Attack/Death (or variation)
    // enable token. Hosts that force a non-looping sequence to loop
    // (Actor::ignoreNonLooping) push the actor's monotonic sequence-cycle
    // counter via SyncSequenceCycle(); on an increase the emitter resets
    // its runtime so the effect re-fires once per loop iteration.
    bool IsNonLoopingEffect() const {
        return isNonLoopingEffect_;
    }
    void SyncSequenceCycle(i32 cycle);

    void Update(f32 dt, bool paused);

    void MarkRenderAttempted() {
        simFlags_ |= SimFlag_RenderAttempted;
    }

    bool Alive() const;
    i32 TotalAlive() const;
    const std::string& PkbPath() const {
        return pkbPath_;
    }
    ::whiteout::cornflakes::EffectRuntime* LiveRuntime() const {
        return live_.runtime.get();
    }

    void SetBackendInit(const std::optional<CornEffectsGfxBackend::Init>& init) {
        backendInit_ = init;
    }
    void SetFrameInputs(const CornEffectsFrameInputs& f) {
        frameInputs_ = f;
    }
    void SetFrameArena(::whiteout::cornflakes::IArena* arena) {
        frameArena_ = arena;
    }

private:
    bool TrySpawn();
    void ParseAnimVisibilityGuide();
    void PushAttributes(::whiteout::cornflakes::EffectRuntime& rt);
    void ResetRuntime();

    bool ShouldBeSpawning() const {
        return (simFlags_ & (SimFlag_Paused | SimFlag_SystemDead)) == 0;
    }
    bool IsVisible() const {
        return (simFlags_ & (SimFlag_OwningAgentIsVisible | SimFlag_RenderAttempted)) ==
               (SimFlag_OwningAgentIsVisible | SimFlag_RenderAttempted);
    }
    f32 GetCornFxScale() const {
        return cornEffectsScaling_ ? hostScale_ : 1.0f;
    }

    static ::whiteout::cornflakes::Mat4x3 ToCornflakesL2W(const Matrix44f& m);

    assets::AssetManager& assets_;
    std::uint32_t assetSlot_ = 0; // AssetManager::kInvalidSlot
    u32 lastAssetGen_ = 0;        // observed slot generation; bump triggers re-spawn
    std::string pkbPath_;
    std::string animVisibilityGuide_;
    bool cornEffectsScaling_ = false;
    i32 replaceableId_ = 0;
    const ::whiteout::cornflakes::EffectAssetModel* assetModel_ = nullptr;

    bool defaultAnimEnabled_ = true;
    bool isNonLoopingEffect_ = false;
    // -1 = unsynced (first sync just records the count without resetting).
    i32 lastSeenCycle_ = -1;
    std::vector<std::string> enabledAnimNames_;
    std::vector<std::string> disabledAnimNames_;
    std::string currentAnimName_;

    Matrix44f modelToWorld_ = Matrix44f::identity();
    f32 hostScale_ = 1.0f;
    f32 alpha_ = 1.0f;
    Vector4f color_ = {1, 1, 1, 1};
    Vector4f replaceableColor_ = {1, 0, 0, 1};
    f32 emissionRateMultiplier_ = 1.0f;
    f32 lifeSpanMultiplier_ = 1.0f;
    f32 speedMultiplier_ = 1.0f;
    Vector3f weatherPosition_ = {0, 0, 0};
    Vector2f weatherSize_ = {1, 1};
    f32 weatherEmissionRate_ = 1.0f;

    f32 gameToCornEffectsScale_ = 0.01f;

    u32 simFlags_ = 0;
    f32 effectAge_ = 0.0f;
    Vector3f position_ = {0, 0, 0};

    struct RuntimeBundle {
        std::unique_ptr<::whiteout::cornflakes::ExpandingArena> bindArena;
        std::unique_ptr<::whiteout::cornflakes::EffectRuntime> runtime;
        std::unique_ptr<CornEffectsGfxBackend> backend;
    };
    RuntimeBundle live_;

    bool wasActive_ = false;

    ::whiteout::cornflakes::IArena* frameArena_ = nullptr;

    std::optional<CornEffectsGfxBackend::Init> backendInit_;
    CornEffectsFrameInputs frameInputs_;

    friend class CornEffectsService;
};

} // namespace whiteout::flakes::renderer::corn_effects
