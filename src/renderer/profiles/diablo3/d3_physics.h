#pragma once

// ============================================================================
// Diablo III rigid bodies — the third host of Domino's glue contract, beside
// `wow/wow_physics.h` and `sc2_heroes/sc2_physics.h`.
//
// A D3 rig is assembled from three assets and none of them is a physics file:
// the geometry is `Appearances::arBones[i].arCollisionShapes`, the material and
// filter are one `.phy` per *actor* (not per body), and the joints are two more
// arrays on the appearance — a single constraint per bone joining it to its
// nearest bodied ancestor, plus an appearance-level list capped at eight.
//
// **D3 ships TWO rigs from two builders, and they share almost nothing.**
//
// *`Physics_CreateActorBoneBodies` (`0x71003DFFF0`) — the collapse.* One body
// per bone with shapes, **Static or Dynamic and never kinematic**: the builder
// writes one flag that `PhysicsBridge_CreateBody` turns into Domino type
// **2 (Static)** or **0 (Dynamic)**, set from the shapes — Dynamic when a
// LOD-matching shape carries a positive `flScaleX`. Its one call site
// (`sub_710021BAF0`) destroys whatever physics the actor had and calls
// `Anim_ResetPlaybackState` / `AnimTree_StopLayerAnim` in the same breath, so
// the actor **stops animating** and a static body holding its spawn transform
// is the whole of "this bone no longer moves".
//
// *`sub_71003E07B0` — the live anchored rig.* Called from
// `ActorAnim_InitAnimTree`, so it exists from load. Every body is created
// Dynamic and then the bones carrying a `dwFlags` bit-0 shape are turned
// **kinematic** by `dmBody_SetType(body, 1)`. Those are the *anchors*; every
// other bodied bone is dynamic, joined to its nearest bodied ancestor by that
// bone's `arConstraints[0]`, and bodied only if the ancestor is itself dynamic
// or the constraint sets `dwFlags` bit 4. A chandelier, a barrel stack, a soul
// grinder: rigid parts nailed to the animation with loose parts hanging off.
//
// So bit 0 marks an **anchor**, not a ragdoll member — the exact inverse of
// what this plan assumed before the second builder was disassembled, and the
// reason `D3RigMode::Ragdoll` produces the *live* rig rather than a collapse.
//
// *The LOD index picks which collapse you get*, and the corpus splits cleanly:
//
//     LOD 0   34,082 shapes on 2,460 models   the full collider set; 2,367 of
//                                             those have a dynamic bone, and
//                                             they are barricades, statues,
//                                             carts, scaffolding — breakables
//     LOD 1    2,779 shapes on   570 models   a coarse 7-10 shape per-limb
//                                             proxy, and *every one of the 570*
//                                             has a dynamic bone. Skeletons,
//                                             crypt children, `PVP_melee` —
//                                             these are the CHARACTER ragdolls
//
// Every model carrying LOD-1 shapes also carries LOD-0 ones, and the client
// passes **1** at the one call site. So LOD 1 is the death rig and LOD 0 is
// what the collision overlay draws — and what the anchored rig above collides
// with, since `sub_71003E07B0` leaves the descriptor's lod at zero.
//
// **The kinematic drive is a documented divergence.** D3's own influence on a
// live rig is `sub_71003E1390` (from `ActorAnim_EvaluatePoseAndSkin`): it
// *adds* a velocity toward where the animation moved the bone, scaled by a
// per-body gain that is the mean `flScaleZ` over the bone's LOD-matching
// shapes, and it is armed by a one-shot flag (`model+12420`) whose source is
// per-clip gameplay data no model file carries. Unarmable from a model, so the
// anchors here are driven the way both other hosts drive theirs — position and
// velocity from the animated bone — which leaves the anchors exactly where the
// animation puts them and lets the hanging parts swing. See
// D3_PHYSICS_PLAN.md §7.
//
// Compiled only under WDX_ENABLE_PHYSICS.
// ============================================================================

#include "renderer/profiles/diablo3/d3_collision.h"
#include "whiteout/flakes/pose_stage.h"

#include "snowball/joint.h"
#include "snowball/scene.h"

#include <whiteout/sno/d3/native/d3_native.h>

#include <memory>
#include <span>

