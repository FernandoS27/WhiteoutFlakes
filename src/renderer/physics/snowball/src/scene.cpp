#include "snowball/scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

#include "snowball/joint.h"
#include "snowball/mesh_contact.h"
#include "snowball/solver.h"

namespace snowball {
namespace {

bool Finite3(const Vec4& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool Finite4(const Vec4& v) { return Finite3(v) && std::isfinite(v.w); }

Vec4 Rotate(const Vec4& q, const Vec4& v) { return RotationMatrix(q).Transform(v); }

Vec4 InvRotate(const Vec4& q, const Vec4& v) {
    return Transpose3(RotationMatrix(q)).Transform(v);
}

/// The inverse of a rigid transform, for carrying world boxes into a mesh's own frame.
Transform InverseTransform(const Transform& xf) {
    Transform out;
    out.rotation = Vec4{-xf.rotation.x, -xf.rotation.y, -xf.rotation.z, xf.rotation.w};
    out.position = -Rotate(out.rotation, xf.position);
    return out;
}

/// A `TriangleSource` view over whichever mesh kind a fixture's shape holds. Owns the
/// concrete source so a caller can hold one reference across the whole collide.
struct MeshSourceView {
    std::optional<HeightFieldSource> field;
    std::optional<TreeMeshSource> tree;
    const TriangleSource* source{nullptr};

    explicit MeshSourceView(const Shape& shape) {
        if (const HeightField* f = std::get_if<HeightField>(&shape)) {
            source = &field.emplace(*f);
        } else {
            source = &tree.emplace(std::get<TreeMesh>(shape));
        }
    }
};

/// Which entry point a pair routes to depends on the shape kinds, and the nine entries are
/// written for one ordering apiece. Rank the kinds and put the higher one first; the manifold
/// normal points from the first shape to the second, so a swap flips it.
i32 DispatchRank(ShapeType type) {
    switch (type) {
        case ShapeType::Triangle: return 3;
        case ShapeType::Polytope: return 2;
        case ShapeType::Capsule: return 1;
        case ShapeType::Sphere:
        case ShapeType::TreeMesh:
        case ShapeType::HeightField: break;
    }
    return 0;
}

ContactManifold CollidePair(const Shape& a, const Shape& b, SatCache* cache = nullptr) {
    switch (TypeOf(a)) {
        case ShapeType::Triangle:
            switch (TypeOf(b)) {
                case ShapeType::Sphere:
                    return CollideTriangleSphere(std::get<Triangle>(a), std::get<Sphere>(b));
                case ShapeType::Capsule:
                    return CollideTriangleCapsule(std::get<Triangle>(a), std::get<Capsule>(b));
                default:
                    return CollideTrianglePolytope(std::get<Triangle>(a), std::get<Polytope>(b),
                                                   Transform{});
            }
        case ShapeType::Polytope:
            switch (TypeOf(b)) {
                case ShapeType::Sphere:
                    return CollidePolytopeSphere(std::get<Polytope>(a), std::get<Sphere>(b));
                case ShapeType::Capsule:
                    return CollidePolytopeCapsule(std::get<Polytope>(a), std::get<Capsule>(b));
                default:
                    // The only pair the contact's SAT cache ever reaches -- the convex
                    // dispatch routes it here and nowhere else.
                    return CollidePolytopePolytope(std::get<Polytope>(a), std::get<Polytope>(b),
                                                   Transform{}, cache);
            }
        case ShapeType::Capsule:
            if (TypeOf(b) == ShapeType::Sphere) {
                return CollideCapsuleSphere(std::get<Capsule>(a), std::get<Sphere>(b));
            }
            return CollideCapsuleCapsule(std::get<Capsule>(a), std::get<Capsule>(b));
        case ShapeType::Sphere:
        case ShapeType::TreeMesh:
        case ShapeType::HeightField:
            break;  // meshes never reach the convex dispatch; contacts branch on IsMesh first
    }
    return CollideSphereSphere(std::get<Sphere>(a), std::get<Sphere>(b));
}

u64 PairKey(i32 proxyA, i32 proxyB) {
    const u64 lo = static_cast<u64>(std::min(proxyA, proxyB));
    const u64 hi = static_cast<u64>(std::max(proxyA, proxyB));
    return (hi << 32) | lo;
}

u64 ProxyData(i32 body, i32 fixture) {
    return (static_cast<u64>(body) << 32) | static_cast<u32>(fixture);
}

i32 ProxyBody(u64 data) { return static_cast<i32>(data >> 32); }
i32 ProxyFixture(u64 data) { return static_cast<i32>(data & 0xFFFFFFFF); }

}  // namespace

void Body::SyncTransform() {
    const Mtx rotation = RotationMatrix(transform.rotation);
    transform.position = worldCentre - rotation.Transform(localCentre);
    // The tensor is authored in the body frame; the solver needs it in the world frame, so it
    // has to be rotated rather than used as stored. R * invI * R^T.
    invInertiaWorld = Concat3(Concat3(rotation, invInertia), Transpose3(rotation));
}

ShapeId ShapeStore::Add(Shape shape) {
    shapes_.push_back(std::move(shape));
    return static_cast<ShapeId>(shapes_.size()) - 1;
}

BodyId Scene::AddBody(const BodyDef& def) {
    Body body;
    body.transform.position = def.position;
    body.worldCentre = def.position;
    body.type = def.type;
    body.active = def.active;
    body.linearDamping = def.linearDamping;
    body.angularDamping = def.angularDamping;
    body.gravityScale = def.gravityScale;
    body.inertiaScale = def.inertiaScale;
    body.allowSleep = def.allowSleep;
    body.sleeping = def.sleeping;
    body.SyncTransform();
    body.sweep.c0 = body.sweep.c = body.worldCentre;
    body.sweep.q0 = body.sweep.q = body.transform.rotation;
    bodies_.push_back(std::move(body));
    return static_cast<BodyId>(bodies_.size()) - 1;
}

void Scene::AddFixture(BodyId body, ShapeId shape, f32 density, f32 friction, f32 restitution) {
    if (!Contains(body) || !shapes_->Contains(shape)) {
        return;
    }
    Body& b = bodies_[static_cast<usize>(body)];
    Fixture fixture{shape, density, friction, restitution, kNullNode};
    fixture.aabb = ComputeAabb(shapes_->Get(shape), b.transform);
    fixture.proxy = tree_.CreateProxy(fixture.aabb,
                                      ProxyData(body, static_cast<i32>(b.fixtures.size())));
    b.fixtures.push_back(fixture);
    RecomputeMass(b);
}

JointId Scene::AddJoint(const JointDef& def) {
    if (!Contains(def.bodyA) || !Contains(def.bodyB)) {
        return -1;
    }
    Joint joint;
    joint.kind = def.kind;
    joint.bodyA = def.bodyA;
    joint.bodyB = def.bodyB;
    joint.localAnchorA = def.localAnchorA;
    joint.localAnchorB = def.localAnchorB;
    joint.localFrameA = def.localFrameA;
    joint.localFrameB = def.localFrameB;
    joint.collideConnected = def.collideConnected;
    joint.restLength = def.restLength;
    joint.lowerTwist = def.lowerTwist;
    joint.upperTwist = def.upperTwist;
    joint.enableTwistLimit = def.enableTwistLimit;
    joint.enableAngularSpring = def.enableAngularSpring;
    joint.springHertz = def.springHertz;
    joint.springDamping = def.springDamping;
    joint.maxSpringTorque = def.maxSpringTorque;

    // Clamped here rather than at the point of use: the clamped value is what gets stored, so
    // reading a shoulder's cone back gives the angle it will actually enforce and not the one
    // the def asked for. Zero becomes ten degrees, not a locked joint.
    joint.coneAngle = std::clamp(def.coneAngle, kMinConeAngle, kMaxConeAngle);

    joints_.push_back(joint);

    if (!def.collideConnected) {
        for (usize i = 0; i < contacts_.size(); ++i) {
            if (!contacts_[i].alive) {
                continue;
            }
            const Contact& c = contacts_[i].contact;
            const bool between = (c.bodyA == def.bodyA && c.bodyB == def.bodyB) ||
                                 (c.bodyA == def.bodyB && c.bodyB == def.bodyA);
            if (between) {
                pairs_.erase(PairKey(bodies_[static_cast<usize>(c.bodyA)]
                                         .fixtures[static_cast<usize>(c.fixtureA)].proxy,
                                     bodies_[static_cast<usize>(c.bodyB)]
                                         .fixtures[static_cast<usize>(c.fixtureB)].proxy));
                DestroyContact(static_cast<ContactId>(i));
            }
        }
    }
    return static_cast<JointId>(joints_.size()) - 1;
}

void Scene::RecomputeMass(Body& body) const {
    // A non-dynamic body has infinite mass, which is represented as a zero inverse -- so it
    // takes no impulse and needs no tensor. This is why the body type has to be known before
    // mass is computed rather than after. Kinematic and static are alike here: mass is derived
    // from the fixtures only for a dynamic body, so both of the other types come out massless
    // however dense their fixtures claim to be.
    if (body.type != BodyType::Dynamic) {
        body.mass = 0.0f;
        body.invMass = 0.0f;
        body.inertia = Mtx{};
        body.invInertia = Mtx{};
        body.localCentre = Vec4{};
        body.SyncTransform();
        return;
    }

    f32 mass = 0.0f;
    Mtx inertia{};
    Vec4 weightedCentre{};
    // The extents fold in the same loop as the mass -- one walk over the fixtures derives
    // both -- min of the thinnest features and max of the farthest reaches. They feed
    // nothing but IsFast.
    body.minExtent = constants::kMaxFloat.x;
    body.maxExtent = 0.0f;
    for (const Fixture& f : body.fixtures) {
        const MassProperties m = ComputeMass(shapes_->Get(f.shape), f.density);
        mass += m.mass;
        weightedCentre = weightedCentre + m.centre * m.mass;
        for (i32 k = 0; k < 3; ++k) {
            inertia.c[k] = inertia.c[k] + m.inertia.c[k];
        }
        f32 minExtent = 0.0f, maxExtent = 0.0f;
        ShapeExtents(shapes_->Get(f.shape), minExtent, maxExtent);
        body.minExtent = std::min(body.minExtent, minExtent);
        body.maxExtent = std::max(body.maxExtent, maxExtent);
    }

    body.mass = mass;
    body.invMass = mass > 0.0f ? Rcp(mass) : 0.0f;
    body.localCentre = mass > 0.0f ? weightedCentre * body.invMass : Vec4{};

    // The shapes report their tensors about the body origin, and a body rotates about its centre
    // of mass -- so shift by the parallel-axis term. It vanishes for a centred fixture and does
    // not for an offset one, which makes its absence invisible to every centred test case.
    const Vec4 c = body.localCentre;
    const f32 cc = LengthSquared3(c);
    if (mass > 0.0f && cc > 0.0f) {
        const Mtx shift{{Vec4{cc - c.x * c.x, -c.y * c.x, -c.z * c.x},
                         Vec4{-c.x * c.y, cc - c.y * c.y, -c.z * c.y},
                         Vec4{-c.x * c.z, -c.y * c.z, cc - c.z * c.z}, constants::kUnitW}};
        for (i32 k = 0; k < 3; ++k) {
            inertia.c[k] = inertia.c[k] - shift.c[k] * mass;
        }
    }
    for (i32 k = 0; k < 3; ++k) {
        inertia.c[k] = inertia.c[k] * body.inertiaScale;
    }

    body.inertia = inertia;

    // Invert by solving for each basis image rather than reciprocating the diagonal. A shape
    // offset from its body's origin has a tensor with real off-diagonal terms, and inverting
    // only the diagonal there is wrong in a way that looks right for every centred test case.
    // Solve33 returns zero for a singular system, which is the answer a massless body wants.
    body.invInertia.c[0] = Solve33(inertia, constants::kUnitX);
    body.invInertia.c[1] = Solve33(inertia, constants::kUnitY);
    body.invInertia.c[2] = Solve33(inertia, constants::kUnitZ);

    body.worldCentre =
        body.transform.position + RotationMatrix(body.transform.rotation).Transform(c);
    body.SyncTransform();
}

// -- driving bodies --------------------------------------------------------------------------

void Scene::SetLinearVelocity(BodyId body, const Vec4& v) {
    if (Contains(body)) {
        bodies_[static_cast<usize>(body)].linearVelocity = v;
    }
}

void Scene::SetAngularVelocity(BodyId body, const Vec4& w) {
    if (Contains(body)) {
        bodies_[static_cast<usize>(body)].angularVelocity = w;
    }
}

void Scene::SetPosition(BodyId body, const Vec4& p) {
    if (!Contains(body)) {
        return;
    }
    Body& b = bodies_[static_cast<usize>(body)];
    b.transform.position = p;
    b.worldCentre = p + RotationMatrix(b.transform.rotation).Transform(b.localCentre);
    b.SyncTransform();
    // A teleport has no motion history: the sweep collapses onto the new pose, so the swept
    // proxy below is tight rather than smeared across the jump.
    b.sweep.c0 = b.sweep.c = b.worldCentre;
    b.sweep.q0 = b.sweep.q = b.transform.rotation;
    // Move the proxies with it, rather than leaving them until the end of the next step: a
    // teleported body whose broadphase entry is a step behind keeps the contacts it had at its
    // old position for one step, and those contacts still hold the impulse they accumulated
    // there.
    SynchronizeBody(b);
}

void Scene::SetTransform(BodyId body, const Transform& xf) {
    if (!Contains(body)) {
        return;
    }
    Body& b = bodies_[static_cast<usize>(body)];
    b.transform = xf;
    // The rotation moved, so the centre of mass has to be re-derived from it rather than carried
    // over: `worldCentre` is what integrates, and leaving it behind makes the body rotate about
    // the wrong point for exactly one step.
    b.worldCentre = xf.position + RotationMatrix(xf.rotation).Transform(b.localCentre);
    b.SyncTransform();
    b.sweep.c0 = b.sweep.c = b.worldCentre;
    b.sweep.q0 = b.sweep.q = b.transform.rotation;
    SynchronizeBody(b);
}

void Scene::SetActive(BodyId body, bool active) {
    if (Contains(body)) {
        bodies_[static_cast<usize>(body)].active = active;
    }
}

void Scene::SetBodyType(BodyId body, BodyType type) {
    if (!Contains(body)) {
        return;
    }
    Body& b = bodies_[static_cast<usize>(body)];
    if (b.type == type) {
        return;
    }
    b.type = type;

    if (type == BodyType::Dynamic) {
        RecomputeMass(b);
    } else {
        // Not RecomputeMass: its non-dynamic branch also zeroes `localCentre`, which is correct
        // for a body that never had one and a teleport for a body that did. See the header.
        b.mass = 0.0f;
        b.invMass = 0.0f;
        b.inertia = Mtx{};
        b.invInertia = Mtx{};
        b.SyncTransform();
        if (type == BodyType::Static) {
            b.linearVelocity = Vec4{};
            b.angularVelocity = Vec4{};
        }
    }

    // The centre may have moved under the body (a dynamic body's is derived from its fixtures),
    // and a type change is not motion -- so the sweep collapses onto the new pose rather than
    // recording a jump the TOI pass would try to resolve.
    b.sweep.c0 = b.sweep.c = b.worldCentre;
    b.sweep.q0 = b.sweep.q = b.transform.rotation;

    for (usize i = 0; i < contacts_.size(); ++i) {
        if (!contacts_[i].alive) {
            continue;
        }
        const Contact& c = contacts_[i].contact;
        if (c.bodyA != body && c.bodyB != body) {
            continue;
        }
        pairs_.erase(
            PairKey(bodies_[static_cast<usize>(c.bodyA)].fixtures[static_cast<usize>(c.fixtureA)]
                        .proxy,
                    bodies_[static_cast<usize>(c.bodyB)].fixtures[static_cast<usize>(c.fixtureB)]
                        .proxy));
        DestroyContact(static_cast<ContactId>(i));
    }
}

// -- contacts --------------------------------------------------------------------------------

ContactId Scene::FirstContact() const {
    for (usize i = 0; i < contacts_.size(); ++i) {
        if (contacts_[i].alive) {
            return static_cast<ContactId>(i);
        }
    }
    return -1;
}

ContactId Scene::NextContact(ContactId id) const {
    for (usize i = static_cast<usize>(id < 0 ? 0 : id + 1); i < contacts_.size(); ++i) {
        if (contacts_[i].alive) {
            return static_cast<ContactId>(i);
        }
    }
    return -1;
}

const Contact* Scene::GetContact(ContactId id) const {
    if (id < 0 || id >= static_cast<ContactId>(contacts_.size()) ||
        !contacts_[static_cast<usize>(id)].alive) {
        return nullptr;
    }
    return &contacts_[static_cast<usize>(id)].contact;
}

Contact* Scene::GetContact(ContactId id) {
    return const_cast<Contact*>(static_cast<const Scene*>(this)->GetContact(id));
}

ContactId Scene::AllocateContact() {
    if (freeContact_ >= 0) {
        const ContactId id = freeContact_;
        freeContact_ = contacts_[static_cast<usize>(id)].nextFree;
        contacts_[static_cast<usize>(id)] = ContactSlot{};
        contacts_[static_cast<usize>(id)].alive = true;
        ++liveContacts_;
        return id;
    }
    contacts_.emplace_back();
    contacts_.back().alive = true;
    ++liveContacts_;
    return static_cast<ContactId>(contacts_.size()) - 1;
}

void Scene::DestroyContact(ContactId id) {
    ContactSlot& slot = contacts_[static_cast<usize>(id)];
    slot = ContactSlot{};
    slot.nextFree = freeContact_;
    freeContact_ = id;
    --liveContacts_;
}

bool Scene::ShouldCollide(const Body& a, const Body& b) const {
    // Two bodies that can never move relative to each other have nothing to solve.
    if (!a.IsDynamic() && !b.IsDynamic()) {
        return false;
    }
    // ...and neither do two the author has jointed with collideConnected off. Destroying the
    // existing contact at creation is not enough on its own: the broadphase would hand the pair
    // straight back on the next step.
    const i32 indexA = static_cast<i32>(&a - bodies_.data());
    const i32 indexB = static_cast<i32>(&b - bodies_.data());
    for (const Joint& joint : joints_) {
        if (joint.collideConnected) {
            continue;
        }
        if ((joint.bodyA == indexA && joint.bodyB == indexB) ||
            (joint.bodyA == indexB && joint.bodyB == indexA)) {
            return false;
        }
    }
    return true;
}

void Scene::UpdatePairs() {
    // Destroy first: a pair whose fat AABBs have parted is gone, and with it the accumulated
    // impulse. The fattening is what makes that rare -- a resting contact never sees it.
    for (usize i = 0; i < contacts_.size(); ++i) {
        if (!contacts_[i].alive) {
            continue;
        }
        const Contact& c = contacts_[i].contact;
        const i32 proxyA = bodies_[static_cast<usize>(c.bodyA)]
                               .fixtures[static_cast<usize>(c.fixtureA)].proxy;
        const i32 proxyB = bodies_[static_cast<usize>(c.bodyB)]
                               .fixtures[static_cast<usize>(c.fixtureB)].proxy;
        if (!Overlaps(tree_.ProxyAabb(proxyA), tree_.ProxyAabb(proxyB))) {
            pairs_.erase(PairKey(proxyA, proxyB));
            DestroyContact(static_cast<ContactId>(i));
        }
    }

    for (usize bi = 0; bi < bodies_.size(); ++bi) {
        const Body& body = bodies_[bi];
        for (usize fi = 0; fi < body.fixtures.size(); ++fi) {
            const i32 proxy = body.fixtures[fi].proxy;
            if (proxy == kNullNode) {
                continue;
            }
            tree_.Query(tree_.ProxyAabb(proxy), [&](i32 other) {
                if (other <= proxy) {
                    return;  // each pair once, and never a proxy against itself
                }
                const u64 data = tree_.Nodes()[static_cast<usize>(other)].userData;
                const i32 otherBody = ProxyBody(data);
                if (otherBody == static_cast<i32>(bi) ||
                    !ShouldCollide(body, bodies_[static_cast<usize>(otherBody)])) {
                    return;
                }
                const u64 key = PairKey(proxy, other);
                if (pairs_.count(key) != 0) {
                    return;
                }
                const ContactId id = AllocateContact();
                Contact& c = contacts_[static_cast<usize>(id)].contact;
                c.bodyA = static_cast<i32>(bi);
                c.fixtureA = static_cast<i32>(fi);
                c.bodyB = otherBody;
                c.fixtureB = ProxyFixture(data);
                const Fixture& fa = body.fixtures[fi];
                const Fixture& fb = bodies_[static_cast<usize>(c.bodyB)]
                                        .fixtures[static_cast<usize>(c.fixtureB)];
                // A mesh contact keeps the mesh as fixture A, whichever side the broadphase
                // found it on -- the whole mesh path (query frame, manifold normals) assumes
                // that orientation and never checks it again.
                if (IsMesh(TypeOf(shapes_->Get(fb.shape))) &&
                    !IsMesh(TypeOf(shapes_->Get(fa.shape)))) {
                    std::swap(c.bodyA, c.bodyB);
                    std::swap(c.fixtureA, c.fixtureB);
                }
                c.friction = MixFriction(fa.friction, fb.friction);
                c.restitution = MixRestitution(fa.restitution, fb.restitution);
                pairs_[key] = id;
            });
        }
    }
}

void Scene::CollideContact(Contact& c, bool invalidateSat) {
    const Body& a = bodies_[static_cast<usize>(c.bodyA)];
    const Body& b = bodies_[static_cast<usize>(c.bodyB)];
    const Shape& shapeA = shapes_->Get(a.fixtures[static_cast<usize>(c.fixtureA)].shape);
    const Shape& shapeB = shapes_->Get(b.fixtures[static_cast<usize>(c.fixtureB)].shape);

    if (IsMesh(TypeOf(shapeA))) {
        CollideMeshContact(c, invalidateSat);
        return;
    }
    if (invalidateSat) {
        // Cool the SAT cache's type byte before a post-advance collide; only the polytope
        // pair ever reads it, so this is a no-op everywhere else.
        c.sat.type = 0;
    }

    // The entries take both shapes in one frame, so hand them world-space copies and keep
    // the narrowphase free of body transforms.
    const bool swap = DispatchRank(TypeOf(shapeB)) > DispatchRank(TypeOf(shapeA));
    const Shape first = TransformShape(swap ? shapeB : shapeA,
                                       swap ? b.transform : a.transform);
    const Shape second = TransformShape(swap ? shapeA : shapeB,
                                        swap ? a.transform : b.transform);
    ContactManifold fresh = CollidePair(first, second, &c.sat);
    if (swap) {
        fresh.normal = -fresh.normal;  // the entries point A to B; the pair was reordered
    }

    Manifold next;
    next.count = fresh.count;
    next.normal = fresh.normal;
    for (i32 i = 0; i < fresh.count; ++i) {
        next.points[static_cast<usize>(i)].point = fresh.points[static_cast<usize>(i)].point;
        next.points[static_cast<usize>(i)].separation =
            fresh.points[static_cast<usize>(i)].separation;
        next.points[static_cast<usize>(i)].id = fresh.points[static_cast<usize>(i)].id;
    }

    if (next.count == 0) {
        c.touching = false;
        c.manifolds.clear();
        return;
    }
    if (c.manifolds.empty()) {
        next.token = nextManifoldToken_++;
        for (i32 i = 0; i < next.count; ++i) {
            next.points[static_cast<usize>(i)].isNew = true;
        }
        c.manifolds.push_back(next);
    } else {
        MatchManifold(c.manifolds.front(), next);
        c.manifolds.front() = next;
    }
    c.touching = true;
}

void Scene::CollideMeshContact(Contact& c, bool invalidateSat) {
    const Body& meshBody = bodies_[static_cast<usize>(c.bodyA)];
    const Body& convexBody = bodies_[static_cast<usize>(c.bodyB)];
    const Fixture& convexFixture = convexBody.fixtures[static_cast<usize>(c.fixtureB)];
    const Shape& meshShape = shapes_->Get(meshBody.fixtures[static_cast<usize>(c.fixtureA)].shape);
    const Shape& convexShape = shapes_->Get(convexFixture.shape);
    const Transform& xfA = meshBody.transform;
    const Transform& xfB = convexBody.transform;

    // Everything runs in the mesh's own frame: the query box, the convex shape, the whole
    // per-triangle narrowphase. The mesh never transforms a triangle; the results are lifted
    // to the world at the very end (mesh_contact.cpp).
    const MeshSourceView view(meshShape);
    const Transform inverse = InverseTransform(xfA);
    UpdateTriangleBuffer(c.mesh, *view.source, TransformAabb(inverse, convexFixture.aabb));

    std::vector<Manifold> fresh;
    if (!c.mesh.entries.empty()) {
        switch (TypeOf(convexShape)) {
            case ShapeType::Polytope: {
                Transform xfRel;
                xfRel.rotation = QuatMultiply(inverse.rotation, xfB.rotation);
                xfRel.position = InvRotate(xfA.rotation, xfB.position - xfA.position);
                fresh = CollideMeshPolytope(*view.source, c.mesh.entries,
                                            std::get<Polytope>(convexShape), xfRel, xfA,
                                            c.manifolds, invalidateSat);
                break;
            }
            case ShapeType::Capsule: {
                const Capsule& capsule = std::get<Capsule>(convexShape);
                Capsule local = capsule;
                local.p1 = InvRotate(xfA.rotation,
                                     Rotate(xfB.rotation, capsule.p1) + xfB.position -
                                         xfA.position);
                local.p2 = InvRotate(xfA.rotation,
                                     Rotate(xfB.rotation, capsule.p2) + xfB.position -
                                         xfA.position);
                fresh = CollideMeshCapsule(*view.source, c.mesh.entries, local, xfA,
                                           c.manifolds);
                break;
            }
            case ShapeType::Sphere: {
                const Sphere& sphere = std::get<Sphere>(convexShape);
                Sphere local = sphere;
                local.centre = InvRotate(xfA.rotation, Rotate(xfB.rotation, sphere.centre) +
                                                           xfB.position - xfA.position);
                fresh = CollideMeshSphere(*view.source, c.mesh.entries, local, xfA,
                                          c.manifolds);
                break;
            }
            default:
                break;  // triangle or a second mesh can never be the convex side
        }
    }

    if (fresh.empty()) {
        // Zero triangles, or none of them produced a manifold: the stored manifolds go, and
        // with them the warm-start history -- only the per-triangle caches survive a lost
        // contact, because they live in the triangle buffer.
        c.manifolds.clear();
        c.touching = false;
        return;
    }
    for (Manifold& m : fresh) {
        // A patch the reducer matched to last step kept its token through MatchManifold; a
        // patch that matched nothing is a new allocation and gets a new identity.
        if (m.token == 0) {
            m.token = nextManifoldToken_++;
        }
    }
    c.manifolds = std::move(fresh);
    c.touching = true;
}

void Scene::Collide() {
    // Walked newest-first: the reverse walk presents contacts in the order a push-front list
    // would, newest contact first, and the order the solver sees is the reverse of creation
    // order. It is not cosmetic: sequential impulses propagate load down a stack in one sweep
    // in this direction and over many sweeps in the other, so a settled stack comes to rest
    // measurably differently if the walk flips.
    for (usize i = contacts_.size(); i-- > 0;) {
        ContactSlot& slot = contacts_[i];
        if (!slot.alive) {
            continue;
        }
        CollideContact(slot.contact);
    }
}

// -- the step --------------------------------------------------------------------------------

void Scene::IntegrateVelocities(const std::vector<i32>& bodies, f32 dt) {
    for (const i32 index : bodies) {
        Body& body = bodies_[static_cast<usize>(index)];
        if (!body.IsSimulated()) {
            continue;
        }
        body.lastStep = stepStamp_;
        body.linearVelocity = body.linearVelocity + gravity_ * (body.gravityScale * dt);

        // v *= (1 - h*d), deliberately NOT v /= (1 + h*d). Past h*d >= 1 the factor goes to
        // zero or negative and the velocity is killed outright rather than eased -- a cliff
        // hosts tune damping against, and frame-rate dependent where the divide form is not.
        const f32 linear = 1.0f - dt * body.linearDamping;
        const f32 angular = 1.0f - dt * body.angularDamping;
        body.linearVelocity = body.linearVelocity * (linear > 0.0f ? linear : 0.0f);
        body.angularVelocity = body.angularVelocity * (angular > 0.0f ? angular : 0.0f);
    }
}

void Scene::IntegratePositions(const std::vector<i32>& bodies, f32 dt) {
    for (const i32 index : bodies) {
        Body& body = bodies_[static_cast<usize>(index)];
        if (!body.IsSimulated()) {
            continue;
        }
        // The MAIN integrate clamps at the same limits as the TOI island, and writes the
        // clamped velocities back: a contact-free body at 300 comes out of one step at
        // exactly 120, and a 60 rad/s spin at exactly (pi/4)/dt. One shared factor covers
        // both components, so the clamp preserves the 6D direction of motion.
        f32 scale = 1.0f;
        const f32 translation = LengthSquared3(body.linearVelocity * dt);
        if (translation > kMaxTranslation * kMaxTranslation) {
            scale = kMaxTranslation * Rcp(std::sqrt(translation));
        }
        const f32 rotation = LengthSquared3(body.angularVelocity * dt);
        if (rotation > kMaxRotation * kMaxRotation) {
            scale = std::min(scale, kMaxRotation * Rcp(std::sqrt(rotation)));
        }
        body.linearVelocity = body.linearVelocity * scale;
        body.angularVelocity = body.angularVelocity * scale;

        // Semi-implicit Euler: the position uses the velocity gravity has *already* been added
        // to, which is what makes free fall an exact arithmetic series.
        body.worldCentre = body.worldCentre + body.linearVelocity * dt;
        body.transform.rotation =
            IntegrateRotation(body.transform.rotation, body.angularVelocity, dt);
        body.SyncTransform();
    }
}

void Scene::SynchronizeBody(Body& body) {
    // The proxy covers the SWEPT bounds -- where the step started as well as where it ended,
    // union of both poses' boxes. This is what makes continuous collision reachable at all:
    // a fast body's end-pose AABB is already past the wall, and only the union overlaps it,
    // so only the union gets the pair the TOI scan needs.
    Transform start;
    start.rotation = body.sweep.q0;
    start.position =
        body.sweep.c0 - RotationMatrix(body.sweep.q0).Transform(body.localCentre);
    for (Fixture& f : body.fixtures) {
        if (f.proxy == kNullNode) {
            continue;
        }
        const Shape& shape = shapes_->Get(f.shape);
        const Aabb end = ComputeAabb(shape, body.transform);
        const Aabb swept = ComputeAabb(shape, start);
        f.aabb = Aabb{{std::min(swept.lower.x, end.lower.x),
                       std::min(swept.lower.y, end.lower.y),
                       std::min(swept.lower.z, end.lower.z)},
                      {std::max(swept.upper.x, end.upper.x),
                       std::max(swept.upper.y, end.upper.y),
                       std::max(swept.upper.z, end.upper.z)}};
        tree_.MoveProxy(f.proxy, f.aabb);
    }
}

void Scene::IntegrateKinematic(f32 dt) {
    // Kinematic bodies are not island members -- they take no impulse and contribute no
    // constraint -- so nothing in the solver would ever advance them. They still move, and this
    // is the pass that moves them. Same semi-implicit Euler as a dynamic body, minus the gravity
    // and damping that only apply to something with mass.
    //
    // It runs after the islands and before the proxy sync, which is where a dynamic body's
    // position also lands: integrate at the end of the step, collide against it on the next one.
    for (Body& body : bodies_) {
        if (!body.IsKinematicallyDriven()) {
            continue;
        }
        body.lastStep = stepStamp_;
        body.worldCentre = body.worldCentre + body.linearVelocity * dt;
        body.transform.rotation =
            IntegrateRotation(body.transform.rotation, body.angularVelocity, dt);
        body.SyncTransform();
    }
}

void Scene::SynchronizeProxies() {
    for (Body& body : bodies_) {
        SynchronizeBody(body);
    }
}

void Scene::SolveIsland(Island& island, f32 dt, i32 velocityIterations,
                       i32 positionIterations) {
    // The solver is built *before* gravity is integrated, and the ordering is load-bearing:
    // restitution comes out as `e` times the approach speed the body arrived with, not the
    // speed it has after this step's gravity. Building the constraints after integrating --
    // the more common ordering -- over-bounces every impact by exactly `e * g * dt`, a
    // discrepancy small enough to survive inspection and large enough to accumulate.
    ContactSolver solver(bodies_, island.contacts, dt);
    JointSolver joints(bodies_, island.joints, dt);
    IntegrateVelocities(island.bodies, dt);
    solver.WarmStart();
    for (i32 i = 0; i < velocityIterations; ++i) {
        // Joints before contacts, and both inside the same iteration. Box2D's order, and the one
        // that gives a jointed body resting on the ground a chance to satisfy both.
        joints.SolveVelocityConstraints();
        solver.SolveVelocityConstraints();
    }
    solver.StoreImpulses();

    IntegratePositions(island.bodies, dt);
    bool solved = island.contacts.empty() && island.joints.empty();
    for (i32 i = 0; i < positionIterations; ++i) {
        solved = solver.SolvePositionConstraints();
        solved = joints.SolvePositionConstraints() && solved;
    }
    // An island still being pushed apart is not at rest however slowly its bodies are moving,
    // so the position solver has to have converged before anything counts as ready. Readiness
    // is only recorded here; whether anything actually sleeps is the batch's decision.
    island.ready = UpdateSleepTimers(island, dt, solved);
}

bool Scene::UpdateSleepTimers(const Island& island, f32 dt, bool positionsSolved) {
    bool ready = positionsSolved;
    for (const i32 index : island.bodies) {
        Body& body = bodies_[static_cast<usize>(index)];
        if (!body.IsDynamic()) {
            continue;
        }
        if (!body.allowSleep || !body.IsQuiet()) {
            body.sleepTime = 0.0f;
            ready = false;
        } else {
            body.sleepTime += dt;
            ready = ready && body.sleepTime >= body.timeToSleep;
        }
    }
    return ready;
}

void Scene::ApplySleepBatches() {
    // Islands are packed into solver batches of at least this many bodies; the batch is the
    // unit that sleeps. E.g. twenty singleton islands with a late settler at body index 2
    // sleep as {1..15} -- gated by the laggard -- and {16..20} independently, the flush
    // boundary falling after the sixteenth body.
    constexpr i32 kSleepBatchMinBodies = 16;

    // Batches form in body-pool order, walking the pool forwards, so islands are ordered here
    // by their smallest dynamic member. The island BUILD order (newest-first, an intra-island
    // solver-sweep property) is deliberately NOT the batch order: the two directions serve
    // different mechanisms, and reusing one for the other regroups the sleep decisions.
    batchOrder_.clear();
    for (usize k = 0; k < islands_.size(); ++k) {
        i32 first = std::numeric_limits<i32>::max();
        for (const i32 index : islands_[k].bodies) {
            if (bodies_[static_cast<usize>(index)].IsDynamic()) {
                first = std::min(first, index);
            }
        }
        batchOrder_.emplace_back(first, static_cast<i32>(k));
    }
    std::sort(batchOrder_.begin(), batchOrder_.end());
    staticBatchSeen_.resize(bodies_.size(), 0);

    usize start = 0;
    i32 count = 0;
    bool allReady = true;
    ++staticBatchStamp_;
    for (usize i = 0; i < batchOrder_.size(); ++i) {
        const Island& island = islands_[static_cast<usize>(batchOrder_[i].second)];
        for (const i32 index : island.bodies) {
            if (bodies_[static_cast<usize>(index)].IsDynamic()) {
                ++count;
            } else if (staticBatchSeen_[static_cast<usize>(index)] != staticBatchStamp_) {
                // A static borders any number of islands but enters the batch's body list --
                // and therefore its size -- only once.
                staticBatchSeen_[static_cast<usize>(index)] = staticBatchStamp_;
                ++count;
            }
        }
        allReady = allReady && island.ready;
        const bool last = i + 1 == batchOrder_.size();
        if (count < kSleepBatchMinBodies && !last) {
            continue;
        }
        // The whole batch goes under together, or not at all: one island still settling keeps
        // every island in its batch awake, however long the others have been quiet.
        if (allReady) {
            for (usize j = start; j <= i; ++j) {
                for (const i32 index :
                     islands_[static_cast<usize>(batchOrder_[j].second)].bodies) {
                    Body& body = bodies_[static_cast<usize>(index)];
                    if (!body.IsDynamic()) {
                        continue;
                    }
                    body.sleeping = true;
                    body.linearVelocity = Vec4{};
                    body.angularVelocity = Vec4{};
                    // `sleepTime` is deliberately left where it is rather than cleared: it
                    // reads back at timeToSleep on the step the body goes under, and Wake is
                    // the one place the timer ever resets.
                }
            }
        }
        start = i + 1;
        count = 0;
        allReady = true;
        ++staticBatchStamp_;
    }
}

void Scene::Wake(Body& body) const {
    // Only a body that was actually asleep gets its timer cleared. The island walk calls this on
    // every body it reaches, every step, so clearing unconditionally pins the timer at one `dt`
    // for ever and nothing in the world ever sleeps -- while every individual part still looks
    // right: bodies come to rest, the timer ticks, the threshold is correct.
    if (!body.sleeping) {
        return;
    }
    body.sleeping = false;
    body.sleepTime = 0.0f;
}

void Scene::BuildIslands() {
    islands_.clear();
    bodyInIsland_.assign(bodies_.size(), 0);
    contactInIsland_.assign(contacts_.size(), 0);
    jointInIsland_.assign(joints_.size(), 0);

    // Seeded newest-first, like the contact scan below and for the same reason: it decides
    // which end of a stack the depth-first walk starts from, and so which direction load
    // propagates in a solver sweep. A settled stack comes to rest at measurably different
    // positions if the direction flips.
    for (usize seed = bodies_.size(); seed-- > 0;) {
        // Seeds are awake, active and dynamic. A sleeping island is never entered, so nothing
        // inside it wakes itself -- and a body reached *from* an awake seed does wake, which is
        // the one and only mechanism that wakes anything.
        if (bodyInIsland_[seed] != 0 || !bodies_[seed].IsSimulated()) {
            continue;
        }

        Island island;
        stack_.clear();
        stack_.push_back(static_cast<i32>(seed));
        bodyInIsland_[seed] = 1;

        while (!stack_.empty()) {
            const i32 index = stack_.back();
            stack_.pop_back();
            island.bodies.push_back(index);
            Wake(bodies_[static_cast<usize>(index)]);
            if (!bodies_[static_cast<usize>(index)].IsDynamic()) {
                continue;  // a static body joins an island but does not carry it further
            }

            // Newest-first, the same direction as the island seeding above; the order
            // islands are built in is the order the solver walks them.
            for (usize k = contacts_.size(); k-- > 0;) {
                if (!contacts_[k].alive || contactInIsland_[k] != 0) {
                    continue;
                }
                Contact& c = contacts_[k].contact;
                if (!c.touching || (c.bodyA != index && c.bodyB != index)) {
                    continue;
                }
                contactInIsland_[k] = 1;
                island.contacts.push_back(&c);
                const i32 other = c.bodyA == index ? c.bodyB : c.bodyA;
                if (bodyInIsland_[static_cast<usize>(other)] == 0) {
                    bodyInIsland_[static_cast<usize>(other)] = 1;
                    stack_.push_back(other);
                }
            }

            // A joint carries the island too, and unconditionally: unlike a contact it does not
            // have to be touching to hold two bodies together, so a bob hanging in mid-air is in
            // its anchor's island and goes to sleep on the same step it does.
            for (usize k = joints_.size(); k-- > 0;) {
                if (jointInIsland_[k] != 0) {
                    continue;
                }
                Joint& joint = joints_[k];
                if (joint.bodyA != index && joint.bodyB != index) {
                    continue;
                }
                jointInIsland_[k] = 1;
                island.joints.push_back(&joint);
                const i32 other = joint.bodyA == index ? joint.bodyB : joint.bodyA;
                if (bodyInIsland_[static_cast<usize>(other)] == 0) {
                    bodyInIsland_[static_cast<usize>(other)] = 1;
                    stack_.push_back(other);
                }
            }
        }

        // A static body borders any number of islands, so it is released back for the next one.
        for (const i32 index : island.bodies) {
            if (!bodies_[static_cast<usize>(index)].IsDynamic()) {
                bodyInIsland_[static_cast<usize>(index)] = 0;
            }
        }
        islands_.push_back(std::move(island));
    }
}

// -- continuous collision --------------------------------------------------------------------
//
// Continuous collision is dynamic against STATIC and nothing else: the candidate gate
// requires a dynamic body, the contact scan and the island gather both require the other
// body static, and there is no bullet flag anywhere. A fast body still tunnels through a
// slow dynamic one -- contract, not a hole: hosts pace fast movers against it, and widening
// the scope changes which pairs can tunnel. The arithmetic is uniform with the rest of the
// engine: the sweep advance and the false-position steps run on the rcp estimate plus a
// Newton refinement, and the TOI island clamps both velocity components by one shared factor.

bool Scene::IsFast(const Body& body, f32 dt) const {
    // `r` is the body origin relative to the centre of mass -- the lever arm the rotation
    // sweeps. A centred sphere spinning in place has |r| = 0 and maxExtent = 0 and is never
    // fast, however hard it spins, which is exactly right: nothing about it moves.
    const Vec4 r = body.transform.position - body.sweep.c;
    const Vec4 tip = Cross3(body.angularVelocity, r) + body.linearVelocity;
    const f32 motion = std::sqrt(LengthSquared3(body.angularVelocity)) * body.maxExtent +
                       std::sqrt(LengthSquared3(tip));
    // Fast = this step's motion exceeds a quarter of the thinnest feature. True square
    // roots on purpose: this test runs full sqrt where most of the engine estimates.
    return 0.25f * body.minExtent < motion * dt;
}

void Scene::FinalizeSweep(Body& body) {
    body.sweep.c0 = body.sweep.c;
    body.sweep.q0 = body.sweep.q;
    body.sweep.alpha0 = 1.0f;
    body.worldCentre = body.sweep.c;
    body.transform.rotation = body.sweep.q;
    body.SyncTransform();
}

void Scene::ProcessToiCandidate(usize slot) {
    const i32 index = toiCandidates_[slot];
    Body& body = bodies_[static_cast<usize>(index)];
    f32& fraction = toiFractions_[slot];
    toiHits_[slot] = 0;
    if (fraction == 1.0f) {
        return;
    }
    if (body.toiCount == kMaxToiSubSteps) {
        // Out of budget: give up and finalise at the step end. The counter resets next step.
        FinalizeSweep(body);
        fraction = 1.0f;
        return;
    }

    // Every contact against a static body is a TOI candidate -- existence, not touching: the
    // fat proxies mean the wall's contact exists before anything meets it, which is the only
    // reason there is something here to test. Capped at 32 (kMaxToiContacts); past the cap
    // the remaining static contacts simply go untested.
    toiContacts_.clear();
    for (usize k = 0; k < contacts_.size(); ++k) {
        if (!contacts_[k].alive) {
            continue;
        }
        Contact& c = contacts_[k].contact;
        if (c.bodyA != index && c.bodyB != index) {
            continue;
        }
        const i32 other = c.bodyA == index ? c.bodyB : c.bodyA;
        if (bodies_[static_cast<usize>(other)].type != BodyType::Static) {
            continue;
        }
        if (static_cast<i32>(toiContacts_.size()) == kMaxToiContacts) {
            break;
        }
        toiContacts_.push_back(&c);
    }

    f32 minT = constants::kMaxFloat.x;
    bool hit = false;
    for (Contact* c : toiContacts_) {
        // Argument order follows the contact's own A/B orientation; the core re-bases on
        // sweep A's start, so the order is observable in ulps.
        const Body& a = bodies_[static_cast<usize>(c->bodyA)];
        const Body& b = bodies_[static_cast<usize>(c->bodyB)];
        const Shape& shapeA = shapes_->Get(a.fixtures[static_cast<usize>(c->fixtureA)].shape);

        if (IsMesh(TypeOf(shapeA))) {
            // The mesh half: A is the mesh (static, so the swap at creation put it there), B
            // the fast body. Refresh the triangle set for the CURRENT swept box -- later
            // sub-steps re-query the advanced, shrunken sweep -- then run the per-triangle
            // core with the entries' warm caches.
            const Fixture& fixtureB = b.fixtures[static_cast<usize>(c->fixtureB)];
            const Shape& shapeB = shapes_->Get(fixtureB.shape);
            const MeshSourceView view(shapeA);
            UpdateTriangleBuffer(c->mesh, *view.source,
                                 TransformAabb(InverseTransform(a.transform), fixtureB.aabb));
            Sweep sweepMesh = a.sweep;
            sweepMesh.alpha0 = body.sweep.alpha0;  // the same bookkeeping copy as below
            const ToiOutput out =
                MeshTimeOfImpact(c->mesh, *view.source, sweepMesh, a.localCentre,
                                 SupportProxy(shapeB), ShapeCentroid(shapeB), b.sweep,
                                 b.localCentre);
            if (out.state != ToiState::Separated) {
                minT = std::min(minT, out.t);
                hit = true;
            }
            continue;
        }

        ToiInput in;
        in.proxyA = SupportProxy(shapeA);
        in.proxyB = SupportProxy(shapes_->Get(b.fixtures[static_cast<usize>(c->fixtureB)].shape));
        in.sweepA = a.sweep;
        in.sweepB = b.sweep;
        in.localCentreA = a.localCentre;
        in.localCentreB = b.localCentre;
        // The static side inherits the moving body's consumed fraction -- bookkeeping the
        // core never reads, kept so the sweep records match the reference engine's.
        (c->bodyA == index ? in.sweepB : in.sweepA).alpha0 = body.sweep.alpha0;

        const ToiOutput out = TimeOfImpact(in);
        if (out.state != ToiState::Separated) {
            minT = std::min(minT, out.t);
            hit = true;
        }
    }
    if (!hit) {
        FinalizeSweep(body);
        fraction = 1.0f;
        return;
    }

    // The core answered on the remaining sweep; rescale into the whole step, snapping an
    // impact within 1.192e-5 of the end onto it.
    f32 alpha = minT * (1.0f - body.sweep.alpha0) + body.sweep.alpha0;
    if (alpha > kToiSnapToOne) {
        alpha = 1.0f;
    }
    fraction = alpha;
    toiHits_[slot] = 1;
    ++body.toiCount;  // spent even if the advance is rolled back below -- the budget counts tries

    // Move the body to the impact: advance the sweep start, collapse the sweep onto it, and
    // rebuild the manifolds there. If nothing actually touches at the advanced pose the TOI
    // was a false positive -- roll the sweep back and finalise at the step end.
    const Sweep backup = body.sweep;
    AdvanceSweep(body.sweep, alpha);
    body.sweep.c = body.sweep.c0;
    body.sweep.q = body.sweep.q0;
    body.worldCentre = body.sweep.c0;
    body.transform.rotation = body.sweep.q0;
    body.SyncTransform();

    bool touching = false;
    for (Contact* c : toiContacts_) {
        // true: the body was just teleported to the impact pose, so every cached SAT axis is
        // meaningless and gets cooled before colliding. GJK caches stay warm.
        CollideContact(*c, true);
        touching = touching || c->touching;
    }
    if (!touching) {
        body.sweep = backup;
        FinalizeSweep(body);
        fraction = 1.0f;
        toiHits_[slot] = 0;
    }
}

void Scene::SolveToiIsland(i32 index, f32 dt, i32 velocityIterations) {
    Body& body = bodies_[static_cast<usize>(index)];

    // The TOI island is the body plus its static contacts that actually hold a manifold --
    // gathered fresh, because the advance just rebuilt them. Joints are NOT gathered; a TOI
    // island never solves one.
    toiContacts_.clear();
    for (usize k = 0; k < contacts_.size(); ++k) {
        if (!contacts_[k].alive) {
            continue;
        }
        Contact& c = contacts_[k].contact;
        if ((c.bodyA != index && c.bodyB != index) || c.manifolds.empty()) {
            continue;
        }
        const i32 other = c.bodyA == index ? c.bodyB : c.bodyA;
        if (bodies_[static_cast<usize>(other)].type != BodyType::Static) {
            continue;
        }
        toiContacts_.push_back(&c);
    }

    const Vec4 preCentre = body.worldCentre;
    const Vec4 preRotation = body.transform.rotation;

    // Cold on purpose: no WarmStart and no StoreImpulses. The TOI solve finds its whole
    // impulse from nothing and leaves the main solve's accumulated impulses untouched.
    ContactSolver solver(bodies_, toiContacts_, dt);
    for (i32 i = 0; i < velocityIterations; ++i) {
        solver.SolveVelocityConstraints();
    }
    for (i32 i = 0; i < kToiPositionIterations; ++i) {
        if (solver.SolvePositionConstraints(kToiBaumgarte)) {
            break;
        }
    }

    // Every write-back below is NaN-guarded: a non-finite solve result is simply NOT
    // written. The solver mutates the body in place, so "not written" here has to mean
    // restored.
    const bool poseFinite = Finite3(body.worldCentre) && Finite4(body.transform.rotation);
    if (poseFinite) {
        body.sweep.c0 = body.sweep.c = body.worldCentre;
        body.sweep.q0 = body.sweep.q = body.transform.rotation;
    } else {
        body.worldCentre = preCentre;
        body.transform.rotation = preRotation;
        body.SyncTransform();
    }

    Vec4 v = body.linearVelocity;
    Vec4 w = body.angularVelocity;
    if (!Finite3(v) || !Finite3(w)) {
        v = Vec4{};
        w = Vec4{};
    }

    // One shared clamp factor for both components: a fast translation also scales the
    // rotation down and vice versa, so the 6D direction of motion is preserved rather than
    // clamped one axis at a time.
    f32 scale = 1.0f;
    const f32 translation = LengthSquared3(v * dt);
    if (translation > kMaxTranslation * kMaxTranslation) {
        scale = kMaxTranslation * Rcp(std::sqrt(translation));
    }
    const f32 rotation = LengthSquared3(w * dt);
    if (rotation > kMaxRotation * kMaxRotation) {
        scale = std::min(scale, kMaxRotation * Rcp(std::sqrt(rotation)));
    }
    v = v * scale;
    w = w * scale;

    // Integrate the remainder of the step in one shot, with no gravity: the sweep start
    // keeps the solved pose and only the end moves.
    if (poseFinite) {
        const Vec4 centre = body.sweep.c + v * dt;
        const Vec4 spin = IntegrateRotation(body.sweep.q, w, dt);
        if (Finite3(centre) && Finite4(spin)) {
            body.sweep.c = centre;
            body.sweep.q = spin;
            body.worldCentre = centre;
            body.transform.rotation = spin;
            body.SyncTransform();
        }
    }
    body.linearVelocity = v;
    body.angularVelocity = w;
}

void Scene::SolveToi(f32 dt, i32 velocityIterations) {
    if (toiCandidates_.empty()) {
        return;
    }
    toiFractions_.assign(toiCandidates_.size(), 0.0f);
    toiHits_.assign(toiCandidates_.size(), 0);

    // Sub-step until quiet: every iteration advances each live candidate to its earliest
    // impact, solves the impacts, and refreshes the pairs so the next iteration can see the
    // contacts the advance created. The per-body budget of kMaxToiSubSteps bounds the loop.
    for (;;) {
        bool any = false;
        for (usize slot = 0; slot < toiCandidates_.size(); ++slot) {
            ProcessToiCandidate(slot);
            any = any || toiHits_[slot] != 0;
        }
        if (!any) {
            break;
        }
        for (usize slot = 0; slot < toiCandidates_.size(); ++slot) {
            if (toiHits_[slot] != 0) {
                SolveToiIsland(toiCandidates_[slot], (1.0f - toiFractions_[slot]) * dt,
                               velocityIterations);
            }
        }
        for (usize slot = 0; slot < toiCandidates_.size(); ++slot) {
            if (toiHits_[slot] != 0) {
                SynchronizeBody(bodies_[static_cast<usize>(toiCandidates_[slot])]);
            }
        }
        UpdatePairs();
    }
}

void Scene::Step(f32 dt, i32 velocityIterations, i32 positionIterations) {
    ++stepStamp_;

    // The sweep starts where the last step ended. Only the TOI machinery ever moves the
    // start after this.
    for (Body& body : bodies_) {
        body.sweep.c0 = body.sweep.c = body.worldCentre;
        body.sweep.q0 = body.sweep.q = body.transform.rotation;
        body.sweep.alpha0 = 0.0f;
    }

    UpdatePairs();
    Collide();
    BuildIslands();

    for (Island& island : islands_) {
        SolveIsland(island, dt, velocityIterations, positionIterations);
    }
    ApplySleepBatches();

    IntegrateKinematic(dt);
    SynchronizeProxies();

    // The step's tail: this step's fast bodies become the TOI candidates, the pairs refresh
    // so the continuous pass can see the contacts the motion just created, and SolveToi
    // consumes the list before the step returns.
    toiCandidates_.clear();
    for (usize i = 0; i < bodies_.size(); ++i) {
        Body& body = bodies_[i];
        if (!body.IsSimulated()) {
            continue;
        }
        body.sweep.c = body.worldCentre;
        body.sweep.q = body.transform.rotation;
        if (IsFast(body, dt)) {
            body.toiCount = 0;
            toiCandidates_.push_back(static_cast<i32>(i));
        }
    }
    UpdatePairs();
    SolveToi(dt, velocityIterations);
}

}  // namespace snowball
