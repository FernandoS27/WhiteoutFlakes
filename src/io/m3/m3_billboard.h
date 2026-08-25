#pragma once

/// @file m3_billboard.h
/// @brief StarCraft II's `BBSC` billboard behaviours.
///
/// **Where the engine runs these.** Not from the solver update: `CBBSolver`
/// inherits `M3Solver_Solve_Base_NoOp`, so the object built from `BBSC` is
/// never consulted there (SC2_ANIM_RE §4b). The apply lives in the draw-prep
/// pass `sub_10290A890`, which walks the asset's `BBSC` array, calls
/// `CBBSolver::ApplyBillboard(camera)` (`0x1027F1F30`) per record against the
/// camera on top of the scene's camera stack, and re-runs
/// `M3Anim_BuildSkinPalette`. So a billboard is resolved **per view**, after
/// the skeleton and after the IK/turret solvers, and it writes the bone's
/// *local rotation* — which means the dirty-bit re-resolve carries the whole
/// subtree with it.
///
/// We apply it inside the bone walk instead, for the same reason
/// `M2ModelAdapter` does: our matrices are built once per evaluate and a
/// billboarded bone has to carry its children.
///
/// **What the bone flags say: nothing.** `BoneFlag::Billboard1` (0x10) and
/// `Billboard2` (0x40) are set on **zero** bones across the 51469 `.m3` files
/// of the SC2 + HotS corpora. `BBSC` is the only source there has ever been.
///
/// **What the corpus asks for**, over 10525 records in 4735 files: type 6
/// (free) 9233, type 2 (world Z) 1116, type 1 72, type 4 54, type 0 21, type 5
/// 18, type 3 11. `cameraLookAt` is genuinely mixed (5088 on / 4145 off for
/// type 6 alone), the `U16_` dependents list is empty in every single record,
/// and every billboarded bone carries a keyed rotation track.

#include "whiteout/models/m3/structures/misc.h"
#include "whiteout/vector_types.h"

namespace whiteout::flakes::io {

/// @brief The rendering camera as a billboard sees it, in the model's space.
///
/// StarCraft II reads a 4x4 off the camera object whose rows are its basis —
/// `+336` X, `+352` forward, `+368` up, `+384` position — and does all of the
/// billboard maths in world space. Ours is a model-space bone palette, so the
/// camera is brought the other way instead. The two agree as long as the
/// actor's world transform is a rotation plus a uniform scale; a non-uniform
/// one would skew the basis, and there is nothing sensible to do about that
/// (the same caveat `M2CameraBasis` carries).
struct M3CameraFrame {
    /// @brief Camera position. Only read when `cameraLookAt` is set.
    Vector3f position = {0.0f, 0.0f, 0.0f};
    /// @brief The camera's X axis. Only a degenerate-direction fallback, and
    ///        only for type 0.
    Vector3f axisX = {1.0f, 0.0f, 0.0f};
    /// @brief Where the camera looks. The aim direction when `cameraLookAt` is
    ///        clear, which makes every such bone share one orientation.
    Vector3f forward = {0.0f, 1.0f, 0.0f};
    /// @brief The camera's up axis: the roll hint for type 6, and the
    ///        degenerate-direction fallback for types 1 and 2.
    Vector3f up = {0.0f, 0.0f, 1.0f};
};

/// @brief Bring the render camera into the space @p world maps to the world.
///
/// @param world      the actor's world transform, as `PoseRequest::world`
/// @param view       world→view; `look_at_*` keeps the camera basis in its
///                   COLUMNS, so column 0 is right, 1 up, 2 backward
/// @param cameraPos  the eye, in world space
M3CameraFrame M3BuildCameraFrame(const Matrix44f& world, const Matrix44f& view,
                                 const Vector3f& cameraPos);

/// @brief A quaternion's rotation as a row-vector matrix — rows are the axes.
///
/// `Matrix44f::rotation` is the column-vector form and composes the other way
/// round, which is the wrong one for every `.m3` matrix: the engine multiplies
/// `local * parent`.
Matrix44f M3RotationRows(const Quaternion& q);

/// @brief The inverse: the unit quaternion for a row-vector basis.
///
/// `QuaternionFromMatrix3Packed` (SC2 `0x100D57D60`), branch for branch. It is
/// not a formality — a quaternion can only express a *rotation*, so this is
/// also where the engine quietly repairs a basis that came out of the
/// axis-locked fallbacks not quite square. Reproducing the repair is the
/// difference between agreeing with the game and shearing a bone.
Quaternion M3QuatFromRows(const Matrix44f& m);

/// @brief The orientation `CBBSolver::ApplyBillboard` would give one bone,
///        as a row-vector basis (row *i* is local axis *i*).
///
/// @param billboardType  `BBSC.billboardType`, @ref whiteout::m3::BillboardType
/// @param cameraLookAt   `BBSC.cameraLookAt`: aim at the eye rather than along
///                       the view direction
/// @param cam            @ref M3BuildCameraFrame's result
/// @param boneWorld      the bone's model-space matrix as the parent chain
///                       left it — its position drives the aim, and types 3
///                       and 5 lock one of its current axes
/// @param outBasis       the basis, rows 0..2; untouched when this returns
///                       false
///
/// @return false when the engine leaves the bone alone: type 4, a direction
///         too small to aim with, a lock axis parallel to it, or a basis that
///         fails the engine's own `|len² - 1| < 1e-3` check on all three rows.
bool M3BillboardBasis(::whiteout::u8 billboardType, bool cameraLookAt, const M3CameraFrame& cam,
                      const Matrix44f& boneWorld, Matrix44f& outBasis);

/// @brief Apply one `BBSC` record to a bone's model-space matrix.
///
/// The engine writes the bone's LOCAL ROTATION and lets the hierarchy
/// re-compose, which is reproduced here rather than approximated: the world
/// orientation is divided back through the parent's, scaled by the bone's own
/// local scale, and multiplied by the parent again. Overwriting the world
/// matrix's rows with the basis directly gets the same answer for the ordinary
/// bone and the wrong one for two kinds that exist in the corpus — a bone with
/// a NEGATIVE scale component, whose mirroring would be quietly undone
/// (flipping its skinned normals), and a bone under a scaled parent.
///
/// @param bb            the record
/// @param cam           @ref M3BuildCameraFrame's result
/// @param spin          the bone's sampled local rotation, or null.
///                      Non-null means "this bone hangs off the model root",
///                      which is the case the engine distinguishes: it tests
///                      the parent *node* and only composes the sampled
///                      rotation back on when the parent is not a bone. The
///                      effect is that a root-level billboard keeps its
///                      animated spin in the screen plane while a child's
///                      animated rotation is discarded outright.
/// @param localScale    the bone's sampled scale, SIGNED
/// @param parentWorld   the parent bone's model-space matrix, or null for a
///                      bone parented to the model root
/// @param boneWorld     rewritten in place on success
///
/// @return whatever @ref M3BillboardBasis returned.
bool M3ApplyBillboard(const ::whiteout::m3::BillboardBehavior& bb, const M3CameraFrame& cam,
                      const Quaternion* spin, const Vector3f& localScale,
                      const Matrix44f* parentWorld, Matrix44f& boneWorld);

} // namespace whiteout::flakes::io