namespace whiteout::flakes::renderer::profiles::diablo3 {

namespace d3n = ::whiteout::sno::d3::native;

/// @brief Which of D3's two builders the stage reproduces.
enum class D3RigMode {
    /// `Physics_CreateActorBoneBodies`. Every bodied bone Static or Dynamic,
    /// no joints from the bone list unless the appearance authors them, and the
    /// animation stopped. Built at @ref kD3BoneBodyLod.
    BoneBodies,
    /// `sub_71003E07B0`. `dwFlags` bit-0 bones are **kinematic anchors** driven
    /// by the animation; their bodied descendants are dynamic and jointed to
    /// them. Built at lod 0, which is where the shapes those models carry are.
    Ragdoll,
};

/// @brief The shared "collapse now" switch between a host and the stage.
///
/// D3 has no model-level "ragdoll now": the client drives it from a gameplay
/// event, so the control belongs to whatever is standing in for gameplay — the
/// viewer — and not to the renderer (`feedback_renderer_scope`). Shared rather
/// than owned by either end because `CreatePoseStages` is const and the adapter
/// that makes the stage keeps no other handle on it.
///
/// Arming seeds every body from that frame's pose and starts stepping.
/// Disarming stops the stage claiming anything, which hands the bones straight
/// back to the sampler.
struct D3PhysicsControl {
    bool simulating = false;
};

/// @brief Build the rigid-body stage for @p app, or nullptr if it has none.
///
/// Returns nullptr rather than an inert stage when the chosen builder could
/// produce no dynamic body — the rule both existing stages use, and for the
/// same reason: an empty stage still costs a virtual call and a claim scan
/// every frame, per actor. "Could" is not hedging here the way it is for
/// StarCraft II, whose body type is a channel; D3 decides once, at build, from
/// the shape list. It is a cheap pre-check of exactly the predicate the build
/// uses — `D3HasDynamicBody` for `BoneBodies`, `D3HasRagdollAnchor` for
/// `Ragdoll`, which needs an anchor before anything can hang off one.
///
/// @param appBytes the whole `.app`. Two thirds of every rig is a cooked
///        polytope held as a payload reference, so without the file most bodies
///        would come out with no fixture and be dropped. Passing an empty span
///        is legal and yields a sphere-and-capsule rig.
/// @param phy the actor's `.phy`, or null. One per *actor*, not per body: it
///        carries the friction, the restitution, the two damping constants, the
///        density class and the collision filter. Null falls back to the
///        registered defaults, which the corpus says are also the shipped modes
///        (friction 0.3 and restitution 0.0 on 64 of 74 files).
/// @param control the host's collapse switch, shared with the stage. Null gives
///        a stage that never simulates, which is only useful for testing the
///        build.
/// @param mode which builder to reproduce. See @ref D3RigMode.
/// @param lodIndex which shapes become fixtures. **-1 takes the mode's own**:
///        @ref kD3BoneBodyLod for `BoneBodies`, 0 for `Ragdoll`, which is what
///        each builder passes. Anything else overrides it, which is how the
///        gate reaches a breakable through the bone-body builder.
///
/// One `snowball::Scene` per actor. D3 runs every actor in one `dmWorld` and
/// separates them with a filter whose group index is the owning actor id and
/// whose category is 8 — so a rig's own bodies collide with each other and
/// (via `maskBits = (wCollisionMask & 0xFD9F) | 0x20`) with the world, which
/// the viewer stands in for with one static ground slab.
///
/// **The scene is not built until the stage is armed.** D3 creates this rig on
/// a gameplay event and throws away whatever physics object the actor had, so
/// building at load would both cost every one of the 2,367 breakables a scene
/// nobody looks at and drop them on the floor the moment they appear.
std::unique_ptr<animation::IPoseStage>
CreateD3PhysicsStage(const d3n::Appearances& app, std::span<const ::whiteout::u8> appBytes,
                     const d3n::Physics* phy, std::shared_ptr<D3PhysicsControl> control,
                     D3RigMode mode = D3RigMode::BoneBodies, i32 lodIndex = -1);

/// @brief `ConstraintParameters` -> `snowball::JointDef`. False builds no joint.
///
/// Exposed because it is the highest-risk mapping in the profile and none of it
/// is visible in a settled pose: a frame pre-rotated by the wrong quaternion
/// gives a hinge about the wrong axis, which still hangs, still collides and
/// still settles. Six independent things have to be right at once —
///
///  - **the frames are `tFrameB` and `tFrameC`**, not `tFrameA`, which is read
///    by nothing in the joint path;
///  - **revolute and shoulder frames are pre-rotated** by `(±½,±½,±½,½)`, a
///    120-degree rotation about `(1,1,1)/√3` — a cyclic axis permutation — and
///    the shoulder takes the *conjugate* of the revolute's;
///  - spherical and weld take their frames raw;
///  - the revolute's twist limit is `dwFlags` **bit 1** and the shoulder's is
///    unconditional;
///  - `collideConnected` is `dwFlags` bit 0;
///  - **type 4 builds nothing**, and it is 4,475 of the corpus's 14,570.
///
/// @return false when @p c names a type with no case in the client's switch,
///         in which case @p def is untouched.
bool D3MakeJointDef(const d3n::ConstraintParameters& c, ::snowball::BodyId bodyA,
                    ::snowball::BodyId bodyB, ::snowball::JointDef& def);

/// @brief Bone world matrix -> rigid-body frame -> bone world matrix, with no
///        simulation in between.
///
/// Exposed for the same reason the other two stages expose their round trips:
/// the two boundary conversions have to be exact inverses and nothing about a
/// finished frame can tell you whether they are — a transposed extraction hands
/// every body its **conjugate** rotation, which is stable, plausible, and wrong
/// only in orientation.
///
/// D3's own trap is milder than StarCraft II's and still present: a bone matrix
/// carries scale, but a *uniform* one (`PRSTransform::flScale` is a single
/// float and `Skeleton_ComposeWorldPose` multiplies the parent's into the
/// child's), so an extraction fed unnormalised rows returns a rotation that is
/// right and a quaternion that is not unit — which the solver then normalises
/// silently, hiding the mistake until a scaled model's collider is the wrong
/// size.
///
/// Returns @p world unchanged when the conversions agree.
Matrix44f D3RoundTripBoneFrame(const Matrix44f& world);

} // namespace whiteout::flakes::renderer::profiles::diablo3
