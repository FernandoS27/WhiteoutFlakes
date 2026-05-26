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
#include <unordered_map>

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

    void Simulate(f32 dt);

private:
    mutable std::mutex mutex_;
    ::whiteout::cornflakes::ExpandingArena frameArena_{1U << 20};
    std::unordered_map<EmitterKey, std::unique_ptr<CornEffectsEmitter>, EmitterKeyHash> emitters_;
    f32 gameToCornEffectsScale_ = 0.01f;
    std::optional<CornEffectsGfxBackend::Init> backendInit_;
    CornEffectsFrameInputs frameInputs_;
    f32 pendingDt_ = 0.0f;
};

} // namespace whiteout::flakes::renderer::corn_effects
