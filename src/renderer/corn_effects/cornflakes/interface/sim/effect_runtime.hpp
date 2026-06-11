#pragma once

/// @file
/// @brief Top-level per-effect runtime: owns layer pools and drives one tick per frame.
///
/// `EffectRuntime` binds an `EffectAssetModel` once into an `EffectExecutionPlan`
/// (kept in `bindArena`) and reuses it for every tick. Per-frame scratch lives
/// in the separate `frameArena` and is reset by the caller between ticks.

#include <cornflakes/interface/asset/effect_asset_model.hpp>
#include <cornflakes/interface/binding/effect_binder.hpp>
#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/render/pool_extractor.hpp>
#include <cornflakes/interface/render/render_backend.hpp>
#include <cornflakes/interface/render/render_view.hpp>
#include <cornflakes/interface/sim/emitter_scope_state.hpp>
#include <cornflakes/interface/sim/particle_pool.hpp>
#include <cornflakes/interface/sim/proximity_hash.hpp>
#include <cornflakes/interface/sim/spawn_event.hpp>

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace whiteout::cornflakes {

/// @brief Per-frame inputs handed to `EffectRuntime::tick`.
struct EffectFrameInputs {
    ViewParams view;
    f32 dt = 1.0F / 60.0F;
    f32 effectAge = 0.0F;
    Mat4x3 emitterL2W = Mat4x3::identity();
    u32 baseRngSeed = 0xC0FFEE00U;

    bool effectIsRunning = true;
};

using LayerRenderInputMap = RenderInputMap;

/// @brief Owns the bound effect plan, particle pools, and orchestrates one tick per frame.
///
/// Construct with separate bind/frame arenas. The bind arena holds the
/// `EffectExecutionPlan` for the lifetime of this runtime; the frame arena is
/// reset between ticks by the caller.
class EffectRuntime {
public:
    EffectRuntime(const EffectAssetModel& model, EffectId effectId, IArena& bindArena,
                  IArena& frameArena, IssueBag& issues);

    [[deprecated("pass separate bind / frame arenas; per-tick scratch leaks otherwise")]]
    EffectRuntime(const EffectAssetModel& model, EffectId effectId, IArena& arena,
                  IssueBag& issues);

    /// @brief True when binding succeeded and the runtime is usable.
    bool isValid() const noexcept {
        return plan_ != nullptr;
    }

    const EffectExecutionPlan* plan() const noexcept {
        return plan_;
    }

    std::size_t layerCount() const noexcept;

    /// @brief Debug/inspection: count of live (non-dead) particles in `layerIdx`'s pool.
    /// Distinct from a render packet's `particleCount`, which reports pool capacity.
    std::size_t aliveCount(std::size_t layerIdx) const noexcept;

    /// @brief Debug/inspection: cumulative particles ever spawned into `layerIdx`. Divided by
    /// elapsed frames gives the incoming rate; `aliveCount / rate` estimates particle lifetime.
    u64 spawnedTotal(std::size_t layerIdx) const noexcept;

    /// @brief Debug/inspection: axis-aligned bounding box of the layer's render `*__Position`
    /// field over live particles. Returns the live count; `outMin`/`outMax` are left untouched
    /// when no Position binding or no live particles. The XY extent is the objective
    /// collapse/ring metric (≈0 ⇒ collapsed onto the central axis).
    u32 positionSpread(std::size_t layerIdx, f32 outMin[3], f32 outMax[3]) const noexcept;

    /// @brief Resize the particle pool of `layerIdx`. Must be set before first tick.
    void setPoolSize(std::size_t layerIdx, std::size_t count);

    /// @brief Set the slot→external name mapping shared by all renderers on the layer.
    void setRenderInputMap(std::size_t layerIdx, const LayerRenderInputMap& mapping);

    /// @brief Set the slot→external mapping for a specific renderer on a multi-renderer layer.
    void setRenderInputMap(std::size_t layerIdx, std::size_t rendererIdx,
                           const LayerRenderInputMap& mapping);

    /// @brief Override an attribute default by name; written into bound externals each tick.
    void setAttribute(std::string_view name, const std::array<f32, 4>& value);

    void setSpawnerEnabled(bool enabled) noexcept {
        spawnerEnabled_ = enabled;
    }
    bool spawnerEnabled() const noexcept {
        return spawnerEnabled_;
    }

    /// @brief Attach a backend that consumes packets each tick. `prepare()` runs lazily.
    bool setBackend(IRenderBackend* backend, IssueBag& issues);

    /// @brief Run one frame: drain spawns, evaluate scripts, route events, build packets.
    bool tick(const EffectFrameInputs& inputs, IssueBag& issues);

    /// @brief Drop all live particles and reset per-tick state. Bound plan stays valid.
    void reset() noexcept;

    /// @brief Packets emitted by the most recent successful tick.
    std::span<const RenderPacket> lastPackets() const noexcept {
        return std::span<const RenderPacket>{lastPackets_.data(), lastPackets_.size()};
    }

private:
    bool ensureBackendPrepared(IssueBag& issues);
    void buildPackets(IArena& arena, IssueBag& issues);

    void setupLayerStorage(std::size_t layerIdx);
    void setupSelfLifeSlots(std::size_t layerIdx);
    void setupSpatialHashes(std::size_t layerIdx);

    bool isKickTarget(LayerId id) const noexcept;

    void initializeOnFirstTick(const EffectFrameInputs& inputs, IssueBag& issues);
    void drainPendingSpawns(std::size_t layerIdx, const EffectFrameInputs& inputs,
                            IssueBag& issues);
    void prepareParticlesForTick(std::size_t layerIdx, const EffectFrameInputs& inputs);
    void injectSceneDt(std::size_t layerIdx, f32 dt);
    void injectSceneScalar(std::size_t layerIdx, const char* name, f32 value);
    void applyAttributeOverrides(std::size_t layerIdx);
    void routeEventsForLayer(std::size_t layerIdx);

    IArena& bindArena_;
    IArena& frameArena_;

    std::optional<EffectExecutionPlan> ownedPlan_;
    const EffectExecutionPlan* plan_ = nullptr;

    std::vector<ParticlePool> pools_;

    std::vector<LayerRenderInputMap> inputMaps_;

    std::vector<std::vector<LayerRenderInputMap>> perRendererInputMaps_;
    std::vector<SpawnEventQueue> spawnQueues_;

    std::vector<std::unique_ptr<ProximityHash>> spatialHashesOwned_;
    std::vector<std::string> spatialHashNames_;

    std::vector<std::vector<ProximityHash*>> spatialHashesPerLayer_;

    std::vector<EmitterScopeState> emitterScopeStates_;

    std::vector<u32> spawnHeads_;

    /// Cumulative spawns committed into each layer (debug bisection — see `spawnedTotal`).
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

    /// Deterministic accumulating scene clock (Σ dt). This is `scene.time` — a real scene
    /// simulation time, independent of (possibly wall-clock-based) per-frame effectAge.
    f32 sceneTime_ = 0.0F;

    std::vector<RenderPacket> lastPackets_;
};

} // namespace whiteout::cornflakes
