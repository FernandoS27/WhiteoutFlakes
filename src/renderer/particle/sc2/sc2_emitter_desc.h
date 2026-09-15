#pragma once

// ============================================================================
// EmitterDesc — the `PAR_` record, converted once at load and read only when
// `family == Family::Sc2`. Grouped by the STAGE that reads it, not by `PAR_`
// field order; raw bits stay WhiteoutLib's enums (design R7), and the runtime
// words (stateFlags, emitFlags, element flags) do not live here.
// See SC2_PARTICLE_DESIGN.md §16.2.
// ============================================================================

#include "renderer/particle/sc2/sc2_kernel_types.h"
#include "renderer/sc2/sc2_constants.h"
#include "types.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"
#include "whiteout/models/m3/structures/base.h"

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

} // namespace whiteout::flakes::renderer::particle

namespace whiteout::flakes::renderer::particle::sc2 {

/// A raw `PAR_` word tested for one of its enum's bits. The kernels take the
/// three words as the goldens record them — plain `u32`s — and read them
/// through this, so a bit is named at every test even where the word is raw.
template <class Flag>
constexpr bool Has(u32 word, Flag bit) {
    return (word & static_cast<u32>(bit)) != 0;
}

/// The `flags` bits WhiteoutLib's `m3::ParticleFlag` names wrongly, by what
/// executing the image shows the runtime doing with them.
namespace ParBit {
/// Bit 31. Demotes the emitter to the Euler path (`CanUseGpuMotion`, OP1,
/// tested signed) and runs the CPU bounds pass (`SimulateParticles`).
/// TODO(WhiteoutLib): labelled `ForceProceduralPosition` there — the opposite
/// of what it does.
inline constexpr ParticleFlag CpuBounds = static_cast<ParticleFlag>(0x80000000u);
} // namespace ParBit

/// The `rotationFlags` bits by what the runtime does with them (RE §3, §16).
/// WhiteoutLib's `m3::ParticleRotationFlag` names only 0x2, 0x4, 0x40 and 0x80,
/// two of those provisionally, so the roles are named here over its enum.
namespace RotationBit {
/// A random u16 `flipbookRand` per particle — the random UV byte pair.
/// TODO(WhiteoutLib): unnamed in `ParticleRotationFlag`.
inline constexpr ParticleRotationFlag RandomUvOffset = static_cast<ParticleRotationFlag>(0x1u);
/// The mid and end rotation keys are deltas on the running value.
inline constexpr ParticleRotationFlag Relative = ParticleRotationFlag::Relative;
/// A model particle reads the ELEMENT's own keys, not the emitter's cache.
inline constexpr ParticleRotationFlag ElementKeys = ParticleRotationFlag::AlwaysSet;
/// Drop z of the spawn velocity and rescale xy to the original magnitude.
/// TODO(WhiteoutLib): unnamed in `ParticleRotationFlag`.
inline constexpr ParticleRotationFlag FlattenVelocityXY = static_cast<ParticleRotationFlag>(0x8u);
/// `Update` pushes the emitter's row lengths onto child 0 / child 1.
/// TODO(WhiteoutLib): unnamed in `ParticleRotationFlag`.
inline constexpr ParticleRotationFlag ScaleCollisionChild = static_cast<ParticleRotationFlag>(0x10u);
inline constexpr ParticleRotationFlag ScaleTrailChild = static_cast<ParticleRotationFlag>(0x20u);
/// A model particle draws a random direction at its pending spawn. The low
/// byte is tested signed, so this is bit 0x80 and not bit 31.
inline constexpr ParticleRotationFlag RandomDirection = ParticleRotationFlag::Unknown7;
} // namespace RotationBit

struct EmitterDesc {
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
        u8 velocityType = 0;  ///< @ref VelocityType
        u32 maxParticles = 0; ///< Pool cap = min(authored, `renderer::sc2::kMaxParticles`) (RE §3.1).
        u8 lodReduce = 0, lodCut = 0; ///< Table ROW indices, not counts.

        /// Bone per emission slot; slot 0 is the `PAR_` itself and 1..n are its
        /// `PARC` copies, so `slotBones[0] == boneIndex`.
        std::vector<i32> slotBones;
        std::vector<Sc2SquirtKeys> squirt;
        /// Mesh shape only: the `.m3` region ids particles are born on.
        std::vector<i32> shapeRegions;

        /// The three per-channel enables. The speed, lifetime and mass
        /// randomisers are `additionalFlags` bits, which the kernels read raw.
        bool sizeRandom = false, rotationRandom = false, colorRandom = false;

