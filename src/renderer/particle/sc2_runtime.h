#pragma once

// ============================================================================
// Sc2Runtime — everything an SC2 emitter keeps between frames, in one block.
//
// The emitter owns it through `std::unique_ptr<Sc2Runtime> sc2_`, allocated by
// SetDesc when `desc->family == Family::Sc2` and freed otherwise. That pointer
// is then the family test everywhere else, so the selector is read in exactly
// one place (design R1) and `sizeof(Emitter2)` grows by a pointer for every
// dialect that is not SC2.
//
// One block rather than a dozen `sc2*_` members on the emitter, which is what
// the ribbon did: tolerable at twelve fields, and this one is heading for
// fifty. Grouped by the stage that reads it, matching Sc2EmitterDesc.
//
// Each field arrived with the phase that first read it — a field nothing can
// fill is a field nothing can test. The few that are read and never written
// say so where they are declared.
// ============================================================================

#include "emit_mesh.h"
#include "emitter_placement.h"
#include "particle_stages_sc2.h"
#include "renderer/sc2/sc2_rng.h"
#include "sc2_compose.h"
#include "types.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::particle {

class Emitter2;

// `Sc2PendingModel`, `Sc2PendingModels` and `RoutedSpawnRequest` live in
// `sc2_kernel_types.h`, where the emitter's interface can name them.

/// The per-frame working buffers `Sc2TickEmitter` fills and empties. Kept on
/// the runtime and cleared, never freed, so a steady-state frame allocates
/// nothing; no stage reads one across frames.
struct Sc2TickScratch {
    std::vector<f32> counts;
    std::vector<f32> carry;
    std::vector<u32> targets;
    std::vector<u32> emitted;
    std::vector<Sc2EmitEvent> events;
    std::vector<i32> killed;
    std::vector<Sc2SpawnedElement> batch;
    Sc2SpawnBatchPlan batchPlan;
    Sc2ChildRequests asked;
};

struct Sc2Runtime {
    // ---- PREP ----
    /// Everything `Sc2TickClock` carries between frames — the emitter clock,
    /// the overlay-wave clock, the sub-step cursor, the frame and wall stamps
    /// and the previous position. It used to be six loose fields here that
    /// duplicated the kernel's own struct; passing the kernel a copy and
    /// writing the results back is exactly the kind of join that goes wrong
    /// silently, so the runtime now holds the struct the kernel mutates.
    Sc2EmitClock clock;

    /// Where the emitter is THIS frame. `clock.prevPos` is the other end of
    /// the spawn sweep, and the tick refreshes it only on the full-step path —
    /// which is why the two are not one field.
    Vector3f curPos{0, 0, 0};

    /// `CParticleSystem+0x17C` and `+0x180`. The sub-step rate is only ever
    /// tested against zero (RE §5.1) — a zero forces the full-step path — and
    /// the offset shifts the emitter clock. Neither is a `PAR_` field.
    /// `subStepRate` has no writer: retail's is 15 and the port's 1 read the
    /// same through that test, so it is a constant the oracle rig varies.
    f32 subStepRate = 1.0f;
    f32 timeOffset = 0.0f;

    /// The host does not hand an emitter a wall clock or a frame number, so
    /// these two stand in. `frameIndex` guards a second tick in the same frame
    /// and `wallMs` is what the pre-roll gap is measured from — a gap this
    /// counter can never show, because it advances by exactly the frame it was
    /// given. Wiring the real clock is X5's, along with the pre-roll it gates.
    i32 frameIndex = 0;
    i32 wallMs = 0;

    /// This emitter's own draw stream. Retail passes the GLOBAL `g_Rand` to
    /// every emitter; the design took a per-emitter stream instead, because
    /// per-CALL parity is what the gates measure and a shared stream makes one
    /// emitter's spawns depend on how many another drew that frame — which is
    /// not reproducible and not worth reproducing.
    sc2::Rng rng;

    /// `CParticleSystem+0x400`. The sub-step time the previous frame could not
    /// spend; it BIASES the next frame's running target rather than being
    /// dropped.
    f32 accumTime = 0.0f;

    /// `elementState != 0` — the actor layer's freeze. The rate stops being
    /// sampled at all and the stored carry is the whole want, so a frozen
    /// emitter drains its fraction to zero and stops. No host writes it: the
    /// viewer has no actor freeze, so it stays false outside the gates.
    bool emissionFrozen = false;

