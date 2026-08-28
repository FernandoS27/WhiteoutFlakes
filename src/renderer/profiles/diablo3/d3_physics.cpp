#include "renderer/profiles/diablo3/d3_physics.h"

#include "io/d3/d3_types.h"
#include "renderer/profiles/diablo3/d3_collision.h"
#include "renderer/physics/ground_plane.h"

#include "snowball/joint.h"
#include "snowball/polytope.h"
#include "snowball/scene.h"
#include "snowball/shape.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace whiteout::flakes::renderer::profiles::diablo3 {
namespace {

namespace sb = ::snowball;
using ::whiteout::flakes::renderer::animation::BoneClaim;
using ::whiteout::flakes::renderer::animation::IPoseStage;
using ::whiteout::flakes::renderer::animation::PoseStageContext;
using ::whiteout::flakes::renderer::model::FrameState;

/// Diablo III's gravity, and a third number out of one engine: WoW builds its
/// world at `(0, 0, -10)` and StarCraft II at `(0, 0, -8.8)`. The step policy
/// belongs to the host, not to Domino, and so does this.
constexpr sb::Vec4 kGravity{0.0f, 0.0f, -32.2f, 0.0f};

/// @name The step
///
/// **Not measured — this is StarCraft II's, standing in.** D3's world step is
/// not in the per-actor update (`sub_71003E2240` applies wind and a vertical
/// acceleration and steps nothing), and the global caller has not been
/// recovered; see D3_PHYSICS_PLAN.md O4. A fixed 1/60 behind a capped
/// accumulator is the conservative stand-in because it cannot explode on a
/// stalled frame, and it is recorded as a divergence rather than a finding.
/// @{
constexpr f32 kSubStep = 1.0f / 60.0f;
constexpr i32 kMaxSubSteps = 3;
constexpr f32 kMaxAccumulator = 0.05f;
constexpr i32 kVelocityIterations = 8;
constexpr i32 kPositionIterations = 2;
/// @}

/// A radius floor the client applies **after** the body scale:
/// `max(bodyScale * flRadius, 0.0833)` in `PhysicsBridge_CreateFixture`. One
/// inch in D3's units, and the reason a hair-thin authored capsule still
/// collides.
constexpr f32 kMinRadius = 0.0833f;

/// A bone matrix row this short is treated as degenerate rather than normalised.
constexpr f32 kTinyScale = 1.0e-8f;

/// @brief `g_density(nBodyClass)`, the table at `0x7100E5DA60`.
///
/// The fixture density is `g_density(phy.nBodyClass) * shape.flScaleX` — the
/// class picks a material density and the shape scales it. Fourteen entries;
/// anything outside 1..14 takes the same 1.9375 the switch's default returns,
/// which is also what a missing `.phy` gets here.
f32 DensityForClass(i32 bodyClass) {
    static constexpr f32 kTable[14] = {3.75f,   1.5625f, 1.9375f, 1.9375f, 1.9375f,
                                       2.5f,    2.5f,    1.5625f, 3.75f,   3.75f,
                                       1.5625f, 1.5625f, 2.5f,    2.5f};
    if (bodyClass < 1 || bodyClass > 14)
        return 1.9375f;
    return kTable[bodyClass - 1];
}

/// @name `.phy` defaults, from the registered asset the client falls back on
///
/// The corpus says these are also the shipped modes — friction 0.3 and
/// restitution 0.0 on 64 of 74 files — so a model with no `.phy` behaves like
/// the overwhelming majority of models that have one.
/// @{
constexpr f32 kDefaultFriction = 0.3f;
constexpr f32 kDefaultRestitution = 0.0f;
/// @}

sb::Vec4 ToVec4(const Vector3f& v, f32 s) {
    return sb::Vec4{v.x * s, v.y * s, v.z * s, 0.0f};
}

f32 Length3(const sb::Vec4& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

/// @brief A bone's model-space frame, with the scale taken back out of it.
///
/// D3 composes world poses as `parentRot * localRot`, `parentTrans +
/// parentScale * (parentRot . localTrans)`, `parentScale * localScale` — one
/// scale float all the way down (`Skeleton_ComposeWorldPose`). So the three row
/// lengths of a bone matrix are equal by construction and one of them is the
/// body scale the fixture builder wants. All three are still measured, and the
/// smallest taken, so a caller that hands this a non-D3 matrix gets a
/// conservative collider rather than a stretched one.
struct BoneFrame {
    sb::Transform xf;
    f32 scale = 1.0f;
};

BoneFrame DecomposeBone(const Matrix44f& m) {
    BoneFrame out;
    f32 n[3];
    for (int i = 0; i < 3; ++i) {
        n[i] = std::sqrt(m.data[i][0] * m.data[i][0] + m.data[i][1] * m.data[i][1] +
                         m.data[i][2] * m.data[i][2]);
    }
    out.scale = (std::min)({n[0], n[1], n[2]});
    out.xf.position = sb::Vec4{m.data[3][0], m.data[3][1], m.data[3][2], 0.0f};

    if (n[0] <= kTinyScale || n[1] <= kTinyScale || n[2] <= kTinyScale) {
        // A collapsed basis is not a rotation, and the identity is the only
        // answer here that cannot spread NaN through the solver.
        out.xf.rotation = sb::Vec4{0.0f, 0.0f, 0.0f, 1.0f};
        out.scale = 1.0f;
        return out;
    }

    // Bone matrices are **row-vector**: row `i` is the image of basis vector
    // `i`, so the column-major basis the extraction wants is the transpose of
    // the top-left 3x3. Reading it the other way round yields the conjugate,
    // which looks correct for any symmetric pose and mirrors every other one.
    f32 r[3][3];
    for (int i = 0; i < 3; ++i) {
        const f32 inv = 1.0f / n[i];
        for (int j = 0; j < 3; ++j)
            r[j][i] = m.data[i][j] * inv;
    }

    const f32 trace = r[0][0] + r[1][1] + r[2][2];
    sb::Vec4 q{};
    if (trace > 0.0f) {
        const f32 s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s, 0.25f * s};
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const f32 s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;
        q = {0.25f * s, (r[0][1] + r[1][0]) / s, (r[0][2] + r[2][0]) / s, (r[2][1] - r[1][2]) / s};
    } else if (r[1][1] > r[2][2]) {
        const f32 s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;
        q = {(r[0][1] + r[1][0]) / s, 0.25f * s, (r[1][2] + r[2][1]) / s, (r[0][2] - r[2][0]) / s};
    } else {
        const f32 s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;
        q = {(r[0][2] + r[2][0]) / s, (r[1][2] + r[2][1]) / s, 0.25f * s, (r[1][0] - r[0][1]) / s};
    }
    out.xf.rotation = sb::QuatNormalize(q);
    return out;
}

/// @brief The exact inverse of @ref DecomposeBone: a simulated pose wearing the
///        animated scale.
Matrix44f ToMatrix(const sb::Transform& xf, f32 scale) {
    const sb::Mtx b = sb::RotationMatrix(xf.rotation);
    Matrix44f m = Matrix44f::identity();
    // Row `j` of `data` is column `j` of `Mtx`, componentwise — the two are
    // *not* related by a transpose. Work it out from @ref DecomposeBone, which
    // this has to invert exactly.
    m.data[0][0] = b.c[0].x * scale;
    m.data[0][1] = b.c[0].y * scale;
    m.data[0][2] = b.c[0].z * scale;
    m.data[1][0] = b.c[1].x * scale;
    m.data[1][1] = b.c[1].y * scale;
    m.data[1][2] = b.c[1].z * scale;
    m.data[2][0] = b.c[2].x * scale;
    m.data[2][1] = b.c[2].y * scale;
    m.data[2][2] = b.c[2].z * scale;
    m.data[3][0] = xf.position.x;
    m.data[3][1] = xf.position.y;
    m.data[3][2] = xf.position.z;
    return m;
}

/// @brief The pre-rotation a revolute frame is multiplied by.
///
/// `q x c` in `sub_7100319F40`'s first case, built from the lane constants at
/// `0x7100E45110/20/30` -- worked out componentwise and matched against the
/// quaternion product, exactly. `c = (0.5, 0.5, 0.5, 0.5)` is a 120-degree
/// rotation about `(1,1,1)/sqrt(3)`: a **cyclic axis permutation**, x->y->z->x.
///
/// D3 authors the constrained axis on a different axis than Domino expects, and
/// only for the two families that *name* one. Spherical and weld take their
/// frames raw.
constexpr sb::Vec4 kRevolutePreRotation{0.5f, 0.5f, 0.5f, 0.5f};
/// The shoulder's, and the conjugate of the revolute's -- the same permutation
/// run the other way.
constexpr sb::Vec4 kShoulderPreRotation{-0.5f, -0.5f, -0.5f, 0.5f};

/// @name The kinematic drive, for an anchored rig's anchors
///
/// **Not D3's own** -- see the header note. Its shape is WoW's and SC2's: past
/// either cap the body is teleported by the excess and the rest is handed over
/// as velocity, so a moving anchor drags what hangs off it instead of passing
/// through. The caps are StarCraft II's, which is the closer of the two hosts
/// here (both drive from a real world transform rather than a palette).
/// @{
constexpr f32 kSnapLinear = 0.25f;
constexpr f32 kSnapAngular = 0.3926991f;  // pi/8
constexpr f32 kDriveEpsilon = 9.0e-6f;
/// @}

/// @brief The D3 joint types that reach a `switch` case at all.
///
/// 0 -> Revolute, 1 -> Shoulder, 2 -> Spherical, 3 -> Weld. **Type 4 has no
/// case**, and it is 4,475 of the corpus's 14,570 constraints: the builder's
/// `default:` falls through to the allocation-failure path, so 31% of shipped
/// constraints create no joint in 2.6.2. Treating an unknown type as a
/// spherical would over-constrain nearly a third of every rig.
bool JointKindOf(i32 d3Type, sb::JointKind& out) {
    switch (d3Type) {
    case 0: out = sb::JointKind::Revolute; return true;
    case 1: out = sb::JointKind::Shoulder; return true;
    case 2: out = sb::JointKind::Spherical; return true;
    case 3: out = sb::JointKind::Weld; return true;
    default: return false;
    }
}

sb::Vec4 QuatOf(const Vector4f& v) {
    return sb::Vec4{v.x, v.y, v.z, v.w};
}

/// @brief `ConstraintParameters` -> `snowball::JointDef`, or false for no joint.
///
/// The frames are **`tFrameB` and `tFrameC`**, not `tFrameA`: runtime +64 and
/// +92 are disk +120 and +148 under the -56 shift that collapses `szName`'s 64
/// inline bytes to a handle. `tFrameA` and `vUnknown34` are read by nothing in
/// the joint path.
///
/// Anchors are the frames' translations times the **actor** scale -- not the
/// bone's. Our bone matrices are model-space, so that factor is 1 here and the
/// `actorScale^4` break thresholds are likewise unscaled; both re-enter through
/// `ctx.world` after the rig has run.
///
/// The break thresholds themselves have no Snowball counterpart: nothing in the
/// engine destroys a joint, and D3 does it from the outside by id
/// (`ConstraintParameters::szBreakEffect`, `sub_71003E20C0`). They are read and
/// dropped rather than silently mapped onto something else.
bool MakeJointDef(const d3n::ConstraintParameters& c, sb::BodyId bodyA, sb::BodyId bodyB,
                  sb::JointDef& def) {
    sb::JointKind kind{};
    if (!JointKindOf(c.eConstraintType, kind))
        return false;

    const sb::Vec4 qB = QuatOf(c.tFrameB.qRotation);
    const sb::Vec4 qC = QuatOf(c.tFrameC.qRotation);
    const sb::Vec4 anchorB = ToVec4(c.tFrameB.vTranslation, 1.0f);
    const sb::Vec4 anchorC = ToVec4(c.tFrameC.vTranslation, 1.0f);

    def = sb::JointDef{};
    def.kind = kind;
    def.bodyA = bodyA;
    def.bodyB = bodyB;
    def.collideConnected = (c.dwFlags & 1) != 0;

    switch (kind) {
    case sb::JointKind::Revolute: {
        const sb::Vec4 fa = sb::QuatMultiply(qB, kRevolutePreRotation);
        const sb::Vec4 fb = sb::QuatMultiply(qC, kRevolutePreRotation);
        def.localFrameA = fa;
        def.localFrameB = fb;
        // The hinge reference is the anchor-A slot read as a quaternion, whose
        // z axis is the hinge axis; a zero reference leaves the whole angular
        // half silently inert.
        def.localAnchorA = fa;
        // Every family but distance and spherical pins *B's origin* at the
        // point `localAnchorB` expressed in **A's** frame and ignores
        // `localAnchorA` for position, so the A-space slot takes frame A's
        // origin.
        def.localAnchorB = anchorB;
        def.lowerTwist = c.flLimitLower;
        def.upperTwist = c.flLimitUpper;
        // **`dwFlags` bit 1, and only for the revolute.** The shoulder below
        // enables its twist unconditionally.
        def.enableTwistLimit = (c.dwFlags & 2) != 0;
        break;
    }

    case sb::JointKind::Shoulder: {
        const sb::Vec4 fa = sb::QuatMultiply(qB, kShoulderPreRotation);
        const sb::Vec4 fb = sb::QuatMultiply(qC, kShoulderPreRotation);
        def.localFrameA = fa;
        def.localFrameB = fb;
        def.localAnchorA = fa;
        def.localAnchorB = anchorB;
        // Clamped to [10, 170] degrees by Snowball on construction, so a
        // zero-cone constraint gets a ten-degree cone rather than a lock.
        def.coneAngle = c.flConeAngle;
        def.lowerTwist = c.flTwistLower;
        def.upperTwist = c.flTwistUpper;
        def.enableTwistLimit = true;
        break;
    }

    case sb::JointKind::Spherical:
        // The one family that keeps the symmetric two-anchor rule; both
        // rotations are discarded outright.
        def.localAnchorA = anchorB;
        def.localAnchorB = anchorC;
        break;

    case sb::JointKind::Weld:
    default:
        // Frames raw -- no pre-rotation, because a weld names no axis.
        def.localFrameA = qB;
        def.localFrameB = qC;
        // The weld's angular target, read as a quaternion and deliberately not
        // normalised: its length is the gain on the position correction.
        def.localAnchorA = qB;
        def.localAnchorB = anchorB;
        // **D3's weld is soft and Snowball's is rigid**, the same divergence
        // StarCraft II's stage records. The client synthesises
        // `hz = max(1 - flParam06, 0) * 15 + 1` at damping 0.9 whenever the
        // explicit pair is unset -- which is every shipped constraint, both
        // halves being zero on all 14,570. Snowball runs its angular spring for
        // the shoulder only, so setting it here would be dead weight that reads
        // as implemented. A welded D3 attachment is stiffer here than in the
        // client: a piece that does not wobble, not one in the wrong place.
        break;
    }
    return true;
}

// ---------------------------------------------------------------------------

class D3PhysicsStage final : public IPoseStage {
public:
    D3PhysicsStage(const d3n::Appearances& app, std::span<const ::whiteout::u8> appBytes,
                   const d3n::Physics* phy, std::shared_ptr<D3PhysicsControl> control,
                   D3RigMode mode, i32 lod)
        : app_(&app), appBytes_(appBytes.begin(), appBytes.end()), mode_(mode), lod_(lod),
          control_(std::move(control)), scene_(shapes_, kGravity) {
        if (phy != nullptr) {
            friction_ = phy->flFriction;
            restitution_ = phy->flRestitution;
            linearDamping_ = phy->flLinearDamping;
            angularDamping_ = phy->flAngularDamping;
            density_ = DensityForClass(phy->nBodyClass);
        }
    }

