#pragma once

// ============================================================================
// Sc2EmitterDesc — the `PAR_` record, converted once at load.
//
// The load-time half of an SC2 particle emitter, hanging off EmitterDesc and
// read only when `family == Family::Sc2`. It is grouped by the STAGE that
// reads it (SC2_PARTICLE_DESIGN.md §3.2) rather than by the `PAR_` field
// order, because each §5.0 kernel takes one group plus the frame it needs —
// which is what keeps those kernels' inputs small enough to test against the
// oracle goldens one at a time.
//
// Raw bits are kept as WhiteoutLib's enums rather than as `u32`, so a
// consumer names the bit it wants and the compiler catches the wrong word
// (design R7). The runtime words the RE describes — stateFlags, emitFlags and
// the per-element flags — are NOT file bits and do not live here.
//
// Filled by `DescFromSc2ParticleConfig` since P1. The STAGES that read it
// arrive one phase at a time, so a group with no consumer yet is a group whose
// conversion is already gated — which is the point of converting it early.
// ============================================================================

#include "types.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"
#include "whiteout/models/m3/structures/base.h"

#include <algorithm>
#include <vector>

namespace whiteout::flakes::renderer::particle {

namespace m3 = ::whiteout::m3;

using m3::ParticleAdditionalFlag;
using m3::ParticleFlag;
using m3::ParticleRotationFlag;

/// One emission slot's squirt table, carried over from the config unchanged —
/// the key TIMES are what `CrossedSquirtAmount` (RE §7b) needs, and sampling
/// has already thrown them away by the time a frame arrives.
using effects::Sc2SquirtKeys;

struct Sc2EmitterDesc {
    // ---- raw bits, read through the enums ----
    ParticleFlag flags{};
    ParticleAdditionalFlag additionalFlags{};
    ParticleRotationFlag rotationFlags{};

    bool Has(ParticleFlag f) const {
        return (static_cast<u32>(flags) & static_cast<u32>(f)) != 0;
    }
    bool Has(ParticleAdditionalFlag f) const {
        return (static_cast<u32>(additionalFlags) & static_cast<u32>(f)) != 0;
    }
    bool Has(ParticleRotationFlag f) const {
        return (static_cast<u32>(rotationFlags) & static_cast<u32>(f)) != 0;
    }

    // ---- EMIT ----
    struct Emit {
        u8 shape = 0;         ///< 0 Point … 5 Disc, 6 Spline, 7 Mesh (RE §11.1).
        u8 velocityType = 0;  ///< 0 cone, 1 radial, 2 axis, 3 random, 4 mesh normal.
        u32 maxParticles = 0; ///< Pool cap = min(authored, 0x200000/464) (RE §3.1).
        u8 lodReduce = 0, lodCut = 0; ///< Table ROW indices, not counts.

        /// Bone per emission slot; slot 0 is the `PAR_` itself and 1..n are its
        /// `PARC` copies, so `slotBones[0] == boneIndex`.
        std::vector<i32> slotBones;
        std::vector<Sc2SquirtKeys> squirt;
        /// Mesh shape only: the `.m3` region ids particles are born on.
        std::vector<i32> shapeRegions;

        bool sizeRandom = false, rotationRandom = false, colorRandom = false;
        bool speedRandom = false, lifetimeRandom = false, massRandom = false;

        /// yaw, pitch, speed, size, alpha, color (dead), rotation, horizontal,
        /// vertical — the overlay wave type per channel group (RE §9).
        u32 overlayType[9] = {};

        /// `SimulateInit`'s peak per global container, and the init value an
        /// unbound track takes (RE §15.4, §16.33); `Sc2PreRollPeakFor` indexes
        /// it with the active sequence.
        std::vector<f32> preRollPeaks;
        f32 preRollInit = 0.0f;
        bool worldSpace = false;      ///< additionalFlags & WorldSpace.
        bool inheritVelocity = false; ///< flags & InheritParentVelocity.
    } emit;

    // ---- MOVE ----
    struct Motion {
        /// `drag` floors to 0.01 at render time rather than here (RE §8.1), so
        /// the authored zero survives into the golden.
        f32 drag = 0.0f, mass = 0.001f, massRandom = 1.0f;
        Vector3f gravity3{0, 0, 0}; ///< (gravityX, gravityY, gravity).
        f32 bounce = 0.0f, friction = 1.0f;
        u32 collisionDieBounce = 0;
        f32 windMultiplier = 0.0f, killRadius = 0.0f;
        f32 noiseAmplitude = 0.0f, noiseFrequency = 0.0f;
        f32 noiseCoherence = 0.0f, noiseEdge = 0.0f;
        /// `local | world << 16` — the `FOR_` channel masks. Stubbed; both
        /// pairs are kept because `CanUseGpuMotion` tests the FALLBACK one.
        u32 forces = 0, forcesFallback = 0;
        /// `CanUseGpuMotion` (RE §2), decided once at load, never per frame.
        bool analytic = false;
    } motion;

