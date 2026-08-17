#include "snowball/solver.h"

#include <algorithm>
#include <cmath>

namespace snowball {
namespace {

Vec4 Normalize3(const Vec4& v) {
    const f32 lengthSquared = LengthSquared3(v);
    return lengthSquared > 0.0f ? v * Rsqrt(lengthSquared) : Vec4{};
}

/// Two directions spanning the contact plane. Which two is arbitrary and does not need to be
/// stable, because the accumulated friction is stored as a world vector and re-projected each
/// step rather than kept in these coordinates.
void BuildTangents(const Vec4& normal, Vec4& t1, Vec4& t2) {
    const Vec4 axis = std::abs(normal.x) < 0.57735f ? constants::kUnitX : constants::kUnitY;
    t1 = Normalize3(Cross3(normal, axis));
    t2 = Cross3(normal, t1);
}

/// The effective mass along `direction` for a pair of bodies pinned at `rA`/`rB`.
f32 EffectiveMass(const Body& a, const Body& b, const Vec4& rA, const Vec4& rB,
                  const Vec4& direction) {
    const Vec4 crossA = Cross3(rA, direction);
    const Vec4 crossB = Cross3(rB, direction);
    return a.invMass + b.invMass +
           Dot3(direction, Cross3(a.invInertiaWorld.Transform(crossA), rA)) +
           Dot3(direction, Cross3(b.invInertiaWorld.Transform(crossB), rB));
}

Vec4 PointVelocity(const Body& body, const Vec4& r) {
    return body.linearVelocity + Cross3(body.angularVelocity, r);
}

}  // namespace

ContactSolver::ContactSolver(std::vector<Body>& bodies, const std::vector<Contact*>& contacts,
                             f32 dt)
    : bodies_(&bodies) {
    (void)dt;
    for (Contact* contact : contacts) {
        for (Manifold& manifold : contact->manifolds) {
            if (manifold.count == 0) {
                continue;
            }
            Body& a = bodies[static_cast<usize>(contact->bodyA)];
            Body& b = bodies[static_cast<usize>(contact->bodyB)];
            if (!a.IsDynamic() && !b.IsDynamic()) {
                continue;
            }

            Constraint c;
            c.manifold = &manifold;
            c.bodyA = contact->bodyA;
            c.bodyB = contact->bodyB;
            c.friction = contact->friction;
            c.restitution = contact->restitution;
            c.normal = manifold.normal;
            c.count = manifold.count;
            BuildTangents(c.normal, c.tangent[0], c.tangent[1]);

            const Mtx rotationA = RotationMatrix(a.transform.rotation);
            const Mtx rotationB = RotationMatrix(b.transform.rotation);
            Vec4 patch{};
            for (i32 i = 0; i < c.count; ++i) {
                const ManifoldPoint& mp = manifold.points[static_cast<usize>(i)];
                Point& p = c.points[static_cast<usize>(i)];
                p.rA = mp.point - a.worldCentre;
                p.rB = mp.point - b.worldCentre;
                // Held in each body's own frame so the position pass can re-derive the anchor
                // after the body has been rotated, rather than pushing along a stale lever arm.
                p.localA = Transpose3(rotationA).Transform(p.rA);
                p.localB = Transpose3(rotationB).Transform(p.rB);
                p.separation = mp.separation;
                p.impulse = mp.normalImpulse;

                const f32 k = EffectiveMass(a, b, p.rA, p.rB, c.normal);
                p.normalMass = k > 0.0f ? Rcp(k) : 0.0f;

                // Restitution is a coefficient on the approach speed, not a spring. Below the
                // threshold it is suppressed entirely, or a resting body bounces on its own
                // micro-impacts for ever.
                const f32 approach =
                    Dot3(c.normal, PointVelocity(b, p.rB) - PointVelocity(a, p.rA));
                p.velocityBias = approach < -kVelocityThreshold ? -c.restitution * approach : 0.0f;
                patch = patch + mp.point;
            }

            // One friction anchor for the whole patch, because friction here is one accumulated
            // impulse for the whole manifold, not one per point.
            patch = patch * (1.0f / static_cast<f32>(c.count));
            c.anchorA = patch - a.worldCentre;
            c.anchorB = patch - b.worldCentre;
            for (i32 t = 0; t < 2; ++t) {
                const f32 k = EffectiveMass(a, b, c.anchorA, c.anchorB, c.tangent[t]);
                c.tangentMass[t] = k > 0.0f ? Rcp(k) : 0.0f;
                c.tangentImpulse[t] = Dot3(manifold.frictionImpulse, c.tangent[t]);
            }
            constraints_.push_back(c);
        }
    }
}

void ContactSolver::ApplyImpulse(Constraint& c, const Vec4& impulse, const Vec4& rA,
                                 const Vec4& rB) {
    Body& a = (*bodies_)[static_cast<usize>(c.bodyA)];
    Body& b = (*bodies_)[static_cast<usize>(c.bodyB)];
    if (a.IsDynamic()) {
        a.linearVelocity = a.linearVelocity - impulse * a.invMass;
        a.angularVelocity =
            a.angularVelocity - a.invInertiaWorld.Transform(Cross3(rA, impulse));
    }
    if (b.IsDynamic()) {
        b.linearVelocity = b.linearVelocity + impulse * b.invMass;
        b.angularVelocity =
            b.angularVelocity + b.invInertiaWorld.Transform(Cross3(rB, impulse));
    }
}

void ContactSolver::WarmStart() {
    for (Constraint& c : constraints_) {
        for (i32 i = 0; i < c.count; ++i) {
            Point& p = c.points[static_cast<usize>(i)];
            ApplyImpulse(c, c.normal * p.impulse, p.rA, p.rB);
        }
        const Vec4 friction =
            c.tangent[0] * c.tangentImpulse[0] + c.tangent[1] * c.tangentImpulse[1];
        ApplyImpulse(c, friction, c.anchorA, c.anchorB);
    }
}

void ContactSolver::SolveVelocityConstraints() {
    for (Constraint& c : constraints_) {
        const Body& a = (*bodies_)[static_cast<usize>(c.bodyA)];
        const Body& b = (*bodies_)[static_cast<usize>(c.bodyB)];

        // Friction first, then the normal -- Box2D's order, and the two are coupled through the
        // bound, so swapping them changes where a sliding crate stops.
        f32 totalNormal = 0.0f;
        for (i32 i = 0; i < c.count; ++i) {
            totalNormal += c.points[static_cast<usize>(i)].impulse;
        }
        const f32 bound = c.friction * totalNormal;
        for (i32 t = 0; t < 2; ++t) {
            const f32 vt = Dot3(c.tangent[t],
                                PointVelocity(b, c.anchorB) - PointVelocity(a, c.anchorA));
            f32 lambda = -c.tangentMass[t] * vt;
            const f32 wanted = c.tangentImpulse[t] + lambda;
            const f32 clamped = std::clamp(wanted, -bound, bound);
            lambda = clamped - c.tangentImpulse[t];
            c.tangentImpulse[t] = clamped;
            ApplyImpulse(c, c.tangent[t] * lambda, c.anchorA, c.anchorB);
        }

        for (i32 i = 0; i < c.count; ++i) {
            Point& p = c.points[static_cast<usize>(i)];
            const f32 vn = Dot3(c.normal, PointVelocity(b, p.rB) - PointVelocity(a, p.rA));
            f32 lambda = -p.normalMass * (vn - p.velocityBias);
            // Accumulate and clamp the total rather than the increment: a contact may pull the
            // impulse back down between iterations, but never below zero, because it can push
            // and cannot pull.
            const f32 wanted = std::max(p.impulse + lambda, 0.0f);
            lambda = wanted - p.impulse;
            p.impulse = wanted;
            ApplyImpulse(c, c.normal * lambda, p.rA, p.rB);
        }
    }
}

bool ContactSolver::SolvePositionConstraints(f32 baumgarte) {
    f32 worst = 0.0f;
    for (Constraint& c : constraints_) {
        Body& a = (*bodies_)[static_cast<usize>(c.bodyA)];
        Body& b = (*bodies_)[static_cast<usize>(c.bodyB)];

        // Sequential within the manifold: each point re-derives its anchors from whatever the
        // point before it left, and the pose is carried through the loop rather than a sum
        // being applied at the end.
        //
        // The sequential form is load-bearing even though a summed pass produces almost the
        // same resting heights. The *rotation* nearly cancels the shortfall -- pushing at one
        // corner tilts the body down at the others, so the later points see close to the
        // separation they would have seen anyway -- which makes heights a poor test of the
        // difference. What the sum drops is entirely angular, and dropping it is what gives a
        // symmetric flat contact its slow yaw.
        for (i32 i = 0; i < c.count; ++i) {
            const Point& p = c.points[static_cast<usize>(i)];
            const Vec4 rA = RotationMatrix(a.transform.rotation).Transform(p.localA);
            const Vec4 rB = RotationMatrix(b.transform.rotation).Transform(p.localB);
            // The anchors coincided when the constraint was set up, so whatever the bodies have
            // moved since shows up here as the change in separation.
            const f32 separation =
                p.separation + Dot3((b.worldCentre + rB) - (a.worldCentre + rA), c.normal);
            worst = std::min(worst, separation);

            // Push out a fraction of the remaining overlap, and stop at linearSlop rather than
            // at zero: the 0.005 residue is pinned regression behaviour, and an implementation
            // that resolves to zero leaves every object resting 5 mm high with the error
            // compounding up a stack.
            //
            // Clamped at zero and **nowhere else**: there is no b2_maxLinearCorrection here.
            // Box2D caps a single iteration's push at 0.2 so a deep overlap cannot resolve as
            // an explosion; this pass deliberately has only the min against zero.
            const f32 correction =
                std::min(baumgarte * (separation + constants::kLinearSlop.x), 0.0f);
            if (correction == 0.0f) {
                continue;
            }

            // The effective mass is the one `Setup` stored, not one re-derived from the pose
            // this iteration has moved to. The anchors above track and this does not, which is
            // the asymmetry that keeps the pass stable while the body rotates under it.
            const Vec4 impulse = c.normal * (-correction * p.normalMass);
            if (a.IsDynamic()) {
                a.worldCentre = a.worldCentre - impulse * a.invMass;
                a.transform.rotation = IntegrateRotation(
                    a.transform.rotation, -a.invInertiaWorld.Transform(Cross3(rA, impulse)),
                    1.0f);
                a.SyncTransform();
            }
            if (b.IsDynamic()) {
                b.worldCentre = b.worldCentre + impulse * b.invMass;
                b.transform.rotation = IntegrateRotation(
                    b.transform.rotation, b.invInertiaWorld.Transform(Cross3(rB, impulse)),
                    1.0f);
                b.SyncTransform();
            }
        }
    }
    // Rest threshold: -1.5 slops of residual overlap is converged enough to sleep on. The test
    // is `-(slop + 0.5*slop) <= worst` -- deliberately tighter than Box2D's three slops, so a
    // body is held to a higher standard before its island is allowed to rest.
    return worst >= -1.5f * constants::kLinearSlop.x;
}

void ContactSolver::StoreImpulses() {
    for (Constraint& c : constraints_) {
        for (i32 i = 0; i < c.count; ++i) {
            c.manifold->points[static_cast<usize>(i)].normalImpulse =
                c.points[static_cast<usize>(i)].impulse;
        }
        // One vector for the patch, in world space -- not a tangent impulse per point.
        c.manifold->frictionImpulse =
            c.tangent[0] * c.tangentImpulse[0] + c.tangent[1] * c.tangentImpulse[1];
    }
}

}  // namespace snowball
