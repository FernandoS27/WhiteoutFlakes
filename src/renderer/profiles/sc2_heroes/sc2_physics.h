#pragma once

// ============================================================================
// `PHRB`/`PHYJ` rigid-body simulation — the StarCraft II half of Domino's glue
// contract, beside `wow/wow_physics.h`'s WoW half.
//
// An M3 model's physics is one `PHRB` per simulated bone (a run of `PHSH`
// collision shapes each), chained by `PHYJ` joints. Unlike WoW's `.phys`, the
// body's *type* is not authored as a flat enum: `simulationType` says how it is
// created and the `dynamicState` channel says what it becomes, so the same
// chunk describes both a Marine's kinematic hit proxies and a `*DeathRagdoll`
// that collapses the moment it loads.
//
// And `dynamicState` is a channel in earnest, not a constant with an animation
// slot it never uses: 591 of the corpus's 4160 physicalised models key it, and
// on many of them — a building's rubble, a statue's debris — every body starts
// at zero and only a Death sequence turns it on. That is why this needs the
// sampler's help (`FrameState::physicsBodyDynamic`) and why the stage flips
// body types every frame rather than deciding once at build.
//
// The engine underneath is the same Snowball (../Domino), and so is the
// vocabulary — `dmWorld`, `dmBody`, the seven joint families. What differs is
// everything *around* it, because the two clients wrote their glue separately:
// SC2 steps a fixed 60 Hz substep loop rather than one clamped variable step,
// drives its kinematic bodies with different snap caps, spends `PHYJ`'s angles
// in radians where `.phys` spends degrees, and hangs its bodies off bone
// matrices that are real world transforms rather than a skinning palette.
// Recovered from a 4.8 client; `M3Physics_*` in the SC2 IDB.
//
// Compiled only under WDX_ENABLE_PHYSICS.
// ============================================================================

#include "whiteout/flakes/pose_stage.h"

#include <memory>
#include <span>
#include <vector>

namespace whiteout {
namespace m3 {
struct Model;
}
} // namespace whiteout