    void Run(FrameState& fs, const PoseStageContext& ctx) override;

    std::vector<BoneClaim> Claims() const override {
        return claims_;
    }

    /// Everything this rig simulates comes out of the appearance and the ground
    /// plane, so it needs neither the aim target nor the host ground query.
    bool NeedsHostInputs() const override {
        return false;
    }

private:
    struct Link {
        i32 bone = -1;
        sb::BodyId body = -1;
        bool dynamic = false;
        /// A `dwFlags` bit-0 anchor: created dynamic and turned kinematic, then
        /// driven from the animated bone every frame. Only @ref
        /// D3RigMode::Ragdoll produces one.
        bool kinematic = false;
        /// This link's nearest bodied ancestor, or -1. A pure function of the
        /// bone tree once the bodies exist, so it is resolved at build rather
        /// than walked every frame.
        i32 ancestor = -1;
        /// The animated scale this bone carried when the rig was seeded,
        /// replayed into the write-back so a scaled model keeps its size.
        f32 scale = 1.0f;
        sb::Vec4 seedPos{};
    };

    void Build(const FrameState& fs);
    /// @return the body's link index, or -1 if the bridge would have destroyed
    ///         it for having no fixture.
    i32 AddBoneBody(std::size_t bone, const FrameState& fs, sb::BodyType type);
    void BuildBoneBodies(const FrameState& fs);
    void BuildAnchoredRig(const FrameState& fs);
    void BuildJoints();
    void DriveAnchors(const FrameState& fs, f32 invT);
    /// @return how many fixtures the body got. Zero means the bridge would have
    ///         destroyed it.
    int BuildFixtures(const d3n::BoneStructure& bone, sb::BodyId body, f32 bodyScale);
    void Seed(const FrameState& fs);
    void WriteBack(FrameState& fs, const PoseStageContext& ctx);
    void RebuildClaims(const FrameState& fs);
    void Teardown();

