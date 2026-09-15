#pragma once

// ============================================================================
// SC2 particle kernels — PREP and EMIT: the emit clock and its pre-roll, how
// much each slot wants, the squirt crossing, and how a frame's count is laid
// across its sub-steps. The contract is `particle_stages_sc2.h`'s.
// ============================================================================

#include "renderer/particle/sc2/sc2_kernel_types.h"
#include "renderer/sc2/sc2_constants.h"
#include "renderer/sc2/sc2_rng.h"
#include "whiteout/flakes/types.h"
#include "whiteout/vector_types.h"

#include <array>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::particle::sc2 {

// ---------------------------------------------------------------------------
// PREP — the emit clock (`CElementBased::Tick`, RE §5.1, gate OP3).
// ---------------------------------------------------------------------------

/// Everything `Tick` carries between frames. Lives in @ref Runtime; passed
/// here by reference because the kernel mutates it exactly as retail does and
/// the golden records the mutated fields, not just the return.
struct EmitClock {
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
    /// (@ref SpawnSweep).
    Vector3f prevPos{0, 0, 0};
    /// `CParticleSystem+0x120`, the derived state word (`renderer::sc2::SystemStateFlag`).
    /// Carried here because the tick clears bit 31 on the restart check.
    u32 stateFlags = 0;
};

