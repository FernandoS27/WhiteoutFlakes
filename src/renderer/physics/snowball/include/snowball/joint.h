//===----------------------------------------------------------------------===//
// snowball/joint.h -- the seven joint families and the solver that runs them.
//
// There are exactly seven families. They share a def prefix and diverge past it, and what each
// one actually constrains does not follow from its name -- three of the names are misleading,
// and the behaviours below are the contract, not naming bugs to repair:
//
//   * **Four families do not use the two-anchor rule at all.** `plane`, `revolute`, `shoulder`
//     and `weld` pin body B's *origin* to `localAnchorB` expressed in **A's** frame, and
//     `localAnchorA` has no effect on where B ends up. Only `distance` and `spherical` use
//     the symmetric `xfA*anchorA == xfB*anchorB` that the field names suggest.
//   * **A `revolute` with neutral frames is not a hinge.** It leaves all three rotational
//     degrees of freedom free, so it behaves as a ball joint on the anchor rule above.
//   * **`plane` is a prismatic joint wearing another name**, and with neutral frames it slides
//     along **z** and locks every rotation.
//
// The solver is the contact solver's shape -- a velocity pass then a position pass -- with one
// difference that is worth more than it looks: **the position pass is a full correction**. No
// Baumgarte factor, no `maxLinearCorrection` clamp. One iteration takes a two-unit error to
// zero. A port that reuses the contact position code here leaves a distance joint creeping
// toward its rest length over dozens of steps and a ragdoll visibly stretching under load.
//===----------------------------------------------------------------------===//
#pragma once

#include <vector>

#include "snowball/common_types.h"
#include "snowball/math.h"
#include "snowball/transform.h"

namespace snowball {

struct Body;

/// The family names, numbered from 1 -- the ids are part of the observable contract. `Plane`
/// is a prismatic joint under a user-facing name; the user-facing name wins.
enum class JointKind { Distance = 1, Mouse, Plane, Revolute, Shoulder, Spherical, Weld };

inline constexpr i32 kJointKindCount = 7;

/// A shoulder's cone angle is clamped into this range on construction -- 10 and 170 degrees.
/// A ragdoll authored with a zero cone gets a ten-degree one rather than a locked joint, and
/// one authored at 180 gets 170 rather than a joint that can invert.
inline constexpr f32 kMinConeAngle = 0.17453294f;
inline constexpr f32 kMaxConeAngle = 2.9670596f;

struct JointDef {
    JointKind kind{JointKind::Spherical};
    i32 bodyA{-1};
    i32 bodyB{-1};

    /// Read as a point by `distance` and `spherical`, and as a **quaternion** by `weld`, which
    /// is where its angular target comes from. See `Joint::angularTarget`.
    Vec4 localAnchorA{};
    Vec4 localAnchorB{};

    /// Quaternions, not axes -- the neutral value is (0,0,0,1). Writing a unit axis with w = 0
    /// here is a valid 180-degree rotation, and the weld and shoulder lock the bodies at it.
    Vec4 localFrameA{constants::kQuatIdentity};
    Vec4 localFrameB{constants::kQuatIdentity};

    bool collideConnected{false};

    f32 restLength{0.0f};        ///< distance only
    f32 coneAngle{0.0f};         ///< shoulder only, clamped on construction
    f32 lowerTwist{0.0f};
    f32 upperTwist{0.0f};
    bool enableTwistLimit{false};

