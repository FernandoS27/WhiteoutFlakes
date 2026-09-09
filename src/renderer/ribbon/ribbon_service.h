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

// RibbonDrawList and RibbonBuildContext live in ribbon_emitter.h — the BUILD
// stage emits the records; the service only stamps model/emitterId on them.

class RibbonService {
public:
    void AddEmitter(ModelId model, i32 emitterId, const RibbonDesc& desc,
                    const RibbonBehavior& behavior);
    /// @brief Register a desc that already knows its family — the SC2 route,
    ///        whose desc DescFromSc2Config built (behavior is the WC3↔WoW
    ///        sub-dialect and does not apply).
    void AddEmitter(ModelId model, i32 emitterId, const RibbonDesc& desc);
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

    /// @brief Build one model's triangles into `outVertices`, appending the
    ///        draw records each emitter's BUILD stage produced. Offsets are
    ///        relative to the start of this call, because the pipeline uploads
    ///        each actor into its own vertex buffer. `ctx` carries the camera
    ///        for the SC2 billboard frames; headless callers pass a default.
    void BuildGeometry(ModelId model, const RibbonBuildContext& ctx,
                       std::vector<Vertex>& outVertices,
                       std::vector<RibbonDrawList>& outDrawLists) const;

    void ForEachEmitter(const std::function<void(const EmitterKey&, const RibbonEmitter&)>& fn)
        const;

private:
    mutable std::mutex mutex_;
    std::map<EmitterKey, RibbonEmitter> emitters_;
};

} // namespace whiteout::flakes::renderer::ribbon