    // ---- BUILD ----
    struct Look {
        i32 materialIndex = -1; ///< `MATM` index.
        /// Resolved by the loader BEFORE the desc is frozen; −1 leaves the draw
        /// on the BLS fallback rather than a prebuilt SC2 surface.
        i32 m3Surface = -1;
        u8 instanceType = 0; ///< 0..10, the shader's `b_iInstanceType`.
        /// As authored; 2 is Bezier. The legacy Bezier bits reach nothing
        /// (design §8); the per-frame conversion a Bezier channel does need is
        /// `Sc2ConvertBezierKeys`.
        u8 sizeSmoothing = 0, colorSmoothing = 0, rotationSmoothing = 0;
        /// size / color / alpha / rotation, each clamped to ≤ 0.996 at load.
        ///
        /// The clamp is `InitCopy`'s (template → clone), in 4.8 as in 5.0;
        /// plain `Init` leaves the authored value alone (RE §1). Applying it
        /// here follows the clone path, which is what an emitter instance is.
        f32 midTime[4] = {}, midHold[4] = {};
        f32 flipbookMidTime = 0.0f;
        u16 flipbookColumns = 0, flipbookRows = 0;
        f32 flipbookColumnFraction = 0.0f, flipbookRowFraction = 0.0f;
        /// `EndStop` is authored and never read — see RE §11.
        u8 flipbookStartInit = 0, flipbookStartStop = 0, flipbookEndInit = 0;
        /// `b_iUVMapping[0] == UVMAP_PARTICLE_FLIPBOOK` on the resolved
        /// surface's diffuse layer, stamped by the loader beside
        /// @ref m3Surface. It is a MATERIAL property, not a `PAR_` one, and it
        /// lands here so the geometry build never looks a material up — the
        /// same rule `m3Surface` follows. False leaves every particle on cell
        /// 0 of its sheet, which is what an unresolved material gets too.
        ///
        /// Its sibling arm `b_UVRandomOffsetEnable[0]` has no field here: the
        /// shader arm is measured (OP12) but the `.m3` field that drives it is
        /// not identified — see RE §16.29 — so the build passes `false` at
        /// the call site rather than guessing a source.
        bool flipbookUv = false;
        f32 tailLength = 1.0f;
        Vector3f instanceAngle{0, 0, 0};
        f32 instanceDistance = 1.0f;
        /// `flags & LitParts`. The draw takes `forceUnshaded = !lit`.
        bool lit = false;
    } look;

    // ---- OUTPUT ----
    struct Children {
        i32 collisionSpawnIndex = -1;
        u32 collisionSpawnMin = 0, collisionSpawnMax = 0;
        f32 collisionSpawnChance = 0.0f, collisionSpawnEnergy = 0.0f;
        /// Resolved at load from the child's own config, so the MOVE stage
        /// never looks an emitter up at run time (design R4).
        bool collisionChildIsWorldSpace = false;
        i32 trailLinkIndex = -1;
        f32 trailChance = 0.0f;
        i32 splatProjectorIndex = -1;
        f32 splatChance = 0.0f;
        /// `PAR_+0x5D0` / `+0x5D4`. The ribbon campaign's field names
        /// (`spawnRibbonOnBounceChance`, `ribbonLinkIndex`); this consumer
        /// reads the first as a plain NON-ZERO TEST — a legacy-orientation
        /// flag, not a chance and not a preset index — and the second as the
        /// variant it selects for instanceType 3. OP14 found only four of the
        /// seven presets distinct, and the `variant == 6` arm dead.
        bool modelOrientLegacy = false;
        i32 modelOrientVariant = -1;
        bool scaleCollisionChild = false; ///< rotationFlags & 0x10.
        bool scaleTrailChild = false;     ///< rotationFlags & 0x20.
    } children;
};

/// @brief `CanUseGpuMotion` (RE §2, oracle OP1) — which MOVE variant this
///        emitter takes, decided once at load and never per frame.
///
/// The closed-form path integrates a particle's position from its birth state
/// in one expression; anything that makes a step depend on the step before it
/// — a force, a collision, noise, a trail, a model particle, sorting — demotes
/// the emitter to Euler. Written as one expression because that is what it is
/// in the image, and because OP1's 94 vectors pin the boundaries: every
/// threshold is `0.001 <= field`, so exactly 0.001 demotes and 0.000999 does
/// not.
inline bool Sc2CanUseGpuMotion(const Sc2EmitterDesc& d) {
    // 0x10480003 — Sort | CollideTerrain | SpawnTrailingParticles |
    // ModelParticles | RequiresGpuSim.
    constexpr u32 kDemote = static_cast<u32>(ParticleFlag::Sort) |
                            static_cast<u32>(ParticleFlag::CollideTerrain) |
                            static_cast<u32>(ParticleFlag::SpawnTrailingParticles) |
                            static_cast<u32>(ParticleFlag::ModelParticles) |
                            static_cast<u32>(ParticleFlag::RequiresGpuSim);
    static_assert(kDemote == 0x10480003u);
    const u32 flags = static_cast<u32>(d.flags);
    // TerrainOriented and TerrainDirOriented need the ground under a particle
    // at every step, which the closed form has no way to ask about.
    //
    // The force operand is the FALLBACK pair alone. OP1's 94 vectors are
    // explicit: `localForces`/`worldForces` at 1, 2 and 0xFFFF all stay
    // analytic, and only the fallback pair demotes — the same surprise the
    // ribbon's `Ribbon_SelectSimTechnique` produced (RE §3). Testing both, as
    // this line first did, demotes emitters retail runs on the closed form.
    return d.look.instanceType != 5 && d.look.instanceType != 6 &&
           d.motion.forcesFallback == 0 && (flags & kDemote) == 0 &&
           d.motion.windMultiplier < 0.001f && d.motion.killRadius < 0.001f &&
           d.motion.noiseAmplitude < 0.001f &&
           (flags & static_cast<u32>(ParticleFlag::ForceProceduralPosition)) == 0;
}

} // namespace whiteout::flakes::renderer::particle
