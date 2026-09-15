#pragma once

// ============================================================================
// SC2 particle kernels — SPAWN: where a particle is born and how it heads off,
// its colour, size and rotation, `InitSpawnedParticles` over a batch, and the
// batch order. The contract is `particle_stages_sc2.h`'s.
// ============================================================================

#include "renderer/particle/sc2/sc2_kernel_types.h"
#include "renderer/sc2/sc2_constants.h"
#include "renderer/sc2/sc2_rng.h"
#include "whiteout/flakes/types.h"
#include "whiteout/vector_types.h"

#include <array>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::particle::sc2 {

// ---------------------------------------------------------------------------
// SPAWN — position and velocity (`SampleSpawnPosition` / `SampleSpawnVelocity`,
// RE §5.6-5.7, gates OP4 and OP5). The DRAW ORDER is as much of the contract
// as the arithmetic; every ordering below is one the golden separated.
// See SC2_PARTICLE_RE.md §17.8.
// ---------------------------------------------------------------------------

// `SpawnShape` lives in `sc2_kernel_types.h`.

// ---------------------------------------------------------------------------
// SPAWN — the Mesh shape (`SampleEmitterMeshSurface`, RE §16.14, gate OP13).
// The loop rejects POSITIONS, never particles: four draws an attempt, and the
// 32nd attempt is taken whether or not its mask passed. See SC2_PARTICLE_RE.md §17.8.
// ---------------------------------------------------------------------------

/// One entry of an emitter-region slot's triangle table.
struct MeshTriangle {
    u32 firstIndex = 0; ///< into the face buffer
    u32 region = 0;     ///< which region record biases the three indices
};

/// A region record's vertex base: retail adds BOTH REGN+4 (WhiteoutLib's
/// `unknown`) and `firstVertex`. See SC2_PARTICLE_RE.md §17.8.
struct MeshRegionBase {
    u32 bias = 0;
    u32 firstVertex = 0;
};

struct MeshSurfaceInputs {
    /// `model+0x150` and `asset+0x88`. Either missing returns before the pick
    /// and draws nothing.
    bool haveAsset = true;
    bool haveVertexDesc = true;
    /// The emitter's slot. Empty is both the null table and the zero-count
    /// one: the pick is called, refuses, and draws nothing.
    std::span<const MeshTriangle> triangles{};
    std::span<const u32> faces{};
    std::span<const MeshRegionBase> regions{};
    /// R of each vertex's BGRA colour (the byte at colour offset + 2). Empty
    /// is a vertex format without colour, which reads 255 at every corner.
    std::span<const u8> colorR{};
    /// `M3_ComputeSkinnedRegionPositions` for one absolute vertex index, in
    /// model space. A function pointer for the collider's reason: this runs
    /// three times a particle.
    void* ctx = nullptr;
    Vector3f (*position)(void* ctx, u32 vertex) = nullptr;
};

struct MeshSample {
    bool hit = false;
    Vector3f position{0, 0, 0};
    /// The winding's face normal, exactly `(0,0,1)` on a zero-area triangle.
    Vector3f normal{0, 0, 0};
    /// Calls to the triangle pick — a refused one included.
    u32 tries = 0;
};

/// `SampleEmitterMeshSurface` — model space.
MeshSample SampleMeshSurface(renderer::sc2::Rng& rng, const MeshSurfaceInputs& in);

struct SpawnPosInputs {
    SpawnShape shape = SpawnShape::Point;
    /// `ParticleFlag::EmitShapeCutout` — hollows the shape: the radius becomes a
    /// range and the box picks one face pair.
    bool cutout = false;
    Vector3f shapeOuter{0.0f, 0.0f, 0.0f};
    Vector3f shapeInner{0.0f, 0.0f, 0.0f};
    f32 outerRadius = 0.0f;
    f32 innerRadius = 0.0f;
    /// The element's scale. The plane reads it OFF BY ONE — see the .cpp.
    Vector3f elemScale{1.0f, 1.0f, 1.0f};
    /// Spline control points, four per segment.
    std::span<const Vector3f> spline{};
    f32 splineLowerBound = 0.0f;
    f32 splineUpperBound = 1.0f;
    /// Mesh (shape 7) only. Null is an emitter pointed at no asset: nothing is
    /// drawn and the origin comes back.
    const MeshSurfaceInputs* mesh = nullptr;
};

/// `CParticleSystem::SampleSpawnPosition` — emitter space, except the Mesh
/// shape, whose points are the posed model's. @p normal receives the Mesh
/// shape's face normal and is left alone by every other shape.
/// See SC2_PARTICLE_RE.md §17.8.
Vector3f SampleSpawnPosition(renderer::sc2::Rng& rng, const SpawnPosInputs& in,
                                Vector3f* normal = nullptr);

