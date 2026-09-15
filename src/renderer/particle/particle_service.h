#pragma once

#include "particle2_emitter.h"
#include "particle_draw.h"
#include "particle_emitter.h"
#include "particle_geometry.h"
#include "particle_output.h"
#include "sc2_kernel_types.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace whiteout::flakes::renderer::particle {

// `ModelId`, `EmitterKey`, `ChildModelEvent` and the trail id space are in
// `particle_output.h`; the draw list and the side streams in `particle_draw.h`.

class ParticleService {
public:
    ParticleService();
    ~ParticleService();

    /// Register @p emitter under the output its own desc declares, so the key
    /// and the desc cannot disagree.
    void AddEmitter(ModelId model, i32 emitterId, std::unique_ptr<ParticleEmitter> emitter);
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

    /// @brief The emitter under this key, or null.
    ///
    /// The pointer is not guarded past the call. It is safe to hold for the
    /// frame because the map is mutated only on the load thread: the mutex
    /// guards the map's structure, not the emitters in it.
    ParticleEmitter* GetEmitter(ModelId model, ParticleOutput output, i32 emitterId);

    i32 EmitterCount() const;
    i32 TotalParticleCount() const;

    bool HasEmittersForModel(ModelId model) const;

    // Visit every registered emitter under the service lock. For diagnostics and
    // the trace harness — not a per-frame render path.
    void ForEachEmitter(
        const std::function<void(const EmitterKey&, const ParticleEmitter&)>& fn) const;

    /// @brief Visit one model's emitters, mutably, under the service lock.
    ///
    /// The per-frame route for the actor layer: key order, and only that
    /// model's range of the map rather than every emitter in the scene.
    /// Trails are not visited — their owner is.
    template <class Fn>
    void ForModelEmitters(ModelId model, Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = emitters_.lower_bound(FirstKeyOf(model));
             it != emitters_.end() && it->first.model == model; ++it)
            fn(it->first, *it->second);
    }

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
    /// The smallest key a model's emitters can have: the first id space, the
    /// lowest id. Where that model's range of the map begins.
    static EmitterKey FirstKeyOf(ModelId model) {
        return {model, ParticleOutput::Billboard, (std::numeric_limits<i32>::min)()};
    }

    mutable std::mutex mutex_;
    // The SC2 ModelParticles elements every emitter registered this frame,
    // walked once after all of them have updated. Declared before the emitters
    // so it outlives them: an emitter's destructor takes its own entries out.
    Sc2PendingModels sc2PendingModels_;
    std::map<EmitterKey, std::unique_ptr<ParticleEmitter>> emitters_;

    // Accumulated during Simulate, moved out by DrainChildModelEvents.
    std::vector<ChildModelEvent> childEvents_;

    f32 emissionScaler_ = 1.0f;
    bool fogEnabled_ = false;
    FogSampler fogSampler_;
    GroundQuery groundQuery_;
};

} // namespace whiteout::flakes::renderer::particle
