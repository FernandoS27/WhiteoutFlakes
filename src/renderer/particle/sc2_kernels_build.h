#pragma once

// ============================================================================
// SC2 particle kernels — BUILD.
//
// The once-written GPU vertex, the CPU vertex refresh, the quad expansion
// and the batch row the shader indexes.
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
// The GPU vertex (OP11b, design P2 BUILD).
//
// `UploadGpuParticles` writes each element into the ParticleVB block ONCE —
// when it has no slot yet, or when the block moved under it — and never
// touches those bytes again; the vertex shader does the rest from `age`. So
// this is the analytic variant's entire vertex body, and there is no second
// chance to correct anything it gets wrong.
// ---------------------------------------------------------------------------

/// `drag` and `invDrag` are two independent lanes, not a reciprocal pair.
struct Sc2DragLanes {
    f32 drag = 0.01f;
    f32 invDrag = 100.0f;
};

/// The floor and its reciprocal, in the builder's order.
///
/// The binary floors `drag` and then branches on the RAW value for `invDrag`,
/// taking a literal 100 below the floor. That branch is kept for fidelity, not
/// because it changes an answer: `1.0f / 0.01f` is bit-exactly `100.0f`, so
/// flooring first and dividing agrees on every input. A mutation to
/// `1 / out.drag` stayed green across all 63 OP11b vectors, and no vector
/// could separate them — they are extensionally the same function.
///
/// The claim that IS measured is a different one: `invDrag` is an uploaded
/// lane the shader reads on its own, so a shader-side `1/drag` is wrong. Every
/// OP12 vector had `invDrag == 1/drag` exactly until the grid gained a
/// deliberately inconsistent pair (RE §16.26).
Sc2DragLanes Sc2ComputeDragLanes(f32 drag);

struct Sc2VertexBodyInputs {
    f32 drag = 0.0f;
    /// `PAR_.gravity` times the scene's wind scale — 1.0 with no scene. The
    /// product is what lands in the vertex; the shader negates it.
    f32 gravity = 0.0f;
    f32 worldGravityScale = 1.0f;
    u32 instanceType = 0;
    f32 tailLength = 0.0f;
    Vector3f instanceAngle{0, 0, 0};
    /// The emitter's slot in the batched-constant arrays (`CParticleSystem`
    /// +0x34F, annotated `randSeed` in the IDB and renamed once it was clear
    /// nothing seeds a random stream with it: `Init` hands out
    /// `(previous + 1) & 0x1F` and `BuildRenderBatch_GPU` flags the emitter as
    /// `1 << it`). Retail's analytic path never writes it into the vertex —
    /// see @ref Sc2GpuVertex::batchIndex — so we do, and bind one emitter's
    /// constants at index 0 rather than inheriting a stale lane.
    u32 batchIndex = 0;
};

/// What `BuildParticleQuadVertices_List` / `_Ranged` read besides the element
/// (RE §16.12, gate OP11) — the CPU path's per-frame vertex builder.
struct Sc2CpuVertexInputs {
    u32 instanceType = 0;
    f32 tailLength = 0.0f;
    Vector3f instanceAngle{0, 0, 0};
    f32 drag = 0.0f;
    f32 gravity = 0.0f;
    f32 gravityScale = 1.0f;
    /// The size knee the tail clamp budgets against.
    f32 sizeMidTime = 0.5f;
    u32 parFlags = 0;
    /// `emitFlags & kEmitNoise` — the runtime's noise bit, not a `PAR_` one.
    bool noise = false;
    f32 noiseAmplitude = 0.0f;
    f32 noiseFrequency = 0.0f;
    f32 noiseCoherence = 0.0f;
    f32 noiseEdge = 0.0f;
    f32 emitterTime = 0.0f;
    /// `stateFlags & kStateGpuMotion`. Set, the builder refreshes the GPU lanes from
    /// `PAR_` and the element; clear, it keeps the element's cached ones.
    bool gpuMotion = false;
    u32 batchIndex = 0;
    /// `SampleVectorField2D` on the scene's wind object, for types 5 and 6.
    /// Null is a scene with no such object, which retail answers with a flat
    /// `(0,0,1)` and a zero gravity lane.
    void* fieldCtx = nullptr;
    void (*field)(void* ctx, f32 x, f32 y, f32 out[2]) = nullptr;
};

