#pragma once

#include "particle_geometry.h"
#include "plane_emitter.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::renderer::particle {

using ModelId = u32;

struct EmitterKey {
    ModelId model;
    i32 id;

    bool operator==(const EmitterKey& o) const {
        return model == o.model && id == o.id;
    }
};

struct EmitterKeyHash {
    usize operator()(const EmitterKey& k) const noexcept {
        return (static_cast<u64>(k.model) * 0x9E3779B97F4A7C15ull) ^ static_cast<u32>(k.id);
    }
};

struct EmitterDrawList {
    ModelId model;
    i32 emitterId;
    i32 vertexOffset;
    i32 vertexCount;
    i32 priorityPlane;
    ParticleMaterialDesc material;
};

class ParticleService {
public:
    ParticleService();
    ~ParticleService();

    void AddPlaneEmitter(ModelId model, i32 emitterId, std::unique_ptr<PlaneEmitter> emitter);
    void RemoveModel(ModelId model);
    void Clear();

    PlaneEmitter* GetEmitter(ModelId model, i32 emitterId);

    i32 EmitterCount() const;
    i32 TotalParticleCount() const;

    bool HasEmittersForModel(ModelId model) const;

    void Simulate(f32 dt);

    void BuildGeometry(const Matrix44f& worldToView, std::vector<Vertex>& outVertices,
                       std::vector<EmitterDrawList>& outDrawLists) const;

    void SetGlobalScaler(f32 s);
    f32 GlobalScaler() const;

    void SetFogEnabled(bool on) {
        fogEnabled_ = on;
    }
    bool FogEnabled() const {
        return fogEnabled_;
    }

    void SetFogSampler(FogSampler sampler);

private:
    mutable std::mutex mutex_;
    std::unordered_map<EmitterKey, std::unique_ptr<PlaneEmitter>, EmitterKeyHash> emitters_;

    bool fogEnabled_ = false;
    FogSampler fogSampler_;
};

} // namespace whiteout::flakes::renderer::particle
