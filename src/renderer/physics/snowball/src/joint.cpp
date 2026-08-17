#include "snowball/joint.h"

#include <algorithm>
#include <cmath>

#include "snowball/scene.h"
#include "snowball/solver.h"  // kBaumgarte — the shoulder's limits use the contact solver's gain

namespace snowball {
namespace {

Vec4 Conjugate(const Vec4& q) { return {-q.x, -q.y, -q.z, q.w}; }

/// The world-frame offset from a body's centre of mass to a point given in the body's frame.
/// Offsetting from the origin instead is wrong in a way that looks right for every centred
/// fixture, so the mistake hides until a shape's centre of mass is off its origin.
Vec4 OffsetTo(const Body& body, const Vec4& localPoint) {
    return RotationMatrix(body.transform.rotation).Transform(localPoint - body.localCentre);
}

/// `skew(r) * v == r x v`, as columns.
Mtx Skew(const Vec4& r) {
    Mtx m;
    m.c[0] = {0.0f, r.z, -r.y, 0.0f};
    m.c[1] = {-r.z, 0.0f, r.x, 0.0f};
    m.c[2] = {r.y, -r.x, 0.0f, 0.0f};
    m.c[3] = constants::kUnitW;
    return m;
}

Mtx Scale3(const Mtx& m, f32 s) {
    Mtx out;
    for (i32 i = 0; i < 3; ++i) {
        out.c[i] = m.c[i] * s;
    }
    out.c[3] = constants::kUnitW;
    return out;
}

Mtx Add3(const Mtx& a, const Mtx& b) {
    Mtx out;
    for (i32 i = 0; i < 3; ++i) {
        out.c[i] = a.c[i] + b.c[i];
    }
    out.c[3] = constants::kUnitW;
    return out;
}

Mtx Diagonal(f32 v) {
    Mtx m;
    m.c[0] = {v, 0.0f, 0.0f, 0.0f};
    m.c[1] = {0.0f, v, 0.0f, 0.0f};
    m.c[2] = {0.0f, 0.0f, v, 0.0f};
    m.c[3] = constants::kUnitW;
    return m;
}

/// The z axis of a body's joint frame, in the world -- the axis every family that leaves
/// something free leaves free: the plane slides along it and the shoulder twists about it.
Vec4 FrameAxis(const Body& body, const Vec4& localFrame) {
    return RotationMatrix(QuatMultiply(body.transform.rotation, localFrame))
        .Transform(constants::kUnitZ);
}

Vec4 Normalize3(const Vec4& v) {
    const f32 lengthSquared = LengthSquared3(v);
    return lengthSquared > constants::kZeroSafe.x ? v * Rsqrt(lengthSquared) : Vec4{};
}

}  // namespace

bool JointLocksRotation(JointKind kind) {
    return kind == JointKind::Weld || kind == JointKind::Plane;
}

Vec4 JointFreeAxis(const Joint& joint) {
    return joint.kind == JointKind::Plane ? constants::kUnitZ : Vec4{};
}

JointSolver::JointSolver(std::vector<Body>& bodies, const std::vector<Joint*>& joints, f32 dt)
    : bodies_(&bodies), dt_(dt) {
    joints_.reserve(joints.size());
    for (Joint* joint : joints) {
        if (joint != nullptr && joint->bodyA >= 0 && joint->bodyB >= 0) {
            joints_.push_back(joint);
        }
    }
    InitTwist();
}

namespace {

/// @brief The engine's `atan`, coefficient and branch both.
///
/// `t / (1 + 0.28 t^2)` inside the unit interval and `+-pi/2 - t / (0.28 + t^2)` outside it --
/// the classic one-term Pade form, with 0.28 as the fixed coefficient. Deliberately not
/// `std::atan`: the twist angle it produces is compared against the def's limits, so where a
/// limit *is* depends on this exact approximation -- it is off the true arctangent by up to
/// about a tenth of a degree, and swapping in the library function moves every authored limit
/// by that much. The quadrant offset the atan2 adds -- the float nearest pi/2.
constexpr f32 kHalfPi = 1.5707963705062866f;

f32 Atan(f32 t) {
    const f32 magnitude = t < 0.0f ? -t : t;
    if (magnitude < 1.0f) {
        return t / (1.0f + 0.28f * (t * t));
    }
    const f32 offset = t < 0.0f ? -kHalfPi : kHalfPi;
    return offset - t / (0.28f + (t * t));
}

/// @brief `atan2` built on @ref Atan, with the standard quadrant fixups.
f32 Atan2(f32 y, f32 x) {
    if (x == 0.0f) {
        return y > 0.0f ? kHalfPi : (y < 0.0f ? -kHalfPi : 0.0f);
    }
    const f32 angle = Atan(y / x);
    if (x < 0.0f) {
        return y >= 0.0f ? angle + constants::kPi.x : angle - constants::kPi.x;
    }
    return angle;
}

/// @brief The revolute's angular reference: **`localAnchorA`, read as a quaternion.**
///
/// Same storage convention as the weld's angular target: a unit quaternion in the anchor-A
/// slot arms the whole angular half — Jacobian rows, 2x2 mass, latched limit state — and
/// zeroing it disarms everything while the point constraint keeps working. The reference's
/// z axis, in A's frame, is the hinge axis; twist is measured about it relative to the
/// reference orientation.
///
/// **A zero reference is a degenerate quaternion and the joint's angular half is silently
/// inert.** That is deliberate and part of the engine's contract — a revolute with neutral
/// frames constrains no rotation, and pinned regression behaviour depends on it — so do not
/// "fix" it into a default hinge. Non-unit references are normalised here: used raw, the
/// length would act as a gain on the correction — the behaviour the weld's unnormalised
/// target exhibits — which is exactly what a hinge frame must not do, so the reference is
/// brought to unit length before anything reads it.
bool RevoluteReference(const Joint& joint, Vec4& out) {
    const Vec4& q = joint.localAnchorA;
    const f32 lengthSquared = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (lengthSquared <= constants::kZeroSafe.x) {
        return false;
    }
    out = q * Rsqrt(lengthSquared);
    return true;
}

/// The angular frames the twist machinery measures against, per family: the shoulder authors
/// both sides in `localFrameA/B`; the revolute authors its A-side reference in the anchor-A
/// slot — often the only angular quaternion its def carries.
///
/// B's side reads `localFrameB` rather than a hard identity, and the distinction is
/// deliberate: a def that leaves the slot neutral sees no difference, but authored ragdoll
/// content writes a real B frame, and measuring twist against B's raw body instead conjugates
/// the twist coordinate by frameB — which turns an asymmetric authored range like [-90, 0]
/// into a bound on the wrong direction, swinging a chain root backward through the very range
/// the limit was written to close.
Vec4 AngularFrameA(const Joint& joint) {
    if (joint.kind == JointKind::Revolute) {
        Vec4 reference = constants::kQuatIdentity;
        RevoluteReference(joint, reference);
        return reference;
    }
    return joint.localFrameA;
}

Vec4 AngularFrameB(const Joint& joint) {
    return joint.localFrameB;
}

/// @brief The relative rotation of B's joint frame in A's.
Vec4 RelativeFrame(const Body& a, const Body& b, const Joint& joint) {
    const Vec4 qa = QuatMultiply(a.transform.rotation, AngularFrameA(joint));
    const Vec4 qb = QuatMultiply(b.transform.rotation, AngularFrameB(joint));
    return QuatMultiply(Vec4{-qa.x, -qa.y, -qa.z, qa.w}, qb);
}

/// @brief Rotation about the frame's own z, in (-pi, pi].
///
/// The swing-twist twist angle: `2 * atan2(q.z, q.w)` on the relative frame, which is the
/// half-angle of the z component doubled. The wrap adds or subtracts a full turn once rather
/// than taking a modulo, so a joint driven past pi comes back in on the other side of the
/// (-pi, pi] interval.
f32 TwistAngle(const Vec4& relative) {
    f32 angle = 2.0f * Atan2(relative.z, relative.w);
    if (angle < -constants::kPi.x) {
        angle += constants::kTwoPi.x;
    } else if (angle > constants::kPi.x) {
        angle -= constants::kTwoPi.x;
    }
    return angle;
}

/// Two angular slops. Below this the range is treated as locked rather than as a range --
/// Box2D's revolute rule, with the constant fixed at 4 degrees (0.06981318).
constexpr f32 kTwistEqualEpsilon = 0.06981318f;


}  // namespace

void JointSolver::InitTwist() {
    for (Joint* joint : joints_) {
        if ((joint->kind != JointKind::Shoulder && joint->kind != JointKind::Revolute) ||
            !joint->enableTwistLimit) {
            continue;
        }
        if (joint->kind == JointKind::Revolute) {
            // No reference, no limit: with the anchor-A quaternion degenerate the twist angle
            // evaluates to zero forever, so the state machine could never leave Inactive —
            // latch it there explicitly and skip the work. See `RevoluteReference`.
            Vec4 reference;
            if (!RevoluteReference(*joint, reference)) {
                joint->twistState = Joint::TwistState::Inactive;
                continue;
            }
        }
        const Body& a = (*bodies_)[static_cast<usize>(joint->bodyA)];
        const Body& b = (*bodies_)[static_cast<usize>(joint->bodyB)];
        const f32 angle = TwistAngle(RelativeFrame(a, b, *joint));
        const f32 span = joint->upperTwist - joint->lowerTwist;
        const Joint::TwistState previous = joint->twistState;

        if ((span < 0.0f ? -span : span) < kTwistEqualEpsilon) {
            // A range narrower than the slop is a *lock*, not a range -- and it is the common
            // case, because authored ragdoll content overwhelmingly writes
            // `lower == upper == 0`. Reading that as "no limit" is what lets cloth corkscrew.
            joint->twistState = Joint::TwistState::Equal;
        } else if (angle <= joint->lowerTwist) {
            joint->twistState = Joint::TwistState::AtLower;
        } else if (angle >= joint->upperTwist) {
            joint->twistState = Joint::TwistState::AtUpper;
        } else {
            joint->twistState = Joint::TwistState::Inactive;
            joint->twistImpulse = 0.0f;
        }
        // The accumulated impulse survives a step that stays on the same side, and is dropped
        // when the limit changes sides -- warm starting a push in the direction it no longer
        // pushes is worse than starting cold.
        if (joint->twistState != previous &&
            joint->twistState != Joint::TwistState::Equal) {
            joint->twistImpulse = 0.0f;
        }
    }
}

void JointSolver::SolveAngularSpringVelocity(Joint& joint) {
    // The opt-in angular drive -- see `JointDef`'s angular spring block for the formulation
    // and why it defaults off: authored ragdoll content carries no spring parameters, so only
    // a def built by hand ever enables it.
    if (!joint.enableAngularSpring || joint.springHertz <= 0.0f || dt_ <= 0.0f) {
        return;
    }
    Body& a = BodyA(joint);
    Body& b = BodyB(joint);

    Mtx k = Add3(a.invInertiaWorld, b.invInertiaWorld);
    const f32 trace = k.c[0].x + k.c[1].y + k.c[2].z;
    if (trace <= 0.0f) {
        return;  // both bodies immovable
    }
    // Box2D's soft constraint is scalar, so the 3x3 needs one representative inertia to build
    // the stiffness from. The harmonic mean of the diagonal is the natural choice: it is the
    // scalar a symmetric body would have, and it degrades gracefully for an elongated one.
    const f32 mass = 3.0f * Rcp(trace);
    const f32 omega = constants::kTwoPi.x * joint.springHertz;
    const f32 damping = 2.0f * mass * joint.springDamping * omega;
    const f32 stiffness = mass * omega * omega;
    const f32 denominator = dt_ * (damping + dt_ * stiffness);
    if (denominator <= 0.0f) {
        return;
    }
    const f32 gamma = Rcp(denominator);
    const f32 beta = dt_ * stiffness * gamma;

    // World-space orientation error, as a rotation vector. `2 * q.xyz` is the small-angle form
    // of the axis-angle of `q`, which is what the constraint wants; the hemisphere flip keeps a
    // spring past a half turn pulling the short way round rather than winding up.
    const Vec4 frameA = QuatMultiply(a.transform.rotation, joint.localFrameA);
    const Vec4 frameB = QuatMultiply(b.transform.rotation, joint.localFrameB);
    Vec4 error = QuatMultiply(frameB, Vec4{-frameA.x, -frameA.y, -frameA.z, frameA.w});
    if (error.w < 0.0f) {
        error = Vec4{-error.x, -error.y, -error.z, -error.w};
    }
    const Vec4 rotation{2.0f * error.x, 2.0f * error.y, 2.0f * error.z, 0.0f};

    // Softening adds gamma down the diagonal, which is what makes this a spring rather than a
    // rigid weld: the constraint is allowed to be violated in proportion to the force it takes.
    Mtx soft = k;
    soft.c[0].x += gamma;
    soft.c[1].y += gamma;
    soft.c[2].z += gamma;

    const Vec4 relative = b.angularVelocity - a.angularVelocity;
    const Vec4 bias = rotation * beta + joint.springImpulse * gamma;
    Vec4 impulse = Solve33(soft, (relative + bias) * -1.0f);

    Vec4 total = joint.springImpulse + impulse;
    if (joint.maxSpringTorque > 0.0f) {
        // The clamp is on the accumulated impulse, at `maxTorque * h`, and rescales rather
        // than truncating per axis, so a saturated spring keeps its direction.
        const f32 ceiling = joint.maxSpringTorque * dt_;
        if (LengthSquared3(total) > ceiling * ceiling) {
            total = Normalize3(total) * ceiling;
            impulse = total - joint.springImpulse;
        }
    }
    joint.springImpulse = total;
    ApplyAngular(joint, impulse);
}

void JointSolver::SolveTwistVelocity(Joint& joint) {
    if (!joint.enableTwistLimit || joint.twistState == Joint::TwistState::Inactive) {
        return;
    }
    Body& a = BodyA(joint);
    Body& b = BodyB(joint);

    const Vec4 axis = FrameAxis(a, AngularFrameA(joint));
    const Vec4 rowA = a.invInertiaWorld.Transform(axis);
    const Vec4 rowB = b.invInertiaWorld.Transform(axis);
    const f32 k = Dot3(axis, rowA + rowB);
    if (k <= 0.0f) {
        return;  // two infinite-inertia bodies have no twist to solve
    }

    const f32 mass = Rcp(k);
    f32 impulse = -mass * Dot3(b.angularVelocity - a.angularVelocity, axis);
    const f32 previous = joint.twistImpulse;

    switch (joint.twistState) {
        case Joint::TwistState::AtLower:
            // Sitting on the lower stop, so the limit may only push the twist *up*.
            joint.twistImpulse = std::max(previous + impulse, 0.0f);
            impulse = joint.twistImpulse - previous;
            break;
        case Joint::TwistState::AtUpper:
            joint.twistImpulse = std::min(previous + impulse, 0.0f);
            impulse = joint.twistImpulse - previous;
            break;
        case Joint::TwistState::Equal:
            // Locked: both directions are held, so the impulse is applied unclamped.
            joint.twistImpulse = previous + impulse;
            break;
        case Joint::TwistState::Inactive:
            return;
    }

    a.angularVelocity = a.angularVelocity - rowA * impulse;
    b.angularVelocity = b.angularVelocity + rowB * impulse;
}

JointSolver::Anchors JointSolver::Resolve(const Joint& joint) const {
    const Body& a = (*bodies_)[static_cast<usize>(joint.bodyA)];
    const Body& b = (*bodies_)[static_cast<usize>(joint.bodyB)];

    Anchors out;
    if (joint.kind == JointKind::Distance || joint.kind == JointKind::Spherical) {
        // The symmetric rule the field names suggest: each anchor is in its own body's frame.
        out.rA = OffsetTo(a, joint.localAnchorA);
        out.rB = OffsetTo(b, joint.localAnchorB);
    } else {
        // ...and the rule the other four actually use: B's ORIGIN is held at `localAnchorB`
        // expressed in A's frame, with `localAnchorA` playing no part in the position at all.
        // A weld authored with a decoy anchor on A still holds B at the anchor on B -- the
        // asymmetry is the contract, not a swapped argument to fix.
        out.rA = OffsetTo(a, joint.localAnchorB);
        out.rB = OffsetTo(b, Vec4{});
    }
    out.worldA = a.worldCentre + out.rA;
    out.worldB = b.worldCentre + out.rB;
    return out;
}

Mtx JointSolver::PointMass(const Joint& joint, const Anchors& anchors) const {
    const Body& a = (*bodies_)[static_cast<usize>(joint.bodyA)];
    const Body& b = (*bodies_)[static_cast<usize>(joint.bodyB)];

    const Mtx sa = Skew(anchors.rA);
    const Mtx sb = Skew(anchors.rB);
    Mtx k = Diagonal(a.invMass + b.invMass);
    k = Add3(k, Scale3(Concat3(Concat3(sa, a.invInertiaWorld), sa), -1.0f));
    k = Add3(k, Scale3(Concat3(Concat3(sb, b.invInertiaWorld), sb), -1.0f));
    return k;
}

void JointSolver::ApplyLinear(const Joint& joint, const Vec4& impulse, const Anchors& anchors) {
    Body& a = (*bodies_)[static_cast<usize>(joint.bodyA)];
    Body& b = (*bodies_)[static_cast<usize>(joint.bodyB)];
    b.linearVelocity = b.linearVelocity + impulse * b.invMass;
    b.angularVelocity =
        b.angularVelocity + b.invInertiaWorld.Transform(Cross3(anchors.rB, impulse));
    a.linearVelocity = a.linearVelocity - impulse * a.invMass;
    a.angularVelocity =
        a.angularVelocity - a.invInertiaWorld.Transform(Cross3(anchors.rA, impulse));
}

void JointSolver::ApplyAngular(const Joint& joint, const Vec4& impulse) {
    Body& a = (*bodies_)[static_cast<usize>(joint.bodyA)];
    Body& b = (*bodies_)[static_cast<usize>(joint.bodyB)];
    b.angularVelocity = b.angularVelocity + b.invInertiaWorld.Transform(impulse);
    a.angularVelocity = a.angularVelocity - a.invInertiaWorld.Transform(impulse);
}

// -- velocity ---------------------------------------------------------------------------------

void JointSolver::SolvePointVelocity(const Joint& joint) {
    const Anchors anchors = Resolve(joint);
    const Body& a = (*bodies_)[static_cast<usize>(joint.bodyA)];
    const Body& b = (*bodies_)[static_cast<usize>(joint.bodyB)];

    Vec4 relative = (b.linearVelocity + Cross3(b.angularVelocity, anchors.rB)) -
                    (a.linearVelocity + Cross3(a.angularVelocity, anchors.rA));

    // A plane joint leaves one axis of A's frame free, so the constraint is the other two: drop
    // the component along it rather than solving a rank-deficient system.
    const Vec4 free = JointFreeAxis(joint);
    if (LengthSquared3(free) > 0.0f) {
        const Vec4 axis = Normalize3(RotationMatrix(
            QuatMultiply(a.transform.rotation, joint.localFrameA)).Transform(free));
        relative = relative - axis * Dot3(relative, axis);
    }

    ApplyLinear(joint, Solve33(PointMass(joint, anchors), -relative), anchors);
}

void JointSolver::SolveDistanceVelocity(const Joint& joint) {
    const Anchors anchors = Resolve(joint);
    const Body& a = (*bodies_)[static_cast<usize>(joint.bodyA)];
    const Body& b = (*bodies_)[static_cast<usize>(joint.bodyB)];

    const Vec4 axis = Normalize3(anchors.worldB - anchors.worldA);
    if (LengthSquared3(axis) == 0.0f) {
        return;  // coincident anchors leave the constraint direction undefined
    }

    const Vec4 crossA = Cross3(anchors.rA, axis);
    const Vec4 crossB = Cross3(anchors.rB, axis);
    const f32 k = a.invMass + b.invMass +
                    Dot3(crossA, a.invInertiaWorld.Transform(crossA)) +
                    Dot3(crossB, b.invInertiaWorld.Transform(crossB));
    if (k <= 0.0f) {
        return;
    }

    const f32 speed = Dot3(axis, (b.linearVelocity + Cross3(b.angularVelocity, anchors.rB)) -
                                       (a.linearVelocity + Cross3(a.angularVelocity, anchors.rA)));
    ApplyLinear(joint, axis * (-speed * Rcp(k)), anchors);
}

void JointSolver::SolveAngularVelocity(const Joint& joint) {
    Body& a = (*bodies_)[static_cast<usize>(joint.bodyA)];
    Body& b = (*bodies_)[static_cast<usize>(joint.bodyB)];
    const Mtx k = Add3(a.invInertiaWorld, b.invInertiaWorld);
    ApplyAngular(joint, Solve33(k, -(b.angularVelocity - a.angularVelocity)));
}

void JointSolver::SolveConeVelocity(const Joint& joint) {
    // The cone stops a limb tilting further off its twist axis than the def allows; the twist
    // itself stays free unless `enableTwistLimit` asked otherwise. A shoulder whose cone did
    // nothing would be a ball joint wearing the name, which is the one failure a ragdoll cannot
    // survive -- so this is a real constraint rather than a stub, and its overshoot is pinned
    // regression behaviour rather than free to drift.
    Body& a = BodyA(joint);
    Body& b = BodyB(joint);
    const Vec4 axisA = FrameAxis(a, joint.localFrameA);
    const Vec4 axisB = FrameAxis(b, joint.localFrameB);
    if (Dot3(axisA, axisB) >= std::cos(joint.coneAngle)) {
        return;
    }
    const Vec4 tiltAxis = Normalize3(Cross3(axisA, axisB));
    const f32 opening = Dot3(b.angularVelocity - a.angularVelocity, tiltAxis);
    if (opening <= 0.0f) {
        return;  // already coming back inside the cone; a limit pushes nothing
    }
    const Mtx k = Add3(a.invInertiaWorld, b.invInertiaWorld);
    const f32 mass = Dot3(tiltAxis, k.Transform(tiltAxis));
    if (mass > 0.0f) {
        ApplyAngular(joint, tiltAxis * (-opening * Rcp(mass)));
    }
}

namespace {

/// The two tangent rows of a hinge's angular Jacobian: the world X and Y axes of the reference
/// frame. Equivalently the `0.5 * G(q_ref)` rows — the quaternion-derivative form — which for
/// a unit reference spans the same plane with a scale that cancels in the 2x2 solve.
struct HingeTangents {
    Vec4 t1;
    Vec4 t2;
};

HingeTangents HingeBasis(const Body& a, const Vec4& reference) {
    const Mtx frame = RotationMatrix(QuatMultiply(a.transform.rotation, reference));
    return {frame.Transform(constants::kUnitX), frame.Transform(constants::kUnitY)};
}

/// Solve the 2x2 system the two tangents span. Returns false when both bodies have infinite
/// inertia about the plane, which is the only case the determinant can vanish.
bool SolveSym22(const Mtx& k, const HingeTangents& t, f32 c1, f32 c2, f32& l1, f32& l2) {
    const Vec4 kt1 = k.Transform(t.t1);
    const Vec4 kt2 = k.Transform(t.t2);
    const f32 k11 = Dot3(t.t1, kt1);
    const f32 k12 = Dot3(t.t1, kt2);
    const f32 k22 = Dot3(t.t2, kt2);
    const f32 det = k11 * k22 - k12 * k12;
    if (!(det > constants::kZeroSafe.x || det < -constants::kZeroSafe.x)) {
        return false;
    }
    const f32 invDet = Rcp(det);
    l1 = (k22 * c1 - k12 * c2) * invDet;
    l2 = (k11 * c2 - k12 * c1) * invDet;
    return true;
}

}  // namespace

void JointSolver::SolveHingeVelocity(const Joint& joint) {
    Vec4 reference;
    if (!RevoluteReference(joint, reference)) {
        return;  // a zero reference disarms the whole angular half; see RevoluteReference
    }
    Body& a = BodyA(joint);
    Body& b = BodyB(joint);
    const HingeTangents t = HingeBasis(a, reference);
    const Vec4 wRel = b.angularVelocity - a.angularVelocity;
    const Mtx k = Add3(a.invInertiaWorld, b.invInertiaWorld);
    f32 l1 = 0.0f, l2 = 0.0f;
    if (SolveSym22(k, t, -Dot3(wRel, t.t1), -Dot3(wRel, t.t2), l1, l2)) {
        ApplyAngular(joint, t.t1 * l1 + t.t2 * l2);
    }
}

void JointSolver::SolveVelocityConstraints() {
    for (Joint* joint : joints_) {
        switch (joint->kind) {
            case JointKind::Mouse:
                break;  // inert without a target, which this seam has no way to set
            case JointKind::Distance:
                SolveDistanceVelocity(*joint);
                break;
            default:
                SolvePointVelocity(*joint);
                break;
        }
        if (JointLocksRotation(joint->kind)) {
            SolveAngularVelocity(*joint);
        } else if (joint->kind == JointKind::Shoulder) {
            SolveAngularSpringVelocity(*joint);
            SolveTwistVelocity(*joint);
            SolveConeVelocity(*joint);
        } else if (joint->kind == JointKind::Revolute) {
            // Limit before lock, both reading whatever the point solve above left behind.
            // There is deliberately no motor: nothing maps authored motor parameters onto
            // these defs, and a wrong drive is worse than none. The limit is the
            // shoulder-twist machinery, shared deliberately.
            SolveTwistVelocity(*joint);
            SolveHingeVelocity(*joint);
        }
    }
}

// -- position ---------------------------------------------------------------------------------

f32 JointSolver::SolvePointPosition(const Joint& joint) {
    const Anchors anchors = Resolve(joint);
    Body& a = (*bodies_)[static_cast<usize>(joint.bodyA)];
    Body& b = (*bodies_)[static_cast<usize>(joint.bodyB)];

    Vec4 error = anchors.worldB - anchors.worldA;
    const Vec4 free = JointFreeAxis(joint);
    if (LengthSquared3(free) > 0.0f) {
        const Vec4 axis = Normalize3(RotationMatrix(
            QuatMultiply(a.transform.rotation, joint.localFrameA)).Transform(free));
        error = error - axis * Dot3(error, axis);
    }

    // The whole error, not a fraction of it: no Baumgarte factor and no clamp. One iteration
    // takes a two-unit error to zero, which is what makes a distance joint snap to its rest
    // length in a single step rather than creeping toward it.
    const Vec4 impulse = Solve33(PointMass(joint, anchors), -error);
    b.worldCentre = b.worldCentre + impulse * b.invMass;
    b.transform.rotation = IntegrateRotation(
        b.transform.rotation, b.invInertiaWorld.Transform(Cross3(anchors.rB, impulse)), 1.0f);
    b.SyncTransform();
    a.worldCentre = a.worldCentre - impulse * a.invMass;
    a.transform.rotation = IntegrateRotation(
        a.transform.rotation, -a.invInertiaWorld.Transform(Cross3(anchors.rA, impulse)), 1.0f);
    a.SyncTransform();
    return std::sqrt(LengthSquared3(error));
}

f32 JointSolver::SolveDistancePosition(const Joint& joint) {
    const Anchors anchors = Resolve(joint);
    Body& a = (*bodies_)[static_cast<usize>(joint.bodyA)];
    Body& b = (*bodies_)[static_cast<usize>(joint.bodyB)];

    const Vec4 separation = anchors.worldB - anchors.worldA;
    const f32 length = std::sqrt(LengthSquared3(separation));
    if (length <= constants::kZeroSafe.x) {
        return 0.0f;
    }
    const Vec4 axis = separation * Rcp(length);

    // Bilateral: a distance joint is a rod, not a rope. Started inside its rest length it pushes
    // the bodies APART -- deliberate, and not what the name suggests, so leave it bilateral.
    const f32 error = length - joint.restLength;
    const Vec4 crossA = Cross3(anchors.rA, axis);
    const Vec4 crossB = Cross3(anchors.rB, axis);
    const f32 k = a.invMass + b.invMass +
                    Dot3(crossA, a.invInertiaWorld.Transform(crossA)) +
                    Dot3(crossB, b.invInertiaWorld.Transform(crossB));
    if (k <= 0.0f) {
        return error < 0.0f ? -error : error;
    }

    const Vec4 impulse = axis * (-error * Rcp(k));
    b.worldCentre = b.worldCentre + impulse * b.invMass;
    b.transform.rotation = IntegrateRotation(
        b.transform.rotation, b.invInertiaWorld.Transform(Cross3(anchors.rB, impulse)), 1.0f);
    b.SyncTransform();
    a.worldCentre = a.worldCentre - impulse * a.invMass;
    a.transform.rotation = IntegrateRotation(
        a.transform.rotation, -a.invInertiaWorld.Transform(Cross3(anchors.rA, impulse)), 1.0f);
    a.SyncTransform();
    return error < 0.0f ? -error : error;
}

namespace {

/// Slop and gain for the shoulder's two angular limits — 2 degrees of slack, at most 30
/// degrees of correction per iteration, and the same 0.2 Baumgarte factor the contact solver
/// uses.
///
/// Unlike `SolvePointPosition`, which drives its whole error out in one step, both limits are
/// deliberately soft: the correction is clamped and scaled, so a joint recovers from a large
/// violation over several iterations instead of snapping.
inline constexpr f32 kAngularSlop = 0.034906588f;
inline constexpr f32 kMaxAngularCorrection = 0.52359879f;

/// Rotate B by `+w` and A by `-w`, as a position-level nudge. Same split as `ApplyAngular`, one
/// level up: there it is a velocity, here a rotation.
void NudgeRotations(Body& a, Body& b, const Vec4& impulse) {
    b.transform.rotation =
        IntegrateRotation(b.transform.rotation, b.invInertiaWorld.Transform(impulse), 1.0f);
    b.SyncTransform();
    a.transform.rotation =
        IntegrateRotation(a.transform.rotation, -a.invInertiaWorld.Transform(impulse), 1.0f);
    a.SyncTransform();
}

}  // namespace

f32 JointSolver::SolveConePosition(const Joint& joint) {
    Body& a = BodyA(joint);
    Body& b = BodyB(joint);
    const Vec4 axisA = FrameAxis(a, joint.localFrameA);
    const Vec4 axisB = FrameAxis(b, joint.localFrameB);

    // Angle from the two frames' z axes, as `Atan2(|cross|, dot)` on the engine's Padé
    // approximation rather than an acos — near zero the two disagree by enough to matter to a
    // 5-degree cone, so the choice of form is load-bearing.
    Vec4 tiltAxis = Cross3(axisA, axisB);
    const f32 lengthSquared = LengthSquared3(tiltAxis);
    f32 sine = 0.0f;
    if (lengthSquared > constants::kZeroSafe.x) {
        sine = std::sqrt(lengthSquared);
        tiltAxis = tiltAxis * Rsqrt(lengthSquared);
    }
    const f32 angle = Atan2(sine, Dot3(axisA, axisB));
    if (angle < joint.coneAngle) {
        return 0.0f;
    }

    const f32 error = std::clamp(angle - joint.coneAngle - kAngularSlop, 0.0f,
                                 kMaxAngularCorrection);
    const Mtx k = Add3(a.invInertiaWorld, b.invInertiaWorld);
    const f32 mass = Dot3(tiltAxis, k.Transform(tiltAxis));
    if (mass <= 0.0f) {
        return error;
    }
    NudgeRotations(a, b, tiltAxis * (-(error * kBaumgarte) * Rcp(mass)));
    return error;
}

f32 JointSolver::SolveHingePosition(const Joint& joint) {
    Vec4 reference;
    if (!RevoluteReference(joint, reference)) {
        return 0.0f;  // a zero reference disarms the whole angular half; see RevoluteReference
    }
    Body& a = BodyA(joint);
    Body& b = BodyB(joint);
    const Vec4 axisA = FrameAxis(a, reference);
    const Vec4 axisB = FrameAxis(b, AngularFrameB(joint));

    Vec4 cross = Cross3(axisA, axisB);
    const f32 lengthSquared = LengthSquared3(cross);
    f32 sine = 0.0f;
    if (lengthSquared > constants::kZeroSafe.x) {
        sine = std::sqrt(lengthSquared);
        cross = cross * Rsqrt(lengthSquared);
    } else if (Dot3(axisA, axisB) < 0.0f) {
        // Folded exactly back on itself: the cross is degenerate, so any perpendicular is the
        // shortest way home. Pick one from the frame basis rather than inventing a rule.
        cross = HingeBasis(a, reference).t1;
        sine = 0.0f;
    } else {
        return 0.0f;  // aligned
    }
    const f32 angle = Atan2(sine, Dot3(axisA, axisB));
    if (angle <= 0.0f) {
        return 0.0f;
    }

    // The error vector lies along the tilt axis; the correction is solved on the same 2x2
    // tangent mass as the velocity half and driven out whole (see the header on why there is
    // no Baumgarte factor here).
    const Vec4 c = cross * angle;
    const HingeTangents t = HingeBasis(a, reference);
    const Mtx k = Add3(a.invInertiaWorld, b.invInertiaWorld);
    f32 l1 = 0.0f, l2 = 0.0f;
    if (SolveSym22(k, t, -Dot3(c, t.t1), -Dot3(c, t.t2), l1, l2)) {
        NudgeRotations(a, b, t.t1 * l1 + t.t2 * l2);
    }
    return angle;
}

f32 JointSolver::SolveTwistPosition(const Joint& joint) {
    // The whole pass is gated on the limit's state field, and typical authored ragdoll content
    // leaves the limit disabled so the state never arms — on that content this is dead code,
    // live only for formats that enable the limit.
    if (!joint.enableTwistLimit || joint.twistState == Joint::TwistState::Inactive) {
        return 0.0f;
    }
    Body& a = BodyA(joint);
    Body& b = BodyB(joint);

    // Twist runs first, then the cone, and the cone recomputes both frames from the rotations
    // this pass leaves behind. A cone that is already wide open is left alone entirely --
    // past 170 degrees the tilt axis is degenerate and the correction would be noise.
    const Vec4 axisA = FrameAxis(a, AngularFrameA(joint));
    const Vec4 axisB = FrameAxis(b, AngularFrameB(joint));
    if (Atan2(std::sqrt(LengthSquared3(Cross3(axisA, axisB))), Dot3(axisA, axisB)) >=
        kMaxConeAngle) {
        return 0.0f;
    }

    const Vec4 rel = RelativeFrame(a, b, joint);
    const f32 twist = TwistAngle(rel);

    f32 error = 0.0f;
    if (joint.twistState == Joint::TwistState::Equal) {
        // A degenerate range: drive straight at it from whichever side, no slop.
        error = std::clamp(twist, -kMaxAngularCorrection, kMaxAngularCorrection);
    } else if (twist <= joint.lowerTwist) {
        error = std::max(std::min(twist - joint.lowerTwist + kAngularSlop, 0.0f),
                         -kMaxAngularCorrection);
    } else if (twist >= joint.upperTwist) {
        error = std::clamp(twist - joint.upperTwist - kAngularSlop, 0.0f, kMaxAngularCorrection);
    } else {
        return 0.0f;
    }

    // The swing-twist twist axis, not frame A's z. The two agree exactly when the joint is in
    // pure twist and diverge as swing grows, so substituting the frame axis would correct about
    // the wrong line for precisely the poses a limit exists to catch.
    const Vec4 unitZ = constants::kUnitZ;
    const f32 denominator = rel.z * rel.z + rel.w * rel.w;
    Vec4 twistAxis = FrameAxis(a, AngularFrameA(joint));
    if (denominator > constants::kZeroSafe.x) {
        const Vec4 swung = (unitZ * rel.w + Cross3(Vec4{rel.x, rel.y, rel.z}, unitZ)) * rel.w;
        const Vec4 local = (Vec4{rel.x, rel.y, rel.z} * rel.z + swung) * Rcp(denominator);
        twistAxis = RotationMatrix(QuatMultiply(a.transform.rotation, AngularFrameA(joint)))
                        .Transform(local);
    }
    const Mtx k = Add3(a.invInertiaWorld, b.invInertiaWorld);
    const f32 mass = Dot3(twistAxis, k.Transform(twistAxis));
    const f32 magnitude = error < 0.0f ? -error : error;
    if (mass <= 0.0f) {
        return magnitude;
    }
    NudgeRotations(a, b, twistAxis * (-(error * kBaumgarte) * Rcp(mass)));
    return magnitude;
}

f32 JointSolver::SolveAngularPosition(const Joint& joint) {
    Body& a = (*bodies_)[static_cast<usize>(joint.bodyA)];
    Body& b = (*bodies_)[static_cast<usize>(joint.bodyB)];

    // The target is `localAnchorA` read as a quaternion and left unnormalised, so its LENGTH
    // is the gain on this correction. A unit target lands in one step, a quarter-length one
    // takes about thirty, and one of length seven oscillates and settles 77 degrees short --
    // pinned behaviour, so normalising here would silently retune every non-unit def. All
    // zeros is a zero-length target: no correction at all, so the weld holds whatever
    // orientation it was created at.
    const Vec4 target = joint.angularTarget();
    const Vec4 error = QuatMultiply(b.transform.rotation,
                                    Conjugate(QuatMultiply(a.transform.rotation, target)));
    const Vec4 c = Vec4{error.x, error.y, error.z} * 2.0f;

    const Mtx k = Add3(a.invInertiaWorld, b.invInertiaWorld);
    const Vec4 impulse = Solve33(k, -c);
    b.transform.rotation = IntegrateRotation(b.transform.rotation,
                                             b.invInertiaWorld.Transform(impulse), 1.0f);
    b.SyncTransform();
    a.transform.rotation = IntegrateRotation(a.transform.rotation,
                                             -a.invInertiaWorld.Transform(impulse), 1.0f);
    a.SyncTransform();
    return std::sqrt(LengthSquared3(c));
}

bool JointSolver::SolvePositionConstraints() {
    f32 worst = 0.0f;
    for (Joint* joint : joints_) {
        f32 error = 0.0f;
        // The order is load-bearing: twist, then cone, then the point constraint last so it
        // has the final say on where the anchors sit. Each stage reads the rotations the
        // previous one left behind rather than a shared snapshot — the cone in particular
        // recomputes both frames after the twist has moved them.
        if (joint->kind == JointKind::Shoulder) {
            const f32 twist = SolveTwistPosition(*joint);
            const f32 cone = SolveConePosition(*joint);
            error = twist > cone ? twist : cone;
        } else if (joint->kind == JointKind::Revolute) {
            const f32 limit = SolveTwistPosition(*joint);
            const f32 swing = SolveHingePosition(*joint);
            error = limit > swing ? limit : swing;
        }
        switch (joint->kind) {
            case JointKind::Mouse:
                break;
            case JointKind::Distance:
                error = SolveDistancePosition(*joint);
                break;
            default: {
                const f32 point = SolvePointPosition(*joint);
                error = error > point ? error : point;
                break;
            }
        }
        if (JointLocksRotation(joint->kind)) {
            const f32 angular = SolveAngularPosition(*joint);
            error = error > angular ? error : angular;
        }
        worst = worst > error ? worst : error;
    }
    return worst <= 3.0f * constants::kLinearSlop.x;
}

Body& JointSolver::BodyA(const Joint& joint) const {
    return (*bodies_)[static_cast<usize>(joint.bodyA)];
}

Body& JointSolver::BodyB(const Joint& joint) const {
    return (*bodies_)[static_cast<usize>(joint.bodyB)];
}

}  // namespace snowball
