#pragma once

// ============================================================================
// SC2 particle stage kernels — the pure functions the oracle pins.
//
// The contract the ribbon track established (design R3): every routine
// `tools/sc2_particle_oracle/` recorded from the shipped client is a free
// function with an explicit inputs struct, called by the emitter's stage
// method and by the replay test alike. Nothing here reads `Emitter2`, so a
// divergence from the golden is a red gate rather than a silent drift inside
// a loop that also does ten other things.
//
// Kernels shared with the ribbon dialect live in `sc2/` instead; these are the
// particle-only half (design R6: `particle/` and `ribbon/` never include each
// other).
//
// The float operation ORDER is part of the contract — written in the binary's
// order, not the algebraically tidy one.
// ============================================================================

#include "renderer/sc2/sc2_rng.h"
#include "whiteout/flakes/types.h"
#include "whiteout/vector_types.h"

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::particle {

using whiteout::Matrix44f;
using whiteout::Vector3f;

/// One emitter asking another to spawn a particle at a place. Retail's
/// `SParticleSpawnRequest`, 36 bytes (RE §3.3): a collision child is asked at
/// the hit point, a trail child along the parent's path. The receiver
/// materialises it before its own spawns on its next EMIT.
///
/// Defined here rather than beside the runtime state because `Sc2InitSpawned`
/// is what READS it — one definition, in the layer that consumes it.
struct SpawnRequest {
    Vector3f position{0, 0, 0};
    Vector3f velocityScale{1, 1, 1};
    Vector3f orientVec{0, 0, 1};
};

// ---------------------------------------------------------------------------
// PREP — the emit clock (`CElementBased::Tick`, RE §5.1, gate OP3).
// ---------------------------------------------------------------------------

/// Everything `Tick` carries between frames. Lives in @ref Sc2Runtime; passed
/// here by reference because the kernel mutates it exactly as retail does and
/// the golden records the mutated fields, not just the return.
struct Sc2EmitClock {
    /// Pinned to the scene clock every frame, never accumulated: outside the
    /// local-time arm `catchUp` is itself `now - emitterTime`, so `emitterTime`
    /// ends at `now` and any drift is erased. RE §3.6.
    f32 emitterTime = 0.0f;
    /// The overlay-wave phase clock, which IS an accumulation of `dt`.
    f32 variationTime = 0.0f;
    /// Where the previous sub-step sweep ended, in `variationTime`.
    f32 lastSubStepTime = 0.0f;
    /// The model frame this emitter last ticked on; a second tick in the same
    /// frame returns without touching anything.
    i32 lastFrameIndex = -1;
    /// Wall clock at the last tick — the pre-roll gap is measured from it.
    i32 lastTimeMs = 0;
    /// Refreshed only on the FULL-step path, so a sub-stepping emitter's
    /// displacement is measured from wherever the last full step left it.
    Vector3f prevPos{0, 0, 0};
    /// `CParticleSystem+0x120`, the derived state word (`sc2::SystemStateFlag`).
    /// Carried here because the tick clears bit 31 on the restart check.
    u32 stateFlags = 0;
};

/// The per-frame inputs. `dtMs` is the host's milliseconds and stays an
/// integer through the time-scale multiply, which is where a negative scale
/// stops being a small negative step and becomes a 4.29-billion-ms one.
struct Sc2ClockInputs {
    i32 dtMs = 0;
    f32 timeScale = 1.0f;
    /// Only ever tested against zero — never used as a rate (RE §5.1).
    f32 subStepRate = 1.0f;
    f32 timeOffset = 0.0f;
    i32 nowMs = 0;
    i32 frameIndex = 0;
    Vector3f worldPos{0, 0, 0};
    /// `pModel[921]` — a paused model takes the full-step path.
    bool modelPaused = false;
    /// The `g_force60Hz` byte: pins the ladder at 60 Hz whatever `dt` says.
    bool force60Hz = false;
};

/// What `Tick` hands to `EmitParticles` — the whole observable output of the
/// stage, which is why the gate captures the argument tuple rather than the
/// object.
struct Sc2StepPlan {
    /// False when the emitter already ticked this frame: nothing ran and the
    /// clock is untouched.
    bool ticked = false;
    /// One step covering the whole frame: first frame, skipped frame, paused
    /// model, `subStepRate == 0`, or the explicit bit.
    bool fullStep = false;
    /// Legitimately **zero** on a frame shorter than one sub-step. Retail still
    /// calls `EmitParticles`, runs no `Update`, and carries the whole elapsed
    /// time as the remainder — so a viewer above 60 Hz simulates on some
    /// frames only, and that is retail behaviour, not something to smooth.
    u32 nSteps = 0;
    Vector3f displacement{0, 0, 0};
    f32 subDt = 0.0f;
    f32 dt = 0.0f;
    f32 catchUp = 0.0f;
    f32 remainder = 0.0f;
};

/// `CElementBased::Tick(dtMs, timeScale)` — RE §5.1, gate OP3.
///
/// Advances @p clock and returns the plan EMIT walks. Two orderings the gate
/// settled and this reproduces: `nSteps` is capped at 100 only AFTER the
/// displacement was divided by the uncapped count (a frame wanting 200 steps
/// takes 100 steps of half the displacement and covers half the distance), and
/// the emitter-time seed tests exact equality with zero, so an emitter that
/// genuinely lands on 0.0 is re-seeded again.
Sc2StepPlan Sc2TickClock(Sc2EmitClock& clock, const Sc2ClockInputs& in);

// ---------------------------------------------------------------------------
// PREP — the pre-roll (`Tick`'s restart check + `EmitBurst`, RE §15.1 and
// §16.7, gates OP3 `restart` and OP3c).
// ---------------------------------------------------------------------------

/// What `Tick` does FIRST: before its once-per-frame test, so it runs even on
/// a frame the emitter has already ticked.
struct Sc2RestartCheck {
    /// Bit 31 was set and the busy bit was not, so the check consumed it.
    bool armed = false;
    /// Armed, and more than two frames of wall time have passed since the last
    /// tick.
    bool owed = false;
    /// `nowMs − lastTimeMs`, what `EmitBurst` is asked for.
    u32 gapMs = 0;
};

/// Consumes `stateFlags` bit 31 when it is set and the busy bit (0x800) is
/// not, and reports whether a pre-roll is owed. Both sides of the gap compare
/// are UNSIGNED, as retail's are, and the frame length is the scaled one.
/// The caller brackets the pre-roll blocks with the busy bit, so a block's own
/// tick never re-enters the check.
Sc2RestartCheck Sc2TickRestartCheck(Sc2EmitClock& clock, const Sc2ClockInputs& in);

/// The 33 ms block `EmitBurst` drives `Tick` with.
inline constexpr i32 kSc2PreRollBlockMs = 33;
/// `0x103C2BC90`, added to `timeOffset` before every block — so a pre-rolled
/// emitter's clock runs AHEAD of the scene by the whole pre-roll from then on.
inline constexpr f32 kSc2PreRollOffsetStep = 0.033f;

/// The lifetime peak `EmitBurst` budgets from: the max of the active
/// sequence's curve seeded at ZERO (so an all-negative curve pre-rolls
/// nothing), or the AnimRef's raw init value when there is no curve.
f32 Sc2PreRollPeak(std::span<const f32> curve, bool haveCurve, f32 initValue);

struct Sc2PreRollPlan {
    u32 budgetMs = 0;
    /// Blocks of `kSc2PreRollBlockMs`. The loop is do-while, so any non-zero
    /// budget runs at least one and the total overshoots to the next 33.
    u32 blocks = 0;
};

/// `EmitBurst`'s schedule, with two design §8 departures. The peak is clamped
/// at zero — retail's init-value fallback is not, and a negative lifetime
/// there becomes a 4.29-billion-ms budget that never returns. And the budget
/// is `min(requested, peak·1000)` always, where retail first compares its
/// millisecond budget against the session's WALL CLOCK and ignores the request
/// for any emitter created in a session's first `peak` seconds.
Sc2PreRollPlan Sc2PlanPreRoll(f32 peak, u32 requestedMs);

// ---------------------------------------------------------------------------
// EMIT — how much one slot wants (`ComputeEmitCount`, RE §5.2, gate OP7a).
// ---------------------------------------------------------------------------

/// One slot's ask for one frame. Slot 0 is the `PAR_` itself; `1..n` are its
/// `PARC` copies, whose rate and squirt keys come from their own record.
struct Sc2EmitCountInputs {
    /// The SAMPLED emission rate — retail's sampler writes it into the
    /// emitter's cache rather than returning it, which is why it arrives here
    /// as a plain number.
    f32 rate = 0.0f;
    f32 dt = 0.0f;
    /// The player time scale. Globally-paused does NOT zero the count — it
    /// forces this to 1.0, the opposite of what the name suggests (RE §5.2).
    f32 timeScale = 1.0f;
    /// Σ of the POSITIVE squirt key values crossed since the last frame; a
    /// negative key contributes nothing. Zero unless the crossing is allowed
    /// and the track's flags carry bit 1. Filled by `sc2::CrossedSquirtAmount`
    /// (OP7b, phase X5).
    f32 burst = 0.0f;
    /// Rows of the LOD tables in `sc2/sc2_element.h`; `quality` is the global
    /// level, 4 at viewer settings.
    i32 lodCut = 0;
    i32 lodReduce = 0;
    i32 quality = 4;
    f32 elemScaleX = 1.0f;
    /// `renderState == 14 || !enabled || (emitFlags & 0x20)` — all three fold
    /// into one early zero.
    bool suppressed = false;
    /// The transform node's visibility bit. A shape-7 (mesh) emitter reads its
    /// parent transform instead, and skips the test when that is null.
    bool nodeVisible = true;
};

/// `CParticleSystem::ComputeEmitCount` — RE §5.2, gate OP7a.
///
/// Returns a FRACTIONAL count. The design's table folded the floor and the
/// carry into this signature; they belong to `EmitParticles` and are pinned by
/// OP3b, so they live in @ref Sc2SlotEmitTarget instead and each function is
/// gated by the gate that actually measured it.
f32 Sc2ComputeEmitCount(const Sc2EmitCountInputs& in);

/// The floor-and-carry `EmitParticles` applies to that count (RE §5.3, part of
/// gate OP3b). @p carry is the fractional particle the rate has not released
/// yet; it is replaced by the new remainder.
///
/// NOT clamped at zero. A negative rate floors to −1 and returns 0xFFFFFFFF,
/// which spawns nothing (the spawn guard is signed) but is still what retail
/// divides `catchUp` by — the golden records a `spawnTimeStep` of 3.9e-12 for
/// exactly that case, so clamping here would quietly diverge.
u32 Sc2SlotEmitTarget(f32& carry, f32 count);

// ---------------------------------------------------------------------------
// EMIT — the squirt crossing (`M3Anim_CollectCrossedKeys`, RE §16.7, gate
// OP7b).
//
// A squirt is not a rate: it fires when the playhead STEPS OVER a key. The
// reader keeps a cursor one past the last frame it saw and reports every key
// between that and now, into a 128-entry sink `ComputeEmitCount` sums.
//
// This follows design §8's rule rather than the binary's search: every key in
// the crossed window is reported. Retail's two bisections lose four kinds of
// key — index 0 sitting exactly on the cursor, a wrap's tail past the cursor,
// a wrap's head key exactly at now, and a wrap's head never reaching the last
// key — and the replay transcribes those so each is shown to be the only
// difference.
// ---------------------------------------------------------------------------

/// One discrete track, as keyed for the playing sequence.
struct Sc2KeyTrack {
    std::span<const i32> times; ///< ascending frames
    std::span<const u16> values;
};

/// One live player: its track and its frame, already through the player's
/// offsets, time scale and loop modulo. Players with no track are simply not
/// passed — retail skips them WITHOUT consuming a cursor slot.
struct Sc2KeyPlayer {
    Sc2KeyTrack track;
    i32 frame = 0;
};

struct Sc2CrossedKey {
    u16 value = 0;
    i32 frame = 0;
};

/// `CParticleBatch`: the reported keys, one cursor per live player and the two
/// control bytes. Zeroed where retail's constructor leaves most of it as it
/// found it — no reader ever looks at what it left (design §8).
struct Sc2KeySink {
    static constexpr u32 kCapacity = 128;
    std::array<Sc2CrossedKey, kCapacity> keys{};
    u32 count = 0;
    std::vector<i32> cursors;
    /// `+1545`: snap the cursor to now and report from there.
    bool resync = false;
    /// `+1546`: move the cursor to now and report nothing. Resync wins.
    bool prime = false;
};

/// Walk every player's window into @p sink. `bias` shifts now but not the
/// cursor. Returns false for a playhead that has not moved — every cursor
/// already one past its frame — which leaves the PREVIOUS keys in the sink,
/// as retail does; true when the sink was rebuilt.
bool Sc2CollectCrossedKeys(std::span<const Sc2KeyPlayer> players, i32 bias, Sc2KeySink& sink);

/// `ComputeEmitCount`'s sum over the sink: each key read back as SIGNED, a
/// negative one contributing nothing.
f32 Sc2SquirtBurst(const Sc2KeySink& sink);

/// What an actor keeps between frames for one `PAR_`'s squirt keys: the
/// players it last walked — each one's container and where its playhead was —
/// and one sink per emission slot.
struct Sc2SquirtMemory {
    std::vector<u16> stcs;
    std::vector<i32> timeMs;
    bool valid = false;
    std::vector<Sc2KeySink> sinks;
};

// ---------------------------------------------------------------------------
// EMIT — the sub-step split (`EmitParticles`, RE §5.3, gate OP3b).
// ---------------------------------------------------------------------------

/// The stage calls the schedule produces, in order. Recording them rather than
/// running them is what lets the replay test compare against the golden's own
/// event log — the spawns and the `Update`s interleave, and WHERE a pass
/// spawns nothing is as much a fact as how many it spawned.
enum class Sc2EmitEventKind : u8 {
    Count,   ///< `ComputeEmitCount` was asked for @ref Sc2EmitEvent::slot.
    PreEmit, ///< the pre-emit vtable hook.
    Spawn,   ///< spawn `count` on `slot` (may be 0 when only requests are due).
    Update,  ///< simulate one sub-step.
};

struct Sc2EmitEvent {
    Sc2EmitEventKind kind = Sc2EmitEventKind::Update;
    u16 slot = 0;
    u32 count = 0;
};

struct Sc2ScheduleInputs {
    bool fullStep = false;
    u32 nSubSteps = 0;
    f32 subDt = 0.0f;
    f32 frameDt = 0.0f;
    f32 catchUp = 0.0f;
    f32 remainder = 0.0f;
    /// The sub-step time the previous frame could not spend. It BIASES the
    /// running target: positive leaves the frame's last particles unspawned,
    /// negative over-emits.
    f32 accumTime = 0.0f;
    /// `elementState != 0`. Emission is frozen: the rate is not sampled at all
    /// and the stored carry is the whole want, so a frozen emitter drains its
    /// fraction to zero and stops.
    bool frozen = false;
    /// `elementCount != 0 || aliveTest`. A frame with nothing to emit AND
    /// nothing alive runs no `Update` at all.
    bool anythingAlive = false;
    /// A queued spawn request makes the stage call `Spawn` even for a slot the
    /// rate gave nothing — that is how a request reaches the pool on an idle
    /// frame.
    bool haveRequests = false;
    /// Per slot, the value `Sc2ComputeEmitCount` returned — already MULTIPLIED
    /// by the window, because that multiply lives inside it. Ignored when
    /// `frozen`.
    ///
    /// This used to be the raw rate, with the window applied here instead. The
    /// two are the same number while `burst` is zero and different the moment
    /// it is not: `ComputeEmitCount` returns `(rate·dt·scale + burst)·lodMul`,
    /// so a crossed squirt key is NOT part of what the window scales. OP7a
    /// never runs this function and OP3b replaces `ComputeEmitCount` with a
    /// stub, so neither gate could see it — use @ref Sc2EmitWindow to get the
    /// window and hand `Sc2ComputeEmitCount` the same one.
    std::span<const f32> counts;
};

struct Sc2Schedule {
    /// The frame's per-slot budget. Deliberately unsigned: a negative rate
    /// floors to −1 and lands here as 0xFFFFFFFF, which spawns nothing (the
    /// guard is signed) but still divides `catchUp` into 4.29 billion parts.
    u32 total = 0;
    f32 spawnTimeStep = 0.0f;
    f32 accumTime = 0.0f;
};

/// `CParticleSystem::EmitParticles` — RE §5.3, gate OP3b.
///
/// Fills @p targets and @p events and returns the frame's totals; @p carry is
/// per-slot in/out. Nothing is spawned or simulated here — the stage method
/// walks @p events, which is what keeps the arithmetic testable apart from the
/// pool.
///
/// Two divisions that are NOT the same: the full step computes
/// `catchUp / total`, the sub-stepped path builds `1 / total` once and
/// multiplies all four lanes by it (three displacement lanes and `catchUp`).
/// They differ in the last bits and the golden carries both.
Sc2Schedule Sc2SpawnSchedule(const Sc2ScheduleInputs& in, std::span<f32> carry,
                             std::span<u32> targets, std::vector<Sc2EmitEvent>& events);

/// `rcpps` plus the one Newton step the reciprocal lane above goes through.
f32 Sc2RcpNewton(f32 x);

/// The window `ComputeEmitCount` is asked for: one SUB-step on the full-step
/// path, the whole FRAME on the sub-stepped one.
///
/// A function rather than two call sites, because the schedule and the counter
/// have to agree about it and the only thing stopping them is that they read
/// the same line.
inline f32 Sc2EmitWindow(bool fullStep, f32 subDt, f32 frameDt) {
    return fullStep ? subDt : frameDt;
}


// ---------------------------------------------------------------------------
// SPAWN — position and velocity (`SampleSpawnPosition` / `SampleSpawnVelocity`,
// RE §5.6-5.7, gates OP4 and OP5).
//
// Both draw from the emitter's generator, and the DRAW ORDER is as much of the
// contract as the arithmetic: two shapes that produce the same distribution
// from different stream positions are different emitters the moment anything
// else draws in the same frame. Every ordering below is one the golden
// separated, not one that reads naturally.
// ---------------------------------------------------------------------------

/// Emitter shape, runtime numbering. The M3 spec has 6 and 7 swapped; this is
/// the order the binary dispatches (RE §5.6).
enum class Sc2SpawnShape : u32 {
    Point = 0,
    Plane = 1,
    Sphere = 2,
    Box = 3,
    Cylinder = 4,
    Disc = 5,
    Spline = 6,
    Mesh = 7,
};

// ---------------------------------------------------------------------------
// SPAWN — the Mesh shape (`SampleEmitterMeshSurface`, RE §16.14, gate OP13).
//
// The loop rejects POSITIONS, never particles: four draws an attempt (the
// triangle, two barycentrics, the mask byte) and the 32nd attempt is taken
// whether or not its mask passed. A dark vertex mask therefore moves the
// emitter's whole stream, which is why the draws are burned even when nothing
// needs them.
// ---------------------------------------------------------------------------

/// One entry of an emitter-region slot's triangle table.
struct Sc2MeshTriangle {
    u32 firstIndex = 0; ///< into the face buffer
    u32 region = 0;     ///< which region record biases the three indices
};

/// A region record's vertex base. Retail adds BOTH terms — REGN+4, which
/// WhiteoutLib calls `unknown`, and `firstVertex` — and the first is 0 in all
/// 75,031 shipped regions, so only a fixture can show the sum.
struct Sc2MeshRegionBase {
    u32 bias = 0;
    u32 firstVertex = 0;
};

struct Sc2MeshSurfaceInputs {
    /// `model+0x150` and `asset+0x88`. Either missing returns before the pick
    /// and draws nothing.
    bool haveAsset = true;
    bool haveVertexDesc = true;
    /// The emitter's slot. Empty is both the null table and the zero-count
    /// one: the pick is called, refuses, and draws nothing.
    std::span<const Sc2MeshTriangle> triangles{};
    std::span<const u32> faces{};
    std::span<const Sc2MeshRegionBase> regions{};
    /// R of each vertex's BGRA colour (the byte at colour offset + 2). Empty
    /// is a vertex format without colour, which reads 255 at every corner.
    std::span<const u8> colorR{};
    /// `M3_ComputeSkinnedRegionPositions` for one absolute vertex index, in
    /// model space. A function pointer for the collider's reason: this runs
    /// three times a particle.
    void* ctx = nullptr;
    Vector3f (*position)(void* ctx, u32 vertex) = nullptr;
};

struct Sc2MeshSample {
    bool hit = false;
    Vector3f position{0, 0, 0};
    /// The winding's face normal, exactly `(0,0,1)` on a zero-area triangle.
    Vector3f normal{0, 0, 0};
    /// Calls to the triangle pick — a refused one included.
    u32 tries = 0;
};

/// `SampleEmitterMeshSurface` — model space.
Sc2MeshSample Sc2SampleMeshSurface(sc2::Rng& rng, const Sc2MeshSurfaceInputs& in);

struct Sc2SpawnPosInputs {
    Sc2SpawnShape shape = Sc2SpawnShape::Point;
    /// `flags & 0x10` — hollows the shape: the radius becomes a range and the
    /// box picks one face pair.
    bool cutout = false;
    Vector3f shapeOuter{0.0f, 0.0f, 0.0f};
    Vector3f shapeInner{0.0f, 0.0f, 0.0f};
    f32 outerRadius = 0.0f;
    f32 innerRadius = 0.0f;
    /// The element's scale. The plane reads it OFF BY ONE — see the .cpp.
    Vector3f elemScale{1.0f, 1.0f, 1.0f};
    /// Spline control points, four per segment.
    std::span<const Vector3f> spline{};
    f32 splineLowerBound = 0.0f;
    f32 splineUpperBound = 1.0f;
    /// Mesh (shape 7) only. Null is an emitter pointed at no asset: nothing is
    /// drawn and the origin comes back.
    const Sc2MeshSurfaceInputs* mesh = nullptr;
};

/// `CParticleSystem::SampleSpawnPosition` — emitter space, except the Mesh
/// shape, whose points are the posed model's.
///
/// @p normal receives the Mesh shape's face normal and is left alone by every
/// other shape. Retail passes it only for velocityType 4 and zeroes it first,
/// so a type-4 emitter on any other shape spawns with no velocity at all.
Vector3f Sc2SampleSpawnPosition(sc2::Rng& rng, const Sc2SpawnPosInputs& in,
                                Vector3f* normal = nullptr);

/// One overlay group: `M3_SampleAnimValue(type, freq·variationTime +
/// variationPhase, amplitude)`. `type == 0` is "unarmed" and draws nothing.
struct Sc2Overlay {
    u32 type = 0;
    f32 amplitude = 0.0f;
    f32 frequency = 0.0f;
};

struct Sc2SpawnVelInputs {
    /// 0 cone, 1 radial, 2 axis, 3 random, 4 mesh normal.
    u32 velocityType = 0;
    f32 spawnYaw = 0.0f;    ///< degrees
    f32 spawnPitch = 0.0f;  ///< degrees
    f32 spawnHorizontal = 0.0f; ///< radians
    f32 spawnVertical = 0.0f;   ///< radians
    f32 speed = 0.0f;
    f32 speedRandom = 0.0f;
    /// `additionalFlags & 1`: the speed becomes `Rand(speed, speedRandom)` and
    /// the speed OVERLAY is ignored.
    bool speedIsEndpoint = false;
    /// `rotationFlags & 8`: drop z, rescale xy to the original magnitude.
    bool flattenXY = false;
    Vector3f position{0.0f, 0.0f, 0.0f}; ///< the spawn position (radial/axis)
    Vector3f normal{0.0f, 0.0f, 1.0f};   ///< the mesh face normal (type 4)
    f32 variationTime = 0.0f;
    f32 variationPhase = 0.0f;
    /// Groups 0, 1, 2, 7, 8 of `pOverlayParams`. Group 0 modulates **yaw** and
    /// group 1 **pitch** — the swap RE §11.5 records, kept because the file
    /// field names are the ones that are wrong.
    Sc2Overlay yawOverlay{};
    Sc2Overlay pitchOverlay{};
    Sc2Overlay speedOverlay{};
    Sc2Overlay horizontalOverlay{};
    Sc2Overlay verticalOverlay{};
};

/// `CParticleSystem::SampleSpawnVelocity` — emitter space, speed folded in.
Vector3f Sc2SampleSpawnVelocity(sc2::Rng& rng, const Sc2SpawnVelInputs& in);


// ---------------------------------------------------------------------------
// SPAWN — the three attribute samplers (`SampleParticleColor` / `..Size` /
// `..Rotation`, RE §5.8, gate OP6).
//
// Each reads exactly ONE overlay group — alpha 4, size 3, rotation 6 — and
// samples it BEFORE its own random draws. That order only became observable
// once the gate armed a wave type that draws (type 5); with the wave types the
// original grid used, nothing could tell the two orders apart.
// ---------------------------------------------------------------------------

struct Sc2ColorInputs {
    /// Packed BGRA, so as a u32 the ALPHA is the HIGH byte. start/mid/end.
    std::array<u32, 3> keys{};
    /// The endpoints each key lerps toward under @ref randomEnable.
    std::array<u32, 3> randomKeys{};
    bool randomEnable = false;
    f32 colorMidTime = 0.0f;
    f32 alphaMidTime = 0.0f;
    Sc2Overlay alphaOverlay{};
    f32 variationTime = 0.0f;
    f32 variationPhase = 0.0f;
};

/// `CParticleSystem::SampleParticleColor` — three packed BGRA nodes.
std::array<u32, 3> Sc2SampleColor(sc2::Rng& rng, const Sc2ColorInputs& in);

struct Sc2SizeInputs {
    std::array<f32, 3> keys{};        ///< start/mid/end
    std::array<f32, 3> randomKeys{};
    bool randomEnable = false;
    Sc2Overlay sizeOverlay{};
    /// The caller's scale ratio (`blend`), applied after the halving.
    f32 blend = 1.0f;
    u32 instanceType = 0;
    f32 instanceDistance = 0.0f;
    f32 variationTime = 0.0f;
    f32 variationPhase = 0.0f;
};

/// `CParticleSystem::SampleParticleSize` — HALF extents, as floats. The ×256
/// quantisation to the element's u16 belongs to `InitSpawnedParticles` (OP8),
/// not here. `.w` is `instanceDistance` only for instance type 9.
std::array<f32, 4> Sc2SampleSize(sc2::Rng& rng, const Sc2SizeInputs& in);

struct Sc2RotationInputs {
    std::array<f32, 3> keys{};        ///< start/mid/end, RADIANS
    std::array<f32, 3> randomKeys{};
    bool randomEnable = false;
    /// `rotationFlags & 2` — the mid and end keys become deltas on the running
    /// value instead of offsets from the overlay.
    bool relative = false;
    f32 rotationMidTime = 0.0f;
    Sc2Overlay rotationOverlay{};
    f32 variationTime = 0.0f;
    f32 variationPhase = 0.0f;
};

/// `CParticleSystem::SampleParticleRotation` — radians.
std::array<f32, 3> Sc2SampleRotation(sc2::Rng& rng, const Sc2RotationInputs& in);


// ---------------------------------------------------------------------------
// SPAWN — `InitSpawnedParticles` (RE §5.5, gate OP8).
//
// The five samplers above in one function, plus the space transform. OP4/5/6
// pin each sampler's own draw order; this pins the order they are CALLED in,
// which no per-kernel gate can see.
//
// Note the overlap with `Sc2SpawnInputs` in `sc2_runtime.h`: that one is the
// emitter's per-spawn block (and carries the X6 mesh fields), this one is what
// the gate drives. The emitter loop fills one from the other in a single
// place; they are deliberately not merged while the mesh half is unwritten.
// ---------------------------------------------------------------------------

/// The fields `InitSpawnedParticles` writes, in retail's units — sizes and
/// rotations already quantised, colours still packed BGRA.
struct Sc2SpawnedElement {
    Vector3f position{0, 0, 0};
    Vector3f velocity{0, 0, 0};
    Vector3f orientVec{0, 0, 0};
    Vector3f spawnOrigin{0, 0, 0};
    f32 invMass = 0.0f;
    f32 noisePhase = 0.0f;
    f32 trailAccum = 0.0f;
    f32 birthTime = 0.0f;
    f32 deathTime = 0.0f;
    f32 flipbookRandStart = 0.0f;
    std::array<u16, 4> size{};       ///< half extents ×256, truncated
    std::array<u16, 3> rotation{};   ///< radians ×32, truncated
    std::array<u32, 3> colorNodes{};
    u16 flipbookRand = 0;
    /// `vNoiseVector.xyz`. Written by the noise stage, not at spawn — every
    /// OP8 vector left it at the fixture's zero, so treat a zero here as "no
    /// noise stage has run yet", never as a measured value.
    Vector3f noiseVec{0, 0, 0};
    u16 flags = 0;                   ///< `(PAR_.flags · 2) & 0xC`, plus bit 0 for a trail
    i32 vbSlot = -1;
    i32 bounceCount = 0;
};

/// What the emitter carries across a batch and this function advances.
struct Sc2InitState {
    f32 emitterTime = 0.0f;
    Vector3f curPos{0, 0, 0};
    /// A running MAXIMUM in milliseconds over every element the emitter ever
    /// spawns, so it never retreats while the emitter lives.
    u32 expireFrameMs = 0;
};

struct Sc2InitInputs {
    Sc2SpawnPosInputs shape{};
    Sc2SpawnVelInputs velocity{};
    Sc2ColorInputs color{};
    Sc2SizeInputs size{};
    Sc2RotationInputs rotation{};

    u32 parFlags = 0;
    u32 additionalFlags = 0;
    u32 rotationFlags = 0;
    u32 instanceType = 0;
    /// `CParticleSystem::emitFlagsWord`, not a `PAR_` field: bit 3 arms the
    /// per-particle noise phase.
    u32 emitFlagsWord = 0;
    /// Bit 3 selects, INSIDE the world arm only, the normalised basis plus the
    /// inherited parent velocity. It does nothing on the local path.
    u32 stateFlags = 0;

    f32 noiseCoherence = 0.0f;
    f32 mass = 1.0f;
    f32 massRandom = 0.0f;
    f32 lifetime = 0.0f;
    f32 lifetimeRandom = 0.0f;
    f32 trailChance = 0.0f;
    u16 flipbookColumns = 0;
    u16 flipbookRows = 0;
    bool hasChildEmitter1 = false;

    /// 0 for the `PAR_` itself, `PARC` copy index + 1 otherwise.
    u32 slot = 0;
    Matrix44f worldMatrix = Matrix44f::identity();
    Matrix44f boneMatrix = Matrix44f::identity();
    bool hasBone = false;

    Vector3f spawnPosStep{0, 0, 0};
    f32 spawnTimeStep = 0.0f;
    Vector3f smoothedPos{0, 0, 0};
    f32 inheritVelocityScale = 0.0f;
    u32 nowMs = 0;

    /// One per element, or empty. A request also forces the LOCAL space path
    /// even on an emitter that would otherwise use the world basis.
    std::span<const SpawnRequest> requests{};
};

/// `CParticleSystem::InitSpawnedParticles` over a batch of pending elements.
void Sc2InitSpawned(sc2::Rng& rng, const Sc2InitInputs& in, Sc2InitState& state,
                    std::span<Sc2SpawnedElement> out);

// ---------------------------------------------------------------------------
// SPAWN — the batch order (`SpawnParticles`, RE §15.5 and §16.9, gate OP8b).
// ---------------------------------------------------------------------------

struct Sc2SpawnBatchInputs {
    u32 requests = 0;     ///< waiting in the inbox
    u32 plain = 0;        ///< this call's own count
    u32 elementCount = 0; ///< alive before the call
    u32 maxParticles = 0;
};

/// One `InitSpawnedParticles` call. Its first `requests` elements are
/// request-born, from inbox index `requestBegin` on; the rest are plain.
struct Sc2SpawnFlush {
    u32 requestBegin = 0;
    u32 requests = 0;
    u32 plain = 0;
};

struct Sc2SpawnBatchPlan {
    u32 created = 0;
    std::vector<Sc2SpawnFlush> flushes;
    /// `InitSpawnedParticles` zeroes the request count whenever it runs. So
    /// the inbox is emptied — requests the ceiling refused included — only
    /// when some flush ran; a call that created nothing leaves every request
    /// waiting for the next one.
    bool requestsConsumed = false;
};

/// Requests first, then plain particles, each ONE element guarded by
/// `elementCount < maxParticles` in turn; the pending list flushes at 128 from
/// the request loop only, and once more at the end. The flushes are what make
/// the swept clock and the request pointers line up with retail's.
Sc2SpawnBatchPlan Sc2PlanSpawnBatch(const Sc2SpawnBatchInputs& in);


// ---------------------------------------------------------------------------
// MOVE — the analytic variant (`Particle.fx:236`, gates OP1 + OP12 `proc`).
//
// An emitter `Sc2CanUseGpuMotion` accepts never integrates on the CPU: retail
// uploads the birth state once and `CalculatePositionAndVelocity` re-derives
// the position from the elapsed time in the vertex shader, every frame, from
// scratch. This host builds its quads on the CPU, so the same closed form runs
// here — `sc2::vs::CalculateDisplacementAndVelocity`, already O12-gated, with
// this function supplying the arguments in the shader's order.
//
// **The closed form is numerically hostile at the drag floor, which is also
// its most common setting.** OP11 measures the builder clamping drag to
// `max(0.01, drag)` and writing `invDrag = 100` under it, so every emitter
// that authors no drag lands exactly there; the displacement is then a
// difference of two terms of magnitude `mass² · gravity · invDrag²` — 5.1e6
// for the gate's fixture — whose float32 ulp is 0.5. One ulp of `exp` moves
// the answer by 12.5%. That is a property of the shipped shader, not of this
// transcription: retail's particles carry it too. It is why @ref
// Sc2StepAnalytic is a transcription of the source's operation order rather
// than of its algebra, and why the gate's tolerance on the result is derived
// from the inputs instead of being a flat relative bound.
// ---------------------------------------------------------------------------

/// `b_iInstanceType` — the shader's eleven quad shapes, in its own order.
///
/// Named here rather than in `Sc2EmitterDesc` because the shader is what reads
/// the number: the descriptor carries the authored byte, this is what the
/// permutation means by it.
enum class Sc2InstanceType : u32 {
    Billboard = 0,
    Tail = 1,
    FaceTravelDir = 2,
    FaceWorldDir = 3,
    SingleAxis = 4,
    TerrainOriented = 5,
    TerrainDirOriented = 6,
    EmitterOriented = 7,
    PhysicsOriented = 8,
    Pinned = 9,
    Trail = 10,
};

struct Sc2AnalyticInputs {
    /// The element's birth position — `vPosition.xyz`, already in whatever
    /// space the batch will draw it in.
    Vector3f position{0, 0, 0};
    /// `vInterpolator1` — the birth velocity and the inverse mass.
    Vector3f velocity0{0, 0, 0};
    f32 invMass = 1.0f;
    /// `vBirthDeathAndDrag`. `invDrag` is an uploaded lane, NOT `1 / drag`:
    /// under the floor the builder writes 100 against a drag of 0.01, and the
    /// pair only stays reciprocal above it.
    f32 birthTime = 0.0f;
    f32 deathTime = 1.0f;
    f32 drag = 0.01f;
    f32 invDrag = 100.0f;
    /// `vInterpolator2.w`. Stored NEGATED — the shader passes `-gravityZ` into
    /// the closed form, so a falling particle carries a positive lane here.
    f32 gravityZ = 0.0f;
    /// `p_vSystemTime.x`, the batch's emitter clock.
    f32 systemTime = 0.0f;

    u32 instanceType = 0;
    /// `vInterpolator2.x` for the stretched types: the authored tail length.
    f32 tailLength = 0.0f;
    /// The interpolated size the clamp budgets against — `InterpolateValue`'s
    /// scalar overload at this element's age, which the caller has already.
    f32 size = 1.0f;
    bool fixedTailLength = false;
    bool clampedTailLength = false;
};

struct Sc2AnalyticStep {
    Vector3f position{0, 0, 0};
    Vector3f displacement{0, 0, 0};
    Vector3f velocity{0, 0, 0};
    /// `saturate((now − birth) / (death − birth))`, the age every sampler runs
    /// on. Returned because the caller needs the same one and recomputing it
    /// is how the two drift apart.
    f32 age = 0.0f;
    /// Seconds since birth, clamped at zero — `Bug 187607: no negative time
    /// skew`, in the source's own words. A particle whose batch clock has not
    /// reached its birth instant sits at its spawn point instead of running
    /// the trajectory backwards.
    f32 elapsed = 0.0f;
    /// `vInterpolator2.x` after the step: the possibly-shortened tail for the
    /// stretched types, and the velocity's x for a travel-direction instance,
    /// which writes the whole velocity into that register.
    f32 tailLength = 0.0f;
};

/// `Particle.fx`'s `CalculatePositionAndVelocity` — the whole MOVE for an
/// analytic emitter.
Sc2AnalyticStep Sc2StepAnalytic(const Sc2AnalyticInputs& in);

// ---------------------------------------------------------------------------
// RETIRE — `CParticleSystem::RetireExpiredParticles` (0x1029372D0, gate OP9).
//
// The analytic path's whole per-frame step. It integrates nothing: it walks
// the live list once and unlinks everything whose `deathTime` has been reached,
// returning each one's vertex-buffer slot to the recycle array.
//
// The list is intrusive and doubly linked, and both directions are load
// bearing — though not the way this comment first said. `UploadGpuParticles`
// walks FORWARD from the head by default; the backward walk from the tail, and
// the draw dispatcher's choice of tail over head, are both gated on
// `PAR_.flags & SortReverse` (0x100). So the reverse chain is the draw order
// for sort-reverse emitters and for nobody else. Retail never writes the tail
// directly — it writes `node->next->listPrev`, and the last node's `next` is
// the sentinel, whose own `listPrev` IS the tail. @ref Sc2ElementList models
// that literally rather than maintaining a tail by hand, because the literal
// version is what makes the reverse walk fall out correct at every topology.
// ---------------------------------------------------------------------------

/// Retail's `pElementBuffer` list, over indices into a caller-owned array.
///
/// `kSentinel` stands for the sentinel node; `kNull` for a genuine null link,
/// which is what terminates the backward walk.
struct Sc2ElementList {
    static constexpr i32 kSentinel = -1;
    static constexpr i32 kNull = -2;

    std::vector<i32> next;
    std::vector<i32> prev;
    i32 head = kSentinel;
    /// The SENTINEL's own `listPrev` — the last live node.
    i32 tail = kNull;
    i32 freeHead = kNull;
    i32 freeTail = kNull;
    /// `pElementBuffer + 224`, the pool's own count, separate from the
    /// system's `elementCount` even though both move together here.
    u32 poolCount = 0;

    /// Link `count` elements in index order, as a freshly filled pool is.
    void Reset(usize count);
    /// Live nodes from the head forward.
    void Walk(std::vector<i32>& out) const;
    /// Live nodes from the tail backward — `UploadGpuParticles`' order.
    void WalkBackward(std::vector<i32>& out) const;
    void WalkFree(std::vector<i32>& out) const;
    void Unlink(i32 node);
};

/// The `ParticleVB` slots retirement hands back, in the order it frees them.
///
/// Retail grows this by a fixed increment rather than doubling, and a system
/// with no array at all simply keeps the slot — which is why @ref enabled is a
/// field and not the emptiness of @ref slots.
struct Sc2RecycleArray {
    bool enabled = true;
    u32 capacity = 8;
    u32 growth = 8;
    std::vector<i32> slots;
};

/// `RetireExpiredParticles`: unlink every element at or past its death time.
///
/// Returns how many were retired. `deathTime <= emitterTime` — equality kills,
/// which the gate pins one ULP either side of.
u32 Sc2RetireExpired(Sc2ElementList& list, std::span<Sc2SpawnedElement> elements,
                     f32 emitterTime, Sc2RecycleArray& recycle);

// ---------------------------------------------------------------------------
// MOVE, Euler variant — `CParticleSystem::SimulateParticles` (gate OP9).
//
// The step an emitter takes when `CanUseGpuMotion` refused it: once per
// sub-step, integrate every live element, then run what the closed form cannot
// express — the type-6 freeze, collision with its bounce, friction and rest,
// die-on-bounce, the bounds pass and both kill tests. RE §6 is the order and
// §16.10 the measured detail.
//
// The two child queues are here: a collision spawn onto child 0 and trails
// onto child 1, returned as requests rather than pushed into another emitter,
// because nothing at this layer knows what an emitter is. The model-particle
// pose is X6's.
// ---------------------------------------------------------------------------

/// One contact for the swept segment `from → to`, in the element's own space.
struct Sc2Contact {
    bool hit = false;
    Vector3f position{0, 0, 0};
    Vector3f normal{0, 0, 1};
    /// Where along the segment. The step forces a terrain contact to 1 itself;
    /// only an object query can leave a fraction behind.
    f32 toi = 1.0f;
};

/// The scene's two collision queries.
///
/// Both answer in WORLD space. A local-space emitter's segment goes out
/// through its matrix before the call and the contact comes back through the
/// adjugate inverse inside the step (RE §16.10), so a query never has to know
/// what space an emitter simulates in.
///
/// Function pointers over a context rather than `std::function`: this runs per
/// particle per sub-step, and retail's own swept-sphere assert already shows
/// what a per-call cost there does to a busy map.
struct Sc2Collider {
    void* ctx = nullptr;
    /// `CollideParticle`, element flag 0x4. Report the RAW contact — the step
    /// pushes it out by 0.05 along the normal and overwrites its time.
    bool (*terrain)(void* ctx, const Vector3f& from, const Vector3f& to, Sc2Contact& out) = nullptr;
    /// `ForwardParticleSystemQuery`, element flag 0x8. Wins over a terrain
    /// contact outright when both bits are set.
    bool (*objects)(void* ctx, const Vector3f& from, const Vector3f& to, Sc2Contact& out) = nullptr;
};

struct Sc2SimulateInputs {
    f32 dt = 0.0f;
    /// The emitter clock the kill test and the rest re-key both read.
    f32 emitterTime = 0.0f;
    /// `(gravityX, gravityY, gravity)`, unscaled.
    Vector3f gravity{0, 0, 0};
    /// The scene's multiplier, applied only under `PAR_.flags & 0x20000`.
    /// WhiteoutLib names that bit MultiplyGravityByMass; the runtime multiplies
    /// by THIS, and OP9 pins it with a scale of 0.25 against a mass of one.
    f32 gravityScale = 1.0f;
    /// `M3_SampleWindNoise`'s sample. The step applies the multiplier once.
    Vector3f wind{0, 0, 0};
    f32 windMultiplier = 0.0f;
    u32 parFlags = 0;
    u32 instanceType = 0;
    f32 drag = 0.0f;
    f32 bounce = 0.0f;
    f32 friction = 0.0f;
    u32 collisionDieBounce = 0;
    f32 killRadius = 0.0f;
    Vector3f origin{0, 0, 0};
    /// The scene's collision switch. Nothing is queried without it.
    bool collisionEnabled = false;
    /// The rest transition freezes the spin through this curve.
    i32 rotationSmoothing = 0;
    f32 rotationMidTime = 0.0f;
    f32 rotationMidHold = 0.0f;

    /// The emitter matrix, row-major. A local-space emitter maps the collision
    /// segment out through it and the contact back through its inverse; a
    /// world-space one uses neither.
    std::array<f32, 16> worldMatrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    /// `PAR_.additionalFlags & 8`.
    bool worldSpace = false;
    /// `CParticleSystem+0x3CC`, the sampled `trailEmissionRate`.
    f32 trailRate = 0.0f;

    // ---- the children (RE §16.10) ----
    /// Child 0 exists AND simulates in world space. Retail reads the second
    /// half off the child's own `PAR_`, which is why the desc resolves it at
    /// load (`collisionChildIsWorldSpace`).
    bool collisionChild = false;
    /// Child 1 exists. Retail queues to it unguarded — a trail flag with no
    /// child dereferences null — and only the spawn's own test keeps that from
    /// happening; this is the step's guard for the same case.
    bool trailChild = false;
    f32 collisionSpawnChance = 0.0f;
    u32 collisionSpawnMin = 0;
    u32 collisionSpawnMax = 0;
    f32 collisionSpawnEnergy = 0.0f;
    /// `splatProjectorIndex != -1`. The splat needs a projector the viewer does
    /// not have; its chance draw and the death it causes are kept.
    bool splat = false;
    f32 splatChance = 0.0f;
};

struct Sc2SimulateResult {
    u32 killed = 0;
    /// Maintained only under `PAR_.flags` bit 31, and left at the empty
    /// sentinel otherwise — as retail leaves it.
    Vector3f boundsMin{3.4028235e38f, 3.4028235e38f, 3.4028235e38f};
    Vector3f boundsMax{-3.4028235e38f, -3.4028235e38f, -3.4028235e38f};
};

/// What a sub-step asked of the two children, in the order it asked.
///
/// Uncapped on purpose. `QueueSpawnRequest`'s 128 is the RECEIVER's ceiling —
/// it counts everything waiting on that child, from every parent and every
/// sub-step — so it is applied where a request lands (@ref
/// Sc2QueueSpawnRequest). The randoms that built a dropped request were drawn
/// all the same.
struct Sc2ChildRequests {
    std::vector<SpawnRequest> collision; ///< to child 0
    std::vector<SpawnRequest> trail;     ///< to child 1
};

/// Retail's cap on one emitter's pending requests (`CParticleSystem+0x358`).
inline constexpr usize kSc2MaxSpawnRequests = 128;

/// `QueueSpawnRequest` (`0x102923CD0`): append while fewer than 128 wait, and
/// drop the rest without a word — a dropped request is a particle that never
/// spawns (OP9 `cap`).
void Sc2QueueSpawnRequest(std::vector<SpawnRequest>& queue, const SpawnRequest& req);

/// One element's integration and the type-6 freeze.
///
/// Gravity (unless resting), drag in two branches — linear below `k·dt < 1`,
/// and above it the velocity simply becomes `gravity·dt` — then `v += a·dt` and
/// `p += (v + wind)·dt`. Wind reaches the POSITION only, so it never
/// accumulates into the particle's own momentum.
void Sc2StepEuler(Sc2SpawnedElement& e, const Sc2SimulateInputs& in);

/// `SimulateParticles`: one sub-step over the live list. Returns how many died
/// and appends to @p children what the step asked of them, drawing the
/// collision-spawn, splat and trail randoms from @p rng in retail's order.
///
/// @p killed, when given, receives each unlinked node in the order it died.
/// That order is observable for a ModelParticles emitter: retail unregisters a
/// pending model particle by swap-remove at the kill, so the order of deaths
/// decides which element later receives which model (RE §16.16).
Sc2SimulateResult Sc2SimulateParticles(Sc2ElementList& list,
                                       std::span<Sc2SpawnedElement> elements,
                                       const Sc2SimulateInputs& in,
                                       const Sc2Collider& collider, sc2::Rng& rng,
                                       Sc2ChildRequests& children,
                                       std::vector<i32>* killed = nullptr);

/// `Update`'s push onto the children's nodes (RE §16.10, OP9 `scale`): the
/// emitter matrix's basis ROW lengths, a collapsed row pushing 0 rather than a
/// NaN. Rows 0 and 1 go through `rsqrtps` and row 2 through an exact root,
/// each with the same Newton step, so the three lanes are not one expression.
Vector3f Sc2ChildScale(const std::array<f32, 16>& world);

/// Where that push lands (RE §16.34): on the child's BONE, as its local scale,
/// replacing the one it had. So each basis row of @p childBone — the bone's
/// local matrix times its parents' — is rescaled by `pushed / localScale`, and
/// the translation row stays. A row whose local scale is zero has no direction
/// left to rescale and is kept.
Matrix44f Sc2PushChildScale(const Matrix44f& childBone, const Vector3f& localScale,
                            const Vector3f& pushed);

/// `Update`'s choice of step function (RE §6, gate OP9 `select`).
///
/// `forceCpu` is retail's debug byte; it demotes an analytic emitter back onto
/// the Euler path, which is the only way to compare the two on one emitter.
// ---------------------------------------------------------------------------
// The GPU vertex (OP11b, design P2 BUILD).
//
// `UploadGpuParticles` writes each element into the ParticleVB block ONCE —
// when it has no slot yet, or when the block moved under it — and never
// touches those bytes again; the vertex shader does the rest from `age`. So
// this is the analytic variant's entire vertex body, and there is no second
// chance to correct anything it gets wrong.
// ---------------------------------------------------------------------------

/// `Particle.fx`'s `Input`, byte for byte (RE §8.1).
///
/// Plain scalars rather than `Vector3f` on purpose: this is a vertex-buffer
/// layout, and the offsets below are asserted, not hoped for.
struct Sc2GpuVertex {
    f32 position[3];        ///< +0    `vPosition.xyz`
    /// +12. `vPosition.w`, which no shader path reads — `EmitParticleHPos`
    /// rebuilds the vector as `float4(vPosition.xyz, 1)`. Retail copies
    /// whatever the element's lane holds; nothing ever writes it.
    u32 positionW;
    u16 size[4];            ///< +16   `vSize`, ×256
    u32 color[3];           ///< +24   `cColor0/1/2`, BGRA
    u16 rotation[3];        ///< +36   `vRotation.xyz`, radians ×32
    u16 flipbookRand;       ///< +42   `vRotation.w`
    f32 birthTime;          ///< +44   `vBirthDeathAndDrag.x`
    f32 deathTime;          ///< +48   `.y`
    f32 drag;               ///< +52   `.z`, floored
    f32 invDrag;            ///< +56   `.w`, an uploaded lane and NOT `1/drag`
    /// +60. `iBatchIndex`, declared `uint4` over a UBYTE4 element; the shader
    /// reads lane .x and indexes every batched constant array with it.
    ///
    /// **Retail leaves this alone on the analytic path.** The body is copied
    /// in seven runs that cover element `0x44..0xB4` except `0x80`, so the
    /// four bytes here keep whatever the ParticleVB slot's previous owner
    /// left. The per-frame builder (`AppendParticleVertices`, the CPU and
    /// hardware-instanced paths) does write it. See @ref Sc2VertexBodyInputs.
    u32 batchIndex;
    f32 velocity[3];        ///< +64   `vInterpolator1.xyz`
    f32 invMass;            ///< +76   `.w` — the element's, not the 0 the
                            ///<        pending-spawn path writes there
    f32 instanceVec[3];     ///< +80   `vInterpolator2.xyz`, per instance type
    f32 gravityZ;           ///< +92   `.w`, read back NEGATED by the shader
    f32 noise[3];           ///< +96   `vNoiseVector.xyz`
    f32 flipbookRandStart;  ///< +108  `.w`
    i16 corner[2];          ///< +112  `vOffset`
};
static_assert(sizeof(Sc2GpuVertex) == 116, "the ParticleVB stride is 116");
static_assert(offsetof(Sc2GpuVertex, batchIndex) == 60);
static_assert(offsetof(Sc2GpuVertex, corner) == 112);

/// The four corners, in the order both vertex builders write them: as s16
/// pairs (-1,1) (1,1) (-1,-1) (1,-1).
inline constexpr i16 kSc2Corners[4][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, -1}};

