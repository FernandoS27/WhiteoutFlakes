#include "particle_stages_sc2.h"

#include "renderer/sc2/sc2_element.h"
#include "renderer/sc2/sc2_element_math.h"

#include <algorithm>
#include <bit>
#include <cmath>

#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
#define WDX_SC2_HAS_RCPSS 1
#include <xmmintrin.h>
#else
#define WDX_SC2_HAS_RCPSS 0
#endif

namespace whiteout::flakes::renderer::particle {

namespace {

namespace bits = whiteout::flakes::renderer::sc2;

/// Both bits arm the same multiply — see `kStateScaleTimeAlso`.
constexpr u32 kScaleTimeMask = bits::kStateScaleTimeByParent | bits::kStateScaleTimeAlso;

// The image's own constants, not recomputed: `two_pi` at 0x103BC8C10's row and
// `deg_to_rad` are the exact floats the shipped code multiplies by (RE §16.5).
constexpr f32 kPi = 3.14159274f;
constexpr f32 kTwoPi = 6.2831855f;
constexpr f32 kDegToRad = 0.017453292f;
/// `0x103C2BC94` — a spline tangent above this |z| takes the world basis.
constexpr f32 kSplineVerticalCos = 0.99899995f;

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
        clock.emitterTime = static_cast<f32>(in.nowMs - static_cast<i32>(dtMs)) * 0.001f;

    plan.dt = static_cast<f32>(dtMs) * 0.001f;
    plan.catchUp = (clock.stateFlags & bits::kStateUseLocalTime) != 0
                       ? plan.dt
                       : (static_cast<f32>(in.nowMs) * 0.001f + in.timeOffset) - clock.emitterTime;
    clock.variationTime += plan.dt;

    // `subStepRate` is only ever tested against zero — it is never the rate.
    f32 rate = 0.0f;
    if (in.subStepRate != 0.0f) {
        rate = in.force60Hz                     ? 60.0f
               : plan.dt < 1.0f / 60.0f         ? 60.0f
               : plan.dt < 1.0f / 30.0f         ? 30.0f
                                                : 15.0f;
    }