    const d3n::Appearances* app_ = nullptr;
    std::vector<::whiteout::u8> appBytes_;
    D3RigMode mode_ = D3RigMode::BoneBodies;
    i32 lod_ = kD3BoneBodyLod;
    std::shared_ptr<D3PhysicsControl> control_;

    sb::ShapeStore shapes_;
    sb::Scene scene_;
    std::vector<Link> links_;
    /// Bone -> index into @ref links_, or -1. Both constraint sources name
    /// *bones*, never bodies.
    std::vector<i32> linkOfBone_;
    std::vector<BoneClaim> claims_;
    /// The pose as the sampler produced it, snapshotted before the write-back
    /// overwrites the claimed bones — the write-back reads it to recover each
    /// unclaimed bone's animated local transform. A member so a per-actor stage
    /// allocates nothing per frame.
    std::vector<Matrix44f> animated_;
    std::vector<u8> written_;

    f32 friction_ = kDefaultFriction;
    f32 restitution_ = kDefaultRestitution;
    f32 linearDamping_ = 0.0f;
    f32 angularDamping_ = 0.0f;
    f32 density_ = DensityForClass(0);

    int joints_ = 0;
    bool built_ = false;
    /// Whether the host's switch was on last frame. Separate from
    /// @ref built_ because releasing a rig keeps its bodies and only stops
    /// claiming, and re-arming has to re-seed rather than resume.
    bool armed_ = false;
    f32 accumulator_ = 0.0f;
    int debugFrame_ = 0;
};

i32 D3PhysicsStage::AddBoneBody(std::size_t bone, const FrameState& fs, sb::BodyType type) {
    const BoneFrame frame = DecomposeBone(fs.boneWorldMatrices[bone]);

    sb::BodyDef def;
    def.type = type;
    def.position = frame.xf.position;
    def.linearDamping = linearDamping_;
    def.angularDamping = angularDamping_;
    // Both hardcoded in `PhysicsBridge_CreateBody`: cinfo+44 and cinfo+48 are
    // written 1.0 whatever the asset says, and `.phy` has no path to either.
    def.gravityScale = 1.0f;
    def.inertiaScale = 1.0f;
    // A rig that sleeps mid-collapse stays folded until something wakes it, and
    // nothing here wakes bodies — Snowball's public API has no wake at all, so a
    // sleeping body repositioned by a write-back would hang in mid-air.
    def.allowSleep = false;

    const sb::BodyId body = scene_.AddBody(def);
    scene_.SetTransform(body, frame.xf);

    if (BuildFixtures(app_->arBones[bone], body, frame.scale) == 0) {
        // The bridge destroys a body that got no fixture, and the 64-counter
        // never sees it. Snowball has no remove, so it is left inert and
        // unlinked instead — the same thing observable: no claim, no
        // write-back, and a static body with no shape collides with nothing.
        scene_.SetBodyType(body, sb::BodyType::Static);
        return -1;
    }

    Link link;
    link.bone = static_cast<i32>(bone);
    link.body = body;
    link.dynamic = type == sb::BodyType::Dynamic;
    link.kinematic = type == sb::BodyType::Kinematic;
    link.scale = frame.scale;
    link.seedPos = frame.xf.position;
    linkOfBone_[bone] = static_cast<i32>(links_.size());
    links_.push_back(link);
    return linkOfBone_[bone];
}

void D3PhysicsStage::BuildBoneBodies(const FrameState& fs) {
    const auto& bones = app_->arBones;
    for (std::size_t b = 0; b < bones.size(); ++b) {
        if (links_.size() >= kD3MaxBodies)
            break;
        // The builder tests the shape list's *size*, not how many of them match
        // the lod: a bone with shapes authored only for the other lod still
        // gets a body created, and then destroyed by the bridge for having no
        // fixtures. It costs nothing against the 64 — that counter increments
        // only when a body comes back — which is why the cap above is tested
        // against the surviving links rather than against this loop.
        if (bones[b].arCollisionShapes.empty())
            continue;
        if (b >= fs.boneWorldMatrices.size())
            continue;
        AddBoneBody(b, fs,
                    D3BoneIsDynamic(bones[b], lod_) ? sb::BodyType::Dynamic
                                                    : sb::BodyType::Static);
    }
}

void D3PhysicsStage::BuildAnchoredRig(const FrameState& fs) {
    const auto& bones = app_->arBones;
    // **Parents before children, in one forward pass**, which is what makes the
    // ancestor lookup below a table read rather than a search: a bone's
    // ancestor is bodied by an earlier iteration or not at all. D3 stores its
    // bones that way and `Skeleton_ComposeWorldPose` relies on it too.
    for (std::size_t b = 0; b < bones.size(); ++b) {
        if (links_.size() >= kD3MaxBodies)
            break;
        if (bones[b].arCollisionShapes.empty())
            continue;
        if (b >= fs.boneWorldMatrices.size())
            continue;

        // An anchor is bodied unconditionally. `sub_71003E07B0` creates it
        // Dynamic like everything else and then calls `dmBody_SetType(body, 1)`
        // on it — which is the whole of what `dwFlags` bit 0 means, and the
        // opposite of "this bone is part of the ragdoll".
        if (D3BoneIsAnchor(bones[b])) {
            AddBoneBody(b, fs, sb::BodyType::Kinematic);
            continue;
        }

        // Everything else has to reach a bodied ancestor. The walk skips bones
        // with no body of their own; the client memoises the misses with a −2
        // sentinel, which is an optimisation over the same answer.
        i32 ancestor = -1;
        for (i32 p = bones[b].nParentIndex;
             p >= 0 && static_cast<std::size_t>(p) < bones.size();
             p = bones[static_cast<std::size_t>(p)].nParentIndex) {
            if (linkOfBone_[static_cast<std::size_t>(p)] >= 0) {
                ancestor = linkOfBone_[static_cast<std::size_t>(p)];
                break;
            }
        }
        if (ancestor < 0)
            continue;
        // …and it has to carry a constraint, because the joint to that ancestor
        // is the only reason it is in the rig.
        if (bones[b].arConstraints.empty())
            continue;
        // Two ways in, and the flag is the override: `dwFlags` bit 4 bodies the
        // bone whatever its ancestor is, otherwise the ancestor has to be
        // Dynamic. So a chain grows down from a swinging part and stops at a
        // kinematic one unless a constraint says to keep going.
        const bool forced = (bones[b].arConstraints[0].dwFlags & 0x10) != 0;
        if (!forced && !links_[static_cast<std::size_t>(ancestor)].dynamic)
            continue;

        const i32 link = AddBoneBody(b, fs, sb::BodyType::Dynamic);
        if (link >= 0)
            links_[static_cast<std::size_t>(link)].ancestor = ancestor;
    }
}

void D3PhysicsStage::BuildJoints() {
    const auto& bones = app_->arBones;

    // ---- the per-bone list ---------------------------------------------
    //
    // **`arConstraints[0]` and nothing else.** Both builders pass the array's
    // data pointer straight to the joint builder without an index, so a bone
    // authoring more than one constraint has the rest ignored. Body A is the
    // *ancestor* and body B is this bone, which is the direction the whole
    // parameter block is authored in.
    for (const Link& l : links_) {
        if (l.ancestor < 0)
            continue;
        const auto b = static_cast<std::size_t>(l.bone);
        if (bones[b].arConstraints.empty())
            continue;
        sb::JointDef def;
        if (!MakeJointDef(bones[b].arConstraints[0],
                          links_[static_cast<std::size_t>(l.ancestor)].body, l.body, def))
            continue;
        scene_.AddJoint(def);
        ++joints_;
    }

    // ---- the appearance-level list --------------------------------------
    //
    // Extra joints between any two bones, **capped at 8** by the builder before
    // it iterates. Not a buffer bound that happens to bite: 90 appearances
    // carry this list and the largest authors exactly 8.
    const std::size_t extras = (std::min)(app_->arConstraints.size(), std::size_t{8});
    for (std::size_t k = 0; k < extras; ++k) {
        const auto& c = app_->arConstraints[k];
        if (c.nBoneIndexA < 0 || static_cast<std::size_t>(c.nBoneIndexA) >= linkOfBone_.size())
            continue;
        if (c.nBoneIndexB < 0 || static_cast<std::size_t>(c.nBoneIndexB) >= linkOfBone_.size())
            continue;
        const i32 ia = linkOfBone_[static_cast<std::size_t>(c.nBoneIndexA)];
        const i32 ib = linkOfBone_[static_cast<std::size_t>(c.nBoneIndexB)];
        if (ia < 0 || ib < 0)
            continue;
        sb::JointDef def;
        if (!MakeJointDef(c, links_[static_cast<std::size_t>(ia)].body,
                          links_[static_cast<std::size_t>(ib)].body, def))
            continue;
        scene_.AddJoint(def);
        ++joints_;
    }
}

void D3PhysicsStage::Build(const FrameState& fs) {
    const bool debug = std::getenv("WDX_PHYSICS_DEBUG") != nullptr;
    linkOfBone_.assign(app_->arBones.size(), -1);

    if (mode_ == D3RigMode::Ragdoll)
        BuildAnchoredRig(fs);
    else
        BuildBoneBodies(fs);
    BuildJoints();

    // The ground. D3 collides its rigs against the world through the `.phy`
    // mask; the viewer's world is the grid, so one static box whose top face is
    // the grid plane stands in for it — the same plane terrain IK plants feet
    // on, and the same substitution the other two stages make. Without it a
    // collapsing rig, whose bodies are *all* dynamic with nothing anchoring
    // them, falls forever.
    {
        const char* g = std::getenv("WDX_PHYSICS_GROUND");
        if (!(g != nullptr && g[0] == '0' && g[1] == '\0')) {
            using namespace renderer::physics;
            sb::BodyDef groundDef;
            groundDef.type = sb::BodyType::Static;
            const sb::BodyId ground = scene_.AddBody(groundDef);
            sb::Transform xf;
            xf.position = sb::Vec4{0.0f, 0.0f, kGroundZ - kGroundSlabThickness, 0.0f};
            const sb::ShapeId slab = shapes_.Add(sb::MakeBox(
                sb::Vec4{kGroundSlabHalfExtent, kGroundSlabHalfExtent, kGroundSlabThickness, 0.0f},
                xf));
            scene_.AddFixture(ground, slab, 0.0f, friction_, restitution_);
        }
    }

    if (debug) {
        std::size_t dyn = 0, kin = 0;
        for (const Link& l : links_) {
            dyn += l.dynamic ? 1u : 0u;
            kin += l.kinematic ? 1u : 0u;
        }
        std::fprintf(stderr,
                     "[phys] d3: mode=%s lod=%d bodies=%zu dyn=%zu kine=%zu joints=%d "
                     "density=%g friction=%g restitution=%g\n",
                     mode_ == D3RigMode::Ragdoll ? "ragdoll" : "bone-bodies", lod_, links_.size(),
                     dyn, kin, joints_, density_, friction_, restitution_);
    }
}

void D3PhysicsStage::DriveAnchors(const FrameState& fs, f32 invT) {
    // Only the anchors, and only in the anchored rig — everything else is either
    // simulated or immovable. See the header note: this is not D3's own drive,
    // which is an additive velocity push nothing in a model file can arm.
    for (Link& l : links_) {
        if (!l.kinematic)
            continue;
        const auto bone = static_cast<std::size_t>(l.bone);
        if (bone >= fs.boneWorldMatrices.size())
            continue;

        const BoneFrame frame = DecomposeBone(fs.boneWorldMatrices[bone]);
        const sb::Transform& cur = scene_.Get(l.body).transform;
        sb::Vec4 targetRot = frame.xf.rotation;

        // Hemisphere fix: q and −q are the same orientation, and interpolating
        // toward the far one takes the long way round — visibly, on a bone that
        // crosses the boundary mid-animation.
        const f32 dot = cur.rotation.x * targetRot.x + cur.rotation.y * targetRot.y +
                        cur.rotation.z * targetRot.z + cur.rotation.w * targetRot.w;
        if (dot < 0.0f)
            targetRot = sb::Vec4{-targetRot.x, -targetRot.y, -targetRot.z, -targetRot.w};

        sb::Vec4 dp = frame.xf.position - cur.position;
        sb::Vec4 dq{targetRot.x - cur.rotation.x, targetRot.y - cur.rotation.y,
                    targetRot.z - cur.rotation.z, targetRot.w - cur.rotation.w};
        // `w = 2 * dq (x) conj(q)` — the inverse of `dq/dt = 0.5 * w (x) q` with
        // world-frame w on the left. Routed through `QuatMultiply` rather than
        // expanded by hand: the expansion is easy to write with the cross term
        // negated, which is invisible at rest and turns every moving anchor's
        // spin the wrong way once an animation plays.
        auto angularOf = [](const sb::Vec4& delta, const sb::Vec4& q) {
            const sb::Vec4 w = sb::QuatMultiply(delta, sb::Vec4{-q.x, -q.y, -q.z, q.w});
            return sb::Vec4{2.0f * w.x, 2.0f * w.y, 2.0f * w.z, 0.0f};
        };

        // Past either cap the body is teleported by the **excess only** and the
        // rest is still handed over as velocity. Below both caps nothing is
        // teleported at all, so the anchor drags what hangs off it.
        const f32 linear = Length3(dp);
        const f32 angular = Length3(angularOf(dq, cur.rotation));
        const f32 overLinear = linear > kSnapLinear ? (linear - kSnapLinear) / linear : 0.0f;
        const f32 overAngular = angular > kSnapAngular ? (angular - kSnapAngular) / angular : 0.0f;
        const f32 t = (std::max)(overLinear, overAngular);

        sb::Transform driven = cur;
        if (t > 0.0f) {
            driven.position = cur.position + dp * t;
            driven.rotation = sb::QuatNormalize(sb::Vec4{cur.rotation.x + dq.x * t,
                                                        cur.rotation.y + dq.y * t,
                                                        cur.rotation.z + dq.z * t,
                                                        cur.rotation.w + dq.w * t});
            scene_.SetTransform(l.body, driven);
            dp = frame.xf.position - driven.position;
            dq = sb::Vec4{targetRot.x - driven.rotation.x, targetRot.y - driven.rotation.y,
                          targetRot.z - driven.rotation.z, targetRot.w - driven.rotation.w};
        }

        const sb::Vec4 linVel = dp * invT;
        const sb::Vec4 angVel = angularOf(dq, driven.rotation) * invT;
        scene_.SetLinearVelocity(l.body, Length3(linVel) < kDriveEpsilon ? sb::Vec4{} : linVel);
        scene_.SetAngularVelocity(l.body, Length3(angVel) < kDriveEpsilon ? sb::Vec4{} : angVel);
        l.scale = frame.scale;
    }
}

int D3PhysicsStage::BuildFixtures(const d3n::BoneStructure& bone, sb::BodyId body, f32 bodyScale) {
    int made = 0;
    for (const auto& s : bone.arCollisionShapes) {
        // `PhysicsBridge_CreateBody` walks the whole list and skips every shape
        // whose `nLodIndex` differs from the descriptor's — which is what makes
        // the lod argument pick a rig rather than a detail level.
        if (s.nLodIndex != lod_)
            continue;

        // **`bodyScale` multiplies the geometry, not just the size.** A sphere's
        // centre and a capsule's two endpoints go through it as well, which is
        // the trap StarCraft II's fixture path had: 676 shapes there simulated
        // up to 1.9 units from where they drew.
        sb::ShapeId shape = -1;
        switch (s.eShapeType) {
        case 0: {
            const f32 radius = (std::max)(bodyScale * s.flRadius, kMinRadius);
            shape = shapes_.Add(sb::Sphere{ToVec4(s.vPointA, bodyScale), radius});
            break;
        }

        case 1: {
            const f32 radius = (std::max)(bodyScale * s.flRadius, kMinRadius);
            shape = shapes_.Add(sb::Capsule{ToVec4(s.vPointA, bodyScale),
                                            ToVec4(s.vPointB, bodyScale), radius});
            break;
        }

        case 2: {
            // The cook carries its own points and the fixture carries the scale
            // — `PhysicsBridge_CreateFixture` hands Domino the polytope
            // pointer, an identity-ish transform and `bodyScale` as a separate
            // float, exactly as StarCraft II's `m_scaleOrRadius` does. Snowball
            // builds its polytopes from points, so the scale is spent here.
            //
            // The client rejects a cook whose volume is not finite or is below
            // 4.4143e-6; `D3ReadPolytope` applies the same floor, so a rejected
            // one arrives as an empty optional rather than a degenerate hull.
            if (appBytes_.empty())
                break;
            auto poly = io::D3ReadPolytope(s.arPolytopeData, appBytes_);
            if (!poly || poly->points.size() < 4)
                break;
            std::vector<sb::Vec4> pts;
            pts.reserve(poly->points.size());
            for (const Vector3f& v : poly->points)
                pts.push_back(ToVec4(v, bodyScale));
            // Rebuilt through Snowball's hull builder rather than adopting the
            // cook's own face and half-edge tables: the topology is ours to
            // choose, and a corrupt table would otherwise reach the narrowphase
            // intact.
            shape = shapes_.Add(sb::MakePolytope(pts));
            break;
        }

        default:
            // Unreached on shipped content — the corpus holds 0, 1 and 2 and
            // nothing else — so this is a bad file, not a fourth kind.
            break;
        }

        if (shape < 0)
            continue;
        // Density is per *shape*: `g_density(phy.nBodyClass) * shape.flScaleX`.
        // Friction and restitution are per *actor* and replicated into every
        // fixture — the appearance has no material of its own. `.phy`'s third
        // material float has no Snowball counterpart, and the polytope path
        // zeroes it anyway.
        scene_.AddFixture(body, shape, density_ * s.flScaleX, friction_, restitution_);
        ++made;
    }
    return made;
}

void D3PhysicsStage::Seed(const FrameState& fs) {
    for (Link& l : links_) {
        const auto bone = static_cast<std::size_t>(l.bone);
        if (bone >= fs.boneWorldMatrices.size())
            continue;
        const BoneFrame frame = DecomposeBone(fs.boneWorldMatrices[bone]);
        scene_.SetTransform(l.body, frame.xf);
        scene_.SetLinearVelocity(l.body, sb::Vec4{});
        scene_.SetAngularVelocity(l.body, sb::Vec4{});
        l.scale = frame.scale;
        l.seedPos = frame.xf.position;
    }
}

void D3PhysicsStage::RebuildClaims(const FrameState& fs) {
    // **Static bodies are claimed too**, unlike either other stage, and that is
    // the point rather than an oversight: the client stops the actor's
    // animation when it builds this rig, so a bone whose body cannot move has
    // to hold its spawn pose. Claiming it and writing the body's transform back
    // is exactly that, and it is also what stops half a collapsed rig sliding
    // off with an idle cycle.
    claims_.clear();
    claims_.reserve(links_.size());
    for (const Link& l : links_) {
        const auto bone = static_cast<std::size_t>(l.bone);
        if (bone < fs.boneWorldMatrices.size())
            claims_.push_back(BoneClaim{l.bone, fs.boneWorldMatrices[bone]});
    }
}

void D3PhysicsStage::WriteBack(FrameState& fs, const PoseStageContext& ctx) {
    written_.assign(fs.boneWorldMatrices.size(), 0);

    for (const Link& l : links_) {
        const auto bone = static_cast<std::size_t>(l.bone);
        if (bone >= fs.boneWorldMatrices.size())
            continue;
        fs.boneWorldMatrices[bone] = ToMatrix(scene_.Get(l.body).transform, l.scale);
        written_[bone] = 1;
    }

    // **A stage that writes a bone owns its descendants** (`pose_stage.h`), and
    // a D3 rig covers nowhere near every bone: the lod-1 character proxy is
    // seven shapes on a skeleton with dozens of bones, so hands, weapons and
    // attachment points all hang off simulated limbs with no body of their own.
    // The client gets this for free — `Skeleton_ComposeWorldPose` composes each
    // bone from its parent's *new* world transform and consults a per-bone
    // driver table for the ones physics owns — and this pipeline stores world
    // matrices, so the same effect is had by re-applying each unclaimed bone's
    // animated local transform on top of its new parent.
    if (ctx.nodeParents.size() != fs.boneWorldMatrices.size())
        return;
    for (std::size_t i = 0; i < fs.boneWorldMatrices.size(); ++i) {
        const i32 parent = ctx.nodeParents[i];
        if (written_[i] || parent < 0 || !written_[static_cast<std::size_t>(parent)])
            continue;
        const auto p = static_cast<std::size_t>(parent);
        const Matrix44f local = animated_[i] * Matrix44f::inverse(animated_[p]);
        fs.boneWorldMatrices[i] = local * fs.boneWorldMatrices[p];
        // Marked written so this bone's own children follow it in the same
        // pass. D3 bones are stored parents-first — `Skeleton_ComposeWorldPose`
        // relies on it too — which is what makes one forward pass enough.
        written_[i] = 1;
    }
}

void D3PhysicsStage::Teardown() {
    claims_.clear();
    accumulator_ = 0.0f;
    armed_ = false;
}

void D3PhysicsStage::Run(FrameState& fs, const PoseStageContext& ctx) {
    if (fs.boneWorldMatrices.empty())
        return;

    const bool arm = control_ && control_->simulating;
    if (!arm) {
        // Disarmed is the resting state, not a failure one: D3 spawns this rig
        // on an event and the actor animates normally until then. Dropping the
        // claims hands every bone straight back to the sampler.
        if (armed_)
            Teardown();
        return;
    }

    // **Built on the first armed frame rather than in the constructor**,
    // because a fixture's size is a function of the pose: the client takes
    // `actorScale * boneScale` at creation and multiplies every point and every
    // radius by it. A stage built before any pose exists would assume 1.0 and
    // size a scaled model's collision wrongly.
    if (!built_) {
        Build(fs);
        built_ = true;
        armed_ = true;
        Seed(fs);
        RebuildClaims(fs);
        return;
    }
    if (!armed_) {
        // Re-armed after a release. Re-seeded rather than resumed, because the
        // bones went back to the sampler in between and the rig has to collapse
        // from wherever the animation left them — which is what the client does
        // too, by destroying the whole physics object and building a new one.
        // The fixtures are not rebuilt: their sizes came from the first arm's
        // pose, and a viewer that rescales a model between collapses is not a
        // case any of this has to serve.
        armed_ = true;
        accumulator_ = 0.0f;
        Seed(fs);
        RebuildClaims(fs);
        return;
    }
    if (links_.empty())
        return;

    // Real time, not animation time: a paused actor's rig still settles.
    accumulator_ = (std::min)(accumulator_ + static_cast<f32>(ctx.frameDtMs) * 0.001f,
                              kMaxAccumulator);
    const i32 substeps = (std::min)(static_cast<i32>(accumulator_ / kSubStep), kMaxSubSteps);
    if (substeps > 0) {
        // One drive per frame against the whole span about to be stepped, not
        // per substep: the anchor has to arrive where the animation put it by
        // the end of the frame, not by the end of the first sixtieth of it.
        DriveAnchors(fs, 1.0f / (static_cast<f32>(substeps) * kSubStep));
        for (i32 k = 0; k < substeps; ++k) {
            scene_.Step(kSubStep, kVelocityIterations, kPositionIterations);
            accumulator_ -= kSubStep;
        }
    }

    // Every frame, including one that stepped nothing: the bones must keep
    // showing the simulated pose, or the model snaps back to its animation on
    // every frame the accumulator does not fill.
    animated_.assign(fs.boneWorldMatrices.begin(), fs.boneWorldMatrices.end());
    WriteBack(fs, ctx);
    RebuildClaims(fs);

    if (std::getenv("WDX_PHYSICS_DEBUG") != nullptr && ++debugFrame_ % 200 == 0) {
        f32 worst = 0.0f, drop = 0.0f;
        i32 worstBone = -1;
        for (const Link& l : links_) {
            if (!l.dynamic)
                continue;
            const sb::Vec4& p = scene_.Get(l.body).transform.position;
            const f32 d = Length3(p - l.seedPos);
            if (d > worst) {
                worst = d;
                worstBone = l.bone;
            }
            drop = (std::min)(drop, p.z - l.seedPos.z);
        }
        // Displacement from the **seed** pose, not from the animated one: a
        // claimed bone's entry in `boneWorldMatrices` is last frame's physics
        // result, so comparing against it measures physics against itself.
        std::fprintf(stderr,
                     "[phys] d3 f%d moved-from-seed max=%.4f (bone %d) lowest dz=%.4f "
                     "joints=%d contacts=%d\n",
                     debugFrame_, worst, worstBone, drop, joints_, scene_.ContactCount());
    }
}

} // namespace

