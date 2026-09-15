#pragma once

// ============================================================================
// The forty D3 particle channels, by ENGINE CHANNEL ID.
//
// The id is argument 3 of every `InterpolationPath_Eval*` call and it is the
// only stable key: shipped `.prt` assets are struct version 180, where all
// forty paths are inline, while the Switch binary compiles version 213, which
// moved twelve of them into a side `ExtendedPath_Console` block. Slot indices
// differ between the two; channel ids do not.
//
// The slot -> id tables below were read off `ParticleSystem_Spawn`'s
// constant-channel mask: bit N of that mask gates the evaluation of file slot
// N, and the guarding `TBZ` immediately before each `Eval` names the channel.
// All twenty-four particle slots and all thirteen emitter slots agree with the
// path TYPES WhiteoutLib's `native::Particle` declares, which is an
// independent check on the whole table.
// ============================================================================

#include "types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::particle::d3 {

// ---- units and the engine's own constants ----

/// Every `.prt` rate is authored per frame at 60 fps: a velocity channel is
/// ×60, an acceleration ×60², a frame count ×1/60.
inline constexpr f32 kFramesPerSecond = 60.0f;
inline constexpr f32 kFramesPerSecondSq = 3600.0f;
inline constexpr f32 kFrameSeconds = 1.0f / 60.0f;

/// The engine's correctly-rounded 2π (@0x7100E3BEE4), the one angles wrap by.
/// NOT the azimuth constant the shape samplers multiply by — see
/// `kAzimuthTwoPi` in `d3_emitter.cpp`, two ULPs below it on purpose.
inline constexpr f32 kTwoPi = 6.28318530717958647692f;

/// The engine's degenerate epsilon, `1e-6` @0x7100E3BEA0: below it a length,
/// a divisor or an opacity counts as zero.
inline constexpr f32 kEpsilon = 1e-6f;
/// `0.999999` @0x7100E3BFF8: at or above it an opacity is opaque, and a path's
/// loop end reaching it spans the whole curve.
inline constexpr f32 kNearlyOne = 0.999999f;

/// The two seed values the engine nudges out of the way of its sentinels.
inline constexpr u32 kFirstSentinelSeed = 0xFFFFFFFEu;
/// What a particle's UV-state stream is seeded with: its own seed, salted.
inline constexpr u32 kUvSeedSalt = 0x2D32u;

/// `ParticleSystem_TickEmitter`'s hard ceiling on live particles, and the
/// smallest pool it grows to.
inline constexpr i32 kMaxLiveParticles = 4096;
inline constexpr usize kMinPoolGrowth = 64;
/// Foliage emits once, to at most this many, and then only sways.
inline constexpr i32 kMaxWindSpringPopulation = 256;
/// `PrtFlag::ClampDistanceEmission`'s ceiling on the emitter speed, `.prt`
/// units per second.
inline constexpr f32 kDistanceEmissionMaxSpeed = 300.0f;
/// `tmPreSimulate` is clamped to this many seconds (@0x71000ADF84).
inline constexpr f32 kMaxPreSimulateSeconds = 1000.0f;
/// `particle+212`'s clamp.
inline constexpr f32 kMinParticleSize = 0.0001f;
inline constexpr f32 kMaxParticleSize = 999.0f;

/// `nTimeMode`: how an evaluation turns `time / period` into a curve position.
enum class TimeMode : i32 {
    Raw = 0,         ///< the quotient as it is, no wrap
    Looped = 1,      ///< wrapped into the path's loop sub-range
    LoopedBlend = 2, ///< as Looped, cross-fading toward a second sample at the end
};

/// `dwPrtFlags` — the bits the shipped runtime reads. One more is read and not
/// reproduced: bit 2 gates a placement test in `Particle_InitLifeAndSize` that
/// can reject the birth.
enum class PrtFlag : u32 {
    /// Bit 0: the system runs until told to stop — time mode 1, no release test.
    Persistent = 0x1u,
    /// Bit 8: birth AT the emitter (skipping the lerp and its draw), and carry
    /// the live particles when the emitter moves.
    BirthAtEmitter = 0x100u,
    /// Bit 10: the PARTICLE channels are sampled unwrapped (time mode 0).
    ParticleUnwrapped = 0x400u,
    /// Bit 12: a uniform random initial roll, and for a child actor a second
    /// uniform turn about world X.
    RandomRoll = 0x1000u,
    /// Bit 28: ENABLES the `kDistanceEmissionMaxSpeed` clamp.
    ClampDistanceEmission = 0x10000000u,
    /// Bit 29: carry by translation only, never by rotation.
    CarryWithoutRotation = 0x20000000u,
};

