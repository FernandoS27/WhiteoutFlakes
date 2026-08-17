#pragma once

#include "particle_geometry.h"
#include "particle2_emitter.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <functional>
#include <memory>
#include <mutex>
#include <map>
#include <vector>

namespace whiteout::flakes::renderer::particle {

using ModelId = u32;

// The output kind is part of the identity: PE2 and PE1 emitter ids are both
// 0-based indices into different MDX chunks, so without it they collide the
// moment one actor has both.
struct EmitterKey {
    ModelId model;
    ParticleOutput output;
    i32 id;

    bool operator==(const EmitterKey& o) const {
        return model == o.model && output == o.output && id == o.id;
    }
    // Ordering, not hashing: the emitter map's iteration order reaches output.
    // BuildGeometry emits partDraws in map order, TransparentDrawOrder
    // tie-breaks on the resulting `unit`, and PE1 child handles come from
    // AllocActorId() called while walking the same map — so with a hashed
    // container the tie-break and the handle assignment were both seeded by
    // the hash. At these counts (tens of emitters) a tree is free.
    bool operator<(const EmitterKey& o) const {
        if (model != o.model)
            return model < o.model;
        if (output != o.output)
            return static_cast<u8>(output) < static_cast<u8>(o.output);
        return id < o.id;
    }
};

// One child-model particle's lifecycle, reported as data. The service never
// touches actors: it cannot, without taking a dependency on the loader and
// re-entering its own mutex through DestroyActor -> RemoveModel. FrameTicker
// drains these and owns the spawn/destroy.
struct ChildModelEvent {  // NOLINT: forward-declared in particle2_emitter.h
    enum class Kind : u8 { Birth, Death, Transform };
    Kind kind = Kind::Birth;
    ModelId owner = 0;
    i32 emitterId = 0;
    u32 childHandle = 0;
    Matrix44f transform = Matrix44f::identity();
    // Transform events only. Zero when the emitter wants the child hidden this
    // frame without ending its life — an M2 model particle blinked off by
    // twinkle, which the client expresses by clearing the child model's own
    // render flags. PE1 has no twinkle and leaves this at 1.
    f32 visibility = 1.0f;
};

struct EmitterDrawList {
    ModelId model;
    i32 emitterId;
    i32 vertexOffset;
    i32 vertexCount;
    i32 priorityPlane;
    ParticleMaterialDesc material;
    // Emitter origin in world space — sort key for the back-to-front
    // transparent pass (interleaves with geosets/ribbons/corn).
    Vector3f worldOrigin = {0, 0, 0};
};

class ParticleService {
public:
    ParticleService();
    ~ParticleService();

    void AddEmitter(ModelId model, ParticleOutput output, i32 emitterId,
                    std::unique_ptr<Emitter2> emitter);
    void RemoveModel(ModelId model);
    void Clear();

    Emitter2* GetEmitter(ModelId model, ParticleOutput output, i32 emitterId);

    i32 EmitterCount() const;
    i32 TotalParticleCount() const;

    bool HasEmittersForModel(ModelId model) const;

    // Visit every registered emitter under the service lock. For diagnostics and
    // the trace harness — not a per-frame render path.
    void ForEachEmitter(const std::function<void(const EmitterKey&, const Emitter2&)>& fn) const;

    void Simulate(f32 dt);

    // Moves out everything the child-model emitters produced during the last
    // Simulate. Empty when no such emitter is registered.
    void DrainChildModelEvents(std::vector<ChildModelEvent>& out);

    void BuildGeometry(const Matrix44f& worldToView, std::vector<Vertex>& outVertices,
                       std::vector<EmitterDrawList>& outDrawLists) const;

    // Emission-rate multiplier for every emitter in THIS service. Per-scene:
    // it used to be a process global, which meant scaling one viewport's
    // particles silently scaled every other scene's too.
    void SetEmissionScaler(f32 s);
    f32 EmissionScaler() const;

    void SetFogEnabled(bool on) {
        fogEnabled_ = on;
    }
    bool FogEnabled() const {
        return fogEnabled_;
    }

    void SetFogSampler(FogSampler sampler);

private:
    mutable std::mutex mutex_;
    std::map<EmitterKey, std::unique_ptr<Emitter2>> emitters_;

    // Accumulated during Simulate, moved out by DrainChildModelEvents.
    std::vector<ChildModelEvent> childEvents_;

    f32 emissionScaler_ = 1.0f;
    bool fogEnabled_ = false;
    FogSampler fogSampler_;
};

} // namespace whiteout::flakes::renderer::particle
