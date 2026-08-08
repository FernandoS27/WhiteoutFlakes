#pragma once

#include <cornflakes/interface/asset/effect_asset_model.hpp>
#include <cornflakes/interface/binding/effect_binder.hpp>
#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/render/pool_extractor.hpp>
#include <cornflakes/interface/render/render_backend.hpp>
#include <cornflakes/interface/render/render_view.hpp>
#include <cornflakes/interface/sim/emitter_scope_state.hpp>
#include <cornflakes/interface/sim/lod.hpp>
#include <cornflakes/interface/sim/external_store.hpp>
#include <cornflakes/interface/sim/particle_pool.hpp>
#include <cornflakes/interface/sim/proximity_hash.hpp>
#include <cornflakes/interface/sim/spawn_event.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace whiteout::cornflakes {

class IMeshResourceProvider;
class ITextureResourceProvider;
class IVectorFieldProvider;

struct EffectFrameInputs {
    ViewParams view;
    f32 dt = 1.0F / 60.0F;
    f32 effectAge = 0.0F;
    Mat4x3 emitterL2W = Mat4x3::identity();
    u32 baseRngSeed = 0xC0FFEE00U;

    f32 renderUnitsPerSimUnit = 1.0F;

    bool effectIsRunning = true;
};

using LayerRenderInputMap = RenderInputMap;

struct ParticleInspection {
    bool valid = false;

    std::size_t layerIndex = 0U;
    std::size_t slotIndex = 0U;

    ExternalView externals{};

    std::span<const RegisterValue> streamRegisters{};

    std::span<const RegisterValue> localRegisters{};

    Mat4x3 sceneL2W = Mat4x3::identity();
    f32 simLod = 0.0F;
    f32 simLodDistanceMin = 0.0F;
    f32 simLodDistanceMax = 0.0F;
    f32 effectAge = 0.0F;
    bool effectIsRunning = true;

    u32 rngState = 0U;
    f32 lifeRatio = 0.0F;
    u64 selfId = 0U;
    u64 parentSelfId = 0U;
    bool dead = true;
    bool wasDeadAtFrameStart = true;
};

class EffectRuntime {
public:
    EffectRuntime(const EffectAssetModel& model, EffectId effectId, IArena& bindArena,
                  IArena& frameArena, IssueBag& issues,
                  IMeshResourceProvider* meshProvider = nullptr,
                  ITextureResourceProvider* textureProvider = nullptr,
                  IVectorFieldProvider* vectorFieldProvider = nullptr);

    [[deprecated("pass separate bind / frame arenas; per-tick scratch leaks otherwise")]]
    EffectRuntime(const EffectAssetModel& model, EffectId effectId, IArena& arena,
                  IssueBag& issues);

    bool isValid() const noexcept {
        return plan_ != nullptr;
    }

    const EffectExecutionPlan* plan() const noexcept {
        return plan_;
    }

    std::size_t layerCount() const noexcept;

    std::size_t aliveCount(std::size_t layerIdx) const noexcept;

    u64 spawnedTotal(std::size_t layerIdx) const noexcept;

    u32 positionSpread(std::size_t layerIdx, f32 outMin[3], f32 outMax[3]) const noexcept;

    std::size_t poolSize(std::size_t layerIdx) const noexcept;

    ParticleInspection inspectParticle(std::size_t layerIdx, std::size_t slotIdx) const noexcept;

    std::span<const SpawnEvent> spawnQueueEvents(std::size_t layerIdx) const noexcept;

    std::size_t spatialHashCount() const noexcept;

    std::string_view spatialHashName(std::size_t idx) const noexcept;

    std::vector<ProximityEntry> spatialHashEntries(std::size_t idx) const;

    enum class LayerModel : u8 {
        ModelA,
        ModelB,
    };

    void setLayerModel(std::size_t layerIdx, LayerModel model);

    LayerModel layerModel(std::size_t layerIdx) const noexcept;

    std::size_t simtLayersRanLastTick() const noexcept {
        return simtLayersRanLastTick_;
    }
    std::size_t simtLayersFellBackLastTick() const noexcept {
        return simtLayersFellBackLastTick_;
    }

    void setPoolSize(std::size_t layerIdx, std::size_t count);

    void setRenderInputMap(std::size_t layerIdx, const LayerRenderInputMap& mapping);

    void setRenderInputMap(std::size_t layerIdx, std::size_t rendererIdx,
                           const LayerRenderInputMap& mapping);

