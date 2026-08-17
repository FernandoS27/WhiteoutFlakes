#include "renderer/profiles/wow/wow_physics.h"

#include "whiteout/models/m2/structures.h"

#include "snowball/joint.h"
#include "snowball/polytope.h"
#include "snowball/scene.h"
#include "snowball/shape.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>

namespace whiteout::flakes::renderer::profiles::wow {
namespace {

namespace sb = ::snowball;
using ::whiteout::flakes::renderer::animation::BoneClaim;
using ::whiteout::flakes::renderer::animation::IPoseStage;
using ::whiteout::flakes::renderer::animation::PoseStageContext;
using ::whiteout::flakes::renderer::model::FrameState;

/// Gravity, in the model space this stage simulates in. `DOMINO_GLUE.md` §4:
/// the client creates its one `dmWorld` with `(0, 0, -10)` — z-up yards, and
/// **-10 rather than -9.81**, which is a tuning decision and not an
/// approximation of one. M2 model space is z-up too, so the vector carries
/// across unchanged.
constexpr sb::Vec4 kGravity{0.0f, 0.0f, -10.0f, 0.0f};

/// `Physics::Update` @ 0x101724ed0 takes a single variable step clamped to
/// 1/60 — no accumulator, no substepping. Below 60fps the simulation runs slow
/// rather than catching up (`DOMINO_GLUE.md` §4.1). Reproduced rather than
/// improved: a fixed-step accumulator here would settle cloth differently from
/// the client at every frame rate but one.
constexpr f32 kMaxStep = 1.0f / 60.0f;

/// The snap fraction from `KinematicGroup::PreStep` (`DOMINO_GLUE.md` §5.2).
/// The floor is a function-local static in the client, which is why the branch
/// that skips the snap is unreachable there: the root is *always* moved at
/// least 80% of the way to the animated pose, and the caps only push it further
/// toward a full teleport.
constexpr f32 kSnapFloor = 0.8f;
constexpr f32 kMaxAngularStep = 0.25f * 3.14159265f;  // 45 degrees per step
constexpr f32 kMaxLinearStep = 1.0f;                  // one yard per step

sb::Vec4 ToVec4(const Vector3f& v) {
    return sb::Vec4{v.x, v.y, v.z, 0.0f};
}

/// @brief `p * m`, row-vector — the image of a bind-space point under a bone's
///        palette entry.
sb::Vec4 TransformPoint(const Vector3f& p, const Matrix44f& m) {
    return sb::Vec4{p.x * m.data[0][0] + p.y * m.data[1][0] + p.z * m.data[2][0] + m.data[3][0],
                    p.x * m.data[0][1] + p.y * m.data[1][1] + p.z * m.data[2][1] + m.data[3][1],
                    p.x * m.data[0][2] + p.y * m.data[1][2] + p.z * m.data[2][2] + m.data[3][2],
                    0.0f};
}

/// Quaternion from a bone matrix.
///
/// Bone matrices here are **row-vector**: row `i` is the image of basis vector
/// `i` (`ComposePivotSRT` transposes `Matrix44f::rotation` for exactly that
/// reason), so the column-major basis this extraction wants is the transpose of
/// the top-left 3x3. Reading it the other way round yields the conjugate, which
/// looks correct for any symmetric pose and mirrors every other one.
sb::Vec4 RotationOf(const Matrix44f& m) {
    const f32 r[3][3] = {{m.data[0][0], m.data[1][0], m.data[2][0]},
                         {m.data[0][1], m.data[1][1], m.data[2][1]},
                         {m.data[0][2], m.data[1][2], m.data[2][2]}};
    const f32 trace = r[0][0] + r[1][1] + r[2][2];
    sb::Vec4 q{};
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s,
             0.25f * s};
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const f32 s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
        q = {0.25f * s, (r[0][1] + r[1][0]) / s, (r[0][2] + r[2][0]) / s,
             (r[2][1] - r[1][2]) / s};
    } else if (r[1][1] > r[2][2]) {
        const f32 s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
        q = {(r[0][1] + r[1][0]) / s, 0.25f * s, (r[1][2] + r[2][1]) / s,
             (r[0][2] - r[2][0]) / s};
    } else {
        const f32 s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
        q = {(r[0][2] + r[2][0]) / s, (r[1][2] + r[2][1]) / s, 0.25f * s,
             (r[1][0] - r[0][1]) / s};
    }
    return sb::QuatNormalize(q);
}

/// The inverse of @ref RotationOf plus a translation: a row-vector matrix.
///
/// @param pivot the bone's bind-space origin, subtracted back out — see
///        @ref BoneFrame for why the palette is not the frame.
Matrix44f ToMatrix(const sb::Transform& xf, const Vector3f& pivot) {
    const sb::Mtx b = sb::RotationMatrix(xf.rotation);
    Matrix44f m = Matrix44f::identity();
    // **Row `j` of `data` is column `j` of `Mtx`, not its row `j`.** Work it out
    // from @ref RotationOf, which this has to invert exactly: that builds
    // `r[i][j] = data[j][i]` and extracts the quaternion whose rotation matrix
    // has `c[j] == r[:,j] == data[j][:]`. So each `Mtx` column is copied into
    // the matching `data` row, componentwise — the two are *not* related by a
    // transpose, and writing one in produces the conjugate rotation.
    //
    // That conjugate is silent in position (translation is carried separately)
    // and total in orientation: every simulated bone renders spun the wrong way,
    // which reads as cloth twisting about its own length and rigid pieces —
    // a pauldron, a scabbard — rotating backwards.
    m.data[0][0] = b.c[0].x; m.data[0][1] = b.c[0].y; m.data[0][2] = b.c[0].z;
    m.data[1][0] = b.c[1].x; m.data[1][1] = b.c[1].y; m.data[1][2] = b.c[1].z;
    m.data[2][0] = b.c[2].x; m.data[2][1] = b.c[2].y; m.data[2][2] = b.c[2].z;
    // `S = T(-pivot) * F`, so the translation is `position - pivot*R`.
    const sb::Vec4 shifted = TransformPoint(pivot, m);
    m.data[3][0] = xf.position.x - shifted.x;
    m.data[3][1] = xf.position.y - shifted.y;
    m.data[3][2] = xf.position.z - shifted.z;
    return m;
}

