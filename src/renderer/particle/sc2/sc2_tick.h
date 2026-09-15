#pragma once

// ============================================================================
// TickEmitter — one frame of an SC2 emitter, composed out of the gated
// kernels: the ORDER they run in and what reaches their inputs, as a free
// function over plain structs. `UseRetirePath` picks the motion arm as `Update`
// does. See SC2_PARTICLE_DESIGN.md §16.7.
// ============================================================================

#include "renderer/particle/sc2/sc2_emitter_desc.h"
#include "renderer/particle/sc2/sc2_runtime.h"
#include "renderer/sc2/sc2_rng.h"
#include "types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::particle::sc2 {

/// The per-frame facts that are neither on the desc nor carried in the
/// runtime: the host's clock, the emitter's two transforms, and the quality
/// level the LOD tables index.
struct TickFrame {
    /// Milliseconds, as an INTEGER — the time scale multiplies it while it is
    /// still one, which is where a negative scale becomes a 4.29-billion-ms
    /// step rather than a small backwards one (RE §5.1).
    i32 dtMs = 0;
    i32 nowMs = 0;
    i32 frameIndex = 0;
    /// `Tick`'s own argument: it scales `dtMs` only under `stateFlags & 0x60`.
    f32 timeScale = 1.0f;
    /// The animation player's time scale, which `ComputeEmitCount` multiplies
    /// the rate by; NOT `Tick`'s argument. Never set, so the rate ignores the
    /// player's speed (design §8). See SC2_PARTICLE_DESIGN.md §16.7.
    f32 playerTimeScale = 1.0f;
    /// The host's own emission multiplier, folded into the sampled rate before
    /// `ComputeEmitCount` sees it — it is a viewer control, not a `PAR_` field.
    f32 emissionScaler = 1.0f;
    /// A paused model takes the full-step path.
    bool modelPaused = false;
    /// The global quality level the `lodCut`/`lodReduce` rows index. 4 at
    /// viewer settings.
    i32 quality = 4;
    /// The game's emission scale (`Model_Set*EmissionScale`): `ComputeEmitCount`
    /// multiplies the count by it and the spawn shapes their extents. Not the
    /// world scale, which the world matrix already carries — a host leaves it 1.
    f32 elemScaleX = 1.0f;
    /// The map's gravity multiplier, which the uploaded `gravityZ` lane is the
    /// product of (`VertexBodyInputs::worldGravityScale`). It comes from the
    /// map catalog and a viewer has no map, so it stays 1.
    f32 worldGravityScale = 1.0f;

    Matrix44f worldMatrix = Matrix44f::identity();
    Matrix44f boneMatrix = Matrix44f::identity();
    Vector3f worldPos{0, 0, 0};

    /// Renderer units per SC2 unit (100 for an SC2 actor), in which the
    /// transforms, `PARC` bones and ground query arrive. `TickEmitter` divides
    /// it out of every host-space input and BUILD multiplies it back onto the
    /// corners. See SC2_PARTICLE_DESIGN.md §16.7.
    f32 hostScale = 1.0f;

    /// The ground MOVE collides with and the mesh shape 7 is born on — the
    /// emitter's own surface, handed over per frame like the transforms. Null
    /// collides with nothing and gives a Mesh emitter no surface.
    const EmitSurface* surface = nullptr;
};

/// Renderer units per SC2 unit, as every reader takes it: a scale that is not
/// positive — zero, negative, NaN — is no scale at all.
inline f32 HostScale(f32 hostScale) {
    return hostScale > 0.0f ? hostScale : 1.0f;
}

/// @p m with the host's world scale taken off: columns x, y and z of every row
/// divided by `HostScale(hostScale)`, so a point it maps lands in SC2 units.
/// The w column stays, and a scale of 1 returns @p m bit for bit.
Matrix44f FromHostSpace(const Matrix44f& m, f32 hostScale);

/// What the frame did, for the caller and for the tests.
struct TickResult {
    StepPlan plan{};
    u32 spawned = 0;
    u32 retired = 0;
    /// How many the pool refused because it was full. Retail tests its ceiling
    /// per element and simply stops; the shortfall is never retried, so a
    /// non-zero here is normal for a saturated emitter and not an error.
    u32 refused = 0;
    /// 33 ms pre-roll blocks run ahead of the frame.
    u32 preRollBlocks = 0;
};

/// Run one frame. Mutates @p rt — the clock, the pool, the carries and the
/// inbox — and draws from @p rt.rng.
TickResult TickEmitter(Runtime& rt, const EmitterDesc& d,
                             const TickFrame& f);

/// `UpdateEmitterState`'s last act (RE §16.31, §16.33): under `SimulateInit`,
/// remember @p sequence and raise the pre-roll ask when it differs from the
/// one remembered — the first resolution from −1 included. Without the flag
/// retail never writes `+0x3F0`, so nothing is remembered or asked.
void NoteActiveSequence(Runtime& rt, const EmitterDesc& d, i32 sequence);

/// The peak `EmitBurst` budgets from for @p sequence: the lifetime track in
/// the column the sequence's NUMBER names, although the columns are
/// containers — every sampler indexes with the player's own container, and
/// `EmitBurst` alone with `+0x3F0` (RE §16.33) — or the init value when the
/// track is unbound.
f32 PreRollPeakFor(const EmitterDesc& d, i32 sequence);

/// The pose one model particle holds — the join around `ModelParticlePose`,
/// in SC2 units: @p world has the host's scale off (`FromHostSpace`), and only
/// the position is scaled back out. Types 5 and 6 see the grid's up.
/// See SC2_PARTICLE_DESIGN.md §16.7.
ModelPose PoseModelParticle(const Runtime& rt, const EmitterDesc& d,
                                  const Matrix44f& world, i32 node);

/// Retail's unregistration of a pending model particle that died before its
/// model was built: find the entry, overwrite it with the LAST one, shrink the
/// list. Not a stable erase — one early death re-assigns the model of every
/// element behind it, and that is observable (RE §16.16).
void SwapRemovePending(PendingModels& list, const Runtime* runtime, i32 node);

/// `UpdateAnimatedParams`' Bezier pass (RE §5.4) on one frame's fresh samples:
/// a Bezier (2) channel's keys, through-point to control point, before anything
/// spawns or poses — every channel against `sizeMidTime`, as retail ships.
/// See SC2_PARTICLE_RE.md §17.10.
void ConvertBezierKeys(model::FrameState::ParticleFrameState::Sc2ParticleFrame& s,
                          const EmitterDesc& d);

} // namespace whiteout::flakes::renderer::particle::sc2
