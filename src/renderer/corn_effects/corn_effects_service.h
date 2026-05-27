#pragma once

#include "renderer/corn_effects/corn_effects_emitter.h"
#include "renderer/corn_effects/corn_effects_gfx_backend.h"
#include "renderer/types.h"
#include "whiteout/flakes/types.h"

#include <cornflakes/interface/core/arena.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace whiteout::cornflakes {
struct EffectAssetModel;
}

namespace whiteout::flakes::renderer::corn_effects {

using ActorId = u32;

struct EmitterKey {
    ActorId model;
    i32 id;
    bool operator==(const EmitterKey& o) const {
        return model == o.model && id == o.id;
    }
};

struct EmitterKeyHash {
    size_t operator()(const EmitterKey& k) const noexcept {
        return (static_cast<u64>(k.model) * 0x9E3779B97F4A7C15ull) ^ static_cast<u32>(k.id);
    }
};

class CornEffectsService {
public:
    CornEffectsService();
    ~CornEffectsService();

    CornEffectsService(const CornEffectsService&) = delete;
    CornEffectsService& operator=(const CornEffectsService&) = delete;

    void SetGameToCornEffectsScale(f32 s) {
        gameToCornEffectsScale_ = s;
    }
    f32 GameToCornEffectsScale() const {
        return gameToCornEffectsScale_;
    }

    void SetBackendInit(const std::optional<CornEffectsGfxBackend::Init>& init);

    void SetFrameInputs(const CornEffectsFrameInputs& fi);

    void SetPendingDt(f32 dt) {
        pendingDt_ = dt;
    }
    f32 PendingDt() const {
        return pendingDt_;
    }

    void AddCornEmitter(ActorId model, i32 emitterId, std::unique_ptr<CornEffectsEmitter> emitter);
    void RemoveModel(ActorId model);
    void Clear();

    CornEffectsEmitter* GetEmitter(ActorId model, i32 emitterId);

    i32 EmitterCount() const;
    i32 TotalParticleCount() const;
    bool HasEmittersForModel(ActorId model) const;

    void SetOwningAgentVisibilityForModel(ActorId model, bool visible);

    /// @brief Run sim ticks for every emitter, then issue the
    ///        consolidated GPU draws — single VB/IB/CB write per
    ///        frame regardless of emitter count. Replaces the
    ///        previous per-emitter Simulate-+-draw pattern.
    void SimulateAndRender(f32 dt);

    /// @brief Free the shared GPU resources. Called when the device
    ///        is being torn down so we don't leak handles past it.
    void ReleaseGpuResources();

    /// @brief Trial-bind a parsed @p model just long enough to walk its
    ///        layer programs and return the diffuse texture paths. Used
    ///        by the AssetManager OnApplied hook to discover the
    ///        textures a freshly-loaded .pkb references, so they can
    ///        be Acquired eagerly. The bind arena is ephemeral — the
    ///        emitter does its own real bind later.
    static std::vector<std::string> ExtractDiffuseTexturePaths(
        const ::whiteout::cornflakes::EffectAssetModel& model);

private:
    bool EnsureSharedBuffers(u32 totalVerts, u32 totalIndices);
    void FlushBatchedDraws();

    mutable std::mutex mutex_;
    ::whiteout::cornflakes::ExpandingArena frameArena_{1U << 20};
    std::unordered_map<EmitterKey, std::unique_ptr<CornEffectsEmitter>, EmitterKeyHash> emitters_;
    f32 gameToCornEffectsScale_ = 0.01f;
    std::optional<CornEffectsGfxBackend::Init> backendInit_;
    CornEffectsFrameInputs frameInputs_;
    f32 pendingDt_ = 0.0f;

    // Shared GPU resources used by every corn-fx emitter — one VB/IB/CB
    // set instead of N. Owned by the service, lifetime tied to the gfx
    // device passed via backendInit_.
    gfx::BufferHandle sharedVb_ = gfx::BufferHandle::Invalid;
    u32 sharedVbCap_ = 0;
    gfx::BufferHandle sharedIb_ = gfx::BufferHandle::Invalid;
    u32 sharedIbCap_ = 0;
    gfx::BufferHandle sharedVsCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle sharedPsCb_ = gfx::BufferHandle::Invalid;
};

} // namespace whiteout::flakes::renderer::corn_effects