/// `drag` and `invDrag` are two independent lanes, not a reciprocal pair.
struct Sc2DragLanes {
    f32 drag = 0.01f;
    f32 invDrag = 100.0f;
};

/// The floor and its reciprocal, in the builder's order.
///
/// The binary floors `drag` and then branches on the RAW value for `invDrag`,
/// taking a literal 100 below the floor. That branch is kept for fidelity, not
/// because it changes an answer: `1.0f / 0.01f` is bit-exactly `100.0f`, so
/// flooring first and dividing agrees on every input. A mutation to
/// `1 / out.drag` stayed green across all 63 OP11b vectors, and no vector
/// could separate them — they are extensionally the same function.
///
/// The claim that IS measured is a different one: `invDrag` is an uploaded
/// lane the shader reads on its own, so a shader-side `1/drag` is wrong. Every
/// OP12 vector had `invDrag == 1/drag` exactly until the grid gained a
/// deliberately inconsistent pair (RE §16.26).
Sc2DragLanes Sc2ComputeDragLanes(f32 drag);

struct Sc2VertexBodyInputs {
    f32 drag = 0.0f;
    /// `PAR_.gravity` times the scene's wind scale — 1.0 with no scene. The
    /// product is what lands in the vertex; the shader negates it.
    f32 gravity = 0.0f;
    f32 worldGravityScale = 1.0f;
    u32 instanceType = 0;
    f32 tailLength = 0.0f;
    Vector3f instanceAngle{0, 0, 0};
    /// The emitter's slot in the batched-constant arrays (`CParticleSystem`
    /// +0x34F, annotated `randSeed` in the IDB and renamed once it was clear
    /// nothing seeds a random stream with it: `Init` hands out
    /// `(previous + 1) & 0x1F` and `BuildRenderBatch_GPU` flags the emitter as
    /// `1 << it`). Retail's analytic path never writes it into the vertex —
    /// see @ref Sc2GpuVertex::batchIndex — so we do, and bind one emitter's
    /// constants at index 0 rather than inheriting a stale lane.
    u32 batchIndex = 0;
};

