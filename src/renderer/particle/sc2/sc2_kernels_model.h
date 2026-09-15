#pragma once

// ============================================================================
// SC2 particle kernels — model particles: the pose `UpdateModelParticle`
// writes and the path pick the pending walk makes after MOVE. The contract is
// `particle_stages_sc2.h`'s.
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
// OUTPUT — model particles: the pose (`UpdateModelParticle`, RE §16.15, gate
// OP14) and the path pick after MOVE (`ProcessPendingSpawns`, RE §16.16, gate
// OP14b).
// ---------------------------------------------------------------------------

/// `EvalAnimCurve2D` — the lane-wise sibling of `vs::InterpolateValue`, which
/// is what `EvalAnimCurve1D` already is (OP10). Not the shader's float3
/// overload: modes 1 and 4 group differently, and mode 4 substitutes values the
/// way the scalar evaluator does rather than lerping a control point.
std::array<f32, 4> EvalCurve2D(u32 mode, const std::array<f32, 4>& k0,
                                  const std::array<f32, 4>& k1,
                                  const std::array<f32, 4>& k2, f32 t, f32 mid, f32 hold);

/// The seven quaternions `Particle_InitStaticResources` builds, `(x, y, z, w)`,
/// as OP14 recorded them from the image's own yaw/pitch/roll path. Only four
/// differ: 0 == 2, 1 == 5, 3 == 4.
extern const std::array<std::array<f32, 4>, 7> kModelOrientPresets;

struct ModelPoseInputs {
    // ---- the element ----
    Vector3f position{0, 0, 0};
    Vector3f velocity{0, 0, 0};
    Vector3f orientVec{0, 0, 0};
    Vector3f spawnOrigin{0, 0, 0};
    /// `element+0x84`: what `ProcessPendingSpawns` drew under `rotationFlags &
    /// 0x80` — the slot the quad path calls `gpuVelocity`.
    Vector3f randomDirection{0, 0, 0};
    f32 birthTime = 0.0f;
    f32 deathTime = 1.0f;
    /// The element's own keys, read only under `rotationFlags & 4`: sizes as
    /// s16/256, rotations as s16/32 — the u16 lanes reinterpreted — and three
    /// BGRA words.
    std::array<u16, 3> elementSize{};
    std::array<u16, 3> elementRotation{};
    std::array<u32, 3> elementColors{};

    // ---- the emitter ----
    /// `CElementBased_GetWorldMatrix`, row-major.
    std::array<f32, 16> world = renderer::sc2::kIdentityMat16;
    f32 emitterTime = 0.0f;
    /// `CParticleSystem+0x120`. Bit 0x80 reads a world-space type-7/8
    /// `orientVec` as one plain direction instead of a packed pair.
    u32 stateFlags = 0;
    /// The emitter's cached keys, read without `rotationFlags & 4`.
    std::array<f32, 3> sizeKeys{};
    std::array<f32, 3> rotationKeys{};
    std::array<u32, 3> colorKeys{};

    // ---- PAR_ ----
    u32 parFlags = 0;
    u32 additionalFlags = 0;
    u32 rotationFlags = 0;
    u32 instanceType = 0;
    u32 sizeSmoothing = 0;
    /// Drives the tint AND the alpha: there is no `alphaSmoothing`.
    u32 colorSmoothing = 0;
    u32 rotationSmoothing = 0;
    /// Indexed by `renderer::sc2::MidChannel`.
    std::array<f32, renderer::sc2::MidChannel::kCount> midTime{0.5f, 0.5f, 0.5f, 0.5f};
    std::array<f32, renderer::sc2::MidChannel::kCount> midHold{0.0f, 0.0f, 0.0f, 0.0f};
    Vector3f instanceAngle{0, 0, 0};
    f32 tailLength = 0.0f;
    /// `PAR_+0x5D0`'s raw bits non-zero — so a `-0.0` counts — and `+0x5D4`.
    bool legacyOrient = false;
    i32 orientVariant = 0;

    // ---- the model and the scene ----
    f32 modelAlpha = 1.0f;
    Vector3f modelTint{1, 1, 1};
    /// `sceneCtx+3024`, rows +336 / +352 / +368: the camera's right, its view
    /// direction and its up. Zero-length rows fall back to (1,0,0), (0,−1,0)
    /// and (0,0,1).
    std::array<Vector3f, 3> camera{Vector3f{1, 0, 0}, Vector3f{0, -1, 0}, Vector3f{0, 0, 1}};
    /// `SampleVectorField2D` under the particle, types 5 and 6. No field is a
    /// straight-up normal.
    bool haveTerrain = false;
    Vector3f terrainNormal{0, 0, 1};
};

/// The eight setter payloads that are maths (the last three are the model's
/// own blocks, handed over by address and not computed).
struct ModelPose {
    Vector3f position{0, 0, 0};
    Vector3f scale{1, 1, 1};
    Vector3f tint{1, 1, 1};
    f32 alpha = 1.0f;
    /// `(x, y, z, w)`.
    std::array<f32, 4> rotation{0, 0, 0, 1};
};

/// `CParticleSystem::UpdateModelParticle` — RE §16.15, gate OP14. Kept whole
/// even where the answer is useless (NaN, zero scale, discarded rotations): the
/// caller clamps. See SC2_PARTICLE_RE.md §17.6.
ModelPose ModelParticlePose(const ModelPoseInputs& in);

/// The three rows a unit quaternion from `QuaternionFromMatrix3` was built
/// from: rotating the model's +X, +Y and +Z gives them, so they are also the
/// rows of the row-vector matrix that places the child.
std::array<Vector3f, 3> QuatRows(const std::array<f32, 4>& q);

struct PendingDraw {
    u32 pathIndex = 0;
    /// Drawn under `rotationFlags & 0x80`: a point in the CUBE normalised,
    /// biased toward the eight corners. Written with `gpuInvMass = 0`.
    bool hasDirection = false;
    Vector3f direction{0, 0, 0};
};

/// One pending entry of `ProcessPendingSpawns` — RE §16.16, gate OP14b.
/// Returns false, having drawn NOTHING, for a particle already dead
/// (`deathTime < emitterTime`) or an emitter with no model paths (design §8).
/// See SC2_PARTICLE_RE.md §17.6.
bool PendingSpawnDraw(renderer::sc2::Rng& rng, f32 deathTime, f32 emitterTime, u32 pathCount,
                         bool randomDirection, PendingDraw& out);

} // namespace whiteout::flakes::renderer::particle::sc2