/// The per-frame inputs. `dtMs` is the host's milliseconds and stays an
/// integer through the time-scale multiply, which is where a negative scale
/// stops being a small negative step and becomes a 4.29-billion-ms one.
struct ClockInputs {
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
struct StepPlan {
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
    /// Gate-only: the golden pins it bit-exact and no stage reads it — the
    /// spawn sweep measures its own step from `prevPos`.
    Vector3f displacement{0, 0, 0};
    f32 subDt = 0.0f;
    f32 dt = 0.0f;
    f32 catchUp = 0.0f;
    f32 remainder = 0.0f;
};

/// `CElementBased::Tick(dtMs, timeScale)` — RE §5.1, gate OP3. Advances
/// @p clock and returns the plan EMIT walks; `nSteps` is capped at 100 only
/// AFTER the displacement is divided, and the seed tests `== 0` exactly.
/// See SC2_PARTICLE_RE.md §17.5.
StepPlan TickClock(EmitClock& clock, const ClockInputs& in);

// ---------------------------------------------------------------------------
// PREP — the pre-roll (`Tick`'s restart check + `EmitBurst`, RE §15.1 and
// §16.7, gates OP3 `restart` and OP3c).
// ---------------------------------------------------------------------------

/// What `Tick` does FIRST: before its once-per-frame test, so it runs even on
/// a frame the emitter has already ticked.
struct RestartCheck {
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
RestartCheck TickRestartCheck(EmitClock& clock, const ClockInputs& in);

/// The 33 ms block `EmitBurst` drives `Tick` with.
inline constexpr i32 kPreRollBlockMs = renderer::sc2::kPreRollBlockMs;
/// Added to `timeOffset` before every block — so a pre-rolled emitter's clock
/// runs AHEAD of the scene by the whole pre-roll from then on.
inline constexpr f32 kPreRollOffsetStep = renderer::sc2::kPreRollOffsetStep;

/// The lifetime peak `EmitBurst` budgets from: the max of the active
/// sequence's curve seeded at ZERO (so an all-negative curve pre-rolls
/// nothing), or the AnimRef's raw init value when there is no curve.
f32 PreRollPeak(std::span<const f32> curve, bool haveCurve, f32 initValue);

struct PreRollPlan {
    u32 budgetMs = 0;
    /// Blocks of `kPreRollBlockMs`. The loop is do-while, so any non-zero
    /// budget runs at least one and the total overshoots to the next 33.
    u32 blocks = 0;
};

/// `EmitBurst`'s schedule, with two design §8 departures: the peak is clamped
/// at zero, and the budget is `min(requested, peak·1000)` always.
/// See SC2_PARTICLE_DESIGN.md §16.4.
PreRollPlan PlanPreRoll(f32 peak, u32 requestedMs);

// ---------------------------------------------------------------------------
// EMIT — how much one slot wants (`ComputeEmitCount`, RE §5.2, gate OP7a).
// ---------------------------------------------------------------------------

/// One slot's ask for one frame. Slot 0 is the `PAR_` itself; `1..n` are its
/// `PARC` copies, whose rate and squirt keys come from their own record.
struct EmitCountInputs {
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
    /// and the track's flags carry bit 1. Filled by `renderer::sc2::CrossedSquirtAmount`
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

/// `CParticleSystem::ComputeEmitCount` — RE §5.2, gate OP7a. Returns a
/// FRACTIONAL count; the floor and carry are `EmitParticles`' (OP3b), in
/// `SpawnSchedule`. See SC2_PARTICLE_RE.md §17.5.
f32 ComputeEmitCount(const EmitCountInputs& in);

// ---------------------------------------------------------------------------
// EMIT — the squirt crossing (`M3Anim_CollectCrossedKeys`, RE §16.7, gate
// OP7b). A squirt fires when the playhead STEPS OVER a key: a cursor one past
// the last frame seen, every key up to now into a 128-entry sink. Every key in
// the crossed window is reported (design §8). See SC2_PARTICLE_RE.md §17.5.
// ---------------------------------------------------------------------------

/// One discrete track, as keyed for the playing sequence.
struct KeyTrack {
    std::span<const i32> times; ///< ascending frames
    std::span<const u16> values;
};

/// One live player: its track and its frame, already through the player's
/// offsets, time scale and loop modulo. Players with no track are simply not
/// passed — retail skips them WITHOUT consuming a cursor slot.
struct KeyPlayer {
    KeyTrack track;
    i32 frame = 0;
};

struct CrossedKey {
    u16 value = 0;
    i32 frame = 0;
};

/// `CParticleBatch`: the reported keys, one cursor per live player and the two
/// control bytes. Zeroed where retail's constructor leaves most of it as it
/// found it — no reader ever looks at what it left (design §8).
struct KeySink {
    static constexpr u32 kCapacity = 128;
    std::array<CrossedKey, kCapacity> keys{};
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
bool CollectCrossedKeys(std::span<const KeyPlayer> players, i32 bias, KeySink& sink);

/// `ComputeEmitCount`'s sum over the sink: each key read back as SIGNED, a
/// negative one contributing nothing.
f32 SquirtBurst(const KeySink& sink);

// ---------------------------------------------------------------------------
// EMIT — the sub-step split (`EmitParticles`, RE §5.3, gate OP3b).
// ---------------------------------------------------------------------------

/// The stage calls the schedule produces, in order. Recording them rather than
/// running them is what lets the replay test compare against the golden's own
/// event log — the spawns and the `Update`s interleave, and WHERE a pass
/// spawns nothing is as much a fact as how many it spawned.
enum class EmitEventKind : u8 {
    Count,   ///< `ComputeEmitCount` was asked for @ref EmitEvent::slot.
    PreEmit, ///< the pre-emit vtable hook.
    Spawn,   ///< spawn `count` on `slot` (may be 0 when only requests are due).
    Update,  ///< simulate one sub-step.
};

struct EmitEvent {
    EmitEventKind kind = EmitEventKind::Update;
    u16 slot = 0;
    u32 count = 0;
};

struct ScheduleInputs {
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
    /// Per slot, the value `ComputeEmitCount` returned — already MULTIPLIED
    /// by the window (@ref EmitWindow), which must not scale the squirt burst
    /// again. Ignored when `frozen`. See SC2_PARTICLE_RE.md §17.5.
    std::span<const f32> counts;
};

struct Schedule {
    /// The frame's per-slot budget. Deliberately unsigned: a negative rate
    /// floors to −1 and lands here as 0xFFFFFFFF, which spawns nothing (the
    /// guard is signed) but still divides `catchUp` into 4.29 billion parts.
    u32 total = 0;
    f32 spawnTimeStep = 0.0f;
    f32 accumTime = 0.0f;
};

/// `CParticleSystem::EmitParticles` — RE §5.3, gate OP3b. Fills @p targets and
/// @p events and returns the frame's totals; @p carry is per-slot in/out and
/// @p emitted per-slot scratch. Spawns and simulates nothing: the stage walks
/// @p events. The full step divides `catchUp / total`; the sub-stepped path
/// multiplies by `1 / total`. See SC2_PARTICLE_RE.md §17.5.
Schedule SpawnSchedule(const ScheduleInputs& in, std::span<f32> carry,
                             std::span<u32> targets, std::span<u32> emitted,
                             std::vector<EmitEvent>& events);

struct SweepInputs {
    bool fullStep = false;
    u32 nSubSteps = 0;
    u32 total = 0; ///< @ref Schedule::total
    /// The clock's `prevPos` as `TickClock` left it this frame.
    Vector3f prevPos{0, 0, 0};
    Vector3f worldPos{0, 0, 0};
};

struct Sweep {
    Vector3f spawnPosStep{0, 0, 0}; ///< zero on the full step
    Vector3f prevPos{0, 0, 0};      ///< the clock's `prevPos` after the call
};

/// `EmitParticles`' position lane — RE §16.7, gate OP3b. The sub-stepped arm
/// measures the step from `prevPos` and THEN moves `prevPos` to `worldPos`,
/// busy or idle; `nSubSteps == 0` leaves it alone. See SC2_PARTICLE_RE.md §17.5.
Sweep SpawnSweep(const SweepInputs& in);

/// The window `ComputeEmitCount` is asked for: one SUB-step on the full-step
/// path, the whole FRAME on the sub-stepped one. One line, so the schedule and
/// the counter cannot disagree.
inline f32 EmitWindow(bool fullStep, f32 subDt, f32 frameDt) {
    return fullStep ? subDt : frameDt;
}

} // namespace whiteout::flakes::renderer::particle::sc2