/// `vInterpolator2.xyz` for one instance type.
///
/// Types 1/10 put the tail length in x and zero y,z; 2 the velocity; 3/4 the
/// authored angle triple; 7/8 the element's orientation; 9 its spawn origin;
/// everything else — billboards included — zero.
Vector3f Sc2InstanceVector(const Sc2VertexBodyInputs& in, const Sc2SpawnedElement& e);

/// What `BuildParticleQuadVertices_List` / `_Ranged` read besides the element
/// (RE §16.12, gate OP11) — the CPU path's per-frame vertex builder.
struct Sc2CpuVertexInputs {
    u32 instanceType = 0;
    f32 tailLength = 0.0f;
    Vector3f instanceAngle{0, 0, 0};
    f32 drag = 0.0f;
    f32 gravity = 0.0f;
    f32 gravityScale = 1.0f;
    /// The size knee the tail clamp budgets against.
    f32 sizeMidTime = 0.5f;
    u32 parFlags = 0;
    /// `emitFlags & 8` — the runtime's noise bit, not a `PAR_` one.
    bool noise = false;
    f32 noiseAmplitude = 0.0f;
    f32 noiseFrequency = 0.0f;
    f32 noiseCoherence = 0.0f;
    f32 noiseEdge = 0.0f;
    f32 emitterTime = 0.0f;
    /// `stateFlags & 0x10`. Set, the builder refreshes the GPU lanes from
    /// `PAR_` and the element; clear, it keeps the element's cached ones.
    bool gpuMotion = false;
    u32 batchIndex = 0;
    /// `SampleVectorField2D` on the scene's wind object, for types 5 and 6.
    /// Null is a scene with no such object, which retail answers with a flat
    /// `(0,0,1)` and a zero gravity lane.
    void* fieldCtx = nullptr;
    void (*field)(void* ctx, f32 x, f32 y, f32 out[2]) = nullptr;
};

