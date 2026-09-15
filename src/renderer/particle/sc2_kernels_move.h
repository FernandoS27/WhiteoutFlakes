#pragma once

// ============================================================================
// SC2 particle kernels — MOVE and RETIRE.
//
// The analytic step, the live list and its retirement, the Euler step with
// its collision and children, and the pick between the two paths.
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
// MOVE — the analytic variant (`Particle.fx:236`, gates OP1 + OP12 `proc`).
//
// An emitter `Sc2CanUseGpuMotion` accepts never integrates on the CPU: retail
// uploads the birth state once and `CalculatePositionAndVelocity` re-derives
// the position from the elapsed time in the vertex shader, every frame, from
// scratch. This host builds its quads on the CPU, so the same closed form runs
// here — `sc2::vs::CalculateDisplacementAndVelocity`, already O12-gated, with
// this function supplying the arguments in the shader's order.
//
// **The closed form is numerically hostile at the drag floor, which is also
// its most common setting.** OP11 measures the builder clamping drag to
// `max(0.01, drag)` and writing `invDrag = 100` under it, so every emitter
// that authors no drag lands exactly there; the displacement is then a
// difference of two terms of magnitude `mass² · gravity · invDrag²` — 5.1e6
// for the gate's fixture — whose float32 ulp is 0.5. One ulp of `exp` moves
// the answer by 12.5%. That is a property of the shipped shader, not of this
// transcription: retail's particles carry it too. It is why @ref
// Sc2StepAnalytic is a transcription of the source's operation order rather
// than of its algebra, and why the gate's tolerance on the result is derived
// from the inputs instead of being a flat relative bound.
// ---------------------------------------------------------------------------

// `Sc2InstanceType` lives in `sc2_kernel_types.h`.

struct Sc2AnalyticInputs {
    /// The element's birth position — `vPosition.xyz`, already in whatever
    /// space the batch will draw it in.
    Vector3f position{0, 0, 0};
    /// `vInterpolator1` — the birth velocity and the inverse mass.
    Vector3f velocity0{0, 0, 0};
    f32 invMass = 1.0f;
    /// `vBirthDeathAndDrag`. `invDrag` is an uploaded lane, NOT `1 / drag`:
    /// under the floor the builder writes 100 against a drag of 0.01, and the
    /// pair only stays reciprocal above it.
    f32 birthTime = 0.0f;
    f32 deathTime = 1.0f;
    f32 drag = 0.01f;
    f32 invDrag = 100.0f;
    /// `vInterpolator2.w`. Stored NEGATED — the shader passes `-gravityZ` into
    /// the closed form, so a falling particle carries a positive lane here.
    f32 gravityZ = 0.0f;
    /// `p_vSystemTime.x`, the batch's emitter clock.
    f32 systemTime = 0.0f;

    u32 instanceType = 0;
    /// `vInterpolator2.x` for the stretched types: the authored tail length.
    f32 tailLength = 0.0f;
    /// The interpolated size the clamp budgets against — `InterpolateValue`'s
    /// scalar overload at this element's age, which the caller has already.
    f32 size = 1.0f;
    bool fixedTailLength = false;
    bool clampedTailLength = false;
};

struct Sc2AnalyticStep {
    Vector3f position{0, 0, 0};
    Vector3f displacement{0, 0, 0};
    Vector3f velocity{0, 0, 0};
    /// `saturate((now − birth) / (death − birth))`, the age every sampler runs
    /// on. Returned because the caller needs the same one and recomputing it
    /// is how the two drift apart.
    f32 age = 0.0f;
    /// Seconds since birth, clamped at zero — `Bug 187607: no negative time
    /// skew`, in the source's own words. A particle whose batch clock has not
    /// reached its birth instant sits at its spawn point instead of running
    /// the trajectory backwards.
    f32 elapsed = 0.0f;
    /// `vInterpolator2.x` after the step: the possibly-shortened tail for the
    /// stretched types, and the velocity's x for a travel-direction instance,
    /// which writes the whole velocity into that register.
    f32 tailLength = 0.0f;
};

