//===----------------------------------------------------------------------===//
// snowball/scene.h -- bodies, fixtures, contacts, and the step that advances them.
//
// The step has the classic Box2D shape: broadphase pairs, narrowphase manifolds matched onto
// the previous step's by feature id, semi-implicit Euler, then the two solver loops. Four
// choices here are engine contract and easy to "correct" into a different engine:
//
//   * **Damping is `v *= (1 - h*d)`**, not `v /= (1 + h*d)`. The two agree to first order and
//     part company fast; past `h*d >= 1` the factor is zero or negative and the velocity is
//     killed outright, so heavy damping stops a body dead in one step. Hosts tune against
//     that cliff, so easing it is a behaviour change, not a fix.
//   * **Integration is semi-implicit Euler exactly** -- velocity first, then position using the
//     new velocity, which makes free fall an exact arithmetic series rather than merely
//     something that accelerates downward.
//   * **A contact exists long before it touches.** Proxies are fattened by +/-0.1, so a pair
//     0.11 apart already has one. Treating existence as touching double-counts every near miss.
//   * **Friction accumulates per manifold**, not per point (see contact.h).
//
// Sleep and islands are here too, and they are one mechanism rather than two: a body does not
// fall asleep, a *batch* does, all of its members on the same step. Islands form from
// **touching** contacts, never from proximity -- a body still in the air is its own island
// however close it is about to come -- and islands are then packed, in body-pool order, into
// solver batches of at least sixteen bodies; the sleep decision spans the whole batch, so one
// restless island deliberately keeps its fifteen quiet neighbours awake. Sleeping each island
// on its own schedule looks like an optimisation and changes when whole scenes go quiet.
//
// After the solve comes continuous collision, scoped to **dynamic against static, nothing
// else** -- no bullet flag exists and a fast body tunnels through a slow dynamic one. That
// scope is contract: hosts pace fast movers against it, and widening it changes which pairs
// can tunnel. A body is "fast" when this step's motion exceeds a quarter of its thinnest
// feature (`IsFast`); a fast body is swept against its static contacts, advanced to its
// earliest impact, solved cold there, and re-integrated for the remainder -- up to twelve
// times per step before it gives up (scene.cpp, toi.h).
//
// Waking has exactly one cause: being reached from an awake body across a touching contact.
// **Nothing in the public API wakes anything** -- not a velocity, not a position, not SetActive.
// A sleeping body teleported by gameplay simply hangs at its new position, and a velocity handed
// to it is never integrated. That is a contract hosts design around, and an engine that
// "helpfully" wakes on the setters diverges everywhere a game relied on it.
//===----------------------------------------------------------------------===//
#pragma once

#include <unordered_map>
#include <utility>
#include <vector>

#include "snowball/aabb_tree.h"
#include "snowball/common_types.h"
#include "snowball/contact.h"
#include "snowball/joint.h"
#include "snowball/shape.h"
#include "snowball/toi.h"
#include "snowball/transform.h"

namespace snowball {

/// How long a body must be quiet before its island sleeps. Stored **per body** rather than
/// as a global engine constant, which is why it is a default and not a constant here.
inline constexpr f32 kTimeToSleep = 0.5f;

/// Below these a body counts as quiet: 0.05 m/s and five degrees per second. Round numbers in
/// their own units -- chosen values, not drift -- and deliberately looser than the defaults
/// engines of this shape usually carry.
///
/// The looseness is load-bearing and easy to mistake for slack worth tightening: under
/// tighter tolerances a two-box stack rings just loudly enough after the impact to keep
/// resetting its own sleep timer, and the island sleeps seven steps late while every part of
/// the simulation looks right. The difference is worth about five per cent of a settling
/// time.
inline constexpr f32 kLinearSleepTolerance = 0.05f;
inline constexpr f32 kAngularSleepTolerance = 5.0f / 180.0f * 3.1415927410125732f;

using ShapeId = i32;
using BodyId = i32;
using ContactId = i32;
using JointId = i32;

/// @brief Shapes, owned in one place so a fixture can name one without owning it.
class ShapeStore {
public:
    ShapeId Add(Shape shape);
    const Shape& Get(ShapeId id) const { return shapes_[static_cast<usize>(id)]; }
    bool Contains(ShapeId id) const {
        return id >= 0 && id < static_cast<ShapeId>(shapes_.size());
    }
    void Clear() { shapes_.clear(); }

private:
    std::vector<Shape> shapes_;
};

struct Fixture {
    ShapeId shape{-1};
    f32 density{1.0f};
    f32 friction{0.5f};
    f32 restitution{0.0f};
    i32 proxy{kNullNode};   ///< this fixture's node in the broadphase tree