/// One element through the CPU quad builder.
///
/// @p cache is the element's GPU lanes — retail keeps them inside the element
/// (`0x44..0xB4`) and this store keeps them in its parallel vertex, which is
/// the same bytes by construction. The builder writes the element too: the
/// noise vector, the type-6 rotation re-key, and the tail clamp's latch.
void Sc2CpuVertexBody(const Sc2CpuVertexInputs& in, Sc2SpawnedElement& e, Sc2GpuVertex& cache);

/// The four vertices one element uploads.
///
/// `batchIndex` is written into all four. Retail's block path leaves those
/// bytes at whatever the slot last held, which is why OP11b fills the range
/// with a value keyed to the dword index: a body copy that skipped the lane
/// and one that wrote a constant are otherwise the same green.
std::array<Sc2GpuVertex, 4> Sc2VertexBody(const Sc2VertexBodyInputs& in,
                                          const Sc2SpawnedElement& e);

// ---------------------------------------------------------------------------
// The quad expansion (OP12, design P2 BUILD).
//
// Everything above hands the GPU a point; this is what the vertex shader makes
// of it. `Particle.fx`'s `ParticleVertexShader` interpolates the size, colour
// and rotation from `age`, optionally re-derives the position from the closed
// form (@ref Sc2StepAnalytic), then expands the corner offset into world space
// per instance type. Only the BILLBOARD branch is here — X4 brings the other
// ten, and @ref Sc2QuadResult::supported says so rather than quietly returning
// a billboard for a tail.
// ---------------------------------------------------------------------------

