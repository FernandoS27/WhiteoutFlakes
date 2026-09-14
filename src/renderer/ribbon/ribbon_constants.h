#pragma once

// ============================================================================
// The ribbon module's vocabulary: every named literal and every small enum the
// simulation reads, in one place.
//
// Two things used to be wrong with these values being inline. The first is the
// project's own rule — SC2_PARTICLE_DESIGN.md R7, "named bits, not hex; inline
// hex is a review failure" — which `sc2/sc2_element.h` already applies to the
// RUNTIME words no file enum covers. The second is subtler and is why this
// header exists rather than a pair of `constexpr`s at each use site: the same
// constant was being spelled twice under two names. `kSqFloor` in the emitter
// and `kSqDistGate` in the stages both cited `dword_103BB6B78`; `kDegToRad`
// was defined in both files. Two spellings of one measured constant is how the
// two drift apart after the next RE correction.
//
// The FILE bits are not here: WhiteoutLib's `m3::RibbonFlag`,
// `m3::RibbonAdditionalFlag` and `m3::RibbonType` name every one of them, and
// `Sc2RibbonDesc` reads them through those enums. What is here is the values
// the binary holds as constants, plus the channel orders that were positional
// array indices with no name at all.
// ============================================================================

#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::ribbon {

// -- measured constants (the binary's own dwords) -----------------------------

/// `dword_103C472B8`. The ONLY deg→rad in the ribbon pipeline: yaw and pitch
/// arrive in degrees, twist is radians end to end.
inline constexpr f32 kDegToRad = 0.017453292f;

/// `dword_103BB6B78`. UpdateHeadSegment's stationary floor, tested against a
/// SQUARED length, and re-used as the magnitude of the direction it substitutes
/// for a degenerate velocity. Also the re-emission gate's squared head-distance
/// threshold — one constant, two readers, as in the binary.
inline constexpr f32 kSqStationaryFloor = 1e-4f;

/// `dword_103BC8C10`. The headU delta below which techs 0/2/3 skip re-emission,
/// and the speed below which length-mode catch-up gives up.
inline constexpr f32 kHeadUGate = 1e-3f;

/// `dword_103C45910`.
inline constexpr f32 kMsPerSec = 1000.0f;

/// `dword_103AAD5F0`. The period a zero emission rate stands in for, so the
/// accumulator can never lap it.
inline constexpr f32 kFltMax = 3.4028235e38f;

/// `startBlend` (`dword_103AD52E0`) — UpdateEmit's "already emitting" return.
/// It is a RETURN VALUE, not a general one: `invMass` and a length-mode
/// `deathU` span are their own 1.0f and say so.
inline constexpr f32 kStartBlend = 1.0f;

/// UpdateEmit's other non-zero return: emission resumed with a live trail.
inline constexpr f32 kEmitReactivated = 3.0f;

/// `CRibbon+0x444` bit 1 — the "emitting" render flag UpdateEmit latches.
inline constexpr u32 kEmittingFlag = 0x2u;

/// CatchUpEmission's fixed pre-roll step and its cap, and the tick count a
/// length-mode ribbon falls back to when its launch speed is degenerate.
inline constexpr u32 kCatchUpTickMs = 33u;
inline constexpr f32 kCatchUpTickSeconds = 0.033f;
inline constexpr u32 kCatchUpCapMs = 2500u;
inline constexpr u32 kLengthModeFallbackTicks = 15000u;

/// The load-time ceiling on a mid-time: it is a divisor in the two-piece
/// interpolators, so 1.0 exactly would divide by zero in the second piece.
inline constexpr f32 kMidTimeCeil = 0.996f;

/// The mid-time a twist track falls back to when none was authored. Rotation is
/// the one channel the VS reads without the desc's clamp having run on it.
inline constexpr f32 kDefaultRotationMidTime = 0.5f;

/// The reciprocal the two-piece interpolators take beside the mid-time itself.
/// A zero mid-time would divide by zero in the second piece, so it answers 1 —
/// the same guard the desc's load-time clamp applies from the other end.
inline constexpr f32 InvMidTime(f32 mid) {
    return (mid > 0.0f) ? 1.0f / mid : 1.0f;
}

/// The engine's load-time drag floor.
inline constexpr f32 kDragFloor = 0.01f;

/// "This ribbon authored noise." The same test decides the SIM TECHNIQUE (noise
/// demotes to legacy) and whether BUILD displaces, so the two can never
/// disagree by construction.
inline constexpr f32 kNoiseThreshold = 0.001f;

/// The three fixed Z slices `FillVertices_Animated` samples the noise field at
/// (`0x3EA8F5C3` / `0x3F28F5C3`; the first is 0).
inline constexpr f32 kNoiseZ1 = 0.33f;
inline constexpr f32 kNoiseZ2 = 0.66f;

