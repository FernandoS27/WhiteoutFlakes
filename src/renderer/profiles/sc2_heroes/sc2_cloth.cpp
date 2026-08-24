#include "renderer/profiles/sc2_heroes/sc2_cloth.h"

#include "renderer/profiles/sc2_heroes/sc2_physics.h"

#include "whiteout/models/m3/structures.h"

#include "snowball/cloth_authoring.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace whiteout::flakes::renderer::profiles::sc2_heroes {
namespace {

namespace sb = ::snowball;
namespace w3 = ::whiteout::m3;
using ::whiteout::flakes::renderer::animation::IPoseStage;
using ::whiteout::flakes::renderer::animation::PoseStageContext;
using ::whiteout::flakes::renderer::model::FrameState;

/// The same world gravity the rigid-body stage runs at, scaled per cloth by
/// `PHCL.gravity` — `CModelPhysics_Build` multiplies `dmWorld_GetGravity()` by
/// that float and stores the product as the cloth's own gravity vector, so a
/// cloth at `gravity = 3` falls three times as fast as the ragdoll beside it.
constexpr sb::Vec4 kGravity{0.0f, 0.0f, -8.8f, 0.0f};

/// @name The warm-up, from `M3Physics_StepCloth`
///
/// A freshly built cloth is stepped **fifty times at 0.017 s** before its first
/// real frame — about 0.85 s of settling — so it enters the world hanging
/// rather than standing out in its authored rest shape. The dt is a distinct
/// literal (`0x3C8B4396`), not the 1/60 the same function writes and discards
/// one line earlier.
/// @{
constexpr f32 kWarmupDt = 0.017000001f;
constexpr i32 kWarmupSteps = 50;
/// @}

/// `CModelPhysics_Build` writes this into the def's wind-impulse lane as a bare
/// literal (`0x3F9A1FF3`); nothing in the chunk feeds it.
constexpr f32 kWindImpulseScale = 1.2041f;

/// The rest-pull stiffness is the one def lane `CModelPhysics_Build` never
/// writes — the slot at def+0x1A4 is left as whatever the stack held, the same
/// shipped defect as the world-sphere push scale. Zero is the only defensible
/// reading of "uninitialised", and it turns the pass off, which is what a
/// zeroed stack would do too.
constexpr f32 kRestPoseStiffness = 0.0f;

/// A frame longer than this settles slowly instead of exploding — the same cap
/// the rigid-body stage puts on its accumulator, for the same reason.
constexpr f32 kMaxDt = 0.05f;

/// @name Substepping
///
/// StarCraft II steps each cloth exactly once per rendered frame, at the
/// world's frame delta (`CModel_Update` hands `M3Physics_StepCloth` the world's
/// dt and nothing subdivides it). That is only accurate while the frame is
/// short, and the collision pass is a single positional projection with no
/// swept test: a collider that moves further than its own radius between two
/// samples can end the step on the far side of the cloth.
///
/// Measured on Jaina — whose `PHCL.gravity` of 5 is the second highest in the
/// corpus, so her cape is authored heavy and fast — the thigh and knee capsules
/// sweep 1.0 to 1.6 times their own radius per 60 Hz frame during a walk, and
/// half of those frames end with cloth inside a leg. So the cadence is
/// reproduced faithfully and is still wrong on screen; a game hides it by
/// tuning the numbers in the data editor, which is a lever a viewer of shipped
/// models does not have.
///
/// Subdividing is therefore a deliberate divergence, and a cheap one: it
/// touches only how often the host calls `Cloth::Step`, never a line inside it,
/// so the byte-exact oracle (which drives `dmCloth_Step` with its own dt) is
/// untouched either way. `PoseStageContext::substepPhysics` turns it off.
///
/// The frame is split into equal parts rather than run off a fixed-rate
/// accumulator: an accumulator leaves some frames stepping zero times, which
/// reads as a stuttering cape above 60 fps, where equal parts always advance
/// exactly one frame's worth.
/// @{
constexpr f32 kMaxSubStep = 1.0f / 240.0f;
constexpr i32 kMaxSubSteps = 8;
/// @}

/// An M3 vertex names its bone in a **byte** and the skinning constant buffer
/// holds `bls::kMaxBones` = 256 matrices, so a cloth of more than 256 particles
/// cannot be addressed by the region it drives — both ceilings land on the same
/// number. Twenty of the corpus's 216 mesh-carrying cloths are over it (Zeratul
/// at 282, Tyrande Warden at 447, Zeratul Ultimate at 458); those models keep
/// their authored skin, which is the cape carried rigidly rather than nothing
/// at all. Lifting it means splitting the influenced region's triangles into
/// groups of at most 256 distinct particles and emitting one geoset per group.
constexpr std::size_t kMaxParticles = 256;

Vector3f RowOf(const Matrix44f& m, int r) {
    return {m.data[r][0], m.data[r][1], m.data[r][2]};
}

f32 Length3(const Vector3f& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

sb::Vec4 ToVec4(const Vector3f& v) {
    return sb::Vec4{v.x, v.y, v.z, 0.0f};
}

sb::ClothAnchor AnchorOf(const Matrix44f& m) {
    Quaternion q;
    Vector3f t;
    Sc2DecomposeBone(m, q, t);
    return sb::ClothAnchor{sb::Vec4{q.x, q.y, q.z, q.w}, sb::Vec4{t.x, t.y, t.z, 0.0f}};
}

/// @brief `PHCC` -> `ClothCapsuleDef`, per `CLOTH_HOST_NOTES.md` §3.6.
///
/// Two things in here look like transcription errors and are not. The **axis
/// swizzle** `(r0, r1, r2, r3) -> (r2, r1, -r0, r3)` maps the collider's
/// authored Z axis onto the +X the cloth narrowphase works along — the two
/// halves of the engine disagree about which way a capsule points. And the
/// **crossed scale factors**: after the swizzle the capsule's axis is the
/// authored row 2, yet the client scales the *length* by |row 0| and the
/// *radius* by |row 2|. Invisible for a uniformly scaled collider, which is
/// almost all of them, and as-built for the rest.
bool BuildCapsule(const w3::ClothCollider& cc, const Matrix44f& inverseBind, i16 bone,
                  sb::ClothCapsuleDef& out) {
    const Vector3f r0 = RowOf(cc.transform, 0);
    const Vector3f r1 = RowOf(cc.transform, 1);
    const Vector3f r2 = RowOf(cc.transform, 2);
    const f32 nAxis = Length3(r2);
    const f32 nY = Length3(r1);
    const f32 nZ = Length3(r0);

    Matrix44f swizzled = Matrix44f::identity();
    for (std::size_t j = 0; j < 3; ++j) {
        swizzled.data[0][j] = r2.data[j];
        swizzled.data[1][j] = r1.data[j];
        swizzled.data[2][j] = -r0.data[j];
        swizzled.data[3][j] = cc.transform.data[3][j];
    }

    // The bind matrix's own row norms, which the translation picks back up: the
    // client re-scales `Mloc.row3` by them because the inverse bind it just
    // multiplied through carried their reciprocals.
    const Matrix44f bind = Matrix44f::inverse(inverseBind);
    const Vector3f bindScale{Length3(RowOf(bind, 0)), Length3(RowOf(bind, 1)),
                             Length3(RowOf(bind, 2))};

    const Matrix44f local = swizzled * inverseBind;
    Quaternion q;
    Vector3f t;
    Sc2DecomposeBone(local, q, t);

    sb::ClothCapsuleDesc desc;
    desc.localRotation = sb::Vec4{q.x, q.y, q.z, q.w};
    desc.localPosition =
        sb::Vec4{t.x * bindScale.x, t.y * bindScale.y, t.z * bindScale.z, 0.0f};
    desc.scale = sb::Vec4{nAxis, nY, nZ, 0.0f};
    desc.radius0 = cc.radius * nAxis;
    desc.radius1 = desc.radius0;
    desc.length = cc.height * nZ;
    desc.pushScale = 1.0f;
    desc.anchor = bone;
    return sb::ClassifyClothCapsule(desc, out);
}

/// @brief `PHCL`'s tuning block -> `ClothParams`.
///
/// The correspondence is one-to-one and in order, which is the tell that the
/// chunk *is* the param block with a tool's field names on it — right down to
/// the four "skin collision" floats, which are the tether and attachment lanes
/// wearing the name of the UI group that authored them. Only the last three
/// come from anywhere else.
sb::ClothParams ParamsOf(const w3::ClothPhysics& c, bool hasColliders) {
    sb::ClothParams p;
    p.gravity = sb::Vec4{kGravity.x * c.gravity, kGravity.y * c.gravity, kGravity.z * c.gravity,
                         0.0f};
    p.windVelocity = ToVec4(c.localWind);
    p.worldFollowStiffness = c.tracking;
    p.distanceStiffness[0] = c.stretchStiffness;
    p.distanceStiffness[1] = c.horizontalStiffness;
    p.distanceStiffness[2] = c.shearStiffness;
    p.bendStiffness = c.bendingStiffness;
    p.radialImpulseScale = c.explosionScale;
    p.aeroNoiseGain = c.windScale;
    p.aeroVelocityGain = c.dragFactor;
    p.liftFactor = c.liftFactor;
    p.damping = c.damping;
    p.friction = c.friction;
    p.tetherScale = c.skinOffset;
    p.tetherExponent = c.skinExponent;
    p.attachmentStiffness = c.skinStiffness;
    p.tetherStiffness = c.sphereStiffness;
    p.massMultiplier = c.density;
    p.killBendRestAngle = c.flatten != 0;
    p.attachmentsEnabled = c.useSkinCollision != 0;
    p.hasColliders = hasColliders;
    p.windImpulseScale = kWindImpulseScale;
    p.restPoseStiffness = kRestPoseStiffness;
    return p;
}

// ---------------------------------------------------------------------------

class Sc2ClothStage final : public IPoseStage {
public:
    Sc2ClothStage(const w3::Model& model, std::shared_ptr<const Sc2ClothBuild> build, i32 firstNode)
        : model_(&model), build_(std::move(build)), firstNode_(firstNode) {}

    void Run(FrameState& fs, const PoseStageContext& ctx) override;

    /// Nothing here needs the ground query or an aim target — a cloth is fully
    /// described by the model, exactly as a ragdoll is.
    bool NeedsHostInputs() const override {
        return false;
    }

private:
    void Create();
    void WriteFrames(FrameState& fs);

    /// @brief Fill `animStep_` with the anchor pose a fraction @p t through
    ///        this frame, from `animPrev_` to `anim_`.
    ///
    /// Nlerp, not slerp: the two poses are one rendered frame apart, where the
    /// angular error between the two is far below what a cloth collider can
    /// resolve, and the solver renormalises nothing it is handed — so the
    /// shorter-arc sign fix matters and the constant-speed property does not.
    void LerpAnchors(f32 t);

    /// @brief Whether @p piece is enabled this frame. Absent sampling — a
    ///        format or a model that never keys it — reads as on.
    static bool Active(const FrameState& fs, const Sc2ClothPiece& piece) {
        return piece.chunkIndex >= fs.clothActive.size() || fs.clothActive[piece.chunkIndex] != 0;
    }

    const w3::Model* model_;
    std::shared_ptr<const Sc2ClothBuild> build_;
    i32 firstNode_ = 0;
    std::vector<sb::Cloth> cloths_;
    /// Raw bone slot -> this frame's SQT. One array for every piece, because
    /// the solver indexes it by model bone index and two cloths on one model
    /// routinely share anchor bones.
    std::vector<sb::ClothAnchor> anim_;
    /// Last frame's `anim_`, and the interpolated pose a substep is handed.
    /// Both empty until the first stepped frame, which is why substepping only
    /// engages once `animPrev_` has a frame in it.
    std::vector<sb::ClothAnchor> animPrev_;
    std::vector<sb::ClothAnchor> animStep_;
    bool created_ = false;
};

void Sc2ClothStage::LerpAnchors(f32 t) {
    animStep_.resize(anim_.size());
    for (std::size_t b = 0; b < anim_.size(); ++b) {
        const sb::ClothAnchor& a = animPrev_[b];
        const sb::ClothAnchor& c = anim_[b];
        const f32 dot = a.rotation.x * c.rotation.x + a.rotation.y * c.rotation.y +
                        a.rotation.z * c.rotation.z + a.rotation.w * c.rotation.w;
        const f32 sign = dot < 0.0f ? -1.0f : 1.0f;
        sb::Vec4 q{a.rotation.x + (c.rotation.x * sign - a.rotation.x) * t,
                   a.rotation.y + (c.rotation.y * sign - a.rotation.y) * t,
                   a.rotation.z + (c.rotation.z * sign - a.rotation.z) * t,
                   a.rotation.w + (c.rotation.w * sign - a.rotation.w) * t};
        const f32 lenSq = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
        // A blend of two opposed unit quaternions can cancel; identity is the
        // only answer here that cannot spread NaN through the solver.
        if (lenSq > 1e-12f) {
            const f32 inv = 1.0f / std::sqrt(lenSq);
            q = sb::Vec4{q.x * inv, q.y * inv, q.z * inv, q.w * inv};
        } else {
            q = sb::Vec4{0.0f, 0.0f, 0.0f, 1.0f};
        }
        animStep_[b].rotation = q;
        animStep_[b].position =
            sb::Vec4{a.position.x + (c.position.x - a.position.x) * t,
                     a.position.y + (c.position.y - a.position.y) * t,
                     a.position.z + (c.position.z - a.position.z) * t, 0.0f};
    }
}

void Sc2ClothStage::Create() {
    cloths_.resize(build_->pieces.size());
    for (std::size_t i = 0; i < build_->pieces.size(); ++i)
        cloths_[i].Create(build_->pieces[i].def);
    created_ = true;
}

void Sc2ClothStage::WriteFrames(FrameState& fs) {
    for (std::size_t i = 0; i < build_->pieces.size(); ++i) {
        const Sc2ClothPiece& piece = build_->pieces[i];
        // `active` gates the *write-back*, not the step. StarCraft II samples it
        // after `dmCloth_Step` and tests it at the top of the skinning pass, so
        // an inactive cloth goes on simulating and simply stops feeding the
        // mesh — which is why it snaps back to a settled shape rather than a
        // stale one when it comes on again. Leaving the palette entries alone is
        // the same thing as leaving that vertex buffer unwritten.
        if (!Active(fs, piece))
            continue;
        const sb::Cloth& cloth = cloths_[i];
        const std::size_t base =
            static_cast<std::size_t>(firstNode_) + piece.firstParticle;
        for (std::size_t k = 0; k < piece.particleCount; ++k) {
            Matrix44f m = Matrix44f::identity();
            for (int r = 0; r < 4; ++r) {
                const sb::Vec4& row = cloth.OutputRow(static_cast<i32>(k), r);
                m.data[r][0] = row.x;
                m.data[r][1] = row.y;
                m.data[r][2] = row.z;
            }
            fs.boneWorldMatrices[base + k] = m;
        }
    }
}

void Sc2ClothStage::Run(FrameState& fs, const PoseStageContext& ctx) {
    const std::size_t boneCount = model_->bones.size();
    if (fs.boneWorldMatrices.size() < static_cast<std::size_t>(firstNode_) + build_->particleCount)
        return; // The palette was not widened; writing would run off the end.

    // Anchors first, for both the create and the step: `Cloth::Create` poses
    // the pinned particles and the colliders off the *bind* table, and the
    // first `Step` needs an animated table that is not still zeroed.
    anim_.resize((std::max)(boneCount, anim_.size()));
    for (const Sc2ClothPiece& piece : build_->pieces) {
        for (::whiteout::u16 b : piece.skinBones) {
            if (b < boneCount)
                anim_[b] = AnchorOf(fs.boneWorldMatrices[b]);
        }
    }

    if (!created_) {
        Create();
        // The warm-up runs on the pose that built it, so a model that spawns
        // mid-animation settles into *that* pose rather than into bind.
        for (std::size_t i = 0; i < cloths_.size(); ++i) {
            sb::ClothFrame frame;
            frame.dt = kWarmupDt;
            frame.bind = build_->pieces[i].def.bindPose.data();
            frame.anim = anim_.data();
            for (i32 s = 0; s < kWarmupSteps; ++s)
                cloths_[i].Step(frame);
        }
        animPrev_ = anim_;
        WriteFrames(fs);
        return;
    }

    // Real time, not animation time: a paused actor's cloth still settles. Same
    // rule as the WoW stage, and for the same reason.
    const f32 dt = (std::min)(static_cast<f32>(ctx.frameDtMs) * 0.001f, kMaxDt);
    if (dt > 0.0f) {
        i32 steps = 1;
        if (ctx.substepPhysics && dt > kMaxSubStep && animPrev_.size() == anim_.size()) {
            steps = (std::min)(static_cast<i32>(std::ceil(dt / kMaxSubStep)), kMaxSubSteps);
        }
        const f32 subDt = dt / static_cast<f32>(steps);
        for (i32 s = 1; s <= steps; ++s) {
            // Substepping only helps if the colliders move *between* substeps.
            // Handing every substep the same end-of-frame pose would shrink the
            // cloth's own integration but leave each capsule teleporting its
            // whole frame of travel in one go — the thing being fixed. So the
            // anchor table is walked from last frame's pose to this one, and
            // `AdvanceAnchors` derives its per-step deltas off that.
            if (steps > 1) {
                LerpAnchors(static_cast<f32>(s) / static_cast<f32>(steps));
            }
            for (std::size_t i = 0; i < cloths_.size(); ++i) {
                sb::ClothFrame frame;
                frame.dt = subDt;
                frame.bind = build_->pieces[i].def.bindPose.data();
                frame.anim = (steps > 1) ? animStep_.data() : anim_.data();
                cloths_[i].Step(frame);
            }
        }
    }
    animPrev_ = anim_;
    WriteFrames(fs);
}

} // namespace

Sc2ClothBuild Sc2BuildCloth(const w3::Model& model) {
    Sc2ClothBuild out;
    if (model.clothPhysics.empty() || model.bones.empty() || model.divisions.empty())
        return out;

    const auto& div = model.divisions[0];
    const std::size_t vertexCount = model.vertices.vertexCount();
    if (vertexCount == 0)
        return out;
    const std::vector<Vector3f> positions = model.vertices.getPositions();

    // Every anchor read is by raw model bone slot, so the table covers the
    // whole skeleton whether or not a given bone anchors anything.
    std::vector<sb::ClothAnchor> bindPose(model.bones.size());
    for (std::size_t b = 0; b < model.bones.size(); ++b) {
        const Matrix44f bind = (b < model.initialReference.size())
                                   ? Matrix44f::inverse(model.initialReference[b].matrix)
                                   : Matrix44f::identity();
        bindPose[b] = AnchorOf(bind);
    }

    for (std::size_t ci = 0; ci < model.clothPhysics.size(); ++ci) {
        const w3::ClothPhysics& c = model.clothPhysics[ci];

        // A `PHCL` with no per-vertex data is not a broken cloth: it is a
        // **collider source**, a record that exists only to carry `PHCC`
        // capsules for *another* model's cloth to collide against — a mount
        // under a rider's cape. StarCraft II attaches those through
        // `RegisterClothColliderSource` on the actor layer
        // (`CLOTH_HOST_NOTES.md` §3.5), which needs two models and an
        // attachment; the viewer has neither, so there is nothing to build.
        // Common enough to be worth naming: it is most of the corpus's `PHCL`
        // records.
        if (c.simEnabled.empty())
            continue;

        // `clothMeshCount` is a REGN *index*, not a count — measured over the
        // corpus, and confirmed by every `PHAC` naming the same region in its
        // `clothIndex`. The parser's field name is the tool's.
        const std::size_t simRegion = c.clothMeshCount;
        if (simRegion >= div.regions.size())
            continue;
        const auto& region = div.regions[simRegion];
        const std::size_t n = region.vertexCount;
        if (n < 3 || n > kMaxParticles || region.indexCount < 3)
            continue;
        if (c.simEnabled.size() != n || c.vertexBones.size() != n || c.vertexWeights.size() != n)
            continue;
        if (static_cast<std::size_t>(region.firstVertex) + n > vertexCount)
            continue;
        const std::size_t iEnd = static_cast<std::size_t>(region.firstIndex) + region.indexCount;
        if (iEnd > div.faces.size())
            continue;

        sb::ClothMeshSource source;
        source.vertices.resize(n);
        for (std::size_t v = 0; v < n; ++v) {
            sb::ClothMeshVertex& mv = source.vertices[v];
            // Identity bind rows, which is what makes the output frame a plain
            // rest-to-live frame transfer (see `Sc2ClothBuild`). The rows exist
            // so a host can carry a per-vertex basis through the solver; M3
            // does not author one.
            mv.restPosition = ToVec4(positions[region.firstVertex + v]);
            const ::whiteout::u8 flags = c.simEnabled[v];
            mv.movable = (flags & 1u) != 0u;
            mv.selfCollision = (flags & 2u) != 0u;
            const ::whiteout::u32 bones = c.vertexBones[v];
            const ::whiteout::u32 weights = c.vertexWeights[v];
            for (int k = 0; k < 4; ++k) {
                const f32 w = static_cast<f32>((weights >> (8 * k)) & 0xFFu) / 255.0f;
                const auto slot = static_cast<i16>((bones >> (8 * k)) & 0xFFu);
                mv.weights[static_cast<std::size_t>(k)] = w;
                mv.anchors[static_cast<std::size_t>(k)] =
                    (w > 0.0f && static_cast<std::size_t>(slot) < model.bones.size()) ? slot
                                                                                     : i16{-1};
            }
        }

        source.triangles.reserve(region.indexCount / 3);
        for (std::size_t i = region.firstIndex; i + 2 < iEnd; i += 3) {
            const ::whiteout::u16 a = div.faces[i];
            const ::whiteout::u16 b = div.faces[i + 1];
            const ::whiteout::u16 d = div.faces[i + 2];
            if (a >= n || b >= n || d >= n)
                continue;
            source.triangles.push_back({a, b, d});
        }
        if (source.triangles.empty())
            continue;

        for (const w3::ClothCollider& cc : c.colliders) {
            // `padding` is the collider's **bone index**: the shipped record is
            // {matrix, radius, length, bone} and the parser named the tail from
            // its size rather than its use. Every value in the corpus is a bone
            // this cloth already skins to.
            const std::size_t bone = cc.padding;
            if (bone >= model.bones.size() || bone >= model.initialReference.size())
                continue;
            sb::ClothCapsuleDef def;
            if (BuildCapsule(cc, model.initialReference[bone].matrix, static_cast<i16>(bone), def))
                source.capsules.push_back(def);
        }

        sb::ClothBuildResult result;
        if (!sb::BuildCloth(source, result))
            continue;

        Sc2ClothPiece piece;
        piece.def = std::move(result.def);
        piece.def.worldScale = 1.0f;
        piece.def.params = ParamsOf(c, !piece.def.capsules.empty());
        piece.def.bindPose = bindPose;
        piece.chunkIndex = ci;
        piece.simRegion = simRegion;
        piece.oldToNew = std::move(result.oldToNew);
        piece.skinBones = c.skinBones;
        piece.particleCount = piece.def.particles.size();
        piece.firstParticle = out.particleCount;

        piece.restPositions.resize(piece.particleCount);
        for (std::size_t p = 0; p < piece.particleCount; ++p) {
            const sb::Vec4& r = piece.def.particles[p].restPosition;
            piece.restPositions[p] = {r.x, r.y, r.z};
        }

        for (const w3::ClothProxy& proxy : c.proxies) {
            if (proxy.clothIndex == simRegion && proxy.proxyIndex < div.regions.size())
                piece.influencedRegions.push_back(proxy.proxyIndex);
        }

        out.particleCount += piece.particleCount;
        out.pieces.push_back(std::move(piece));
    }
    return out;
}

std::unique_ptr<animation::IPoseStage>
CreateSc2ClothStage(const w3::Model& model, std::shared_ptr<const Sc2ClothBuild> build,
                    i32 firstNode) {
    if (!build || build->pieces.empty())
        return nullptr;
    return std::make_unique<Sc2ClothStage>(model, std::move(build), firstNode);
}

} // namespace whiteout::flakes::renderer::profiles::sc2_heroes
