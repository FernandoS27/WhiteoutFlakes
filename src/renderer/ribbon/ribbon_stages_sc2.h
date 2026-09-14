#pragma once

// ============================================================================
// The SC2 CPU stage kernels: pure functions over explicit input structs, each
// measured against its own Unicorn golden (tools/sc2_ribbon_oracle).
//
// Kernels are pure and loops are members (SC2_PARTICLE_DESIGN.md R3): the
// emitter's stage methods and the oracle replay call the SAME symbol, so a
// divergence is a red gate rather than a silent drift. What nothing here
// measures is the ORDER these run in — that is the tick's job, and it is
// tested structurally.
//
// Inside this namespace the family token is dropped: `sc2::WriteHead`, not
// `sc2::WriteHead`, matching `sc2::SampleWave` and every result type.
// ============================================================================

#include "renderer/ribbon/ribbon_constants.h"
#include "renderer/sc2/sc2_element.h"
#include "renderer/sc2/sc2_element_math.h"
#include "types.h"
#include "whiteout/flakes/types.h"
#include "whiteout/models/m3/structures/base.h"

namespace whiteout::flakes::renderer::ribbon {

namespace m3 = ::whiteout::m3;


// ---- SC2 time-mode stage kernels (RIBBON_SERVICE_PLAN.md W3) ----------------
// Pure reproductions of the shipped CPU stages, replayed at their stated
// tolerances against the Unicorn goldens: the emit clock (O3 `UpdateEmit`), the
// head-element writer (O4 `UpdateHeadSegment`, emitter-LOCAL subset), and the
// pre-roll clock (O5 `CatchUpEmission`). The emitter's stage methods and the
// oracle-replay test call this SAME code, so a divergence shows up as a red
// gate rather than a silent drift.
namespace sc2 {

/// UpdateEmit's mutable clock state (the CRibbon fields it carries frame to
/// frame). renderFlags bit 1 is the "emitting" flag.
struct EmitClock {
    f32 dtAccumulator = 0; ///< CRibbon+0x3F0.
    u16 renderFlags = 0;   ///< CRibbon+0x444.
    u8 renderFlagsHi = 0;  ///< CRibbon+0x446.
};

/// Everything UpdateEmit reads that is not in EmitClock.
struct EmitGateInputs {
    bool splinePresent = false;
    bool active = false;      ///< CRibbon activeFlag.
    bool worldReemit = false; ///< RIB_ additionalFlags & 8 (re-emission gates).
    SimTechnique simTechnique = SimTechnique::GpuOnly;
    bool haveHead = false;           ///< a live head element exists to gate on.
    f32 headU = 0, headBirthU = 0;   ///< head element birthU (byte 112).
    Vector3f headElemPos = {0, 0, 0};///< head element pos (byte 100).
    Vector3f headPos = {0, 0, 0};    ///< CRibbon headPos.
    i32 quality = 0, lodCut = 0, lodReduce = 0;
    CullMethod cullMethod = CullMethod::Time;
    f32 emissionScale = 1.0f, divisions = 1.0f;
    f32 lifetimeAux = 0, maxLengthAux = 0;
    f32 dt = 0;
    bool sampledActive = true; ///< what the RIB_.active Bool32 sample reports.
    bool nodeActive = true;    ///< transform-node byte & 2.
    u32 elementCount = 0;
};

struct EmitGateResult {
    f32 ret = 0;          ///< 0 no-emit / 1 startBlend / 3 re-activation.
    bool sampled = false; ///< the active sample ran (a period lapsed).
    u32 activeState = 0;
};

/// One UpdateEmit call; mutates `clk`.
///
/// NOT ON THE PRODUCTION PATH. The emitter runs the design-driven headU clock
/// in `AppendSc2` instead (RIBBON_SERVICE.md 5.1); this kernel is the O3 replay
/// of what retail does, kept because it is the measured article and because
/// wiring it is a behaviour change with its own gate, not a cleanup
/// (RIBBON_REFACTOR_PLAN.md C7b). Until that decision is taken, the two clocks
/// are a known divergence rather than an accident.
EmitGateResult EmitGate(EmitClock& clk, const EmitGateInputs& in);

/// The head element's launch state in emitter-LOCAL space (identity basis),
/// as UpdateHeadSegment writes it for the W3 subset (local, no inherit, no
/// overlay waves). The world transform is applied later by BUILD.
struct HeadElement {
    Vector3f velocity = {0, 0, 0}; ///< element +16 == +184 in this subset.
    /// The emission direction (YPR row2), local space and pre-speed. The caller
    /// needs it because the 1e-4 stationary floor runs AFTER the world transform
    /// and inherit add, replacing velocity with the (transformed) direction·1e-4
    /// (UpdateHeadSegment step 5, DRIFT-1) — it cannot be applied here.
    Vector3f dir = {0, 0, 1};
    Vector3f up = {0, 0, 1};
    Vector3f size3 = {0, 0, 0};
    Vector3f rotation3 = {0, 0, 0};
    f32 invMass = 1.0f;
    f32 birthU = 0, deathU = 0;
    /// The frame the element expires on. Part of the O4 golden's field set and
    /// written for that gate; production reads none of it, and never sets the
    /// `nowMs`/`prevExpireMs` it is derived from.
    u32 expireFrameMs = 0;
    /// The alpha overlay wave (0 when no alpha wave); the caller adds it to each
    /// colour stop's alpha and clamps [0,1]. Colours are not the head kernel's
    /// to own, so it hands this back rather than mutating them.
    f32 alphaWave = 0;
};

struct HeadInputs {
    SimTechnique simTechnique = SimTechnique::GpuOnly;
    m3::RibbonType ribbonType = m3::RibbonType::Billboard;
    CullMethod cullMethod = CullMethod::Time;
    bool swapYawPitch = false; ///< Sc2RibbonDesc::SwapsYawPitch (UseLocator).
    f32 headU = 0;
    f32 yawDeg = 0, pitchDeg = 0, speed = 0, lifetime = 1.0f;
    Vector3f size3 = {1, 1, 1};
    Vector3f rotation3 = {0, 0, 0};
    f32 mass = 1.0f, maxLengthBound = 0;
    u32 nowMs = 0, prevExpireMs = 0;
    /// Overlay waves (W6). `waveTypes` gate per channel (0 = inert, so the W3
    /// oracle subset that leaves them 0 is byte-identical); `overlayTime` is the
    /// emission clock the phase runs on. yaw/pitch/speed/size ADD to their base;
    /// the alpha wave comes back on the HeadElement. Indexed by @ref WaveChannel.
    u32 waveTypes[WaveChannel::kCount] = {0, 0, 0, 0, 0};
    f32 waveAmp[WaveChannel::kCount] = {0, 0, 0, 0, 0};
    f32 waveFreq[WaveChannel::kCount] = {0, 0, 0, 0, 0};
    f32 overlayPhase = 0, overlayTime = 0;
};

HeadElement WriteHead(const HeadInputs& in);

/// The emission basis `Math_MatrixFromYawPitchRoll(a2, a3, 0)` builds, as a
/// row-major 3×3 so `v·M` (vs::MulVecMat3) matches the engine. Row 2 is the
/// emission DIRECTION, which is all the head writer takes; the spline path
/// rotates its tangents by the whole matrix. `swap` exchanges the two operands
/// (@ref RibbonDesc::Sc2::SwapsYawPitch).
///
/// One definition: it used to exist twice — here for the spline and inline in
/// `WriteHead` for the head — with a comment in one asking the reader to
/// keep them in sync by hand.
::whiteout::flakes::renderer::sc2::vs::Mat3 YawPitchBasis(f32 yawDeg, f32 pitchDeg,
                                                          bool swap);

/// The overlay-wave sampler, moved to `sc2/sc2_element.h` — it samples an
/// ELEMENT's wave and a `PAR_` needs it as much as a `RIB_` does (R6). Named
/// here so ribbon code keeps spelling it `sc2::SampleWave`; there is one
/// definition.
using ::whiteout::flakes::renderer::sc2::SampleWave;

/// CatchUpEmission's pre-roll duration expressed as 33 ms tick count. `speed`
/// and `lifetime` are the ALREADY-SAMPLED reduced values (min speed / max
/// lifetime) — the track reduction stays out of here per the emulation
/// contract. `earlyOut` is the length-mode too-slow skip (speed < 1e-3).
struct CatchUpResult {
    i32 ticks = 0;
    bool earlyOut = false;
};
CatchUpResult CatchUpTicks(CullMethod cullMethod, f32 speed, f32 lifetime,
                              f32 maxLengthBound);

/// @brief The animated vertex fillers' noise offset (SC2_RIBBON_RE §4.3, gate
///        O13) at trail parameter @p t.
///
/// The particle system's seed-0 table sampled in 3-D at `(t·frequency,
/// coherence·headU, {0, kNoiseZ1, kNoiseZ2})`, times an amplitude muted by
/// `t/edge` below the edge — or, only on a spline, by `(1−t)/edge` above
/// `1 − edge`. The two mutes are alternatives, never a product, and an edge of
/// 0 mutes nothing. Returned in the ribbon's own space.
Vector3f NoiseDisplacement(f32 t, f32 headU, f32 amplitude, f32 frequency,
                              f32 coherence, f32 edge, bool spline);

} // namespace sc2
} // namespace whiteout::flakes::renderer::ribbon