/// `InitInterpDeltas`' early-out: below this much emitter travel a WoW ribbon
/// lays no new edges. A DISTANCE in renderer units — unrelated to
/// @ref kNoiseThreshold and to @ref kHeadUGate, which share its digits.
inline constexpr f32 kWc3StationaryDistance = 0.001f;

// -- build-side constants -----------------------------------------------------

/// Samples along a spline's cubic Bezier, at `t = i / (kSplineSamples - 1)`.
inline constexpr i32 kSplineSamples = 32;

/// Ring vertices a tube section is clamped to. A star ring holds twice this,
/// one inner point and one outer point per authored edge.
inline constexpr i32 kRingEdgeMin = 3;
inline constexpr i32 kRingEdgeMax = 64;

/// Below this a strip segment has no measurable length, so the length-mode arc
/// walk cannot land a cut on it.
inline constexpr f32 kArcEpsilon = 1e-6f;

/// `Ribbon.fx:442` halves the sampled size at the vertex. The trail path is
/// halved once more by the head writer (`widthScale`); the spline path has no
/// head writer, so it applies both here — which is why the two differ.
inline constexpr f32 kSizeHalfScale = 0.5f;
inline constexpr f32 kSplineSizeScale = 0.25f;

inline constexpr f32 kTwoPi = 6.2831853071795864769f;

// -- channel orders -----------------------------------------------------------
// These were positional indices into `midTime[4]`, `waveAmp[5]` and
// `color3[3]` with no name anywhere. Plain unscoped enums so they index without
// a cast, in namespaces so they do not collide.

/// `midTime[]` / `midHold[]` order, as the `RIB_` record stores it.
namespace MidChannel {
enum : i32 { Size = 0, Color = 1, Alpha = 2, Rotation = 3, kCount = 4 };
} // namespace MidChannel

/// The five overlay-wave channels. yaw/pitch/speed/size ADD to their base at
/// the head; alpha rides back to the caller to be added to each colour stop.
namespace WaveChannel {
enum : i32 { Yaw = 0, Pitch = 1, Speed = 2, Size = 3, Alpha = 4, kCount = 5 };
} // namespace WaveChannel

/// The three `SRIB` overlay-wave channels.
namespace SplineWaveChannel {
enum : i32 { Yaw = 0, Pitch = 1, Velocity = 2, kCount = 3 };
} // namespace SplineWaveChannel

/// `color3[]` / `size3` / `rotation3` key order — the three stops every SC2
/// element interpolator takes.
namespace ColorStop {
enum : i32 { Start = 0, Mid = 1, End = 2, kCount = 3 };
} // namespace ColorStop

// -- simulation selectors -----------------------------------------------------

/// `Ribbon_SelectSimTechnique`'s result. The values ARE the binary's, so the O1
/// golden comparison stays a numeric identity — and so that `up` stays
/// column 1 for {0,1} and the re-emission gate can still say "> Mixed".
enum class SimTechnique : u8 {
    GpuOnly = 0,      ///< Pure procedural, camera-flattened frame.
    Spline = 1,       ///< CPU spline; no per-segment emission at all.
    MixedLength = 2,  ///< Procedural + the length cull.
    MixedTangent = 3, ///< Procedural + precomputed accurate tangents.
    Legacy = 4,       ///< Full CPU Euler: forces, collision, noise.
};

/// What ends a segment's life. `RIB_+0x1DC`.
enum class CullMethod : u8 {
    Time = 0,   ///< deathU = birthU + lifetime.
    Length = 1, ///< The strip is cut to `maxLength` of centreline.
};

/// The analytic techniques (0/2/3) leave motion to the closed form BUILD
/// applies; `Legacy` integrates per tick and stores the result on the element.
inline constexpr bool IntegratesOnCpu(SimTechnique t) {
    return t == SimTechnique::Legacy;
}

/// UpdateHeadSegment tests the 1e-4 stationary floor only on the procedural
/// techniques — a legacy or spline element keeps a degenerate launch velocity.
inline constexpr bool AppliesStationaryFloor(SimTechnique t) {
    return t == SimTechnique::GpuOnly || t == SimTechnique::MixedLength ||
           t == SimTechnique::MixedTangent;
}

/// The tangent source (`Ribbon.fx:409/446`): the analytic velocity for the pure
/// procedural techniques, the trail GEOMETRY for the accurate-tangent and
/// legacy ones (which retail precomputes on the CPU). `-instVel` degenerates to
/// {1,0,0} for a slow world trail, so a tube built from it collapses to a line.
inline constexpr bool TangentFromVelocity(SimTechnique t) {
    return t == SimTechnique::GpuOnly || t == SimTechnique::MixedLength;
}

/// `Ribbon.fx:457` — the non-flattened ("smooth") frame branch, which every
/// technique but the pure billboard takes.
inline constexpr bool UsesSmoothFrame(SimTechnique t) {
    return t == SimTechnique::MixedLength || t == SimTechnique::MixedTangent ||
           t == SimTechnique::Legacy;
}

} // namespace whiteout::flakes::renderer::ribbon