    /// @name Angular spring — opt-in, off by default
    ///
    /// A soft angular drive pulling the shoulder toward its rest orientation. Authored ragdoll
    /// content carries no spring parameters, so nothing authored ever enables this pass; it
    /// exists for defs built by hand, and a caller must switch it on explicitly.
    ///
    /// The formulation is Box2D 2.x's soft constraint — `gamma = 1/(h(d + hk))`,
    /// `beta = hk*gamma`, with `k = m*omega^2` and `d = 2*m*zeta*omega` — generalised to a
    /// 3x3 angular system. Box3D (MIT, Erin Catto) is the structural reference for the 3D
    /// algebra, but *not* for the softness: it uses the newer
    /// `biasRate`/`massScale`/`impulseScale` form, which is a different formulation from this
    /// one.
    ///
    /// Defaults off, and stays off unless a caller opts in: no pinned regression behaviour
    /// covers it, so its numbers are free to change in a way the rest of the solver's are not.
    /// @{
    bool enableAngularSpring{false};
    f32 springHertz{0.0f};
    f32 springDamping{0.0f};
    /// Impulse ceiling, as `maxTorque * dt`. Zero means unclamped.
    f32 maxSpringTorque{0.0f};
    /// @}
};

struct Joint {
    JointKind kind{JointKind::Spherical};
    i32 bodyA{-1};
    i32 bodyB{-1};
    Vec4 localAnchorA{};
    Vec4 localAnchorB{};
    Vec4 localFrameA{constants::kQuatIdentity};
    Vec4 localFrameB{constants::kQuatIdentity};
    bool collideConnected{false};
    f32 restLength{0.0f};
    f32 coneAngle{0.0f};
    f32 lowerTwist{0.0f};
    f32 upperTwist{0.0f};
    bool enableTwistLimit{false};

    /// @brief Which side of its twist range a shoulder is sitting on.
    ///
    /// The limit state machine follows Box2D's revolute shape, but the values 1/2/3/4 are the
    /// observable contract: the state is seeded to 1 on creation and rewritten every step, so
    /// the numbering is not free to slide back to a zero-based one.
    enum class TwistState : i32 { Inactive = 1, AtLower = 2, AtUpper = 3, Equal = 4 };

    /// Solver state, carried between steps so the limit warm-starts. Reset when the limit
    /// changes which side it is on, and *not* when it stays put -- see `InitTwist`.
    TwistState twistState{TwistState::Inactive};
    f32 twistImpulse{0.0f};

    /// See @ref JointDef's angular spring — opt-in, off by default.
    bool enableAngularSpring{false};
    f32 springHertz{0.0f};
    f32 springDamping{0.0f};
    f32 maxSpringTorque{0.0f};
    Vec4 springImpulse{};

    /// @brief The orientation the weld's angular half drives B to, relative to A.
    ///
    /// The revolute reads the same slot as its hinge-reference quaternion (see
    /// `SolveHingeVelocity`), so `localAnchorA` is the angular-authoring slot for two of the
    /// four asymmetric-anchor families and dead weight for the other two.
    ///
    /// **`localAnchorA`, read as a quaternion and deliberately NOT normalised.** The length
    /// acts as the gain on the position correction: at length 1 the weld snaps to the target
    /// in a single step; at 0.25 it converges over about thirty; at 2 it oscillates past and
    /// crawls back; at 7 it never arrives at all and parks 77 degrees short. A def authoring
    /// anything but a unit quaternion here gets an attachment held at the wrong angle with
    /// nothing reporting it -- that is pinned behaviour, part of the engine's contract, and
    /// normalising here would silently retune every such def.
    ///
    /// All zeros -- the neutral def -- is a zero-length target, so the position half applies no
    /// correction and the weld holds whatever orientation it was created at. The velocity half
    /// still locks the relative angular velocity, so it is rigid either way.
    Vec4 angularTarget() const { return localAnchorA; }
};

/// @brief Sequential impulses over the joints of one island.
///
/// Deliberately a separate object from `ContactSolver` rather than a mode of it: the two share
/// their shape and nothing else, and the position pass in particular is a different algorithm.
class JointSolver {
public:
    /// @param dt needed only by the opt-in angular spring, whose softness is a function of the
    ///        step; every other pass is scale-free and ignores it.
    JointSolver(std::vector<Body>& bodies, const std::vector<Joint*>& joints, f32 dt = 0.0f);

    void SolveVelocityConstraints();

    /// @brief Drive the remaining error out. Returns whether every joint is within slop.
    bool SolvePositionConstraints();

    i32 ConstraintCount() const { return static_cast<i32>(joints_.size()); }

private:
    /// Where a family's two attachment points live, resolved once per pass. Both are offsets
    /// from a body's centre of mass, which is what the effective mass is expressed about.
    struct Anchors {
        Vec4 rA{};
        Vec4 rB{};
        Vec4 worldA{};
        Vec4 worldB{};
    };

    Body& BodyA(const Joint& joint) const;
    Body& BodyB(const Joint& joint) const;