/// `Particle.fx`'s `CalculatePositionAndVelocity` — the whole MOVE for an
/// analytic emitter.
Sc2AnalyticStep Sc2StepAnalytic(const Sc2AnalyticInputs& in);

// ---------------------------------------------------------------------------
// RETIRE — `CParticleSystem::RetireExpiredParticles` (0x1029372D0, gate OP9).
//
// The analytic path's whole per-frame step. It integrates nothing: it walks
// the live list once and unlinks everything whose `deathTime` has been reached,
// returning each one's vertex-buffer slot to the recycle array.
//
// The list is intrusive and doubly linked, and both directions are load
// bearing — though not the way this comment first said. `UploadGpuParticles`
// walks FORWARD from the head by default; the backward walk from the tail, and
// the draw dispatcher's choice of tail over head, are both gated on
// `PAR_.flags & SortReverse` (0x100). So the reverse chain is the draw order
// for sort-reverse emitters and for nobody else. Retail never writes the tail
// directly — it writes `node->next->listPrev`, and the last node's `next` is
// the sentinel, whose own `listPrev` IS the tail. @ref Sc2ElementList models
// that literally rather than maintaining a tail by hand, because the literal
// version is what makes the reverse walk fall out correct at every topology.
// ---------------------------------------------------------------------------

/// Retail's `pElementBuffer` list, over indices into a caller-owned array.
///
/// `kSentinel` stands for the sentinel node; `kNull` for a genuine null link,
/// which is what terminates the backward walk.
struct Sc2ElementList {
    static constexpr i32 kSentinel = -1;
    static constexpr i32 kNull = -2;

    std::vector<i32> next;
    std::vector<i32> prev;
    i32 head = kSentinel;
    /// The SENTINEL's own `listPrev` — the last live node.
    i32 tail = kNull;
    i32 freeHead = kNull;
    i32 freeTail = kNull;
    /// `pElementBuffer + 224`, the pool's own count, separate from the
    /// system's `elementCount` even though both move together here.
    u32 poolCount = 0;

    /// Link `count` elements in index order, as a freshly filled pool is.
    void Reset(usize count);
    /// Live nodes from the head forward.
    void Walk(std::vector<i32>& out) const;
    /// Live nodes from the tail backward — `UploadGpuParticles`' order.
    void WalkBackward(std::vector<i32>& out) const;
    void WalkFree(std::vector<i32>& out) const;
    void Unlink(i32 node);
};

/// The `ParticleVB` slots retirement hands back, in the order it frees them.
///
/// Retail grows this by a fixed increment rather than doubling, and a system
/// with no array at all simply keeps the slot — which is why @ref enabled is a
/// field and not the emptiness of @ref slots.
struct Sc2RecycleArray {
    bool enabled = true;
    u32 capacity = 8;
    u32 growth = 8;
    std::vector<i32> slots;
};

/// `RetireExpiredParticles`: unlink every element at or past its death time.
///
/// Returns how many were retired. `deathTime <= emitterTime` — equality kills,
/// which the gate pins one ULP either side of.
u32 Sc2RetireExpired(Sc2ElementList& list, std::span<Sc2SpawnedElement> elements,
                     f32 emitterTime, Sc2RecycleArray& recycle);

// ---------------------------------------------------------------------------
// MOVE, Euler variant — `CParticleSystem::SimulateParticles` (gate OP9).
//
// The step an emitter takes when `CanUseGpuMotion` refused it: once per
// sub-step, integrate every live element, then run what the closed form cannot
// express — the type-6 freeze, collision with its bounce, friction and rest,
// die-on-bounce, the bounds pass and both kill tests. RE §6 is the order and
// §16.10 the measured detail.
//
// The two child queues are here: a collision spawn onto child 0 and trails
// onto child 1, returned as requests rather than pushed into another emitter,
// because nothing at this layer knows what an emitter is. The model-particle
// pose is X6's.
// ---------------------------------------------------------------------------