/// `p_*[iBatchIndex]`: the per-emitter constants the shader indexes.
struct Sc2QuadBatch {
    /// x size, y colour RGB, z colour alpha, w rotation — one mid-key set per
    /// interpolated channel, which is why they are four lanes and not one.
    std::array<f32, 4> midKey{0.5f, 0.5f, 0.5f, 0.5f};
    std::array<f32, 4> invMidKey{2.0f, 2.0f, 2.0f, 2.0f};
    std::array<f32, 4> hold{0, 0, 0, 0};

    f32 systemTime = 0.0f;
    f32 elementScale = 1.0f;
    /// `p_vSystemTime_...FlipbookMidKeyTime_FlipbookColumnCount.z`, the age at
    /// which the flipbook switches from the start run to the end run.
    f32 flipbookMidKeyTime = 0.5f;
    /// `.w`. Forced to 1 in the shader when it is 0 — "fixes integer overflow
    /// on c++ side", in the shipped comment.
    f32 flipbookColumns = 1.0f;
    /// `(startInit, startStop, endInit)`. The authored quad's fourth key is
    /// never uploaded, so it is dead on this path.
    std::array<f32, 3> flipbookFrames{0, 0, 0};
    std::array<f32, 2> cellSize{1.0f, 1.0f};

    /// Row-major, HLSL association — see `vs::MulPointMat4`.
    std::array<f32, 16> prWorld{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::array<f32, 16> instanceTransform{1, 0, 0, 0, 0, 1, 0, 0,
                                          0, 0, 1, 0, 0, 0, 0, 1};
};

struct Sc2QuadCamera {
    Vector3f billboardRight{1, 0, 0};
    Vector3f billboardUp{0, 0, 1};
    Vector3f direction{0, 1, 0};
    Vector3f eye{0, 0, 0};
};

struct Sc2QuadFlags {
    u32 instanceType = 0;
    /// The three `b_*Interpolation` modes, one per channel.
    i32 sizeInterp = 0;
    i32 colorInterp = 0;
    i32 rotationInterp = 0;
    bool localSpace = false;
    bool modelInstancing = false;
    bool proceduralPosition = false;
    /// `b_fixedTailLength` — read twice, meaning two things: in the quad it
    /// drops the speed term, in the procedural step it picks the clamp's
    /// budget (RE §16.13).
    bool fixedTailLength = false;
    /// `b_clampedTailLength` — the procedural step's `min` with no side
    /// effect; the CPU builder's 10000 latch is not on this path.
    bool clampedTailLength = false;
    bool randomFlipbookStart = false;
    /// `b_iUVMapping[slot] == PARTICLE_FLIPBOOK`.
    bool flipbookUv = false;
    /// `b_UVRandomOffsetEnable[slot]`, which the flipbook arm outranks.
    bool uvRandomOffset = false;
};

/// `Particle.fx`'s `Input` as the vertex DECLARATION hands it over, which is
/// not quite @ref Sc2GpuVertex: the hardware has already widened the u16 pairs
/// and normalised the UBYTE4 colours. The u32→float4 colour decode belongs to
/// the declaration and OP12 does not measure it, so it is not done here — the
/// caller supplies the three nodes already unpacked, exactly as the gate's
/// fixture does.
struct Sc2QuadInput {
    Vector3f position{0, 0, 0};
    /// The RAW u16s; the shader scales by 1/256 itself.
    std::array<f32, 4> size{256, 256, 256, 256};
    /// `cColor0/1/2`, RGBA in 0..1.
    std::array<std::array<f32, 4>, 3> color{};
    /// The raw u16 triple (the shader scales by 1/32) plus `flipbookRand`
    /// as `.w`.
    std::array<f32, 4> rotation{0, 0, 0, 0};
    f32 birthTime = 0.0f;
    f32 deathTime = 1.0f;
    f32 drag = 0.01f;
    f32 invDrag = 100.0f;
    /// `vInterpolator1` — the velocity and `invMass`.
    Vector3f velocity{0, 0, 0};
    f32 invMass = 1.0f;
    /// `vInterpolator2` — the instance vector and the gravity the shader
    /// negates on the way in.
    Vector3f instanceVec{0, 0, 0};
    f32 gravityZ = 0.0f;
    /// `vNoiseVector` — the offset and `flipbookRandStart`.
    Vector3f noise{0, 0, 0};
    f32 flipbookRandStart = 0.0f;
};

struct Sc2QuadCorner {
    Vector3f position{0, 0, 0};
    Vector2f uv{0, 0};
    Vector3f normal{0, 0, 0};
    Vector3f tangent{0, 0, 0};
    Vector3f binormal{0, 0, 0};
};

struct Sc2QuadResult {
    f32 age = 0.0f;
    f32 size = 0.0f;
    std::array<f32, 4> color{1, 1, 1, 1};
    std::array<Sc2QuadCorner, 4> corner{};
    /// False past the eleven types the shader has. The corners are still
    /// filled — as billboards, which is the shader's own final `else` — so
    /// nothing downstream reads uninitialised memory; no shipped record
    /// carries such a type.
    bool supported = true;
};

/// One UV for one corner: the flipbook cell, or the random per-particle tile.
Vector2f Sc2ParticleUv(const Sc2QuadInput& v, const i16 (&corner)[2], f32 age,
                       const Sc2QuadBatch& b, const Sc2QuadFlags& fl);

/// The four world-space corners of one particle.
Sc2QuadResult Sc2ExpandQuad(const Sc2QuadInput& v, const Sc2QuadBatch& b,
                            const Sc2QuadCamera& cam, const Sc2QuadFlags& fl);

inline bool Sc2UseRetirePath(u32 stateFlags, bool forceCpu) {
    return (stateFlags & 0x10u) != 0 && !forceCpu;
}

// ---------------------------------------------------------------------------
// The batch row (OP15)
// ---------------------------------------------------------------------------
//
// `Sc2QuadBatch` is what the vertex shader reads; this is what fills it.
// Retail's producer is `CParticleSystem::WriteInstanceConstants`, which writes
// one row of eight `p_*` constant arrays per emitter in a batch, indexed by the
// emitter's batch slot.
//
// It is a persistent row, not a value: two of its constants are deliberately
// NOT written under some configurations, and retail then draws with whatever
// the previous batch left there. @ref Sc2WriteQuadBatch keeps that shape - it
// updates a row in place rather than returning a fresh one - so the lanes
// retail leaves alone are visible as lanes this function does not assign.

/// The authored half of a batch row: the `PAR_` fields the producer reads.
struct Sc2BatchDesc {
    /// `p_vMidKeyTimes` — size, colour, alpha, rotation, in that order. The
    /// shader reads `.x/.y/.z/.w` in the same order, so the two agree
    /// independently of each other.
    f32 sizeMidTime = 0.5f;
    f32 colorMidTime = 0.5f;
    f32 alphaMidTime = 0.5f;
    f32 rotationMidTime = 0.5f;

