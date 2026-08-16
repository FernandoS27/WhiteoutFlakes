#include "io/m3/m3_pose_solvers.h"

#include <algorithm>
#include <cmath>

namespace whiteout::flakes::io {

using renderer::animation::PoseStageContext;
using renderer::model::FrameState;

namespace {

Vector3f TranslationOf(const Matrix44f& m) {
    return {m.data[3][0], m.data[3][1], m.data[3][2]};
}

f32 Dot(const Vector3f& a, const Vector3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
Vector3f Cross(const Vector3f& a, const Vector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vector3f Sub(const Vector3f& a, const Vector3f& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vector3f Add(const Vector3f& a, const Vector3f& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
f32 Length(const Vector3f& v) {
    return std::sqrt(Dot(v, v));
}
Vector3f Normalize(const Vector3f& v, const Vector3f& fallback = {0.0f, 0.0f, 1.0f}) {
    const f32 l = Length(v);
    return (l > 1e-6f) ? Vector3f{v.x / l, v.y / l, v.z / l} : fallback;
}

/// @brief Rotate a bone's world matrix about @p axis through @p pivot.
///
/// The delta is applied to the *world* matrix rather than by rebuilding the
/// local TRS and re-stripping the parent, which is what `ApplyChainToBones`
/// does. Same result for the pose, and it keeps this stage independent of how
/// a format composed its locals — the reason it can be tested without a model.
Matrix44f RotateAbout(const Matrix44f& m, const Vector3f& pivot, const Vector3f& axis,
                      f32 radians) {
    const f32 c = std::cos(radians), s = std::sin(radians), t = 1.0f - c;
    const Vector3f a = Normalize(axis);
    // Row-vector rotation, matching the convention the rest of the pose uses.
    Matrix44f r = Matrix44f::identity();
    r.data[0][0] = t * a.x * a.x + c;
    r.data[0][1] = t * a.x * a.y + s * a.z;
    r.data[0][2] = t * a.x * a.z - s * a.y;
    r.data[1][0] = t * a.x * a.y - s * a.z;
    r.data[1][1] = t * a.y * a.y + c;
    r.data[1][2] = t * a.y * a.z + s * a.x;
    r.data[2][0] = t * a.x * a.z + s * a.y;
    r.data[2][1] = t * a.y * a.z - s * a.x;
    r.data[2][2] = t * a.z * a.z + c;

    Matrix44f out = m;
    // Rotate the basis rows...
    for (i32 row = 0; row < 3; ++row) {
        const Vector3f b{m.data[row][0], m.data[row][1], m.data[row][2]};
        out.data[row][0] = b.x * r.data[0][0] + b.y * r.data[1][0] + b.z * r.data[2][0];
        out.data[row][1] = b.x * r.data[0][1] + b.y * r.data[1][1] + b.z * r.data[2][1];
        out.data[row][2] = b.x * r.data[0][2] + b.y * r.data[1][2] + b.z * r.data[2][2];
    }
    // ...and orbit the translation about the pivot.
    const Vector3f p = Sub(TranslationOf(m), pivot);
    const Vector3f q{p.x * r.data[0][0] + p.y * r.data[1][0] + p.z * r.data[2][0],
                     p.x * r.data[0][1] + p.y * r.data[1][1] + p.z * r.data[2][1],
                     p.x * r.data[0][2] + p.y * r.data[1][2] + p.z * r.data[2][2]};
    const Vector3f moved = Add(pivot, q);
    out.data[3][0] = moved.x;
    out.data[3][1] = moved.y;
    out.data[3][2] = moved.z;
    return out;
}

} // namespace

f32 M3TurretEase(f32 t) {
    t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}

Vector3f M3RotateAboutAxis(const Vector3f& v, const Vector3f& axis, f32 radians) {
    const Vector3f a = Normalize(axis);
    const f32 c = std::cos(radians), s = std::sin(radians);
    const Vector3f term1{v.x * c, v.y * c, v.z * c};
    const Vector3f cr = Cross(a, v);
    const Vector3f term2{cr.x * s, cr.y * s, cr.z * s};
    const f32 d = Dot(a, v) * (1.0f - c);
    return {term1.x + term2.x + a.x * d, term1.y + term2.y + a.y * d,
            term1.z + term2.z + a.z * d};
}

// ---------------------------------------------------------------------------
// IKJT
// ---------------------------------------------------------------------------

void M3JtIkStage::Run(FrameState& fs, const PoseStageContext& ctx) {
    if (chain_.size() < 2 || !ctx.queryGround)
        return;
    const auto& bones = fs.boneWorldMatrices;
    for (i32 b : chain_)
        if (b < 0 || static_cast<std::size_t>(b) >= bones.size())
            return;

    const i32 effector = chain_.back();
    const Vector3f effPos = TranslationOf(bones[effector]);

    // ---- goal ------------------------------------------------------------
    // Two probes, `fmaxf` of the hits: one under the effector, one past it
    // along the last link. A foot straddling a ledge rests on the higher
    // surface rather than dropping into the gap.
    const Vector3f prev = TranslationOf(bones[chain_[chain_.size() - 2]]);
    const Vector3f along = Sub(effPos, prev);
    const Vector3f probe2 = Add(effPos, along);

    f32 z0 = 0.0f, z1 = 0.0f;
    const bool hit0 = ctx.queryGround(effPos, searchUp_, searchDown_, z0);
    const bool hit1 = ctx.queryGround(probe2, searchUp_, searchDown_, z1);
    f32 goalZ;
    if (hit0 && hit1)
        goalZ = std::max(z0, z1);
    else if (hit0)
        goalZ = z0;
    else if (hit1)
        goalZ = z1;
    else
        goalZ = effPos.z + searchDown_; // documented no-hit fallback

    // Rate limit, in *real* time: the goal may move at most
    // `dtMs * (maxSpeedPerFrame30 * 0.03)`. This is what stops feet popping
    // when a unit crosses a cliff edge, and it is why the stage keeps state.
    if (haveGoal_ && maxSpeedPerFrame30_ > 0.0f && ctx.frameDtMs > 0) {
        const f32 maxDelta =
            static_cast<f32>(ctx.frameDtMs) * (maxSpeedPerFrame30_ * 0.03f);
        goalZ = std::clamp(goalZ, goalZ_ - maxDelta, goalZ_ + maxDelta);
    }
    goalZ_ = goalZ;
    haveGoal_ = true;

    // Already close enough: skip the whole solve, pose untouched. Faithful to
    // `ComputeGroundGoal` returning false, and the reason a flat-ground actor
    // costs nothing.
    if (std::fabs(goalZ - effPos.z) <= tolerance_)
        return;

    const Vector3f goal{effPos.x, effPos.y, goalZ};

    // ---- solve plane -----------------------------------------------------
    // Through root, mid and effector. Degenerate (a straight chain) has no
    // usable normal, so fall back to the plane containing the chain and the
    // goal displacement — a straight leg still has to be able to bend.
    const Vector3f root = TranslationOf(bones[chain_.front()]);
    const Vector3f mid = TranslationOf(bones[chain_[chain_.size() / 2]]);
    Vector3f normal = Cross(Sub(mid, root), Sub(effPos, root));
    if (Length(normal) <= 1e-6f)
        normal = Cross(Sub(effPos, root), Sub(goal, effPos));
    if (Length(normal) <= 1e-6f)
        return;
    normal = Normalize(normal);

    // ---- CCD -------------------------------------------------------------
    // Bounded iterations with a small gain, walking the chain from the tip
    // back toward the root each pass. Every rotation is about the plane
    // normal, so the chain stays in its plane exactly as the 2-D projection
    // in the binary guarantees.
    std::vector<Matrix44f> work(chain_.size());
    for (std::size_t i = 0; i < chain_.size(); ++i)
        work[i] = bones[chain_[i]];

    const f32 tol2 = tolerance_ * tolerance_;
    for (i32 iter = 0; iter < kMaxIterations; ++iter) {
        const Vector3f tip = TranslationOf(work.back());
        const Vector3f err = Sub(goal, tip);
        if (Dot(err, err) <= tol2)
            break;
        for (std::size_t j = chain_.size() - 1; j-- > 0;) {
            const Vector3f pivot = TranslationOf(work[j]);
            const Vector3f toTip = Sub(TranslationOf(work.back()), pivot);
            const Vector3f toGoal = Sub(goal, pivot);
            const f32 lt = Length(toTip), lg = Length(toGoal);
            if (lt <= 1e-6f || lg <= 1e-6f)
                continue;
            // Signed angle between the two, about the plane normal.
            const f32 cosA = std::clamp(Dot(toTip, toGoal) / (lt * lg), -1.0f, 1.0f);
            f32 angle = std::acos(cosA);
            if (Dot(Cross(toTip, toGoal), normal) < 0.0f)
                angle = -angle;
            // Damped, not the full angle: a joint that snaps its tip onto the
            // goal every pass produces a chain that folds hard at whichever
            // joint got there first. Damping spreads the correction along the
            // chain, which is what makes a knee look like a knee.
            const f32 step = angle * kStepDamping;
            if (std::fabs(step) < 1e-7f)
                continue;
            for (std::size_t k = j; k < work.size(); ++k)
                work[k] = RotateAbout(work[k], pivot, normal, step);
        }
    }

    // ---- write back ------------------------------------------------------
    // Root-first, and every bone of the chain — a stage that moved a parent
    // owns its descendants (pose_stage.h). Bones hanging off the chain that
    // are not in it are the model's problem, and SC2 has the same gap.
    for (std::size_t i = 0; i < chain_.size(); ++i)
        fs.boneWorldMatrices[chain_[i]] = work[i];
}

// ---------------------------------------------------------------------------
// PATU
// ---------------------------------------------------------------------------

void M3TurretStage::Run(FrameState& fs, const PoseStageContext& ctx) {
    if (bone_ < 0 || static_cast<std::size_t>(bone_) >= fs.boneWorldMatrices.size())
        return;

    const Matrix44f& animated = fs.boneWorldMatrices[bone_];
    const Vector3f pivot = TranslationOf(animated);
    const Vector3f axis = Normalize(axis_);

    // The bone's own forward, taken from the animated pose every frame. That
    // is what makes the turret compose *onto* the animation instead of
    // replacing it: when the animation swings the mount, the aim swings with
    // it and only the residual angle is ours.
    const Vector3f forward =
        Normalize({animated.data[1][0], animated.data[1][1], animated.data[1][2]});

    f32 demand = 0.0f;
    if (ctx.aimTarget) {
        Vector3f toTarget = Sub(*ctx.aimTarget, pivot);
        // Only the component perpendicular to the axis is reachable — this is
        // a single-DOF joint, so the out-of-plane part is simply not aimed at.
        const f32 along = Dot(toTarget, axis);
        toTarget = Sub(toTarget, {axis.x * along, axis.y * along, axis.z * along});
        Vector3f flatFwd = forward;
        const f32 fAlong = Dot(flatFwd, axis);
        flatFwd = Sub(flatFwd, {axis.x * fAlong, axis.y * fAlong, axis.z * fAlong});

        if (Length(toTarget) > 1e-6f && Length(flatFwd) > 1e-6f) {
            toTarget = Normalize(toTarget);
            flatFwd = Normalize(flatFwd);
            const f32 c = std::clamp(Dot(flatFwd, toTarget), -1.0f, 1.0f);
            demand = std::acos(c);
            if (Dot(Cross(flatFwd, toTarget), axis) < 0.0f)
                demand = -demand;
        }
        // `yawMin == yawMax` is an *unset* limit, not a locked joint. Every
        // PATU descriptor in the corpus ships zeroed — identity transform, zero
        // weights, zero limits — so a literal reading of the flag clamps every
        // turret to zero degrees and the solver silently does nothing. The
        // shipped chunk names the bone and little else; the real limits live in
        // StarCraft II's game data, not in the model.
        if (yawLimited_ && yawMax_ > yawMin_)
            demand = std::clamp(demand, yawMin_, yawMax_);
    }
    // No target: the demand is zero, i.e. the animated rotation — the release
    // path, which is what lets a turret drift back into its animation rather
    // than freezing where it last aimed.

    if (!haveAngle_) {
        currentAngle_ = demand;
        haveAngle_ = true;
    } else if (ctx.frameDtMs > 0 && turnRate_ > 0.0f) {
        // Blend duration is angle / turnRate, shaped by the easing curve — the
        // binary uses a 1024-entry table, this uses smoothstep (named
        // deviation, see the header).
        const f32 remaining = demand - currentAngle_;
        const f32 span = std::fabs(remaining);
        if (span > 1e-6f) {
            const f32 seconds = static_cast<f32>(ctx.frameDtMs) / 1000.0f;
            const f32 t = std::min(1.0f, (seconds * turnRate_) / span);
            currentAngle_ += remaining * M3TurretEase(t);
        }
    } else {
        currentAngle_ = demand;
    }

    if (std::fabs(currentAngle_) < 1e-7f)
        return; // exactly the animated pose; do not touch the matrix at all

    fs.boneWorldMatrices[bone_] = RotateAbout(animated, pivot, axis, currentAngle_);
}

} // namespace whiteout::flakes::io