    void setAttribute(std::string_view name, const std::array<f32, 4>& value);

    void setSpawnerEnabled(bool enabled) noexcept {
        spawnerEnabled_ = enabled;
    }
    bool spawnerEnabled() const noexcept {
        return spawnerEnabled_;
    }

    bool setBackend(IRenderBackend* backend, IssueBag& issues);

    bool tick(const EffectFrameInputs& inputs, IssueBag& issues);

    void reset() noexcept;

    std::span<const RenderPacket> lastPackets() const noexcept {
        return std::span<const RenderPacket>{lastPackets_.data(), lastPackets_.size()};
    }

private:
    bool ensureBackendPrepared(IssueBag& issues);
    void buildPackets(IArena& arena, IssueBag& issues);

    void setupLayerStorage(std::size_t layerIdx);
    void setupSelfLifeSlots(std::size_t layerIdx);
    void setupSpatialHashes(std::size_t layerIdx);

    std::span<ProximityHash* const> spatialReadHashes(std::size_t layerIdx) const noexcept;
    std::span<ProximityHash* const> spatialWriteHashes(std::size_t layerIdx) const noexcept;

    bool isKickTarget(LayerId id) const noexcept;

    void initializeOnFirstTick(const EffectFrameInputs& inputs, IssueBag& issues);
    void drainPendingSpawns(std::size_t layerIdx, const EffectFrameInputs& inputs,
                            IssueBag& issues);
    void prepareParticlesForTick(std::size_t layerIdx, const EffectFrameInputs& inputs);
    void injectSceneDt(std::size_t layerIdx, f32 dt);
    void injectSceneScalar(std::size_t layerIdx, const char* name, f32 value);
    void applyAttributeOverrides(std::size_t layerIdx);
    void routeEventsForLayer(std::size_t layerIdx);

    void updateCameras(const EffectFrameInputs& inputs) noexcept;

    u16 positionSlotFor(std::size_t layerIdx) const noexcept;

    void refreshLayerLods() noexcept;

    f32 layerLodFor(std::size_t layerIdx) const noexcept {
        return layerIdx < layerLod_.size() ? layerLod_[layerIdx] : lodConfig_.defaultLod;
    }

    std::span<const SceneCamera> cameras() const noexcept {
        return std::span<const SceneCamera>{&camera_, 1U};
    }

    IArena& bindArena_;
    IArena& frameArena_;

    std::optional<EffectExecutionPlan> ownedPlan_;
    const EffectExecutionPlan* plan_ = nullptr;

    std::vector<ParticlePool> pools_;

    std::vector<LayerRenderInputMap> inputMaps_;

    std::vector<std::vector<LayerRenderInputMap>> perRendererInputMaps_;
    std::vector<LayerModel> layerModels_;
    std::size_t simtLayersRanLastTick_ = 0U;
    std::size_t simtLayersFellBackLastTick_ = 0U;
    std::vector<SpawnEventQueue> spawnQueues_;

    std::vector<std::unique_ptr<ProximityHash>> spatialHashesOwned_;
    std::vector<std::unique_ptr<ProximityHash>> spatialHashesOwnedAlt_;
    std::vector<std::string> spatialHashNames_;

    std::vector<std::vector<ProximityHash*>> spatialHashesPerLayer_;
    std::vector<std::vector<ProximityHash*>> spatialHashesAltPerLayer_;
    bool spatialReadIsAlt_ = false;

    std::vector<EmitterScopeState> emitterScopeStates_;

    std::vector<u32> spawnHeads_;

    std::vector<u64> spawnedTotals_;

    u64 nextSelfId_ = 1U;

    std::vector<std::pair<std::string, std::array<f32, 4>>> attributeOverrides_;

    static constexpr u16 kSlotUnbound = 0xFFFFU;
    std::vector<u16> invLifeSlots_;
    std::vector<u16> lifeRatioSlots_;

    IRenderBackend* backend_ = nullptr;
    bool backendPrepared_ = false;
    bool initialized_ = false;
    bool spawnerEnabled_ = true;

    f32 sceneTime_ = 0.0F;

    SceneCamera camera_{};

    LodConfig lodConfig_{};
    std::vector<f32> layerLod_;
    std::vector<u8> layerUsesSimLod_;
    bool layerLodSetupDone_ = false;

    std::vector<RenderPacket> lastPackets_;
};

}
