#include "renderer/ribbon/ribbon_emitter.h"

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

// The LOD tables live in sc2/sc2_element.h: emission LOD is element LOD, and
// the particle dialect reads the same two rows.
using ::whiteout::flakes::renderer::sc2::kLodCut;
using ::whiteout::flakes::renderer::sc2::kLodReduce;
using ::whiteout::flakes::renderer::sc2::LodIndex;

namespace vs = ::whiteout::flakes::renderer::sc2::vs;

// The measured constants live in ribbon_constants.h — they were spelled twice,
// once here and once in the emitter, and two of the pairs cited the same
// binary dword under two different names.

} // namespace

vs::Mat3 YawPitchBasis(f32 yawDeg, f32 pitchDeg, bool swap) {
    // Math_MatrixFromYawPitchRoll (0x100d56240) with roll = 0. Row-major, so
    // `v·M` (vs::MulVecMat3) matches the engine; row 2 = (sin yaw, −sin pitch·
    // cos yaw, cos pitch·cos yaw) is the emission direction. Without the swap
    // a2 = pitch and a3 = yaw.
    const f32 a2 = (swap ? yawDeg : pitchDeg) * kDegToRad;
    const f32 a3 = (swap ? pitchDeg : yawDeg) * kDegToRad;
    const f32 s2 = std::sin(a2), c2 = std::cos(a2);
    const f32 s3 = std::sin(a3), c3 = std::cos(a3);
    vs::Mat3 m{};
    m.m[0][0] = c3;   m.m[0][1] = s3 * s2;   m.m[0][2] = -s3 * c2;
    m.m[1][0] = 0.0f; m.m[1][1] = c2;        m.m[1][2] = s2;
    m.m[2][0] = s3;   m.m[2][1] = -s2 * c3;  m.m[2][2] = c3 * c2;
    return m;
}

EmitGateResult EmitGate(EmitClock& clk, const EmitGateInputs& in) {
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
        const bool gateOnDistance = (in.simTechnique == SimTechnique::Legacy ||
                                     in.simTechnique == SimTechnique::Spline);
        if (gateOnDistance) {
            const f32 dx = in.headElemPos.x - in.headPos.x;
            const f32 dy = in.headElemPos.y - in.headPos.y;
            const f32 dz = in.headElemPos.z - in.headPos.z;
            const f32 sq = dz * dz + (dy * dy + dx * dx);
            if (sq < kSqStationaryFloor)
                return r;
        } else {
            if ((in.headU - in.headBirthU) < kHeadUGate)
                return r;
        }
    }

    // Reached the emit body: accumulate dt unconditionally from here.
    clk.dtAccumulator = in.dt + clk.dtAccumulator;

    // LOD cut suppresses emission but keeps the accumulated dt.
    if (kLodCut[LodIndex(in.lodCut, in.quality)])
        return r;

    const f32 reduce = kLodReduce[LodIndex(in.lodReduce, in.quality)];
    const f32 rate = (in.emissionScale * reduce) * in.divisions;
    const f32 aux =
        (in.cullMethod == CullMethod::Length) ? in.maxLengthAux : in.lifetimeAux;
    const f32 period = (rate == 0.0f) ? kFltMax : aux / rate;
    if (clk.dtAccumulator < period)
        return r; // no lapse; keep the accumulated dt.

    clk.dtAccumulator = clk.dtAccumulator - period;
    r.sampled = true;
    r.activeState = in.sampledActive ? 1u : 0u;
    if (!in.sampledActive || !in.nodeActive) {
        clk.renderFlags &= static_cast<u16>(~kEmittingFlag);
        return r;
    }
    u32 combined = clk.renderFlags | (static_cast<u32>(clk.renderFlagsHi) << 16);
    if (combined & kEmittingFlag) {
        r.ret = kStartBlend; // already emitting
        return r;
    }
    combined |= kEmittingFlag;
    clk.renderFlagsHi = static_cast<u8>(combined >> 16);
    clk.renderFlags = static_cast<u16>(combined);
    r.ret = (in.elementCount == 0) ? kStartBlend : kEmitReactivated;
    return r;
}