    /// The shape's swept bounds, start pose to end pose, **before** the broadphase fattens
    /// them. The mesh contact's triangle query reads this box and must keep reading it: the
    /// fat proxy AABB would pull in a band of triangles two slop widths wide that the
    /// narrowphase then rejects one by one.
    Aabb aabb{};
};

/// @brief Body type, with dynamic deliberately numbered 0 and static 2.
///
/// The numbering matches the reference engine and is relied on wherever a type arrives as a
/// raw integer; renumbering it to a more familiar convention produces a body that silently
/// never falls.
///
/// Types 1 and 2 both come out with zero inverse mass, so neither responds to gravity and neither
/// takes an impulse. Their *only* observable difference is integration: a kinematic body advances
/// its position from a hand-set velocity, a static one ignores velocity entirely. Give both
/// `v = (2,0,0)` and after n steps the kinematic body sits at exactly `v*dt*n` while the
/// static body has not moved.
enum class BodyType : i32 { Dynamic = 0, Kinematic = 1, Static = 2 };

/// @brief Everything a body is authored with. Defaults are a plain dynamic body.
struct BodyDef {
    Vec4 position{};
    BodyType type{BodyType::Dynamic};
    f32 linearDamping{0.0f};
    f32 angularDamping{0.0f};
    f32 gravityScale{1.0f};
    f32 inertiaScale{1.0f};
    bool active{true};

    /// Separate on purpose: a body can be created asleep that was never allowed to sleep.
    bool allowSleep{false};
    bool sleeping{false};
};

struct Body {
    Transform transform{};
    Vec4 linearVelocity{};
    Vec4 angularVelocity{};

    /// The centre of mass, which is what actually integrates: `transform.position` is derived
    /// from it. They coincide for a centred fixture and part company for an offset one, and
    /// rotating about the origin instead is wrong in a way that looks right for every centred
    /// test case.
    Vec4 worldCentre{};
    Vec4 localCentre{};

    BodyType type{BodyType::Dynamic};
    bool active{true};
    bool allowSleep{false};
    bool sleeping{false};

    /// How long this body has been quiet, and how long it must be. The timer is **not** reset
    /// when the body finally sleeps -- it reads back at `timeToSleep` on that step, and stays
    /// there until Wake clears it. Observable behaviour, not an oversight to tidy.
    f32 sleepTime{0.0f};
    f32 timeToSleep{kTimeToSleep};

    f32 linearDamping{0.0f};
    f32 angularDamping{0.0f};
    f32 gravityScale{1.0f};
    f32 inertiaScale{1.0f};

    f32 mass{0.0f};
    f32 invMass{0.0f};
    Mtx inertia{};        ///< about the centre of mass, in the body frame
    Mtx invInertia{};
    Mtx invInertiaWorld{};

    i32 lastStep{0};      ///< the step this body was last integrated on
    std::vector<Fixture> fixtures;

    /// Where this step started and where it ended, for continuous collision. Rebuilt at the
    /// top of every step; only the TOI machinery ever moves the start.
    Sweep sweep{};

    /// The thinnest feature and the farthest reach across the fixtures, filled by the mass
    /// recompute, which folds them in the same loop as the mass. A body with no fixtures
    /// keeps {FLT_MAX, 0}, which can never qualify as fast.
    f32 minExtent{constants::kMaxFloat.x};
    f32 maxExtent{0.0f};

    /// Sub-steps consumed this step; at kMaxToiSubSteps the body gives up and finalises at
    /// the step end. Reset every step the body qualifies as fast, so the budget is per step.
    i32 toiCount{0};

    bool IsStatic() const { return type == BodyType::Static; }
    bool IsKinematic() const { return type == BodyType::Kinematic; }
    bool IsDynamic() const { return type == BodyType::Dynamic && active; }

    /// Integrated, but never solved: no gravity, no impulse, no island membership. The velocity
    /// is whatever the host last set, and it moves the body and nothing else.
    bool IsKinematicallyDriven() const { return IsKinematic() && active; }