/// One contact for the swept segment `from → to`, in the element's own space.
struct Sc2Contact {
    bool hit = false;
    Vector3f position{0, 0, 0};
    Vector3f normal{0, 0, 1};
    /// Where along the segment. The step forces a terrain contact to 1 itself;
    /// only an object query can leave a fraction behind.
    f32 toi = 1.0f;
};

/// The scene's two collision queries.
///
/// Both answer in WORLD space. A local-space emitter's segment goes out
/// through its matrix before the call and the contact comes back through the
/// adjugate inverse inside the step (RE §16.10), so a query never has to know
/// what space an emitter simulates in.
///
/// Function pointers over a context rather than `std::function`: this runs per
/// particle per sub-step, and retail's own swept-sphere assert already shows
/// what a per-call cost there does to a busy map.
struct Sc2Collider {
    void* ctx = nullptr;
    /// `CollideParticle`, element flag 0x4. Report the RAW contact — the step
    /// pushes it out by 0.05 along the normal and overwrites its time.
    bool (*terrain)(void* ctx, const Vector3f& from, const Vector3f& to, Sc2Contact& out) = nullptr;
    /// `ForwardParticleSystemQuery`, element flag 0x8. Wins over a terrain
    /// contact outright when both bits are set.
    bool (*objects)(void* ctx, const Vector3f& from, const Vector3f& to, Sc2Contact& out) = nullptr;
};

struct Sc2SimulateInputs {
    f32 dt = 0.0f;
    /// The emitter clock the kill test and the rest re-key both read.
    f32 emitterTime = 0.0f;
    /// `(gravityX, gravityY, gravity)`, unscaled.
    Vector3f gravity{0, 0, 0};
    /// The scene's multiplier, applied only under `PAR_.flags & 0x20000`
    /// (`kSc2SceneGravity`).
    /// WhiteoutLib names that bit MultiplyGravityByMass; the runtime multiplies
    /// by THIS, and OP9 pins it with a scale of 0.25 against a mass of one.
    f32 gravityScale = 1.0f;
    /// `M3_SampleWindNoise`'s sample. The step applies the multiplier once.
    Vector3f wind{0, 0, 0};
    f32 windMultiplier = 0.0f;
    u32 parFlags = 0;
    u32 instanceType = 0;
    f32 drag = 0.0f;
    f32 bounce = 0.0f;
    f32 friction = 0.0f;
    u32 collisionDieBounce = 0;
    f32 killRadius = 0.0f;
    Vector3f origin{0, 0, 0};
    /// The scene's collision switch. Nothing is queried without it.
    bool collisionEnabled = false;
    /// The rest transition freezes the spin through this curve.
    i32 rotationSmoothing = 0;
    f32 rotationMidTime = 0.0f;
    f32 rotationMidHold = 0.0f;

    /// The emitter matrix, row-major. A local-space emitter maps the collision
    /// segment out through it and the contact back through its inverse; a
    /// world-space one uses neither.
    std::array<f32, 16> worldMatrix = sc2::kIdentityMat16;
    /// `PAR_.additionalFlags & 8`.
    bool worldSpace = false;
    /// `CParticleSystem+0x3CC`, the sampled `trailEmissionRate`.
    f32 trailRate = 0.0f;

    // ---- the children (RE §16.10) ----
    /// Child 0 exists AND simulates in world space. Retail reads the second
    /// half off the child's own `PAR_`, which is why the desc resolves it at
    /// load (`collisionChildIsWorldSpace`).
    bool collisionChild = false;
    /// Child 1 exists. Retail queues to it unguarded — a trail flag with no
    /// child dereferences null — and only the spawn's own test keeps that from
    /// happening; this is the step's guard for the same case.
    bool trailChild = false;
    f32 collisionSpawnChance = 0.0f;
    u32 collisionSpawnMin = 0;
    u32 collisionSpawnMax = 0;
    f32 collisionSpawnEnergy = 0.0f;
    /// `splatProjectorIndex != -1`. The splat needs a projector the viewer does
    /// not have; its chance draw and the death it causes are kept.
    bool splat = false;
    f32 splatChance = 0.0f;
};