HeadElement WriteHead(const HeadInputs& in) {
    HeadElement e;

    // Overlay waves (W6): each channel whose static type is nonzero adds
    // SampleWave(type, freq*overlayTime + overlayPhase, amp). yaw/pitch/speed
    // feed the emission basis; size adds to the sampled size; alpha rides back
    // to the caller. All inert when the type is 0, so the W3 oracle subset (all
    // types 0) replays byte-identical.
    const auto wave = [&](i32 i) -> f32 {
        return in.waveTypes[i] ? SampleWave(in.waveTypes[i],
                                               in.waveFreq[i] * in.overlayTime + in.overlayPhase,
                                               in.waveAmp[i])
                               : 0.0f;
    };
    const f32 yawDeg = in.yawDeg + wave(WaveChannel::Yaw);
    const f32 pitchDeg = in.pitchDeg + wave(WaveChannel::Pitch);
    const f32 speed = in.speed + wave(WaveChannel::Speed);
    const f32 sizeWave = wave(WaveChannel::Size);
    e.alphaWave = wave(WaveChannel::Alpha);

    // Emission basis: row 2 of the yaw/pitch matrix, which the spline path
    // takes whole — one definition, in YawPitchBasis above.
    const vs::Mat3 basis = YawPitchBasis(yawDeg, pitchDeg, in.swapYawPitch);
    const Vector3f dir = {basis.m[2][0], basis.m[2][1], basis.m[2][2]};

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
    const bool upIsColumn1 = (in.simTechnique == SimTechnique::GpuOnly ||
                              in.simTechnique == SimTechnique::Spline);
    e.up = upIsColumn1 ? Vector3f{0, 1, 0} : Vector3f{-1, 0, 0};

    // Half-width scale (widthScale in local billboard space); the size wave
    // adds to the sampled size before the halving, as the head writer does.
    e.size3 = {(in.size3.x + sizeWave) * kSizeHalfScale,
               (in.size3.y + sizeWave) * kSizeHalfScale,
               (in.size3.z + sizeWave) * kSizeHalfScale};
    // A billboard carries no twist; planar/tube keep it.
    e.rotation3 = (in.ribbonType != m3::RibbonType::Billboard) ? in.rotation3
                                                               : Vector3f{0, 0, 0};

    const bool lengthMode = (in.cullMethod == CullMethod::Length);
    e.invMass = 1.0f / in.mass;
    e.birthU = in.headU;
    // A length-mode segment has no time death: its span is a unit, and the cut
    // is geometric (the arc walk in BUILD).
    e.deathU = in.headU + (lengthMode ? 1.0f : in.lifetime);

    u32 lifeTicks;
    if (lengthMode) {
        const f32 vlen = std::sqrt((e.velocity.x * e.velocity.x +
                                    e.velocity.y * e.velocity.y) +
                                   e.velocity.z * e.velocity.z);
        lifeTicks = (vlen >= kHeadUGate)
                        ? static_cast<u32>(in.maxLengthBound / vlen * kMsPerSec)
                        : kLengthModeFallbackTicks;
    } else {
        lifeTicks = static_cast<u32>((e.deathU - e.birthU) * kMsPerSec);
    }
    u32 expire = in.nowMs + lifeTicks;
    if (in.prevExpireMs >= expire)
        expire = in.prevExpireMs;
    e.expireFrameMs = expire;
    return e;
}

CatchUpResult CatchUpTicks(CullMethod cullMethod, f32 speed, f32 lifetime,
                              f32 maxLengthBound) {
    CatchUpResult r;
    f32 rate;
    if (cullMethod == CullMethod::Length) {
        if (speed < kHeadUGate) { // too slow: no pre-roll at all
            r.earlyOut = true;
            return r;
        }
        rate = maxLengthBound / speed;
    } else {
        rate = lifetime;
    }
    u32 catchupMs = kCatchUpCapMs;
    const i32 v = static_cast<i32>(rate * kMsPerSec);
    if (static_cast<u32>(v) <= kCatchUpCapMs)
        catchupMs = static_cast<u32>(v);
    // The engine's quality field is not modelled (0), so the incoming emitCap
    // never binds below catchupMs — the tick count is ceil(catchupMs / tick).
    u32 elapsed = 0;
    while (elapsed < catchupMs) {
        ++r.ticks;
        elapsed += kCatchUpTickMs;
    }
    return r;
}

} // namespace whiteout::flakes::renderer::ribbon::sc2
