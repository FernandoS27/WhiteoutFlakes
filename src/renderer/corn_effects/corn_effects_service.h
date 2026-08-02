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

// One corn emitter's consolidated draw slice, surfaced for the unified
// transparent queue. The owning model id lets the caller compute a world sort
// position (from the actor) without re-entering the service mutex.
struct CornDrawUnit {
    ActorId model;
    i32 emitterId;
    i32 priorityPlane;
    u32 slice;
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

    // Rewind every live effect to its first frame, keeping the emitters
    // registered. Deliberately not Clear(): registrations are created when a
    // model spawns or an attachment loads, so dropping them would silence the
    // effects permanently rather than restarting them.
    void ResetRuntimes();

    void AddCornEmitter(ActorId model, i32 emitterId, std::unique_ptr<CornEffectsEmitter> emitter);
    void RemoveModel(ActorId model);
    void Clear();

    CornEffectsEmitter* GetEmitter(ActorId model, i32 emitterId);

    // World-space (game-unit) AABB of an emitter's live particle cloud, padded
    // by per-particle billboard size. Returns false when the emitter is missing
    // or has no live particles. Render-packet positions are corn-space, so the
    // bounds are converted by 1/gameToCornEffectsScale before returning.
    bool ComputeWorldParticleBounds(ActorId model, i32 emitterId, Vector3f& outMin,
                                    Vector3f& outMax) const;

    i32 EmitterCount() const;
    i32 TotalParticleCount() const;
    bool HasEmittersForModel(ActorId model) const;

    void SetOwningAgentVisibilityForModel(ActorId model, bool visible);

    /// @brief Run sim ticks for every emitter, then issue the
    ///        consolidated GPU draws — single VB/IB/CB write per
    ///        frame regardless of emitter count. Replaces the
    ///        previous per-emitter Simulate-+-draw pattern.
    void SimulateAndRender(f32 dt);

    // ---- Split form for the unified transparent queue ----
    // Tick every emitter (fills the per-emitter pending CPU batches), no GPU.
    void Simulate(f32 dt);
    // Consolidate all pending batches into the shared VB/IB + CBs and surface
    // one CornDrawUnit per emitter (sliced into the shared buffers). The caller
    // sorts these among the other transparent producers and calls DrawCornSlice.
    void PrepareInterleavedDraws(std::vector<CornDrawUnit>& out);
    // Bind the shared corn resources and draw one prepared slice. Safe to call
    // interleaved with other producers (re-binds its own VB/IB/CBs each call).
    void DrawCornSlice(u32 slice);

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
    void SimulateInternal(f32 dt); // Phase 0+1, caller holds mutex_

    // One emitter's slice into the shared VB/IB after consolidation. Kept across
    // PrepareInterleavedDraws → DrawCornSlice so draws can run in sorted order.
    struct PreparedSlice {
        CornEffectsGfxBackend* backend;
        u32 baseVertex;
        u32 baseIndex;
        EmitterKey key;
    };
    std::vector<PreparedSlice> preparedSlices_;

    // Shared core of SimulateAndRender / PrepareInterleavedDraws: pack every
    // pending batch into the shared buffers + CBs, filling preparedSlices_.
    // Returns false if there is nothing to draw. Caller holds mutex_.
    bool ConsolidatePending();
    // Draw one prepared slice (binds the shared resources first). Caller holds
    // mutex_.
    void DrawPreparedSlice(const PreparedSlice& s);

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