namespace whiteout::flakes::renderer::profiles::sc2_heroes {

/// @brief Build the rigid-body stage for @p model, or nullptr if it has none.
///
/// Returns nullptr rather than an inert stage when the model carries no `PHRB`,
/// no body that could *ever* be dynamic, or no bone a body can attach to — the
/// same rule the WoW stage uses, and for the same reason: an empty stage still
/// costs a virtual call and a claim scan every frame, per actor.
///
/// "Could ever" is load-bearing. A body whose `dynamicState` is keyed is
/// authored off and turns on mid-sequence, so asking what it is at t=0 drops
/// the stage for every model whose physics is a death effect — which is most of
/// the ones that have any.
///
/// One `snowball::Scene` per actor. StarCraft II instead runs every model in
/// one `dmWorld` and separates them with collision filters, but the filters it
/// picks say exactly what per-actor isolation gives us: a jointed model's
/// bodies take a **positive** group index (so they collide with each other),
/// and every mask excludes the dynamic category `0x808` (so they ignore other
/// models' ragdolls). The one thing the separation costs is collision against
/// the world and against other units, neither of which the viewer has.
std::unique_ptr<animation::IPoseStage>
CreateSc2PhysicsStage(const ::whiteout::m3::Model& model);

/// @brief Bone world matrix -> rigid-body frame -> bone world matrix, with no
///        simulation in between.
///
/// Exposed for the same reason the WoW stage exposes its round trip: the two
/// boundary conversions have to be exact inverses and nothing about a finished
/// frame can tell you whether they are — a transposed extraction hands every
/// body its **conjugate** rotation, which is stable, plausible, and wrong only
/// in orientation.
///
/// M3 adds a second way to get this wrong that `.phys` does not have. An M3
/// bone matrix carries **scale** (`M3ComposeLocal` multiplies each basis row by
/// it), and a quaternion extraction fed unnormalised rows returns a rotation
/// that is quietly wrong in proportion to how non-uniform that scale is. So the
/// frame conversion divides the scale out and the write-back multiplies it back
/// in — which is what StarCraft II does too, rebuilding a driven bone's basis
/// as `quat-to-matrix * (the animated scale)` and never letting the solver see
/// the scale at all.
///
/// Returns @p world unchanged when the conversions agree.
Matrix44f Sc2RoundTripBoneFrame(const Matrix44f& world);

/// @brief The forward half of that round trip: a bone's rotation and
///        translation with its scale divided out.
///
/// Shared with `sc2_cloth.cpp`, which needs the same SQT for the anchor tables
/// it hands the cloth solver every frame. A second transcription would be a
/// second chance to extract the conjugate — the one error @ref
/// Sc2RoundTripBoneFrame exists to catch — so the two spend one.
void Sc2DecomposeBone(const Matrix44f& world, Quaternion& rotation, Vector3f& translation);

/// @brief And the backward half: an SQT with no scale, as a matrix.
///
/// The exact inverse of @ref Sc2DecomposeBone, which is the only reason it is
/// here rather than written out at the one call site — the cloth overlay places
/// colliders the solver holds as `(rotation, position)` pairs, and composing
/// those with the rotation written down the columns instead of the rows gives a
/// conjugate that is stable, plausible, and mirrored.
Matrix44f Sc2ComposeBone(const Quaternion& rotation, const Vector3f& translation);

/// @brief The `PHSH` shapes as debug wireframes, plus the bone each one rides.
///
/// One entry per `PHSH` on the bodies the stage builds, in the same order —
/// which is what lets the adapter and the stage fill
/// `FrameState::collisionTransforms` interchangeably while agreeing on nothing
/// but this function.
///
/// Mesh shapes are absent, because no fixture is built for one either: drawn
/// beside real colliders they would read as something the solver knows about.
/// Bodies whose filter leaves them colliding with nothing *are* included even
/// though they get no fixtures — "this body exists and touches nothing" is
/// worth seeing, and hiding it looks like a body that failed to build.
///
/// A convex hull comes back as @ref model::CollisionShapeType::Hull carrying
/// the file's own points and `DMSE` edges, not as the box they span: SC2's
/// bodies are hulls almost exclusively, and a box around one is both much
/// larger than the collider and the same picture for every limb.
///
/// `bodyKind` is the body's type **as authored** — the opening frame's answer,
/// and no more than that, since the type is a channel. `bodyIndex` is what
/// carries the rest: the stage resolves each frame's real type into
/// `FrameState::physicsBodyDynamic`, and the overlay re-colours the shape from
/// it per actor per frame.
///
/// Geometry comes back in each shape's **own** frame, with `locals` carrying
/// that frame out to model space. A `PHSH` matrix is a full affine transform and
/// routinely carries a rotation and three unequal scales — a collider is
/// authored as a unit primitive and squashed onto the limb it wraps — so a
/// wireframe flattened into bone space has to give up either the orientation or
/// the shape. Keeping the frame gives up neither, and it is also what lets each
/// shape kind honour exactly the scale its *fixture* honours, which is not the
/// same rule for all five (see @ref Sc2BuildCollisionShapes's implementation).
struct Sc2CollisionShapes {
    std::vector<model::CollisionShapeData> shapes;
    /// @brief Shape frame -> bone space, per shape. Parallel to @ref shapes.
    std::vector<Matrix44f> locals;
    /// @brief The bone each shape rides. Parallel to @ref shapes.
    std::vector<i32> bones;
    /// @brief Whether this shape can wear the bone's scale **per axis**.
    ///        Parallel to @ref shapes. See @ref Sc2PlaceCollisionShapes.
    std::vector<::whiteout::u8> anisotropic;
};
Sc2CollisionShapes Sc2BuildCollisionShapes(const ::whiteout::m3::Model& model);

/// @brief Place a shape list from a posed skeleton, into @p out.
///
/// `local * (the frame the body was built in)`, and the bone's scale enters that
/// second half **two different ways depending on the shape**, which is why
/// @p anisotropic has to be passed alongside:
///
///  - a shape built as a *polytope* — box, cylinder, convex hull — takes the
///    bone's three axis scales verbatim, so a stretched bone stretches its
///    collider;
///  - a *sphere* or *capsule* takes the smallest of the three, splatted. Not a
///    preference: Domino has no ellipsoid and no elliptical capsule, so there is
///    no shape to place. `min` rather than `max` because that is the float
///    StarCraft II hands every fixture (`DOMINO_GLUE.md` §6.1), leaving these
///    two kinds behaving exactly as the client does.
///
/// StarCraft II collapses **all five** this way, because a `dmFixture` carries
/// one `m_scaleOrRadius` float and its polytopes are cooked offline; it recovers
/// the per-axis part by baking it into the cooked hull. Snowball builds its
/// polytopes from points we hand it, so the three that can be anisotropic are,
/// and the divergence is confined to the two kinds that cannot.
void Sc2PlaceCollisionShapes(std::span<const i32> bones, std::span<const Matrix44f> locals,
                             std::span<const ::whiteout::u8> anisotropic,
                             std::span<const Matrix44f> boneWorld,
                             std::vector<Matrix44f>& out);

} // namespace whiteout::flakes::renderer::profiles::sc2_heroes
