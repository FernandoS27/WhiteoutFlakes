#pragma once

// ============================================================================
// RibbonService — every scene's ribbon trails, for every profile.
//
// Mirrors ParticleService: one service per scene, emitters keyed by
// (model, emitterId), and no knowledge of actors. RenderModel used to own a
// RibbonSystem per actor, which meant the simulation and the WC3-shaped config
// were welded to the MDX path; a profile picks a RibbonBehavior here instead.
// ============================================================================

#include "renderer/ribbon/ribbon_emitter.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <functional>
#include <map>
#include <mutex>
#include <vector>

namespace whiteout::flakes::renderer::ribbon {

using ModelId = u32;

struct EmitterKey {
    ModelId model;
    i32 id;

    bool operator==(const EmitterKey& o) const {
        return model == o.model && id == o.id;
    }
    // Ordering, not hashing: BuildGeometry walks this map and the resulting
    // strip order becomes the ribbon unit index the transparent queue
    // tie-breaks on, so a hashed container would seed draw order from a hash.
    bool operator<(const EmitterKey& o) const {
        if (model != o.model)
            return model < o.model;
        return id < o.id;
    }
};

/// @brief One submittable ribbon: a vertex range plus the material state the
///        pipeline needs to pick a PSO.
struct RibbonDrawList {
    ModelId model = 0;
    i32 emitterId = 0;
    i32 vertexOffset = 0;
    i32 vertexCount = 0;
    i32 priorityPlane = 0;
    i32 textureId = -1;
    i32 filterMode = 0;
    bool unshaded = false;
    bool twoSided = true;
    /// Strip head in world space — sort key for the back-to-front transparent
    /// pass, where ribbons interleave with geosets, particles and corn.
    Vector3f worldOrigin = {0, 0, 0};
};

class RibbonService {
public:
    void AddEmitter(ModelId model, i32 emitterId, const RibbonDesc& desc,
                    const RibbonBehavior& behavior);
    void RemoveModel(ModelId model);
    void Clear();
    /// @brief Drop every live trail without deregistering the emitters, which
    ///        is what a rewind wants — Clear() would leave the model with no
    ///        ribbons at all until it was respawned.
    void ResetTrails();

    RibbonEmitter* GetEmitter(ModelId model, i32 emitterId);
    const RibbonEmitter* GetEmitter(ModelId model, i32 emitterId) const;

    bool HasEmittersForModel(ModelId model) const;
    i32 EmitterCount() const;
    i32 TotalEdgeCount() const;
    i32 EdgeCountForModel(ModelId model) const;

    /// @brief Push a sampled frame state at one emitter. Silently ignores an
    ///        unregistered id, the way the per-actor system did.
    void SetState(ModelId model, i32 emitterId, const RibbonState& st);

    /// @brief Tick one model's emitters. FrameTicker calls this per actor so
    ///        the visibility gate stays where actor state lives.
    void SimulateModel(ModelId model, f32 dt);
    /// @brief Tick every registered emitter. Used by tests and headless tools.
    void Simulate(f32 dt);

    /// @brief Build one model's triangles into `outVertices`, appending one
    ///        draw list per emitter that produced geometry. Offsets are
    ///        relative to the start of this call, because the pipeline uploads
    ///        each actor into its own vertex buffer.
    void BuildGeometry(ModelId model, std::vector<Vertex>& outVertices,
                       std::vector<RibbonDrawList>& outDrawLists) const;

    void ForEachEmitter(const std::function<void(const EmitterKey&, const RibbonEmitter&)>& fn)
        const;

private:
    mutable std::mutex mutex_;
    std::map<EmitterKey, RibbonEmitter> emitters_;
};

} // namespace whiteout::flakes::renderer::ribbon
