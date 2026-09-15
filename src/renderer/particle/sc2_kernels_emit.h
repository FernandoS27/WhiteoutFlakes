#pragma once

// ============================================================================
// SC2 particle kernels — PREP and EMIT.
//
// The emit clock and its pre-roll, how much each slot wants, the squirt
// crossing, and how a frame's count is laid across its sub-steps.
//
// The contract is `particle_stages_sc2.h`'s: every routine the oracle
// recorded is a free function over an explicit inputs struct, called by the
// tick and by the replay alike, with the float operation ORDER the binary's.
// ============================================================================

#include "renderer/sc2/sc2_constants.h"
#include "renderer/sc2/sc2_rng.h"
#include "sc2_kernel_types.h"
#include "whiteout/flakes/types.h"
#include "whiteout/vector_types.h"

#include <array>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::particle {

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
    /// Where the last sweep ended: `Tick` refreshes it on the full step,
    /// `EmitParticles` on every sub-stepped frame that runs its loop
    /// (@ref Sc2SpawnSweep).
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
inline constexpr i32 kSc2PreRollBlockMs = sc2::kPreRollBlockMs;
/// Added to `timeOffset` before every block — so a pre-rolled emitter's clock
/// runs AHEAD of the scene by the whole pre-roll from then on.
inline constexpr f32 kSc2PreRollOffsetStep = sc2::kPreRollOffsetStep;

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
    /// `renderState == 14 || !enabled || (emitFlags & kEmitSuppressed)` — all
    /// three fold into one early zero.
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

struct Sc2SweepInputs {
    bool fullStep = false;
    u32 nSubSteps = 0;
    u32 total = 0; ///< @ref Sc2Schedule::total
    /// The clock's `prevPos` as `Sc2TickClock` left it this frame.
    Vector3f prevPos{0, 0, 0};
    Vector3f worldPos{0, 0, 0};
};

struct Sc2Sweep {
    Vector3f spawnPosStep{0, 0, 0}; ///< zero on the full step
    Vector3f prevPos{0, 0, 0};      ///< the clock's `prevPos` after the call
};

/// `EmitParticles`' position lane — RE §16.7, gate OP3b.
///
/// The sub-stepped arm measures the step from `prevPos` and THEN moves
/// `prevPos` to `worldPos`, busy or idle, so the next frame's sweep starts
/// where this one ended. Without that write `prevPos` stayed at the first
/// frame's position and every later sweep stretched back to it: on a moving
/// emitter each slot but the last spawned an ever-growing distance behind.
/// `nSubSteps == 0` leaves it alone (OP3b's vectors hold that arm at the
/// origin, so they do not discriminate).
Sc2Sweep Sc2SpawnSweep(const Sc2SweepInputs& in);

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

} // namespace whiteout::flakes::renderer::particle