    /// `CParticleSystem::emitFlagsWord`, the runtime word `Sc2InitSpawned`
    /// reads for the per-particle noise phase (bit 3) and `ComputeEmitCount`
    /// for the emission-disabled bit (0x20).
    u32 emitFlagsWord = 0;

    /// What `InitSpawnedParticles` carries ACROSS batches: the birth time, the
    /// sweep cursor and the running expiry maximum. Not derived from the clock
    /// — the sweep advances it per element.
    Sc2InitState initState;

    /// The `PAR_` tracks the actor layer sampled this frame. Copied whole by
    /// `ApplyState`; every EMIT and SPAWN input is read out of it.
    model::FrameState::ParticleFrameState::Sc2ParticleFrame frame;

    /// Bit 31's ask, consumed by the PREP stage: raised by
    /// `Sc2NoteActiveSequence` when the active sequence moves. Whether a
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
    Sc2ParticleStore store;

    // ---- BUILD ----
    /// The batch row the vertex shader reads (OP15). Rebuilt per frame from
    /// the desc and the emitter's transform — it is cheap, and keeping it here
    /// rather than on the stack is what lets a future batched draw hand eight
    /// emitters' rows to one constant buffer.
    Sc2QuadBatch batch;

    /// The emitter-region slot's triangle table over the surface's mesh, built
    /// when the emitter first ticks against that mesh: every triangle of every
    /// region `shapeRegions` names, in that order. Retail's asset loader builds its table and nothing measures
    /// its order, so region-then-triangle is a composition choice. The mesh's
    /// indices are already global, so one zero region base serves them all.
    std::vector<Sc2MeshTriangle> meshTriangles;
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
    Sc2PendingModels* pending = nullptr;
    Sc2PendingModels ownPending;
    /// Index-parallel with the store: whether the element holds a model, which
    /// of `childModelPaths` it became, and its pose as of its last `Update` —
    /// retail re-poses inside `SimulateParticles`, so a frame that runs no
    /// sub-step leaves the previous pose standing.
    std::vector<u8> hasModel;
    std::vector<u32> modelPath;
    std::vector<Sc2ModelPose> modelPose;
    /// Elements that died holding a model during this tick, in the order they
    /// died. The emitter turns them into deaths once the tick returns.
    std::vector<i32> modelDeaths;
    /// What the pose reads from the scene: the camera's right, view direction
    /// and up (retail's `sceneCtx+3024` rows), and the owning actor's renderer
    /// units per SC2 unit — the pose runs in SC2 units and the child actor
    /// applies that scale itself.
    std::array<Vector3f, 3> camera{Vector3f{1, 0, 0}, Vector3f{0, -1, 0}, Vector3f{0, 0, 1}};
    f32 actorWorldScale = 1.0f;

    Sc2TickScratch scratch;

    // ---- arming ----
    /// The half of `CParticleSystem::Init` a describe and a rewind both run:
    /// the store sized from the desc's cap — `min(authored,
    /// sc2::kMaxParticles)`, a 2 MiB vertex arena over the 464-byte
    /// four-vertex stride, which the desc already carries — and the runtime
    /// words derived from the record.
    ///
    /// Nothing set those words before this existed: every Euler emitter took
    /// the analytic path (0x10) and never left its spawn point, no emitter ever
    /// had noise (emitFlags 8), and the time-scale bits the clock reads were
    /// always clear.
    void Arm(const Sc2EmitterDesc& d) {
        store.Init(d.emit.maxParticles);
        const Sc2InitWords words = Sc2InitRuntimeWords(d);
        clock.stateFlags = words.stateFlags;
        emitFlagsWord = words.emitFlags;
    }

    /// A rewind: the clocks, the sweep and the queues start over, then @ref Arm
    /// derives the words again on the fresh clock. The slots keep their count
    /// and lose their carry.
    ///
    /// The active sequence goes back to −1 with the clock, so the next frame's
    /// note is a change again and a `SimulateInit` emitter pre-rolls as it did
    /// when it was created; the pre-roll offset and the carried sub-step time
    /// belong to the clock and go with it. Only the draw stream carries on, as
    /// every dialect's does across a rewind.
    void Rearm(const Sc2EmitterDesc& d) {
        clock = Sc2EmitClock{};
        initState = Sc2InitState{};
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

} // namespace whiteout::flakes::renderer::particle
