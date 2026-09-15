#pragma once

// ============================================================================
// What a particle emitter reports to the world outside the module: who it is
// (`EmitterKey`), and what its child-model particles did (`ChildModelEvent`).
// Apart from `particle_service.h` so an emitter can report without seeing the
// service that collects the reports.
// ============================================================================

#include "types.h"
#include "whiteout/flakes/types.h"

#include <functional>
#include <string>
#include <vector>

namespace whiteout::flakes::renderer::particle {

using ModelId = u32;

// What a live particle *is* at frame time. Orthogonal to the spawn shape: a
// cone that emits child models is ConeShape + ChildModel, not a subclass of
// both. Ray / Mesh / Light slot in here without touching the sim.
enum class ParticleOutput : u8 { Billboard = 0, ChildModel = 1 };

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
    // Ordering, not hashing: the map's iteration order reaches the draw order's
    // tie-break and the PE1 child handles. M2_PARTICLE_DESIGN.md §11.10.
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
struct ChildModelEvent {
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
    /// Birth events only: which loader route builds the child. Decided by the
    /// emitter, which knows its own dialect, so the host never looks the
    /// emitter up again to find out.
    enum class Route : u8 {
        /// A WC3 PE1: a staged child template, by path.
        Pe1Template,
        /// An M2 or SC2 model particle: the loader's own model cache, by path.
        ModelParticle,
        /// A Diablo III `.acr`, by SNO id.
        D3Actor,
    };
    Route route = Route::Pe1Template;
    /// The two path routes: the model `pathIndex` picked. Empty when the
    /// emitter names none, which spawns nothing.
    std::string path;
    /// `Route::D3Actor` only.
    i32 snoActor = -1;
    Matrix44f transform = Matrix44f::identity();
    // Transform events only. Zero when the emitter wants the child hidden this
    // frame without ending its life — an M2 model particle blinked off by
    // twinkle, which the client expresses by clearing the child model's own
    // render flags. PE1 has no twinkle and leaves this at 1.
    f32 visibility = 1.0f;
};

/// @brief One emitter's child-model output: which child each slot holds, and the
///        events not yet drained.
///
/// A PE1, an M2 or SC2 model-particle emitter and a Diablo III child-actor
/// system each hold one, so the event is filled in one place. A slot is
/// whatever the emitter numbers its children by — a pool index, an SC2 store
/// node, or, for a D3 system that never kills a child on its own, the birth
/// order. Handle 0 is "this slot holds no child".
class ChildOutputChannel {
public:
    /// Mints a fresh child handle per birth; 0 refuses. Routed through the
    /// scene's actor-id allocator by whoever registers the emitter, so the
    /// renderer exposes no mutable counter.
    using HandleAllocator = std::function<u32()>;

    void Bind(ModelId owner, i32 emitterId, HandleAllocator alloc) {
        owner_ = owner;
        emitterId_ = emitterId;
        alloc_ = std::move(alloc);
    }
    bool Bound() const {
        return static_cast<bool>(alloc_);
    }

    /// Room for @p slots slots, handles kept.
    void Resize(usize slots) {
        handles_.resize(slots, 0);
    }
    usize SlotCount() const {
        return handles_.size();
    }
    bool Holds(u32 slot) const {
        return slot < handles_.size() && handles_[slot] != 0;
    }
    /// How many slots hold a child.
    usize LiveCount() const {
        usize n = 0;
        for (const u32 h : handles_)
            n += h != 0 ? 1u : 0u;
        return n;
    }

    /// Mints the handle @p slot holds from now on and queues its Birth, with
    /// @p fill supplying what the dialect decides — route, path, transform.
    /// Returns 0, queueing nothing and leaving the slot empty, when nothing is
    /// bound or the allocator refuses; @p fill runs only after a handle exists.
    template <class Fill>
    u32 Birth(u32 slot, Fill&& fill) {
        if (slot >= handles_.size())
            handles_.resize(static_cast<usize>(slot) + 1, 0);
        const u32 handle = alloc_ ? alloc_() : 0;
        handles_[slot] = handle;
        if (handle == 0)
            return 0;
        ChildModelEvent ev = Event(ChildModelEvent::Kind::Birth, handle);
        fill(ev);
        pending_.push_back(std::move(ev));
        return handle;
    }

    /// Ends the child @p slot holds, if any.
    void Death(u32 slot) {
        if (slot >= handles_.size())
            return;
        const u32 handle = handles_[slot];
        handles_[slot] = 0;
        if (handle != 0)
            pending_.push_back(Event(ChildModelEvent::Kind::Death, handle));
    }

    /// Ends every child, in slot order, and forgets the slots.
    void DeathAll() {
        for (const u32 handle : handles_) {
            if (handle != 0)
                pending_.push_back(Event(ChildModelEvent::Kind::Death, handle));
        }
        handles_.clear();
    }

    /// The Births and Deaths queued since the last drain, in the order they
    /// happened.
    void Drain(std::vector<ChildModelEvent>& out) {
        for (auto& ev : pending_)
            out.push_back(ev);
        pending_.clear();
    }

    /// Where the child @p slot holds is this frame. The caller asks `Holds`
    /// first, so a slot with no child costs no transform.
    void Transform(u32 slot, const Matrix44f& xf, f32 visibility,
                   std::vector<ChildModelEvent>& out) const {
        ChildModelEvent ev = Event(ChildModelEvent::Kind::Transform, handles_[slot]);
        ev.transform = xf;
        ev.visibility = visibility;
        out.push_back(ev);
    }

private:
    ChildModelEvent Event(ChildModelEvent::Kind kind, u32 handle) const {
        ChildModelEvent ev;
        ev.kind = kind;
        ev.owner = owner_;
        ev.emitterId = emitterId_;
        ev.childHandle = handle;
        return ev;
    }

    ModelId owner_ = 0;
    i32 emitterId_ = 0;
    HandleAllocator alloc_;
    std::vector<u32> handles_;
    std::vector<ChildModelEvent> pending_;
};

/// An emitter adopts at most this many trails — the client's
/// MAX_CHILD_EMITTERS (ParticleSystem2.cpp:2503).
inline constexpr usize kMaxTrailsPerEmitter = 4;

// The id a trail emitter draws and traces under. Its owner holds it, so it has
// no key of its own in the emitter map — this keeps it identifiable and stable
// (a function of the parent's index and the adoption order) without colliding
// with any real emitter index.
constexpr i32 kTrailEmitterIdBase = 0x40000;
constexpr i32 TrailEmitterId(i32 parentId, i32 childIndex) {
    return kTrailEmitterIdBase + parentId * static_cast<i32>(kMaxTrailsPerEmitter) + childIndex;
}

} // namespace whiteout::flakes::renderer::particle