    /// `p_vMidKeyHoldTimes`, the same four channels. Retail copies all sixteen
    /// bytes at once, which is why a fixture that varied one value could never
    /// have told the four lanes apart.
    f32 sizeMidHoldTime = 0.0f;
    f32 colorMidHoldTime = 0.0f;
    f32 alphaMidHoldTime = 0.0f;
    f32 rotationMidHoldTime = 0.0f;

    /// The three flipbook indices that reach the shader. The authored quad's
    /// fourth, `flipbookEndStopIndex`, is uploaded by nothing and read by
    /// nobody, so it is not carried here.
    u8 flipbookStartInitIndex = 0;
    u8 flipbookStartStopIndex = 0;
    u8 flipbookEndInitIndex = 0;
    f32 flipbookMidTime = 0.0f;
    /// Both must be non-zero or the whole flipbook degrades — see
    /// @ref Sc2WriteQuadBatch.
    u16 flipbookColumns = 0;
    u16 flipbookRows = 0;
    /// Authored fractions, NOT `1 / columns`. A sheet whose cells do not tile
    /// its texture exactly is expressible, and some do.
    f32 flipbookColumnFraction = 0.0f;
    f32 flipbookRowFraction = 0.0f;

    /// `additionalFlags & WorldSpace`. Its complement is the shader's
    /// `b_localSpace`.
    bool worldSpace = false;
};

/// The per-frame half: the two transforms and the emitter clock.
struct Sc2BatchFrame {
    /// The emitter's world matrix — `CElementBased::GetWorldMatrix`, which is
    /// the transform node's own, refreshed. Row-major.
    std::array<f32, 16> world{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    /// The model-instancing node's transform, when the emitter has one.
    std::array<f32, 16> instanceTransform{1, 0, 0, 0, 0, 1, 0, 0,
                                          0, 0, 1, 0, 0, 0, 0, 1};
    bool hasInstanceNode = false;
    f32 emitterTime = 0.0f;
};

/// `p_v..._ElementScale_...y` — the largest row length of the world matrix.
///
/// Retail computes it as `rsqrtss` plus one Newton-Raphson step rather than a
/// `sqrtss`, and guards a zero matrix to 0 instead of letting it divide. The
/// refined estimate is within about `2^-22` relative of the true root on real
/// hardware (and within one float32 ulp under the oracle's emulator), so this
/// uses `std::sqrt` and the OP15 replay allows this one lane a relative
/// tolerance where every other lane is compared bit for bit. The value scales
/// particle size, so the last ulp of it is not observable.
f32 Sc2ElementScale(const std::array<f32, 16>& world);

/// Fill one batch row, leaving unassigned exactly what retail leaves unwritten.
///
/// Two configurations write less than the whole row:
///   * a **world-space** emitter never writes `prWorld`. Every shader read of
///     it is guarded by `b_localSpace`, which is that same bit inverted, so
///     the row it leaves behind is never sampled.
///   * a flipbook needs **both** `flipbookColumns` and `flipbookRows`
///     non-zero. Fail either and `flipbookMidKeyTime` and `flipbookColumns`
///     degrade to 1, and `flipbookFrames` and `cellSize` are not written at
///     all — so a degraded emitter drawn after a flipbook one inherits its
///     cell size.
void Sc2WriteQuadBatch(Sc2QuadBatch& row, const Sc2BatchDesc& d,
                       const Sc2BatchFrame& f);

// ---------------------------------------------------------------------------
// OUTPUT — model particles: the pose (`UpdateModelParticle`, RE §16.15, gate
// OP14) and the path pick after MOVE (`ProcessPendingSpawns`, RE §16.16, gate
// OP14b).
// ---------------------------------------------------------------------------

/// `EvalAnimCurve2D` — the lane-wise sibling of `vs::InterpolateValue`, which
/// is what `EvalAnimCurve1D` already is (OP10). Not the shader's float3
/// overload: modes 1 and 4 group differently, and mode 4 substitutes values the
/// way the scalar evaluator does rather than lerping a control point.
std::array<f32, 4> Sc2EvalCurve2D(u32 mode, const std::array<f32, 4>& k0,
                                  const std::array<f32, 4>& k1,
                                  const std::array<f32, 4>& k2, f32 t, f32 mid, f32 hold);

/// The seven quaternions `Particle_InitStaticResources` builds, `(x, y, z, w)`,
/// as OP14 recorded them from the image's own yaw/pitch/roll path. Only four
/// differ: 0 == 2, 1 == 5, 3 == 4.
extern const std::array<std::array<f32, 4>, 7> kSc2ModelOrientPresets;

struct Sc2ModelPoseInputs {
    // ---- the element ----
    Vector3f position{0, 0, 0};
    Vector3f velocity{0, 0, 0};
    Vector3f orientVec{0, 0, 0};
    Vector3f spawnOrigin{0, 0, 0};
    /// `element+0x84`: what `ProcessPendingSpawns` drew under `rotationFlags &
    /// 0x80` — the slot the quad path calls `gpuVelocity`.
    Vector3f randomDirection{0, 0, 0};
    f32 birthTime = 0.0f;
    f32 deathTime = 1.0f;
    /// The element's own keys, read only under `rotationFlags & 4`: sizes as
    /// s16/256, rotations as s16/32 — the u16 lanes reinterpreted — and three
    /// BGRA words.
    std::array<u16, 3> elementSize{};
    std::array<u16, 3> elementRotation{};
    std::array<u32, 3> elementColors{};

