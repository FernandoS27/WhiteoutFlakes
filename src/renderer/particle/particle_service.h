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
    // Birth events only: which of `EmitterDesc::childModelPaths` this particle
    // became. WC3, M2 and D3 author one model and always leave it at 0; an SC2
    // `PAR_` carries a table and draws an index per particle, at a point in the
    // RNG stream that is its own (RE §16.16) — so the index travels with the
    // event rather than being re-derived by whoever spawns the actor.
    u32 pathIndex = 0;
    Matrix44f transform = Matrix44f::identity();
    // Transform events only. Zero when the emitter wants the child hidden this
    // frame without ending its life — an M2 model particle blinked off by
    // twinkle, which the client expresses by clearing the child model's own
    // render flags. PE1 has no twinkle and leaves this at 1.
    f32 visibility = 1.0f;
};

// The id a trail emitter draws and traces under. Its owner holds it, so it has
// no key of its own in the emitter map — this keeps it identifiable and stable
// (a function of the parent's index and the adoption order) without colliding
// with any real emitter index.
constexpr i32 kTrailEmitterIdBase = 0x40000;
constexpr i32 TrailEmitterId(i32 parentId, i32 childIndex) {
    return kTrailEmitterIdBase + parentId * static_cast<i32>(Emitter2::kMaxTrails) + childIndex;
}

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
    // Emitter2::MaterialTimeSec at build time. Only a D3 draw reads it.
    f32 materialTimeSec = 0.0f;
};

// One frame's geometry for the emitters whose particles are the client's
// `CMultiTexParticle` — refraction and multi-texture both. `extraUV` is
// index-parallel with `vertices` and holds the two scrolling texture layers;
// nothing else in the engine has three UV sets, which is why these emitters
// need a stream of their own rather than a wider shared vertex.
//
// The two kinds fill separate instances and are consumed differently:
// refraction leaves the transparent pass entirely (the client buckets it into
// M2PASS_REFRACTION and nowhere else, `AddParticleElement` @0x100f78590),
// while multi-texture stays in it and only changes shader. See
// M2_REFRACTION_DESIGN.md and M2_MULTITEX_DESIGN.md.
struct MultiTexGeometry {
    std::vector<Vertex> vertices;
    std::vector<Vector4f> extraUV;
    std::vector<EmitterDrawList> draws;

    void Clear() {
        vertices.clear();
        extraUV.clear();
        draws.clear();
    }
};

/// @brief What a Diablo III particle vertex carries that the shared one cannot.
///
/// Index-parallel with the ORDINARY vertex stream rather than a stream of its
/// own: a D3 quad is an ordinary billboard in position, colour and sort order,
/// and only its four baked texcoords and its SECOND colour need more room than
/// the shared vertex has. The service keeps every array the same length as that
/// stream, so a draw's `vertexOffset` indexes all of them.
/// See BuildGeometryInput::d3Uv01 and ::d3Color1.
struct D3VertexStream {
    std::vector<Vector4f> uv01; ///< uv set 0 in .xy, set 1 in .zw
    std::vector<Vector4f> uv23;
    std::vector<f32> color1; ///< ch6, the erosion tail's exponent scale

    bool Empty() const {
        return uv01.empty();
    }
    void Clear() {
        uv01.clear();
        uv23.clear();
        color1.clear();
    }
};

class ParticleService {
public:
    ParticleService();
    ~ParticleService();

    void AddEmitter(ModelId model, ParticleOutput output, i32 emitterId,
                    std::unique_ptr<Emitter2> emitter);
    void RemoveModel(ModelId model);
    /// @brief Drop one emitter.
    ///
    /// Whatever it has already queued for the actor layer is harvested first,
    /// so a child-model system that reported its children's deaths on the way
    /// out does not take those reports with it. It does not itself END
    /// anything: a caller that wants the children gone restarts the emitter
    /// before calling this, which is what queues the deaths.
    bool RemoveEmitter(ModelId model, ParticleOutput output, i32 emitterId);
    void Clear();
    /// @brief Restart every emitter without deregistering any, which is what a
    ///        rewind wants — Clear() would leave the model without particles
    ///        until it was reloaded.
    void ResetEmitters();

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

    // Builds every emitter's geometry for this frame.
    //
    // A refraction emitter goes to @p refraction instead of the ordinary lists;
    // pass null and it is skipped outright, which is the correct answer for a
    // frame with no refraction pass — drawing it into the scene would paint a
    // distortion mask as if it were colour.
    //
    // A multi-texture emitter puts its VERTICES in @p multiTex but its draw in
    // @p outDrawLists, so it still sorts with everything else in the
    // transparent pass; pass null and it falls back to single-texture shading
    // off the ordinary stream.
    // A Diablo III emitter additionally fills @p d3Uv with its four baked
    // texcoords per vertex, kept index-parallel with @p outVertices; pass null
    // and it falls back to sampling every layer at the raw quad uv.
    void BuildGeometry(const Matrix44f& worldToView, std::vector<Vertex>& outVertices,
                       std::vector<EmitterDrawList>& outDrawLists,
                       MultiTexGeometry* refraction = nullptr,
                       MultiTexGeometry* multiTex = nullptr,
                       D3VertexStream* d3Uv = nullptr) const;

    // Whether any registered emitter (or trail) draws refraction. Cheap enough
    // to ask per frame, and what lets the pipeline skip the pass entirely.
    bool HasRefractionEmitters() const;

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

    /// @brief Install the surface every emitter's particles collide against.
    ///
    /// Set once by the host and re-installed on every emitter registered
    /// afterwards, so the MOVE stage never rebuilds a `std::function` per
    /// particle per sub-step. Beside SetFogSampler because it is the same kind
    /// of thing: one scene-wide callback the service owns and pushes down,
    /// rather than something each emitter goes looking for.
    void SetGroundQuery(GroundQuery query);

private:
    mutable std::mutex mutex_;
    // The SC2 ModelParticles elements every emitter registered this frame,
    // walked once after all of them have updated. Declared before the emitters
    // so it outlives them: an emitter's destructor takes its own entries out.
    Sc2PendingModels sc2PendingModels_;
    std::map<EmitterKey, std::unique_ptr<Emitter2>> emitters_;

    // Accumulated during Simulate, moved out by DrainChildModelEvents.
    std::vector<ChildModelEvent> childEvents_;

    f32 emissionScaler_ = 1.0f;
    bool fogEnabled_ = false;
    FogSampler fogSampler_;
    GroundQuery groundQuery_;
};

} // namespace whiteout::flakes::renderer::particle
