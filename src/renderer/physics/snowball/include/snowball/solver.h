//===----------------------------------------------------------------------===//
// snowball/solver.h -- sequential impulses over the contact manifolds.
//
// A two-loop sequential-impulse solver in the Box2D family, taken to 3D: a velocity pass
// that stops bodies and a position pass that pushes the remaining overlap out, with the
// accumulated normal impulse carried between steps by feature id.
//
// Three things here diverge from the Box2D's and are teh same as Domino's, and each is deliberate contract:
//
//   * **Friction is one impulse per manifold**, not one per point. Box2D keeps a tangent impulse
//     on every manifold point; this solver accumulates `t1*l1 + t2*l2` for the whole patch.
//     Stored as a world vector so it survives the tangent basis being rebuilt each step.
//   * **The reciprocals are approximate.** Every effective mass here goes through `Rcp`, and the
//     approximation is part of the numeric contract -- swapping in `1.0f / k` changes every
//     trajectory from the first step and the difference compounds.
//   * **The position pass leaves `linearSlop` of overlap behind**, and the constant is pinned
//     regression behaviour: a unit box rests at half + both margins - 0.005, and omitting the
//     slop leaves every object resting 5 mm high with the error compounding up a stack.
//
// The sharpest check on the whole file is one number: a resting body's normal impulses sum to
// exactly the impulse gravity applied over the step. It ties the integrator, the manifold and
// the solver together and it is exact rather than approximate.
//===----------------------------------------------------------------------===//
#pragma once

#include <array>
#include <vector>

#include "snowball/common_types.h"
#include "snowball/scene.h"

namespace snowball {

/// Box2D's b2_baumgarte: the fraction of the remaining overlap one position iteration removes.
inline constexpr f32 kBaumgarte = 0.2f;

/// There is deliberately no `b2_maxLinearCorrection` here. Box2D caps one iteration's push at
/// 0.2 so a deep overlap cannot resolve as an explosion; this solver clamps the correction
/// against zero and nothing else, so a body far enough inside another is pushed out as hard as
/// the arithmetic says. Adding the cap would change deep-overlap recovery, so it stays out.

/// Approach speeds below this bounce with no restitution, whatever the material. Without it a
/// resting body jitters for ever on its own micro-impacts.
inline constexpr f32 kVelocityThreshold = 1.0f;

class ContactSolver {
public:
    ContactSolver(std::vector<Body>& bodies, const std::vector<Contact*>& contacts, f32 dt);

    /// @brief Re-apply the impulses inherited from the previous step, before iterating.
    ///
    /// This *is* warm starting: the iterations then only have to find the correction, not the
    /// whole impulse, which is why eight of them hold a stack that would otherwise sag.
    void WarmStart();

    void SolveVelocityConstraints();

    /// @brief Push the remaining overlap out. Returns whether the island is close enough to
    /// solved to be allowed to sleep -- a stack still being forced apart is not at rest, however
    /// slowly its bodies happen to be moving.
    ///
    /// The baumgarte is an argument rather than a constant because two callers pass two
    /// values: the main solve's 0.2 and the TOI island's 0.75.
    bool SolvePositionConstraints(f32 baumgarte = kBaumgarte);

    /// @brief Write the accumulated impulses back onto the manifolds, for the next step to find.
    void StoreImpulses();

    i32 ConstraintCount() const { return static_cast<i32>(constraints_.size()); }

private:
    struct Point {
        Vec4 rA{};
        Vec4 rB{};
        Vec4 localA{};   ///< the anchor in each body's frame, so the position pass can re-derive it
        Vec4 localB{};
        f32 normalMass{0.0f};
        f32 velocityBias{0.0f};
        f32 separation{0.0f};
        f32 impulse{0.0f};
    };

    struct Constraint {
        Manifold* manifold{nullptr};
        i32 bodyA{-1};
        i32 bodyB{-1};
        f32 friction{0.0f};
        f32 restitution{0.0f};
        Vec4 normal{};
        Vec4 tangent[2]{};
        std::array<Point, 4> points{};
        i32 count{0};
        Vec4 anchorA{};        ///< the friction patch, which is one anchor for the whole manifold
        Vec4 anchorB{};
        f32 tangentMass[2]{};
        f32 tangentImpulse[2]{};
    };

    void ApplyImpulse(Constraint& c, const Vec4& impulse, const Vec4& rA, const Vec4& rB);

    std::vector<Body>* bodies_{nullptr};
    std::vector<Constraint> constraints_;
};

}  // namespace snowball
