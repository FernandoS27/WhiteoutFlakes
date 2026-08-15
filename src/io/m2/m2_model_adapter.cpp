#include "io/m2/m2_model_adapter.h"

#include "io/m2/m2_animation.h"

#include <whiteout/models/m2/parser.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <unordered_map>

namespace whiteout::flakes::io {

using renderer::model::MeshBuffer;
using renderer::model::MeshData;
using renderer::model::SequenceInfo;
using renderer::model::SkinWeightData;
using renderer::model::SkeletonData;
using renderer::model::VertexAttribute;
using renderer::model::VertexInfluence;
using renderer::model::VertexSemantic;

namespace {

using M2Vertex = ::whiteout::m2::Vertex;

// `whiteout::m2::Vertex` IS the on-disk record: 48 bytes, no padding, in the
// order the file stores them. That is what makes the M2 path a copy rather
// than a repack — but it is also invisible, so it is asserted. If WhiteoutLib
// ever reorders or pads that struct, every baked buffer silently becomes
// garbage and this is the only place that would catch it.
static_assert(std::is_standard_layout_v<M2Vertex>, "m2::Vertex must be memcpy-able");
static_assert(sizeof(M2Vertex) == 48, "m2::Vertex must match the on-disk record");
static_assert(offsetof(M2Vertex, position) == 0);
static_assert(offsetof(M2Vertex, boneWeights) == 12);
static_assert(offsetof(M2Vertex, boneIndices) == 16);
static_assert(offsetof(M2Vertex, normal) == 20);
static_assert(offsetof(M2Vertex, texCoords) == 32);

// The record above, described for the GPU. Fixed — unlike `.m3`, `.m2` has no
// per-model vertex format flags.
std::vector<VertexAttribute> DescribeM2Vertex() {
    return {
        {VertexSemantic::Position, 0, gfx::Format::R32G32B32_FLOAT, 0},
        {VertexSemantic::BoneWeights, 0, gfx::Format::R8G8B8A8_UNORM, 12},
        {VertexSemantic::BoneIndices, 0, gfx::Format::R8G8B8A8_UINT, 16},
        {VertexSemantic::Normal, 0, gfx::Format::R32G32B32_FLOAT, 20},
        {VertexSemantic::TexCoord, 0, gfx::Format::R32G32_FLOAT, 32},
        {VertexSemantic::TexCoord, 1, gfx::Format::R32G32_FLOAT, 40},
    };
}

// One bone's local transform, in the renderer's row-vector convention:
//
//     p' = ((p - pivot) · S · R) + pivot + t
//
// which is `AnimateMT`'s `M = R; M.Scale(s); M.row3 += pivot + t;
// M.Translate(-pivot)` read back out — C44Matrix::Translate and ::Scale both
// *pre*-multiply, so the sequence composes to `T(-pivot) · S · R · T(pivot+t)`.
// Identical in form to MDX's Vec3QuatScaleToMatrix44f, which is why that one is
// not reused: it takes `mdx::` types and pulls the whole MDX structure header in
// behind it.
Matrix44f M2BoneLocal(const Vector3f& t, const Quaternion& r, const Vector3f& s,
                      const Vector3f& pivot) {
    const Matrix44f mS = Matrix44f::scaling(s);
    const Matrix44f mR = Matrix44f::rotation(r).transpose();
    const Matrix44f mNegPivot = Matrix44f::translation({-pivot.x, -pivot.y, -pivot.z});
    const Matrix44f mPivotPlusT =
        Matrix44f::translation({pivot.x + t.x, pivot.y + t.y, pivot.z + t.z});
    return mNegPivot * mS * mR * mPivotPlusT;
}

// The parent matrix bone `i` composes against, after its three "ignore parent"
// flags have had their say.
//
// The client builds this in camera space against the model's own world
// transform (`AnimateMT`'s `v379` block). Here every bone matrix is model-space
// and the model transform is applied by the vertex shader, so that world
// transform degenerates to identity: its basis rows are unit length, which
// collapses the client's rescale-to-parent-magnitude step to a plain
// normalisation, and its translation row is the origin.
Matrix44f M2ParentFor(const Matrix44f& parent, u32 flags, const Vector3f& pivot) {
    using ::whiteout::m2::BoneFlag;
    constexpr u32 kIgnoreMask = static_cast<u32>(BoneFlag::IgnoreParentTranslate) |
                                static_cast<u32>(BoneFlag::IgnoreParentScale) |
                                static_cast<u32>(BoneFlag::IgnoreParentRotation);
    if ((flags & kIgnoreMask) == 0)
        return parent;

    Matrix44f m = parent;
    const u32 basis = flags & (static_cast<u32>(BoneFlag::IgnoreParentScale) |
                               static_cast<u32>(BoneFlag::IgnoreParentRotation));
    if (basis == static_cast<u32>(BoneFlag::IgnoreParentScale)) {
        for (i32 row = 0; row < 3; ++row) {
            Vector3f v{m.data[row][0], m.data[row][1], m.data[row][2]};
            const f32 len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
            if (len > 1e-6f)
                v *= 1.0f / len;
            m.data[row][0] = v.x;
            m.data[row][1] = v.y;
            m.data[row][2] = v.z;
        }
    } else if (basis != 0) {
        // Scale *and* rotation ignored: the model's own basis, i.e. identity.
        // The client branches on `flags & 6` and handles only 2 and 6, so
        // rotation-without-scale lands here too rather than in its own case.
        for (i32 row = 0; row < 3; ++row)
            for (i32 col = 0; col < 3; ++col)
                m.data[row][col] = (row == col) ? 1.0f : 0.0f;
    }

    if (flags & static_cast<u32>(BoneFlag::IgnoreParentTranslate)) {
        m.data[3][0] = m.data[3][1] = m.data[3][2] = 0.0f;
    } else {
        // Keep the pivot where the unmodified parent put it, so replacing the
        // basis rotates the bone in place instead of flinging it.
        const Vector3f anchored = whiteout::transform_point(pivot, parent);
        const Vector3f rebased = whiteout::transform_normal(pivot, m);
        m.data[3][0] = anchored.x - rebased.x;
        m.data[3][1] = anchored.y - rebased.y;
        m.data[3][2] = anchored.z - rebased.z;
    }
    return m;
}

} // namespace

std::vector<::whiteout::u8> ContentProviderCascFs::readFile(::whiteout::u32 fileId) const {
    if (!provider_)
        return {};
    auto bytes = provider_->ReadFile(ContentRef::FromFileId(fileId));
    if (!bytes)
        return {};
    return std::move(*bytes);
}

bool ContentProviderCascFs::fileExists(::whiteout::u32 fileId) const {
    // No cheaper probe than a read: IContentProvider has no existence query,
    // and adding one for this would push a CASC-shaped concept into an
    // interface three unrelated hosts implement. The parser calls this rarely.
    return !readFile(fileId).empty();
}

std::vector<::whiteout::u8> ContentProviderPathFs::readFile(const std::string& path) const {
    if (!provider_)
        return {};
    auto bytes = provider_->ReadFile(path);
    if (!bytes)
        return {};
    return std::move(*bytes);
}

bool ContentProviderPathFs::fileExists(const std::string& path) const {
    return !readFile(path).empty();
}

std::shared_ptr<M2ModelAdapter> M2ModelAdapter::Load(const ContentRef& ref,
                                                     std::span<const ::whiteout::u8> bytes,
                                                     IContentProvider* provider,
                                                     bool lazyAnimations) {
    if (bytes.empty())
        return nullptr;
    ::whiteout::m2::Parser parser;
    parser.setLazyAnimations(lazyAnimations);
    ::whiteout::m2::Model model;
    // Heap-allocated rather than a local, because a lazy parse reads `.anim`
    // siblings through this wrapper long after Load returns.
    std::shared_ptr<void> fsKeepAlive;
    try {
        // The ref's discriminant picks the route, because it is the same
        // question: a model named by id has id-named siblings, a model named
        // by path has its siblings on disk beside it.
        if (ref.IsFileId()) {
            auto fs = std::make_shared<ContentProviderCascFs>(provider);
            model = parser.parse(*fs, bytes);
            fsKeepAlive = std::move(fs);
        } else {
            auto fs = std::make_shared<ContentProviderPathFs>(provider);
            model = parser.parse(*fs, ref.path);
            fsKeepAlive = std::move(fs);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[m2] parse failed for '%s': %s\n", ref.Describe().c_str(), e.what());
        return nullptr;
    }
    if (parser.hasIssues()) {
        // Issues are not necessarily fatal — the parser reports what it
        // skipped. Surface them rather than letting a half-read model look
        // like a clean one.
        for (const auto& issue : parser.getIssues())
            std::fprintf(stderr, "[m2] %s\n", issue.c_str());
    }
    if (model.skinProfiles.empty()) {
        // Chunked M2 keeps its skin profiles in sibling files referenced by
        // fileDataID. Reaching here means the parser could not read them,
        // which on this route means the content provider could not resolve an
        // id — a configuration problem, not a malformed model.
        std::fprintf(stderr, "[m2] no skin profile resolved for '%s'; is a WoW CASC configured?\n",
                     model.modelName.c_str());
        return nullptr;
    }
    if (!lazyAnimations)
        fsKeepAlive.reset();
    return std::make_shared<M2ModelAdapter>(std::move(model), std::move(fsKeepAlive));
}

M2ModelAdapter::M2ModelAdapter(::whiteout::m2::Model model, std::shared_ptr<void> fsKeepAlive)
    : model_(std::move(model)), fsKeepAlive_(std::move(fsKeepAlive)) {
    // Profile 0 is the highest detail level. LOD selection is a later phase;
    // taking one and saying so beats taking whichever happens to be first
    // without noticing there were others.
    profileIndex_ = 0;
    if (profileIndex_ < model_.skinProfiles.size())
        submeshCount_ = model_.skinProfiles[profileIndex_].submeshes.size();
    globalLoops_.reserve(model_.globalLoops.size());
    for (const auto& g : model_.globalLoops)
        globalLoops_.push_back(g.timestamp);
}

std::vector<MeshData> M2ModelAdapter::GetMeshes() {
    std::vector<MeshData> out;
    if (profileIndex_ >= model_.skinProfiles.size())
        return out;
    const auto& skin = model_.skinProfiles[profileIndex_];
    out.reserve(skin.submeshes.size());

    // Two levels of indirection, which is the whole trick of the format:
    //   skin.indices[i]            → an index into skin.vertices
    //   skin.vertices[thatIndex]   → an index into model_.vertices
    // A submesh names a contiguous run of both. We flatten each submesh into
    // its own mesh with indices rebased to zero, because MeshData is one
    // vertex array per mesh and the renderer's geoset upload assumes that.
    for (std::size_t s = 0; s < skin.submeshes.size(); ++s) {
        const auto& sec = skin.submeshes[s];
        if (sec.vertexCount == 0 || sec.indexCount == 0)
            continue;

        const std::size_t vBegin = sec.vertexStart;
        const std::size_t vEnd = vBegin + sec.vertexCount;
        const std::size_t iBegin = sec.indexStart;
        const std::size_t iEnd = iBegin + sec.indexCount;
        if (vEnd > skin.vertices.size() || iEnd > skin.indices.size())
            continue; // truncated skin; skip rather than read out of bounds

        MeshData mesh;
        mesh.geosetId = static_cast<i32>(s);
        mesh.materialId = -1; // no materials in this phase; UnlitShading draws it
        mesh.lod = 0;
        // A gather, not a slice: the skin indirection means a submesh's
        // vertices are scattered through the global array. Still verbatim —
        // what moves is whole 48-byte records, never a decoded attribute.
        mesh.positions.reserve(sec.vertexCount);
        mesh.baked.stride = sizeof(M2Vertex);
        mesh.baked.attributes = DescribeM2Vertex();
        mesh.baked.data.resize(sec.vertexCount * sizeof(M2Vertex));
        u8* dst = mesh.baked.data.data();
        for (std::size_t v = vBegin; v < vEnd; ++v, dst += sizeof(M2Vertex)) {
            const std::size_t gv = skin.vertices[v];
            if (gv >= model_.vertices.size()) {
                // Out-of-range index: a zeroed record, matching the zeroed
                // position the CPU copy gets. Degenerate, but in-bounds.
                std::memset(dst, 0, sizeof(M2Vertex));
                mesh.positions.push_back({0.0f, 0.0f, 0.0f});
                continue;
            }
            const M2Vertex& src = model_.vertices[gv];
            std::memcpy(dst, &src, sizeof(M2Vertex));
            mesh.positions.push_back(src.position);
        }

        mesh.indices.reserve(sec.indexCount);
        for (std::size_t i = iBegin; i < iEnd; ++i) {
            const std::size_t local = skin.indices[i];
            // skin.indices is profile-global; rebase into this submesh.
            mesh.indices.push_back(static_cast<u32>(local - vBegin));
        }
        out.push_back(std::move(mesh));
    }
    return out;
}

std::vector<TextureData> M2ModelAdapter::GetTextures() {
    std::vector<TextureData> out;
    out.reserve(model_.textures.size());
    for (usize i = 0; i < model_.textures.size(); ++i) {
        const auto& tex = model_.textures[i];
        TextureData td;
        td.textureId = static_cast<i32>(i);
        // Zero, not the M2 texture *type*: replaceableId is WC3's
        // team-colour/glow slot space, nothing maps the two, and AddModel reads
        // *any* non-zero value as "hand this slot to the replaceable manager",
        // which then owns the binding. M2's own customisation slots are carried
        // by the empty sharedKey below instead.
        td.replaceableId = 0;
        // Bit 0 wrap-U, bit 1 wrap-V — the same encoding StagedTexture uses.
        td.wrapFlags = tex.flags & 0x3u;

        if (!tex.filename.empty()) {
            td.sharedKey = tex.filename;
        } else if (i < model_.texture_ids.size() && model_.texture_ids[i] != 0) {
            // Chunked models name their textures by fileDataID in TXID and
            // leave `filename` a lone NUL. `#<id>` is ContentRef::Describe's
            // own spelling, which UploadStagedTextures reverses.
            td.sharedKey = "#" + std::to_string(model_.texture_ids[i]);
        }
        // Anything left with an empty key is a customisation slot (type 1..26)
        // or a genuinely nameless texture; both bind the white default.
        out.push_back(std::move(td));
    }
    return out;
}

::whiteout::flakes::ModelBounds M2ModelAdapter::GetBounds() {
    ::whiteout::flakes::ModelBounds b;
    const auto& e = model_.bounding;
    const bool degenerate = e.maximum.x <= e.minimum.x && e.maximum.y <= e.minimum.y &&
                            e.maximum.z <= e.minimum.z;
    if (!degenerate) {
        b.min = {e.minimum.x, e.minimum.y, e.minimum.z};
        b.max = {e.maximum.x, e.maximum.y, e.maximum.z};
        b.valid = true;
        return b;
    }
    // Fall through to the interface's union-over-positions default when the
    // model carries no usable box of its own.
    return IModelSource::GetBounds();
}

SkeletonData M2ModelAdapter::GetSkeleton() {
    SkeletonData sk;
    // Every `.m2` has at least one bone — the client asserts `data->bones.count
    // > 0` before it animates anything. Synthesising one for a model that
    // somehow has none keeps the invariant total, so the draw path never has to
    // handle a skinned geoset with no palette behind it.
    const usize boneCount = std::max<usize>(model_.bones.size(), 1);
    sk.nodeCount = static_cast<i32>(boneCount);
    sk.inverseBindMatrices.assign(boneCount, Matrix44f::identity());
    sk.nodePivots.assign(boneCount, Vector3f{0.0f, 0.0f, 0.0f});
    sk.nodeParents.assign(boneCount, -1);
    sk.billboardFlags.assign(boneCount, BONE_BILLBOARD_NONE);

    using ::whiteout::m2::BoneFlag;
    for (usize i = 0; i < model_.bones.size(); ++i) {
        const auto& b = model_.bones[i];
        sk.nodePivots[i] = b.pivot;
        sk.nodeParents[i] =
            (b.parentBoneId >= 0 && static_cast<usize>(b.parentBoneId) < model_.bones.size())
                ? static_cast<i32>(b.parentBoneId)
                : -1;
        // Recorded, not honoured: Evaluate does not billboard. The client does
        // it in *camera* space (a fixed basis, or the bone's local rotation with
        // its axes permuted) and PoseRequest carries a camera position but no
        // view matrix, so a screen-aligned basis is not expressible from what
        // Evaluate is handed. Publishing the flags keeps the data one step from
        // whoever closes that.
        u32 bb = BONE_BILLBOARD_NONE;
        if (hasFlag(static_cast<BoneFlag>(b.flags), BoneFlag::SphericalBillboard))
            bb = BONE_BILLBOARD_FULL;
        else if (hasFlag(static_cast<BoneFlag>(b.flags), BoneFlag::CylindricalBillboardX))
            bb = BONE_BILLBOARD_LOCK_X;
        else if (hasFlag(static_cast<BoneFlag>(b.flags), BoneFlag::CylindricalBillboardY))
            bb = BONE_BILLBOARD_LOCK_Y;
        else if (hasFlag(static_cast<BoneFlag>(b.flags), BoneFlag::CylindricalBillboardZ))
            bb = BONE_BILLBOARD_LOCK_Z;
        sk.billboardFlags[i] = bb;
    }
    return sk;
}

std::vector<SkinWeightData> M2ModelAdapter::GetSkinWeights() {
    std::vector<SkinWeightData> out;
    if (profileIndex_ >= model_.skinProfiles.size())
        return out;
    const auto& skin = model_.skinProfiles[profileIndex_];
    const i32 boneCount = static_cast<i32>(model_.bones.size());
    out.reserve(skin.submeshes.size());

    for (usize s = 0; s < skin.submeshes.size(); ++s) {
        const auto& sec = skin.submeshes[s];
        // Same skips GetMeshes takes, so geoset ids line up one for one.
        if (sec.vertexCount == 0 || sec.indexCount == 0)
            continue;
        const usize vBegin = sec.vertexStart;
        const usize vEnd = vBegin + sec.vertexCount;
        if (vEnd > skin.vertices.size())
            continue;

        SkinWeightData sw;
        sw.geosetId = static_cast<i32>(s);
        sw.influences.resize(sec.vertexCount);

        // Bone 0 always occupies slot 0, so a submesh whose vertices are all
        // unweighted still has a palette to point at.
        std::unordered_map<i32, i32> globalToLocal;
        sw.subsetNodeIndices.push_back(0);
        globalToLocal.emplace(0, 0);

        for (usize v = vBegin; v < vEnd; ++v) {
            VertexInfluence& inf = sw.influences[v - vBegin];
            const usize gv = skin.vertices[v];
            if (gv >= model_.vertices.size())
                continue; // zeroed record, matching GetMeshes
            const M2Vertex& src = model_.vertices[gv];
            for (i32 k = 0; k < 4; ++k) {
                const f32 w = static_cast<f32>(src.boneWeights[k]) * (1.0f / 255.0f);
                inf.weight[k] = w;
                if (w <= 0.0f)
                    continue;
                // The `.m2` vertex names its bones globally — what
                // CM2Model::TransformVerticesNoUVSelect_cpp indexes the bone
                // matrix array with directly. (The `.skin`'s own `bones` array
                // is the *section-local* twin the client's shader path
                // substitutes, resolved through boneCombos; either reaches the
                // same bone, and the global one needs no second table.)
                i32 g = static_cast<i32>(src.boneIndices[k]);
                if (g < 0 || g >= boneCount)
                    g = 0;
                auto [it, inserted] = globalToLocal.emplace(g, 0);
                if (inserted) {
                    it->second = static_cast<i32>(sw.subsetNodeIndices.size());
                    sw.subsetNodeIndices.push_back(g);
                }
                inf.boneIdx[k] = it->second;
            }
        }
        out.push_back(std::move(sw));
    }
    return out;
}

std::vector<u32> M2ModelAdapter::GetGlobalSequences() {
    std::vector<u32> out;
    out.reserve(model_.globalLoops.size());
    for (const auto& g : model_.globalLoops)
        out.push_back(g.timestamp);
    return out;
}

std::vector<SequenceInfo> M2ModelAdapter::GetSequences() const {
    std::vector<SequenceInfo> out;
    out.reserve(model_.sequences.size());
    for (const auto& seq : model_.sequences) {
        SequenceInfo s;
        const std::string_view name = M2AnimationName(seq.id);
        s.name = name.empty() ? ("Anim" + std::to_string(seq.id)) : std::string(name);
        // Variations share an id and are distinguished only by their index, so
        // the number has to be in the name or a host's list reads as duplicates.
        if (seq.variationIndex != 0)
            s.name += " (" + std::to_string(seq.variationIndex) + ")";
        // Each `.m2` sequence is its own timeline from zero — unlike MDX, where
        // every sequence is a window into one global one.
        s.startMs = 0;
        s.endMs = static_cast<i32>(seq.duration);
        s.moveSpeed = seq.movespeed;
        // `rarity` stays 0. M2's `frequency` is a selection *probability* and
        // MDX's rarity runs the other way, so feeding one to the other would
        // make a host's random pick prefer exactly the wrong variations.
        //
        // Bit 0, not WhiteoutLib's `SequenceFlag::Looping` (0x20): 0x20 is part
        // of the `flags & 0x130` mask that says where a sequence's *keys* live
        // and is set on Stand and Attack1H alike. What decides looping is bit 0
        // — `CM2Model::AnimateMTSimple` clamps the clock to the sequence's
        // window when it is set and wraps `now % duration` when it is not.
        s.nonLooping = (static_cast<u32>(seq.flags) & 0x1u) != 0;
        out.push_back(std::move(s));
    }
    if (out.empty()) {
        // A model with no sequence table still needs one entry: the actor's
        // clock advances against it, and global-sequence tracks run regardless.
        SequenceInfo s;
        s.name = "Stand";
        s.endMs = 1000;
        out.push_back(std::move(s));
    }
    return out;
}

renderer::model::FrameState M2ModelAdapter::Evaluate(const PoseRequest& req) const {
    renderer::model::FrameState fs;
    const ClipRef clip = req.PrimaryClip();

    // A lazily parsed model has not read this sequence's `.anim` yet. Asking
    // first keeps a model whose siblings are missing from re-reading them every
    // frame — sequenceKeysPending is false once a load has been tried.
    if (clip.sequence >= 0 &&
        ::whiteout::m2::sequenceKeysPending(model_, static_cast<u32>(clip.sequence))) {
        ::whiteout::m2::loadSequence(model_, static_cast<u32>(clip.sequence));
    }

    M2AnimTime at;
    at.sequence = clip.sequence;
    at.timeMs = clip.timeMs;
    at.globalTimeMs = (req.globalTimeMs >= 0) ? req.globalTimeMs : clip.timeMs;
    at.globalLoops = std::span<const u32>(globalLoops_);
    // `sequence < 0` is the bind pose: no clip, so every track answers with its
    // animref default and every local transform is identity. That reproduces the
    // stored vertex positions exactly, because an `.m2` stores them posed.
    const bool bindPose = clip.sequence < 0;

    EvaluateBones(at, bindPose, fs);
    EvaluateTextureTransforms(at, bindPose, fs);
    EvaluateSurfaces(at, bindPose, fs);
    return fs;
}

void M2ModelAdapter::EvaluateBones(const M2AnimTime& at, bool bindPose,
                                   renderer::model::FrameState& fs) const {
    using ::whiteout::m2::BoneFlag;
    const usize boneCount = std::max<usize>(model_.bones.size(), 1);
    fs.boneWorldMatrices.assign(boneCount, Matrix44f::identity());

    for (usize i = 0; i < model_.bones.size(); ++i) {
        const auto& b = model_.bones[i];
        const i32 parent = b.parentBoneId;
        // `parentIndex < boneIndex` is the client's own assertion, so one
        // forward pass suffices; a violation would read a matrix this pass has
        // not written, which the bounds test below turns into "no parent".
        const Matrix44f parentM =
            (parent >= 0 && static_cast<usize>(parent) < i)
                ? M2ParentFor(fs.boneWorldMatrices[static_cast<usize>(parent)], b.flags, b.pivot)
                : Matrix44f::identity();

        // Only a bone the file marks `Transformed` is sampled. That is the
        // client's own gate (`flags & 0x280`, of which 0x200 is the on-disk
        // bit); an unflagged bone simply inherits its parent's matrix, which is
        // what an identity local transform composes to anyway.
        if (bindPose || !hasFlag(static_cast<BoneFlag>(b.flags), BoneFlag::Transformed)) {
            fs.boneWorldMatrices[i] = parentM;
            continue;
        }

        const Vector3f t = SampleM2Vec3(b.translation, at, {0.0f, 0.0f, 0.0f});
        const Quaternion r = SampleM2Quat(b.rotation, at, Quaternion{0.0f, 0.0f, 0.0f, 1.0f});
        const Vector3f s = SampleM2Vec3(b.scale, at, {1.0f, 1.0f, 1.0f});
        fs.boneWorldMatrices[i] = M2BoneLocal(t, r, s, b.pivot) * parentM;
    }
}

void M2ModelAdapter::EvaluateTextureTransforms(const M2AnimTime& at, bool bindPose,
                                               renderer::model::FrameState& fs) const {
    fs.texAnimMatrices.reserve(model_.textureTransforms.size());
    for (usize i = 0; i < model_.textureTransforms.size(); ++i) {
        const auto& tt = model_.textureTransforms[i];
        Vector3f t{0.0f, 0.0f, 0.0f};
        Quaternion r{0.0f, 0.0f, 0.0f, 1.0f};
        Vector3f s{1.0f, 1.0f, 1.0f};
        if (!bindPose) {
            t = SampleM2Vec3(tt.translation, at, t);
            r = SampleM2Quat(tt.rotation, at, r);
            s = SampleM2Vec3(tt.scaling, at, s);
        }

        // CM2Model::AnimateTextureTransformMT composes, in order, a rotation
        // about (0.5, 0.5), a scale about the same pivot, and a translation —
        // all three *pre*-multiplied, which flattens to
        //
        //     uv' = ((uv + t - 0.5) · S · R) + 0.5
        //
        // the same shape WC3's texture animation takes, which is why the two
        // share FrameState::TexAnimMatrix. Only rows 0 and 1 matter: the
        // shader feeds (u, v, 0, 1) and reads .xy back, so the third column and
        // row can never reach the output.
        const Matrix44f rot = Matrix44f::rotation(r).transpose();
        const f32 a = s.x * rot.data[0][0];
        const f32 bb = s.y * rot.data[1][0];
        const f32 d = s.x * rot.data[0][1];
        const f32 e = s.y * rot.data[1][1];
        const f32 px = t.x - 0.5f;
        const f32 py = t.y - 0.5f;

        renderer::model::FrameState::TexAnimMatrix m{};
        m.textureAnimId = static_cast<i32>(i);
        m.row0[0] = a;
        m.row0[1] = bb;
        m.row0[2] = 0.0f;
        m.row0[3] = a * px + bb * py + 0.5f;
        m.row1[0] = d;
        m.row1[1] = e;
        m.row1[2] = 0.0f;
        m.row1[3] = d * px + e * py + 0.5f;
        fs.texAnimMatrices.push_back(m);
    }
}

void M2ModelAdapter::EvaluateSurfaces(const M2AnimTime& at, bool bindPose,
                                      renderer::model::FrameState& fs) const {
    // Nothing at the bind pose: an empty channel is what makes the draw path
    // read M2SurfaceTable's own constants, and those *are* each track's first
    // key. Filling this with animref defaults instead would quietly override
    // them with 1.0.
    if (bindPose || profileIndex_ >= model_.skinProfiles.size())
        return;
    const auto& skin = model_.skinProfiles[profileIndex_];
    fs.surfaceStates.reserve(skin.batches.size());

    // `surface` is the batch's index in this profile, which is exactly what
    // BuildM2SurfaceTable numbers its entries by. The two walk the same array in
    // the same order; that is the whole of the contract between them.
    for (usize i = 0; i < skin.batches.size(); ++i) {
        const auto& batch = skin.batches[i];
        renderer::model::FrameState::SurfaceState st;
        st.surface = static_cast<i32>(i);

        const u32 units = std::clamp<u32>(batch.textureCount, 1u, 4u);
        for (u32 u = 0; u < units; ++u) {
            const u64 ci = static_cast<u64>(batch.textureWeightComboIndex) + u;
            if (ci >= model_.textureWeightCombos.size())
                continue;
            const u32 w = model_.textureWeightCombos[ci];
            if (w >= model_.textureWeights.size())
                continue;
            st.unitWeights[u] = SampleM2Fixed16(model_.textureWeights[w].weight, at, 1.0f);
        }

        // Whole-element alpha takes unit 0's weight whatever the texture count;
        // the rest reach the shader as the per-unit float4 (batch flag 0x40).
        st.alpha = st.unitWeights[0];
        if (batch.colorIndex >= 0 && static_cast<usize>(batch.colorIndex) < model_.colors.size()) {
            const auto& c = model_.colors[static_cast<usize>(batch.colorIndex)];
            st.color = SampleM2Vec3(c.color, at, {1.0f, 1.0f, 1.0f});
            st.alpha *= SampleM2Fixed16(c.alpha, at, 1.0f);
        }
        st.alpha = std::clamp(st.alpha, 0.0f, 1.0f);
        fs.surfaceStates.push_back(st);
    }
}

} // namespace whiteout::flakes::io