/// @brief The bone's model-space frame, recovered from its palette entry.
///
/// **`FrameState::boneWorldMatrices` is a skinning palette, not a set of bone
/// transforms**, whatever the name says. `M2ModelAdapter::Evaluate` builds each
/// entry as `ComposePivotSRT(t, r, s, pivot) * parentM`, and `ComposePivotSRT`
/// brackets the local SRT with `T(-pivot) ... T(+pivot)` — so an unanimated bone
/// gets **identity**, not a translation to where it lives. The bind frame is
/// carried by the pivot alone, which is why every other consumer in the adapter
/// (ribbons, particle emitters, attachments) pushes its own bind-space offset
/// through the palette entry rather than reading the translation row out of it.
///
/// Reading that row as a position is silent and total: at bind pose it is zero
/// for every bone, so the whole rig seeds onto the model origin, and what hangs
/// off the character afterwards is the tangle the joints pull that into. The
/// shapes and joint anchors in `.phys` are bone-local — relative to the pivot —
/// so they only make sense against this frame.
sb::Transform BoneFrame(const Matrix44f& palette, const Vector3f& pivot) {
    sb::Transform xf;
    xf.rotation = RotationOf(palette);
    xf.position = TransformPoint(pivot, palette);
    return xf;
}

/// `.phys` stores an affine frame as three basis vectors plus an origin
/// (`PhysicsFrame`), which is what `dmQuatFromMtx` is handed.
///
/// **Each consecutive triple is one `dmMtx` *column*, and a `dmMtx` column is
/// the image of a basis vector** — measured, not inferred:
/// `Physics::LegacyLoadPhysData` @ 0x101724744 loads `[rec+0x3C]`, `[rec+0x40]`
/// and `[rec+0x44]` and packs them straight into `m_col[1]`. So `axisX` is the
/// image of local X, and in this codebase's **row-vector** `Matrix44f` the image
/// of a basis vector is a *row*.
///
/// Writing the axes down the columns instead transposes the frame, which for an
/// orthonormal basis is exactly its inverse — so every joint frame comes out as
/// the **conjugate rotation**. That is invisible in the anchors (those are
/// `origin`, untouched) and invisible at bind pose (all three frames of a
/// resting rig nearly agree), and it points every shoulder's cone and twist axis
/// the wrong way the moment the rig moves: the chains fold through themselves.
sb::Transform FrameToTransform(const ::whiteout::m2::PhysicsFrame& f) {
    Matrix44f m = Matrix44f::identity();
    m.data[0][0] = f.axisX.x; m.data[0][1] = f.axisX.y; m.data[0][2] = f.axisX.z;
    m.data[1][0] = f.axisY.x; m.data[1][1] = f.axisY.y; m.data[1][2] = f.axisY.z;
    m.data[2][0] = f.axisZ.x; m.data[2][1] = f.axisZ.y; m.data[2][2] = f.axisZ.z;
    sb::Transform xf;
    xf.rotation = RotationOf(m);
    xf.position = ToVec4(f.origin);
    return xf;
}

