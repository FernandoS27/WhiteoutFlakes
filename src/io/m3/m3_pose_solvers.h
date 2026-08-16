#pragma once

/// @file m3_pose_solvers.h
/// @brief StarCraft II's two live post-skeleton solvers: terrain foot IK
///        (`IKJT`) and the single-axis turret (`PATU`).
///
/// Both **compose a delta onto the animated pose** rather than replacing it —
/// that is the whole point of running after the sampler, and it is why neither
/// claims its bones. SC2 marks an IK chain with runtime-bone bit 5, which
/// explicitly does not suppress sampling; only look-at, actor-side turrets and
/// ragdoll use the bit-0 claim (SC2_ANIM_RE §4c).
///
/// What is faithful here and what is not:
///
/// - The IK goal, its two-probe `fmaxf`, the rate limit and the tolerance skip
///   follow `CJTIKSolver_ComputeGroundGoal` as read.
/// - The CCD loop keeps `CJTIKSolver_IterateCCD`'s shape — bounded iterations,
///   a damped angular step, an early-out on squared error — and solves in the
///   plane through effector/mid/root as the binary does. It does *not* keep the
///   binary's gain constant; see `kStepDamping` for why that is a deliberate
///   deviation rather than an oversight.
/// - `CTurretSolver`'s descriptor carries a rotation axis, reference/forward
///   bases and a rest quaternion at offsets the parser does not expose; what
///   WhiteoutLib gives us is `TurretBehavior` with yaw/pitch limits and a
///   transform. It matters less than it sounds: **every PATU descriptor in the
///   corpus is empty** — identity transform, zero weights, `yawMin == yawMax ==
///   0`. The chunk names the bone and little else, and the axis, limits and
///   turn rate a game would use live in StarCraft II's Actor data. That data is
///   not in the model, and the actor↔solver seam is explicitly unread
///   (SC2_ANIM_RE §11: where the aim target comes from, and who sets
///   `solver+32`). So the axis falls out as +Z, a degenerate limit pair is read
///   as *unset* rather than as a lock, and the easing curve (a 1024-entry table
///   in the binary) is a smoothstep.
///
/// Neither runs unless the host supplies its input — no ground query means no
/// IK, no aim target means the turret converges back to its animation. That is
/// also SC2's behaviour: IK is gated on a world flag, not on the model.

#include "whiteout/flakes/pose_stage.h"
#include "whiteout/flakes/types.h"

#include <whiteout/models/m3/structures.h>

#include <vector>

namespace whiteout::flakes::io {

/// @brief `IKJT` — plants a chain's effector on the ground.
class M3JtIkStage final : public renderer::animation::IPoseStage {
public:
    /// @param chain Bone indices from root to effector, in that order.
    M3JtIkStage(std::vector<i32> chain, f32 searchUp, f32 searchDown, f32 maxSpeedPerFrame30,
                f32 tolerance)
        : chain_(std::move(chain)), searchUp_(searchUp), searchDown_(searchDown),
          maxSpeedPerFrame30_(maxSpeedPerFrame30), tolerance_(tolerance) {}

    void Run(renderer::model::FrameState& fs,
             const renderer::animation::PoseStageContext& ctx) override;

    /// @brief Iteration cap, matching `solver+144`'s default of 100.
    static constexpr i32 kMaxIterations = 100;

    /// @brief How much of each pass's angular error a joint takes.
    ///
    /// **Not** the binary's `solver+148` gain of 0.01. That constant scales an
    /// error term whose units were never read — `CJTIKSolver_IterateCCD`'s body
    /// is listed as skimmed in SC2_ANIM_RE §11 — and taking it as a fraction of
    /// the joint angle does not converge inside 100 iterations, which shows up
    /// as feet hovering short of the ground. So the damping is ours: enough to
    /// spread the correction along the chain rather than folding it at one
    /// joint, and enough to converge well inside the cap.
    ///
    /// The converged pose is the goal either way; what differs from SC2 is the
    /// path taken to it, and how the bend is distributed. Reproducing that
    /// exactly needs the error metric read properly, not a tuned guess.
    static constexpr f32 kStepDamping = 0.5f;

private:
    std::vector<i32> chain_;
    f32 searchUp_ = 0.0f;
    f32 searchDown_ = 0.0f;
    f32 maxSpeedPerFrame30_ = 0.0f;
    f32 tolerance_ = 0.0f;

    /// @brief Last frame's goal Z, which the rate limit is measured from.
    ///        Unset until the first solve, so the first frame snaps.
    bool haveGoal_ = false;
    f32 goalZ_ = 0.0f;
};

/// @brief `PATU` — aims one bone about a single permitted axis.
class M3TurretStage final : public renderer::animation::IPoseStage {
public:
    M3TurretStage(i32 bone, Vector3f axis, f32 turnRatePerSec, bool yawLimited, f32 yawMin,
                  f32 yawMax)
        : bone_(bone), axis_(axis), turnRate_(turnRatePerSec), yawLimited_(yawLimited),
          yawMin_(yawMin), yawMax_(yawMax) {}

    void Run(renderer::model::FrameState& fs,
             const renderer::animation::PoseStageContext& ctx) override;

private:
    i32 bone_ = -1;
    Vector3f axis_{0.0f, 0.0f, 1.0f};
    f32 turnRate_ = 3.0f;
    bool yawLimited_ = false;
    f32 yawMin_ = 0.0f;
    f32 yawMax_ = 0.0f;

    /// @brief Where the turret currently points, in radians about @ref axis_.
    ///        Slerped toward the demand so it swings rather than snapping, and
    ///        toward zero when nothing is aiming — which is what makes a turret
    ///        drift back into its animation.
    f32 currentAngle_ = 0.0f;
    bool haveAngle_ = false;
};

/// @brief Smoothstep in place of the binary's 1024-entry easing table.
f32 M3TurretEase(f32 t);

/// @brief Rotate @p v about @p axis (unit) by @p radians. Rodrigues.
Vector3f M3RotateAboutAxis(const Vector3f& v, const Vector3f& axis, f32 radians);

} // namespace whiteout::flakes::io
