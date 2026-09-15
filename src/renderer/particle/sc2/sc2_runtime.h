#pragma once

// ============================================================================
// Runtime — everything an SC2 emitter keeps between frames, in one block,
// grouped by the stage that reads it. The emitter owns it through
// `std::unique_ptr<Runtime> sc2_`, allocated by SetDesc for `Family::Sc2`; that
// pointer is the family test everywhere else (design R1).
// See SC2_PARTICLE_DESIGN.md §16.6.
// ============================================================================

#include "renderer/particle/base/emit_mesh.h"
#include "renderer/particle/base/emitter_placement.h"
#include "renderer/particle/sc2/particle_stages_sc2.h"
#include "renderer/particle/sc2/sc2_compose.h"
#include "renderer/sc2/sc2_rng.h"
#include "types.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::particle {
class Emitter2;
}

namespace whiteout::flakes::renderer::particle::sc2 {

// `PendingModel`, `PendingModels` and `RoutedSpawnRequest` live in
// `sc2_kernel_types.h`, where the emitter's interface can name them.

/// The per-frame working buffers `TickEmitter` fills and empties. Kept on
/// the runtime and cleared, never freed, so a steady-state frame allocates
/// nothing; no stage reads one across frames.
struct TickScratch {
    std::vector<f32> counts;
    std::vector<f32> carry;
    std::vector<u32> targets;
    std::vector<u32> emitted;
    std::vector<EmitEvent> events;
    std::vector<i32> killed;
    std::vector<SpawnedElement> batch;
    SpawnBatchPlan batchPlan;
    ChildRequests asked;
};

struct Runtime {
    // ---- PREP ----
    /// Everything `TickClock` carries between frames, held as the very struct
    /// the kernel mutates. See SC2_PARTICLE_DESIGN.md §16.6.
    EmitClock clock;

    /// Where the emitter is THIS frame. `clock.prevPos` is the other end of
    /// the spawn sweep, and the tick refreshes it only on the full-step path —
    /// which is why the two are not one field.
    Vector3f curPos{0, 0, 0};

    /// `CParticleSystem+0x17C` and `+0x180`, neither a `PAR_` field. The rate
    /// is only tested against zero (RE §5.1) and has no writer; the offset
    /// shifts the emitter clock. See SC2_PARTICLE_RE.md §17.9.
    f32 subStepRate = 1.0f;
    f32 timeOffset = 0.0f;

    /// Stand-ins for the wall clock and frame number the host does not hand
    /// an emitter: `frameIndex` guards a second tick in a frame, `wallMs` is
    /// where the pre-roll gap is measured from. See SC2_PARTICLE_DESIGN.md §16.6.
    i32 frameIndex = 0;
    i32 wallMs = 0;

    /// This emitter's own draw stream, where retail shares the GLOBAL `g_Rand`
    /// (design §8). See SC2_PARTICLE_DESIGN.md §16.6.
    renderer::sc2::Rng rng;

    /// `CParticleSystem+0x400`. The sub-step time the previous frame could not
    /// spend; it BIASES the next frame's running target rather than being
    /// dropped.
    f32 accumTime = 0.0f;

    /// `elementState != 0` — the actor layer's freeze. The rate stops being
    /// sampled at all and the stored carry is the whole want, so a frozen
    /// emitter drains its fraction to zero and stops. No host writes it: the
    /// viewer has no actor freeze, so it stays false outside the gates.
    bool emissionFrozen = false;

    /// `CParticleSystem::emitFlagsWord`, the runtime word `InitSpawned`
    /// reads for the per-particle noise phase (bit 3) and `ComputeEmitCount`
    /// for the emission-disabled bit (0x20).
    u32 emitFlagsWord = 0;

    /// What `InitSpawnedParticles` carries ACROSS batches: the birth time, the
    /// sweep cursor and the running expiry maximum. Not derived from the clock
    /// — the sweep advances it per element.
    InitState initState;

    /// The `PAR_` tracks the actor layer sampled this frame. Copied whole by
    /// `ApplyState`; every EMIT and SPAWN input is read out of it.
    model::FrameState::ParticleFrameState::Sc2ParticleFrame frame;

    /// Bit 31's ask, consumed by the PREP stage: raised by
    /// `NoteActiveSequence` when the active sequence moves. Whether a
    /// pre-roll then runs is gap-driven (RE §15.4, oracle OP3c), which is
    /// PREP's decision and not the caller's.
    bool preRollPending = false;
    /// `+0x3F0`: the active sequence last noted, −1 until one resolves — the
    /// constructor's value, which makes the first resolution a change. The
    /// pre-roll reads its peak in this column.
    i32 activeSequence = -1;