struct Sc2SimulateResult {
    u32 killed = 0;
    /// Maintained only under `PAR_.flags` bit 31, and left at the empty
    /// sentinel otherwise — as retail leaves it.
    Vector3f boundsMin{sc2::kFltMax, sc2::kFltMax, sc2::kFltMax};
    Vector3f boundsMax{-sc2::kFltMax, -sc2::kFltMax, -sc2::kFltMax};
};

/// What a sub-step asked of the two children, in the order it asked.
///
/// Uncapped on purpose. `QueueSpawnRequest`'s 128 is the RECEIVER's ceiling —
/// it counts everything waiting on that child, from every parent and every
/// sub-step — so it is applied where a request lands (@ref
/// Sc2QueueSpawnRequest). The randoms that built a dropped request were drawn
/// all the same.
struct Sc2ChildRequests {
    std::vector<SpawnRequest> collision; ///< to child 0
    std::vector<SpawnRequest> trail;     ///< to child 1
};

/// Retail's cap on one emitter's pending requests (`CParticleSystem+0x358`).
inline constexpr usize kSc2MaxSpawnRequests = 128;

/// `QueueSpawnRequest` (`0x102923CD0`): append while fewer than 128 wait, and
/// drop the rest without a word — a dropped request is a particle that never
/// spawns (OP9 `cap`).
void Sc2QueueSpawnRequest(std::vector<SpawnRequest>& queue, const SpawnRequest& req);

/// One element's integration and the type-6 freeze.
///
/// Gravity (unless resting), drag in two branches — linear below `k·dt < 1`,
/// and above it the velocity simply becomes `gravity·dt` — then `v += a·dt` and
/// `p += (v + wind)·dt`. Wind reaches the POSITION only, so it never
/// accumulates into the particle's own momentum.
void Sc2StepEuler(Sc2SpawnedElement& e, const Sc2SimulateInputs& in);

/// `SimulateParticles`: one sub-step over the live list. Returns how many died
/// and appends to @p children what the step asked of them, drawing the
/// collision-spawn, splat and trail randoms from @p rng in retail's order.
///
/// @p killed, when given, receives each unlinked node in the order it died.
/// That order is observable for a ModelParticles emitter: retail unregisters a
/// pending model particle by swap-remove at the kill, so the order of deaths
/// decides which element later receives which model (RE §16.16).
Sc2SimulateResult Sc2SimulateParticles(Sc2ElementList& list,
                                       std::span<Sc2SpawnedElement> elements,
                                       const Sc2SimulateInputs& in,
                                       const Sc2Collider& collider, sc2::Rng& rng,
                                       Sc2ChildRequests& children,
                                       std::vector<i32>* killed = nullptr);

/// `Update`'s push onto the children's nodes (RE §16.10, OP9 `scale`): the
/// emitter matrix's basis ROW lengths, a collapsed row pushing 0 rather than a
/// NaN. Rows 0 and 1 go through `rsqrtps` and row 2 through an exact root,
/// each with the same Newton step, so the three lanes are not one expression.
Vector3f Sc2ChildScale(const std::array<f32, 16>& world);

/// Where that push lands (RE §16.34): on the child's BONE, as its local scale,
/// replacing the one it had. So each basis row of @p childBone — the bone's
/// local matrix times its parents' — is rescaled by `pushed / localScale`, and
/// the translation row stays. A row whose local scale is zero has no direction
/// left to rescale and is kept.
Matrix44f Sc2PushChildScale(const Matrix44f& childBone, const Vector3f& localScale,
                            const Vector3f& pushed);

/// `Update`'s choice of step function (RE §6, gate OP9 `select`).
///
/// `forceCpu` is retail's debug byte; it demotes an analytic emitter back onto
/// the Euler path, which is the only way to compare the two on one emitter.
inline bool Sc2UseRetirePath(u32 stateFlags, bool forceCpu) {
    return (stateFlags & sc2::kStateGpuMotion) != 0 && !forceCpu;
}

} // namespace whiteout::flakes::renderer::particle