/// One overlay group: `M3_SampleAnimValue(type, freq·variation.time +
/// variation.phase, amplitude)`. `type == 0` is "unarmed" and draws nothing.
struct Overlay {
    u32 type = 0;
    f32 amplitude = 0.0f;
    f32 frequency = 0.0f;
};

/// The clock every overlay group's wave is sampled on: the emitter's variation
/// time, offset by the frame's overlay phase. The same pair for every sampler
/// a spawn calls.
struct Variation {
    f32 time = 0.0f;
    f32 phase = 0.0f;
};

struct SpawnVelInputs {
    /// `VelocityType`, as the golden records the word.
    u32 velocityType = 0;
    f32 spawnYaw = 0.0f;    ///< degrees
    f32 spawnPitch = 0.0f;  ///< degrees
    f32 spawnHorizontal = 0.0f; ///< radians
    f32 spawnVertical = 0.0f;   ///< radians
    f32 speed = 0.0f;
    f32 speedRandom = 0.0f;
    /// `ParticleAdditionalFlag::EmitSpeedRandomize`: the speed becomes
    /// `Rand(speed, speedRandom)` and the speed OVERLAY is ignored.
    bool speedIsEndpoint = false;
    /// `RotationBit::FlattenVelocityXY`: drop z, rescale xy to the original
    /// magnitude.
    bool flattenXY = false;
    Variation variation{};
    /// Groups 0, 1, 2, 7, 8 of `pOverlayParams`. Group 0 modulates **yaw** and
    /// group 1 **pitch** — the swap RE §11.5 records, kept because the file
    /// field names are the ones that are wrong.
    Overlay yawOverlay{};
    Overlay pitchOverlay{};
    Overlay speedOverlay{};
    Overlay horizontalOverlay{};
    Overlay verticalOverlay{};
};

/// `CParticleSystem::SampleSpawnVelocity` — emitter space, speed folded in.
///
/// @p position is the spawn position the radial and axis types aim along, and
/// @p normal the Mesh shape's face normal type 4 reads — both what
/// `SampleSpawnPosition` just produced for this element.
Vector3f SampleSpawnVelocity(renderer::sc2::Rng& rng, const SpawnVelInputs& in,
                                const Vector3f& position, const Vector3f& normal);


// ---------------------------------------------------------------------------
// SPAWN — the three attribute samplers (`SampleParticleColor` / `..Size` /
// `..Rotation`, RE §5.8, gate OP6). Each reads exactly ONE overlay group —
// alpha 4, size 3, rotation 6 — and samples it BEFORE its own random draws.
// See SC2_PARTICLE_RE.md §17.8.
// ---------------------------------------------------------------------------

struct ColorInputs {
    /// Packed BGRA, so as a u32 the ALPHA is the HIGH byte. start/mid/end.
    std::array<u32, 3> keys{};
    /// The endpoints each key lerps toward under @ref randomEnable.
    std::array<u32, 3> randomKeys{};
    bool randomEnable = false;
    f32 colorMidTime = 0.0f;
    f32 alphaMidTime = 0.0f;
    Overlay alphaOverlay{};
    Variation variation{};
};

/// `CParticleSystem::SampleParticleColor` — three packed BGRA nodes.
std::array<u32, 3> SampleColor(renderer::sc2::Rng& rng, const ColorInputs& in);

struct SizeInputs {
    std::array<f32, 3> keys{};        ///< start/mid/end
    std::array<f32, 3> randomKeys{};
    bool randomEnable = false;
    Overlay sizeOverlay{};
    u32 instanceType = 0;
    f32 instanceDistance = 0.0f;
    Variation variation{};
};

/// `CParticleSystem::SampleParticleSize` — HALF extents, as floats; the ×256
/// quantisation is `InitSpawnedParticles`' (OP8). `.w` is `instanceDistance`
/// only for instance type 9. @p blend, applied after the halving, is 1 for the
/// `PAR_` and the bone's row length over the emitter's for a `PARC` copy.
std::array<f32, 4> SampleSize(renderer::sc2::Rng& rng, const SizeInputs& in, f32 blend);

struct RotationInputs {
    std::array<f32, 3> keys{};        ///< start/mid/end, RADIANS
    std::array<f32, 3> randomKeys{};
    bool randomEnable = false;
    /// `RotationBit::Relative` — the mid and end keys become deltas on the
    /// running value instead of offsets from the overlay.
    bool relative = false;
    f32 rotationMidTime = 0.0f;
    Overlay rotationOverlay{};
    Variation variation{};
};

