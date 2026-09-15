#pragma once

// ============================================================================
// SC2 particle kernels — SPAWN.
//
// Where a particle is born and how it heads off, its colour, size and
// rotation, `InitSpawnedParticles` over a batch, and the batch order.
//
// The contract is `particle_stages_sc2.h`'s: every routine the oracle
// recorded is a free function over an explicit inputs struct, called by the
// tick and by the replay alike, with the float operation ORDER the binary's.
// ============================================================================

#include "renderer/sc2/sc2_constants.h"
#include "renderer/sc2/sc2_rng.h"
#include "sc2_kernel_types.h"
#include "whiteout/flakes/types.h"
#include "whiteout/vector_types.h"

#include <array>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::particle {

// ---------------------------------------------------------------------------
// SPAWN — position and velocity (`SampleSpawnPosition` / `SampleSpawnVelocity`,
// RE §5.6-5.7, gates OP4 and OP5).
//
// Both draw from the emitter's generator, and the DRAW ORDER is as much of the
// contract as the arithmetic: two shapes that produce the same distribution
// from different stream positions are different emitters the moment anything
// else draws in the same frame. Every ordering below is one the golden
// separated, not one that reads naturally.
// ---------------------------------------------------------------------------

// `Sc2SpawnShape` lives in `sc2_kernel_types.h`.

// ---------------------------------------------------------------------------
// SPAWN — the Mesh shape (`SampleEmitterMeshSurface`, RE §16.14, gate OP13).
//
// The loop rejects POSITIONS, never particles: four draws an attempt (the
// triangle, two barycentrics, the mask byte) and the 32nd attempt is taken
// whether or not its mask passed. A dark vertex mask therefore moves the
// emitter's whole stream, which is why the draws are burned even when nothing
// needs them.
// ---------------------------------------------------------------------------

/// One entry of an emitter-region slot's triangle table.
struct Sc2MeshTriangle {
    u32 firstIndex = 0; ///< into the face buffer
    u32 region = 0;     ///< which region record biases the three indices
};

/// A region record's vertex base. Retail adds BOTH terms — REGN+4, which
/// WhiteoutLib calls `unknown`, and `firstVertex` — and the first is 0 in all
/// 75,031 shipped regions, so only a fixture can show the sum.
struct Sc2MeshRegionBase {
    u32 bias = 0;
    u32 firstVertex = 0;
};

struct Sc2MeshSurfaceInputs {
    /// `model+0x150` and `asset+0x88`. Either missing returns before the pick
    /// and draws nothing.
    bool haveAsset = true;
    bool haveVertexDesc = true;
    /// The emitter's slot. Empty is both the null table and the zero-count
    /// one: the pick is called, refuses, and draws nothing.
    std::span<const Sc2MeshTriangle> triangles{};
    std::span<const u32> faces{};
    std::span<const Sc2MeshRegionBase> regions{};
    /// R of each vertex's BGRA colour (the byte at colour offset + 2). Empty
    /// is a vertex format without colour, which reads 255 at every corner.
    std::span<const u8> colorR{};
    /// `M3_ComputeSkinnedRegionPositions` for one absolute vertex index, in
    /// model space. A function pointer for the collider's reason: this runs
    /// three times a particle.
    void* ctx = nullptr;
    Vector3f (*position)(void* ctx, u32 vertex) = nullptr;
};

struct Sc2MeshSample {
    bool hit = false;
    Vector3f position{0, 0, 0};
    /// The winding's face normal, exactly `(0,0,1)` on a zero-area triangle.
    Vector3f normal{0, 0, 0};
    /// Calls to the triangle pick — a refused one included.
    u32 tries = 0;
};

/// `SampleEmitterMeshSurface` — model space.
Sc2MeshSample Sc2SampleMeshSurface(sc2::Rng& rng, const Sc2MeshSurfaceInputs& in);

struct Sc2SpawnPosInputs {
    Sc2SpawnShape shape = Sc2SpawnShape::Point;
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
    const Sc2MeshSurfaceInputs* mesh = nullptr;
};