/// One element through the CPU quad builder.
///
/// @p cache is the element's GPU lanes — retail keeps them inside the element
/// (`0x44..0xB4`) and this store keeps them in its parallel vertex, which is
/// the same bytes by construction. The builder writes the element too: the
/// noise vector, the type-6 rotation re-key, and the tail clamp's latch.
void Sc2CpuVertexBody(const Sc2CpuVertexInputs& in, Sc2SpawnedElement& e, Sc2GpuVertex& cache);

/// The vertex one element uploads, corner lanes left zero.
///
/// Retail writes four, identical but for `vOffset`, which is `kSc2Corners[k]`
/// for every particle — so the store keeps one and the quad builder supplies
/// the corner. `batchIndex` is written; retail's block path leaves those bytes
/// at whatever the slot last held, which is why OP11b fills the range with a
/// value keyed to the dword index: a body copy that skipped the lane and one
/// that wrote a constant are otherwise the same green.
Sc2GpuVertex Sc2VertexBody(const Sc2VertexBodyInputs& in, const Sc2SpawnedElement& e);

// ---------------------------------------------------------------------------
// The quad expansion (OP12, design P2 BUILD).
//
// Everything above hands the GPU a point; this is what the vertex shader makes
// of it. `Particle.fx`'s `ParticleVertexShader` interpolates the size, colour
// and rotation from `age`, optionally re-derives the position from the closed
// form (@ref Sc2StepAnalytic), then expands the corner offset into world space
// per instance type. Only the BILLBOARD branch is here — X4 brings the other
// ten, and @ref Sc2QuadResult::supported says so rather than quietly returning
// a billboard for a tail.
// ---------------------------------------------------------------------------

/// `p_*[iBatchIndex]`: the per-emitter constants the shader indexes.
struct Sc2QuadBatch {
    /// Indexed by `sc2::MidChannel` — one mid-key set per interpolated
    /// channel, which is why they are four lanes and not one.
    std::array<f32, 4> midKey{0.5f, 0.5f, 0.5f, 0.5f};
    std::array<f32, 4> invMidKey{2.0f, 2.0f, 2.0f, 2.0f};
    std::array<f32, 4> hold{0, 0, 0, 0};

    f32 systemTime = 0.0f;
    f32 elementScale = 1.0f;
    /// `p_vSystemTime_...FlipbookMidKeyTime_FlipbookColumnCount.z`, the age at
    /// which the flipbook switches from the start run to the end run.
    f32 flipbookMidKeyTime = 0.5f;
    /// `.w`. Forced to 1 in the shader when it is 0 — "fixes integer overflow
    /// on c++ side", in the shipped comment.
    f32 flipbookColumns = 1.0f;
    /// `(startInit, startStop, endInit)`. The authored quad's fourth key is
    /// never uploaded, so it is dead on this path.
    std::array<f32, 3> flipbookFrames{0, 0, 0};
    std::array<f32, 2> cellSize{1.0f, 1.0f};

    /// Row-major, HLSL association — see `vs::MulPointMat4`.
    std::array<f32, 16> prWorld = sc2::kIdentityMat16;
    std::array<f32, 16> instanceTransform = sc2::kIdentityMat16;
};

struct Sc2QuadCamera {
    Vector3f billboardRight{1, 0, 0};
    Vector3f billboardUp{0, 0, 1};
    Vector3f direction{0, 1, 0};
    Vector3f eye{0, 0, 0};
};