inline constexpr bool Has(u32 prtFlags, PrtFlag f) {
    return (prtFlags & static_cast<u32>(f)) != 0;
}

// ---- particle channels (ids 1..25; there is no id 4) ----
enum ChannelId : i32 {
    kChSize = 1,          ///< FloatPath   — multiplies the birth size
    kChHeightRatio = 2,   ///< FloatPath   — the quad's height / its width
    kChColor = 3,         ///< ColorPath   — packed RGBA
    kChScale = 5,         ///< FloatPath   — a second size term
    kChAlpha = 6,         ///< FloatPath   — 0..1; the whole of COLOR1, not COLOR0.a
    kChOrbitRadius = 7,   ///< FloatPath   — orbit radial offset, differentiated
    kChOrbitRadSpeed = 8, ///< VelocityPath        — orbit radial speed, x60
    kChOrbitAngSpeed = 9, ///< AngularVelocityPath — orbit angular speed, x60
    kChOrbitAxis = 10,    ///< VectorPath  — orbit axis, default (0,0,1)
    kChRadialOffset = 11, ///< FloatPath   — radial push offset, differentiated
    kChRadialSpeed = 12,  ///< VelocityPath — radial push speed, x60
    kChSeekSpeed = 13,    ///< VelocityPath — target seek speed, x60
    kChSeekOffset = 14,   ///< FloatPath    — target seek offset, differentiated
    kChSpinRate = 15,     ///< AngularVelocityPath — free-spin rate, x60
    kChSpinAngle = 16,    ///< AnglePath    — free-spin angle; NOTE the reversed sign
    kChOffsetA = 17,      ///< VectorPath       — world triple, offset
    kChVelocityA = 18,    ///< VelocityVectorPath — world triple, velocity x60
    kChAccelA = 19,       ///< AccelVectorPath    — world triple, accel x3600
    kChOffsetB = 20,      ///< VectorPath       — emitter-local triple, offset
    kChVelocityB = 21,    ///< VelocityVectorPath — emitter-local triple, velocity x60
    kChAccelB = 22,       ///< AccelVectorPath    — emitter-local triple, accel x3600
    kChSpinAxis = 23,     ///< VectorPath   — free-spin axis; unauthored, a random unit vector
    kChRollAngle = 24,    ///< AnglePath    — roll angle, differentiated
    kChRollRate = 25,     ///< AngularVelocityPath — roll rate, x60

    // ---- emitter channels (ids 28..40) ----
    kChBirthSize = 28,     ///< FloatPath   — base size at birth
    kChParticleLife = 29,  ///< TimePath    — lifetime in frames, x1/60
    kChSpeedLifeCut = 30,  ///< VelocityPath — shortens life by emitter speed
    kChTargetCount = 31,   ///< IntPath     — target live population
    kChEmissionRate = 32,  ///< VelocityPath — particles per frame, x60
    kChDistanceRate = 33,  ///< VelocityPath — particles per unit travelled
    kChSizeScale = 34,     ///< FloatPath   — emitter-wide size multiplier
    kChEffectScale = 35,   ///< FloatPath   — emitter-wide effect multiplier
    kChRetiredVector = 36, ///< VectorPath  — retired in the shipped revision
    kChRetiredFloat = 37,  ///< FloatPath   — retired in the shipped revision
    kChInitialVelocity = 38, ///< VelocityVectorPath — birth velocity, x60
    kChSpreadAngle = 39,     ///< AnglePath          — cone half-angle
    kChWorldVelocity = 40,   ///< VelocityVectorPath — added after the cone, x60
};

/// Ids run 1..40, so a 41-entry array indexed by id needs no map.
inline constexpr i32 kChannelIdCount = 41;