/// `CParticleSystem::SampleSpawnPosition` — emitter space, except the Mesh
/// shape, whose points are the posed model's.
///
/// @p normal receives the Mesh shape's face normal and is left alone by every
/// other shape. Retail passes it only for velocityType 4 and zeroes it first,
/// so a type-4 emitter on any other shape spawns with no velocity at all.
Vector3f Sc2SampleSpawnPosition(sc2::Rng& rng, const Sc2SpawnPosInputs& in,
                                Vector3f* normal = nullptr);

/// One overlay group: `M3_SampleAnimValue(type, freq·variationTime +
/// variationPhase, amplitude)`. `type == 0` is "unarmed" and draws nothing.
struct Sc2Overlay {
    u32 type = 0;
    f32 amplitude = 0.0f;
    f32 frequency = 0.0f;
};

struct Sc2SpawnVelInputs {
    /// `Sc2VelocityType`, as the golden records the word.
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
    /// `Sc2RotationBit::FlattenVelocityXY`: drop z, rescale xy to the original
    /// magnitude.
    bool flattenXY = false;
    Vector3f position{0.0f, 0.0f, 0.0f}; ///< the spawn position (radial/axis)
    Vector3f normal{0.0f, 0.0f, 1.0f};   ///< the mesh face normal (type 4)
    f32 variationTime = 0.0f;
    f32 variationPhase = 0.0f;
    /// Groups 0, 1, 2, 7, 8 of `pOverlayParams`. Group 0 modulates **yaw** and
    /// group 1 **pitch** — the swap RE §11.5 records, kept because the file
    /// field names are the ones that are wrong.
    Sc2Overlay yawOverlay{};
    Sc2Overlay pitchOverlay{};
    Sc2Overlay speedOverlay{};
    Sc2Overlay horizontalOverlay{};
    Sc2Overlay verticalOverlay{};
};

/// `CParticleSystem::SampleSpawnVelocity` — emitter space, speed folded in.
Vector3f Sc2SampleSpawnVelocity(sc2::Rng& rng, const Sc2SpawnVelInputs& in);


// ---------------------------------------------------------------------------
// SPAWN — the three attribute samplers (`SampleParticleColor` / `..Size` /
// `..Rotation`, RE §5.8, gate OP6).
//
// Each reads exactly ONE overlay group — alpha 4, size 3, rotation 6 — and
// samples it BEFORE its own random draws. That order only became observable
// once the gate armed a wave type that draws (type 5); with the wave types the
// original grid used, nothing could tell the two orders apart.
// ---------------------------------------------------------------------------

struct Sc2ColorInputs {
    /// Packed BGRA, so as a u32 the ALPHA is the HIGH byte. start/mid/end.
    std::array<u32, 3> keys{};
    /// The endpoints each key lerps toward under @ref randomEnable.
    std::array<u32, 3> randomKeys{};
    bool randomEnable = false;
    f32 colorMidTime = 0.0f;
    f32 alphaMidTime = 0.0f;
    Sc2Overlay alphaOverlay{};
    f32 variationTime = 0.0f;
    f32 variationPhase = 0.0f;
};

/// `CParticleSystem::SampleParticleColor` — three packed BGRA nodes.
std::array<u32, 3> Sc2SampleColor(sc2::Rng& rng, const Sc2ColorInputs& in);

struct Sc2SizeInputs {
    std::array<f32, 3> keys{};        ///< start/mid/end
    std::array<f32, 3> randomKeys{};
    bool randomEnable = false;
    Sc2Overlay sizeOverlay{};
    /// The caller's scale ratio (`blend`), applied after the halving.
    f32 blend = 1.0f;
    u32 instanceType = 0;
    f32 instanceDistance = 0.0f;
    f32 variationTime = 0.0f;
    f32 variationPhase = 0.0f;
};

/// `CParticleSystem::SampleParticleSize` — HALF extents, as floats. The ×256
/// quantisation to the element's u16 belongs to `InitSpawnedParticles` (OP8),
/// not here. `.w` is `instanceDistance` only for instance type 9.
std::array<f32, 4> Sc2SampleSize(sc2::Rng& rng, const Sc2SizeInputs& in);