    const bool skippedFrame = clock.lastFrameIndex < in.frameIndex - 1;
    if (rate == 0.0f || skippedFrame || in.modelPaused ||
        (clock.stateFlags & bits::kStateSquirtResync) != 0) {
        plan.fullStep = true;
        plan.nSteps = 1;
        plan.subDt = rate >= 0.001f ? 1.0f / rate : (std::min)(1.0f, plan.dt);
        plan.remainder = 0.0f;
        // `prevPos` moves ONLY here. A sub-stepping emitter never refreshes it,
        // which is what makes the displacement below span whole frames.
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
            n = (std::min)(n, 100u);
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
    const f32 ms = (peak > 0.0f ? peak : 0.0f) * 1000.0f;
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
    if (in.remainder > 0x1p-23f) {
        Sc2SpawnPass(in, targets, emitted, in.remainder + 1e-4f + t, events);
        out.accumTime = in.remainder;
    }
    return out;
}


namespace {

/// `Rand(-h, +h)`. Every box and plane axis is drawn through this kernel, not
/// through `outer · (rand01 - 0.5)`: the two are algebraically identical and
/// differ in the last bit, and OP4 sees the difference on the plane.
f32 SymRand(sc2::Rng& rng, f32 h) {
    return rng.RangeF(-h, h);
}

/// The radius every round shape shares: a range draw when hollow, otherwise
/// `rand01 · outer` — uniform in r, so the volume is centre-biased. That is
/// the shipped distribution, not a bug to fix.
f32 ShapeRadius(sc2::Rng& rng, const Sc2SpawnPosInputs& in) {
    if (in.cutout)
        return rng.RangeF(in.innerRadius, in.outerRadius);
    return rng.RangeF(0.0f, 1.0f) * in.outerRadius;
}

f32 Component(const Vector3f& v, i32 i) {
    return i == 0 ? v.x : (i == 1 ? v.y : v.z);
}

void SetComponent(Vector3f& v, i32 i, f32 value) {
    (i == 0 ? v.x : (i == 1 ? v.y : v.z)) = value;
}

Vector3f Cross(const Vector3f& a, const Vector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

/// Returns false when the vector has no length, leaving `v` untouched.
bool Normalize(Vector3f& v) {
    const f32 n2 = (v.x * v.x + v.y * v.y) + v.z * v.z;
    if (n2 <= 0.0f)
        return false;
    const f32 inv = 1.0f / std::sqrt(n2);
    v = {v.x * inv, v.y * inv, v.z * inv};
    return true;
}

f32 Bezier3(const Vector3f* p, f32 u, i32 i) {
    const f32 iu = 1.0f - u;
    const f32 b0 = (iu * iu) * iu;
    const f32 b1 = (3.0f * u) * (iu * iu);
    const f32 b2 = (3.0f * (u * u)) * iu;
    const f32 b3 = (u * u) * u;
    return (Component(p[0], i) * b0 + Component(p[1], i) * b1) +
           (Component(p[2], i) * b2 + Component(p[3], i) * b3);
}

f32 Bezier3Deriv(const Vector3f* p, f32 u, i32 i) {
    const f32 iu = 1.0f - u;
    const f32 d0 = -3.0f * (iu * iu);
    const f32 d1 = (3.0f * (iu * iu)) - ((6.0f * u) * iu);
    const f32 d2 = ((6.0f * u) * iu) - (3.0f * (u * u));
    const f32 d3 = 3.0f * (u * u);
    return (Component(p[0], i) * d0 + Component(p[1], i) * d1) +
           (Component(p[2], i) * d2 + Component(p[3], i) * d3);
}

/// The overlay groups are sampled at spawn, and only a type-5 wave touches the
/// generator — so where each one sits in the stream is observable exactly
/// there. OP5's original grid armed one group at a time and could not see the
/// order at all; widened to arm two, it says SPEED is sampled first.
f32 Overlay(sc2::Rng& rng, const Sc2Overlay& o, f32 time, f32 phase) {
    if (o.type == 0)
        return 0.0f;
    return sc2::SampleWave(o.type, o.frequency * time + phase, o.amplitude, &rng);
}

} // namespace

Vector3f Sc2SampleSpawnPosition(sc2::Rng& rng, const Sc2SpawnPosInputs& in, Vector3f* normal) {
    switch (in.shape) {
    case Sc2SpawnShape::Point:
    default:
        // Draws NOTHING — the generator must come back untouched, which is
        // half of what OP4's shape-0 vectors are for.
        return {0.0f, 0.0f, 0.0f};

    case Sc2SpawnShape::Plane: {
        // `elemScale` is read one index over: outer.x pairs with scale.y and
        // outer.y with scale.z. Measured, not inferred — the "scaled" extents
        // vector is the only one that can tell the three apart.
        const f32 x = SymRand(rng, 0.5f * (in.shapeOuter.x * in.elemScale.y));
        const f32 y = SymRand(rng, 0.5f * (in.shapeOuter.y * in.elemScale.z));
        return {x, y, 0.0f};
    }

    case Sc2SpawnShape::Sphere: {
        const f32 r = ShapeRadius(rng, in);
        const f32 w = rng.RangeF(-1.0f, 1.0f);
        const f32 az = rng.RangeF(0.0f, 1.0f) * kTwoPi;
        // The recorder read an `asinf(fabs(w))` elevation off the disassembly.
        // It is not observable: the ring lane cannot separate `cos(asin|w|)`
        // from `sqrt(1-w²)`, and the z lane says `r·w` outright — bit-exact on
        // all 96 vectors, where the asin route misses 8 by an ulp. So the
        // measurable form is transcribed and the asin is left as a note.
        const f32 ring = r * std::sqrt(1.0f - w * w);
        // x takes SIN and y takes COS — the swap against the cylinder below,
        // which is the same azimuth applied the other way round.
        return {ring * std::sin(az), ring * std::cos(az), r * w};
    }

    case Sc2SpawnShape::Box: {
        // The axis pick is drawn FIRST, then the three axes in index order —
        // the slab axis is not drawn last. With cube extents every ordering
        // gives the same numbers, so only the "shell" vector separates them.
        Vector3f p{0.0f, 0.0f, 0.0f};
        const i32 k = in.cutout ? static_cast<i32>(rng.RangeInt(0, 3)) : -1;
        for (i32 a = 0; a < 3; ++a) {
            if (a != k) {
                SetComponent(p, a, SymRand(rng, Component(in.shapeOuter, a) * 0.5f));
                continue;
            }
            const f32 span = (Component(in.shapeOuter, a) - Component(in.shapeInner, a)) * 0.5f;
            const f32 v = SymRand(rng, span);
            const f32 half = Component(in.shapeInner, a) * 0.5f;
            SetComponent(p, a, v >= 0.0f ? v + half : v - half);
        }
        return p;
    }

    case Sc2SpawnShape::Cylinder:
    case Sc2SpawnShape::Disc: {
        const f32 r = ShapeRadius(rng, in);
        // The cylinder draws its HEIGHT before its angle. The disc has no
        // height and so takes the angle one slot earlier — the two shapes do
        // NOT share a draw stream, and treating the disc as "a cylinder with
        // z = 0" gives it the cylinder's height draw as its angle.
        const f32 z = in.shape == Sc2SpawnShape::Cylinder
                          ? in.shapeOuter.z * (rng.RangeF(0.0f, 1.0f) - 0.5f)
                          : 0.0f;
        const f32 th = rng.RangeF(0.0f, 1.0f) * kTwoPi;
        return {r * std::cos(th), r * std::sin(th), z};
    }

    case Sc2SpawnShape::Spline: {
        const i32 nSeg = static_cast<i32>(in.spline.size()) / 4;
        if (nSeg <= 0)
            return {0.0f, 0.0f, 0.0f};
        const f32 t = rng.RangeF(in.splineLowerBound, in.splineUpperBound);
        const i32 seg = (std::min)(nSeg - 1, static_cast<i32>(static_cast<f32>(nSeg) * t));
        const f32 u = (t - static_cast<f32>(seg) / static_cast<f32>(nSeg)) *
                      static_cast<f32>(nSeg);
        const Vector3f* cp = in.spline.data() + 4 * seg;
        const Vector3f p{Bezier3(cp, u, 0), Bezier3(cp, u, 1), Bezier3(cp, u, 2)};

        Vector3f tangent{Bezier3Deriv(cp, u, 0), Bezier3Deriv(cp, u, 1),
                         Bezier3Deriv(cp, u, 2)};
        Vector3f n1{1.0f, 0.0f, 0.0f};
        Vector3f n2{0.0f, 1.0f, 0.0f};
        // A vertical tangent has no well-defined frame about world Z, so the
        // world basis stands in. The threshold is the shipped constant, not a
        // round number: a segment at 0.9989 still builds its own frame.
        if (Normalize(tangent) && std::fabs(tangent.z) <= kSplineVerticalCos) {
            n1 = Cross(tangent, Vector3f{0.0f, 0.0f, 1.0f});
            Normalize(n1);
            n2 = Cross(n1, tangent);
            Normalize(n2);
        }
        const f32 r = ShapeRadius(rng, in);
        const f32 az = rng.RangeF(0.0f, 1.0f) * kTwoPi;
        const f32 ca = std::cos(az);
        const f32 sa = std::sin(az);
        return {p.x + ((r * ca) * n1.x + (r * sa) * n2.x),
                p.y + ((r * ca) * n1.y + (r * sa) * n2.y),
                p.z + ((r * ca) * n1.z + (r * sa) * n2.z)};
    }

    case Sc2SpawnShape::Mesh: {
        if (in.mesh == nullptr)
            return {0.0f, 0.0f, 0.0f};
        const Sc2MeshSample s = Sc2SampleMeshSurface(rng, *in.mesh);
        if (normal != nullptr)
            *normal = s.normal;
        return s.position;
    }
    }
}

Vector3f Sc2SampleSpawnVelocity(sc2::Rng& rng, const Sc2SpawnVelInputs& in) {
    // SPEED FIRST. Only a type-5 overlay draws, so this ordering is invisible
    // until two of them are armed at once; OP5's widened grid is what pins it.
    const f32 speedOverlay = Overlay(rng, in.speedOverlay, in.variationTime, in.variationPhase);
    const f32 yaw = in.spawnYaw +
                    Overlay(rng, in.yawOverlay, in.variationTime, in.variationPhase);
    const f32 pitch = in.spawnPitch +
                      Overlay(rng, in.pitchOverlay, in.variationTime, in.variationPhase);
    const f32 horizontal =
        in.spawnHorizontal +
        Overlay(rng, in.horizontalOverlay, in.variationTime, in.variationPhase);
    const f32 vertical =
        in.spawnVertical + Overlay(rng, in.verticalOverlay, in.variationTime, in.variationPhase);

    Vector3f dir{0.0f, 0.0f, 0.0f};
    f32 speed = 0.0f;

    const auto drawSpeed = [&]() {
        return in.speedIsEndpoint ? rng.RangeF(in.speed, in.speedRandom)
                                  : in.speed + speedOverlay;
    };

    switch (in.velocityType) {
    case 0: {
        // The overlays are added in DEGREES, before the conversion — an
        // amplitude of 1.5 is 1.5°, not 1.5 rad. Yaw never touches x, which is
        // how the group-0/group-1 swap is visible at all.
        const f32 a = yaw * kDegToRad;
        const f32 b = pitch * kDegToRad;
        const f32 h = rng.RangeF(kPi - horizontal, kPi + horizontal);
        const f32 v = rng.RangeF(-vertical, vertical);
        const f32 cv = std::cos(v);
        const f32 sv = std::sin(v);
        const f32 chb = std::cos(h - b);
        const f32 shb = std::sin(h - b);
        const f32 sa = std::sin(a);
        const f32 ca = std::cos(a);
        dir = {cv * shb, (sa * cv) * chb + ca * sv, sa * sv - (ca * cv) * chb};
        speed = drawSpeed();
        break;
    }
    case 1:
        dir = in.position;
        if (!Normalize(dir))
            dir = {0.0f, 0.0f, 0.0f};
        speed = drawSpeed();
        break;
    case 2:
        dir = {0.0f, 0.0f, in.position.z >= 0.0f ? 1.0f : -1.0f};
        speed = drawSpeed();
        break;
    case 3: {
        // Type 3 draws its SPEED BEFORE its direction, and only when the
        // endpoint flag is set — so the w/azimuth pair sits at a different
        // point in the stream depending on a flag about magnitude. The cone
        // above does the opposite. Both orders are measured.
        speed = drawSpeed();
        const f32 w = rng.RangeF(-1.0f, 1.0f);
        const f32 az = rng.RangeF(0.0f, 1.0f) * kTwoPi;
        const f32 ring = std::sqrt(1.0f - w * w);
        dir = {ring * std::sin(az), ring * std::cos(az), w};
        break;
    }
    case 4:
    default:
        dir = in.normal;
        speed = drawSpeed();
        break;
    }

    Vector3f out{dir.x * speed, dir.y * speed, dir.z * speed};

    if (in.flattenXY) {
        const f32 mag = std::sqrt((out.x * out.x + out.y * out.y) + out.z * out.z);
        const f32 xy = std::sqrt(out.x * out.x + out.y * out.y);
        // Below the floor there is no direction to keep, so z is dropped and
        // the (tiny) xy is left as it is rather than blown up by mag/xy.
        if (xy > 1e-5f) {
            const f32 s = mag / xy;
            out = {out.x * s, out.y * s, 0.0f};
        } else {
            out.z = 0.0f;
        }
    }
    return out;
}


namespace {

/// `0x103C2BC9C` — the `>` comparand for every mid-time collapse.
constexpr f32 kMidTimeCollapse = 0.9959f;

struct Bgra {
    i32 a, r, g, b; // as a u32 the alpha is the HIGH byte
};

Bgra Unpack(u32 v) {
    return {static_cast<i32>((v >> 24) & 0xFFu), static_cast<i32>((v >> 16) & 0xFFu),
            static_cast<i32>((v >> 8) & 0xFFu), static_cast<i32>(v & 0xFFu)};
}

u32 Pack(const Bgra& c) {
    return (static_cast<u32>(c.a & 0xFF) << 24) | (static_cast<u32>(c.r & 0xFF) << 16) |
           (static_cast<u32>(c.g & 0xFF) << 8) | static_cast<u32>(c.b & 0xFF);
}

/// `base + ((t · (random − base)) >> 8)` — an INTEGER lerp toward the random
/// endpoint with an 8-bit shift, not a float lerp rounded afterwards.
i32 LerpChannel(i32 base, i32 random, i32 t) {
    return base + (((t * (random - base)) >> 8));
}

} // namespace

std::array<u32, 3> Sc2SampleColor(sc2::Rng& rng, const Sc2ColorInputs& in) {
    const f32 overlay =
        Overlay(rng, in.alphaOverlay, in.variationTime, in.variationPhase);

    Bgra nodes[3];
    for (i32 n = 0; n < 3; ++n) {
        nodes[n] = Unpack(in.keys[static_cast<std::size_t>(n)]);
        if (!in.randomEnable)
            continue;
        // ONE t per node, drawn per node — not one t shared by all three.
        const i32 t = static_cast<i32>(rng.RangeInt(0, 256));
        const Bgra r = Unpack(in.randomKeys[static_cast<std::size_t>(n)]);
        nodes[n] = {LerpChannel(nodes[n].a, r.a, t), LerpChannel(nodes[n].r, r.r, t),
                    LerpChannel(nodes[n].g, r.g, t), LerpChannel(nodes[n].b, r.b, t)};
    }

    // The collapse lives INSIDE the randomise branch, exactly as it does in
    // `SampleParticleRotation`. RE §5.8 states it unconditionally for colour;
    // measured, an emitter with no colour randomisation keeps its mid key
    // however high the mid time is.
    if (in.randomEnable) {
        if (in.colorMidTime > kMidTimeCollapse) {
            nodes[1].r = nodes[2].r;
            nodes[1].g = nodes[2].g;
            nodes[1].b = nodes[2].b;
        }
        if (in.alphaMidTime > kMidTimeCollapse)
            nodes[1].a = nodes[2].a;
    }

    std::array<u32, 3> out{};
    for (i32 n = 0; n < 3; ++n) {
        // Added to the alpha BYTE as a float, clamped as a float, and only
        // then truncated. Truncating the overlay to an integer first turns a
        // −0.91 into 0 and leaves alpha untouched where retail drops it by one.
        f32 a = static_cast<f32>(nodes[n].a) + overlay;
        a = (std::max)(0.0f, (std::min)(255.0f, a));
        Bgra c = nodes[n];
        c.a = static_cast<i32>(a);
        out[static_cast<std::size_t>(n)] = Pack(c);
    }
    return out;
}

std::array<f32, 4> Sc2SampleSize(sc2::Rng& rng, const Sc2SizeInputs& in) {
    const f32 overlay = Overlay(rng, in.sizeOverlay, in.variationTime, in.variationPhase);

    std::array<f32, 3> keys = in.keys;
    if (in.randomEnable) {
        // ONE draw lerps all three keys — unlike colour, which draws per node.
        const f32 t = rng.RangeF(0.0f, 1.0f);
        for (std::size_t k = 0; k < 3; ++k)
            keys[k] = keys[k] + t * (in.randomKeys[k] - keys[k]);
    }

    // Half extents, and the overlay arrives already halved: the shipped form is
    // `(key·0.5 + overlay·0.5)·blend`, not `(key + overlay)·0.5·blend`.
    const f32 half = overlay * 0.5f;
    std::array<f32, 4> out{};
    for (std::size_t k = 0; k < 3; ++k)
        out[k] = (keys[k] * 0.5f + half) * in.blend;
    out[3] = in.instanceType == 9 ? in.instanceDistance : 1.0f;
    return out;
}

std::array<f32, 3> Sc2SampleRotation(sc2::Rng& rng, const Sc2RotationInputs& in) {
    const f32 overlay =
        Overlay(rng, in.rotationOverlay, in.variationTime, in.variationPhase);

    f32 start = in.keys[0];
    f32 mid = in.keys[1];
    f32 end = in.keys[2];
    if (in.randomEnable) {
        // Three draws, and all three happen in BOTH arms — the relative flag
        // changes what is added, never what is drawn.
        start = rng.RangeF(start, in.randomKeys[0]);
        mid = rng.RangeF(mid, in.randomKeys[1]);
        end = rng.RangeF(end, in.randomKeys[2]);
    }

    std::array<f32, 3> out{};
    out[0] = start + overlay;
    if (in.relative) {
        out[1] = mid + out[0];
        out[2] = end + out[1];
    } else {
        out[1] = mid + overlay;
        out[2] = end + overlay;
    }
    // Random-only, and this one the RE already records (§16.6).
    if (in.randomEnable && in.rotationMidTime > kMidTimeCollapse)
        out[1] = out[2];
    return out;
}


namespace {

/// `×256` for size and `×32` for rotation, both a truncating `cvttss2si` of
/// the sampled float. The samplers themselves return floats (OP6); the
/// quantisation is this function's job, not theirs.
constexpr f32 kSizeQuant = 256.0f;
constexpr f32 kRotQuant = 32.0f;
/// `(v + 1) · 127.5`, the instanceType-7 basis packing.
constexpr f32 kOrientQuant = 127.5f;
/// `dword_103BB6B74` — the squared-speed floor a type-6 instance must clear.
constexpr f32 kStillSpeedSq = 1.0e-5f;

u16 Quant(f32 v) {
    return static_cast<u16>(static_cast<i32>(v));
}

Vector3f Row(const Matrix44f& m, i32 r) {
    return {m.data[r][0], m.data[r][1], m.data[r][2]};
}

/// The largest squared row length of the basis — `maxColumnLenSq` in the
/// disassembly's naming, over the rows as this layout stores them.
f32 MaxRowLenSq(const Matrix44f& m) {
    f32 best = 0.0f;
    for (i32 r = 0; r < 3; ++r) {
        const Vector3f v = Row(m, r);
        best = (std::max)(best, (v.x * v.x + v.y * v.y) + v.z * v.z);
    }
    return best;
}

Matrix44f NormalizedBasis(const Matrix44f& m) {
    Matrix44f n = m;
    for (i32 r = 0; r < 3; ++r) {
        const Vector3f v = Row(m, r);
        const f32 sq = (v.x * v.x + v.y * v.y) + v.z * v.z;
        if (sq <= 0.0f)
            continue;
        const f32 inv = 1.0f / std::sqrt(sq);
        n.data[r][0] = v.x * inv;
        n.data[r][1] = v.y * inv;
        n.data[r][2] = v.z * inv;
    }
    return n;
}

Vector3f BasisMul(const Matrix44f& b, const Vector3f& v) {
    return {(v.x * b.data[0][0] + v.y * b.data[1][0]) + v.z * b.data[2][0],
            (v.x * b.data[0][1] + v.y * b.data[1][1]) + v.z * b.data[2][1],
            (v.x * b.data[0][2] + v.y * b.data[1][2]) + v.z * b.data[2][2]};
}

f32 PackOrient(f32 hi, f32 lo) {
    const i32 h = static_cast<i32>((hi + 1.0f) * kOrientQuant);
    const i32 l = static_cast<i32>((lo + 1.0f) * kOrientQuant);
    return static_cast<f32>((h << 16) | (l & 0xFFFF));
}

} // namespace

void Sc2InitSpawned(sc2::Rng& rng, const Sc2InitInputs& in, Sc2InitState& state,
                    std::span<Sc2SpawnedElement> out) {
    // The size sampler's scale argument. Exactly 1.0 for slot 0; for a `PARC`
    // slot it is the bone's max row length over the emitter's, and zero when
    // the bone is degenerate.
    f32 sizeBlend = 1.0f;
    if (in.slot > 0 && in.hasBone) {
        const f32 bsq = MaxRowLenSq(in.boneMatrix);
        const f32 wsq = MaxRowLenSq(in.worldMatrix);
        sizeBlend = (bsq <= 0.0f || wsq <= 0.0f) ? 0.0f
                                                 : std::sqrt(bsq) / std::sqrt(wsq);
    }

    // The world arm's BASIS: the emitter's own matrix for slot 0 — and for a
    // Mesh emitter at ANY slot — the PARC bone's rows otherwise.
    const bool emitterBasis =
        in.shape.shape == Sc2SpawnShape::Mesh || in.slot == 0;
    const Matrix44f& basis =
        emitterBasis ? in.worldMatrix : (in.hasBone ? in.boneMatrix : in.worldMatrix);
    const Matrix44f nbasis = NormalizedBasis(basis);

    // Its TRANSLATION is NOT the matrix's: `curPos + spawnPosStep`, and for
    // slot > 0 the bone's translation MINUS the emitter world matrix's, so a
    // PARC copy emits from its bone while simulating in the emitter's frame.
    Vector3f emitPos{state.curPos.x + in.spawnPosStep.x,
                     state.curPos.y + in.spawnPosStep.y,
                     state.curPos.z + in.spawnPosStep.z};
    if (in.slot > 0 && in.hasBone) {
        emitPos = {emitPos.x + in.boneMatrix.data[3][0] - in.worldMatrix.data[3][0],
                   emitPos.y + in.boneMatrix.data[3][1] - in.worldMatrix.data[3][1],
                   emitPos.z + in.boneMatrix.data[3][2] - in.worldMatrix.data[3][2]};
    }

    // instanceType 7 packs two normalised rows into three floats, two u16
    // each: x = right.y<<16 | right.x, y = right.z<<16 | up.x, z = up.y<<16 |
    // up.z.
    const Vector3f right = Row(nbasis, 0);
    const Vector3f up = Row(nbasis, 1);
    const Vector3f fwd = Row(nbasis, 2);
    const Vector3f packed{PackOrient(right.y, right.x), PackOrient(right.z, up.x),
                          PackOrient(up.y, up.z)};

    // The local path's transform, built once: bone × inverse(emitter world).
    Matrix44f localXform = Matrix44f::identity();
    if (in.slot > 0 && in.hasBone)
        localXform = in.boneMatrix * Matrix44f::inverse(in.worldMatrix);

    for (std::size_t n = 0; n < out.size(); ++n) {
        Sc2SpawnedElement& e = out[n];
        e = Sc2SpawnedElement{};

        state.emitterTime += in.spawnTimeStep;
        state.curPos = {state.curPos.x + in.spawnPosStep.x,
                        state.curPos.y + in.spawnPosStep.y,
                        state.curPos.z + in.spawnPosStep.z};

        e.flags = static_cast<u16>((in.parFlags * 2) & 0xC);
        if ((in.emitFlagsWord & 8) != 0)
            e.noisePhase = rng.RangeF(0.0f, in.noiseCoherence);

        // The normal a velocityType 4 reads is ZEROED first and only the Mesh
        // shape writes it, so type 4 on any other shape has no velocity.
        Vector3f normal{0.0f, 0.0f, 0.0f};
        Vector3f pos = Sc2SampleSpawnPosition(rng, in.shape,
                                              in.velocity.velocityType == 4 ? &normal : nullptr);
        Sc2SpawnVelInputs vel = in.velocity;
        vel.position = pos;
        vel.normal = normal;
        Vector3f velocity = Sc2SampleSpawnVelocity(rng, vel);

        const SpawnRequest* req = n < in.requests.size() ? &in.requests[n] : nullptr;
        if (req != nullptr) {
            // Position ADDS, velocity multiplies per axis, orientation is
            // COPIED wholesale rather than combined.
            pos = {pos.x + req->position.x, pos.y + req->position.y,
                   pos.z + req->position.z};
            velocity = {velocity.x * req->velocityScale.x,
                        velocity.y * req->velocityScale.y,
                        velocity.z * req->velocityScale.z};
            e.orientVec = req->orientVec;
        }

        e.birthTime = state.emitterTime;
        const f32 life = (in.additionalFlags & 2) != 0
                             ? rng.RangeF(in.lifetime, in.lifetimeRandom)
                             : in.lifetime;
        e.deathTime = life + e.birthTime;

        e.colorNodes = Sc2SampleColor(rng, in.color);
        Sc2SizeInputs sz = in.size;
        sz.blend = sizeBlend;
        const auto size = Sc2SampleSize(rng, sz);
        for (std::size_t k = 0; k < 4; ++k)
            e.size[k] = Quant(size[k] * kSizeQuant);
        const auto rot = Sc2SampleRotation(rng, in.rotation);
        for (std::size_t k = 0; k < 3; ++k)
            e.rotation[k] = Quant(rot[k] * kRotQuant);
        if ((in.rotationFlags & 1) != 0)
            e.flipbookRand = static_cast<u16>(rng.RangeInt(0, 0xFFFF));

        const f32 mass = (in.additionalFlags & 4) != 0
                             ? rng.RangeF(in.mass, in.massRandom)
                             : in.mass;
        e.invMass = 1.0f / mass;

        // A request forces the LOCAL path even on a world-space emitter, so a
        // request-born particle is placed in its parent's frame.
        const bool worldArm = (in.additionalFlags & 8) != 0 && req == nullptr;
        if (worldArm) {
            const Vector3f b = BasisMul(basis, pos);
            pos = {b.x + emitPos.x, b.y + emitPos.y, b.z + emitPos.z};
            if ((in.stateFlags & 8) != 0) {
                // NORMALISED basis, plus the inherited parent velocity. Both
                // halves hang off this bit: without it the raw basis is used
                // and no parent velocity is inherited at all.
                const Vector3f nv = BasisMul(nbasis, velocity);
                velocity = {nv.x + in.smoothedPos.x * in.inheritVelocityScale,
                            nv.y + in.smoothedPos.y * in.inheritVelocityScale,
                            nv.z + in.smoothedPos.z * in.inheritVelocityScale};
            } else {
                velocity = BasisMul(basis, velocity);
            }
            emitPos = {emitPos.x + in.spawnPosStep.x, emitPos.y + in.spawnPosStep.y,
                       emitPos.z + in.spawnPosStep.z};
            if (in.instanceType == 7)
                e.orientVec = packed;
            else if (in.instanceType == 6)
                e.orientVec = fwd;
        } else {
            // On the local path a type-7 instance gets the normalised forward
            // row, NOT the packed basis — and a type 6 gets nothing. The two
            // arms disagree about what orientVec even means.
            if (in.instanceType == 7)
                e.orientVec = fwd;
            if (in.slot > 0 && in.hasBone) {
                const Vector3f p = BasisMul(localXform, pos);
                pos = {p.x + localXform.data[3][0], p.y + localXform.data[3][1],
                       p.z + localXform.data[3][2]};
                velocity = BasisMul(localXform, velocity);
            }
        }

        e.position = pos;
        e.velocity = velocity;

        if ((in.parFlags & 0x10000) != 0 && in.flipbookColumns != 0 && in.flipbookRows != 0) {
            e.flipbookRandStart = rng.RangeF(
                0.0f, static_cast<f32>(static_cast<i32>(in.flipbookColumns) *
                                       static_cast<i32>(in.flipbookRows)));
        }
        e.spawnOrigin = e.position;

        if ((in.parFlags & 0x80000) != 0 && in.hasChildEmitter1) {
            if (rng.RangeF(0.0f, 1.0f) <= in.trailChance) {
                e.flags |= 1u;
                e.trailAccum = 0.0f;
            }
        }

        // A type-6 instance too slow to point anywhere is killed at birth
        // rather than drawn facing an arbitrary direction.
        if (in.instanceType == 6) {
            const Vector3f& v = e.velocity;
            const f32 sq = (v.y * v.y + v.x * v.x) + v.z * v.z;
            if (sq < kStillSpeedSq) {
                e.deathTime = e.birthTime + -1.0f;
                e.velocity = {1.0f, 0.0f, 0.0f};
                e.orientVec = {1.0f, 0.0f, 0.0f};
            }
        }

        const u32 ms = in.nowMs + static_cast<u32>(static_cast<i32>(
                                      (e.deathTime - e.birthTime) * 1000.0f));
        state.expireFrameMs = (std::max)(state.expireFrameMs, ms);
    }
}

Sc2SpawnBatchPlan Sc2PlanSpawnBatch(const Sc2SpawnBatchInputs& in) {
    constexpr u32 kFlushAt = 128;
    Sc2SpawnBatchPlan plan;
    u32 alive = in.elementCount;
    u32 pending = 0;
    Sc2SpawnFlush cur;

    for (u32 i = 0; i < in.requests; ++i) {
        if (alive >= in.maxParticles)
            break;
        ++alive;
        ++plan.created;
        if (cur.requests == 0)
            cur.requestBegin = i;
        ++cur.requests;
        // Only this loop tests the threshold.
        if (++pending >= kFlushAt) {
            plan.flushes.push_back(cur);
            cur = Sc2SpawnFlush{};
            pending = 0;
        }
    }
    for (u32 i = 0; i < in.plain; ++i) {
        if (alive >= in.maxParticles)
            break;
        ++alive;
        ++plan.created;
        ++cur.plain;
        ++pending;
    }
    if (pending != 0)
        plan.flushes.push_back(cur);
    plan.requestsConsumed = !plan.flushes.empty();
    return plan;
}


// ===========================================================================
// MOVE (analytic) + RETIRE.
// ===========================================================================

Sc2AnalyticStep Sc2StepAnalytic(const Sc2AnalyticInputs& in) {
    namespace vs = whiteout::flakes::renderer::sc2::vs;

    Sc2AnalyticStep out;
    out.age = vs::Saturate((in.systemTime - in.birthTime) /
                           (in.deathTime - in.birthTime));

    // `Bug 187607: no negative time skew`, in the shader's own comment. A
    // batch clock behind the birth instant would otherwise run the closed form
    // with a negative exponent and fling the particle backwards along its
    // trajectory — which is exactly what an emitter that pre-rolls does to
    // every particle it has not yet reached.
    out.elapsed = in.systemTime - in.birthTime;
    if (out.elapsed < 0.0f)
        out.elapsed = 0.0f;

    // The shader recovers the mass by reciprocal — the element carries only
    // the inverse — and NEGATES the gravity lane on the way in.
    const f32 mass = 1.0f / in.invMass;
    const auto r = vs::CalculateDisplacementAndVelocity(
        out.elapsed, in.velocity0, mass, in.invMass, in.drag, in.invDrag,
        -in.gravityZ);

    out.displacement = r.displacement;
    out.velocity = r.velocity;
    out.position = vs::Add(in.position, r.displacement);
    out.tailLength = in.tailLength;

    const auto type = static_cast<Sc2InstanceType>(in.instanceType);
    if (type == Sc2InstanceType::FaceTravelDir) {
        // The whole velocity goes into `vInterpolator2`, whose x lane held the
        // tail length for the stretched types — the two uses share a register.
        out.tailLength = r.velocity.x;
    } else if (type == Sc2InstanceType::Tail ||
               type == Sc2InstanceType::Pinned ||
               type == Sc2InstanceType::Trail) {
        if (in.clampedTailLength) {
            // `b_fixedTailLength` means something DIFFERENT here than it does
            // in the frame build: there it picks `max(tail, tail·speed)`, here
            // it picks the clamp's budget, and only the unfixed spelling takes
            // the max. Same flag, two readings, both measured (OP12).
            const f32 speed = vs::Length3(r.velocity);
            const f32 budget =
                in.fixedTailLength
                    ? (in.tailLength * speed) * in.size
                    : (std::max)(in.tailLength, in.tailLength * speed) * in.size;
            const f32 dist = vs::Length3(r.displacement);
            if (dist < budget) {
                out.tailLength =
                    (std::min)(dist / (speed * in.size), in.tailLength);
            }
        }
    }
    return out;
}

void Sc2ElementList::Reset(usize count) {
    const i32 n = static_cast<i32>(count);
    next.assign(count, kSentinel);
    prev.assign(count, kNull);
    for (i32 i = 0; i < n; ++i) {
        next[static_cast<usize>(i)] = (i + 1 < n) ? i + 1 : kSentinel;
        prev[static_cast<usize>(i)] = (i > 0) ? i - 1 : kNull;
    }
    head = n > 0 ? 0 : kSentinel;
    tail = n > 0 ? n - 1 : kNull;
    freeHead = kNull;
    freeTail = kNull;
    poolCount = static_cast<u32>(count);
}

namespace {

void WalkFrom(const std::vector<i32>& links, i32 start, std::vector<i32>& out) {
    out.clear();
    i32 node = start;
    while (node >= 0 && out.size() < links.size()) {
        out.push_back(node);
        node = links[static_cast<usize>(node)];
    }
}

} // namespace

void Sc2ElementList::Walk(std::vector<i32>& out) const {
    WalkFrom(next, head, out);
}

void Sc2ElementList::WalkBackward(std::vector<i32>& out) const {
    WalkFrom(prev, tail, out);
}

void Sc2ElementList::WalkFree(std::vector<i32>& out) const {
    WalkFrom(next, freeHead, out);
}

void Sc2ElementList::Unlink(i32 node) {
    const usize n = static_cast<usize>(node);
    const i32 nxt = next[n];
    if (head == node)
        head = nxt;
    const i32 pv = prev[n];
    if (pv >= 0)
        next[static_cast<usize>(pv)] = nxt;
    // `node->next->listPrev = prev`, and for the last node `node->next` is the
    // sentinel — whose listPrev is the tail. Writing it through the same
    // expression is what keeps the backward walk right when the tail moves.
    if (nxt == kSentinel)
        tail = pv;
    else
        prev[static_cast<usize>(nxt)] = pv;
    prev[n] = kNull;
    next[n] = kNull;

    if (freeTail >= 0)
        next[static_cast<usize>(freeTail)] = node;
    else
        freeHead = node;
    freeTail = node;
    --poolCount;
}

u32 Sc2RetireExpired(Sc2ElementList& list, std::span<Sc2SpawnedElement> elements,
                     f32 emitterTime, Sc2RecycleArray& recycle) {
    u32 retired = 0;
    i32 node = list.head;
    while (node >= 0) {
        // Read the successor BEFORE unlinking: the unlink clears both links on
        // the node it frees, so a walk that re-read them would stop dead on
        // the first death.
        const i32 nxt = list.next[static_cast<usize>(node)];
        Sc2SpawnedElement& e = elements[static_cast<usize>(node)];
        if (e.deathTime <= emitterTime) {
            if (recycle.enabled && e.vbSlot != -1) {
                // Grows by a fixed increment, not by doubling. Modelled
                // because the capacity is observable through the gate and
                // because a system whose increment is zero never grows at all.
                if (recycle.slots.size() + 1 > recycle.capacity &&
                    recycle.capacity + recycle.growth != 0) {
                    recycle.capacity += recycle.growth;
                }
                recycle.slots.push_back(e.vbSlot);
                e.vbSlot = -1;
            }
            list.Unlink(node);
            ++retired;
        }
        node = nxt;
    }
    return retired;
}

namespace {

constexpr u32 kParSceneGravity = 0x20000;
constexpr u32 kParBounds = 0x80000000;

/// The swept sphere's radius, which is also the terrain push-out
/// (`0x103C2A004`).
constexpr f32 kCollideRadius = 0.05f;
/// `0x103BB6B74`. At or below this determinant the emitter matrix counts as
/// singular and a contact comes back through identity — which a MIRRORED
/// emitter, whose determinant is negative, reaches as well.
constexpr f32 kMinInvertibleDet = 1.0e-5f;
/// `0x103C458F8`: the tangential term needs `|v + wind|²` above this.
constexpr f32 kFrictionSpeedSq = 0.01f;
/// `0x103AAD5F4`: a landed particle rests below `|v|² < 3·dt`.
constexpr f32 kRestPerDt = 3.0f;
/// Element flag 2: a collision spawn or splat that skips its chance roll.
constexpr u16 kElemForced = 0x2;

/// `a·b` left to right — the grouping the CPU vertex builder's tail clamp and
/// velocity test use (OP11). The simulate step groups its own dot products
/// differently and spells them out where they are.
f32 EulerDot(const Vector3f& a, const Vector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/// `r·(−0.5)·((v·r)·r − 3)` — the Newton step every normalise in the CPU step
/// takes after its reciprocal root, in the binary's grouping at all three
/// sites.
f32 NewtonRsqrt(f32 v, f32 seed) {
    return (seed * -0.5f) * ((v * seed) * seed + -3.0f);
}

/// The same step spelled as a LENGTH, the way `Update` refines its row
/// lengths: `((v·r)·r − 3)·(−0.5·v·r)`.
f32 NewtonLength(f32 v, f32 seed) {
    const f32 vr = v * seed;
    return ((vr * seed) + -3.0f) * (-0.5f * vr);
}

/// `RandomNextU32` reinterpreted as a float in `[1, 2)` and biased by −1: the
/// collision-spawn and splat chances, drawn inline rather than through
/// `Rand_RangeF` (RE §16.10).
f32 UnitDraw(sc2::Rng& rng) {
    const u32 bits = (rng.NextU32() & 0x7FFFFFu) | 0x3F800000u;
    return std::bit_cast<f32>(bits) + -1.0f;
}

using Mat16 = std::array<f32, 16>;

/// `Mat4Adjugate` (`0x10056A760`), term for term: the transposed cofactors of
/// a row-major 4×4, before any division.
Mat16 Adjugate(const Mat16& a) {
    const f32 v2 = a[9], v65 = a[13], v3 = a[14], v4 = a[1], v5 = a[2], v50 = a[12];
    const f32 v60 = v50 * v4;
    const f32 v45 = a[10];
    const f32 v55 = v4 * v45 - v5 * v2;
    const f32 v49 = v5 * v65 - v4 * v3;
    const f32 v53 = a[8];
    const f32 v6 = v53 * v4;
    const f32 v7 = a[4];
    const f32 v8 = v7 * v4;
    const f32 v9 = a[6];
    const f32 v10 = a[5];
    const f32 v11 = v4 * v9 - v5 * v10;
    const f32 v58 = v7 * v45 - v53 * v9;
    const f32 v67 = v50 * v9 - v7 * v3;
    const f32 v57 = v50 * v5 - a[0] * v3;
    const f32 v12 = a[0] * v9 - v7 * v5;
    const f32 v64 = v7 * v2 - v53 * v10;
    const f32 v52 = v53 * v65;
    const f32 v13 = v7 * v65;
    const f32 v51 = v50 * v2;
    const f32 v48 = v2 * a[0];
    const f32 v14 = a[0] * v10 - v8;
    const f32 v41 = v65 * v9;
    const f32 v42 = v65 * v45;
    const f32 v15 = v3 * v10;
    const f32 v16 = a[11];
    const f32 v17 = (v65 * v9 - v3 * v10) * v16;
    const f32 v18 = a[15];
    const f32 v62 = v53 * v3;
    const f32 v40 = a[0] * v45;
    const f32 v54 = v53 * v5;
    const f32 v61 = v60 - v65 * a[0];
    const f32 v43 = (v50 * v10 - v13) * v45;
    const f32 v66 = (v48 - v6) * v3;
    const f32 v19 = v45 * v10 - v2 * v9;
    const f32 v20 = v3 * v2 - v42;
    const f32 v21 = a[7];
    const f32 v22 = (v20 * v21 + v18 * v19) + v17;
    const f32 v23 = a[3];
    const f32 v24 = (v20 * v23 + v55 * v18) + v49 * v16;
    const f32 v25 = ((v15 - v41) * v23 + v11 * v18) + v49 * v21;
    const f32 v56 = v55 * v21 - (v19 * v23 + v11 * v16);
    const f32 v26 = v62 - v50 * v45;
    const f32 v27 = (v26 * v21 + v58 * v18) + v67 * v16;
    const f32 v63 = (v26 * v23 + (v40 - v54) * v18) + v57 * v16;
    const f32 v68 = (v67 * v23 - v12 * v18) - v57 * v21;
    const f32 v59 = (v58 * v23 + v12 * v16) + (v54 - v40) * v21;
    const f32 v28 = ((v52 - v51) * v21 + v64 * v18) + (v50 * v10 - v13) * v16;
    const f32 v29 = ((v52 - v51) * v23 + (v48 - v6) * v18) + v61 * v16;
    const f32 v30 = v13 - v50 * v10;
    const f32 v31 = (v30 * v23 + v18 * v14) + v61 * v21;
    const f32 v32 = v6 - v48;
    return {v22,
            -v24,
            v25,
            v56,
            -v27,
            v63,
            v68,
            v59,
            v28,
            -v29,
            v31,
            -((v23 * v64 + v16 * v14) + v21 * v32),
            -(((v52 - v51) * v9 + v64 * v3) + v43),
            ((v52 - v51) * v5 + v66) + v61 * v45,
            -((v30 * v5 + v3 * v14) + v61 * v9),
            (v64 * v5 + v14 * v45) + v32 * v9};
}

/// The determinant `SimulateParticles` builds inline before it divides, in its
/// own SSE grouping — the same number the adjugate would give, spelled
/// differently in the last bits.
f32 InlineDeterminant(const Mat16& w) {
    const f32 c0 = (w[7] * (w[8] * w[14] - w[10] * w[12]) + w[15] * (w[4] * w[10] - w[6] * w[8])) +
                   w[11] * (w[12] * w[6] - w[14] * w[4]);
    const f32 c1 = (w[7] * (w[9] * w[14] - w[10] * w[13]) + w[15] * (w[5] * w[10] - w[6] * w[9])) +
                   w[11] * (w[13] * w[6] - w[14] * w[5]);
    const f32 a = w[4] * w[9] - w[8] * w[5];
    const f32 b = w[8] * w[13] - w[9] * w[12];
    const f32 c = w[12] * w[5] - w[4] * w[13];
    const f32 s0 = (b * w[6] + a * w[14]) + c * w[10];
    const f32 s1 = (b * w[7] + a * w[15]) + c * w[11];
    return (w[2] * s1 + w[0] * c1) - (w[3] * s0 + w[1] * c0);
}

/// The way a contact comes back into a local-space emitter: the adjugate over
/// the determinant, or identity with the translation negated below the floor.
struct LocalFrame {
    std::array<f32, 9> r{};
    std::array<f32, 3> t{};
};

LocalFrame BuildLocalFrame(const Mat16& w) {
    LocalFrame f;
    const f32 det = InlineDeterminant(w);
    if (det <= kMinInvertibleDet) {
        f.r = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        f.t = {-w[12], -w[13], -w[14]};
        return f;
    }
    const Mat16 adj = Adjugate(w);
    const f32 s = 1.0f / det;
    f.r = {adj[0] * s, adj[1] * s, adj[2] * s, adj[4] * s, adj[5] * s,
           adj[6] * s, adj[8] * s, adj[9] * s, adj[10] * s};
    f.t = {adj[12] * s, adj[13] * s, s * adj[14]};
    return f;
}

Vector3f PointToLocal(const LocalFrame& f, const Vector3f& p) {
    return {(p.z * f.r[6] + p.y * f.r[3]) + (p.x * f.r[0] + f.t[0]),
            (p.z * f.r[7] + p.y * f.r[4]) + (p.x * f.r[1] + f.t[1]),
            (p.z * f.r[8] + p.y * f.r[5]) + (p.x * f.r[2] + f.t[2])};
}

/// Through the inverse basis, and normalised on the way — so the response a
/// local emitter computes is against a unit normal even when it is scaled.
Vector3f NormalToLocal(const LocalFrame& f, const Vector3f& n) {
    const f32 x = n.z * f.r[6] + (n.y * f.r[3] + n.x * f.r[0]);
    const f32 y = n.z * f.r[7] + (n.y * f.r[4] + n.x * f.r[1]);
    const f32 z = n.z * f.r[8] + (n.y * f.r[5] + n.x * f.r[2]);
    const f32 sq = (z * z + x * x) + y * y;
    const f32 r = NewtonRsqrt(sq, 1.0f / std::sqrt(sq));
    return {x * r, y * r, r * z};
}

Vector3f PointToWorld(const Mat16& m, const Vector3f& p) {
    return {(m[8] * p.z + (m[4] * p.y + m[0] * p.x)) + m[12],
            (m[9] * p.z + (m[5] * p.y + m[1] * p.x)) + m[13],
            (p.z * m[10] + (p.y * m[6] + p.x * m[2])) + m[14]};
}

/// Out again for a request, and NOT renormalised: a scaled emitter hands its
/// child a scaled normal (OP9 `cspawn`).
Vector3f NormalToWorld(const Mat16& m, const Vector3f& n) {
    return {m[8] * n.z + (m[4] * n.y + m[0] * n.x), m[9] * n.z + (m[5] * n.y + m[1] * n.x),
            n.z * m[10] + (n.y * m[6] + n.x * m[2])};
}

/// Below `|v|² < 3·dt` a landed particle stops spinning AT THE ANGLE IT
/// LANDED ON: all three rotation keys are overwritten with the curve's value
/// now, truncated back into the s16 they are stored as — not reset to key 0.
void RekeyRotationAtRest(Sc2SpawnedElement& e, const Sc2SimulateInputs& in) {
    namespace vs = whiteout::flakes::renderer::sc2::vs;
    const f32 age = vs::Saturate((in.emitterTime - e.birthTime) / (e.deathTime - e.birthTime));
    // The reciprocal before the curve, as `EvalAnimCurve1D` builds it (§16.4).
    const f32 invMid = 1.0f / in.rotationMidTime;
    const auto key = [&](usize k) {
        return static_cast<f32>(static_cast<i16>(e.rotation[k])) * (1.0f / 32.0f);
    };
    const f32 angle = vs::InterpolateValue(age, key(0), key(1), key(2), in.rotationMidTime,
                                           invMid, in.rotationMidHold, in.rotationSmoothing);
    const u16 q = static_cast<u16>(static_cast<i32>(angle * 32.0f));
    e.rotation = {q, q, q};
}

} // namespace

void Sc2StepEuler(Sc2SpawnedElement& e, const Sc2SimulateInputs& in) {
    Vector3f a{0.0f, 0.0f, 0.0f};
    if ((e.flags & bits::kElemAtRest) == 0) {
        a = in.gravity;
        if ((in.parFlags & kParSceneGravity) != 0)
            a = {a.x * in.gravityScale, a.y * in.gravityScale, a.z * in.gravityScale};
    }

    // Over-damped, the drag cancels the whole velocity in one step and what is
    // left is gravity's. Both branches are the binary's; OP9 arms each side of
    // `k·dt == 1` and a zero inverse mass that switches drag off entirely.
    const f32 k = in.drag * e.invMass;
    if (k * in.dt >= 1.0f) {
        a = {a.x - e.velocity.x / in.dt, a.y - e.velocity.y / in.dt,
             a.z - e.velocity.z / in.dt};
    } else {
        a = {a.x - k * e.velocity.x, a.y - k * e.velocity.y, a.z - k * e.velocity.z};
    }

    const Vector3f before = e.velocity;
    e.velocity = {e.velocity.x + a.x * in.dt, e.velocity.y + a.y * in.dt,
                  e.velocity.z + a.z * in.dt};
    const Vector3f wind{in.wind.x * in.windMultiplier, in.wind.y * in.windMultiplier,
                        in.wind.z * in.windMultiplier};
    e.position = {e.position.x + (e.velocity.x + wind.x) * in.dt,
                  e.position.y + (e.velocity.y + wind.y) * in.dt,
                  e.position.z + (e.velocity.z + wind.z) * in.dt};

    // The threshold reads the stepped velocity and the direction the one it
    // stepped FROM (RE §6). No OP9 vector separates the two — every freeze row
    // runs without gravity or drag — so this follows the disassembly.
    if (in.instanceType == static_cast<u32>(Sc2InstanceType::TerrainDirOriented) &&
        (e.flags & bits::kElemOrientationFrozen) == 0) {
        const Vector3f& v = e.velocity;
        if ((v.z * v.z + v.x * v.x) + v.y * v.y < 0.001f) {
            const f32 lsq = (before.z * before.z + before.x * before.x) + before.y * before.y;
            // An exact reciprocal root and one Newton step. A zero velocity
            // turns the refinement into a NaN, and that is what sends it to
            // (1,0,0) — the binary classifies the result, not the length.
            const f32 r = NewtonRsqrt(lsq, 1.0f / std::sqrt(lsq));
            if (std::isfinite(r))
                e.orientVec = {before.x * r, before.y * r, before.z * r};
            else
                e.orientVec = {1.0f, 0.0f, 0.0f};
            e.flags = static_cast<u16>(e.flags | bits::kElemOrientationFrozen);
        }
    }
}

namespace {

/// The collision half of one element's sub-step: the swept query in world
/// space, the contact back into the element's space, the response and its
/// rest transition, the bounce count, and what a hit asks of child 0.
void CollideElement(Sc2SpawnedElement& e, const Vector3f& from, const Sc2SimulateInputs& in,
                    const Sc2Collider& collider, const LocalFrame& local, const Vector3f& wind,
                    sc2::Rng& rng, Sc2ChildRequests& children) {
    const Vector3f a = in.worldSpace ? from : PointToWorld(in.worldMatrix, from);
    const Vector3f b = in.worldSpace ? e.position : PointToWorld(in.worldMatrix, e.position);

    Sc2Contact c;
    bool terrainHit = false;
    if ((e.flags & bits::kElemCollideTerrain) != 0 && collider.terrain) {
        Sc2Contact t;
        if (collider.terrain(collider.ctx, a, b, t) && t.hit) {
            // Pushed out in WORLD space, and the time overwritten with 1 — so
            // a terrain bounce always lands exactly on the pushed point.
            t.position = {t.normal.x * kCollideRadius + t.position.x,
                          t.normal.y * kCollideRadius + t.position.y,
                          t.normal.z * kCollideRadius + t.position.z};
            t.toi = 1.0f;
            c = t;
            terrainHit = true;
        }
    }
    bool objectHit = false;
    if ((e.flags & bits::kElemCollideObjects) != 0 && collider.objects) {
        Sc2Contact o;
        if (collider.objects(collider.ctx, a, b, o) && o.hit) {
            c = o;
            objectHit = true;
        }
    }
    if (!terrainHit && !objectHit)
        return;
    // Retail swaps in the terrain's own normal field here (`TestShapeCutout`,
    // then `SampleVectorField2D`). A viewer's ground answers with its normal
    // already and has no cutout to test.

    Vector3f p = c.position;
    Vector3f n = c.normal;
    if (!in.worldSpace) {
        p = PointToLocal(local, p);
        n = NormalToLocal(local, n);
    }

    // A particle already leaving the surface is not bounced, does not land on
    // the contact, and asks nothing of its child.
    const Vector3f mv{wind.x + e.velocity.x, wind.y + e.velocity.y, wind.z + e.velocity.z};
    const f32 d = (n.z * mv.z + n.x * mv.x) + n.y * mv.y;
    if (d >= 0.0f)
        return;

    const Vector3f vn{d * n.x, d * n.y, n.z * d};
    const f32 nb = -in.bounce;
    Vector3f r{nb * vn.x, nb * vn.y, nb * vn.z};
    // `|v + wind|²` and `v + wind − vn`: the friction gate and the tangent both
    // read the MOVING velocity, not the particle's own.
    if ((mv.z * mv.z + mv.x * mv.x) + mv.y * mv.y > kFrictionSpeedSq) {
        r.x = r.x + in.friction * (mv.x - vn.x);
        r.y = r.y + in.friction * (mv.y - vn.y);
        r.z = r.z + (mv.z - vn.z) * in.friction;
    }

    // The wind comes back OUT, so it never accumulates into the particle's
    // own momentum across a bounce either.
    e.velocity = {r.x - wind.x, r.y - wind.y, r.z - wind.z};
    const Vector3f& v = e.velocity;
    if ((v.z * v.z + v.x * v.x) + v.y * v.y < kRestPerDt * in.dt) {
        e.velocity = {0.0f, 0.0f, 0.0f};
        e.flags = static_cast<u16>(e.flags | bits::kElemAtRest);
        if (in.instanceType != static_cast<u32>(Sc2InstanceType::TerrainDirOriented))
            RekeyRotationAtRest(e, in);
    }

    // The rest of the step from the contact. Only an object contact leaves
    // `toi` below 1, and it continues with the response as it was BEFORE the
    // wind came out and before a rest zeroed it.
    const f32 rest = 1.0f - c.toi;
    e.position = {(r.x * in.dt) * rest + p.x, (r.y * in.dt) * rest + p.y,
                  rest * (r.z * in.dt) + p.z};
    if (++e.bounceCount == static_cast<i32>(in.collisionDieBounce))
        e.deathTime = 0.0f;

    // Element flag 2 skips the chance roll, so the two arms leave the stream
    // at different words and the count drawn next differs (OP9 `cdraw`).
    const bool forced = (e.flags & kElemForced) != 0;
    if (in.collisionChild && (forced || UnitDraw(rng) <= in.collisionSpawnChance)) {
        SpawnRequest req;
        req.position = p;
        req.velocityScale = {in.collisionSpawnEnergy, in.collisionSpawnEnergy,
                             in.collisionSpawnEnergy};
        req.orientVec = n;
        if (!in.worldSpace) {
            req.position = PointToWorld(in.worldMatrix, p);
            req.orientVec = NormalToWorld(in.worldMatrix, n);
        }
        for (u32 k = rng.RangeInt(in.collisionSpawnMin, in.collisionSpawnMax); k != 0; --k)
            children.collision.push_back(req);
        e.deathTime = 0.0f;
    }
    if (in.splat && (forced || UnitDraw(rng) <= in.splatChance))
        e.deathTime = 0.0f;
}

/// `trailAccum` drains on a STRICT `> 1`, one request per whole unit, each with
/// a fresh random direction as its velocity scale (RE §16.10).
void QueueTrails(Sc2SpawnedElement& e, const Sc2SimulateInputs& in, sc2::Rng& rng,
                 Sc2ChildRequests& children) {
    SpawnRequest req;
    req.position = e.position;
    req.velocityScale = {0.0f, 0.0f, 0.0f};
    req.orientVec = {0.0f, 0.0f, 0.0f};
    e.trailAccum = in.trailRate * in.dt + e.trailAccum;
    if (e.trailAccum <= 1.0f)
        return;
    do {
        const f32 x = rng.RangeF(-1.0f, 1.0f);
        const f32 y = rng.RangeF(-1.0f, 1.0f);
        const f32 z = rng.RangeF(-1.0f, 1.0f);
        const f32 sq = (y * y + x * x) + z * z;
        const f32 r = NewtonRsqrt(sq, 1.0f / std::sqrt(sq));
        req.velocityScale = {x * r, y * r, r * z};
        children.trail.push_back(req);
        e.trailAccum = e.trailAccum + -1.0f;
    } while (e.trailAccum > 1.0f);
}

} // namespace

Sc2SimulateResult Sc2SimulateParticles(Sc2ElementList& list,
                                       std::span<Sc2SpawnedElement> elements,
                                       const Sc2SimulateInputs& in,
                                       const Sc2Collider& collider, sc2::Rng& rng,
                                       Sc2ChildRequests& children, std::vector<i32>* killed) {
    Sc2SimulateResult out;
    const Vector3f wind{in.wind.x * in.windMultiplier, in.wind.y * in.windMultiplier,
                        in.wind.z * in.windMultiplier};
    const bool bounds = (in.parFlags & kParBounds) != 0;
    // Built once before the walk, whether or not anything collides, as retail
    // builds it.
    const LocalFrame local = BuildLocalFrame(in.worldMatrix);

    i32 node = list.head;
    while (node >= 0) {
        const i32 nxt = list.next[static_cast<usize>(node)];
        Sc2SpawnedElement& e = elements[static_cast<usize>(node)];
        const Vector3f from = e.position;
        Sc2StepEuler(e, in);

        // Only a particle still alive is collided, so one that dies this
        // sub-step neither bounces nor asks its child for a spawn.
        if (e.deathTime > in.emitterTime &&
            (e.flags & (bits::kElemCollideTerrain | bits::kElemCollideObjects)) != 0 &&
            in.collisionEnabled) {
            CollideElement(e, from, in, collider, local, wind, rng, children);
        }

        // After the collision, so a particle a hit killed lays no trail.
        if (e.deathTime > in.emitterTime && (e.flags & bits::kElemTrail) != 0 && in.trailChild)
            QueueTrails(e, in, rng, children);

        if (bounds) {
            out.boundsMin = {(std::min)(out.boundsMin.x, e.position.x),
                             (std::min)(out.boundsMin.y, e.position.y),
                             (std::min)(out.boundsMin.z, e.position.z)};
            out.boundsMax = {(std::max)(out.boundsMax.x, e.position.x),
                             (std::max)(out.boundsMax.y, e.position.y),
                             (std::max)(out.boundsMax.z, e.position.z)};
        }

        // Both kill tests run after the bounds pass, so a particle dying this
        // step still widens them. The radius is compared AS AUTHORED against the
        // squared distance: OP9's radius-10 row is what separates that from the
        // squared radius this step once used.
        const f32 dx = e.position.x - in.origin.x;
        const f32 dy = e.position.y - in.origin.y;
        const f32 dz = e.position.z - in.origin.z;
        const bool survives =
            e.deathTime > in.emitterTime &&
            (in.killRadius <= 0.0f || dz * dz + (dy * dy + dx * dx) <= in.killRadius);
        if (!survives) {
            list.Unlink(node);
            ++out.killed;
            if (killed != nullptr)
                killed->push_back(node);
        }
        node = nxt;
    }
    return out;
}

void Sc2QueueSpawnRequest(std::vector<SpawnRequest>& queue, const SpawnRequest& req) {
    if (queue.size() < kSc2MaxSpawnRequests)
        queue.push_back(req);
}

Vector3f Sc2ChildScale(const std::array<f32, 16>& w) {
    const f32 sq0 = w[2] * w[2] + (w[1] * w[1] + w[0] * w[0]);
    const f32 sq1 = w[6] * w[6] + (w[5] * w[5] + w[4] * w[4]);
    const f32 sq2 = w[10] * w[10] + (w[9] * w[9] + w[8] * w[8]);
#if WDX_SC2_HAS_RCPSS
    // Rows 0 and 1 in one packed `rsqrtps`, whose seed is the hardware's
    // approximation - which is why the gate holds these two lanes to the rsqrt
    // bound and not to the bit.
    const __m128 seeds = _mm_rsqrt_ps(_mm_set_ps(0.0f, 0.0f, sq1, sq0));
    const f32 seed0 = _mm_cvtss_f32(seeds);
    const f32 seed1 = _mm_cvtss_f32(_mm_shuffle_ps(seeds, seeds, _MM_SHUFFLE(1, 1, 1, 1)));
#else
    const f32 seed0 = 1.0f / std::sqrt(sq0);
    const f32 seed1 = 1.0f / std::sqrt(sq1);
#endif
    // Each lane masked against a length-squared of exactly zero, so a collapsed
    // row pushes 0 where the refinement would have made a NaN.
    return {sq0 != 0.0f ? NewtonLength(sq0, seed0) : 0.0f,
            sq1 != 0.0f ? NewtonLength(sq1, seed1) : 0.0f,
            sq2 != 0.0f ? NewtonLength(sq2, 1.0f / std::sqrt(sq2)) : 0.0f};
}

Matrix44f Sc2PushChildScale(const Matrix44f& childBone, const Vector3f& localScale,
                            const Vector3f& pushed) {
    Matrix44f out = childBone;
    const f32 local[3] = {localScale.x, localScale.y, localScale.z};
    const f32 push[3] = {pushed.x, pushed.y, pushed.z};
    for (usize r = 0; r < 3; ++r) {
        if (local[r] == 0.0f)
            continue;
        const f32 k = push[r] / local[r];
        for (usize c = 0; c < 3; ++c)
            out.data[r][c] = childBone.data[r][c] * k;
    }
    return out;
}

namespace {

/// The floor and the reciprocal's numerator are two separate globals in the
/// binary; naming them keeps the comparison's operand honest.
constexpr f32 kDragFloor = 0.01f;
constexpr f32 kInvDragUnderFloor = 100.0f;

void Store3(f32 (&dst)[3], const Vector3f& v) {
    dst[0] = v.x;
    dst[1] = v.y;
    dst[2] = v.z;
}

} // namespace

Sc2DragLanes Sc2ComputeDragLanes(f32 drag) {
    Sc2DragLanes out;
    out.drag = (std::max)(kDragFloor, drag);
    // The comparison is against the RAW drag, before the floor.
    out.invDrag = drag < kDragFloor ? kInvDragUnderFloor : 1.0f / drag;
    return out;
}

Vector3f Sc2InstanceVector(const Sc2VertexBodyInputs& in, const Sc2SpawnedElement& e) {
    switch (static_cast<Sc2InstanceType>(in.instanceType)) {
    case Sc2InstanceType::Tail:
    case Sc2InstanceType::Trail:
        return {in.tailLength, 0.0f, 0.0f};
    case Sc2InstanceType::FaceTravelDir:
        return e.velocity;
    case Sc2InstanceType::FaceWorldDir:
    case Sc2InstanceType::SingleAxis:
        return in.instanceAngle;
    case Sc2InstanceType::EmitterOriented:
    case Sc2InstanceType::PhysicsOriented:
        return e.orientVec;
    case Sc2InstanceType::Pinned:
        return e.spawnOrigin;
    default:
        return {0.0f, 0.0f, 0.0f};
    }
}

std::array<Sc2GpuVertex, 4> Sc2VertexBody(const Sc2VertexBodyInputs& in,
                                          const Sc2SpawnedElement& e) {
    Sc2GpuVertex v{};
    Store3(v.position, e.position);
    // The element's lane, which nothing writes and no shader path reads.
    v.positionW = 0;
    for (usize i = 0; i < 4; ++i)
        v.size[i] = e.size[i];
    for (usize i = 0; i < 3; ++i)
        v.color[i] = e.colorNodes[i];
    for (usize i = 0; i < 3; ++i)
        v.rotation[i] = e.rotation[i];
    v.flipbookRand = e.flipbookRand;
    v.birthTime = e.birthTime;
    v.deathTime = e.deathTime;

    const Sc2DragLanes drag = Sc2ComputeDragLanes(in.drag);
    v.drag = drag.drag;
    v.invDrag = drag.invDrag;
    v.batchIndex = in.batchIndex;

    // One 16-byte copy in the binary, which is why `invMass` travels with the
    // velocity instead of staying the 0 the pending-spawn path leaves.
    Store3(v.velocity, e.velocity);
    v.invMass = e.invMass;

    Store3(v.instanceVec, Sc2InstanceVector(in, e));
    v.gravityZ = in.worldGravityScale * in.gravity;

    Store3(v.noise, e.noiseVec);
    v.flipbookRandStart = e.flipbookRandStart;

    std::array<Sc2GpuVertex, 4> quad{};
    for (usize k = 0; k < 4; ++k) {
        quad[k] = v;
        quad[k].corner[0] = kSc2Corners[k][0];
        quad[k].corner[1] = kSc2Corners[k][1];
    }
    return quad;
}

void Sc2CpuVertexBody(const Sc2CpuVertexInputs& in, Sc2SpawnedElement& e, Sc2GpuVertex& cache) {
    constexpr u32 kClampTailLength = 0x40000;
    constexpr u32 kFixTailLengthOnCreation = 0x100000;

    // A ONE-byte store: `HIBYTE(*(u32*)(sys+0x34C))` into element `0x80`, and the
    // four-byte lane then copied — so the upper three bytes are whatever the
    // element held. OP11's `hole` rows poison them to show it.
    cache.batchIndex = (cache.batchIndex & 0xFFFFFF00u) | (in.batchIndex & 0xFFu);

    if (in.gpuMotion) {
        const Sc2DragLanes drag = Sc2ComputeDragLanes(in.drag);
        cache.drag = drag.drag;
        cache.invDrag = drag.invDrag;
        Store3(cache.velocity, e.velocity);
        cache.invMass = e.invMass;
        cache.gravityZ = in.gravity * in.gravityScale;
    }

    const f32 elapsed = in.emitterTime - e.birthTime;
    const f32 age = whiteout::flakes::renderer::sc2::vs::Saturate(elapsed / (e.deathTime - e.birthTime));

    if (in.noise) {
        // The ramp HOLDS above the edge: past it the amplitude is simply full.
        const f32 ramp = age < in.noiseEdge ? age / in.noiseEdge : 1.0f;
        const f32 amp = in.noiseAmplitude * ramp;
        f32 s[3];
        whiteout::flakes::renderer::sc2::GlobalNoiseTable().Sample(
            elapsed * in.noiseFrequency, age * in.noiseCoherence + e.noisePhase, s);
        // Into the ELEMENT, then copied: on this path the element's cache is
        // the shader's input.
        e.noiseVec = {amp * s[0], amp * s[1], amp * s[2]};
    }

    // Types 5 and 6 read the terrain's vector field under the particle, and
    // their gravity lane becomes a has-field flag rather than a gravity.
    const auto terrainLanes = [&] {
        if (in.field) {
            f32 lanes[2];
            in.field(in.fieldCtx, e.position.x, e.position.y, lanes);
            cache.instanceVec[0] = lanes[0];
            cache.instanceVec[1] = lanes[1];
            cache.instanceVec[2] = e.position.y;
            cache.gravityZ = 1.0f;
        } else {
            cache.instanceVec[0] = 0.0f;
            cache.instanceVec[1] = 0.0f;
            cache.instanceVec[2] = 1.0f;
            cache.gravityZ = 0.0f;
        }
        cache.invMass = 1.0f;
    };

    switch (static_cast<Sc2InstanceType>(in.instanceType)) {
    case Sc2InstanceType::Tail:
    case Sc2InstanceType::Trail: {
        Store3(cache.velocity, e.velocity);
        // `.x` alone — the list builder leaves `.y/.z` as the cache had them.
        cache.instanceVec[0] = in.tailLength;
        if ((in.parFlags & kClampTailLength) != 0 && !in.gpuMotion) {
            const Vector3f d{e.position.x - e.spawnOrigin.x, e.position.y - e.spawnOrigin.y,
                             e.position.z - e.spawnOrigin.z};
            const f32 dist2 = EulerDot(d, d);
            const f32 speed = std::sqrt(EulerDot(e.velocity, e.velocity));
            f32 reach = in.tailLength * speed;
            if ((in.parFlags & kFixTailLengthOnCreation) == 0)
                reach = (std::max)(reach, in.tailLength);
            if (dist2 >= reach * reach) {
                // Not an epsilon: nothing writes `spawnOrigin.x` back, so this
                // pins the particle in the long branch for the rest of its
                // life — and destroys the spawn origin for anything else.
                e.spawnOrigin.x = e.position.x + 10000.0f;
            } else {
                // A TIME, on the short branch only: the distance over the
                // speed, in units of the particle's size now — the same
                // two-segment knee the spawn size uses.
                const f32 k0 = static_cast<f32>(e.size[0]) / 256.0f;
                const f32 k1 = static_cast<f32>(e.size[1]) / 256.0f;
                const f32 k2 = static_cast<f32>(e.size[2]) / 256.0f;
                const f32 mid = in.sizeMidTime;
                const f32 sizeNow = age < mid ? k0 + (k1 - k0) * (age / mid)
                                              : k1 + (k2 - k1) * ((age - mid) / (1.0f - mid));
                cache.instanceVec[0] = std::sqrt(dist2) / (speed * sizeNow);
            }
        }
        break;
    }
    case Sc2InstanceType::FaceTravelDir:
        Store3(cache.instanceVec, e.velocity);
        break;
    case Sc2InstanceType::FaceWorldDir:
    case Sc2InstanceType::SingleAxis:
        Store3(cache.instanceVec, in.instanceAngle);
        break;
    case Sc2InstanceType::TerrainOriented:
        terrainLanes();
        Store3(cache.velocity, in.instanceAngle);
        break;
    case Sc2InstanceType::TerrainDirOriented: {
        terrainLanes();
        Store3(cache.velocity,
               EulerDot(e.velocity, e.velocity) < 0.001f ? e.orientVec : e.velocity);
        // The tail length, not `instanceAngle.x`, and one dword store that zeroes
        // the other two keys and stops short of `flipbookRand`.
        e.rotation = {static_cast<u16>(static_cast<i32>(in.tailLength * 32.0f)), 0, 0};
        break;
    }
    case Sc2InstanceType::EmitterOriented:
    case Sc2InstanceType::PhysicsOriented:
        Store3(cache.instanceVec, e.orientVec);
        break;
    case Sc2InstanceType::Pinned:
        Store3(cache.instanceVec, e.spawnOrigin);
        break;
    default:
        // Type 0 writes nothing: the cache keeps whatever was last there.
        break;
    }

    // The one 112-byte run from the element.
    Store3(cache.position, e.position);
    for (usize i = 0; i < 4; ++i)
        cache.size[i] = e.size[i];
    for (usize i = 0; i < 3; ++i) {
        cache.color[i] = e.colorNodes[i];
        cache.rotation[i] = e.rotation[i];
    }
    cache.flipbookRand = e.flipbookRand;
    cache.birthTime = e.birthTime;
    cache.deathTime = e.deathTime;
    Store3(cache.noise, e.noiseVec);
    cache.flipbookRandStart = e.flipbookRandStart;
}

namespace {

/// HLSL integer `/`: truncation toward zero, not C++'s — which agrees, but
/// only since C++11, and the shader's intent is worth spelling out.
i32 IDiv(i32 a, i32 b) {
    const i32 q = std::abs(a) / std::abs(b);
    return (a < 0) != (b < 0) ? -q : q;
}

i32 IMod(i32 a, i32 b) {
    return a - IDiv(a, b) * b;
}

} // namespace

Vector2f Sc2ParticleUv(const Sc2QuadInput& v, const i16 (&corner)[2], f32 age,
                       const Sc2QuadBatch& b, const Sc2QuadFlags& fl) {
    f32 u = static_cast<f32>(corner[0]) * 0.5f + 0.5f;
    // The V axis is flipped, which is why the corner order reads bottom-up.
    f32 vv = static_cast<f32>(corner[1]) * -0.5f + 0.5f;

    if (fl.flipbookUv) {
        const f32 mid = b.flipbookMidKeyTime;
        f32 cellF = 0.0f;
        // `<=`, as the shader spells it — though nothing can tell it from `<`.
        // The grid does hit `age == mid` exactly (24 vectors), and both arms
        // return `flipbookFrames[1]` there: the start run ends on the frame
        // the end run begins on, and the indices are whole numbers, so
        // `floor(n + 0.5) == n`. A mutation to `<` stays green. That is an
        // equivalence, not a gap — widening the grid would never separate
        // them.
        if (age <= mid) {
            const f32 range = b.flipbookFrames[1] - b.flipbookFrames[0];
            cellF = b.flipbookFrames[0] + std::floor(range * (age / mid) + 0.5f);
        } else {
            const f32 range = b.flipbookFrames[2] - b.flipbookFrames[1];
            cellF = b.flipbookFrames[1] +
                    std::floor(range * ((age - mid) / (1.0f - mid)) + 0.5f);
        }
        i32 cell = static_cast<i32>(std::trunc(cellF));
        if (fl.randomFlipbookStart) {
            // The element's `flipbookRandStart` at +108, floored — a whole
            // number of cells, so two particles never land mid-frame.
            cell += static_cast<i32>(std::trunc(std::floor(v.flipbookRandStart)));
        }
        const f32 colsF = b.flipbookColumns == 0.0f ? 1.0f : b.flipbookColumns;
        const i32 cols = static_cast<i32>(std::trunc(colsF));
        const i32 cellX = IMod(cell, cols);
        const i32 cellY = IDiv(cell, cols);
        u = u * b.cellSize[0] + static_cast<f32>(cellX) * b.cellSize[0];
        vv = vv * b.cellSize[1] + static_cast<f32>(cellY) * b.cellSize[1];
    } else if (fl.uvRandomOffset) {
        // `vRotation.w` = the element's `flipbookRand` u16, split hi/lo and
        // divided by 255 — not 256, so a full byte shifts a whole tile.
        const f32 r = v.rotation[3];
        const f32 x = std::floor(r / 256.0f);
        const f32 y = r - x * 256.0f;
        u = u + x / 255.0f;
        vv = vv + y / 255.0f;
    }
    return {u, vv};
}

Sc2QuadResult Sc2ExpandQuad(const Sc2QuadInput& v, const Sc2QuadBatch& b,
                            const Sc2QuadCamera& cam, const Sc2QuadFlags& fl) {
    namespace vs = whiteout::flakes::renderer::sc2::vs;
    Sc2QuadResult out;
    const auto type = static_cast<Sc2InstanceType>(fl.instanceType);
    out.supported = fl.instanceType <= static_cast<u32>(Sc2InstanceType::Trail);

    f32 inSize[4];
    for (usize i = 0; i < 4; ++i)
        inSize[i] = v.size[i] * (1.0f / 256.0f);
    inSize[3] = vs::Saturate(inSize[3]);
    f32 inRot[3];
    for (usize i = 0; i < 3; ++i)
        inRot[i] = v.rotation[i] * (1.0f / 32.0f);

    out.age = vs::Saturate((b.systemTime - v.birthTime) / (v.deathTime - v.birthTime));

    // The SCALAR overload broadcast into .xyz — so the size takes mode 4's
    // substitution variant, never the float3 plateau.
    out.size = vs::InterpolateValue(out.age, inSize[0], inSize[1], inSize[2],
                                    b.midKey[0], b.invMidKey[0], b.hold[0],
                                    fl.sizeInterp);

    // The shader's own copy of `Input`. `CalculatePositionAndVelocity` writes
    // two of its registers back, and the tail types add the noise into one.
    Vector3f position = v.position;
    Vector3f interp1 = v.velocity;
    Vector3f interp2 = v.instanceVec;
    if (fl.proceduralPosition) {
        Sc2AnalyticInputs step;
        step.position = position;
        step.velocity0 = v.velocity;
        step.invMass = v.invMass;
        step.birthTime = v.birthTime;
        step.deathTime = v.deathTime;
        step.drag = v.drag;
        step.invDrag = v.invDrag;
        step.gravityZ = v.gravityZ;
        step.systemTime = b.systemTime;
        step.instanceType = fl.instanceType;
        step.tailLength = interp2.x;
        step.size = out.size;
        step.fixedTailLength = fl.fixedTailLength;
        step.clampedTailLength = fl.clampedTailLength;
        const Sc2AnalyticStep st = Sc2StepAnalytic(step);
        position = st.position;
        if (type == Sc2InstanceType::FaceTravelDir) {
            interp2 = st.velocity;
        } else if (type == Sc2InstanceType::Tail || type == Sc2InstanceType::Pinned ||
                   type == Sc2InstanceType::Trail) {
            interp1 = st.velocity;
            interp2.x = st.tailLength;
        }
    }
    if (fl.localSpace)
        position = vs::MulPointMat4(position, b.prWorld);
    // Noise is added in WORLD space, after the local transform, for every
    // instance type — so a rotated emitter matrix never turns it.
    position = vs::Add(position, v.noise);

    const f32 angle = vs::InterpolateValue(out.age, inRot[0], inRot[1], inRot[2],
                                           b.midKey[3], b.invMidKey[3], b.hold[3],
                                           fl.rotationInterp);
    const auto rgbOf = [&](usize k) {
        return Vector3f{v.color[k][0], v.color[k][1], v.color[k][2]};
    };
    const Vector3f rgb = vs::InterpolateValue3(
        out.age, rgbOf(0), rgbOf(1), rgbOf(2), b.midKey[1], b.invMidKey[1],
        b.hold[1], fl.colorInterp);
    out.color[0] = rgb.x;
    out.color[1] = rgb.y;
    out.color[2] = rgb.z;
    // The alpha carries its OWN mid key, not the RGB one.
    out.color[3] = vs::InterpolateValue(
        out.age, v.color[0][3], v.color[1][3], v.color[2][3], b.midKey[2],
        b.invMidKey[2], b.hold[2], fl.colorInterp);

    const f32 scale = b.elementScale;

    // The two camera-facing idioms disagree exactly on the particle's plane:
    // 5 and 6 negate with a `> 0` ternary, the 2/3/7/8 second pass multiplies
    // by `sign()` and so ZEROES the frame there (OP12). Both run on the
    // corner's final position.
    const auto faceCamera = [&](const Vector3f& p, Sc2QuadCorner& c, bool useSign) {
        const f32 d = -vs::Dot3(p, c.normal);
        const f32 dist = ((cam.eye.x * c.normal.x + cam.eye.y * c.normal.y) +
                          cam.eye.z * c.normal.z) +
                         1.0f * d;
        f32 sgn = 1.0f;
        if (useSign)
            sgn = dist > 0.0f ? 1.0f : (dist < 0.0f ? -1.0f : 0.0f);
        else if (!(dist > 0.0f))
            sgn = -1.0f;
        c.normal = vs::Scale(c.normal, sgn);
        c.tangent = vs::Scale(c.tangent, sgn);
        c.binormal = vs::Scale(c.binormal, sgn);
    };

    // `UnpackNormals` for 2/3/7/8. Type 7 in local space takes the emitter's
    // own rows; in world space it unpacks the byte-packed basis spawn wrote.
    const auto unpackNormals = [&](Vector3f& right, Vector3f& up, Vector3f& forward) {
        if (type == Sc2InstanceType::EmitterOriented) {
            if (fl.localSpace) {
                right = vs::Normalize3({b.prWorld[0], b.prWorld[1], b.prWorld[2]});
                up = vs::Normalize3({b.prWorld[4], b.prWorld[5], b.prWorld[6]});
            } else {
                const f32 ry = std::trunc(interp2.x / 65536.0f);
                const f32 rx = interp2.x - ry * 65536.0f;
                const f32 rz = std::trunc(interp2.y / 65536.0f);
                const f32 ux = interp2.y - rz * 65536.0f;
                const f32 uy = std::trunc(interp2.z / 65536.0f);
                const f32 uz = interp2.z - uy * 65536.0f;
                const auto unit = [](f32 c) { return ((c / 255.0f) * 2.0f) - 1.0f; };
                right = vs::Normalize3({unit(rx), unit(ry), unit(rz)});
                up = vs::Normalize3({unit(ux), unit(uy), unit(uz)});
            }
            forward = vs::Normalize3(vs::Cross3(right, up));
            return;
        }
        forward = vs::Normalize3(vs::Add(interp2, Vector3f{0.0f, 0.0001f, 0.0f}));
        right = vs::Normalize3(vs::Cross3(Vector3f{0.0f, 0.0f, 1.0f}, forward));
        up = vs::Normalize3(vs::Cross3(forward, right));
    };

    for (usize k = 0; k < 4; ++k) {
        const f32 ox = static_cast<f32>(kSc2Corners[k][0]);
        const f32 oy = static_cast<f32>(kSc2Corners[k][1]);
        Sc2QuadCorner& c = out.corner[k];
        Vector3f p = position;

        switch (type) {
        case Sc2InstanceType::SingleAxis: {
            const Vector3f right = vs::Normalize3(vs::Cross3(interp2, cam.direction));
            const Vector3f forward = vs::Normalize3(vs::Cross3(right, interp2));
            const vs::Mat3 m = vs::MakeRotation(angle, forward);
            Vector3f off = vs::Add(vs::Scale(right, ox), vs::Scale(interp2, oy));
            off = vs::Scale(off, scale);
            p = vs::Add(p, vs::MulVecMat3(vs::Scale(off, out.size), m));
            c.normal = vs::Normalize3(vs::Cross3(right, forward));
            c.tangent = right;
            c.binormal = forward;
            break;
        }
        case Sc2InstanceType::FaceTravelDir:
        case Sc2InstanceType::FaceWorldDir:
        case Sc2InstanceType::EmitterOriented:
        case Sc2InstanceType::PhysicsOriented: {
            Vector3f right, up, direction;
            unpackNormals(right, up, direction);
            Vector3f off = vs::Add(vs::Scale(right, ox), vs::Scale(up, oy));
            off = vs::Scale(off, scale);
            const vs::Mat3 m = vs::MakeRotation(angle, direction);
            p = vs::Add(p, vs::MulVecMat3(vs::Scale(off, out.size), m));
            // The frame is the SECOND pass's, after the instance transform.
            break;
        }
        case Sc2InstanceType::TerrainOriented: {
            Vector3f projected =
                vs::Sub(interp1, vs::Scale(interp2, vs::Dot3(interp1, interp2)));
            const vs::Mat3 m = vs::MakeRotation(angle, interp2);
            if (vs::Dot3(projected, projected) < 0.01f)
                projected = {1.0f, 0.0f, 0.0f};
            projected = vs::Normalize3(projected);
            Vector3f right = vs::Cross3(projected, interp2);
            projected = vs::MulVecMat3(projected, m);
            right = vs::MulVecMat3(right, m);
            Vector3f off = vs::Add(vs::Scale(right, ox), vs::Scale(projected, oy));
            off = vs::Scale(off, scale);
            p = vs::Add(p, vs::Scale(off, out.size));
            c.normal = vs::Normalize3(vs::Cross3(right, projected));
            c.tangent = right;
            c.binormal = projected;
            faceCamera(p, c, false);
            break;
        }
        case Sc2InstanceType::TerrainDirOriented: {
            // No rotation at all: `rot.x` is a length SCALE here, which is
            // what the CPU builder's re-key of `rotation[0]` is for.
            const f32 mag = vs::Length3(interp1);
            const Vector3f direction = vs::Normalize3(interp1);
            Vector3f projected = vs::Normalize3(
                vs::Sub(direction, vs::Scale(interp2, vs::Dot3(direction, interp2))));
            const Vector3f right = vs::Cross3(projected, interp2);
            projected = vs::Scale(projected, (std::max)(inRot[0], mag * inRot[0]));
            Vector3f off = vs::Add(vs::Scale(right, ox), vs::Scale(projected, oy));
            off = vs::Scale(off, scale);
            p = vs::Add(p, vs::Scale(off, out.size));
            c.normal = vs::Normalize3(vs::Cross3(right, projected));
            c.tangent = right;
            c.binormal = projected;
            faceCamera(p, c, false);
            break;
        }
        case Sc2InstanceType::Tail:
        case Sc2InstanceType::Trail: {
            Vector3f velocity = vs::Add(interp1, v.noise);
            // Taken BEFORE the local-to-world rotation.
            const f32 mag = vs::Length3(velocity);
            if (fl.localSpace)
                velocity = vs::MulVecMat4As3(velocity, b.prWorld);
            Vector3f direction = vs::Normalize3(velocity);
            const Vector3f right = vs::Normalize3(vs::Cross3(cam.direction, direction));
            const f32 tail =
                fl.fixedTailLength ? interp2.x : (std::max)(interp2.x, mag * interp2.x);
            direction = vs::Scale(direction, tail);
            const Vector3f off = vs::Add(vs::Scale(right, ox), vs::Scale(direction, oy));
            const f32 vsize = scale * out.size;
            p = vs::Add(p, vs::Scale(off, vsize));
            if (type == Sc2InstanceType::Trail)
                p = vs::Sub(p, vs::Scale(direction, vsize));
            c.normal = vs::Normalize3(vs::Cross3(right, direction));
            c.tangent = right;
            // Scaled by the tail: this binormal is NOT unit (1, 6, 10 only).
            c.binormal = vs::Scale(direction, -1.0f);
            break;
        }
        case Sc2InstanceType::Pinned: {
            // The noise moved the head end only, so a noisy Pinned particle
            // lengthens its streak rather than displacing it.
            const Vector3f origin =
                fl.localSpace ? vs::MulPointMat4(interp2, b.prWorld) : interp2;
            const Vector3f delta = vs::Sub(p, origin);
            const Vector3f forward = vs::SafeNormalize(delta, Vector3f{1.0f, 0.0f, 0.0f});
            const Vector3f right = vs::Normalize3(vs::Cross3(cam.direction, forward));
            const f32 endScale = vs::Lerp(1.0f, inSize[3], oy * 0.5f + 0.5f);
            const Vector3f centre = vs::Scale(vs::Add(p, origin), 0.5f);
            // No elementScale on this branch — the one type whose quad does not
            // follow the emitter's world scale — and the only trapezoid.
            const Vector3f off = vs::Add(vs::Scale(vs::Scale(right, ox * out.size), endScale),
                                         vs::Scale(delta, 0.5f * oy));
            p = vs::Add(centre, off);
            c.normal = vs::Cross3(right, forward);
            c.tangent = right;
            c.binormal = vs::Scale(forward, -1.0f);
            break;
        }
        default: {
            // The billboard, and the shader's final `else` for anything past
            // the eleven. Type 0 transforms the position BEFORE building its
            // quad and skips the common instance transform after.
            if (fl.modelInstancing)
                p = vs::MulPointMat4(p, b.instanceTransform);
            const vs::Mat3 m = vs::MakeRotation(angle, cam.direction);
            Vector3f off = vs::Add(vs::Scale(cam.billboardRight, ox),
                                   vs::Scale(cam.billboardUp, oy));
            off = vs::Scale(off, scale);
            p = vs::Add(p, vs::MulVecMat3(vs::Scale(off, out.size), m));
            const Vector3f right = vs::MulVecMat3(cam.billboardRight, m);
            const Vector3f up = vs::MulVecMat3(cam.billboardUp, m);
            c.normal = vs::Normalize3(vs::Cross3(right, up));
            c.tangent = right;
            c.binormal = vs::Scale(up, -1.0f);
            break;
        }
        }

        const bool billboard = !out.supported || type == Sc2InstanceType::Billboard;
        if (fl.modelInstancing && !billboard)
            p = vs::MulPointMat4(p, b.instanceTransform);

        if (type == Sc2InstanceType::FaceTravelDir || type == Sc2InstanceType::FaceWorldDir ||
            type == Sc2InstanceType::EmitterOriented ||
            type == Sc2InstanceType::PhysicsOriented) {
            Vector3f right, up, direction;
            unpackNormals(right, up, direction);
            c.normal = direction;
            c.tangent = right;
            c.binormal = up;
            faceCamera(p, c, true);
        }

        c.position = p;
        c.uv = Sc2ParticleUv(v, kSc2Corners[k], out.age, b, fl);
    }
    return out;
}

f32 Sc2ElementScale(const std::array<f32, 16>& m) {
    // `z*z + (y*y + x*x)`, in that association — the binary adds the z term to
    // the already-summed pair, and float addition does not reassociate.
    const auto sq = [](f32 x, f32 y, f32 z) { return z * z + (y * y + x * x); };
    const f32 r0 = sq(m[0], m[1], m[2]);
    const f32 r1 = sq(m[4], m[5], m[6]);
    const f32 r2 = sq(m[8], m[9], m[10]);
    // Rows 0..2 only: neither the translation row nor the w column takes part,
    // which is what the `translate` and `wrow` vectors pin.
    const f32 mx = (std::max)(r2, (std::max)(r0, r1));
    // The zero guard is retail's, and it is INERT here: it exists because
    // `rsqrtss(0)` is an infinity that the refinement then multiplies by zero,
    // and `std::sqrt(0)` is simply 0. Removing it stays green over all 81
    // vectors and always would. Kept because it is what the binary does, and
    // because it is the line that would have to come back if this ever used an
    // estimate. Same for the association above: the two orderings differ by at
    // most an ulp of the squared sum, which is inside the tolerance the
    // approximate root already forces on this lane, so no vector can separate
    // them — it is transcribed from the disassembly, not measured.
    return mx == 0.0f ? 0.0f : std::sqrt(mx);
}

void Sc2WriteQuadBatch(Sc2QuadBatch& row, const Sc2BatchDesc& d,
                       const Sc2BatchFrame& f) {
    if (!d.worldSpace)
        row.prWorld = f.world;

    row.instanceTransform =
        f.hasInstanceNode ? f.instanceTransform
                          : std::array<f32, 16>{1, 0, 0, 0, 0, 1, 0, 0,
                                                0, 0, 1, 0, 0, 0, 0, 1};

    row.midKey = {d.sizeMidTime, d.colorMidTime, d.alphaMidTime,
                  d.rotationMidTime};
    // A plain division, with no floor and no guard: an authored mid time of 0
    // uploads an infinity, and the shader then multiplies it by an age of 0.
    // `invDrag` looked like the same shape in OP11b and was not, which is why
    // this one is measured rather than assumed. Computing it in double and
    // narrowing is bit-identical and not a second implementation: binary64 has
    // 53 bits and 53 >= 2 * 24 + 2, so double rounding a division through it is
    // provably innocuous. A mutant that does exactly that stays green, and no
    // widening of the grid would change that.
    for (usize i = 0; i < 4; ++i)
        row.invMidKey[i] = 1.0f / row.midKey[i];
    row.hold = {d.sizeMidHoldTime, d.colorMidHoldTime, d.alphaMidHoldTime,
                d.rotationMidHoldTime};

    row.systemTime = f.emitterTime;
    row.elementScale = Sc2ElementScale(f.world);

    if (d.flipbookColumns != 0 && d.flipbookRows != 0) {
        row.flipbookMidKeyTime = d.flipbookMidTime;
        row.flipbookColumns = static_cast<f32>(d.flipbookColumns);
        row.flipbookFrames = {static_cast<f32>(d.flipbookStartInitIndex),
                              static_cast<f32>(d.flipbookStartStopIndex),
                              static_cast<f32>(d.flipbookEndInitIndex)};
        row.cellSize = {d.flipbookColumnFraction, d.flipbookRowFraction};
    } else {
        // The shader's own comment calls the 1 a fix for an integer overflow
        // on the C++ side; the frames and the cell size are simply not written.
        row.flipbookMidKeyTime = 1.0f;
        row.flipbookColumns = 1.0f;
    }
}

} // namespace whiteout::flakes::renderer::particle
