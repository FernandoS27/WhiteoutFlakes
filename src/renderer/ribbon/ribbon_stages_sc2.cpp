#include "renderer/ribbon/ribbon_emitter.h"

#include <bit>
#include <cmath>

// ============================================================================
// StarCraft II time-mode CPU stage kernels (RIBBON_SERVICE_PLAN.md W3).
//
// Each function is a line-for-line reproduction of one shipped routine, gated
// against its Unicorn golden (tools/sc2_ribbon_oracle): the float operation
// ORDER is part of the contract, so the arithmetic below is written in the
// binary's order, not the algebraically-tidy one. Only the sin/cos-derived
// lanes carry a ULP bound (the basis runs through __sincosf_stret); everything
// else replays bit-exact.
// ============================================================================

namespace whiteout::flakes::renderer::ribbon::sc2 {

namespace {

// LOD tables, indexed [5*row + quality] (O3 golden o3_lodtables). The reduce
// row scales the emission rate; a nonzero cut byte suppresses emission whole.
constexpr f32 kLodReduce[20] = {
    1.0f, 1.0f, 1.0f, 1.0f, 1.0f,       // row 0
    0.75f, 1.0f, 1.0f, 1.0f, 1.0f,      // row 1
    0.5f, 0.75f, 1.0f, 1.0f, 1.0f,      // row 2
    0.25f, 0.5f, 0.75f, 1.0f, 1.0f,     // row 3
};
constexpr u8 kLodCut[20] = {
    0, 0, 0, 0, 0,
    1, 0, 0, 0, 0,
    1, 1, 0, 0, 0,
    1, 1, 1, 0, 0,
};

constexpr f32 kFltMax = 3.4028235e38f; // dword_103AAD5F0
constexpr f32 kStartBlend = 1.0f;      // startBlend (dword_103AD52E0)
constexpr f32 kSqDistGate = 1e-4f;     // dword_103BB6B78
constexpr f32 kHeadUGate = 1e-3f;      // dword_103BC8C10
constexpr f32 kDegToRad = 0.017453292f;// dword_103C472B8
constexpr f32 kMsPerSec = 1000.0f;     // dword_103C45910

i32 lodIndex(i32 row, i32 quality) {
    return 5 * row + quality;
}

} // namespace

f32 Sc2SampleWave(u32 type, f32 phase, f32 amp) {
    switch (type) {
    case 1:
        return std::sin(phase) * amp;
    case 2:
        return std::cos(phase) * amp;
    case 4: {
        // Square: ±amp about the half-period (M3_SampleAnimValue case 4).
        const f32 frac = phase - std::floor(phase);
        return (frac > 0.5f) ? -amp : amp;
    }
    case 3:
        // Sawtooth (M3_SampleAnimValue case 3, recovered from a clean disasm):
        // amp·(2·fmod(phase, 1) − 1) — a bipolar ramp per unit period. Retail
        // runs the fmod in double then narrows; K=1.0, C=−1.0 (both doubles).
        return amp * (2.0f * std::fmod(phase, 1.0f) - 1.0f);
    case 5: {
        // Retail is a nondeterministic global RNG; the fixed-timeline
        // determinism the plan demands rules that out, so this is a hash of the
        // phase bits into [−amp, amp] — a stated deviation (curve-shape only).
        u32 h = std::bit_cast<u32>(phase);
        h ^= h >> 16;
        h *= 0x7feb352du;
        h ^= h >> 15;
        h *= 0x846ca68bu;
        h ^= h >> 16;
        const f32 unit = static_cast<f32>(h >> 8) * (1.0f / 16777216.0f); // [0,1)
        return (unit * 2.0f - 1.0f) * amp;
    }
    default: // 0 off; 6 table lookup (unk_108254834) unextracted — no overlay.
        return 0.0f;
    }
}

EmitGateResult Sc2EmitGate(EmitClock& clk, const EmitGateInputs& in) {
    EmitGateResult r;

    // Spline emitters bank dt and never gate here.
    if (in.splinePresent) {
        clk.dtAccumulator = in.dt + clk.dtAccumulator;
        return r;
    }
    if (!in.active)
        return r; // activeFlag clear: return WITHOUT accumulating.

    // Re-emission gate: a live head plus the world-space flag. tech {1, >3}
    // gate on head movement (squared distance), tech {0,2,3} on the headU
    // delta. Both early-out WITHOUT accumulating.
    if (in.haveHead && in.worldReemit) {
        if (in.simTechnique > 3 || in.simTechnique == 1) {
            const f32 dx = in.headElemPos.x - in.headPos.x;
            const f32 dy = in.headElemPos.y - in.headPos.y;
            const f32 dz = in.headElemPos.z - in.headPos.z;
            const f32 sq = dz * dz + (dy * dy + dx * dx);
            if (sq < kSqDistGate)
                return r;
        } else {
            if ((in.headU - in.headBirthU) < kHeadUGate)
                return r;
        }
    }

    // Reached the emit body: accumulate dt unconditionally from here.
    clk.dtAccumulator = in.dt + clk.dtAccumulator;

    // LOD cut suppresses emission but keeps the accumulated dt.
    if (kLodCut[lodIndex(in.lodCut, in.quality)])
        return r;

    const f32 reduce = kLodReduce[lodIndex(in.lodReduce, in.quality)];
    const f32 rate = (in.emissionScale * reduce) * in.divisions;
    const f32 aux = (in.cullMethod == 1) ? in.maxLengthAux : in.lifetimeAux;
    const f32 period = (rate == 0.0f) ? kFltMax : aux / rate;
    if (clk.dtAccumulator < period)
        return r; // no lapse; keep the accumulated dt.

    clk.dtAccumulator = clk.dtAccumulator - period;
    r.sampled = true;
    r.activeState = in.sampledActive ? 1u : 0u;
    if (!in.sampledActive || !in.nodeActive) {
        clk.renderFlags &= static_cast<u16>(~0x2u);
        return r;
    }
    u32 combined = clk.renderFlags | (static_cast<u32>(clk.renderFlagsHi) << 16);
    if (combined & 0x2u) {
        r.ret = kStartBlend; // already emitting
        return r;
    }
    combined |= 0x2u;
    clk.renderFlagsHi = static_cast<u8>(combined >> 16);
    clk.renderFlags = static_cast<u16>(combined);
    r.ret = (in.elementCount == 0) ? kStartBlend : 3.0f;
    return r;
}

HeadElement Sc2WriteHead(const HeadInputs& in) {
    HeadElement e;

    // Overlay waves (W6): each channel whose static type is nonzero adds
    // SampleWave(type, freq*overlayTime + overlayPhase, amp). yaw/pitch/speed
    // feed the emission basis; size adds to the sampled size; alpha rides back
    // to the caller. All inert when the type is 0, so the W3 oracle subset (all
    // types 0) replays byte-identical.
    const auto wave = [&](int i) -> f32 {
        return in.waveTypes[i] ? Sc2SampleWave(in.waveTypes[i],
                                               in.waveFreq[i] * in.overlayTime + in.overlayPhase,
                                               in.waveAmp[i])
                               : 0.0f;
    };
    const f32 yawDeg = in.yawDeg + wave(0);
    const f32 pitchDeg = in.pitchDeg + wave(1);
    const f32 speed = in.speed + wave(2);
    const f32 sizeWave = wave(3);
    e.alphaWave = wave(4);

    // Emission basis: Math_MatrixFromYawPitchRoll(a2, a3, 0) row 2. Without the
    // 0x8000 swap a2 = pitch, a3 = yaw; the swap exchanges them. row2 =
    // (sin a3, -sin a2 * cos a3, cos a2 * cos a3).
    const f32 a2 = (in.swapYawPitch ? yawDeg : pitchDeg) * kDegToRad;
    const f32 a3 = (in.swapYawPitch ? pitchDeg : yawDeg) * kDegToRad;
    const f32 s2 = std::sin(a2), c2 = std::cos(a2);
    const f32 s3 = std::sin(a3), c3 = std::cos(a3);
    const Vector3f dir = {s3, -s2 * c3, c2 * c3};

    // Launch velocity = direction·speed. Inherit (flags & 0x10) AND the 1e-4
    // stationary floor (techs 0/2/3) are the caller's, applied after the world
    // transform: the binary folds inherit into velocity, tests |velocity|² on
    // the post-inherit vector, and replaces a degenerate one with the
    // world-transformed direction·1e-4 (UpdateHeadSegment steps 4→5). Doing the
    // floor here in local, pre-inherit space was DRIFT-1; hand `dir` back so the
    // caller can floor in the element's true space.
    e.dir = dir;
    e.velocity = {dir.x * speed, dir.y * speed, dir.z * speed};

    // Up: matrix column 1 for techs 0/1 (local identity -> +Y); techs 2..4
    // overwrite with normalized -column0 (local identity -> -X).
    e.up = (in.simTechnique == 0 || in.simTechnique == 1) ? Vector3f{0, 1, 0}
                                                          : Vector3f{-1, 0, 0};

    // Half-width scale (widthScale 0.5 in local billboard space); the size wave
    // adds to the sampled size before the halving, as the head writer does.
    e.size3 = {(in.size3.x + sizeWave) * 0.5f, (in.size3.y + sizeWave) * 0.5f,
               (in.size3.z + sizeWave) * 0.5f};
    // Billboard (ribbonType 0) carries no twist; planar/tube keep it.
    e.rotation3 = (in.ribbonType != 0) ? in.rotation3 : Vector3f{0, 0, 0};

    e.invMass = kStartBlend / in.mass;
    e.birthU = in.headU;
    e.deathU = in.headU + (in.cullMethod == 1 ? kStartBlend : in.lifetime);

    u32 lifeTicks;
    if (in.cullMethod == 1) {
        const f32 vlen = std::sqrt((e.velocity.x * e.velocity.x +
                                    e.velocity.y * e.velocity.y) +
                                   e.velocity.z * e.velocity.z);
        lifeTicks = (vlen >= kHeadUGate)
                        ? static_cast<u32>(in.maxLengthBound / vlen * kMsPerSec)
                        : 15000u;
    } else {
        lifeTicks = static_cast<u32>((e.deathU - e.birthU) * kMsPerSec);
    }
    u32 expire = in.nowMs + lifeTicks;
    if (in.prevExpireMs >= expire)
        expire = in.prevExpireMs;
    e.expireFrameMs = expire;
    return e;
}

CatchUpResult Sc2CatchUpTicks(u8 cullMethod, f32 speed, f32 lifetime,
                              f32 maxLengthBound) {
    CatchUpResult r;
    f32 rate;
    if (cullMethod == 1) {
        if (speed < kHeadUGate) { // too slow: no pre-roll at all
            r.earlyOut = true;
            return r;
        }
        rate = maxLengthBound / speed;
    } else {
        rate = lifetime;
    }
    u32 catchupMs = 2500;
    const i32 v = static_cast<i32>(rate * kMsPerSec);
    if (static_cast<u32>(v) <= 2500u)
        catchupMs = static_cast<u32>(v);
    // The engine's quality field is not modelled (0), so the incoming emitCap
    // never binds below catchupMs — the tick count is ceil(catchupMs / 33).
    u32 elapsed = 0;
    while (elapsed < catchupMs) {
        ++r.ticks;
        elapsed += 33;
    }
    return r;
}

} // namespace whiteout::flakes::renderer::ribbon::sc2