struct Sc2RotationInputs {
    std::array<f32, 3> keys{};        ///< start/mid/end, RADIANS
    std::array<f32, 3> randomKeys{};
    bool randomEnable = false;
    /// `Sc2RotationBit::Relative` — the mid and end keys become deltas on the
    /// running value instead of offsets from the overlay.
    bool relative = false;
    f32 rotationMidTime = 0.0f;
    Sc2Overlay rotationOverlay{};
    f32 variationTime = 0.0f;
    f32 variationPhase = 0.0f;
};

/// `CParticleSystem::SampleParticleRotation` — radians.
std::array<f32, 3> Sc2SampleRotation(sc2::Rng& rng, const Sc2RotationInputs& in);


// ---------------------------------------------------------------------------
// SPAWN — `InitSpawnedParticles` (RE §5.5, gate OP8).
//
// The five samplers above in one function, plus the space transform. OP4/5/6
// pin each sampler's own draw order; this pins the order they are CALLED in,
// which no per-kernel gate can see.
//
// Note the overlap with `Sc2SpawnInputs` in `sc2_runtime.h`: that one is the
// emitter's per-spawn block (and carries the X6 mesh fields), this one is what
// the gate drives. The emitter loop fills one from the other in a single
// place; they are deliberately not merged while the mesh half is unwritten.
// ---------------------------------------------------------------------------

/// What the emitter carries across a batch and this function advances.
struct Sc2InitState {
    f32 emitterTime = 0.0f;
    Vector3f curPos{0, 0, 0};
    /// A running MAXIMUM in milliseconds over every element the emitter ever
    /// spawns, so it never retreats while the emitter lives.
    u32 expireFrameMs = 0;
};

struct Sc2InitInputs {
    Sc2SpawnPosInputs shape{};
    Sc2SpawnVelInputs velocity{};
    Sc2ColorInputs color{};
    Sc2SizeInputs size{};
    Sc2RotationInputs rotation{};

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

    /// One per element, or empty. A request also forces the LOCAL space path
    /// even on an emitter that would otherwise use the world basis.
    std::span<const SpawnRequest> requests{};
};

/// `CParticleSystem::InitSpawnedParticles` over a batch of pending elements.
void Sc2InitSpawned(sc2::Rng& rng, const Sc2InitInputs& in, Sc2InitState& state,
                    std::span<Sc2SpawnedElement> out);

// ---------------------------------------------------------------------------
// SPAWN — the batch order (`SpawnParticles`, RE §15.5 and §16.9, gate OP8b).
// ---------------------------------------------------------------------------

struct Sc2SpawnBatchInputs {
    u32 requests = 0;     ///< waiting in the inbox
    u32 plain = 0;        ///< this call's own count
    u32 elementCount = 0; ///< alive before the call
    u32 maxParticles = 0;
};

/// One `InitSpawnedParticles` call. Its first `requests` elements are
/// request-born, from inbox index `requestBegin` on; the rest are plain.
struct Sc2SpawnFlush {
    u32 requestBegin = 0;
    u32 requests = 0;
    u32 plain = 0;
};

struct Sc2SpawnBatchPlan {
    u32 created = 0;
    std::vector<Sc2SpawnFlush> flushes;
    /// `InitSpawnedParticles` zeroes the request count whenever it runs. So
    /// the inbox is emptied — requests the ceiling refused included — only
    /// when some flush ran; a call that created nothing leaves every request
    /// waiting for the next one.
    bool requestsConsumed = false;
};

/// Requests first, then plain particles, each ONE element guarded by
/// `elementCount < maxParticles` in turn; the pending list flushes at 128 from
/// the request loop only, and once more at the end. The flushes are what make
/// the swept clock and the request pointers line up with retail's.
Sc2SpawnBatchPlan Sc2PlanSpawnBatch(const Sc2SpawnBatchInputs& in);

} // namespace whiteout::flakes::renderer::particle
