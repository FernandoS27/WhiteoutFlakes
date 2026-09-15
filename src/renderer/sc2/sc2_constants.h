#pragma once

// ============================================================================
// The StarCraft II element vocabulary: every constant the shipped binary holds
// that more than one SC2 consumer reads, named once.
//
// Two rules shape this file (SC2_PARTICLE_DESIGN.md R6 and R7):
//
//  * A measured dword is recorded ONCE, as `kDword<address>`, and every reader
//    names it by the ROLE it plays at its site. Equal digits in two roles stay
//    two names over one definition — `kNoiseThreshold` and `kModelScaleFloor`
//    are the same float and say different things, and an RE correction to one
//    reading must be able to land without silently moving the other.
//  * Where two sites spell what should be one dword with different BITS, both
//    keep their value under a site-specific name marked `DISAGREES`. This file
//    unifies spellings, never values; PARTICLE_REFACTOR_PLAN.md §2.7 routes
//    each disagreement to its own decision.
//
// `particle/` and `ribbon/` both include this and never each other.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <array>

namespace whiteout::flakes::renderer::sc2 {

// -- the dwords ----------------------------------------------------------------

/// `0x103BC8C10`.
inline constexpr f32 kDword103BC8C10 = 0.001f;
/// `0x103C458F8`.
inline constexpr f32 kDword103C458F8 = 0.01f;
/// `0x103BB6B74`.
inline constexpr f32 kDword103BB6B74 = 1.0e-5f;
/// `0x103BB6B78`.
inline constexpr f32 kDword103BB6B78 = 1.0e-4f;
/// `0x103C2BC90`.
inline constexpr f32 kDword103C2BC90 = 0.033f;
/// `0x103C2BC9C`.
inline constexpr f32 kDword103C2BC9C = 0.9959f;
/// `0x103AAD5F4`.
inline constexpr f32 kDword103AAD5F4 = 3.0f;
/// `0x103C2B674`.
inline constexpr f32 kDword103C2B674 = 2.0f / 255.0f;
/// `0x103C472B8`.
inline constexpr f32 kDword103C472B8 = 0.017453292f;
/// `0x103C45910`.
inline constexpr f32 kDword103C45910 = 1000.0f;
/// `0x103AAD5F0`.
inline constexpr f32 kDword103AAD5F0 = 3.4028235e38f;

// -- by role -------------------------------------------------------------------

/// An authored noise amplitude above this arms noise; `CanUseGpuMotion` demotes
/// at or above it. The same test decides the ribbon's sim technique.
inline constexpr f32 kNoiseThreshold = kDword103BC8C10;
/// `CanUseGpuMotion`'s `field < gate` for wind, kill radius and noise.
inline constexpr f32 kGpuMotionGate = kDword103BC8C10;
/// The smallest uniform scale a model particle's size curve can hand the model.
inline constexpr f32 kModelScaleFloor = kDword103BC8C10;
/// A SQUARED length below which a model particle's plain orient direction
/// builds no frame (the reading is not a scale, the dword is the same).
inline constexpr f32 kPlainOrientMinSq = kDword103BC8C10;
/// The ribbon's headU delta below which techs 0/2/3 skip re-emission.
inline constexpr f32 kHeadUGate = kDword103BC8C10;

/// `|v + wind|²` must clear this for friction to act on a contact.
inline constexpr f32 kFrictionSpeedSq = kDword103C458F8;
/// The ribbon ground response's friction floor, the same dword.
inline constexpr f32 kCollideSpeedSq = kDword103C458F8;
/// A terrain-projected direction shorter (squared) than this falls back to +X.
inline constexpr f32 kTerrainProjectMinSq = kDword103C458F8;

/// The squared-speed floor a type-6 instance must clear at birth.
inline constexpr f32 kStillSpeedSq = kDword103BB6B74;
/// At or below this determinant the emitter matrix counts as singular.
inline constexpr f32 kMinInvertibleDet = kDword103BB6B74;

/// The `1e-4` on y a facing basis (types 2 and 3) adds before normalising.
inline constexpr f32 kFacingYGuard = kDword103BB6B78;
/// The ribbon head's stationary floor, tested against a SQUARED length.
inline constexpr f32 kSqStationaryFloor = kDword103BB6B78;

/// Added to `timeOffset` before every pre-roll block.
inline constexpr f32 kPreRollOffsetStep = kDword103C2BC90;
/// The same 33 ms as seconds, the ribbon's catch-up step.
inline constexpr f32 kCatchUpTickSeconds = kDword103C2BC90;
/// The block both pre-rolls drive `Tick` with, in integer milliseconds.
inline constexpr i32 kPreRollBlockMs = 33;

/// The `>` comparand every random mid-time collapse tests.
inline constexpr f32 kMidTimeCollapse = kDword103C2BC9C;

/// A landed particle rests below `|v|² < 3·dt`.
inline constexpr f32 kRestPerDt = kDword103AAD5F4;

/// A packed direction byte back to [-1, 1]: `code · (2/255) − 1`.
inline constexpr f32 kCodeScale = kDword103C2B674;

/// Degrees to radians, the one conversion both dialects' yaw and pitch take.
inline constexpr f32 kDegToRad = kDword103C472B8;

/// Milliseconds per second, as the ribbon multiplies.
inline constexpr f32 kMsPerSec = kDword103C45910;
/// Seconds per millisecond, as `Tick` MULTIPLIES — 0.001 is inexact, so this is
/// not `x / kMsPerSec` and must not become it.
inline constexpr f32 kSecondsPerMs = 0.001f;

/// `FLT_MAX`: the ribbon's stand-in period for a zero rate, and the empty
/// sentinel a particle bounds pass starts from.
inline constexpr f32 kFltMax = kDword103AAD5F0;

/// The global quality level the LOD tables index; 4 at viewer settings.
inline constexpr i32 kViewerQuality = 4;

/// The image's pi and two pi, as the shipped code multiplies by them.
inline constexpr f32 kPi = 3.14159274f;
inline constexpr f32 kTwoPi = 6.2831855f;

/// The load-time drag floor, and the reciprocal written below it.
inline constexpr f32 kDragFloor = 0.01f;
inline constexpr f32 kInvDragUnderFloor = 100.0f;

/// The load-time ceiling on an authored mid time: it is a divisor in the
/// two-piece interpolators (`InitCopy`, 4.8 and 5.0).
inline constexpr f32 kMidTimeCeil = 0.996f;

/// `1/255` as both colour tables hold it, `0x3B808081`. NOT the element
/// kernels' `kByteToUnit` (`0x3B80806E`): two engine constants, both correct.
inline constexpr f32 kInv255 = 1.0f / 255.0f;

// -- the ground response ---------------------------------------------------------

/// `0x103C2C9D0`, the RIBBON's swept radius against the map colliders.
/// DISAGREES — F5: the particle step's push-out cites `0x103C2A004` (0.05) under
/// the same role name; see `kParticleCollidePushOut`.
inline constexpr f32 kCollideRadius = 0.03f;
/// `0x103C2A004`, the particle step's swept radius and terrain push-out.
/// DISAGREES — F5 with `kCollideRadius`.
inline constexpr f32 kParticleCollidePushOut = 0.05f;

// -- the spline and the pole ------------------------------------------------------

/// `0x103C2BC94` as the SPAWN spline's vertical test reads it (bits
/// `0x3F7FBE76`; the oracle tooling agrees).
/// DISAGREES — F4: the model-particle pose spells the same dword `0x3F7FBE77`.
inline constexpr f32 kSplineVerticalCos = 0.99899995f;
/// `0x103C2BC94` as the model-particle pose's pole test reads it.
/// DISAGREES — F4 with `kSplineVerticalCos`, one ULP apart.
inline constexpr f32 kModelPoleCos = 0.999f;

// -- quantisation ---------------------------------------------------------------

/// Half extents ×256 and radians ×32, both truncated into the element's u16s.
inline constexpr f32 kSizeQuant = 256.0f;
inline constexpr f32 kRotationQuant = 32.0f;
inline constexpr f32 kInvSizeQuant = 1.0f / 256.0f;
inline constexpr f32 kInvRotationQuant = 1.0f / 32.0f;
/// `(v + 1) · 127.5`, the instanceType-7 basis byte packing.
inline constexpr f32 kOrientQuant = 127.5f;
/// Two u16 halves packed into one float, `lo + 65536·hi`.
inline constexpr f32 kPackHalf = 65536.0f;

// -- matrices -----------------------------------------------------------------------

/// The identity as the sixteen row-major floats every SC2 kernel matrix is.
inline constexpr std::array<f32, 16> kIdentityMat16{1, 0, 0, 0, 0, 1, 0, 0,
                                                    0, 0, 1, 0, 0, 0, 0, 1};

// -- pools and caps ----------------------------------------------------------------

/// The vertex arena over the 464-byte four-vertex stride: retail's ceiling on
/// one emitter's particles, `min(authored, 0x200000 / 464)`.
inline constexpr u32 kVertexArenaBytes = 0x200000u;
inline constexpr u32 kElementStrideBytes = 464u;
inline constexpr u32 kMaxParticles = kVertexArenaBytes / kElementStrideBytes;

// -- runtime bit names (RE §3) -----------------------------------------------
// These three words are RUNTIME state, not file fields: WhiteoutLib's
// `ParticleFlag` / `ParticleRotationFlag` enums cover the `PAR_` bits, and
// nothing covers these, which is why they were inline hex at every use site.
// Named once here (R7); the RE section is the authority for each meaning.

/// `CParticleSystem+0x120` — the emitter's derived state word.
enum SystemStateFlag : u32 {
    kStateForces = 0x1,             ///< local force pair nonzero.
    kStateWorldForces = 0x2,        ///< `localForces|worldForces >= 0x10000`.
    kStateWorldSpace = 0x4,         ///< `additionalFlags & 8`.
    kStateInheritVelocity = 0x8,    ///< `flags & 0x40`.
    kStateGpuMotion = 0x10,         ///< the CPU/GPU motion split (RE §2).
    kStateScaleTimeByParent = 0x20,
    /// Unnamed in the RE's `0x120` table. `Tick` tests `0x20|0x40` together
    /// before scaling `dtMs`, so this bit arms the same multiply — found by
    /// tracing what the tick READS, which is the only place either bit is
    /// consumed (OP3).
    kStateScaleTimeAlso = 0x40,
    kStateEmissionDisabled = 0x80,
    /// `Tick`'s second consumer of this bit: a resync forces the FULL-step
    /// path, so the sweep never straddles the discontinuity (OP3).
    kStateSquirtResync = 0x100,
    kStateUseLocalTime = 0x400,
    /// Re-entrancy guard around the sequence-change restart check; while it is
    /// set, `Tick` skips the check AND leaves bit 31 standing (OP3).
    kStateRestartBusy = 0x800,
    kStateSquirtPrime = 0x1000,
    kStateSequenceChanged = 0x80000000u, ///< set by UpdateEmitterState.
};

/// `CParticleSystem+0x34C` — the u24 emission-state word.
enum EmitStateFlag : u32 {
    kEmitAnyAnimated = 0x4,  ///< some AnimRef is animated (gates the sampler).
    kEmitNoise = 0x8,
    kEmitStillEmitting = 0x10,
    kEmitSuppressed = 0x20,
};

/// `SParticleElement+0x3E` — the per-element state word.
enum ElementFlag : u16 {
    kElemTrail = 0x1,
    /// A collision spawn or splat that skips its chance roll.
    kElemForced = 0x2,
    kElemCollideTerrain = 0x4,
    kElemCollideObjects = 0x8,
    kElemOrientationFrozen = 0x10, ///< instanceType 6 froze its direction.
    kElemAtRest = 0x40,            ///< gravity off; collision put it to sleep.
};

// -- channel orders ----------------------------------------------------------------
// Positional indices with no name before. Unscoped enums in namespaces, so they
// index an array without a cast and do not collide.

/// `midTime[]` / `midHold[]` / the batch row's mid-key lanes.
namespace MidChannel {
enum : i32 { Size = 0, Color = 1, Alpha = 2, Rotation = 3, kCount = 4 };
} // namespace MidChannel

/// The nine overlay-wave groups of a `PAR_`, in the runtime's order. Group 0
/// drives YAW and group 1 PITCH — the swap RE §11.5 records.
namespace OverlayGroup {
enum : i32 {
    Yaw = 0,
    Pitch = 1,
    Speed = 2,
    Size = 3,
    Alpha = 4,
    ColorDead = 5,
    Rotation = 6,
    Horizontal = 7,
    Vertical = 8,
    kCount = 9
};
} // namespace OverlayGroup

/// `vs::InterpolateValue`'s mode, which is what every smoothing byte selects.
namespace SmoothingMode {
enum : i32 { Linear = 0, Smooth = 1, Bezier = 2, LinearHold = 3, BezierHold = 4 };
} // namespace SmoothingMode

} // namespace whiteout::flakes::renderer::sc2
