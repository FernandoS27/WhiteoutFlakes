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
    kChSpinAxis = 23,     ///< VectorPath   — free-spin axis, default (0,1,0)
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

/// File slot -> channel id, `arEmitterPath[0..12]` at `0x030 + 48N`.
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
enum Capability : u32 {
    kCapOrbit = 0x0001,
    kCapRadial = 0x0002,
    kCapTripleA = 0x0004,
    kCapTripleB = 0x0008,
    kCapSpin = 0x0010,
    kCapSpinAxis = 0x0040,
    kCapSeek = 0x0080,
    kCapRoll = 0x0100,
    kCapRibbon = 0x2000, ///< eSystemType in {1,3,4}
};

/// `eSystemType` — this picks the whole update path, not a variation of one.
/// Three of the ten shipped values are never simulated at all.
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

/// Whether the ordinary particle simulation runs for this system type.
/// `ParticleSystem_UpdateDispatch` @0x71000B1660: `(1<<type) & 0xB0` returns
/// outright, `& 0x140` goes to the wind spring, everything else simulates.
inline bool SimulatesParticles(i32 systemType) {
    if (systemType < 0 || systemType > 10)
        return true;
    const u32 bit = 1u << static_cast<u32>(systemType);
    return (bit & 0xB0u) == 0 && (bit & 0x140u) == 0;
}

/// Types 6 and 8 — foliage and clutter. They run the damped wind spring
/// INSTEAD of the particle simulation, never alongside it.
inline bool UsesWindSpring(i32 systemType) {
    return systemType == 6 || systemType == 8;
}

} // namespace whiteout::flakes::renderer::particle::d3
