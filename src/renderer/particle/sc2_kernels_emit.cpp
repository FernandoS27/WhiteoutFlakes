#include "sc2_kernels_emit.h"

#include "sc2_kernel_math.h"

#include "renderer/sc2/sc2_element.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::renderer::particle {

namespace {

namespace bits = whiteout::flakes::renderer::sc2;

/// Both bits arm the same multiply — see `kStateScaleTimeAlso`.
constexpr u32 kScaleTimeMask = bits::kStateScaleTimeByParent | bits::kStateScaleTimeAlso;

/// `Tick`'s sub-step ladder — 60 Hz below a 60th of a second, 30 Hz below a
/// 30th, 15 Hz otherwise — and the cap on the steps one frame may take.
constexpr f32 kSubStepHz60 = 60.0f;
constexpr f32 kSubStepHz30 = 30.0f;
constexpr f32 kSubStepHz15 = 15.0f;
constexpr u32 kMaxSubSteps = 100u;
/// Below this a rate cannot be inverted into a sub-step, and the full step
/// takes `min(1, dt)` instead.
constexpr f32 kMinInvertibleRate = 0.001f;

} // namespace

Sc2StepPlan Sc2TickClock(Sc2EmitClock& clock, const Sc2ClockInputs& in) {
    Sc2StepPlan plan;

    // Already ticked this frame: retail returns before touching anything, so a
    // second call in the same frame is not a second half-step.
    if (clock.lastFrameIndex == in.frameIndex)
        return plan;
    plan.ticked = true;

    const bool scaled = (clock.stateFlags & kScaleTimeMask) != 0;
    // The scaled step goes back into an UNSIGNED millisecond slot. That is not
    // pedantry: a time scale of −1 does not step backwards, it wraps to
    // 4,294,967,280 ms and the frame asks for a 4.29-million-second step, which
    // the 100-step cap is then the only thing standing between and a hang.
    const u32 dtMs =
        scaled ? static_cast<u32>(static_cast<i32>(static_cast<f32>(in.dtMs) * in.timeScale))
               : static_cast<u32>(in.dtMs);

    // Exact equality with zero, deliberately: an emitter whose time genuinely
    // lands on 0.0 after a frame is re-seeded again rather than left behind.
    if (clock.emitterTime == 0.0f)
        clock.emitterTime =
            static_cast<f32>(in.nowMs - static_cast<i32>(dtMs)) * sc2::kSecondsPerMs;

    plan.dt = static_cast<f32>(dtMs) * sc2::kSecondsPerMs;
    plan.catchUp = (clock.stateFlags & bits::kStateUseLocalTime) != 0
                       ? plan.dt
                       : (static_cast<f32>(in.nowMs) * sc2::kSecondsPerMs + in.timeOffset) -
                             clock.emitterTime;
    clock.variationTime += plan.dt;

    // `subStepRate` is only ever tested against zero — it is never the rate.
    f32 rate = 0.0f;
    if (in.subStepRate != 0.0f) {
        rate = in.force60Hz                    ? kSubStepHz60
               : plan.dt < 1.0f / kSubStepHz60 ? kSubStepHz60
               : plan.dt < 1.0f / kSubStepHz30 ? kSubStepHz30
                                               : kSubStepHz15;
    }

    const bool skippedFrame = clock.lastFrameIndex < in.frameIndex - 1;
    if (rate == 0.0f || skippedFrame || in.modelPaused ||
        (clock.stateFlags & bits::kStateSquirtResync) != 0) {
        plan.fullStep = true;
        plan.nSteps = 1;
        plan.subDt = rate >= kMinInvertibleRate ? 1.0f / rate : (std::min)(1.0f, plan.dt);
        plan.remainder = 0.0f;
        // `Tick`'s only `prevPos` write; the sub-stepped path's is
        // `EmitParticles`', after it has measured the frame (`Sc2SpawnSweep`).
        clock.prevPos = in.worldPos;
        clock.lastSubStepTime = clock.variationTime;
        // The resync is one-shot: it buys this one full step and is spent. The
        // golden cannot say whether retail clears it here or before the test —
        // both produce the same frame — so it is cleared where it is consumed.
        clock.stateFlags &= ~static_cast<u32>(bits::kStateSquirtResync);
    } else {
        const f32 gap = clock.variationTime - clock.lastSubStepTime;
        plan.subDt = 1.0f / rate;

        const f32 wanted = std::floor(gap * rate);
        u32 n = wanted <= 0.0f ? 0u
                : wanted >= 4294967296.0f
                    ? 0xFFFFFFFFu   // unreachable at any sane gap; keeps the cast defined
                    : static_cast<u32>(wanted);
        plan.remainder = gap - static_cast<f32>(n) * plan.subDt;

        if (n != 0) {
            // Divided by the UNCAPPED count and only then capped, so a frame
            // that wants 200 steps takes 100 steps of half the displacement and
            // covers half the distance. Retail's behaviour, not a rounding
            // artifact — a fast emitter genuinely falls behind its own motion.
            //
            // TWO vector multiplies, not one folded scalar: retail scales the
            // delta by `1/n` and then by the covered fraction, and at n = 200
            // folding them costs an ulp the golden sees.
            const f32 inv = 1.0f / static_cast<f32>(n);
            const f32 covered = (static_cast<f32>(n) * plan.subDt) / gap;
            plan.displacement = {(in.worldPos.x - clock.prevPos.x) * inv * covered,
                                 (in.worldPos.y - clock.prevPos.y) * inv * covered,
                                 (in.worldPos.z - clock.prevPos.z) * inv * covered};
            clock.lastSubStepTime = clock.variationTime - plan.remainder;
            n = (std::min)(n, kMaxSubSteps);
        }
        plan.nSteps = n;
    }

    // A scaled frame that rounded to zero milliseconds does not advance the
    // clock at all — otherwise `catchUp` would pull it forward while `dt` said
    // nothing happened.
    if (dtMs != 0 || !scaled)
        clock.emitterTime += plan.catchUp;

    clock.lastFrameIndex = in.frameIndex;
    clock.lastTimeMs = in.nowMs;
    return plan;
}

