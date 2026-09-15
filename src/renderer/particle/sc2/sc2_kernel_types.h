#pragma once

// ============================================================================
// The small SC2 vocabulary the kernels, the runtime and the desc all name, and
// the two records every stage passes around: the element and its GPU vertex.
//
// The desc carries the enums as the authored byte and the kernels take the
// golden's raw word, because that is what the oracle records; the enums are
// what either side compares that number against.
// ============================================================================

#include "whiteout/flakes/types.h"
#include "whiteout/vector_types.h"

#include <array>
#include <cstddef>
#include <vector>

namespace whiteout::flakes::renderer::particle::sc2 {

using whiteout::Matrix44f;
using whiteout::Vector3f;

/// `b_iInstanceType` — the shader's eleven quad shapes, in its own order.
///
/// Named here rather than in `EmitterDesc` because the shader is what reads
/// the number: the descriptor carries the authored byte, this is what the
/// permutation means by it.
enum class InstanceType : u32 {
    Billboard = 0,
    Tail = 1,
    FaceTravelDir = 2,
    FaceWorldDir = 3,
    SingleAxis = 4,
    TerrainOriented = 5,
    TerrainDirOriented = 6,
    EmitterOriented = 7,
    PhysicsOriented = 8,
    Pinned = 9,
    Trail = 10,
};

/// The authored instance byte as the enum. Values past `Trail` stay what they
/// are: a caller that has to reject them compares against `Trail`.
inline constexpr InstanceType InstanceTypeOf(u32 raw) {
    return static_cast<InstanceType>(raw);
}

/// Emitter shape, runtime numbering. The M3 spec has 6 and 7 swapped; this is
/// the order the binary dispatches (RE §5.6).
enum class SpawnShape : u32 {
    Point = 0,
    Plane = 1,
    Sphere = 2,
    Box = 3,
    Cylinder = 4,
    Disc = 5,
    Spline = 6,
    Mesh = 7,
};

/// `SampleSpawnVelocity`'s direction models (RE §5.7).
enum class VelocityType : u32 {
    Cone = 0,
    Radial = 1,
    Axis = 2,
    Random = 3,
    MeshNormal = 4,
};

/// One emitter asking another to spawn a particle at a place: retail's
/// `SParticleSpawnRequest`, 36 bytes (RE §3.3) — a collision child at the hit
/// point, a trail child along the parent's path. The receiver materialises it
/// before its own spawns on its next EMIT. Defined where `InitSpawned` READS it.
struct SpawnRequest {
    Vector3f position{0, 0, 0};
    Vector3f velocityScale{1, 1, 1};
    Vector3f orientVec{0, 0, 1};
};

struct Runtime;

/// A request with the emitter it is addressed to. The maker pushes these onto
/// its outbox; the service drains them and delivers each to the target's inbox
/// — which is why no emitter ever looks another one up (design R4).
struct RoutedSpawnRequest {
    i32 targetEmitterId = -1;
    SpawnRequest req;
};

/// A ModelParticles element still waiting for its model. Retail's spawn-batch
/// entry is `{element, PAR_, CParticleSystem}`; the runtime stands for the last
/// two and the node for the first.
struct PendingModel {
    Runtime* runtime = nullptr;
    i32 node = -1;
};

/// The frame's pending list. Retail keeps one per update thread and walks it
/// once, after the parallel update job — the frame driver raises a
/// thread-local byte around the job so `Update` skips its own walk (RE
/// §16.32). One per service here, walked after every emitter has updated.
using PendingModels = std::vector<PendingModel>;

/// The fields `InitSpawnedParticles` writes, in retail's units — sizes and
/// rotations already quantised, colours still packed BGRA.
struct SpawnedElement {
    Vector3f position{0, 0, 0};
    Vector3f velocity{0, 0, 0};
    Vector3f orientVec{0, 0, 0};
    Vector3f spawnOrigin{0, 0, 0};
    f32 invMass = 0.0f;
    f32 noisePhase = 0.0f;
    f32 trailAccum = 0.0f;
    f32 birthTime = 0.0f;
    f32 deathTime = 0.0f;
    f32 flipbookRandStart = 0.0f;
    std::array<u16, 4> size{};       ///< half extents ×256, truncated
    std::array<u16, 3> rotation{};   ///< radians ×32, truncated
    std::array<u32, 3> colorNodes{};
    u16 flipbookRand = 0;
    /// `vNoiseVector.xyz`. Written by the noise stage, not at spawn — every
    /// OP8 vector left it at the fixture's zero, so treat a zero here as "no
    /// noise stage has run yet", never as a measured value.
    Vector3f noiseVec{0, 0, 0};
    u16 flags = 0;                   ///< `(PAR_.flags · 2) & 0xC`, plus bit 0 for a trail
    i32 vbSlot = -1;
    i32 bounceCount = 0;
};


/// `Particle.fx`'s `Input`, byte for byte (RE §8.1).
///
/// Plain scalars rather than `Vector3f` on purpose: this is a vertex-buffer
/// layout, and the offsets below are asserted, not hoped for.
struct GpuVertex {
    f32 position[3];        ///< +0    `vPosition.xyz`
    /// +12. `vPosition.w`, which no shader path reads — `EmitParticleHPos`
    /// rebuilds the vector as `float4(vPosition.xyz, 1)`. Retail copies
    /// whatever the element's lane holds; nothing ever writes it.
    u32 positionW;
    u16 size[4];            ///< +16   `vSize`, ×256
    u32 color[3];           ///< +24   `cColor0/1/2`, BGRA
    u16 rotation[3];        ///< +36   `vRotation.xyz`, radians ×32
    u16 flipbookRand;       ///< +42   `vRotation.w`
    f32 birthTime;          ///< +44   `vBirthDeathAndDrag.x`
    f32 deathTime;          ///< +48   `.y`
    f32 drag;               ///< +52   `.z`, floored
    f32 invDrag;            ///< +56   `.w`, an uploaded lane and NOT `1/drag`
    /// +60. `iBatchIndex` (`uint4` over UBYTE4); the shader indexes every
    /// batched constant array with lane .x. Retail's analytic path leaves it
    /// stale. See @ref VertexBodyInputs and SC2_PARTICLE_RE.md §17.3.
    u32 batchIndex;
    f32 velocity[3];        ///< +64   `vInterpolator1.xyz`
    f32 invMass;            ///< +76   `.w` — the element's, not the 0 the
                            ///<        pending-spawn path writes there
    f32 instanceVec[3];     ///< +80   `vInterpolator2.xyz`, per instance type
    f32 gravityZ;           ///< +92   `.w`, read back NEGATED by the shader
    f32 noise[3];           ///< +96   `vNoiseVector.xyz`
    f32 flipbookRandStart;  ///< +108  `.w`
    i16 corner[2];          ///< +112  `vOffset`
};
static_assert(sizeof(GpuVertex) == 116, "the ParticleVB stride is 116");
static_assert(offsetof(GpuVertex, batchIndex) == 60);
static_assert(offsetof(GpuVertex, corner) == 112);

/// The four corners, in the order both vertex builders write them: as s16
/// pairs (-1,1) (1,1) (-1,-1) (1,-1).
inline constexpr i16 kCorners[4][2] = {{-1, 1}, {1, 1}, {-1, -1}, {1, -1}};

} // namespace whiteout::flakes::renderer::particle::sc2
