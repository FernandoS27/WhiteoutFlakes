#include "renderer/profiles/sc2_heroes/sc2_physics.h"

#include "whiteout/models/m3/structures.h"

#include "snowball/joint.h"
#include "snowball/polytope.h"
#include "snowball/scene.h"
#include "snowball/shape.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

namespace whiteout::flakes::renderer::profiles::sc2_heroes {
namespace {

namespace sb = ::snowball;
namespace w3 = ::whiteout::m3;
using ::whiteout::flakes::renderer::animation::BoneClaim;
using ::whiteout::flakes::renderer::animation::IPoseStage;
using ::whiteout::flakes::renderer::animation::PoseStageContext;
using ::whiteout::flakes::renderer::model::FrameState;

/// `CModelPhysicsScene_EnsureWorld` builds its world with the gravity its static initialiser
/// holds, `(0, 0, -8.8)`, which the game then overrides per map from its own data.
/// **-8.8, not -9.81 and not WoW's -10**: three hosts of one engine, three numbers, and none of
/// them an approximation of the others.
constexpr sb::Vec4 kGravity{0.0f, 0.0f, -8.8f, 0.0f};

/// @name The step, from `CModelPhysicsWorld_Update` (`DOMINO_GLUE.md` §6.5)
///
/// A fixed 1/60 behind an accumulator, capped at 50 ms and at three substeps, with 8 velocity
/// and 2 position iterations. That is a flat contradiction of WoW's single clamped variable
/// step out of the same engine (§4.1) — the step policy belongs to the host, so each profile
/// reproduces its own rather than the two sharing one.
///
/// The 50 ms cap is also what makes this safe in a viewer that stalls: time beyond it is
/// dropped rather than banked, so a long frame settles slowly instead of exploding.
/// @{
constexpr f32 kSubStep = 1.0f / 60.0f;
constexpr i32 kMaxSubSteps = 3;
constexpr f32 kMaxAccumulator = 0.05f;
constexpr i32 kVelocityIterations = 8;
constexpr i32 kPositionIterations = 2;
/// @}

/// @name Kinematic snap caps, from `M3Physics_SyncKinematicBoneToBody`
///
/// An animated bone drives its body by **velocity**; only a bone that moved further than one of
/// these caps in a frame is teleported, and then only by the *excess*, with the remainder still
/// handed over as velocity. WoW's equivalent floors the snap at 0.8 of the whole error and caps
/// at a yard and 45 degrees (§5.2) — so a warped SC2 model drags its chains where a warped WoW
/// model mostly carries them.
///
/// The angular quantity compared is `2*sin(theta/2)`, not the angle.
/// @{
constexpr f32 kSnapLinear = 0.25f;
constexpr f32 kSnapAngular = 0.3926991f;  // pi/8
/// @}

/// @name Velocity caps on the kinematic -> dynamic transition
///
/// A body that goes limp keeps the velocity the *drive* last gave it, which is what makes a limb
/// flung by an animation carry on rather than starting from rest. It is also unbounded — the
/// drive can hand over an arbitrary speed when an animation teleports a bone — so
/// `M3Physics_SetBodyDynamic` clamps both, comparing against the squared caps and rescaling
/// through `rsqrt` plus one Newton step (the `dmRcp` contract) rather than dividing.
/// @{
constexpr f32 kMaxTransitionLinear = 8.0f;
constexpr f32 kMaxTransitionAngular = 4.0f;
/// @}

/// Below this a driven velocity is written as exactly zero rather than left as a crawl; the
/// binary compares the squared length against 8.1e-11.
constexpr f32 kDriveEpsilon = 9.0e-6f;

/// A bone matrix row this short is treated as degenerate rather than normalised.
constexpr f32 kTinyScale = 1.0e-8f;

/// A capsule whose scaled **full** length falls to this is emitted as a *sphere* of the same
/// radius instead (§6.2). Worth reproducing rather than tidying: the two shapes take different
/// code paths, and G4 established that the collision margin belongs to the path rather than to
/// the shape — so a collapsed capsule and a sphere of that radius settle at different heights.
constexpr f32 kCapsuleCollapse = 0.005f;

sb::Vec4 ToVec4(const Vector3f& v) {
    return sb::Vec4{v.x, v.y, v.z, 0.0f};
}

f32 Length3(const sb::Vec4& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

Vector3f OriginOf(const Matrix44f& m) {
    return {m.data[3][0], m.data[3][1], m.data[3][2]};
}

/// @brief A bone's model-space frame, with the scale taken back out of it.
///
/// **An M3 bone matrix is a real world transform, and it carries scale.** Both halves separate
/// this from the WoW stage. `M2ModelAdapter::Evaluate` hands physics a skinning *palette* whose
/// unanimated entries are identity, so the WoW stage has to rebuild each frame from the bone's
/// pivot; `M3ModelAdapter::Evaluate` composes `local * parent` and stores exactly the bone's
/// world matrix, so the frame is read straight off it.
///
/// The scale then has to be divided out before the quaternion is extracted, which is what
/// `M3Physics_CreateRigidBody` does with three `rsqrt` row norms. Feeding the extraction
/// unnormalised rows gives a rotation wrong in proportion to the non-uniformity: invisible on a
/// uniformly scaled model, a visible skew on anything else.
struct BoneFrame {
    sb::Transform xf;
    Vector3f scale{1.0f, 1.0f, 1.0f};
};

BoneFrame DecomposeBone(const Matrix44f& m) {
    BoneFrame out;
    f32 n[3];
    for (int i = 0; i < 3; ++i) {
        n[i] = std::sqrt(m.data[i][0] * m.data[i][0] + m.data[i][1] * m.data[i][1] +
                         m.data[i][2] * m.data[i][2]);
    }
    out.scale = {n[0], n[1], n[2]};
    out.xf.position = sb::Vec4{m.data[3][0], m.data[3][1], m.data[3][2], 0.0f};

    if (n[0] <= kTinyScale || n[1] <= kTinyScale || n[2] <= kTinyScale) {
        // A collapsed basis is not a rotation, and the identity is the only answer here that
        // cannot spread NaN through the solver.
        out.xf.rotation = sb::Vec4{0.0f, 0.0f, 0.0f, 1.0f};
        return out;
    }

    // Bone matrices are **row-vector**: row `i` is the image of basis vector `i`, so the
    // column-major basis the extraction wants is the transpose of the top-left 3x3. Reading it
    // the other way round yields the conjugate, which looks correct for any symmetric pose and
    // mirrors every other one.
    f32 r[3][3];
    for (int i = 0; i < 3; ++i) {
        const f32 inv = 1.0f / n[i];
        for (int j = 0; j < 3; ++j) {
            r[j][i] = m.data[i][j] * inv;
        }
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

/// @brief The exact inverse of @ref DecomposeBone: a simulated pose wearing the animated scale.
///
/// `M3Physics_PostStepSyncToBones` rebuilds a driven bone's basis as the body's quaternion times
/// the scale the *animation* produced, and takes only the translation from the body. The solver
/// never sees scale in either direction, and a scaled model's driven bones stay scaled.
Matrix44f ToMatrix(const sb::Transform& xf, const Vector3f& scale) {
    const sb::Mtx b = sb::RotationMatrix(xf.rotation);
    Matrix44f m = Matrix44f::identity();
    // Row `j` of `data` is column `j` of `Mtx`, componentwise — the two are *not* related by a
    // transpose. Work it out from @ref DecomposeBone, which this has to invert exactly; writing
    // one in produces the conjugate rotation, which is stable, plausible and mirrored.
    m.data[0][0] = b.c[0].x * scale.x;
    m.data[0][1] = b.c[0].y * scale.x;
    m.data[0][2] = b.c[0].z * scale.x;
    m.data[1][0] = b.c[1].x * scale.y;
    m.data[1][1] = b.c[1].y * scale.y;
    m.data[1][2] = b.c[1].z * scale.y;
    m.data[2][0] = b.c[2].x * scale.z;
    m.data[2][1] = b.c[2].y * scale.z;
    m.data[2][2] = b.c[2].z * scale.z;
    m.data[3][0] = xf.position.x;
    m.data[3][1] = xf.position.y;
    m.data[3][2] = xf.position.z;
    return m;
}

/// @brief The rotation-and-translation part of a `PHSH`/`PHYJ` matrix, scale divided out.
sb::Transform ShapeFrame(const Matrix44f& m) {
    const BoneFrame f = DecomposeBone(m);
    return f.xf;
}

// ---------------------------------------------------------------------------

class Sc2PhysicsStage final : public IPoseStage {
public:
    /// @p model must outlive the stage. It does in the one place stages are made:
    /// `AnimationDriver::Bind` holds the source `shared_ptr` and the stage list together and
    /// replaces them in the same call. Unlike the WoW stage, this one cannot copy what it needs
    /// up front and forget the model — the scene is not built until a pose exists (see
    /// @ref Run).
    explicit Sc2PhysicsStage(const w3::Model& model) : model_(&model), scene_(shapes_, kGravity) {}

    /// @brief Whether this model can *ever* produce a simulated bone.
    ///
    /// Answered before the scene exists, because the scene is not built until the first frame —
    /// see @ref Run. "Ever" rather than "now": a body's type is decided per frame, so the only
    /// question a model file can settle on its own is whether the answer could ever be yes.
    bool WouldSimulate() const;

    void Run(FrameState& fs, const PoseStageContext& ctx) override;

    std::vector<BoneClaim> Claims() const override {
        return claims_;
    }

    /// Everything a `PHRB` rig simulates comes out of the model file, so it needs neither the
    /// ground query nor the aim target and does not belong behind the solver toggle.
    bool NeedsHostInputs() const override {
        return false;
    }

private:
    struct Link {
        i32 bone = -1;
        sb::BodyId body = -1;
        /// The body's type **right now**, which is not a property of the file — see
        /// @ref UpdateDrivenState.
        bool dynamic = false;
        /// `simulationType == 2`. The one type that never switches.
        bool isStatic = false;
        /// `kFlagInheritDynamic`: take the answer from @ref ancestor instead of the channel.
        bool inherit = false;
        /// Index into `model_->rigidBodies`, which is what `FrameState::physicsBodyDynamic` is
        /// keyed by. Not the same as this link's own index: a body on an out-of-range or
        /// already-claimed bone gets no link.
        i32 rbIndex = -1;
        /// The nearest ancestor bone's link, or -1. A pure function of the bone tree, so it is
        /// resolved once at build rather than walked every frame.
        i32 ancestor = -1;
        /// The animated scale this bone last carried, replayed into the write-back so a scaled
        /// model's driven bones keep their scale.
        Vector3f scale{1.0f, 1.0f, 1.0f};
        sb::Vec4 seedPos{};
    };

    void Build(FrameState& fs);
    void BuildFixtures(const w3::RigidBody& rb, sb::BodyId body, f32 scale);
    void BuildJoints(const FrameState& fs);
    void Seed(const FrameState& fs);
    void UpdateDrivenState(const FrameState& fs);
    void EnterDynamic(Link& l, const FrameState& fs);
    void EnterKinematic(Link& l);
    void DriveKinematic(const FrameState& fs, f32 invT);
    void WriteBack(FrameState& fs, const PoseStageContext& ctx);
    void RebuildClaims(const FrameState& fs);

    const w3::Model* model_ = nullptr;
    sb::ShapeStore shapes_;
    sb::Scene scene_;
    std::vector<Link> links_;
    /// Bone -> index into `links_`, or -1. `PHYJ` names bones, not bodies.
    std::vector<i32> linkOfBone_;
    /// The debug collision shapes' bones and frames, in `Sc2BuildCollisionShapes` order. Kept
    /// so the overlay can be redrawn from the *simulated* pose rather than the animated one the
    /// source saw.
    std::vector<i32> shapeBones_;
    std::vector<Matrix44f> shapeLocals_;
    std::vector<BoneClaim> claims_;
    /// The pose as the sampler produced it, snapshotted before the write-back overwrites the
    /// driven bones — the write-back reads it to recover each unclaimed bone's animated local
    /// transform. Members rather than locals so a per-actor stage allocates nothing per frame.
    std::vector<Matrix44f> animated_;
    std::vector<u8> written_;
    bool built_ = false;
    f32 accumulator_ = 0.0f;
    int debugFrame_ = 0;
};

/// @brief `PhysicsJoint::enableLimits`, read the way the client reads it.
///
/// **The low byte, not the word.** `M3Physics_CreateJoint` loads a single byte from `PHYJ+140`
/// and the parser gives us a `u32` spanning +140..+143 — and those upper bytes are not padding:
/// every joint in the corpus reads `01 7f 00 00` there, so the field is an enable byte with a
/// second, unidentified byte behind it. Testing the whole word happens to agree on that value
/// and would silently enable the limit on any joint authoring `00 7f 00 00`.
bool LimitsEnabled(const w3::PhysicsJoint& pj) {
    return (pj.enableLimits & 0xFFu) != 0u;
}

/// The `PHRB` flag that says "**take my dynamic state from my nearest physicalised ancestor**"
/// rather than from my own channel. 29% of the corpus sets it, and ignoring it is not a subtle
/// error: a body that inherits a kinematic ancestor's state but is read from its own authored
/// default becomes a dynamic body attached to no joint, which free-falls away from the model
/// while every joint in the rig holds perfectly.
constexpr ::whiteout::u32 kFlagInheritDynamic = 0x40u;

/// @brief Whether this body's `dynamicState` is *keyed* rather than constant.
///
/// **The gate is the AnimRef's flag bit 1, not its `animId`.** Every shipped `dynamicState`
/// carries a non-zero `animId` — the sentinel WhiteoutLib documents for "unbound" does not
/// appear on this channel — yet only about a tenth of them ask to be sampled, and reading the
/// id as the gate makes the other nine tenths chase a track that has no keys for them.
bool DynamicStateSampled(const w3::RigidBody& rb) {
    return (rb.dynamicState.flags & 0x2u) != 0u;
}

/// @brief The `dynamicState` a body holds when nothing is sampling it.
///
/// `initValue`, not `nullValue`. `M3Physics_UpdateBodyDrivenState` loads `PHRB+40` into its
/// scratch and only lets the sampler overwrite it, so the init value is the *base* rather than
/// a fallback for the unbound case.
bool AuthoredDynamic(const w3::RigidBody& rb) {
    return rb.dynamicState.initValue != 0;
}

/// @brief Whether a body could ever be dynamic, however the frame goes.
///
/// Used for two decisions that have to be made before any frame exists: whether the stage is
/// worth creating at all, and whether a body gets fixtures. Both would be wrong if they read
/// only the authored value — a body that is kinematic at t=0 and keyed dynamic at t=1s is
/// exactly the content this stage is for.
bool CouldBeDynamic(const w3::RigidBody& rb) {
    if (rb.getVersion() >= 3 && rb.simulationType == 2) {
        return false; // static, and static never switches
    }
    return AuthoredDynamic(rb) || DynamicStateSampled(rb) ||
           (static_cast<::whiteout::u32>(rb.flags) & kFlagInheritDynamic) != 0u;
}

/// @brief For each body, the body whose bone is its nearest physicalised ancestor, or -1.
///
/// @p indexOfBone maps a bone to the entry riding it (-1 for none) and @p boneOfIndex is its
/// inverse. Both the stage and the debug-shape builder need this walk over different index
/// spaces — links and raw `PHRB` — which is the whole reason it takes them as spans.
///
/// Bounded by the bone count, so a malformed parent link that cycles costs a skipped inherit
/// rather than a hang.
std::vector<i32> ResolveAncestors(const w3::Model& model, std::span<const i32> indexOfBone,
                                  std::span<const i32> boneOfIndex) {
    const std::size_t boneCount = model.bones.size();
    std::vector<i32> out(boneOfIndex.size(), -1);
    for (std::size_t i = 0; i < boneOfIndex.size(); ++i) {
        if (boneOfIndex[i] < 0 || static_cast<std::size_t>(boneOfIndex[i]) >= boneCount) {
            continue;
        }
        auto bone = static_cast<std::size_t>(boneOfIndex[i]);
        for (std::size_t hop = 0; hop <= boneCount; ++hop) {
            const ::whiteout::u16 p = model.bones[bone].parentIndex;
            if (p == 0xFFFFu || p >= boneCount) {
                break;
            }
            if (indexOfBone[p] >= 0) {
                out[i] = indexOfBone[p];
                break;
            }
            bone = p;
        }
    }
    return out;
}

/// @brief Each body's type as *authored*, for the debug overlay's colour.
///
/// The steady state the runtime pass converges on with no animation playing, which is what a
/// per-template shape list can say: the real type is per-actor and per-frame (see
/// @ref Sc2PhysicsStage::UpdateDrivenState). Resolved in body order reading whatever the
/// ancestor already has, exactly as the runtime does — bodies are authored parents-first, so
/// one pass is enough for shipped content and a file that is not costs an overlay colour.
std::vector<u8> AuthoredDynamicPerBody(const w3::Model& model) {
    const std::size_t n = model.rigidBodies.size();
    std::vector<i32> indexOfBone(model.bones.size(), -1);
    std::vector<i32> boneOfIndex(n, -1);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t bone = model.rigidBodies[i].parentBoneIndex;
        if (bone >= model.bones.size()) {
            continue;
        }
        boneOfIndex[i] = static_cast<i32>(bone);
        if (indexOfBone[bone] < 0) {
            indexOfBone[bone] = static_cast<i32>(i);
        }
    }
    const std::vector<i32> ancestor = ResolveAncestors(model, indexOfBone, boneOfIndex);

    std::vector<u8> out(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        const w3::RigidBody& rb = model.rigidBodies[i];
        if (rb.getVersion() >= 3 && rb.simulationType == 2) {
            continue;
        }
        if ((static_cast<::whiteout::u32>(rb.flags) & kFlagInheritDynamic) != 0u) {
            out[i] = ancestor[i] >= 0 ? out[static_cast<std::size_t>(ancestor[i])] : 0;
        } else {
            out[i] = AuthoredDynamic(rb) ? 1 : 0;
        }
    }
    return out;
}

bool Sc2PhysicsStage::WouldSimulate() const {
    for (const w3::RigidBody& rb : model_->rigidBodies) {
        if (rb.parentBoneIndex >= model_->bones.size() || rb.rigidBodyShape.empty()) {
            continue;
        }
        if (CouldBeDynamic(rb)) {
            return true;
        }
    }
    return false;
}

/// @brief The per-frame body-type pass, `M3Physics_UpdateBodyDrivenState`.
///
/// **`simulationType` says how a body is *created*; it does not say what it is.** SC2 creates
/// the body with that type verbatim (§6.1) and then flips it between kinematic and dynamic
/// every frame through `dmBody_SetType` — which is why 23,601 of the corpus's 24,000 bodies
/// ship as `simulationType == 1` and a `*DeathRagdoll` still collapses. Type 2 never switches.
///
/// Two sources feed the decision:
///
///  - `kFlagInheritDynamic` — copy the nearest physicalised ancestor's **current type**. Not a
///    recursive read of what the ancestor was authored as: the client walks its body table in
///    order and asks `dmBody_GetType`, so an ancestor already visited this frame answers with
///    its new state and one visited later answers with last frame's. Reproduced by walking
///    `links_` in the same order, which is `PHRB` order.
///  - otherwise the `dynamicState` channel, sampled by the source into
///    `FrameState::physicsBodyDynamic` because only the source has the layer stack.
///
/// The client has a third input this cannot have: a model-level "ragdoll now" switch that
/// forces every body dynamic except those flagged `0x100`. Nothing in a model file sets it —
/// it is a gameplay event — so a viewer only ever sees the channel.
void Sc2PhysicsStage::UpdateDrivenState(const FrameState& fs) {
    const std::span<const u8> sampled = fs.physicsBodyDynamic;
    for (Link& l : links_) {
        if (l.isStatic) {
            continue;
        }
        bool want;
        if (l.inherit) {
            // No ancestor to inherit from means kinematic, which leaves the bone on its
            // animation. The alternative — defaulting to dynamic — is what makes a live hero's
            // collision proxies fall off it, jointed to nothing.
            want = l.ancestor >= 0 && links_[static_cast<std::size_t>(l.ancestor)].dynamic;
        } else {
            const auto rb = static_cast<std::size_t>(l.rbIndex);
            // A host that does not fill the channel gets the authored constant, which is what
            // every body whose channel is unkeyed resolves to anyway.
            want = rb < sampled.size()
                       ? sampled[rb] != 0
                       : rb < model_->rigidBodies.size() && AuthoredDynamic(model_->rigidBodies[rb]);
        }
        if (want == l.dynamic) {
            continue;
        }
        if (want) {
            EnterDynamic(l, fs);
        } else {
            EnterKinematic(l);
        }
    }
}

void Sc2PhysicsStage::EnterDynamic(Link& l, const FrameState& fs) {
    scene_.SetBodyType(l.body, sb::BodyType::Dynamic);
    l.dynamic = true;

    const auto bone = static_cast<std::size_t>(l.bone);
    if (bone < fs.boneWorldMatrices.size()) {
        // Placed at the bone the animation just posed, not left where the drive had pushed it
        // to. The two differ by exactly the drive's tracking error, and starting a ragdoll from
        // the lagging one is a visible pop at the moment a character goes limp.
        scene_.SetTransform(l.body, DecomposeBone(fs.boneWorldMatrices[bone]).xf);
    }

    // The drive's last velocity carries over — that is what throws a limb that was already
    // moving — but it is unbounded, so both halves are capped rather than passed through.
    auto capped = [](sb::Vec4 v, f32 cap) {
        const f32 len = Length3(v);
        return len > cap ? v * (cap / len) : v;
    };
    scene_.SetLinearVelocity(l.body,
                             capped(scene_.Get(l.body).linearVelocity, kMaxTransitionLinear));
    scene_.SetAngularVelocity(l.body,
                              capped(scene_.Get(l.body).angularVelocity, kMaxTransitionAngular));
}

void Sc2PhysicsStage::EnterKinematic(Link& l) {
    // No re-placement and no velocity reset on the way back: the next `DriveKinematic` reads
    // the body where the simulation left it and drives it home from there, which is what turns
    // a settled ragdoll back into an animated model without a jump.
    scene_.SetBodyType(l.body, sb::BodyType::Kinematic);
    l.dynamic = false;
}

void Sc2PhysicsStage::Build(FrameState& fs) {
    const w3::Model& model = *model_;
    const std::size_t boneCount = model.bones.size();
    const bool debug = std::getenv("WDX_PHYSICS_DEBUG") != nullptr;
    int fixtureCount = 0, skippedShapes = 0;

    linkOfBone_.assign(boneCount, -1);

    for (std::size_t rbIndex = 0; rbIndex < model.rigidBodies.size(); ++rbIndex) {
        const w3::RigidBody& rb = model.rigidBodies[rbIndex];
        const std::size_t bone = rb.parentBoneIndex;
        if (bone >= boneCount || bone >= fs.boneWorldMatrices.size()) {
            continue;
        }
        // Two bodies on one bone would fight over the same write-back slot, and the file has no
        // rule for which wins. First one keeps it.
        if (linkOfBone_[bone] >= 0) {
            continue;
        }

        const bool legacy = rb.getVersion() < 3;
        const bool isStatic = !legacy && rb.simulationType == 2;

        const BoneFrame frame = DecomposeBone(fs.boneWorldMatrices[bone]);

        sb::BodyDef def;
        // **Created kinematic, then flipped.** The client creates from `simulationType`
        // verbatim and lets its driven-state pass converge the type in the same tick, so the two
        // agree on the end state whatever the file says — and this way one place decides what a
        // body's type means rather than two that have to stay in step.
        def.type = isStatic ? sb::BodyType::Static : sb::BodyType::Kinematic;
        def.linearDamping = rb.linearDamping;
        def.angularDamping = rb.angularDamping;
        // **`m_gravityScale` is hardcoded to 1.0 and M3 has no path to it**; the field
        // WhiteoutLib calls `gravityScale` (`PHRB+28`) is what SC2 feeds to `m_inertiaScale`
        // (§7.2). Reading it as gravity would make heavy-inertia bodies fall slowly.
        def.gravityScale = 1.0f;
        def.inertiaScale = rb.gravityScale;
        // A chain that sleeps mid-swing stays folded until something wakes it, and nothing here
        // wakes bodies on an animation change yet.
        def.allowSleep = false;

        const sb::BodyId body = scene_.AddBody(def);
        const auto flags = static_cast<::whiteout::u32>(rb.flags);
        linkOfBone_[bone] = static_cast<i32>(links_.size());
        Link link;
        link.bone = static_cast<i32>(bone);
        link.body = body;
        link.dynamic = false;
        link.isStatic = isStatic;
        link.inherit = (flags & kFlagInheritDynamic) != 0u;
        link.rbIndex = static_cast<i32>(rbIndex);
        link.scale = frame.scale;
        links_.push_back(link);

        // **The smallest of the three scale components, splatted.** SC2 collapses the bone's
        // per-axis world scale to one float and hands that to every fixture, so a
        // non-uniformly scaled physics bone is simulated at its *thinnest* axis and
        // non-uniform scale is fundamentally unsupported by the format's own runtime.
        const f32 uniform = (std::min)({frame.scale.x, frame.scale.y, frame.scale.z});
        // **A body that collides with nothing gets no fixtures.** SC2 says so through the
        // filter rather than through the shape list: a kinematic or static body with neither
        // the collidable (0x1) nor the walkable (0x2) flag is given an include mask of *zero*,
        // and a zero mask never matches (§6.3). Dynamic bodies are never dropped this way —
        // they carry the rig's mass, and a massless dynamic body still falls while no joint can
        // hold it, which reads as a broken solver and is not one.
        //
        // Asked of what the body *could* become, not what it is: fixtures are built once, and a
        // body that gets none because it is kinematic now has no mass to find when the channel
        // turns it dynamic later.
        const bool inert = !CouldBeDynamic(rb) && (flags & 0x3u) == 0u;
        if (!inert) {
            BuildFixtures(rb, body, uniform);
        }
        fixtureCount += static_cast<int>(scene_.Get(body).fixtures.size());
        skippedShapes += static_cast<int>(rb.rigidBodyShape.size() -
                                          scene_.Get(body).fixtures.size());

        if (debug) {
            std::fprintf(stderr,
                         "[phys] bone %zu %s shapes=%zu scale=%.3f density=%g keyed=%d\n", bone,
                         isStatic ? "static" : "kine", rb.rigidBodyShape.size(), uniform,
                         rb.density, static_cast<int>(DynamicStateSampled(rb)));
        }
    }

    // Resolved once because it is a pure function of the bone tree, unlike the answer it feeds.
    {
        std::vector<i32> boneOfLink;
        boneOfLink.reserve(links_.size());
        for (const Link& l : links_) {
            boneOfLink.push_back(l.bone);
        }
        const std::vector<i32> ancestors = ResolveAncestors(model, linkOfBone_, boneOfLink);
        for (std::size_t i = 0; i < links_.size(); ++i) {
            links_[i].ancestor = ancestors[i];
        }
    }

    BuildJoints(fs);

    {
        Sc2CollisionShapes debug = Sc2BuildCollisionShapes(model);
        shapeBones_ = std::move(debug.bones);
        shapeLocals_ = std::move(debug.locals);
    }

    // The ground. StarCraft II collides its ragdolls against the terrain collider; the viewer's
    // world is a flat plane at z=0, so one static box stands in for it, exactly as the WoW
    // stage does. Without it a `*DeathRagdoll` — whose bodies are *all* dynamic, with nothing
    // kinematic anchoring them — falls forever.
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
            scene_.AddFixture(ground, slab, 0.0f, 0.5f, 0.0f);
        }
    }

    // Seeded before the first driven pass, because a body that goes dynamic reads the bone it
    // is placed on — and because the pass is what decides which bodies those are.
    Seed(fs);
    UpdateDrivenState(fs);
    RebuildClaims(fs);
    Sc2PlaceCollisionShapes(shapeBones_, shapeLocals_, fs.boneWorldMatrices,
                            fs.collisionTransforms);

    if (debug) {
        std::fprintf(stderr,
                     "[phys] sc2: bodies=%zu dyn=%zu fixtures=%d joints=%d/%zu skipped=%d\n",
                     links_.size(), claims_.size(), fixtureCount, scene_.JointCount(),
                     model.physicsJoints.size(), skippedShapes);
    }
}

void Sc2PhysicsStage::RebuildClaims(const FrameState& fs) {
    // Rebuilt every frame rather than fixed at build: a bone's claim is the "physics owns this
    // one" bit (`+150` bit 0), which is set when a body goes dynamic and cleared when it goes
    // back. A stale claim keeps the sampler skipping a bone nothing is driving any more.
    claims_.clear();
    for (const Link& l : links_) {
        const auto bone = static_cast<std::size_t>(l.bone);
        if (l.dynamic && bone < fs.boneWorldMatrices.size()) {
            claims_.push_back(BoneClaim{l.bone, fs.boneWorldMatrices[bone]});
        }
    }
}

void Sc2PhysicsStage::BuildFixtures(const w3::RigidBody& rb, sb::BodyId body, f32 scale) {
    for (const w3::PhysicsShape& ps : rb.rigidBodyShape) {
        // The shape matrix decomposed exactly as `M3Physics_CreateShapeFixture` does it: an
        // orthonormal rotation, a translation, and three row lengths that scale the dimensions.
        // **Every shape kind collapses those three differently** — the box keeps all three, the
        // sphere takes the largest, the capsule takes row 0 for its radius and row 2 for its
        // length — which is measured behaviour and not a simplification worth tidying.
        const BoneFrame shapeFrame = DecomposeBone(ps.transform);
        const sb::Transform local = shapeFrame.xf;
        const Vector3f row = shapeFrame.scale;
        const Vector3f dims = ps.shapeDimensions;
        const sb::Mtx basis = sb::RotationMatrix(local.rotation);
        sb::ShapeId shape = -1;

        switch (ps.shapeType) {
        case w3::PhysicsShapeType::Box:
            // A box is a **polytope** to Domino, not a shape kind of its own — the same
            // conversion `.phys` boxes take (§3.2 against §6.2). Half extents come from the
            // three dimensions scaled by their own matrix row.
            shape = shapes_.Add(sb::MakeBox(
                sb::Vec4{dims.x * row.x, dims.y * row.y, dims.z * row.z, 0.0f}, local, scale));
            break;

        case w3::PhysicsShapeType::Sphere:
            shape = shapes_.Add(sb::Sphere{
                local.position, dims.x * (std::max)({row.x, row.y, row.z}) * scale});
            break;

        case w3::PhysicsShapeType::Capsule: {
            // **`shapeDimensions.y` is the full distance between the two cap centres**, not the
            // capsule's overall length and not a half-length: the endpoints are the midpoint
            // plus and minus half of it, along the frame's own +Z (its normalised row 2).
            const f32 radius = dims.x * row.x * scale;
            const f32 height = dims.y * row.z * scale;
            if (height <= kCapsuleCollapse) {
                shape = shapes_.Add(sb::Sphere{local.position, radius});
                break;
            }
            const sb::Vec4 axis =
                basis.Transform(sb::Vec4{0.0f, 0.0f, height * 0.5f, 0.0f});
            shape =
                shapes_.Add(sb::Capsule{local.position + axis, local.position - axis, radius});
            break;
        }

        case w3::PhysicsShapeType::Cylinder: {
            // Hull-converted, because Domino has no cylinder primitive: SC2 bakes a prism
            // offline and ships it beside the shape. A prism rather than a capsule stand-in,
            // since a capsule's round caps are exactly what a cylinder is not.
            //
            // Eight sides is our choice — the shipped prism's vertex count is a property of the
            // baked table, which is not in the file.
            const f32 radius = dims.x * row.x * scale;
            const f32 half = dims.y * row.z * scale * 0.5f;
            std::vector<sb::Vec4> pts;
            pts.reserve(16);
            for (int k = 0; k < 8; ++k) {
                const f32 a = 6.2831853f * static_cast<f32>(k) / 8.0f;
                const f32 x = radius * std::cos(a), y = radius * std::sin(a);
                pts.push_back(basis.Transform(sb::Vec4{x, y, half, 0.0f}) + local.position);
                pts.push_back(basis.Transform(sb::Vec4{x, y, -half, 0.0f}) + local.position);
            }
            shape = shapes_.Add(sb::MakePolytope(pts));
            break;
        }

        case w3::PhysicsShapeType::ConvexHull: {
            // **The hull's transform is already baked into its vertices.** SC2's offline cook
            // transforms the point cloud and then writes the shape matrix back as identity, so
            // applying it here is a no-op on shipped data and the right thing on anything else.
            std::vector<sb::Vec4> pts;
            pts.reserve(ps.hullVertexPositions.size());
            for (const Vector4f& v : ps.hullVertexPositions) {
                pts.push_back(basis.Transform(sb::Vec4{v.x, v.y, v.z, 0.0f}) + local.position);
            }
            if (pts.size() >= 4) {
                // Rebuilt through our own hull builder rather than adopting the file's face and
                // half-edge tables the way the client does: the topology is ours to choose, and
                // a corrupt `DMSE` table would otherwise reach the narrowphase intact.
                shape = shapes_.Add(
                    sb::MakePolytope(pts, (std::max)({row.x, row.y, row.z}) * scale));
            }
            break;
        }

        case w3::PhysicsShapeType::Mesh:
            // `dmTreeMesh`, and the only path in either host that produces one. Snowball has
            // the shape (`tree_mesh.h`, phase 2h) but not the `MT16`/`MT32` decode that feeds
            // it — and the client rebuilds the tree from those triangles at load anyway,
            // reading none of the shipped `DMMN` nodes. So a mesh shape contributes no fixture
            // rather than a wrong one: they are 2.3% of the corpus and belong to static
            // scenery, not to the jointed rigs this stage exists for.
            break;
        }

        if (shape >= 0) {
            // Density is per *body* and replicated into every fixture, as are friction and
            // restitution — the file has no per-shape material (§7.2). Rolling resistance
            // (0.05 on spheres and capsules, 0 elsewhere) has no Snowball counterpart.
            scene_.AddFixture(body, shape, rb.density, rb.friction, rb.restitution);
        }
    }
}

void Sc2PhysicsStage::BuildJoints(const FrameState& fs) {
    const w3::Model& model = *model_;
    const bool debug = std::getenv("WDX_PHYSICS_DEBUG") != nullptr;

    for (const w3::PhysicsJoint& pj : model.physicsJoints) {
        if (pj.boneIndex1 >= linkOfBone_.size() || pj.boneIndex2 >= linkOfBone_.size()) {
            continue;
        }
        const i32 ia = linkOfBone_[pj.boneIndex1];
        const i32 ib = linkOfBone_[pj.boneIndex2];
        if (ia < 0 || ib < 0) {
            continue;
        }

        // **The anchors inherit the bone's per-axis world scale; the frames do not.** SC2
        // multiplies each matrix's translation componentwise by the three row norms of its
        // bone's node transform and leaves the rotation alone.
        const BoneFrame fa = DecomposeBone(fs.boneWorldMatrices[pj.boneIndex1]);
        const BoneFrame fb = DecomposeBone(fs.boneWorldMatrices[pj.boneIndex2]);
        const sb::Transform frameA = ShapeFrame(pj.matrixBody1);
        const sb::Transform frameB = ShapeFrame(pj.matrixBody2);
        const Vector3f originA = OriginOf(pj.matrixBody1);
        const Vector3f originB = OriginOf(pj.matrixBody2);
        const sb::Vec4 anchorA{originA.x * fa.scale.x, originA.y * fa.scale.y,
                               originA.z * fa.scale.z, 0.0f};
        const sb::Vec4 anchorB{originB.x * fb.scale.x, originB.y * fb.scale.y,
                               originB.z * fb.scale.z, 0.0f};

        sb::JointDef def;
        def.bodyA = links_[static_cast<std::size_t>(ia)].body;
        def.bodyB = links_[static_cast<std::size_t>(ib)].body;
        // `PhysicsJoint::enableShape` lands in the def slot every WoW joint leaves false, which
        // by position and type is `collideConnected` (§7.2). Unlike WoW — which never sets it —
        // SC2 authors it, and 0 is the common value.
        def.collideConnected = pj.enableShape != 0;

        switch (pj.jointType) {
        case 0:
            // Spherical is the one family that keeps the symmetric two-anchor rule; the two
            // matrices' rotations are discarded outright.
            def.kind = sb::JointKind::Spherical;
            def.localAnchorA = anchorA;
            def.localAnchorB = anchorB;
            break;

        case 1:
            def.kind = sb::JointKind::Revolute;
            def.localFrameA = frameA.rotation;
            def.localFrameB = frameB.rotation;
            // The hinge reference is **the anchor-A slot read as a quaternion**, whose z axis
            // is the hinge axis; a zero reference leaves the whole angular half silently inert.
            def.localAnchorA = frameA.rotation;
            // Every family except distance and spherical pins *B's origin* at the point
            // `localAnchorB` expressed in **A's** frame, and ignores `localAnchorA` for
            // position entirely. So the A-space slot receives frame A's origin.
            def.localAnchorB = anchorA;
            def.lowerTwist = pj.limitMin;
            def.upperTwist = pj.limitMax;
            def.enableTwistLimit = LimitsEnabled(pj);
            break;

        case 2:
            def.kind = sb::JointKind::Shoulder;
            def.localFrameA = frameA.rotation;
            def.localFrameB = frameB.rotation;
            def.localAnchorA = frameA.rotation;
            def.localAnchorB = anchorA;
            def.coneAngle = pj.coneAngle;
            def.lowerTwist = pj.limitMin;
            def.upperTwist = pj.limitMax;
            // `enableLimits` gates the twist only; the cone is unconditional.
            def.enableTwistLimit = LimitsEnabled(pj);
            break;

        case 3:
            def.kind = sb::JointKind::Weld;
            def.localFrameA = frameA.rotation;
            def.localFrameB = frameB.rotation;
            def.localAnchorA = frameA.rotation;
            def.localAnchorB = anchorA;
            // `angularFrequency` (5 Hz across the corpus) and `dampingRatio` (0.7) make SC2's
            // weld a *soft* constraint. Snowball's weld is rigid and its only soft drive is the
            // shoulder's opt-in angular spring, so a welded SC2 attachment is stiffer here than
            // in the client — visible as a piece that does not wobble, not as one in the wrong
            // place.
            break;

        default:
            // Four types, not eight: the file has no fifth (§6.4).
            continue;
        }

        scene_.AddJoint(def);

        if (debug) {
            std::fprintf(stderr,
                         "[phys]   joint type=%u bones %u->%u cone=%.3f twist=[%.3f %.3f] "
                         "limits=%u collide=%d\n",
                         pj.jointType, pj.boneIndex1, pj.boneIndex2, pj.coneAngle, pj.limitMin,
                         pj.limitMax, pj.enableLimits, static_cast<int>(def.collideConnected));
        }
    }
}

void Sc2PhysicsStage::Seed(const FrameState& fs) {
    for (Link& l : links_) {
        if (static_cast<std::size_t>(l.bone) >= fs.boneWorldMatrices.size()) {
            continue;
        }
        const BoneFrame frame = DecomposeBone(fs.boneWorldMatrices[static_cast<std::size_t>(l.bone)]);
        scene_.SetTransform(l.body, frame.xf);
        scene_.SetLinearVelocity(l.body, sb::Vec4{});
        scene_.SetAngularVelocity(l.body, sb::Vec4{});
        l.scale = frame.scale;
        l.seedPos = frame.xf.position;
    }
}

void Sc2PhysicsStage::DriveKinematic(const FrameState& fs, f32 invT) {
    for (Link& l : links_) {
        if (l.dynamic || static_cast<std::size_t>(l.bone) >= fs.boneWorldMatrices.size()) {
            continue;
        }
        const sb::Body& b = scene_.Get(l.body);
        if (b.type == sb::BodyType::Static) {
            continue;
        }

        const BoneFrame frame =
            DecomposeBone(fs.boneWorldMatrices[static_cast<std::size_t>(l.bone)]);
        const sb::Transform& cur = b.transform;
        sb::Vec4 targetRot = frame.xf.rotation;

        // Hemisphere fix: q and -q are the same orientation, and interpolating toward the far
        // one takes the long way round — visibly, on a bone that crosses the boundary
        // mid-animation.
        const f32 dot = cur.rotation.x * targetRot.x + cur.rotation.y * targetRot.y +
                        cur.rotation.z * targetRot.z + cur.rotation.w * targetRot.w;
        if (dot < 0.0f) {
            targetRot = sb::Vec4{-targetRot.x, -targetRot.y, -targetRot.z, -targetRot.w};
        }

        sb::Vec4 dp = frame.xf.position - cur.position;
        sb::Vec4 dq{targetRot.x - cur.rotation.x, targetRot.y - cur.rotation.y,
                    targetRot.z - cur.rotation.z, targetRot.w - cur.rotation.w};
        // `w = 2 * dq (x) conj(q)` — the inverse of `dq/dt = 0.5 * w (x) q` with world-frame w
        // on the left. Routed through `QuatMultiply` rather than expanded by hand: the
        // expansion is easy to write with the cross term negated, which is invisible at rest
        // and turns every moving collider's spin the wrong way once an animation plays.
        auto angularOf = [](const sb::Vec4& delta, const sb::Vec4& q) {
            const sb::Vec4 w = sb::QuatMultiply(delta, sb::Vec4{-q.x, -q.y, -q.z, q.w});
            return sb::Vec4{2.0f * w.x, 2.0f * w.y, 2.0f * w.z, 0.0f};
        };

        // Past either cap the body is teleported by the **excess only** — the fraction that
        // brings the error back down to the cap — and the rest is still handed over as
        // velocity. Below both caps nothing is teleported at all.
        const f32 linear = Length3(dp);
        const f32 angular = Length3(angularOf(dq, cur.rotation));
        const f32 overLinear = linear > kSnapLinear ? (linear - kSnapLinear) / linear : 0.0f;
        const f32 overAngular =
            angular > kSnapAngular ? (angular - kSnapAngular) / angular : 0.0f;
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

        // The residual becomes velocity rather than being snapped away: that is what the
        // contact solver reads, and it is why a moving collider drags what it touches instead
        // of merely passing through it.
        const sb::Vec4 linVel = dp * invT;
        const sb::Vec4 angVel = angularOf(dq, driven.rotation) * invT;
        scene_.SetLinearVelocity(l.body, Length3(linVel) < kDriveEpsilon ? sb::Vec4{} : linVel);
        scene_.SetAngularVelocity(l.body, Length3(angVel) < kDriveEpsilon ? sb::Vec4{} : angVel);
    }
}

void Sc2PhysicsStage::WriteBack(FrameState& fs, const PoseStageContext& ctx) {
    written_.assign(fs.boneWorldMatrices.size(), 0);

    for (const Link& l : links_) {
        if (!l.dynamic) {
            continue;
        }
        const std::size_t bone = static_cast<std::size_t>(l.bone);
        if (bone < fs.boneWorldMatrices.size()) {
            fs.boneWorldMatrices[bone] = ToMatrix(scene_.Get(l.body).transform, l.scale);
            written_[bone] = 1;
        }
    }

    // **A stage that writes a bone owns its descendants** (`pose_stage.h`), and a `PHRB` rig
    // does not cover every bone under it — a ragdoll's hands and its attachment points hang off
    // simulated arms with no body of their own. StarCraft II gets this for free by storing each
    // node's transform *parent-relative* and recomputing the local TRS from the new world pose;
    // this pipeline stores world matrices, so the same effect is had by re-applying each
    // unclaimed bone's animated local transform on top of its new parent.
    //
    // Both matrices come from the pre-simulation snapshot, so the local is the one the sampler
    // produced whether or not the parent was itself simulated.
    if (ctx.nodeParents.size() != fs.boneWorldMatrices.size()) {
        return;
    }
    for (std::size_t i = 0; i < fs.boneWorldMatrices.size(); ++i) {
        const i32 parent = ctx.nodeParents[i];
        if (written_[i] || parent < 0 || !written_[static_cast<std::size_t>(parent)]) {
            continue;
        }
        const auto p = static_cast<std::size_t>(parent);
        const Matrix44f local = animated_[i] * Matrix44f::inverse(animated_[p]);
        fs.boneWorldMatrices[i] = local * fs.boneWorldMatrices[p];
        // Marked written so this bone's own children follow it in the same pass. Bones are
        // stored parents-first, which is what makes one forward pass enough.
        written_[i] = 1;
    }
}

void Sc2PhysicsStage::Run(FrameState& fs, const PoseStageContext& ctx) {
    if (fs.boneWorldMatrices.empty()) {
        return;
    }

    // **Built on the first frame rather than in the constructor**, because a fixture's size is
    // a function of the pose: SC2 takes the bone's world scale at creation and hands it to
    // every shape, and joint anchors are scaled the same way. A stage constructed before any
    // pose exists would have to assume 1.0 and would size a scaled hero's collision wrongly.
    if (!built_) {
        Build(fs);
        built_ = true;
        return;
    }
    if (links_.empty()) {
        return;
    }

    // Real time, not animation time: a paused actor's ragdoll still settles.
    accumulator_ = (std::min)(accumulator_ + static_cast<f32>(ctx.frameDtMs) * 0.001f,
                              kMaxAccumulator);
    const i32 substeps =
        (std::min)(static_cast<i32>(accumulator_ / kSubStep), kMaxSubSteps);

    animated_.assign(fs.boneWorldMatrices.begin(), fs.boneWorldMatrices.end());
    for (Link& l : links_) {
        const auto bone = static_cast<std::size_t>(l.bone);
        if (bone < animated_.size()) {
            l.scale = DecomposeBone(animated_[bone]).scale;
        }
    }

    // Before the drive, and against the animated pose: a body that goes dynamic this frame is
    // placed on the bone the sampler just produced, and one that goes back to kinematic must be
    // driven rather than stepped on the same frame it changes.
    UpdateDrivenState(fs);

    if (substeps > 0) {
        // One drive per frame against the whole span about to be stepped, not per substep —
        // the binary computes `1 / (substeps * (1/60))` once and lets the body integrate
        // across all of them.
        DriveKinematic(fs, 1.0f / (static_cast<f32>(substeps) * kSubStep));
        for (i32 k = 0; k < substeps; ++k) {
            scene_.Step(kSubStep, kVelocityIterations, kPositionIterations);
            accumulator_ -= kSubStep;
        }
    }

    // Every frame, including one that stepped nothing: the bones must keep showing the
    // simulated pose, or the model snaps back to its animation on every frame the accumulator
    // does not fill.
    WriteBack(fs, ctx);
    RebuildClaims(fs);
    // Redrawn from the simulated pose, over whatever the source placed from the animated one.
    // The two differ by exactly the thing the overlay exists to show — nothing else can
    // separate "the bodies are in the wrong place" from "the bodies are right and the skinning
    // is wrong".
    Sc2PlaceCollisionShapes(shapeBones_, shapeLocals_, fs.boneWorldMatrices,
                            fs.collisionTransforms);

    if (std::getenv("WDX_PHYSICS_DEBUG") != nullptr && ++debugFrame_ % 200 == 0) {
        f32 worst = 0.0f, drop = 0.0f;
        i32 worstBone = -1;
        for (const Link& l : links_) {
            if (!l.dynamic) continue;
            const sb::Vec4& p = scene_.Get(l.body).transform.position;
            const f32 d = Length3(p - l.seedPos);
            if (d > worst) { worst = d; worstBone = l.bone; }
            drop = (std::min)(drop, p.z - l.seedPos.z);
        }
        int touching = 0;
        for (sb::ContactId c = scene_.FirstContact(); c >= 0; c = scene_.NextContact(c)) {
            const sb::Contact* ct = scene_.GetContact(c);
            if (ct == nullptr) break;
            if (ct->touching) ++touching;
        }
        // Displacement from the **seed** pose, not from the animated one: a claimed bone's
        // entry in `boneWorldMatrices` is last frame's physics result, so comparing against it
        // measures physics against itself.
        std::fprintf(stderr,
                     "[phys] sc2 f%d moved-from-seed max=%.4f (bone %d) lowest dz=%.4f "
                     "contacts=%d touching=%d\n",
                     debugFrame_, worst, worstBone, drop, scene_.ContactCount(), touching);
    }
}

} // namespace

Matrix44f Sc2RoundTripBoneFrame(const Matrix44f& world) {
    const BoneFrame f = DecomposeBone(world);
    return ToMatrix(f.xf, f.scale);
}

void Sc2DecomposeBone(const Matrix44f& world, Quaternion& rotation, Vector3f& translation) {
    const BoneFrame f = DecomposeBone(world);
    rotation = {f.xf.rotation.x, f.xf.rotation.y, f.xf.rotation.z, f.xf.rotation.w};
    translation = {f.xf.position.x, f.xf.position.y, f.xf.position.z};
}

Sc2CollisionShapes Sc2BuildCollisionShapes(const ::whiteout::m3::Model& model) {
    namespace rm = ::whiteout::flakes::renderer::model;
    Sc2CollisionShapes out;
    const std::vector<u8> dynamic = AuthoredDynamicPerBody(model);
    // The same one-body-per-bone rule the stage uses, so the two agree on which bodies exist.
    std::vector<u8> taken(model.bones.size(), 0);

    for (std::size_t rbIndex = 0; rbIndex < model.rigidBodies.size(); ++rbIndex) {
        const w3::RigidBody& rb = model.rigidBodies[rbIndex];
        const std::size_t bone = rb.parentBoneIndex;
        if (bone >= model.bones.size() || taken[bone]) {
            continue;
        }
        taken[bone] = 1;

        const bool isStatic = rb.getVersion() >= 3 && rb.simulationType == 2;
        const auto kind = static_cast<i32>(isStatic          ? rm::CollisionBodyKind::Static
                                           : dynamic[rbIndex] ? rm::CollisionBodyKind::Dynamic
                                                              : rm::CollisionBodyKind::Kinematic);

        for (const w3::PhysicsShape& ps : rb.rigidBodyShape) {
            // **A `PHSH` matrix is a full affine transform, and its three rows routinely have
            // three different lengths** — a collider is authored as a unit primitive and
            // squashed onto the limb it wraps. Which parts of that a *fixture* honours is not
            // one rule but five, so the overlay follows the same five rather than picking one:
            //
            //   box       all three row lengths, per axis (the offline cook bakes them into the
            //             polytope and the fixture then carries a uniform body scale)
            //   cylinder  likewise — a prism with an elliptical cross-section
            //   sphere    the **largest**, since a sphere has one radius
            //   capsule   row 0 for the radius and row 2 for the length; the shaft stays round
            //   hull      the largest, uniformly, and the shape matrix's rotation is baked into
            //             the shipped vertices
            //
            // The per-axis kinds get their scale through @ref Sc2CollisionShapes::locals, which
            // is also what keeps their *orientation* — the alternative, an axis-aligned span of
            // a rotated box's corners, is a wireframe that grows and shrinks as the limb turns.
            const BoneFrame sf = DecomposeBone(ps.transform);
            const Vector3f row = sf.scale;
            const Vector3f dims = ps.shapeDimensions;
            const Vector3f kUnitScale{1.0f, 1.0f, 1.0f};

            rm::CollisionShapeData d{};
            d.bodyKind = kind;
            // Rotation and translation only. The three kinds that collapse their scale to one
            // number bake it into the geometry below instead, or the wireframe would be
            // stretched where the fixture is not.
            Matrix44f local = ToMatrix(sf.xf, kUnitScale);
            bool ok = false;

            switch (ps.shapeType) {
            case w3::PhysicsShapeType::Box:
                d.type = static_cast<i32>(rm::CollisionShapeType::Box);
                d.vertices[0] = {-dims.x, -dims.y, -dims.z};
                d.vertices[1] = {dims.x, dims.y, dims.z};
                local = ToMatrix(sf.xf, row);
                ok = true;
                break;

            case w3::PhysicsShapeType::Sphere:
                d.type = static_cast<i32>(rm::CollisionShapeType::Sphere);
                d.vertices[0] = {0.0f, 0.0f, 0.0f};
                d.radius = dims.x * (std::max)({row.x, row.y, row.z});
                ok = true;
                break;

            case w3::PhysicsShapeType::Capsule: {
                const f32 height = dims.y * row.z;
                if (height <= kCapsuleCollapse) {
                    // Drawn as the sphere it is simulated as, not as the capsule it is authored
                    // as — the overlay's job is to show the fixture, not the file.
                    d.type = static_cast<i32>(rm::CollisionShapeType::Sphere);
                    d.vertices[0] = {0.0f, 0.0f, 0.0f};
                } else {
                    d.type = static_cast<i32>(rm::CollisionShapeType::Cylinder);
                    d.vertices[0] = {0.0f, 0.0f, height * 0.5f};
                    d.vertices[1] = {0.0f, 0.0f, -height * 0.5f};
                }
                d.radius = dims.x * row.x;
                ok = true;
                break;
            }

            case w3::PhysicsShapeType::Cylinder:
                // Drawn round in its own frame and made elliptical by the frame's own scale,
                // which is what the baked prism is. The eight sides the fixture uses are our
                // choice rather than the file's, so the wireframe does not repeat them.
                d.type = static_cast<i32>(rm::CollisionShapeType::Cylinder);
                d.vertices[0] = {0.0f, 0.0f, dims.y * 0.5f};
                d.vertices[1] = {0.0f, 0.0f, -dims.y * 0.5f};
                d.radius = dims.x;
                local = ToMatrix(sf.xf, row);
                ok = true;
                break;

            case w3::PhysicsShapeType::ConvexHull: {
                if (!ps.hullVertexPositions.empty()) {
                    // A hull has no primitive to draw, so its extent is the box its points
                    // span — under the same uniform scale the fixture takes.
                    const f32 uniform = (std::max)({row.x, row.y, row.z});
                    Vector3f lo{ps.hullVertexPositions[0].x, ps.hullVertexPositions[0].y,
                                ps.hullVertexPositions[0].z};
                    Vector3f hi = lo;
                    for (const Vector4f& v : ps.hullVertexPositions) {
                        lo = {(std::min)(lo.x, v.x), (std::min)(lo.y, v.y), (std::min)(lo.z, v.z)};
                        hi = {(std::max)(hi.x, v.x), (std::max)(hi.y, v.y), (std::max)(hi.z, v.z)};
                    }
                    d.type = static_cast<i32>(rm::CollisionShapeType::Box);
                    d.vertices[0] = {lo.x * uniform, lo.y * uniform, lo.z * uniform};
                    d.vertices[1] = {hi.x * uniform, hi.y * uniform, hi.z * uniform};
                    ok = true;
                }
                break;
            }

            case w3::PhysicsShapeType::Mesh:
                // No fixture is built for one, so none is drawn: a mesh shape shown beside real
                // colliders would read as something the solver knows about, and it is not.
                break;
            }

            if (ok) {
                out.shapes.push_back(d);
                out.locals.push_back(local);
                out.bones.push_back(static_cast<i32>(bone));
            }
        }
    }
    return out;
}

void Sc2PlaceCollisionShapes(std::span<const i32> bones, std::span<const Matrix44f> locals,
                             std::span<const Matrix44f> boneWorld, std::vector<Matrix44f>& out) {
    if (bones.empty() || locals.size() != bones.size()) {
        return;
    }
    out.resize(bones.size());
    // The bone frames are shared between shapes and cost a decompose each, so the last one is
    // kept rather than recomputed — a body's shapes are contiguous, which makes this the common
    // case rather than an optimisation for a case that does not occur.
    i32 cachedBone = -1;
    Matrix44f placement = Matrix44f::identity();
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const i32 bone = bones[i];
        if (bone < 0 || static_cast<std::size_t>(bone) >= boneWorld.size()) {
            out[i] = Matrix44f::identity();
            continue;
        }
        if (bone != cachedBone) {
            const BoneFrame f = DecomposeBone(boneWorld[static_cast<std::size_t>(bone)]);
            const f32 uniform = (std::min)({f.scale.x, f.scale.y, f.scale.z});
            placement = ToMatrix(f.xf, Vector3f{uniform, uniform, uniform});
            cachedBone = bone;
        }
        out[i] = locals[i] * placement;
    }
}

std::unique_ptr<animation::IPoseStage> CreateSc2PhysicsStage(const ::whiteout::m3::Model& model) {
    if (model.rigidBodies.empty() || model.bones.empty()) {
        return nullptr;
    }
    auto stage = std::make_unique<Sc2PhysicsStage>(model);
    // No body that ever becomes dynamic means nothing is ever written back, and an inert stage
    // still costs a virtual call and a claim copy per actor per frame. That is the common case
    // by a wide margin: most `PHRB` in the corpus are kinematic hit proxies on models with no
    // joints at all.
    if (!stage->WouldSimulate()) {
        return nullptr;
    }
    return stage;
}

} // namespace whiteout::flakes::renderer::profiles::sc2_heroes