Sc2RestartCheck Sc2TickRestartCheck(Sc2EmitClock& clock, const Sc2ClockInputs& in) {
    Sc2RestartCheck out;
    constexpr u32 kArmMask = bits::kStateSequenceChanged | bits::kStateRestartBusy;
    if ((clock.stateFlags & kArmMask) != bits::kStateSequenceChanged)
        return out;
    clock.stateFlags &= ~static_cast<u32>(bits::kStateSequenceChanged);
    out.armed = true;
    // The same scaled frame `Sc2TickClock` steps, measured against the stamp
    // the PREVIOUS tick left: the pre-roll is owed for the frames the emitter
    // missed, not for this one.
    const bool scaled = (clock.stateFlags & kScaleTimeMask) != 0;
    const i32 dtMs =
        scaled ? static_cast<i32>(static_cast<f32>(in.dtMs) * in.timeScale) : in.dtMs;
    out.gapMs = static_cast<u32>(in.nowMs) - static_cast<u32>(clock.lastTimeMs);
    out.owed = out.gapMs > static_cast<u32>(2 * dtMs);
    return out;
}

f32 Sc2PreRollPeak(std::span<const f32> curve, bool haveCurve, f32 initValue) {
    if (!haveCurve)
        return initValue;
    // Seeded at zero, then the image's unrolled pairwise reduction: an odd
    // count takes key 0 against the seed first.
    const usize n = curve.size();
    f32 peak = 0.0f;
    usize k = 0;
    if ((n & 1) != 0) {
        peak = curve[0] >= 0.0f ? curve[0] : 0.0f;
        k = 1;
    }
    for (; k < n; k += 2) {
        f32 w = peak > curve[k] ? peak : curve[k];
        w = w > curve[k + 1] ? w : curve[k + 1];
        peak = w;
    }
    return peak;
}

Sc2PreRollPlan Sc2PlanPreRoll(f32 peak, u32 requestedMs) {
    Sc2PreRollPlan out;
    const f32 ms = (peak > 0.0f ? peak : 0.0f) * sc2::kMsPerSec;
    // `cvttss2si`: out of range lands on the integer indefinite, not on a clamp.
    const u32 want = ms >= 2147483648.0f ? 0x80000000u : static_cast<u32>(static_cast<i32>(ms));
    out.budgetMs = (std::min)(want, requestedMs);
    if (out.budgetMs != 0)
        out.blocks = static_cast<u32>((u64{out.budgetMs} + (kSc2PreRollBlockMs - 1)) /
                                      kSc2PreRollBlockMs);
    return out;
}

f32 Sc2ComputeEmitCount(const Sc2EmitCountInputs& in) {
    if (in.suppressed || !in.nodeVisible)
        return 0.0f;

    const i32 idxCut = bits::LodIndex(in.lodCut, in.quality);
    const i32 idxReduce = bits::LodIndex(in.lodReduce, in.quality);
    const f32 lodMul =
        bits::kLodCut[idxCut] != 0 ? 0.0f : in.elemScaleX * bits::kLodReduce[idxReduce];

    const f32 count = (in.rate * in.dt * in.timeScale + in.burst) * lodMul;
    return count > 0.0f ? count : 0.0f;
}

