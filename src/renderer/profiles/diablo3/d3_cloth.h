#pragma once

// ============================================================================
// Diablo III cloth — a solver of its own, deliberately not `snowball::Cloth`.
//
// Snowball is the re-implementation of **Domino**, and D3's cloth is not
// Domino. The 2.6.2 image ships Domino with its assert strings intact — the
// `ecatto@blizzard.com` panics, all seven joint capacity messages — and there
// is **no `dmCloth` string, symbol or panic anywhere in it**. The solver is
// `ClothSim_Step` (`0x71000276C0`), in the game's own actor module beside the
// appearance code, and it shares no vocabulary with the engine next door: no
// bodies, no fixtures, no islands, no scene. Building it on `snowball::Cloth`
// would put a second unrelated solver behind a name that is supposed to mean
// one specific engine (`reference_domino_engine`).
//
// **The geometry is baked into the `.app`, not into the `.clt`.** A cloth block
// is `SubObject::arClothData[0]` — vertices, faces with rest areas, staples,
// and two constraint sets — and the `.clt` (SNO group 11, 74 files in the
// corpus) is *only* tuning: stiffnesses, damping, gravity, the relax count.
// Cloth exists at all only when `Cloth::flMass >= 1e-6`.
//
// **Vertices are ordered pinned-first.** `ClothInstance_Init` writes the staple
// count into the solver's "first free vertex" slot and every integration loop
// starts there, so vertices `[0, stapleCount)` are never integrated, never
// collided and never given a velocity — they are written each frame from the
// skeleton by @ref D3ClothSolver::UpdateDrivingBones and nothing else. The
// corpus agrees to the unit: `flInvMass == 0` on **exactly 23,817** of 84,457
// vertices, and the total staple count is **23,817**.
//
// **A staple is three things at once**: the pin (which vertex), the skin (three
// bones and three weights, into the same palette the mesh uses), and the
// membership of a *driving bone* — an averaged rigid frame per group of
// staples. Driving bones are what `flSkinBlendRate` drags the cloth back
// toward, and the drag is a **rigid delta**, not a re-skin: last frame's
// driving frame is inverted and this frame's applied, so a cape follows the
// shoulder without being snapped to its bind shape.
//
// Two constants decide whether a port looks right, and neither can be derived:
//
//   * **The distance correction is a rational approximation.**
//     `k * (1 - 2L^2/(L^2 + d^2)) * dt * 60`, where the exact constraint is
//     `1 - L/d`. They agree to first order and diverge under stretch — exactly
//     where a cape is interesting. Transcribed as shipped.
//   * **Cloth gravity is `flGravity * 3600 - 43.2`** units/s^2. The `* 3600`
//     converts units/tick^2 at 60 ticks/s; the `-43.2` is a bare literal added
//     after it. The rigid world's `-32.2` has nothing to do with either, and no
//     one of the three may be derived from another.
//
// The capsule pass uses the **Quake reciprocal square root** (`0x5F3759DF`,
// one Newton step) rather than a hardware instruction, so that one place is
// reproducible bit-for-bit across ISAs even though the rest of the binary — an
// ARM64 build, `FRECPE`/`FRSQRTE` — is not (D3_PHYSICS_PLAN.md §10).
//
// **The timestep is not fixed, but it starts fixed and stays there.** The
// per-frame job smooths it: `dt = 0.9*dt_prev + 0.1*clamp(frameDt, 0.005,
// 1/60)`, seeded at exactly 1/60 by `ClothInstance_Init`. At 60 Hz or faster
// the clamp pins the input and the filter is a fixed point, so the shipped
// cadence *is* 1/60 — which is what `ClothInstance_Presettle` passes
// explicitly. @ref D3ClothSolver::Advance runs that filter; @ref
// D3ClothSolver::Step takes a dt directly, which is what the goldens use.
//
// ## Where the simulated vertices go (plan O5, answered)
//
// **A CPU vertex deform, not particles-as-bones.** `sub_7100024EB0`, reached
// once per actor per frame from `sub_7100024D90`, walks the sub-object's mesh
// vertices, reads each one's cloth index, and writes a **fresh 32-byte vertex
// stream** into a ring-buffered allocation on the cloth instance: position
// straight from the cloth vertex, normal packed from it as three UNORM bytes,
// every other attribute copied from the static source. The driving-bone array
// is solver *input* only — `ClothSim_BlendToSkinnedPose` is its one consumer,
// and its `{min, max, count, contiguous}` tail is a per-bone iteration
// shortcut, not an upload range. The `.app`'s per-vertex cloth index is
// likewise not a GPU attribute: the vertex declaration is stride 40, which is
// the 44-byte `FatVertex` minus exactly that field.
//
// So this stage produces @ref D3ClothOutput — model-space positions and normals
// per **mesh** vertex — and stops there. Feeding it to a dynamic vertex buffer
// needs a renderer hook that does not exist yet, and the plan calls that a
// phase of its own rather than a detail of this one.
//
// Everything here is model space. The client simulates in world space (it bakes
// the actor root into every vertex at spawn), which for a viewer with one actor
// at the origin is the same arithmetic with `actorScale` folded out; the scale
// is kept as an explicit build parameter so the three places it is *not*
// linear — inverse mass, rest area, rest length — stay faithful.
//
// Compiled only under WDX_ENABLE_PHYSICS, beside `d3_physics.h`. Nothing here
// touches Snowball; the guard is about which subsystem the user asked for, not
// about a dependency.
// ============================================================================