/// File slot -> channel id, `arEmitterPath[0..12]` at `0x030 + 48N`. No code
/// indexes this table or the next: they are the record of how each `.prt`
/// path was matched to its channel id, kept beside the ids they explain.
inline constexpr i32 kEmitterSlotChannel[13] = {
    kChSizeScale,       // 0  arSizeScalePath
    kChTargetCount,     // 1  arCountPath
    kChEffectScale,     // 2  arEffectScalePath
    kChParticleLife,    // 3  arParticleLifePath
    kChBirthSize,       // 4  arInitialSizePath
    kChSpreadAngle,     // 5  arSpreadAnglePath
    kChInitialVelocity, // 6  arInitialVelocityPath
    kChWorldVelocity,   // 7  arInitialVelocityWorldPath
    kChEmissionRate,    // 8  arEmissionRatePath
    kChDistanceRate,    // 9  arEmitterRatePathA   <- settled by the trail test
    kChSpeedLifeCut,    // 10 arEmitterRatePathB
    kChRetiredVector,   // 11 arUnknownEmitterVectorPath
    kChRetiredFloat,    // 12 arUnknownEmitterFloatPath
};

/// File slot -> channel id, `arParticlePath[0..23]` at `0x458 + 48N`. This IS
/// the constancy-mask bit order.
inline constexpr i32 kParticleSlotChannel[24] = {
    kChColor,          // 0  arColorPath
    kChScale,          // 1  arScalePath
    kChAlpha,          // 2  arAlphaPath
    kChSize,           // 3  arSizePath
    kChHeightRatio,    // 4  arSize2Path
    kChRollAngle,      // 5  arRotationPath
    kChRollRate,       // 6  arRotationRatePath
    kChSpinRate,       // 7  arRotation2RatePath
    kChSpinAngle,      // 8  arRotation2Path
    kChSpinAxis,       // 9  arAxisPath
    kChOrbitRadius,    // 10 arScalarPathCh07
    kChOrbitRadSpeed,  // 11 arRatePathCh08
    kChOrbitAngSpeed,  // 12 arSpinRatePathCh09
    kChOrbitAxis,      // 13 arAxis2Path
    kChRadialOffset,   // 14 arScalarPathCh11
    kChRadialSpeed,    // 15 arRatePathCh12
    kChOffsetA,        // 16 arOffsetPath
    kChVelocityA,      // 17 arVelocityPath
    kChAccelA,         // 18 arAccelerationPath
    kChOffsetB,        // 19 arOffset2Path
    kChVelocityB,      // 20 arVelocity2Path
    kChAccelB,         // 21 arAcceleration2Path
    kChSeekSpeed,      // 22 arRatePathCh13
    kChSeekOffset,     // 23 arScalarPathCh14
};

/// Emitter shapes. `ParticleSystem_SampleEmitterShape`'s switch asserts on its
/// default, so this set is closed. Values 0, 2 and 3 do not exist.
enum class Shape : i32 {
    Point = 1,
    SphereShell = 4,     ///< annulus radius x uniform direction on a full sphere
    Cylinder = 5,        ///< annulus radius in XY, uniform Z from extent 1
    MeshRandom = 6,      ///< area-weighted random point on the actor's skinned mesh
    MeshActorKind4 = 7,  ///< as 6, through a different scene-object table
    Box = 8,             ///< per-axis uniform between the extent-2 endpoints
    Ring = 9,            ///< as 5, but the azimuth is evenly spaced across the tick
    HemisphereShell = 10, ///< as 4, +Z half only
    MeshSequential = 11,  ///< as 6, walking triangles in order
};

/// The per-system capability mask `ParticleSystem_Spawn` derives at load, and
/// the thing the per-frame step actually branches on. Not `dwPrtFlags` — that
/// word has five read bits in the whole shipped runtime and none of them is a
/// motion model.
///
/// The engine's mask carries one more bit nothing here branches on: 0x2000,
/// `eSystemType` in {1, 3, 4} (`EmitsActors` answers that off the type).
enum Capability : u32 {
    kCapOrbit = 0x0001,
    kCapRadial = 0x0002,
    kCapTripleA = 0x0004,
    kCapTripleB = 0x0008,
    kCapSpin = 0x0010,
    /// Channel 23 is authored: its start lane's minimum is not (0,0,0). Without
    /// it the spin axis is the unit-sphere point drawn at birth.
    kCapSpinAxis = 0x0040,
    kCapSeek = 0x0080,
    kCapRoll = 0x0100,
};