u32 Sc2SlotEmitTarget(f32& carry, f32 count) {
    const f32 wanted = count + carry;
    const f32 whole = std::floor(wanted);
    carry = wanted - whole;
    // Not clamped at zero: a negative rate floors to −1 and the caller wants
    // that 0xFFFFFFFF, because it is what retail divides `catchUp` by.
    return static_cast<u32>(static_cast<i32>(whole));
}

namespace {

/// Frames and cursors add with 32-bit wrap: a saturated frame of `INT_MAX`
/// leaves its cursor at `INT_MIN`, and OP7b records exactly that.
i32 AddWrap(i32 a, i32 b) {
    return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b));
}

} // namespace

bool Sc2CollectCrossedKeys(std::span<const Sc2KeyPlayer> players, i32 bias, Sc2KeySink& sink) {
    if (sink.cursors.size() < players.size())
        sink.cursors.resize(players.size(), 0);

    // A playhead that has not moved — every cursor already one past its frame,
    // the bias playing no part — is a no-op that keeps the last frame's keys.
    if (!sink.resync && !sink.prime) {
        bool moved = false;
        for (usize i = 0; i < players.size() && !moved; ++i)
            moved = sink.cursors[i] != AddWrap(players[i].frame, 1);
        if (!moved)
            return false;
    }

    sink.count = 0;
    for (usize i = 0; i < players.size(); ++i) {
        const Sc2KeyPlayer& p = players[i];
        const i32 now = AddWrap(p.frame, bias);
        if (sink.prime && !sink.resync) {
            sink.cursors[i] = now;
            continue;
        }
        if (sink.resync)
            sink.cursors[i] = now;
        const i32 then = sink.cursors[i];

        const usize n = (std::min)(p.track.times.size(), p.track.values.size());
        const std::span<const i32> times = p.track.times.first(n);
        const auto report = [&](usize k) {
            if (sink.count < Sc2KeySink::kCapacity)
                sink.keys[sink.count++] = {p.track.values[k], times[k]};
        };
        const usize from =
            static_cast<usize>(std::lower_bound(times.begin(), times.end(), then) - times.begin());
        if (now >= then) {
            for (usize k = from; k < n && times[k] <= now; ++k)
                report(k);
        } else {
            // Wrapped: the rest of the track from the cursor, then its head up
            // to and including now. Retail keeps only the key AT the cursor
            // from the tail and stops the head short of now and of the last
            // key; this reports all of them (design §8).
            for (usize k = from; k < n; ++k)
                report(k);
            for (usize k = 0; k < n && times[k] <= now; ++k)
                report(k);
        }
        if (!sink.prime)
            sink.cursors[i] = AddWrap(now, 1);
    }
    sink.resync = false;
    sink.prime = false;
    return true;
}

f32 Sc2SquirtBurst(const Sc2KeySink& sink) {
    i32 sum = 0;
    for (u32 k = 0; k < sink.count; ++k) {
        const i16 v = static_cast<i16>(sink.keys[k].value);
        sum += v > 0 ? v : 0;
    }
    return static_cast<f32>(sum);
}

f32 Sc2RcpNewton(f32 x) {
#if WDX_SC2_HAS_RCPSS
    const f32 r = _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x)));
#else
    const f32 r = 1.0f / x;
#endif
    return r * (2.0f - x * r);
}

namespace {

/// A remainder at or below this runs no trailing catch-up pass.
constexpr f32 kRemainderFloor = 0x1p-23f;
/// Added to the trailing pass's aim so a whole-count target is reached.
constexpr f32 kCatchUpNudge = 1e-4f;

/// One spawn pass of the sub-stepped loop. `t` is time into the frame; the aim
/// is a RUNNING target, so a slot that overshot early spawns nothing until the
/// target catches up, and the shortfall is never banked.
void Sc2SpawnPass(const Sc2ScheduleInputs& in, std::span<const u32> targets,
                  std::span<u32> emitted, f32 t, std::vector<Sc2EmitEvent>& events) {
    for (usize s = 0; s < targets.size(); ++s) {
        const f32 aim = std::floor(static_cast<f32>(targets[s]) * (t / in.frameDt));
        const i32 want = static_cast<i32>(aim) - static_cast<i32>(emitted[s]);
        const u32 n = want > 0 ? static_cast<u32>(want) : 0u;
        if (n != 0 || in.haveRequests) {
            events.push_back({Sc2EmitEventKind::Spawn, static_cast<u16>(s), n});
            emitted[s] += n;
        }
    }
}

} // namespace