f32 Length3(const sb::Vec4& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

/// How far past a cap a required motion is, as a fraction in [0, 1).
f32 Excess(f32 magnitude, f32 cap) {
    return magnitude > cap && magnitude > 0.0f ? (magnitude - cap) / magnitude : 0.0f;
}

/// @brief `.phys` joint angles are **degrees**; Domino's are radians.
///
/// `ShoulderJoint::coneAngle`'s own doc in WhiteoutLib says the loader converts on the way in.
/// It does not — `binary_parse_visitor/phys.cpp` reads the float straight through — so the
/// conversion has to happen here, and the comment is describing an intention rather than the
/// code. Measured on Lor'themar: cones of **60** and twists of **±20**, which are only
/// sensible as degrees.
///
/// Getting this wrong is silent and catastrophic. `dmShoulderJoint::Create` clamps the cone to
/// [10°, 170°] *expressed in radians*, and Snowball reproduces the clamp — so a cone handed
/// over as 60 does not blow up, it saturates at 170°. Every joint in the rig becomes a free
/// ball joint that still looks like a configured cone in a debug dump, and the cloth folds
/// through itself and through the body.
constexpr f32 kDegreesToRadians = 3.14159265f / 180.0f;

/// @brief Whether to drive shoulders with Snowball's angular spring. `WDX_PHYSICS_SPRING=0` off.
///
/// **Opt-out rather than opt-in, and the only inferred behaviour in this stage.** `SHJ2` gives
/// every shoulder a motor — Lor'themar's are all 1 Hz at damping 0.7 — and without something
/// pulling a joint back toward its rest orientation a chain is a string of beads: it hangs, it
/// collides, and it rotates freely inside its cone.
///
/// The drive is real and in the shipped engine. What is *not* recoverable from the 6.0.1
/// reference is how the authored hertz/damping reach it, because that client predates `SHJ2`
/// and zeroes the two def fields outright. So the spring here is Box2D's soft constraint rather
/// than a transcription, it is switchable, and it stays that way until a retail client settles
/// the mapping.
/// **Default off, on the binary's authority.** `CPhysicsShoulderJointDef::CreateInstance` @
/// 0x1017269B0 initialises the def's drive fields (`+124`, `+128`) to zero and never copies over
/// them — it copies the two frames, the anchors, the twist angles and the cone, and stops. So a
/// WoW shoulder has no angular drive at all, whatever `SHJ2` carries. Opt in with
/// `WDX_PHYSICS_SPRING=1`; measured to change nothing on Lor'themar either way.
bool AngularSpringEnabled() {
    const char* v = std::getenv("WDX_PHYSICS_SPRING");
    return v != nullptr && v[0] == '1' && v[1] == '\0';
}

// ---------------------------------------------------------------------------

class WowPhysicsStage final : public IPoseStage {
public:
    explicit WowPhysicsStage(const ::whiteout::m2::Model& model) : scene_(shapes_, kGravity) {
        Build(model);
    }

    bool Empty() const {
        return links_.empty() || claims_.empty();
    }

    void Run(FrameState& fs, const PoseStageContext& ctx) override;

    std::vector<BoneClaim> Claims() const override {
        return claims_;
    }

    /// A `.phys` rig needs no ground query and no aim target — everything it
    /// simulates comes out of the model file — so it does not belong behind the
    /// solver toggle.
    bool NeedsHostInputs() const override {
        return false;
    }

private:
    struct Link {
        i32 bone = -1;
        sb::BodyId body = -1;
        bool dynamic = false;
        /// The bone's bind-space origin. `.phys` geometry is expressed relative
        /// to it, and the palette does not carry it (see @ref BoneFrame).
        Vector3f pivot{};
        /// Where Seed put this body. Debug only — the baseline for "has the rig
        /// moved at all", which is the question a stiff-looking chain poses and
        /// which no per-frame delta can answer.
        sb::Vec4 seedPos{};
    };

    i32 BoneOf(sb::BodyId id) const {
        for (const Link& l : links_) {
            if (l.body == id) return l.bone;
        }
        return -1;
    }

    void Build(const ::whiteout::m2::Model& model);
    void Seed(const FrameState& fs);
    void DriveKinematic(const FrameState& fs, f32 dt);
    void WriteBack(FrameState& fs);

    sb::ShapeStore shapes_;
    sb::Scene scene_;
    std::vector<Link> links_;
    std::vector<BoneClaim> claims_;
    bool seeded_ = false;
    int debugFrame_ = 0;
};

void WowPhysicsStage::Build(const ::whiteout::m2::Model& model) {
    namespace w2 = ::whiteout::m2;
    const w2::PhysicsData& phys = *model.physics;
    const std::size_t boneCount = model.bones.size();
    int fixtureCount = 0, jointCount = 0, skippedShapes = 0;
    const bool debug = std::getenv("WDX_PHYSICS_DEBUG") != nullptr;
    const bool spring = AngularSpringEnabled();
    const char* twistEnv = std::getenv("WDX_PHYSICS_TWIST");
    const bool twist = !(twistEnv != nullptr && twistEnv[0] == '0' && twistEnv[1] == '\0');

    std::vector<sb::BodyId> bodyOf(phys.bodies.size(), -1);

    for (std::size_t i = 0; i < phys.bodies.size(); ++i) {
        const w2::PhysicsBody& pb = phys.bodies[i];
        if (pb.boneIndex >= boneCount) {
            continue;
        }

        sb::BodyDef def;
        // `.phys` inverts Domino's own enum: file 1 is dynamic and file 0 is
        // the kinematic anchor (`DOMINO_GLUE.md` §2.3). Domino in turn inverts
        // Box2D's, which is why this reads backwards twice and is still right.
        const bool dynamic = pb.type == w2::PhysicsBodyType::Dynamic;
        def.type = dynamic ? sb::BodyType::Dynamic : sb::BodyType::Kinematic;
        def.linearDamping = pb.linearDamping;
        def.angularDamping = pb.angularDamping;
        def.gravityScale = pb.gravityScale;
        def.inertiaScale = pb.inertiaScale;
        // The client passes `allowSleep = true`, but a cloth chain that sleeps
        // mid-swing stays folded until something wakes it, and nothing here
        // wakes bodies on animation change yet. Off until that exists.
        def.allowSleep = false;

        const sb::BodyId body = scene_.AddBody(def);
        bodyOf[i] = body;
        links_.push_back({static_cast<i32>(pb.boneIndex), body, dynamic,
                          model.bones[pb.boneIndex].pivot});

        const std::size_t first = static_cast<std::size_t>(std::max(0, pb.shapeIndex));
        const std::size_t count = static_cast<std::size_t>(std::max(0, pb.shapeCount));
        for (std::size_t s = first; s < first + count && s < phys.shapes.size(); ++s) {
            const w2::PhysicsShape& ps = phys.shapes[s];
            const std::size_t idx = static_cast<std::size_t>(std::max<i16>(0, ps.shapeIndex));
            sb::ShapeId shape = -1;

            switch (ps.shapeType) {
            case w2::PhysicsShapeType::Box:
                if (idx < phys.boxShapes.size()) {
                    const w2::BoxShape& b = phys.boxShapes[idx];
                    // A `.phys` box is a **polytope** to Domino, not a shape
                    // kind of its own: `CPhysicsBoxShapeDef::CreateInstance`
                    // sets `m_shapeType = 2` and hands the transform and half
                    // extents over as polytope data (§3.2).
                    shape = shapes_.Add(sb::MakeBox(ToVec4(b.halfExtents),
                                                    FrameToTransform(b.frame)));
                }
                break;
            case w2::PhysicsShapeType::Capsule:
                if (idx < phys.capsuleShapes.size()) {
                    const w2::CapsuleShape& c = phys.capsuleShapes[idx];
                    // Two endpoints, not centre-and-height.
                    shape = shapes_.Add(sb::Capsule{ToVec4(c.localPosition1),
                                                    ToVec4(c.localPosition2), c.radius});
                }
                break;
            case w2::PhysicsShapeType::Sphere:
                if (idx < phys.sphereShapes.size()) {
                    const w2::SphereShape& sp = phys.sphereShapes[idx];
                    shape = shapes_.Add(sb::Sphere{ToVec4(sp.localPosition), sp.radius});
                }
                break;
            case w2::PhysicsShapeType::Polytope:
                if (idx < phys.polytopeShapes.size()) {
                    const w2::PolytopeShape& hull = phys.polytopeShapes[idx];
                    std::vector<sb::Vec4> pts;
                    pts.reserve(hull.vertices.size());
                    for (const Vector3f& v : hull.vertices) {
                        pts.push_back(ToVec4(v));
                    }
                    if (pts.size() >= 4) {
                        // Rebuilt through our own hull builder rather than
                        // trusting the file's half-edge table: the topology is
                        // ours to choose (RE plan §4.4) and a corrupt table
                        // would otherwise reach the narrowphase intact.
                        shape = shapes_.Add(sb::MakePolytope(pts));
                    }
                }
                break;
            }

            if (shape >= 0) {
                scene_.AddFixture(body, shape, ps.density, ps.friction, ps.restitution);
                ++fixtureCount;
                // Shape type next to the mass it produces: a whole shape family with no mass
                // association is invisible in every other view of the rig, and looks exactly
                // like a solver bug (see the joints-per-body dump below).
                if (debug) {
                    std::fprintf(stderr,
                                 "[phys]   fixture bone %d shapeType=%d density=%g scale=%g\n",
                                 static_cast<int>(pb.boneIndex), static_cast<int>(ps.shapeType),
                                 ps.density, ps.scale);
                }
            } else {
                ++skippedShapes;
            }
        }
    }

    auto boneOfBody = [&](sb::BodyId id) {
        for (const Link& l : links_) {
            if (l.body == id) return l.bone;
        }
        return -1;
    };

    for (const w2::PhysicsJoint& pj : phys.joints) {
        if (pj.bodyAIndex >= bodyOf.size() || pj.bodyBIndex >= bodyOf.size()) {
            continue;
        }
        const sb::BodyId a = bodyOf[pj.bodyAIndex];
        const sb::BodyId b = bodyOf[pj.bodyBIndex];
        if (a < 0 || b < 0) {
            continue;
        }
        const std::size_t id = static_cast<std::size_t>(std::max<i16>(0, pj.jointId));

        sb::JointDef def;
        def.bodyA = a;
        def.bodyB = b;
        // Every WoW joint def leaves this false, which is what keeps the two
        // halves of a chain from colliding at the hinge (§3.4).
        def.collideConnected = false;

        switch (pj.jointType) {
        case w2::PhysicsJointType::Spherical:
            if (id >= phys.sphericalJoints.size()) continue;
            def.kind = sb::JointKind::Spherical;
            def.localAnchorA = ToVec4(phys.sphericalJoints[id].anchorA);
            def.localAnchorB = ToVec4(phys.sphericalJoints[id].anchorB);
            break;
        case w2::PhysicsJointType::Shoulder: {
            if (id >= phys.shoulderJoints.size()) continue;
            const w2::ShoulderJoint& sj = phys.shoulderJoints[id];
            const sb::Transform fa = FrameToTransform(sj.frameA);
            const sb::Transform fb = FrameToTransform(sj.frameB);
            def.kind = sb::JointKind::Shoulder;
            def.localFrameA = fa.rotation;
            def.localFrameB = fb.rotation;
            // **Not the symmetric assignment the names suggest.** Every family
            // except distance and spherical pins *B's origin* at the point
            // `localAnchorB` expressed in **A's** frame, and ignores
            // `localAnchorA` for position entirely (`snowball/joint.cpp`,
            // `Resolve` — measured, not assumed). So the A-space slot has to
            // receive frameA's origin.
            //
            // Feeding it frameB's origin instead is silent and total: a joint
            // frame sits at its own body's origin, so frameB's origin is ~0,
            // and every link pins B onto A. The chain concertinas into a point
            // and the skinned mesh stretches to follow — which reads as "the
            // joints are broken" when they are holding perfectly, at the wrong
            // place.
            def.localAnchorA = fb.position;
            def.localAnchorB = fa.position;
            def.coneAngle = sj.coneAngle * kDegreesToRadians;
            def.lowerTwist = sj.lowerTwistAngle * kDegreesToRadians;
            def.upperTwist = sj.upperTwistAngle * kDegreesToRadians;
            // **On for modern content, and that is a dated decision, not a contradiction of the
            // binary.** The 6.0.1 loader provably ships the limit off —
            // `CPhysicsShoulderJointDef::CreateInstance` @ 0x1017269B0 zeroes `def+132` and
            // never copies over it — but 6.0.1 predates `SHJ2`, so for its content there were
            // no twist angles to honour in the first place. This model authors real ranges
            // (±20/±10/±5 degrees, not 0/0) in a chunk only later clients read, and the visible
            // cost of dropping them is exactly what it sounds like: every strip free to
            // corkscrew about its own length. `WDX_PHYSICS_TWIST=0` restores the 6.0.1 reading.
            def.enableTwistLimit = twist;
            // **`motorMode` is the enable; a zero `maxMotorTorque` is off, not unclamped.**
            //
            // Measured on Lor'themar: 19 of 23 shoulders are `mode=0 torque=0`, and 4 are
            // `mode=1 torque=0.01`. `motorFrequencyHz` is 1 and damping 0.7 on every one of
            // them, which is what an authored default looks like when it is written whether or
            // not the drive runs — so gating on the frequency enables all 23.
            //
            // The previous gate did exactly that *and* read torque 0 as unbounded, which put an
            // unclamped 1 Hz spring on nearly every joint in the rig. That is not a weak
            // restoring force: it holds each chain at its authored rest orientation hard enough
            // that gravity never visibly sags it, so the cloth hangs in bind pose and the whole
            // rig reads as "the joints are solving wrong" while every joint solves fine.
            //
            // Measured either way on Lor'themar, because the old gate looked like a prime
            // suspect for a rig that would not sag: it is not. Both gates settle bone 161 at
            // ~0.85 below its seed. The gate below is the better *reading* of the file; it is
            // not the fix for anything.
            def.enableAngularSpring = spring && sj.motorMode != 0 && sj.maxMotorTorque > 0.0f;
            def.springHertz = sj.motorFrequencyHz;
            def.springDamping = sj.motorDampingRatio;
            def.maxSpringTorque = sj.maxMotorTorque;
            if (debug) {
                std::fprintf(stderr,
                             "[phys]   shoulder motor: mode=%u torque=%g freq=%g damping=%g\n",
                             sj.motorMode, sj.maxMotorTorque, sj.motorFrequencyHz,
                             sj.motorDampingRatio);
            }
            break;
        }
        case w2::PhysicsJointType::Weld: {
            if (id >= phys.weldJoints.size()) continue;
            const w2::WeldJoint& wj = phys.weldJoints[id];
            const sb::Transform fa = FrameToTransform(wj.frameA);
            const sb::Transform fb = FrameToTransform(wj.frameB);
            def.kind = sb::JointKind::Weld;
            def.localFrameA = fa.rotation;
            def.localFrameB = fb.rotation;
            // A-space slot, as above.
            def.localAnchorA = fb.position;
            def.localAnchorB = fa.position;
            break;
        }
        case w2::PhysicsJointType::Distance:
            if (id >= phys.distanceJoints.size()) continue;
            def.kind = sb::JointKind::Distance;
            def.localAnchorA = ToVec4(phys.distanceJoints[id].localAnchorA);
            def.localAnchorB = ToVec4(phys.distanceJoints[id].localAnchorB);
            def.restLength = phys.distanceJoints[id].distance;
            break;
        case w2::PhysicsJointType::Revolute: {
            if (id >= phys.revoluteJoints.size()) continue;
            const w2::RevoluteJoint& rj = phys.revoluteJoints[id];
            const sb::Transform fa = FrameToTransform(rj.frameA);
            const sb::Transform fb = FrameToTransform(rj.frameB);
            // A real hinge now — Snowball's Revolute reproduces `dmRevoluteJoint` as measured
            // on the oracle (probe_revolute.py; G7 `revolute_hinge`): the angular reference is
            // **the anchor-A slot read as a quaternion** — the weld convention on a second
            // family — whose z axis is the hinge axis; the 2-DOF swing lock arms on that
            // reference alone, and the byte gates the [lower, upper] twist limit about it. A
            // zero reference leaves the whole angular half silently inert, which is what the
            // previous mapping (frameB's ~zero origin in this slot) would have authored.
            //
            // The reference is frameA's quaternion, plainly, and frameB rides `localFrameB` for
            // the B side — Snowball reads it there for revolutes precisely so modern content's
            // two authored frames both land (a 2014 def leaves it neutral and is unaffected).
            // An earlier draft folded conj(frameB) into the reference and measured B raw
            // instead: bind pose stays exact under that composition, but the twist coordinate
            // is conjugated by frameB, and an asymmetric range like [-90, 0] then bounds the
            // wrong direction — the chain roots swung backward into the torso. The motor is
            // not mapped; see the Snowball dispatch note.
            def.kind = sb::JointKind::Revolute;
            def.localFrameA = fa.rotation;
            def.localFrameB = fb.rotation;
            def.localAnchorA = fa.rotation;
            def.localAnchorB = fa.position;  // A-space slot, as above
            def.lowerTwist = rj.lowerAngle * kDegreesToRadians;
            def.upperTwist = rj.upperAngle * kDegreesToRadians;
            def.enableTwistLimit = true;
            break;
        }
        // Prismatic is a slider; there is no nearby family to stand in for one,
        // and the whole corpus holds 12 of them against 1158 shoulders. Dropped.
        default:
            continue;
        }
        scene_.AddJoint(def);
        ++jointCount;

        if (debug) {
            std::fprintf(stderr,
                         "[phys] joint filetype=%d kind=%d %d(%s) -> %d(%s) "
                         "aB=(%.3f %.3f %.3f) cone=%g twist=[%g %g]\n",
                         static_cast<int>(pj.jointType), static_cast<int>(def.kind),
                         boneOfBody(a), scene_.Get(a).IsDynamic() ? "dyn" : "kine",
                         boneOfBody(b), scene_.Get(b).IsDynamic() ? "dyn" : "kine",
                         def.localAnchorB.x, def.localAnchorB.y, def.localAnchorB.z,
                         def.coneAngle, def.lowerTwist, def.upperTwist);
        }
    }

    // The ground. The shipped client collides cloth against Domino's baked world collider
    // (`DOMINO_SPEC.md` phase 2h — the BVH the terrain bakes to); the viewer's world is a flat
    // plane at z=0, so a flat plane is what stands in for it: one static box whose top face is
    // the grid. A box rather than a halfspace because it is the shape the narrowphase already
    // proves, and 100 yards of it because model space is yards and nothing simulated leaves a
    // model's own neighbourhood.
    //
    // Two honest limits, both fine for a viewer: the plane lives in *model* space, so it is the
    // ground only while the host leaves the actor at the origin (this viewer does); and the
    // friction is a viewer choice, not a recovered material — the world collider's material
    // response has not been transcribed. `WDX_PHYSICS_GROUND=0` removes it.
    {
        const char* g = std::getenv("WDX_PHYSICS_GROUND");
        if (!(g != nullptr && g[0] == '0' && g[1] == '\0')) {
            sb::BodyDef groundDef;
            groundDef.type = sb::BodyType::Static;
            const sb::BodyId ground = scene_.AddBody(groundDef);
            sb::Transform xf;
            xf.position = sb::Vec4{0.0f, 0.0f, -1.0f, 0.0f};  // top face exactly at z=0
            const sb::ShapeId slab =
                shapes_.Add(sb::MakeBox(sb::Vec4{100.0f, 100.0f, 1.0f, 0.0f}, xf));
            scene_.AddFixture(ground, slab, 0.0f, 0.8f, 0.0f);
        }
    }

    claims_.clear();
    for (const Link& l : links_) {
        if (l.dynamic) {
            claims_.push_back(BoneClaim{l.bone, Matrix44f::identity()});
        }
    }

    // **Print the mass.** A massless dynamic body still falls — gravity is applied to velocity
    // regardless of mass — but no joint can hold it, because every joint impulse is scaled by an
    // inverse mass of zero. It reads as "the joints are broken" and is not, and it is
    // invisible in a dump of bodies, joints or positions.
    //
    // This is not hypothetical: Snowball had no capsule mass association, so the eleven bodies
    // on Lor'themar's capsule fixtures came out at zero and the four chains hanging off them
    // fell through the floor while the sixteen polytope-shaped ones held perfectly.
    if (debug) {
        std::fprintf(stderr, "[phys] bodies=%zu dyn=%zu fixtures=%d joints=%d/%zu skipped=%d\n",
                     links_.size(), claims_.size(), fixtureCount, jointCount,
                     phys.joints.size(), skippedShapes);
        // Joints per body. A dynamic body that appears in no joint is attached
        // to nothing: it free-falls while every joint in the rig holds, which
        // is indistinguishable from "the solver is broken" unless you count.
        std::vector<int> jointsOn(links_.size(), 0);
        for (i32 j = 0; j < scene_.JointCount(); ++j) {
            const sb::Joint& jt = scene_.GetJoint(j);
            for (std::size_t k = 0; k < links_.size(); ++k) {
                if (links_[k].body == jt.bodyA || links_[k].body == jt.bodyB) {
                    ++jointsOn[k];
                }
            }
        }
        for (std::size_t k = 0; k < links_.size(); ++k) {
            const sb::Body& b = scene_.Get(links_[k].body);
            std::fprintf(stderr,
                         "[phys]   bone %d %s mass=%g fixtures=%zu joints=%d gravityScale=%g "
                         "linDamp=%g angDamp=%g\n",
                         links_[k].bone, links_[k].dynamic ? "dyn " : "kine", b.mass,
                         b.fixtures.size(), jointsOn[k], b.gravityScale, b.linearDamping,
                         b.angularDamping);
        }
    }
}

void WowPhysicsStage::Seed(const FrameState& fs) {
    for (Link& l : links_) {
        if (static_cast<std::size_t>(l.bone) >= fs.boneWorldMatrices.size()) {
            continue;
        }
        const Matrix44f& m = fs.boneWorldMatrices[static_cast<std::size_t>(l.bone)];
        const sb::Transform xf = BoneFrame(m, l.pivot);
        scene_.SetTransform(l.body, xf);
        scene_.SetLinearVelocity(l.body, sb::Vec4{});
        scene_.SetAngularVelocity(l.body, sb::Vec4{});
        l.seedPos = xf.position;
    }
    seeded_ = true;
}

void WowPhysicsStage::DriveKinematic(const FrameState& fs, f32 dt) {
    if (dt <= 0.0f) {
        return;
    }
    const f32 invDt = 1.0f / dt;

    for (const Link& l : links_) {
        if (l.dynamic || static_cast<std::size_t>(l.bone) >= fs.boneWorldMatrices.size()) {
            continue;
        }
        const Matrix44f& m = fs.boneWorldMatrices[static_cast<std::size_t>(l.bone)];
        const sb::Transform target = BoneFrame(m, l.pivot);
        const sb::Vec4 targetPos = target.position;
        sb::Vec4 targetRot = target.rotation;

        const sb::Transform& cur = scene_.Get(l.body).transform;

        // Hemisphere fix: q and -q are the same orientation, and interpolating
        // toward the far one takes the long way round — visibly, on a bone that
        // crosses the boundary mid-animation.
        const f32 dot = cur.rotation.x * targetRot.x + cur.rotation.y * targetRot.y +
                        cur.rotation.z * targetRot.z + cur.rotation.w * targetRot.w;
        if (dot < 0.0f) {
            targetRot = sb::Vec4{-targetRot.x, -targetRot.y, -targetRot.z, -targetRot.w};
        }

        const sb::Vec4 dp = targetPos - cur.position;
        const sb::Vec4 dq{targetRot.x - cur.rotation.x, targetRot.y - cur.rotation.y,
                          targetRot.z - cur.rotation.z, targetRot.w - cur.rotation.w};

        // §5.2: how far past the per-step caps the required motion is, floored
        // at 0.8. `t` is therefore never below 0.8 and rises to 1 for a large
        // move, which is what makes a warped character's cloth follow rather
        // than whip.
        const f32 t = std::min(1.0f, std::max(kSnapFloor,
            std::max(Excess(Length3(dp) * invDt, kMaxLinearStep * invDt),
                     Excess(2.0f * Length3(dq) * invDt, kMaxAngularStep * invDt))));

        sb::Transform snapped;
        snapped.position = cur.position + dp * t;
        snapped.rotation = sb::QuatNormalize(sb::Vec4{cur.rotation.x + dq.x * t,
                                                  cur.rotation.y + dq.y * t,
                                                  cur.rotation.z + dq.z * t,
                                                  cur.rotation.w + dq.w * t});
        scene_.SetTransform(l.body, snapped);

        // The residual becomes velocity rather than being snapped away. That is
        // what the contact solver reads, and it is why the cloth is dragged
        // instead of merely displaced — the whole point of the split.
        scene_.SetLinearVelocity(l.body, (targetPos - snapped.position) * invDt);
        const sb::Vec4 rq{targetRot.x - snapped.rotation.x, targetRot.y - snapped.rotation.y,
                          targetRot.z - snapped.rotation.z, targetRot.w - snapped.rotation.w};
        // `IntegrateRotation` is `dq/dt = 0.5 * w (x) q` with **world-frame** w on the left, so
        // the inverse is `w = 2 * dq (x) conj(q)`. Routed through `QuatMultiply` rather than
        // expanded by hand: the expansion here had the cross-product term negated on all three
        // components, which is invisible at rest — `dq` is zero in a static pose — and turns
        // every moving collider's spin the wrong way the moment an animation plays.
        const sb::Vec4& q = snapped.rotation;
        const sb::Vec4 w = sb::QuatMultiply(rq, sb::Vec4{-q.x, -q.y, -q.z, q.w});
        scene_.SetAngularVelocity(l.body, sb::Vec4{2.0f * w.x * invDt, 2.0f * w.y * invDt,
                                                   2.0f * w.z * invDt, 0.0f});
    }
}

void WowPhysicsStage::WriteBack(FrameState& fs) {
    std::size_t claim = 0;
    for (const Link& l : links_) {
        if (!l.dynamic) {
            continue;
        }
        if (static_cast<std::size_t>(l.bone) < fs.boneWorldMatrices.size()) {
            const Matrix44f m = ToMatrix(scene_.Get(l.body).transform, l.pivot);
            fs.boneWorldMatrices[static_cast<std::size_t>(l.bone)] = m;
            if (claim < claims_.size()) {
                claims_[claim].transform = m;
            }
        }
        ++claim;
    }
}

void WowPhysicsStage::Run(FrameState& fs, const PoseStageContext& ctx) {
    if (links_.empty() || fs.boneWorldMatrices.empty()) {
        return;
    }
    if (!seeded_) {
        Seed(fs);
        return;
    }

    // Real time, not animation time: a paused actor's cloth still settles, and
    // that is what `PoseStageContext::frameDtMs` exists to say.
    const f32 dt = std::min(static_cast<f32>(ctx.frameDtMs) * 0.001f, kMaxStep);
    if (dt <= 0.0f) {
        return;
    }

    const bool debug = std::getenv("WDX_PHYSICS_DEBUG") != nullptr;

    DriveKinematic(fs, dt);
    scene_.Step(dt);
    WriteBack(fs);

    // **Displacement from the seed pose, not from the animated one.** The
    // earlier probe here compared each simulated body against
    // `fs.boneWorldMatrices` and reported millimetres — which read as "the rig
    // tracks the animation closely" and was measuring nothing of the sort. A
    // claimed bone's entry in that array *is* last frame's physics result,
    // arriving back through the override path, so the comparison was physics
    // against itself: a per-frame delta wearing the label of an error.
    //
    // Against the seed it answers the question actually being asked — has
    // gravity moved this rig at all since it started.
    if (debug && (debugFrame_ + 1) % 200 == 0) {
        f32 worst = 0.0f, drop = 0.0f;
        i32 worstBone = -1;
        for (const Link& l : links_) {
            if (!l.dynamic) continue;
            const sb::Vec4& p = scene_.Get(l.body).transform.position;
            const f32 d = Length3(p - l.seedPos);
            if (d > worst) { worst = d; worstBone = l.bone; }
            drop = std::min(drop, p.z - l.seedPos.z);
        }
        std::fprintf(stderr, "[phys] f%d moved-from-seed max=%.4f (bone %d) lowest dz=%.4f\n",
                     debugFrame_ + 1, worst, worstBone, drop);

        // **Are the joints actually holding?** Every non-spherical family pins B's origin at
        // `localAnchorB` expressed in A's frame, so the residual is a direct read of how well
        // the position constraint converged. A chain that has visibly come apart and one that
        // is merely posed oddly look the same in a wireframe; they do not look the same here.
        f32 worstSep = 0.0f, worstCone = 0.0f;
        i32 sepJoint = -1, coneJoint = -1;
        for (i32 j = 0; j < scene_.JointCount(); ++j) {
            const sb::Joint& jt = scene_.GetJoint(j);
            const sb::Transform& ta = scene_.Get(jt.bodyA).transform;
            const sb::Transform& tb = scene_.Get(jt.bodyB).transform;
            const sb::Vec4 want =
                ta.position + sb::RotationMatrix(ta.rotation).Transform(jt.localAnchorB);
            const f32 sep = Length3(tb.position - want);
            if (sep > worstSep) { worstSep = sep; sepJoint = BoneOf(jt.bodyB); }
            // Cone: the angle between the two frames' local +Z against the limit it was given.
            auto axis = [](const sb::Vec4& q, const sb::Vec4& f) {
                return sb::RotationMatrix(sb::QuatMultiply(q, f))
                    .Transform(sb::Vec4{0.0f, 0.0f, 1.0f, 0.0f});
            };
            const f32 dot = std::clamp(
                sb::Dot3(axis(ta.rotation, jt.localFrameA), axis(tb.rotation, jt.localFrameB)),
                -1.0f, 1.0f);
            const f32 over = std::acos(dot) - jt.coneAngle;
            if (over > worstCone) { worstCone = over; coneJoint = BoneOf(jt.bodyB); }
        }
        std::fprintf(stderr,
                     "[phys]      joint separation max=%.4f (bone %d), cone overshoot max=%.1f deg"
                     " (bone %d)\n",
                     worstSep, sepJoint, worstCone * 57.2957795f, coneJoint);
    }

    // Kinematic bodies never appear in Claims(), so a dump of the claimed
    // bones alone cannot tell "the chain sags off a fixed anchor" from "the
    // whole rig is falling". This prints both halves.
    if (std::getenv("WDX_PHYSICS_DEBUG") != nullptr) {
        ++debugFrame_;
        // Contact count. Cloth that hangs correctly but passes through the body
        // is indistinguishable from cloth that hangs correctly and is never
        // asked to collide, unless you count the pairs the broadphase found.
        if (debugFrame_ % 100 == 0 || debugFrame_ == 1) {
            int touching = 0, withKinematic = 0;
            std::string pairs;
            for (sb::ContactId c = scene_.FirstContact(); c >= 0;) {
                const sb::Contact* ct = scene_.GetContact(c);
                if (ct == nullptr) break;
                if (ct->touching) {
                    ++touching;
                    const bool kine = !scene_.Get(ct->bodyA).IsDynamic() ||
                                      !scene_.Get(ct->bodyB).IsDynamic();
                    if (kine) {
                        ++withKinematic;
                        char buf[32];
                        std::snprintf(buf, sizeof buf, " %d-%d", BoneOf(ct->bodyA),
                                      BoneOf(ct->bodyB));
                        pairs += buf;
                    }
                }
                c = scene_.NextContact(c);
            }
            std::fprintf(stderr, "[phys] f%d contacts=%d touching=%d vs-kinematic=%d:%s\n",
                         debugFrame_, scene_.ContactCount(), touching, withKinematic,
                         pairs.c_str());
        }
        // Joint-frame convention check. A shoulder's cone is measured between the
        // two frames' local +Z, and a rig is authored at rest — so at bind pose
        // `dot` must be near +1. If it is near -1 or scattered, `FrameToTransform`
        // is handing over the conjugate and every cone limit is constraining a
        // direction the author never meant.
        if (debugFrame_ == 1) {
            f32 sumDirect = 0.0f, sumConj = 0.0f;
            int n = 0;
            for (i32 j = 0; j < scene_.JointCount(); ++j) {
                const sb::Joint& jt = scene_.GetJoint(j);
                const sb::Vec4& qa = scene_.Get(jt.bodyA).transform.rotation;
                const sb::Vec4& qb = scene_.Get(jt.bodyB).transform.rotation;
                auto axis = [](const sb::Vec4& q, const sb::Vec4& f) {
                    return sb::RotationMatrix(sb::QuatMultiply(q, f))
                        .Transform(sb::Vec4{0.0f, 0.0f, 1.0f, 0.0f});
                };
                auto conj = [](const sb::Vec4& q) {
                    return sb::Vec4{-q.x, -q.y, -q.z, q.w};
                };
                sumDirect += sb::Dot3(axis(qa, jt.localFrameA), axis(qb, jt.localFrameB));
                sumConj += sb::Dot3(axis(qa, conj(jt.localFrameA)),
                                    axis(qb, conj(jt.localFrameB)));
                ++n;
            }
            if (n > 0) {
                std::fprintf(stderr,
                             "[phys] joint-frame cone alignment: as-read %.4f, conjugate %.4f "
                             "(want ~+1)\n",
                             sumDirect / static_cast<f32>(n), sumConj / static_cast<f32>(n));
            }
        }
        if (debugFrame_ == 1 || debugFrame_ == 300) {
            for (const Link& l : links_) {
                const sb::Vec4& p = scene_.Get(l.body).transform.position;
                std::fprintf(stderr, "[phys] f%d bone %d %s pos=(%.3f %.3f %.3f)\n",
                             debugFrame_, l.bone, l.dynamic ? "dyn " : "kine", p.x, p.y, p.z);
            }
        }
    }
}

} // namespace

Matrix44f RoundTripBoneFrame(const Matrix44f& palette, const Vector3f& pivot) {
    return ToMatrix(BoneFrame(palette, pivot), pivot);
}

std::unique_ptr<animation::IPoseStage> CreateWowPhysicsStage(const ::whiteout::m2::Model& model) {
    if (!model.physics.has_value()) {
        return nullptr;
    }
    auto stage = std::make_unique<WowPhysicsStage>(model);
    // No dynamic body means nothing is ever written back, and an inert stage
    // still costs a virtual call and a claim copy per actor per frame.
    if (stage->Empty()) {
        return nullptr;
    }
    return stage;
}

} // namespace whiteout::flakes::renderer::profiles::wow
