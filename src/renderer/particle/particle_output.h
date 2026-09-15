#pragma once

// ============================================================================
// What a particle emitter reports to the world outside the module: who it is
// (`EmitterKey`), and what its child-model particles did (`ChildModelEvent`).
//
// Split out of `particle_service.h` so an emitter can report without seeing
// the service that collects the reports — which is what used to make
// `child_model_emitter.h` and `d3_emitter.cpp` include the whole service, and
// `particle2_emitter.h` dodge the cycle with elaborated type names.
// ============================================================================

#include "types.h"
#include "whiteout/flakes/types.h"

#include <string>

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