struct Sc2QuadFlags {
    u32 instanceType = 0;
    /// The three `b_*Interpolation` modes, one per channel.
    i32 sizeInterp = 0;
    i32 colorInterp = 0;
    i32 rotationInterp = 0;
    bool localSpace = false;
    bool modelInstancing = false;
    bool proceduralPosition = false;
    /// `b_fixedTailLength` — read twice, meaning two things: in the quad it
    /// drops the speed term, in the procedural step it picks the clamp's
    /// budget (RE §16.13).
    bool fixedTailLength = false;
    /// `b_clampedTailLength` — the procedural step's `min` with no side
    /// effect; the CPU builder's 10000 latch is not on this path.
    bool clampedTailLength = false;
    bool randomFlipbookStart = false;
    /// `b_iUVMapping[slot] == PARTICLE_FLIPBOOK`.
    bool flipbookUv = false;
    /// `b_UVRandomOffsetEnable[slot]`, which the flipbook arm outranks.
    bool uvRandomOffset = false;
};

/// `Particle.fx`'s `Input` as the vertex DECLARATION hands it over, which is
/// not quite @ref Sc2GpuVertex: the hardware has already widened the u16 pairs
/// and normalised the UBYTE4 colours. The u32→float4 colour decode belongs to
/// the declaration and OP12 does not measure it, so it is not done here — the
/// caller supplies the three nodes already unpacked, exactly as the gate's
/// fixture does.
struct Sc2QuadInput {
    Vector3f position{0, 0, 0};
    /// The RAW u16s; the shader scales by 1/256 itself.
    std::array<f32, 4> size{256, 256, 256, 256};
    /// `cColor0/1/2`, RGBA in 0..1.
    std::array<std::array<f32, 4>, 3> color{};
    /// The raw u16 triple (the shader scales by 1/32) plus `flipbookRand`
    /// as `.w`.
    std::array<f32, 4> rotation{0, 0, 0, 0};
    f32 birthTime = 0.0f;
    f32 deathTime = 1.0f;
    f32 drag = 0.01f;
    f32 invDrag = 100.0f;
    /// `vInterpolator1` — the velocity and `invMass`.
    Vector3f velocity{0, 0, 0};
    f32 invMass = 1.0f;
    /// `vInterpolator2` — the instance vector and the gravity the shader
    /// negates on the way in.
    Vector3f instanceVec{0, 0, 0};
    f32 gravityZ = 0.0f;
    /// `vNoiseVector` — the offset and `flipbookRandStart`.
    Vector3f noise{0, 0, 0};
    f32 flipbookRandStart = 0.0f;
};

struct Sc2QuadCorner {
    Vector3f position{0, 0, 0};
    Vector2f uv{0, 0};
    Vector3f normal{0, 0, 0};
    /// Gate-only, with `Sc2QuadResult::age`/`size`: the gate compares them and
    /// the CPU vertex stream has no lane for them.
    Vector3f tangent{0, 0, 0};
    Vector3f binormal{0, 0, 0};
};

struct Sc2QuadResult {
    f32 age = 0.0f;
    f32 size = 0.0f;
    std::array<f32, 4> color{1, 1, 1, 1};
    std::array<Sc2QuadCorner, 4> corner{};
    /// False past the eleven types the shader has. The corners are still
    /// filled — as billboards, which is the shader's own final `else` — so
    /// nothing downstream reads uninitialised memory; no shipped record
    /// carries such a type.
    bool supported = true;
};

/// One UV for one corner: the flipbook cell, or the random per-particle tile.
Vector2f Sc2ParticleUv(const Sc2QuadInput& v, const i16 (&corner)[2], f32 age,
                       const Sc2QuadBatch& b, const Sc2QuadFlags& fl);

/// The four world-space corners of one particle.
Sc2QuadResult Sc2ExpandQuad(const Sc2QuadInput& v, const Sc2QuadBatch& b,
                            const Sc2QuadCamera& cam, const Sc2QuadFlags& fl);