    /// Dynamic *and* awake -- what actually gets integrated and takes impulses.
    bool IsSimulated() const { return IsDynamic() && !sleeping; }

    bool IsQuiet() const {
        return LengthSquared3(linearVelocity) <= kLinearSleepTolerance * kLinearSleepTolerance &&
               LengthSquared3(angularVelocity) <=
                   kAngularSleepTolerance * kAngularSleepTolerance;
    }

    /// @brief Re-derive the world-frame inverse inertia and the body origin after a rotation.
    void SyncTransform();
};

class Scene {
public:
    Scene(ShapeStore& shapes, const Vec4& gravity) : shapes_(&shapes), gravity_(gravity) {}

    BodyId AddBody(const BodyDef& def);

    /// @brief Attach a fixture and recompute the body's mass from all of them.
    ///
    /// Recomputing on every attach rather than lazily is what makes a body's mass a function of
    /// its fixtures at all times -- a freshly built box answers for its weight before the
    /// first step ever runs.
    void AddFixture(BodyId body, ShapeId shape, f32 density, f32 friction, f32 restitution);

    /// @brief Connect two bodies. Returns the new joint, or -1 if either body is unknown.
    ///
    /// A joint with `collideConnected` false destroys the contact between its two bodies **on
    /// creation** as well as suppressing new ones, which is what stops two jointed limbs of a
    /// ragdoll fighting each other from the step they are attached.
    JointId AddJoint(const JointDef& def);

    const Joint& GetJoint(JointId joint) const { return joints_[static_cast<usize>(joint)]; }
    i32 JointCount() const { return static_cast<i32>(joints_.size()); }

    /// @brief Advance the world. Pairs, manifolds, integration, then the two solver loops.
    void Step(f32 dt, i32 velocityIterations = 8, i32 positionIterations = 3);

    const Body& Get(BodyId body) const { return bodies_[static_cast<usize>(body)]; }
    bool Contains(BodyId body) const {
        return body >= 0 && body < static_cast<BodyId>(bodies_.size());
    }
    i32 BodyCount() const { return static_cast<i32>(bodies_.size()); }
    const Vec4& Gravity() const { return gravity_; }
    const AabbTree& Tree() const { return tree_; }
    i32 StepCount() const { return stepStamp_; }

    void SetLinearVelocity(BodyId body, const Vec4& v);
    void SetAngularVelocity(BodyId body, const Vec4& w);
    void SetPosition(BodyId body, const Vec4& p);

    /// @brief Place a body outright, position and rotation together.
    ///
    /// Hosts drive posed bodies through this rather than by position alone -- an
    /// animation-driven ragdoll root is snapped most of the way to its target pose every
    /// frame, orientation included. Setting the position and leaving the rotation behind is
    /// the failure that looks almost right.
    void SetTransform(BodyId body, const Transform& xf);

    void SetActive(BodyId body, bool active);

    /// @brief Change a body's type after creation, as `dmBody_SetType` does.
    ///
    /// A host needs this: StarCraft II flips a rigid body between kinematic and dynamic every
    /// frame from an animated channel, so the same body is an animation-driven collision proxy
    /// one frame and a falling ragdoll segment the next.
    ///
    /// Three things happen besides the assignment, and the last is the one that is not obvious:
    ///
    ///  - becoming dynamic recomputes the mass from the fixtures, and becoming anything else
    ///    zeroes it — mass is a property of the *type* as much as of the shapes (@ref
    ///    RecomputeMass), so a body that changed type without this is a ragdoll that never falls;
    ///  - becoming static also drops both velocities, since a static body ignores them;
    ///  - **every contact touching the body is destroyed.** `ShouldCollide` is consulted when a
    ///    pair is created and never again, so a pair that formed while the body was dynamic
    ///    outlives the change and keeps solving between two bodies that can no longer move
    ///    relative to each other. The pairs re-form next step against the new type.
    ///
    /// The centre of mass is deliberately **not** cleared on the way down to kinematic or static.
    /// `transform.position` is derived from `worldCentre` and `localCentre`, so zeroing one of
    /// them here teleports the body by its centre-of-mass offset — a small sideways jump on
    /// exactly the frame a limb goes limp.
    ///
    /// Sleep is the caller's business, as it is in the reference engine: both of its callers
    /// wake the body themselves first, and waking is what zeroes the velocities there.
    void SetBodyType(BodyId body, BodyType type);