    Anchors Resolve(const Joint& joint) const;
    Mtx PointMass(const Joint& joint, const Anchors& a) const;

    void SolvePointVelocity(const Joint& joint);
    void SolveDistanceVelocity(const Joint& joint);
    void SolveAngularVelocity(const Joint& joint);

    /// @brief Stop a shoulder tilting further off its twist axis than the cone allows.
    void SolveConeVelocity(const Joint& joint);

    /// @brief Decide which side of its range each shoulder's twist is on, once per step.
    ///
    /// Settled before the iterations begin and never inside them, which matters: a state
    /// recomputed per iteration can flip mid-solve and fight itself.
    void InitTwist();

    /// @brief Hold a shoulder's rotation *about* its own axis inside [lower, upper].
    ///
    /// Without this pass a shoulder is a cone and nothing more, so a chain of them stays
    /// where it is put but spins freely about its own length -- which on cloth reads as
    /// strips corkscrewing for no reason.
    void SolveTwistVelocity(Joint& joint);

    /// @brief Pull a shoulder back toward its rest orientation. **Opt-in — see @ref JointDef.**
    void SolveAngularSpringVelocity(Joint& joint);

    f32 SolvePointPosition(const Joint& joint);
    f32 SolveDistancePosition(const Joint& joint);
    f32 SolveAngularPosition(const Joint& joint);

    /// @brief Push a shoulder back *inside* its cone — the restoring half of the limit.
    ///
    /// `SolveConeVelocity` can only cancel an opening angular velocity, so it stops a joint
    /// going further out and can never bring one back: `opening <= 0` returns early, and a link
    /// sitting outside its cone with no outward velocity stays there for good. Without this
    /// pass the overshoot is a ratchet rather than an oscillation, and authored ragdoll content
    /// trips it on frame one — authored frames can sit up to 15 degrees apart at bind pose,
    /// wider than several of the cones they are paired with, so those joints begin violated
    /// with nothing else able to restore them.
    f32 SolveConePosition(const Joint& joint);

    /// @brief Drive a shoulder's twist back inside [lower, upper], per its latched state.
    f32 SolveTwistPosition(const Joint& joint);

    /// @brief A revolute's 2-DOF angular lock: no rotation off the hinge axis.
    ///
    /// The hinge is defined by **`localAnchorA` read as a quaternion** — the weld's storage
    /// convention on a second family, and the only angular quaternion most authored defs
    /// carry. Its z axis in A's frame is the hinge axis; a zero reference leaves the whole
    /// angular half inert, a deliberate degeneracy the default def hits — see
    /// `RevoluteReference` before "fixing" it. The lock's Jacobian is the two tangent rows of
    /// the reference frame, solved through a **2x2** effective mass. The limit is byte-for-byte
    /// the shoulder's twist machinery — same Pade atan2, same 0.0698 Equal epsilon, same
    /// 1/2/3/4 state field — which is why Revolute reuses
    /// `InitTwist`/`SolveTwistVelocity`/`SolveTwistPosition` rather than owning copies.
    void SolveHingeVelocity(const Joint& joint);

    /// @brief Position half of the lock: rotate the two hinge axes back into alignment.
    ///
    /// A full correction, not a Baumgarte fraction: only the *limit* correction is scaled by
    /// 0.2; the alignment is an equality constraint and is driven out whole, like the weld's.
    f32 SolveHingePosition(const Joint& joint);

    void ApplyLinear(const Joint& joint, const Vec4& impulse, const Anchors& a);
    void ApplyAngular(const Joint& joint, const Vec4& impulse);

    std::vector<Body>* bodies_{nullptr};
    std::vector<Joint*> joints_;
    f32 dt_{0.0f};  ///< only the opt-in angular spring reads this
};

/// @brief Which axis of A's joint frame a family leaves free, if any.
///
/// `plane` slides along z and locks every rotation, `shoulder` twists freely about z and is
/// limited off it, and `revolute` with neutral frames constrains no rotation at all.
Vec4 JointFreeAxis(const Joint& joint);

/// @brief Does this family lock the relative orientation?
bool JointLocksRotation(JointKind kind);

}  // namespace snowball