Sc2Schedule Sc2SpawnSchedule(const Sc2ScheduleInputs& in, std::span<f32> carry,
                             std::span<u32> targets, std::vector<Sc2EmitEvent>& events) {
    Sc2Schedule out;
    events.clear();

    const usize slots = targets.size();
    std::vector<u32> emitted(slots, 0u);

    const auto measure = [&] {
        for (usize s = 0; s < slots; ++s) {
            f32 count = 0.0f;
            if (!in.frozen) {
                events.push_back({Sc2EmitEventKind::Count, static_cast<u16>(s), 0});
                // Already windowed — `ComputeEmitCount` owns that multiply, and
                // applying it a second time here scaled a squirt burst by the
                // frame length as well. See `Sc2ScheduleInputs::counts`.
                count = s < in.counts.size() ? in.counts[s] : 0.0f;
            }
            targets[s] = Sc2SlotEmitTarget(carry[s], count);
            out.total += targets[s];
        }
    };

    // The two paths order the pre-emit hook against the rate sampling
    // differently; the golden's event log is the only reason we know.
    if (in.fullStep) {
        measure();
        events.push_back({Sc2EmitEventKind::PreEmit, 0, 0});
        if (out.total != 0)
            out.spawnTimeStep = in.catchUp / static_cast<f32>(out.total);
        for (usize s = 0; s < slots; ++s) {
            const bool any = static_cast<i32>(targets[s]) > 0;
            if (any || in.haveRequests) {
                const u32 n = any ? targets[s] : 0u;
                events.push_back({Sc2EmitEventKind::Spawn, static_cast<u16>(s), n});
                emitted[s] = n;
            }
        }
        if (out.total != 0 || in.anythingAlive)
            events.push_back({Sc2EmitEventKind::Update, 0, 0});
        return out;
    }

    events.push_back({Sc2EmitEventKind::PreEmit, 0, 0});
    measure();
    if (out.total != 0)
        out.spawnTimeStep = in.catchUp * Sc2RcpNewton(static_cast<f32>(out.total));

    if (in.nSubSteps == 0) {
        // Spawns the whole frame's count and never simulates, then carries the
        // entire frame into `accumTime`. A viewer above 60 Hz lands here often.
        for (usize s = 0; s < slots; ++s) {
            const bool any = static_cast<i32>(targets[s]) > 0;
            if (any || in.haveRequests) {
                const u32 n = any ? targets[s] : 0u;
                events.push_back({Sc2EmitEventKind::Spawn, static_cast<u16>(s), n});
                emitted[s] = n;
            }
        }
        out.accumTime = in.accumTime + in.frameDt;
        return out;
    }

    f32 t = in.subDt - in.accumTime;
    for (u32 step = 0; step < in.nSubSteps; ++step) {
        if (step != 0)
            t += in.subDt;
        Sc2SpawnPass(in, targets, emitted, t, events);
        if (out.total != 0 || in.anythingAlive)
            events.push_back({Sc2EmitEventKind::Update, 0, 0});
    }

    // `t` stops at the LAST pass, not one sub-step past it — the trailing
    // catch-up aims from there. It is load-bearing, not an optimisation: the
    // loop's shortfall is not banked anywhere else.
    if (in.remainder > kRemainderFloor) {
        Sc2SpawnPass(in, targets, emitted, in.remainder + kCatchUpNudge + t, events);
        out.accumTime = in.remainder;
    }
    return out;
}

Sc2Sweep Sc2SpawnSweep(const Sc2SweepInputs& in) {
    Sc2Sweep out;
    out.prevPos = in.prevPos;
    if (in.fullStep)
        return out;
    if (in.total != 0) {
        // The reciprocal the time lane takes too: retail builds `1 / total`
        // once and multiplies all four lanes by it.
        const f32 inv = Sc2RcpNewton(static_cast<f32>(in.total));
        out.spawnPosStep = {(in.worldPos.x - in.prevPos.x) * inv,
                            (in.worldPos.y - in.prevPos.y) * inv,
                            (in.worldPos.z - in.prevPos.z) * inv};
    }
    if (in.nSubSteps != 0)
        out.prevPos = in.worldPos;
    return out;
}

} // namespace whiteout::flakes::renderer::particle