    /// @brief Wake a body. Deliberately **not** reachable from any setter above.
    void Wake(Body& body) const;

    // -- contacts
    /// Live contacts, which is not the number of slots ever used: a destroyed contact frees its
    /// slot and the high-water mark stays where it was.
    i32 ContactCount() const { return liveContacts_; }

    /// The first live contact, or -1.
    ContactId FirstContact() const;

    /// The next live contact after @p id, or -1. Slots are reused, so walking raw ids skips
    /// dead ones -- this and @ref FirstContact are the only sound way to enumerate.
    ContactId NextContact(ContactId id) const;

    const Contact* GetContact(ContactId id) const;
    Contact* GetContact(ContactId id);

private:
    struct ContactSlot {
        Contact contact;
        bool alive{false};
        i32 nextFree{-1};
    };

    /// One connected group of bodies, joined by touching contacts **or by a joint**. Solved
    /// together. A joint binds its two bodies into one island whether or not they touch, so a
    /// hanging bob and its anchor share an island. Sleep is decided one level up: islands are
    /// packed into batches (`ApplySleepBatches`) and a batch sleeps as one.
    struct Island {
        std::vector<i32> bodies;
        std::vector<Contact*> contacts;
        std::vector<Joint*> joints;
        /// Set by SolveIsland: positions converged and every dynamic member has been quiet for
        /// its full timeToSleep. An island is never slept on its own say-so -- see the batch.
        bool ready{false};
    };

    void RecomputeMass(Body& body) const;
    void UpdatePairs();
    void Collide();

    /// @brief Rebuild one contact's manifolds at the bodies' current poses -- Collide's body,
    /// and what the TOI advance calls on the contacts it moved.
    ///
    /// `invalidateSat` is the TOI advance's flag: the body was just teleported to its impact
    /// pose, so every cached SAT axis is cooled before colliding. The GJK caches are NOT
    /// cleared -- they warm-start straight through an advance, and clearing them is the
    /// mistake that looks like tidiness.
    void CollideContact(Contact& contact, bool invalidateSat = false);

    /// @brief The mesh half of the above: triangle buffer, per-triangle narrowphase, reduction.
    void CollideMeshContact(Contact& contact, bool invalidateSat);

    /// @brief Partition the awake bodies, waking anything a touching contact reaches.
    void BuildIslands();

    void SolveIsland(Island& island, f32 dt, i32 velocityIterations, i32 positionIterations);
    bool UpdateSleepTimers(const Island& island, f32 dt, bool positionsSolved);
    void ApplySleepBatches();
    void IntegrateVelocities(const std::vector<i32>& bodies, f32 dt);
    void IntegratePositions(const std::vector<i32>& bodies, f32 dt);
    void IntegrateKinematic(f32 dt);
    void SynchronizeBody(Body& body);
    void SynchronizeProxies();

    // -- continuous collision (see the file comment: dynamic against static, nothing else)
    bool IsFast(const Body& body, f32 dt) const;
    void FinalizeSweep(Body& body);
    void ProcessToiCandidate(usize slot);
    void SolveToiIsland(i32 body, f32 dt, i32 velocityIterations);
    void SolveToi(f32 dt, i32 velocityIterations);
    bool ShouldCollide(const Body& a, const Body& b) const;
    ContactId AllocateContact();
    void DestroyContact(ContactId id);

    ShapeStore* shapes_;
    Vec4 gravity_{};
    std::vector<Body> bodies_;
    AabbTree tree_;

    std::vector<ContactSlot> contacts_;
    std::vector<Joint> joints_;
    std::unordered_map<u64, ContactId> pairs_;
    i32 freeContact_{-1};
    i32 liveContacts_{0};
    u64 nextManifoldToken_{1};
    i32 stepStamp_{0};

    /// Scratch reused across steps.
    std::vector<Island> islands_;
    std::vector<u8> bodyInIsland_;
    std::vector<u8> contactInIsland_;
    std::vector<u8> jointInIsland_;
    std::vector<i32> stack_;
    std::vector<std::pair<i32, i32>> batchOrder_;
    std::vector<i32> staticBatchSeen_;
    i32 staticBatchStamp_{0};

    std::vector<i32> toiCandidates_;
    std::vector<f32> toiFractions_;
    std::vector<u8> toiHits_;
    std::vector<Contact*> toiContacts_;
};

}  // namespace snowball