        /// The overlay wave type per channel group, indexed by
        /// `renderer::sc2::OverlayGroup` (RE §9).
        u32 overlayType[renderer::sc2::OverlayGroup::kCount] = {};

        /// `SimulateInit`'s peak per global container, and the init value an
        /// unbound track takes (RE §15.4, §16.33); `PreRollPeakFor` indexes
        /// it with the active sequence.
        std::vector<f32> preRollPeaks;
        f32 preRollInit = 0.0f;
        bool worldSpace = false; ///< additionalFlags & WorldSpace.
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
        u8 instanceType = 0; ///< `InstanceType`, the shader's `b_iInstanceType`.
        /// `renderer::sc2::SmoothingMode`, as authored. The legacy Bezier bits reach nothing
        /// (design §8); the per-frame conversion a Bezier channel does need is
        /// `ConvertBezierKeys`.
        u8 sizeSmoothing = 0, colorSmoothing = 0, rotationSmoothing = 0;
        /// Indexed by `renderer::sc2::MidChannel`, each clamped to ≤ `renderer::sc2::kMidTimeCeil`
        /// at load: `InitCopy`'s clamp, the clone path an emitter instance is.
        /// See SC2_PARTICLE_RE.md §17.2.
        f32 midTime[renderer::sc2::MidChannel::kCount] = {}, midHold[renderer::sc2::MidChannel::kCount] = {};
        f32 flipbookMidTime = 0.0f;
        u16 flipbookColumns = 0, flipbookRows = 0;
        f32 flipbookColumnFraction = 0.0f, flipbookRowFraction = 0.0f;
        /// `EndStop` is authored and never read — see RE §11.
        u8 flipbookStartInit = 0, flipbookStartStop = 0, flipbookEndInit = 0;
        /// `b_iUVMapping[0] == UVMAP_PARTICLE_FLIPBOOK` on the resolved
        /// surface's diffuse layer: a MATERIAL property the loader stamps here
        /// so the build never looks a material up. False keeps cell 0. The
        /// sibling `b_UVRandomOffsetEnable[0]` has no source yet (RE §16.29).
        /// See SC2_PARTICLE_RE.md §17.2.
        bool flipbookUv = false;
        f32 tailLength = 1.0f;
        Vector3f instanceAngle{0, 0, 0};
        f32 instanceDistance = 1.0f;
        /// `flags & LitParts`. The adapter turns it into
        /// `ParticleMaterialDesc::unshaded = !lit`, which the M3 draw forces.
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
        /// `PAR_+0x5D0` / `+0x5D4`, read as a NON-ZERO legacy-orientation test
        /// and the instanceType 3 variant it selects. See SC2_PARTICLE_RE.md §17.2.
        bool modelOrientLegacy = false;
        i32 modelOrientVariant = -1;
    } children;
};

/// @brief `CanUseGpuMotion` (RE §2, oracle OP1) — which MOVE variant this
///        emitter takes, decided once at load and never per frame. Anything
///        that makes a step depend on the one before demotes to Euler; every
///        threshold is `kGpuMotionGate <= field`. See SC2_PARTICLE_RE.md §17.2.
inline bool CanUseGpuMotion(const EmitterDesc& d) {
    // 0x10480003 — Sort | CollideTerrain | SpawnTrailingParticles |
    // ModelParticles | RequiresGpuSim.
    constexpr u32 kDemote = static_cast<u32>(ParticleFlag::Sort) |
                            static_cast<u32>(ParticleFlag::CollideTerrain) |
                            static_cast<u32>(ParticleFlag::SpawnTrailingParticles) |
                            static_cast<u32>(ParticleFlag::ModelParticles) |
                            static_cast<u32>(ParticleFlag::RequiresGpuSim);
    static_assert(kDemote == 0x10480003u);
    const u32 flags = static_cast<u32>(d.flags);
    // The terrain types need the ground at every step. The force operand is
    // the FALLBACK pair alone (OP1): testing both demotes emitters retail runs
    // on the closed form.
    const InstanceType type = InstanceTypeOf(d.look.instanceType);
    return type != InstanceType::TerrainOriented &&
           type != InstanceType::TerrainDirOriented && d.motion.forcesFallback == 0 &&
           (flags & kDemote) == 0 && d.motion.windMultiplier < renderer::sc2::kGpuMotionGate &&
           d.motion.killRadius < renderer::sc2::kGpuMotionGate &&
           d.motion.noiseAmplitude < renderer::sc2::kGpuMotionGate &&
           !d.Has(ParBit::CpuBounds);
}

} // namespace whiteout::flakes::renderer::particle::sc2