    // ---- the emitter ----
    /// `CElementBased_GetWorldMatrix`, row-major.
    std::array<f32, 16> world{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    f32 emitterTime = 0.0f;
    /// `CParticleSystem+0x120`. Bit 0x80 reads a world-space type-7/8
    /// `orientVec` as one plain direction instead of a packed pair.
    u32 stateFlags = 0;
    /// The emitter's cached keys, read without `rotationFlags & 4`.
    std::array<f32, 3> sizeKeys{};
    std::array<f32, 3> rotationKeys{};
    std::array<u32, 3> colorKeys{};

    // ---- PAR_ ----
    u32 parFlags = 0;
    u32 additionalFlags = 0;
    u32 rotationFlags = 0;
    u32 instanceType = 0;
    u32 sizeSmoothing = 0;
    /// Drives the tint AND the alpha: there is no `alphaSmoothing`.
    u32 colorSmoothing = 0;
    u32 rotationSmoothing = 0;
    /// size, colour, alpha, rotation.
    std::array<f32, 4> midTime{0.5f, 0.5f, 0.5f, 0.5f};
    std::array<f32, 4> midHold{0.0f, 0.0f, 0.0f, 0.0f};
    Vector3f instanceAngle{0, 0, 0};
    f32 tailLength = 0.0f;
    /// `PAR_+0x5D0`'s raw bits non-zero — so a `-0.0` counts — and `+0x5D4`.
    bool legacyOrient = false;
    i32 orientVariant = 0;

    // ---- the model and the scene ----
    f32 modelAlpha = 1.0f;
    Vector3f modelTint{1, 1, 1};
    /// `sceneCtx+3024`, rows +336 / +352 / +368: the camera's right, its view
    /// direction and its up. Zero-length rows fall back to (1,0,0), (0,−1,0)
    /// and (0,0,1).
    std::array<Vector3f, 3> camera{Vector3f{1, 0, 0}, Vector3f{0, -1, 0}, Vector3f{0, 0, 1}};
    /// `SampleVectorField2D` under the particle, types 5 and 6. No field is a
    /// straight-up normal.
    bool haveTerrain = false;
    Vector3f terrainNormal{0, 0, 1};
};

/// The eight setter payloads that are maths (the last three are the model's
/// own blocks, handed over by address and not computed).
struct Sc2ModelPose {
    Vector3f position{0, 0, 0};
    Vector3f scale{1, 1, 1};
    Vector3f tint{1, 1, 1};
    f32 alpha = 1.0f;
    /// `(x, y, z, w)`.
    std::array<f32, 4> rotation{0, 0, 0, 1};
};

/// `CParticleSystem::UpdateModelParticle` — RE §16.15, gate OP14.
///
/// Kept whole even where the answer is useless: types 5 and 6 normalise
/// `instanceAngle` unguarded and hand back NaN for a zero one, an unauthored
/// tail scales a type 1 or 10 model to exactly zero, and types 1, 6, 9 and 10
/// evaluate a rotation they then discard. The caller clamps; the kernel stays
/// comparable against the golden.
Sc2ModelPose Sc2ModelParticlePose(const Sc2ModelPoseInputs& in);

/// The three rows a unit quaternion from `QuaternionFromMatrix3` was built
/// from: rotating the model's +X, +Y and +Z gives them, so they are also the
/// rows of the row-vector matrix that places the child.
std::array<Vector3f, 3> Sc2QuatRows(const std::array<f32, 4>& q);

struct Sc2PendingDraw {
    u32 pathIndex = 0;
    /// Drawn under `rotationFlags & 0x80`: a point in the CUBE normalised,
    /// biased toward the eight corners. Written with `gpuInvMass = 0`.
    bool hasDirection = false;
    Vector3f direction{0, 0, 0};
};

/// One pending entry of `ProcessPendingSpawns` — RE §16.16, gate OP14b.
///
/// Returns false, having drawn NOTHING, for a particle already dead
/// (`deathTime < emitterTime`; a death exactly on the clock still spawns) and
/// for an emitter with no model paths. Retail divides by zero on the second;
/// design §8 skips it before the draw, so the stream is unaffected.
bool Sc2PendingSpawnDraw(sc2::Rng& rng, f32 deathTime, f32 emitterTime, u32 pathCount,
                         bool randomDirection, Sc2PendingDraw& out);

} // namespace whiteout::flakes::renderer::particle
