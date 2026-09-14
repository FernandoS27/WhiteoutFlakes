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

#include <climits>
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
    /// @brief Register one emitter. The desc carries its own family AND its
    ///        WC3/WoW sub-dialect, so there is one overload: the SC2 route
    ///        used to take the other one and be handed a WC3 behaviour that
    ///        described neither runtime it runs.
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
    /// The map's lower bound for a model's range: smaller than any real id,
    /// never one itself.
    static constexpr i32 kBeforeFirstEmitterId = INT32_MIN;

    /// Apply @p fn to every emitter of @p model, in id order. The map is
    /// ordered precisely so this range exists (and so draw order is not
    /// seeded by a hash), and four call sites were open-coding the walk.
    template <class Self, class Fn>
    static void ForModel(Self& self, ModelId model, Fn&& fn) {
        for (auto it = self.emitters_.lower_bound({model, kBeforeFirstEmitterId});
             it != self.emitters_.end() && it->first.model == model; ++it)
            fn(it);
    }

    /// The mutex guards the map's STRUCTURE, not the emitters in it:
    /// registration happens on the load thread and the per-frame walks are
    /// the frame's. That is why `GetEmitter` may hand back a pointer the
    /// caller keeps, the same contract ParticleService runs under.
    mutable std::mutex mutex_;
    std::map<EmitterKey, RibbonEmitter> emitters_;
};

} // namespace whiteout::flakes::renderer::ribbon