/// `CParticleSystem::SampleParticleRotation` — radians.
std::array<f32, 3> SampleRotation(renderer::sc2::Rng& rng, const RotationInputs& in);


// ---------------------------------------------------------------------------
// SPAWN — `InitSpawnedParticles` (RE §5.5, gate OP8): the five samplers above
// in their CALL order, plus the space transform. The mesh shape's half is
// `SpawnPosInputs::mesh`. See SC2_PARTICLE_RE.md §17.8.
// ---------------------------------------------------------------------------

/// What the emitter carries across a batch and this function advances.
struct InitState {
    f32 emitterTime = 0.0f;
    Vector3f curPos{0, 0, 0};
    /// A running MAXIMUM in milliseconds over every element the emitter ever
    /// spawns, so it never retreats while the emitter lives. Gate-only:
    /// nothing here reads it.
    u32 expireFrameMs = 0;
};

struct InitInputs {
    SpawnPosInputs shape{};
    SpawnVelInputs velocity{};
    ColorInputs color{};
    SizeInputs size{};
    RotationInputs rotation{};

    u32 parFlags = 0;
    u32 additionalFlags = 0;
    u32 rotationFlags = 0;
    u32 instanceType = 0;
    /// `CParticleSystem::emitFlagsWord`, not a `PAR_` field: bit 3 arms the
    /// per-particle noise phase.
    u32 emitFlagsWord = 0;
    /// Bit 3 selects, INSIDE the world arm only, the normalised basis plus the
    /// inherited parent velocity. It does nothing on the local path.
    u32 stateFlags = 0;

    f32 noiseCoherence = 0.0f;
    f32 mass = 1.0f;
    f32 massRandom = 0.0f;
    f32 lifetime = 0.0f;
    f32 lifetimeRandom = 0.0f;
    f32 trailChance = 0.0f;
    u16 flipbookColumns = 0;
    u16 flipbookRows = 0;
    bool hasChildEmitter1 = false;

    /// 0 for the `PAR_` itself, `PARC` copy index + 1 otherwise.
    u32 slot = 0;
    Matrix44f worldMatrix = Matrix44f::identity();
    Matrix44f boneMatrix = Matrix44f::identity();
    bool hasBone = false;

    Vector3f spawnPosStep{0, 0, 0};
    f32 spawnTimeStep = 0.0f;
    Vector3f smoothedPos{0, 0, 0};
    f32 inheritVelocityScale = 0.0f;
    u32 nowMs = 0;
};

/// `CParticleSystem::InitSpawnedParticles` over a batch of pending elements.
/// @p requests is one per element from the front of @p out, or empty; a
/// request forces the LOCAL space path even on a world-space emitter.
/// See SC2_PARTICLE_RE.md §17.8.
void InitSpawned(renderer::sc2::Rng& rng, const InitInputs& in,
                    std::span<const SpawnRequest> requests, InitState& state,
                    std::span<SpawnedElement> out);

// ---------------------------------------------------------------------------
// SPAWN — the batch order (`SpawnParticles`, RE §15.5 and §16.9, gate OP8b).
// ---------------------------------------------------------------------------

struct SpawnBatchInputs {
    u32 requests = 0;     ///< waiting in the inbox
    u32 plain = 0;        ///< this call's own count
    u32 elementCount = 0; ///< alive before the call
    u32 maxParticles = 0;
};

/// One `InitSpawnedParticles` call. Its first `requests` elements are
/// request-born, from inbox index `requestBegin` on; the rest are plain.
struct SpawnFlush {
    u32 requestBegin = 0;
    u32 requests = 0;
    u32 plain = 0;
};

struct SpawnBatchPlan {
    u32 created = 0;
    std::vector<SpawnFlush> flushes;
    /// `InitSpawnedParticles` zeroes the request count whenever it runs. So
    /// the inbox is emptied — requests the ceiling refused included — only
    /// when some flush ran; a call that created nothing leaves every request
    /// waiting for the next one.
    bool requestsConsumed = false;
};

/// Requests first, then plain particles, each ONE element guarded by
/// `elementCount < maxParticles` in turn; the pending list flushes at 128 from
/// the request loop only, and once more at the end. The flushes are what make
/// the swept clock and the request pointers line up with retail's. @p plan is
/// overwritten, its flush storage reused.
void PlanSpawnBatch(const SpawnBatchInputs& in, SpawnBatchPlan& plan);

} // namespace whiteout::flakes::renderer::particle::sc2
