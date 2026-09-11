#pragma once

// ============================================================================
// Sc2TickEmitter — one frame of an SC2 emitter, composed out of the gated
// kernels.
//
// Everything here is a join. `Sc2TickClock`, `Sc2ComputeEmitCount`,
// `Sc2SpawnSchedule`, the three attribute samplers, `Sc2SampleSpawnPosition`,
// `Sc2SampleSpawnVelocity`, `Sc2InitSpawned` and `Sc2RetireExpired` are each
// measured against a golden on their own; what nothing measures is the ORDER
// they run in, which values reach which of their inputs, and what happens
// between two of them. That is what this file is, and why it is a free
// function over plain structs rather than a method on `Emitter2`.
//
// It does NOT integrate. An analytic emitter's particles are moved by the
// vertex shader from the birth state alone (`Sc2ExpandQuad`), so the whole of
// MOVE on that path is the retirement — which is what `Sc2UseRetirePath` says
// and what `Update` does. The Euler arm is X4's.
// ============================================================================

#include "sc2_runtime.h"
#include "sc2_emitter_desc.h"
#include "renderer/sc2/sc2_rng.h"
#include "types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::particle {

/// The per-frame facts that are neither on the desc nor carried in the
/// runtime: the host's clock, the emitter's two transforms, and the quality
/// level the LOD tables index.
struct Sc2TickFrame {
    /// Milliseconds, as an INTEGER — the time scale multiplies it while it is
    /// still one, which is where a negative scale becomes a 4.29-billion-ms
    /// step rather than a small backwards one (RE §5.1).
    i32 dtMs = 0;
    i32 nowMs = 0;
    i32 frameIndex = 0;
    /// `Tick`'s own argument: it scales `dtMs` only under `stateFlags & 0x60`.
    f32 timeScale = 1.0f;
    /// The animation player's time scale, which `ComputeEmitCount` multiplies
    /// the rate by. NOT `Tick`'s argument — retail samples it out of the rate
    /// AnimRef — so a pre-roll block that overrides the one leaves the other
    /// alone.
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
    /// product of (`Sc2VertexBodyInputs::worldGravityScale`). It comes from the
    /// map catalog and a viewer has no map, so it stays 1.
    f32 worldGravityScale = 1.0f;

    Matrix44f worldMatrix = Matrix44f::identity();
    Matrix44f boneMatrix = Matrix44f::identity();
    bool hasBone = false;
    Vector3f worldPos{0, 0, 0};

    /// Renderer units per SC2 unit, in which the transforms above, every
    /// `PARC` slot's bone and the ground query arrive — the actor's world
    /// scale, 100 for an SC2 actor. The runtime runs in SC2 units, as retail's
    /// runs in its own world units: `Sc2TickEmitter` divides this out of every
    /// host-space input, and BUILD multiplies it back onto the corners. Left
    /// in, it reached the terms retail's constants assume game units for — a
    /// world-space Tail's `|v|·tail` came out 100× long, world-space gravity
    /// and kill radius 100× weak, a local emitter's noise 100× small.
    f32 hostScale = 1.0f;
};

/// @p m with the host's world scale taken off: columns x, y and z of every row
/// divided by @p hostScale, so a point it maps lands in SC2 units. The w column
/// stays, and a scale of 1 returns @p m bit for bit.
Matrix44f Sc2FromHostSpace(const Matrix44f& m, f32 hostScale);

/// What the frame did, for the caller and for the tests.
struct Sc2TickResult {
    Sc2StepPlan plan{};
    u32 spawned = 0;
    u32 retired = 0;
    /// How many the pool refused because it was full. Retail tests its ceiling
    /// per element and simply stops; the shortfall is never retried, so a
    /// non-zero here is normal for a saturated emitter and not an error.
    u32 refused = 0;
    /// Requests MOVE made of the children this frame, before any cap.
    usize childRequests = 0;
    /// 33 ms pre-roll blocks run ahead of the frame.
    u32 preRollBlocks = 0;
    usize events = 0;
};

/// Run one frame. Mutates @p rt — the clock, the pool, the carries and the
/// inbox — and draws from @p rt.rng.
Sc2TickResult Sc2TickEmitter(Sc2Runtime& rt, const Sc2EmitterDesc& d,
                             const Sc2TickFrame& f);

/// `UpdateEmitterState`'s last act (RE §16.31, §16.33): under `SimulateInit`,
/// remember @p sequence and raise the pre-roll ask when it differs from the
/// one remembered — the first resolution from −1 included. Without the flag
/// retail never writes `+0x3F0`, so nothing is remembered or asked.
void Sc2NoteActiveSequence(Sc2Runtime& rt, const Sc2EmitterDesc& d, i32 sequence);

/// The peak `EmitBurst` budgets from for @p sequence: the lifetime track in
/// the column the sequence's NUMBER names, although the columns are
/// containers — every sampler indexes with the player's own container, and
/// `EmitBurst` alone with `+0x3F0` (RE §16.33) — or the init value when the
/// track is unbound.
f32 Sc2PreRollPeakFor(const Sc2EmitterDesc& d, i32 sequence);

/// The pose one model particle holds — the join around `Sc2ModelParticlePose`.
///
/// The kernel runs in SC2 units, as the whole runtime does: @p world is the
/// emitter's transform with the host's scale already off (`Sc2FromHostSpace`),
/// and only the position is multiplied back by the actor's world scale on the
/// way out. The scale stays in SC2 units, because the child actor applies that
/// world scale itself — fed renderer units, `AlwaysSet`'s longest-row factor
/// would scale it twice. No scene has a terrain vector field, so types 5 and 6
/// see the grid's up.
Sc2ModelPose Sc2PoseModelParticle(const Sc2Runtime& rt, const Sc2EmitterDesc& d,
                                  const Matrix44f& world, i32 node);

/// Retail's unregistration of a pending model particle that died before its
/// model was built: find the entry, overwrite it with the LAST one, shrink the
/// list. Not a stable erase — one early death re-assigns the model of every
/// element behind it, and that is observable (RE §16.16).
void Sc2SwapRemovePending(Sc2PendingModels& list, const Sc2Runtime* runtime, i32 node);

/// `UpdateAnimatedParams`' Bezier pass (RE §5.4) on one frame's fresh samples.
///
/// A channel smoothed as Bezier (2) keeps, in its track, the value the curve
/// passes THROUGH at the mid time, and the curve wants its control point: the
/// sampled keys are converted before anything spawns or poses — size and its
/// random, rotation and its random, the colour and, with random colour on, its
/// random — every one against `sizeMidTime`, the channel-crossing bug retail
/// ships. Retail's constructor arms the pass for every emitter (`emitFlags`
/// 6), so a static emitter is converted once and an animated one every frame;
/// sampled fresh each frame here, that is one conversion per sample either way.
void Sc2ConvertBezierKeys(model::FrameState::ParticleFrameState::Sc2ParticleFrame& s,
                          const Sc2EmitterDesc& d);

} // namespace whiteout::flakes::renderer::particle