    // ---- EMIT ----
    /// Per emission slot: slot 0 is the `PAR_` itself, 1..n its `PARC` copies.
    /// `carry` is the fractional particle the rate has not yet released,
    /// `burst` what a crossed squirt key still owes, and `target`/`emitted`
    /// the frame's budget and how much of it the sub-steps have spent.
    struct Slot {
        f32 carry = 0.0f;
        u32 burst = 0;
        u32 target = 0;
        u32 emitted = 0;
    };
    std::vector<Slot> slots;

    /// Requests other emitters made of this one, capped at retail's 128
    /// (RE §3.1); materialised before this emitter's own spawns.
    std::vector<SpawnRequest> inbox;
    /// Requests this emitter made of others, drained by the service after
    /// Update. Never delivered directly — the maker does not know the target.
    std::vector<RoutedSpawnRequest> outbox;

    /// Every live particle, its once-written vertex, the free list and the
    /// recycled `ParticleVB` slots. Sized by SetDesc from the desc's own cap,
    /// because that cap is a load-time decision (`min(authored, 0x200000/464)`)
    /// and re-deciding it per frame would let one grow without bound.
    ParticleStore store;

    // ---- BUILD ----
    /// The batch row the vertex shader reads (OP15). Rebuilt per frame from
    /// the desc and the emitter's transform — it is cheap, and keeping it here
    /// rather than on the stack is what lets a future batched draw hand eight
    /// emitters' rows to one constant buffer.
    QuadBatch batch;

    /// The emitter-region slot's triangle table, built on the first tick
    /// against the mesh: every triangle of every `shapeRegions` region, in that
    /// order, over one zero region base. See SC2_PARTICLE_DESIGN.md §16.6.
    std::vector<MeshTriangle> meshTriangles;
    /// The mesh @ref meshTriangles was built over. Null after a describe,
    /// whose regions may differ, so the next tick rebuilds it.
    const EmitMesh* meshTrianglesOf = nullptr;

    // ---- OUTPUT: model particles ----
    /// The emitter this runtime belongs to — the pending walk fires the birth
    /// on it. Set by SetDesc.
    Emitter2* host = nullptr;
    /// Where this emitter registers its ModelParticles elements: the service's
    /// frame-wide list once it is registered, @ref ownPending before that,
    /// which the emitter walks itself at the end of its own tick.
    PendingModels* pending = nullptr;
    PendingModels ownPending;
    /// Index-parallel with the store: whether the element holds a model, which
    /// of `childModelPaths` it became, and its pose as of its last `Update` —
    /// retail re-poses inside `SimulateParticles`, so a frame that runs no
    /// sub-step leaves the previous pose standing.
    std::vector<u8> hasModel;
    std::vector<u32> modelPath;
    std::vector<ModelPose> modelPose;
    /// Elements that died holding a model during this tick, in the order they
    /// died. The emitter turns them into deaths once the tick returns.
    std::vector<i32> modelDeaths;
    /// What the pose reads from the scene: the camera's right, view direction
    /// and up (retail's `sceneCtx+3024` rows), and the owning actor's renderer
    /// units per SC2 unit — the pose runs in SC2 units and the child actor
    /// applies that scale itself.
    std::array<Vector3f, 3> camera{Vector3f{1, 0, 0}, Vector3f{0, -1, 0}, Vector3f{0, 0, 1}};
    f32 actorWorldScale = 1.0f;

    TickScratch scratch;

    // ---- arming ----
    /// The half of `CParticleSystem::Init` a describe and a rewind both run:
    /// the store sized from the desc's cap (`min(authored,
    /// renderer::sc2::kMaxParticles)`) and the runtime words derived from the
    /// record. See SC2_PARTICLE_RE.md §17.9.
    void Arm(const EmitterDesc& d) {
        store.Init(d.emit.maxParticles);
        const InitWords words = InitRuntimeWords(d);
        clock.stateFlags = words.stateFlags;
        emitFlagsWord = words.emitFlags;
    }

    /// A rewind: the clocks, the sweep and the queues start over, then @ref Arm
    /// derives the words again; the slots keep their count and lose their carry.
    /// The active sequence returns to −1, so a `SimulateInit` emitter pre-rolls
    /// again. Only the draw stream carries on. See SC2_PARTICLE_DESIGN.md §16.6.
    void Rearm(const EmitterDesc& d) {
        clock = EmitClock{};
        initState = InitState{};
        preRollPending = false;
        activeSequence = -1;
        timeOffset = 0.0f;
        accumTime = 0.0f;
        curPos = {0, 0, 0};
        Arm(d);
        inbox.clear();
        outbox.clear();
        for (Slot& slot : slots)
            slot = Slot{};
    }
};

} // namespace whiteout::flakes::renderer::particle::sc2