std::unique_ptr<animation::IPoseStage>
CreateD3PhysicsStage(const d3n::Appearances& app, std::span<const ::whiteout::u8> appBytes,
                     const d3n::Physics* phy, std::shared_ptr<D3PhysicsControl> control,
                     D3RigMode mode, i32 lodIndex) {
    if (app.arBones.empty())
        return nullptr;
    // Each builder's own lod unless the caller names one: the bone-body builder
    // is called with 1 and the anchored one leaves the descriptor at 0.
    if (lodIndex < 0)
        lodIndex = (mode == D3RigMode::Ragdoll) ? 0 : kD3BoneBodyLod;
    if (mode == D3RigMode::Ragdoll) {
        // An anchored rig with no anchor is not a rig: every other body in it
        // exists only because it reaches one.
        if (!D3HasRagdollAnchor(app))
            return nullptr;
        return std::make_unique<D3PhysicsStage>(app, appBytes, phy, std::move(control), mode,
                                                lodIndex);
    }
    // No bone that becomes dynamic means nothing ever moves and nothing is ever
    // written back, and an inert stage still costs a virtual call and a claim
    // copy per actor per frame. At lod 1 that drops every model but the 570
    // that carry a character proxy; at lod 0 it drops 93 of the 2,460 that
    // carry colliders at all.
    if (!D3HasDynamicBody(app, lodIndex))
        return nullptr;
    return std::make_unique<D3PhysicsStage>(app, appBytes, phy, std::move(control), mode,
                                            lodIndex);
}

bool D3MakeJointDef(const d3n::ConstraintParameters& c, sb::BodyId bodyA, sb::BodyId bodyB,
                    sb::JointDef& def) {
    return MakeJointDef(c, bodyA, bodyB, def);
}

Matrix44f D3RoundTripBoneFrame(const Matrix44f& world) {
    const BoneFrame f = DecomposeBone(world);
    return ToMatrix(f.xf, f.scale);
}

} // namespace whiteout::flakes::renderer::profiles::diablo3