// ---------------------------------------------------------------------------
// The batch row (OP15)
// ---------------------------------------------------------------------------
//
// `Sc2QuadBatch` is what the vertex shader reads; this is what fills it.
// Retail's producer is `CParticleSystem::WriteInstanceConstants`, which writes
// one row of eight `p_*` constant arrays per emitter in a batch, indexed by the
// emitter's batch slot.
//
// It is a persistent row, not a value: two of its constants are deliberately
// NOT written under some configurations, and retail then draws with whatever
// the previous batch left there. @ref Sc2WriteQuadBatch keeps that shape - it
// updates a row in place rather than returning a fresh one - so the lanes
// retail leaves alone are visible as lanes this function does not assign.

/// The authored half of a batch row: the `PAR_` fields the producer reads.
struct Sc2BatchDesc {
    /// `p_vMidKeyTimes` — size, colour, alpha, rotation, in that order. The
    /// shader reads `.x/.y/.z/.w` in the same order, so the two agree
    /// independently of each other.
    f32 sizeMidTime = 0.5f;
    f32 colorMidTime = 0.5f;
    f32 alphaMidTime = 0.5f;
    f32 rotationMidTime = 0.5f;

    /// `p_vMidKeyHoldTimes`, the same four channels. Retail copies all sixteen
    /// bytes at once, which is why a fixture that varied one value could never
    /// have told the four lanes apart.
    f32 sizeMidHoldTime = 0.0f;
    f32 colorMidHoldTime = 0.0f;
    f32 alphaMidHoldTime = 0.0f;
    f32 rotationMidHoldTime = 0.0f;

    /// The three flipbook indices that reach the shader. The authored quad's
    /// fourth, `flipbookEndStopIndex`, is uploaded by nothing and read by
    /// nobody, so it is not carried here.
    u8 flipbookStartInitIndex = 0;
    u8 flipbookStartStopIndex = 0;
    u8 flipbookEndInitIndex = 0;
    f32 flipbookMidTime = 0.0f;
    /// Both must be non-zero or the whole flipbook degrades — see
    /// @ref Sc2WriteQuadBatch.
    u16 flipbookColumns = 0;
    u16 flipbookRows = 0;
    /// Authored fractions, NOT `1 / columns`. A sheet whose cells do not tile
    /// its texture exactly is expressible, and some do.
    f32 flipbookColumnFraction = 0.0f;
    f32 flipbookRowFraction = 0.0f;

    /// `additionalFlags & WorldSpace`. Its complement is the shader's
    /// `b_localSpace`.
    bool worldSpace = false;
};

/// The per-frame half: the two transforms and the emitter clock.
struct Sc2BatchFrame {
    /// The emitter's world matrix — `CElementBased::GetWorldMatrix`, which is
    /// the transform node's own, refreshed. Row-major.
    std::array<f32, 16> world = sc2::kIdentityMat16;
    /// The model-instancing node's transform, when the emitter has one.
    std::array<f32, 16> instanceTransform = sc2::kIdentityMat16;
    bool hasInstanceNode = false;
    f32 emitterTime = 0.0f;
};

/// Fill one batch row, leaving unassigned exactly what retail leaves unwritten.
///
/// Two configurations write less than the whole row:
///   * a **world-space** emitter never writes `prWorld`. Every shader read of
///     it is guarded by `b_localSpace`, which is that same bit inverted, so
///     the row it leaves behind is never sampled.
///   * a flipbook needs **both** `flipbookColumns` and `flipbookRows`
///     non-zero. Fail either and `flipbookMidKeyTime` and `flipbookColumns`
///     degrade to 1, and `flipbookFrames` and `cellSize` are not written at
///     all — so a degraded emitter drawn after a flipbook one inherits its
///     cell size.
void Sc2WriteQuadBatch(Sc2QuadBatch& row, const Sc2BatchDesc& d,
                       const Sc2BatchFrame& f);

} // namespace whiteout::flakes::renderer::particle