/// `eSystemType` — this picks the whole update path, not a variation of one.
/// Three of the ten shipped values are never simulated at all.
///
/// `Ribbon` is the first RE pass's name and is wrong: type 1 is 4,790 files that
/// each spawn a MODEL (see `EmitterDesc::SpawnsChildActors`).
enum class SystemType : i32 {
    Standard = 0,
    Ribbon = 1,        ///< 22% of the corpus: segment records, not pool particles
    Swarm = 2,         ///< + a body in the shared world-collision solver
    RibbonPhysics = 3, ///< ribbon and body
    LightShaft = 4,    ///< never updated
    Unused5 = 5,       ///< never updated; zero files in the corpus
    Foliage = 6,       ///< wind spring only
    StaticClutter = 7, ///< never updated
    WindClutter = 8,   ///< wind spring only
    Weather = 9,       ///< + a body
    WorldAnchored = 10, ///< emits from the world origin
};

/// The types `ParticleSystem_UpdateDispatch` @0x71000B1660 never updates:
/// `(1 << type) & 0xB0` — light shafts, the unused slot and static clutter.
inline constexpr u32 kNeverUpdatedTypes = (1u << 4) | (1u << 5) | (1u << 7);
/// `& 0x140` — the two that run only the wind spring.
inline constexpr u32 kWindSpringTypes = (1u << 6) | (1u << 8);

/// Whether the ordinary particle simulation runs for this system type.
/// Types outside the shipped range simulate, as the dispatch's default does.
inline bool SimulatesParticles(SystemType type) {
    const i32 t = static_cast<i32>(type);
    if (t < 0 || t > static_cast<i32>(SystemType::WorldAnchored))
        return true;
    const u32 bit = 1u << static_cast<u32>(t);
    return (bit & kNeverUpdatedTypes) == 0 && (bit & kWindSpringTypes) == 0;
}

/// Types 6 and 8 — foliage and clutter. They run the damped wind spring
/// INSTEAD of the particle simulation, never alongside it.
inline bool UsesWindSpring(SystemType type) {
    return type == SystemType::Foliage || type == SystemType::WindClutter;
}

/// Types 7 and 8 place their instances at load: `ParticleSystem_TickEmitter`
/// returns before its accumulator (`eSystemType - 7 < 2`).
inline bool SkipsEmission(SystemType type) {
    return type == SystemType::StaticClutter || type == SystemType::WindClutter;
}

/// Types 1, 3 and 4: `ParticleSystem_EmitParticle`'s `(type - 3) < 2 || type
/// == 1` — an emission is a whole actor, not a pool particle.
inline bool EmitsActors(SystemType type) {
    return type == SystemType::Ribbon || type == SystemType::RibbonPhysics ||
           type == SystemType::LightShaft;
}

/// Types 2, 3 and 9 own a body in the shared world-collision solver.
inline bool OwnsSolverBody(SystemType type) {
    return type == SystemType::Swarm || type == SystemType::RibbonPhysics ||
           type == SystemType::Weather;
}

/// `nRenderMode` — which frame `Particle_BuildOrientationBasis` @0x71000BAB30
/// builds. See `d3_orientation.h` for what each one is.
enum class PrtRenderMode : i32 {
    CameraGated = 0,    ///< writes nothing ungated; the child arm faces the camera
    Unoriented = 1,     ///< writes nothing
    AxisStreak = 2,     ///< along the motion axis, turned to the camera
    AcrossAxis = 3,     ///< across the motion axis
    AcrossSystemXY = 4, ///< across the flattened direction from the system
    AcrossSystem = 5,   ///< across the direction from the system
    AcrossCameraXY = 6, ///< across the flattened camera direction
    EmitterFrame = 7,   ///< the emitter's own frame
    UnorientedAlt = 8,  ///< writes nothing
    Ground = 9,         ///< conformed to the terrain normal
    GroundAlt = 10,     ///< as 9
    SystemStreak = 11,  ///< along the direction from the system, turned to the camera
    AxisUpright = 12,   ///< along the motion axis, about world up
    Vertical = 13,      ///< world-vertical, yawed to the camera
};

/// Modes 9 and 10 read the terrain normal under the particle.
inline bool ConformsToGround(PrtRenderMode mode) {
    return mode == PrtRenderMode::Ground || mode == PrtRenderMode::GroundAlt;
}

} // namespace whiteout::flakes::renderer::particle::d3