#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/pose_stage.h"
#include "whiteout/flakes/types.h"

#include <whiteout/sno/d3/native/d3_native.h>

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::profiles::diablo3 {

namespace d3n = ::whiteout::sno::d3::native;

/// @brief The most colliders one cloth ever sees, from the client's own
///        `if (n > 15) break` in `ClothInstance_GatherCollisionCapsules`.
inline constexpr i32 kD3ClothMaxCapsules = 16;

/// @brief `Cloth::nCollisionPlane0..3` — four slots, and the corpus fills at
///        most two (slot 0 in 16 of 74 files, slot 1 in 4, slots 2 and 3 never).
inline constexpr i32 kD3ClothMaxPlanes = 4;

/// @brief `.clt` `dwFlags` bit 1: collide against the *scene*.
///
/// A spatial query around the cloth's bounding sphere. 15 of 74 files, and a
/// viewer has no scene, so it produces no colliders here — recorded rather than
/// approximated, because standing a ground plane in for "the world" would drape
/// a cape over geometry that is not there.
inline constexpr i32 kD3ClothFlagCollideWorld = 0x2;

/// @brief `.clt` `dwFlags` bit 2: collide against the actor's own capsules.
///
/// 64 of 74 files, and the only one a model viewer can serve completely. Bit 1
/// wins where both are set — the client tests it first.
inline constexpr i32 kD3ClothFlagCollideSelf = 0x4;

/// @brief The `.clt` tuning block, resolved to what the solver actually reads.
///
/// Field for field with `ClothInstance_ApplyClothAssetParams`, which copies the
/// asset into the instance **every frame** — so a `.clt` is live tuning, not
/// load-time state, and nothing here may be pre-baked into the vertex arrays.
struct D3ClothParams {
    /// `Cloth::dwRelaxIterations`. Full steps run at spawn. 25 in 71 of 74
    /// corpus files, 50 in 2, 20 in 1.
    i32 relaxIterations = 25;
    /// `Cloth::flMass`. Below 1e-6 the client builds no cloth at all; it is
    /// otherwise used once, to rescale the baked inverse masses.
    f32 mass = 1.0f;
    /// `Cloth::flSkinBlendRate`. Per-step drag toward the driving bones'
    /// motion: 0 free, 1 rigidly attached.
    f32 skinBlendRate = 0.0f;
    /// `Cloth::flStretchStiffness0/1`, blended per constraint by its own
    /// `flStiffnessBlend`. Both clamped to [0,1] inside the step.
    f32 stretchStiffness0 = 0.0f;
    f32 stretchStiffness1 = 0.0f;
    /// `Cloth::flBendStiffness`. Applies to compression only.
    f32 bendStiffness = 0.0f;
    /// `Cloth::flDragCoefficient`. The aero pass is skipped entirely at zero.
    f32 dragCoefficient = 0.0f;
    /// **Already multiplied by 3600.** `flGravity * 60 * 60`, as the client
    /// stores it. The extra `-43.2` is applied in the step, not here.
    f32 gravity = 0.0f;
    /// `Cloth::flRootStiffness`. Divides the effective inverse mass of
    /// whichever constraint endpoint sits nearer the pinned root.
    f32 rootStiffness = 1.0f;
    /// `Cloth::flLinearDamping` / `flContactDamping`, both applied as
    /// `exp(-x/60)` per step regardless of dt.
    f32 linearDamping = 0.0f;
    f32 contactDamping = 0.0f;
    /// `Cloth::dwFlags`. See @ref kD3ClothFlagCollideWorld.
    i32 flags = 0;
    /// `Cloth::vWindVelocity`, used only when `nUseCustomWind` is set. The
    /// global wind sampler it otherwise reads is gameplay state a model file
    /// cannot supply, so this profile leaves the wind at zero there.
    Vector3f windVelocity{0.0f, 0.0f, 0.0f};
    bool useCustomWind = false;
    /// `Cloth::nCollisionPlane0..3`: 0 for none, else 1..8 naming
    /// `HP_cloth_plane_1` .. `HP_cloth_plane_8` in the appearance's hardpoints.
    i32 collisionPlane[kD3ClothMaxPlanes] = {0, 0, 0, 0};
};

/// @brief One simulated particle. The shipped 84-byte record, field for field.
struct D3ClothVertex {
    Vector3f position{0.0f, 0.0f, 0.0f};
    Vector3f normal{0.0f, 0.0f, 0.0f};
    /// **Two meanings, decided by whether the vertex is pinned.** For a free
    /// vertex this is last step's position and the velocity pass differences
    /// it. For a pinned one — index < @ref D3ClothDef::firstFreeVertex — the
    /// integrator never touches it and it stays the vertex's *rest* position,
    /// which is what the staple pass skins. The client unions them in the same
    /// twelve bytes; keeping them apart here would make the two halves of
    /// `ClothVertex+24` look like different data when they are the same slot.
    Vector3f prevPosition{0.0f, 0.0f, 0.0f};
    Vector3f velocity{0.0f, 0.0f, 0.0f};
    /// Rescaled at build by `1 / (mass * 0.031056) / scale^2`.
    f32 invMass = 0.0f;
    /// The vertex whose position *probes* the capsules. The push is applied to
    /// this vertex; only the contact normal comes from the proxy.
    i32 collisionProxy = 0;
    /// Graph distance from the pinned root. Only ever compared between the two
    /// ends of a constraint, to decide which end `flRootStiffness` weighs down.
    i32 pinDistance = 0;
    /// Index into the owning `SubObject`'s vertex array. The seed at build and
    /// the deform's destination.
    i32 meshVertex = 0;
    /// Which averaged rigid frame drags this vertex.
    i32 drivingBone = 0;
    Vector3f contactNormal{0.0f, 0.0f, 0.0f};
    /// Set by the plane pass only. Capsules push without recording a contact,
    /// so a vertex resting on a capsule gets no friction — shipped behaviour.
    i32 contactFlag = 0;
};

/// @brief `ClothFace`. `normal` is runtime-only (zero on disk in 117,537 of
///        117,537 corpus faces) and is recomputed every step.
struct D3ClothFace {
    i32 v[3] = {0, 0, 0};
    /// Scaled by `scale^2` at build. Divided by 3 in the aero pass.
    f32 restArea = 0.0f;
    Vector3f normal{0.0f, 0.0f, 0.0f};
};

/// @brief `ClothStaple` — a pin, a three-bone skin, and a driving-bone vote.
struct D3ClothStaple {
    i32 vertex = 0;
    /// Palette indices. A zero weight rewrites its bone to -1, tail first, and
    /// -1 ends the list.
    i32 bone[3] = {-1, -1, -1};
    f32 weight[3] = {0.0f, 0.0f, 0.0f};
};

/// @brief One distance constraint. `weight0`/`weight1` are zero on disk and
///        computed at build from the two endpoints' inverse masses.
struct D3ClothConstraint {
    i32 v0 = 0;
    i32 v1 = 0;
    /// Scaled by `scale^2` at build — it is a squared length.
    f32 restLengthSq = 0.0f;
    f32 weight0 = 0.0f;
    f32 weight1 = 0.0f;
    /// Lerps `stretchStiffness0` -> `stretchStiffness1`. Unused by the bend
    /// set, which takes `bendStiffness` flat.
    f32 stiffnessBlend = 0.0f;
};

/// @brief A collider, already in the space the cloth simulates in.
struct D3ClothCapsule {
    Vector3f p0{0.0f, 0.0f, 0.0f};
    Vector3f p1{0.0f, 0.0f, 0.0f};
    f32 radius = 0.0f;
};

/// @brief `dot(p, normal) + d >= 0.01` is what the solver keeps.
struct D3ClothPlane {
    Vector3f normal{0.0f, 0.0f, 1.0f};
    f32 d = 0.0f;
};

/// @brief An averaged rigid frame over one group of staples.
///
/// Not authored: `ClothStructure::dwDrivingBoneCount` says how many, and which
/// group a staple joins comes from its vertex's `nDrivingBone`. The two frames
/// are this step's and last step's, and their *difference* is what the skin
/// blend applies.
struct D3ClothDrivingBone {
    Vector4f rotation{0.0f, 0.0f, 0.0f, 1.0f};
    Vector3f translation{0.0f, 0.0f, 0.0f};
    f32 scale = 1.0f;
    Vector4f prevRotation{0.0f, 0.0f, 0.0f, 1.0f};
    Vector3f prevTranslation{0.0f, 0.0f, 0.0f};
    f32 prevScale = 1.0f;
    /// Largest squared distance any of this bone's staples moved this step.
    /// Past 1.0 the blend is forced to 1 — the "the model teleported" test.
    f32 maxDisplacementSq = 0.0f;
    /// How many staples average into this frame. Zero means the bone is not
    /// driven and every pass skips it.
    i32 contributors = 0;
    /// Vertices in `[minVertex, maxVertex]` may belong to this bone; when
    /// `contiguous` they all do and the walk skips its per-vertex
    /// `drivingBone` test. An iteration shortcut and nothing else — and not the
    /// common case: measured over the corpus, 467 of the 2,216 driving bones
    /// that own free vertices are contiguous, so the filtered walk is the
    /// normal path.
    i32 minVertex = 0;
    i32 maxVertex = -1;
    i32 vertexCount = 0;
    bool contiguous = false;
};

/// @brief Everything one cloth block needs, with nothing resolved per frame.
///
/// Buildable without a `.app` — which is the point: the plan's PH4 gate is a
/// pinned strip stepped N times against a golden, and it never opens a file.
struct D3ClothDef {
    D3ClothParams params;
    std::vector<D3ClothVertex> vertices;
    std::vector<D3ClothFace> faces;
    std::vector<D3ClothStaple> staples;
    /// `arStretchConstraints`. Applied twice per step, either side of the bend
    /// set.
    std::vector<D3ClothConstraint> stretch;
    /// `arBendConstraints`. **Compression only** — the correction is applied
    /// solely when the term comes out negative.
    std::vector<D3ClothConstraint> bend;
    std::vector<D3ClothDrivingBone> drivingBones;
    /// `== staples.size()`. Every integration loop starts here.
    i32 firstFreeVertex = 0;
    /// Per **mesh** vertex: which cloth vertex drives it, or -1. Built from
    /// `FatVertex::dwClothVertexIndex`, which is the runtime's own u16 table.
    std::vector<i32> meshToCloth;
    /// The mesh normal of each staple's vertex, parallel to @ref staples.
    ///
    /// Restored into the pinned vertex **every frame** before the staple skin
    /// runs — `ClothInstance_ApplyClothAssetParams` does it in its tail loop —
    /// because the staple pass rotates the rest normal rather than
    /// accumulating a fresh one, and a pinned vertex is never in a face's
    /// normal sum.
    std::vector<Vector3f> stapleNormals;
    /// `ClothStructure::flExternalForceScale`, carried but unused: its consumer
    /// is `ClothInstance_ApplyExternalForce`, a gameplay impulse.
    f32 externalForceScale = 0.0f;
    /// The actor scale the build folded in.
    f32 scale = 1.0f;
};

/// @brief The deformed mesh for one cloth block, in model space.
///
/// One entry per **mesh** vertex of the sub-object, which is what a vertex
/// deform would upload — see the header note on O5. Vertices the cloth does not
/// drive keep their authored position and normal.
struct D3ClothDeform {
    /// Emitted geoset index, so a consumer can find the draw this belongs to.
    i32 geoset = -1;
    std::vector<Vector3f> positions;
    std::vector<Vector3f> normals;
};

/// @brief The stage's published result, shared with whoever draws it.
///
/// Same shape as `D3PhysicsControl`: the stage is built inside a `const`
/// `CreatePoseStages`, so the seam has to be a shared object rather than an
/// accessor on the stage. Unlike `D3PhysicsControl` it is *per actor*, never
/// per drawable — see @ref CreateD3ClothStage.
struct D3ClothOutput {
    std::vector<D3ClothDeform> pieces;
    /// Bumped every time a step writes @ref pieces. A consumer that has not
    /// seen a new number has nothing to upload.
    u64 revision = 0;
};

/// @brief `ClothSim_Step` and the two passes that bracket it.
///
/// Owns its vertices; the def is consumed. Colliders are set from outside
/// because both of their sources — the actor's capsules and the hardpoint
/// planes — are per-frame skeleton products, and the client rebuilds them every
/// frame for exactly that reason.
class D3ClothSolver {
public:
    explicit D3ClothSolver(D3ClothDef def);

    /// @brief One solver step at @p dt. The whole of `ClothSim_Step`.
    ///
    /// Order is load-bearing and is not the order a PBD paper would use: skin
    /// blend, aero, integrate, stretch, bend, stretch, capsules, planes,
    /// velocity-from-positions, normals.
    void Step(f32 dt);

    /// @brief Step once at the client's own smoothed timestep.
    ///
    /// `dt <- 0.9*dt + 0.1*clamp(frameDt, 0.005, 1/60)`, seeded at 1/60. At 60
    /// Hz or better this is 1/60 exactly and the filter never leaves it.
    void Advance(f32 frameDt);

    /// @brief `ClothInstance_UpdateDrivingBones`: skin the staples, average the
    ///        driving frames.
    ///
    /// The driving frames are the *same* transforms, averaged — the client
    /// reads one palette entry and feeds it to both, which is why a driving
    /// bone's frame is a skinning frame and not a bone pose. The capsules and
    /// planes are the other half of that 96-byte entry and take `boneWorld`.
    ///
    /// @param skin one matrix per palette node — `inverseBind * boneWorld`, the
    ///        product the renderer skins with. A staple's bone indices are
    ///        palette indices, so this is the array they index.
    void UpdateDrivingBones(std::span<const Matrix44f> skin);

    /// @brief `ClothInstance_Presettle`: @ref D3ClothParams::relaxIterations
    ///        full steps at 1/60, so the cloth enters hanging rather than in
    ///        its authored rest shape.
    void Presettle();

    /// @brief Force the next step's skin blend to 1 — the client's one-shot
    ///        "this actor moved, do not drag the cloth across the map" flag.
    void RequestSnap() {
        snapRequested_ = true;
    }

    void SetCapsules(std::vector<D3ClothCapsule> capsules);
    void SetPlanes(std::vector<D3ClothPlane> planes);

    const D3ClothDef& Def() const {
        return def_;
    }
    std::span<const D3ClothVertex> Vertices() const {
        return def_.vertices;
    }
    /// @brief Bounding sphere of the free vertices, as `ClothSim_ComputeBounds`
    ///        leaves it. The capsule gather culls against this.
    Vector3f BoundsCentre() const {
        return centre_;
    }
    f32 BoundsRadius() const {
        return radius_;
    }

    /// @brief Write the deformed mesh vertices into @p out.
    ///
    /// `sub_7100024EB0` without the packing: position straight from the cloth
    /// vertex, normal from it as a float rather than three UNORM bytes.
    /// @param restPositions the authored mesh positions, for vertices the cloth
    ///        does not drive.
    void PublishDeform(std::span<const Vector3f> restPositions,
                       std::span<const Vector3f> restNormals, D3ClothDeform& out) const;

private:
    void BlendToSkinnedPose(f32 dt);
    void Aero(f32 dt);
    void Integrate(f32 dt);
    void SolveDistance(std::span<D3ClothConstraint> set, f32 kA, f32 kB, f32 dt,
                       bool compressionOnly);
    void CollideCapsules();
    void CollidePlanes();
    void DeriveVelocities(f32 dt);
    void RecomputeNormals();
    void ComputeBounds();

    D3ClothDef def_;
    std::vector<D3ClothCapsule> capsules_;
    std::vector<D3ClothPlane> planes_;
    Vector3f centre_{0.0f, 0.0f, 0.0f};
    f32 radius_ = 0.0f;
    f32 smoothedDt_ = 1.0f / 60.0f;
    bool snapRequested_ = true;
    /// `snapRequested_` as the last @ref UpdateDrivingBones latched it, and
    /// held until the *next* one — @ref BlendToSkinnedPose only reads it. The
    /// client keeps the request and the latch in two different words for the
    /// same reason: the request is raised from outside at any time, and the
    /// latch governs every step until the driving bones are next rebuilt,
    /// which is what makes the presettle's `relaxIterations` steps a
    /// relaxation toward the skinned pose rather than a free fall away from
    /// it.
    bool snapLatched_ = false;
    /// True when the last @ref BlendToSkinnedPose forced a full snap; the aero
    /// pass is skipped on such a step.
    bool snapped_ = false;
};

/// @brief Build the cloth on @p subObject, or nullopt if it carries none.
///
/// @param sub the sub-object. `arClothData` empty, or a zero vertex count, is
///        the "no cloth here" answer and is by far the common case.
/// @param clt the `.clt`, or null. Null gives the registered defaults, which
///        the corpus says are also the shipped values for most files — but it
///        does **not** substitute for the mass gate: without a `.clt` there is
///        no `flMass`, and the client would not have built the cloth at all, so
///        null returns nullopt.
/// @param scale the actor scale. Folded into inverse mass (`/scale^2`), rest
///        area (`*scale^2`) and rest length (`*scale^2`).
std::optional<D3ClothDef> D3BuildCloth(const d3n::SubObject& sub, const d3n::Cloth* clt,
                                       f32 scale = 1.0f);

/// @brief The actor's own collision capsules, posed and culled.
///
/// `ClothInstance_GatherCollisionCapsules`' `kD3ClothFlagCollideSelf` branch:
/// walk `Appearances::arCollisionCapsules`, keep the ones whose bounding sphere
/// meets the cloth's, stop at @ref kD3ClothMaxCapsules.
///
/// A `CollisionCapsule` is a radius, a length and a hardpoint (a bone plus a
/// local rotation and offset). The segment is **centred** on the hardpoint
/// origin and runs along its **local +Z**, half-length `0.5 * flLength`, and
/// the client scales both it and the radius by the *actor* scale only — never
/// by the bone's. All four of those are worth stating because a wrong axis
/// produces a capsule of the right size in the right place pointing the wrong
/// way, and a cape draped over it still looks like cloth.
///
/// Its cull sphere is the client's: centre at the hardpoint origin, radius
/// `flRadius + flLength/2`, tested against the cloth's own bounds.
std::vector<D3ClothCapsule> D3GatherClothCapsules(const d3n::Appearances& app,
                                                  std::span<const Matrix44f> boneWorld,
                                                  const Vector3f& centre, f32 radius);

/// @brief The `HP_cloth_plane_N` planes named by @p params, posed.
///
/// Each non-zero slot names a hardpoint by index (1 -> `HP_cloth_plane_1`); the
/// hardpoint is composed with its bone and emitted as `(n, -dot(n, p))` with
/// `n` the frame's **local +X** — a different axis from the capsule's, from a
/// different constant, and the pair is exactly the kind of thing that reads
/// like a typo until both are read out of the image. Missing names are
/// skipped, which is what the
/// client does — a `.clt` naming a plane the appearance does not carry simply
/// gets no plane.
std::vector<D3ClothPlane> D3BuildClothPlanes(const d3n::Appearances& app,
                                             const D3ClothParams& params,
                                             std::span<const Matrix44f> boneWorld);

/// @brief `.clt` -> @ref D3ClothParams, with the `* 3600` on gravity applied.
D3ClothParams D3ClothParamsOf(const d3n::Cloth& clt);

/// @brief One cloth block found on an appearance.
struct D3ClothPiece {
    /// Index into `tGeoSet0.arSubObjects` (geoset 0) — the only geoset the
    /// corpus ever puts cloth on.
    i32 subObject = -1;
    /// The emitted geoset index the adapter draws it as, or -1 if the caller
    /// did not supply a mapping.
    i32 geoset = -1;
    /// `SubObjectAppearance::snoCloth` for the look that was resolved.
    i32 clothSno = -1;
};

/// @brief Every sub-object of @p app that carries cloth, with its `.clt` id.
///
/// The id comes from the *look's* `SubObjectAppearance`, joined the way
/// `D3VariantFor` joins it — so two looks of one appearance can dress the same
/// mesh in two different cloths, which is why this takes a look index rather
/// than reading slot 0.
std::vector<D3ClothPiece> D3FindClothPieces(const d3n::Appearances& app, u32 lookIndex = 0);

/// @brief Build the cloth stage for @p app, or nullptr if it has none.
///
/// Appended **after** the rigid stage: a cape anchored to a bone the ragdoll
/// drives has to read where the ragdoll put it, not where the animation did.
///
/// @param pieces what @ref D3FindClothPieces returned, with `geoset` filled in
///        by the caller (only the adapter knows the emitted-geoset mapping).
/// @param cloths one `.clt` per entry of @p pieces, parallel; null entries are
///        skipped, since without `flMass` the client builds nothing.
/// @param output where each step publishes its deformed vertices. **One per
///        stage** — `FrameState::geosetDeforms` hands the renderer spans into
///        it, so two stages sharing one buffer make every actor but the last to
///        step draw somebody else's cloth. May be null, which runs the
///        simulation and throws the result away — useful only for measuring.
std::unique_ptr<animation::IPoseStage>
CreateD3ClothStage(const d3n::Appearances& app, std::span<const D3ClothPiece> pieces,
                   std::span<const std::shared_ptr<const d3n::Cloth>> cloths,
                   std::shared_ptr<D3ClothOutput> output, f32 scale = 1.0f);

/// @brief The debug-overlay description of the same cloths, in the same order.
///
/// Takes the identical arguments and applies the identical filter as
/// @ref CreateD3ClothStage — a piece only appears here if `D3BuildCloth`
/// accepted it there — because the overlay index is what the stage's
/// `FrameState::clothParticles` entry is keyed by. Keeping the two loops in one
/// file is the whole reason this lives here rather than in the adapter.
///
/// `particleNodes` comes back **empty**: a D3 cloth vertex is not a bone, which
/// is why the stage publishes positions rather than letting the overlay gather
/// them from the palette.
std::vector<model::ClothOverlayData>
D3BuildClothOverlays(const d3n::Appearances& app, std::span<const D3ClothPiece> pieces,
                     std::span<const std::shared_ptr<const d3n::Cloth>> cloths,
                     f32 scale = 1.0f);

} // namespace whiteout::flakes::renderer::profiles::diablo3
