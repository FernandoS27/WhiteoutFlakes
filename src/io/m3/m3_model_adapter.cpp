#include "io/m3/m3_model_adapter.h"

#include "io/m3/m3_pose_solvers.h"
#if WDX_HAS_PHYSICS
#include "renderer/profiles/sc2_heroes/sc2_cloth.h"
#include "renderer/profiles/sc2_heroes/sc2_physics.h"
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <unordered_map>

namespace whiteout::flakes::io {

using renderer::model::MeshBuffer;
using renderer::model::MeshData;
using renderer::model::SequenceInfo;
using renderer::model::SkeletonData;
using renderer::model::SkinWeightData;
using renderer::model::VertexAttribute;
using renderer::model::VertexSemantic;

namespace {

// The `.m3` vertex record, described rather than decoded. Offsets mirror
// VertexBuffer::initialize() exactly — see whiteout/models/m3/types.cpp:
//
//   0  position      f32 x3
//   12 boneWeights   u8  x4  (/255)
//   16 boneIndices   u8  x4
//   20 normal        u8  x4  UNORM (v/255*2-1); .w is the bitangent handedness
//   24 colour        u8  x4  BGRA, only when the VertexColor flag is set
//   .. uv0..uv4      i16 x2  each, one per UV flag
//   -4 tangent       u8  x4  UNORM, always the last four bytes; .w unused (255)
//
// UNORM, not SNORM: retail declares both basis vectors `ubyte4n` and decodes
// them `2 * v - 1` (vsmodelvertexformat.fx TranslateVert). Read as i8/127 the
// same bytes are 0.73..1.41 long and point the wrong way — that was the
// "inverted .m3 normals" the shader used to negate.
//
// Deriving this a second time here rather than asking the parser is the one
// genuinely risky thing in the M3 path, which is why mesh_buffer_test asserts
// the description agrees with getPositions()/getNormals() byte for byte.
std::vector<VertexAttribute> DescribeM3Vertex(const ::whiteout::m3::VertexBuffer& vb) {
    const u16 stride = static_cast<u16>(vb.vertexSize());
    const u16 uvBase = vb.hasVertexColors() ? 28 : 24;

    std::vector<VertexAttribute> attrs;
    attrs.push_back({VertexSemantic::Position, 0, gfx::Format::R32G32B32_FLOAT, 0});
    attrs.push_back({VertexSemantic::BoneWeights, 0, gfx::Format::R8G8B8A8_UNORM, 12});
    attrs.push_back({VertexSemantic::BoneIndices, 0, gfx::Format::R8G8B8A8_UINT, 16});
    attrs.push_back({VertexSemantic::Normal, 0, gfx::Format::R8G8B8A8_UNORM, 20});
    if (vb.hasVertexColors())
        attrs.push_back({VertexSemantic::Color, 0, gfx::Format::R8G8B8A8_UNORM, 24});
    for (u8 i = 0; i < static_cast<u8>(vb.UVsNum()); ++i) {
        // Declared, but nothing consumes it yet: `.m3` UVs are i16/2048, and
        // SNORM decodes /32767. The scale — plus REGN v5+'s per-region
        // uvMultiply/uvOffset — is material work, so the honest description
        // here is the storage layout, not a usable texture coordinate.
        attrs.push_back({VertexSemantic::TexCoord, i, gfx::Format::R16G16_SNORM,
                         static_cast<u16>(uvBase + i * 4)});
    }
    attrs.push_back(
        {VertexSemantic::Tangent, 0, gfx::Format::R8G8B8A8_UNORM, static_cast<u16>(stride - 4)});
    return attrs;
}

// The local matrix, built exactly as M3Anim_EvaluateBoneTransform builds it:
// the quaternion's row-vector rotation matrix with row i scaled by scale[i],
// and the translation in row 3. Written out rather than routed through
// Matrix44f::rotation because that one is the column-vector form.
Matrix44f M3ComposeLocal(const Vector3f& t, const Quaternion& q, const Vector3f& s) {
    const f32 x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    const f32 xx = q.x * x2, xy = q.x * y2, xz = q.x * z2;
    const f32 yy = q.y * y2, yz = q.y * z2, zz = q.z * z2;
    const f32 wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;

    Matrix44f m = Matrix44f::identity();
    m.data[0][0] = (1.0f - yy - zz) * s.x;
    m.data[0][1] = (xy + wz) * s.x;
    m.data[0][2] = (xz - wy) * s.x;
    m.data[1][0] = (xy - wz) * s.y;
    m.data[1][1] = (1.0f - xx - zz) * s.y;
    m.data[1][2] = (yz + wx) * s.y;
    m.data[2][0] = (xz + wy) * s.z;
    m.data[2][1] = (yz - wx) * s.z;
    m.data[2][2] = (1.0f - xx - yy) * s.z;
    m.data[3][0] = t.x;
    m.data[3][1] = t.y;
    m.data[3][2] = t.z;
    return m;
}

// ---- per-type block access and interpolation -------------------------------

const ::whiteout::m3::AnimBlock<Vector3f>* BlockOf(const ::whiteout::m3::SubTrackContainer& stc,
                                                   M3TrackHandle h, const Vector3f*) {
    return h.slot == M3SdSlot::Vec3 ? &stc.sd3v[h.block] : nullptr;
}
const ::whiteout::m3::AnimBlock<Quaternion>* BlockOf(const ::whiteout::m3::SubTrackContainer& stc,
                                                     M3TrackHandle h, const Quaternion*) {
    return h.slot == M3SdSlot::Quat ? &stc.sd4q[h.block] : nullptr;
}
const ::whiteout::m3::AnimBlock<f32>* BlockOf(const ::whiteout::m3::SubTrackContainer& stc,
                                              M3TrackHandle h, const f32*) {
    return h.slot == M3SdSlot::Float ? &stc.sdr3[h.block] : nullptr;
}

Vector3f MixValue(const Vector3f& a, const Vector3f& b, f32 t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
// Track-level: the raw componentwise lerp the engine uses (see m3_animation.h).
Quaternion MixTrack(const Quaternion& a, const Quaternion& b, f32 t) {
    return M3LerpQuatRaw(a, b, t);
}
Vector3f MixTrack(const Vector3f& a, const Vector3f& b, f32 t) {
    return MixValue(a, b, t);
}
f32 MixTrack(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}
// Cross-layer: rotations really do slerp when several layers combine.
Quaternion MixLayers(const Quaternion& a, const Quaternion& b, f32 t) {
    return M3SlerpQuat(a, b, t);
}
Vector3f MixLayers(const Vector3f& a, const Vector3f& b, f32 t) {
    return MixValue(a, b, t);
}
f32 MixLayers(f32 a, f32 b, f32 t) {
    return a + (b - a) * t;
}

} // namespace

template <typename T>
T M3ModelAdapter::SampleRef(const ::whiteout::m3::AnimRef<T>& ref,
                            std::span<const M3Layer> layers) const {
    // An unbound reference is a constant, and its own init value is the
    // constant. Both sentinels appear in shipped data: WhiteoutLib documents
    // 0, the runtime tests for 0xFFFFFFFF.
    if (ref.animId == 0 || ref.animId == 0xFFFFFFFFu || layers.empty())
        return ref.initValue;

    const i32 row = tables_.RowOf(ref.animId);
    if (row < 0)
        return ref.initValue;

    // interpType 0 and the runtime's step bit say the same thing; the file
    // keeps both, the engine keeps only the second.
    const bool interpolate = ref.interpType != 0 && (ref.flags & 0x10u) == 0;

    struct Contribution {
        T value;
        f32 w;
    };
    Contribution contribs[16];
    int count = 0;
    f32 budget = kM3StartBudget;
    ::whiteout::u64 visited = 0;

    for (const M3Layer& l : layers) {
        if (l.play < 64 && ((visited >> l.play) & 1u) != 0)
            continue; // this play already contributed

        const M3TrackHandle h = tables_.At(row, l.stc);
        // Global index: the container may live in the model or in any attached
        // `.m3a`, so it is resolved through the tables, never by indexing
        // `model_`.
        const ::whiteout::m3::SubTrackContainer* coll = tables_.StcAt(l.stc);
        const ::whiteout::m3::AnimBlock<T>* blk =
            (h.Valid() && coll) ? BlockOf(*coll, h, static_cast<const T*>(nullptr)) : nullptr;

        // A transparent layer with no track abstains entirely: it neither
        // contributes nor spends budget, so lower layers show through. This is
        // the whole of split body.
        if (!blk && l.transparent)
            continue;

        // Past that filter the bracket is claimed either way — an opaque
        // default-fill counts, verified at the stamp site in
        // M3Anim_BlendVec3_Weighted.
        if (l.play < 64)
            visited |= (::whiteout::u64{1} << l.play);

        T value;
        if (blk && !blk->keys.empty()) {
            const M3KeySpan sp = M3LocateKey(blk->timestamps, l.timeMs, l.loop, interpolate);
            if (!sp.valid) {
                // A bound but unusable track spends its weight and contributes
                // nothing — reproduced, not repaired.
                budget -= l.weight;
                if (budget <= kM3BudgetEpsilon)
                    break;
                continue;
            }
            const std::size_t last = blk->keys.size() - 1;
            const std::size_t i0 = (std::min)(sp.i0, last);
            const std::size_t i1 = (std::min)(sp.i1, last);
            value = (i0 == i1) ? blk->keys[i0] : MixTrack(blk->keys[i0], blk->keys[i1], sp.frac);
        } else {
            // Opaque layer with no track: force the property's default, which
            // pulls the channel back toward bind pose at full weight.
            value = ref.initValue;
        }

        if (count < 16)
            contribs[count++] = {value, l.weight};
        budget -= l.weight;
        if (budget <= kM3BudgetEpsilon)
            break;
    }

    if (count == 0)
        return ref.initValue;
    if (count == 1 || budget > kM3SettledBudget)
        return contribs[0].value;

    // Overspend is absorbed by the lowest-priority contributor so the weights
    // sum to exactly one.
    if (budget < 0.0f)
        contribs[count - 1].w += budget;

    // Combine lowest priority first, each step smoothstepped against the
    // weight accumulated so far.
    T out = contribs[count - 1].value;
    f32 acc = contribs[count - 1].w;
    for (int i = count - 2; i >= 0; --i) {
        out = MixLayers(out, contribs[i].value, M3SmoothstepFactor(acc, contribs[i].w));
        acc += contribs[i].w;
    }
    return out;
}

::whiteout::u32 M3ModelAdapter::SampleRefOverride(const ::whiteout::m3::AnimRef<::whiteout::u32>& ref,
                                                  std::span<const M3Layer> layers) const {
    // Override mode: every contributor enters at a flat weight of 1.0, so the
    // first one past the transparent/opaque filter wins outright and nothing is
    // blended. Discrete channels have no meaningful midpoint — a bone is not
    // 40% visible — which is why the engine samples them through
    // M3Anim_BlendBool32_Override_Flags rather than the weighted worker.
    if (ref.animId == 0 || ref.animId == 0xFFFFFFFFu || layers.empty())
        return ref.initValue;

    const i32 row = tables_.RowOf(ref.animId);
    if (row < 0)
        return ref.initValue;

    ::whiteout::u64 visited = 0;
    for (const M3Layer& l : layers) {
        if (l.play < 64 && ((visited >> l.play) & 1u) != 0)
            continue;
        const M3TrackHandle h = tables_.At(row, l.stc);
        const ::whiteout::m3::SubTrackContainer* collPtr = tables_.StcAt(l.stc);
        if (!collPtr)
            continue;
        const auto& coll = *collPtr;

        // **A discrete u32 channel lands in `SDFG`, not `SDU3`.** Both hold
        // four-byte keys and the engine reaches them through one generic
        // indexer, so the file is free to pick either — and it always picks
        // slot 11: measured over the 54724-model corpus, 30617 keyed bone
        // visibilities and all 598 keyed `PHRB.dynamicState` resolve there, and
        // not one to slot 10. Accepting only the u32 slot compiles, runs, and
        // silently answers `initValue` for every keyed channel in shipped
        // content, which renders as an animation that simply has no visibility
        // track.
        const bool isFlag = h.Valid() && h.slot == M3SdSlot::Flag &&
                            h.block < coll.sdfg.size() && !coll.sdfg[h.block].keys.empty();
        const bool isU32 = h.Valid() && h.slot == M3SdSlot::U32 && h.block < coll.sdu3.size() &&
                           !coll.sdu3[h.block].keys.empty();
        if (!isFlag && !isU32 && l.transparent)
            continue;
        if (l.play < 64)
            visited |= (::whiteout::u64{1} << l.play);
        if (!isFlag && !isU32)
            return ref.initValue;

        // Discrete: never interpolated, whatever the ref's interp type says.
        const std::vector<::whiteout::i32>& stamps =
            isFlag ? coll.sdfg[h.block].timestamps : coll.sdu3[h.block].timestamps;
        const M3KeySpan sp = M3LocateKey(stamps, l.timeMs, l.loop, /*interpolate*/ false);
        if (!sp.valid)
            return ref.initValue;
        if (isFlag) {
            const auto& keys = coll.sdfg[h.block].keys;
            return keys[(std::min)(sp.i0, keys.size() - 1)].value;
        }
        const auto& keys = coll.sdu3[h.block].keys;
        return keys[(std::min)(sp.i0, keys.size() - 1)];
    }
    return ref.initValue;
}

std::shared_ptr<M3ModelAdapter> M3ModelAdapter::Load(const ContentRef& ref,
                                                     std::span<const ::whiteout::u8> bytes) {
    if (bytes.empty())
        return nullptr;
    ::whiteout::m3::Parser parser;
    ::whiteout::m3::Model model;
    try {
        model = parser.parse(bytes);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[m3] parse failed for '%s': %s\n", ref.Describe().c_str(), e.what());
        return nullptr;
    }
    if (parser.hasIssues()) {
        // Issues are not necessarily fatal — the parser reports what it
        // skipped. Surface them rather than letting a half-read model look
        // like a clean one.
        for (const auto& issue : parser.getIssues())
            std::fprintf(stderr, "[m3] %s\n", issue.c_str());
    }
    if (model.divisions.empty() || model.vertices.vertexCount() == 0) {
        // Real and common: `.m3` is also the container for effect-only and
        // physics-only assets, which carry no drawable mesh at all. Not an
        // error, but there is nothing for this adapter to return.
        std::fprintf(stderr, "[m3] no geometry in '%s'\n", ref.Describe().c_str());
        return nullptr;
    }
    return std::make_shared<M3ModelAdapter>(std::move(model));
}

M3ModelAdapter::M3ModelAdapter(::whiteout::m3::Model model) : model_(std::move(model)) {
    // Division 0 is the highest detail level. LOD selection is a later phase;
    // taking one and saying so beats taking whichever happens to be first
    // without noticing there were others.
    divisionIndex_ = 0;
    if (divisionIndex_ < model_.divisions.size())
        regionCount_ = model_.divisions[divisionIndex_].regions.size();
    BuildEmittedRegions();
    RebuildAnimationTables();
#if WDX_HAS_PHYSICS
    if (auto build = renderer::profiles::sc2_heroes::Sc2BuildCloth(model_); !build.pieces.empty()) {
        cloth_ = std::make_shared<const renderer::profiles::sc2_heroes::Sc2ClothBuild>(
            std::move(build));
    }
#endif
    BuildClothGeosetMap();
}

void M3ModelAdapter::RebuildAnimationTables() {
    std::vector<const ::whiteout::m3::Model*> attached;
    attached.reserve(animModels_.size());
    for (const auto& m : animModels_)
        attached.push_back(m.get());
    tables_.Build(model_, attached);

    // Restate where each file's sequences landed, so a host can label them
    // without re-deriving the layout.
    std::size_t next = model_.sequences.size();
    for (std::size_t i = 0; i < attached_.size(); ++i) {
        attached_[i].firstSequence = next;
        attached_[i].sequenceCount = animModels_[i]->sequences.size();
        next += attached_[i].sequenceCount;
    }
}

bool M3ModelAdapter::AttachAnimationFile(std::string label,
                                         std::span<const ::whiteout::u8> bytes) {
    if (bytes.empty())
        return false;
    for (const auto& a : attached_) {
        if (a.label == label) {
            // Same rejection StarCraft II makes — it dedupes on the stored path
            // before adding a record — but two mods' files can share a stem, so
            // say which one was refused rather than failing mute.
            std::fprintf(stderr, "[m3a] '%s' is already attached — ignored\n", label.c_str());
            return false;
        }
    }

    // An `.m3a` is an ordinary MD34 model — same MODL versions, same chunk
    // layout — that happens to carry bones and no vertices. Measured over all
    // 1110 shipped files: every one has BONE, none has vertex data. So it goes
    // through the same parser, and only `Load`'s drawable-geometry check has to
    // be skipped.
    ::whiteout::m3::Model anim;
    ::whiteout::m3::Parser parser;
    try {
        anim = parser.parse(bytes);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[m3a] parse failed for '%s': %s\n", label.c_str(), e.what());
        return false;
    }
    if (anim.sequences.empty()) {
        std::fprintf(stderr, "[m3a] '%s' carries no sequences — not attached\n", label.c_str());
        return false;
    }

    animModels_.push_back(std::make_unique<::whiteout::m3::Model>(std::move(anim)));
    AttachedAnimation info;
    info.label = std::move(label);
    attached_.push_back(std::move(info));
    RebuildAnimationTables();
    return true;
}

bool M3ModelAdapter::DetachAnimationFile(std::size_t index) {
    if (index >= animModels_.size())
        return false;
    animModels_.erase(animModels_.begin() + static_cast<std::ptrdiff_t>(index));
    attached_.erase(attached_.begin() + static_cast<std::ptrdiff_t>(index));
    RebuildAnimationTables();
    return true;
}

void M3ModelAdapter::ClearAnimationFiles() {
    if (animModels_.empty())
        return;
    animModels_.clear();
    attached_.clear();
    RebuildAnimationTables();
}

void M3ModelAdapter::BuildClothGeosetMap() {
    geosetClothPiece_.assign(emittedRegions_.size(), -1);
    geosetClothProxy_.assign(emittedRegions_.size(), 0);
#if WDX_HAS_PHYSICS
    if (!cloth_)
        return;
    for (std::size_t p = 0; p < cloth_->pieces.size(); ++p) {
        const auto& piece = cloth_->pieces[p];
        for (std::size_t g = 0; g < emittedRegions_.size(); ++g) {
            if (emittedRegions_[g] == piece.simRegion)
                geosetClothProxy_[g] = 1;
            for (std::size_t r : piece.influencedRegions) {
                if (emittedRegions_[g] == r)
                    geosetClothPiece_[g] = static_cast<i32>(p);
            }
        }
    }
#endif
}

void M3ModelAdapter::BuildEmittedRegions() {
    emittedRegions_.clear();
    geosetRegionFlags_.clear();
    if (divisionIndex_ >= model_.divisions.size())
        return;
    const auto& div = model_.divisions[divisionIndex_];
    const std::size_t vertexCount = model_.vertices.vertexCount();
    const std::size_t stride = model_.vertices.vertexSize();
    const std::size_t blobSize = model_.vertices.data.size();

    for (std::size_t r = 0; r < div.regions.size(); ++r) {
        const auto& region = div.regions[r];
        if (region.vertexCount == 0 || region.indexCount == 0)
            continue;
        const std::size_t vEnd = static_cast<std::size_t>(region.firstVertex) + region.vertexCount;
        const std::size_t iEnd = static_cast<std::size_t>(region.firstIndex) + region.indexCount;
        if (vEnd > vertexCount || iEnd > div.faces.size())
            continue;
        if (vEnd * stride > blobSize)
            continue;
        emittedRegions_.push_back(r);
        geosetRegionFlags_.push_back(static_cast<::whiteout::u32>(region.flags));
    }
}

std::string M3CleanPath(const std::string& raw) {
    // The Ref<CHAR> keeps its terminator: size() is one past the text, and the
    // embedded NUL survives every string operation silently. c_str() is the
    // one honest exit.
    return std::string(raw.c_str());
}

bool M3LayerHasTexture(const ::whiteout::m3::TextureLayer& layer) {
    if ((static_cast<u32>(layer.flags) & static_cast<u32>(::whiteout::m3::TextureLayerFlag::Color)) != 0)
        return false;
    return !M3CleanPath(layer.texturePath).empty();
}

bool M3LayerActive(const ::whiteout::m3::TextureLayer& layer) {
    if ((static_cast<u32>(layer.flags) & static_cast<u32>(::whiteout::m3::TextureLayerFlag::Color)) != 0)
        return true;
    return M3LayerHasTexture(layer);
}

const ::whiteout::m3::TextureLayer* M3LayerForSlot(const ::whiteout::m3::StandardMaterial& mat,
                                                   M3LayerSlot slot) {
    auto get = [](const std::optional<::whiteout::m3::TextureLayer>& l)
        -> const ::whiteout::m3::TextureLayer* { return l ? &*l : nullptr; };
    switch (slot) {
    case M3LayerSlot::Diffuse:
        return get(mat.diffuseLayer);
    case M3LayerSlot::Decal:
        return get(mat.decalLayer);
    case M3LayerSlot::Specular:
        return get(mat.specularLayer);
    case M3LayerSlot::Emissive:
        return get(mat.emissiveLayer1);
    case M3LayerSlot::Emissive2:
        return get(mat.emissiveLayer2);
    case M3LayerSlot::Normal:
        return get(mat.normalLayer);
    case M3LayerSlot::AlphaMask:
        return get(mat.alphaLayer1);
    case M3LayerSlot::Gloss:
        return get(mat.glossLayer);
    case M3LayerSlot::AlphaMask2:
        return get(mat.alphaLayer2);
    case M3LayerSlot::Environment:
        return get(mat.environmentLayer);
    case M3LayerSlot::EnvironmentMask:
        return get(mat.environmentMaskLayer);
    default:
        return nullptr;
    }
}

std::vector<M3TextureRef> CollectM3Textures(const ::whiteout::m3::Model& model) {
    std::vector<M3TextureRef> out;
    std::unordered_map<std::string, std::size_t> seen; // lowercase key -> index
    for (const auto& mat : model.standardMaterials) {
        for (u32 s = 0; s < static_cast<u32>(M3LayerSlot::Count); ++s) {
            const auto* layer = M3LayerForSlot(mat, static_cast<M3LayerSlot>(s));
            if (!layer || !M3LayerHasTexture(*layer))
                continue;
            const std::string path = M3CleanPath(layer->texturePath);
            const bool cube = static_cast<M3LayerSlot>(s) == M3LayerSlot::Environment;
            // Cube-ness joins the key: the same file wanted both ways is two
            // GPU textures, and one view cannot answer both bindings.
            std::string key = (cube ? "cube:" : "") + path;
            std::transform(key.begin(), key.end(), key.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (seen.contains(key))
                continue;
            seen.emplace(std::move(key), out.size());
            using ::whiteout::m3::TextureLayerFlag;
            const u32 f = static_cast<u32>(layer->flags);
            M3TextureRef ref;
            ref.path = path;
            ref.wrapFlags = ((f & static_cast<u32>(TextureLayerFlag::UVWrapX)) ? 0x1u : 0u) |
                            ((f & static_cast<u32>(TextureLayerFlag::UVWrapY)) ? 0x2u : 0u);
            ref.cube = cube;
            out.push_back(std::move(ref));
        }
    }
    return out;
}

std::vector<renderer::model::TextureData> M3ModelAdapter::GetTextures() {
    const std::vector<M3TextureRef> refs = CollectM3Textures(model_);
    std::vector<renderer::model::TextureData> out;
    out.reserve(refs.size());
    for (std::size_t i = 0; i < refs.size(); ++i) {
        renderer::model::TextureData td;
        td.textureId = static_cast<i32>(i);
        td.replaceableId = 0;
        td.width = 0;
        td.height = 0;
        td.wrapFlags = refs[i].wrapFlags;
        td.cubeMap = refs[i].cube;
        td.sharedKey = refs[i].path;
        out.push_back(std::move(td));
    }
    return out;
}

std::vector<MeshData> M3ModelAdapter::GetMeshes() {
    std::vector<MeshData> out;
    if (divisionIndex_ >= model_.divisions.size())
        return out;
    const auto& div = model_.divisions[divisionIndex_];

    // getPositions() decodes the whole blob, so it is called once rather than
    // per region — the stride walk is the expensive part and it is identical
    // for every region. Still needed with the baked path: `positions` is the
    // CPU-side copy GetBounds and the sort centroid read.
    const std::vector<::whiteout::Vector3f> positions = model_.vertices.getPositions();

    // The blob is model-global and a region is a contiguous vertex range, so
    // each region's buffer is a stride-aligned slice of it. No decode, no
    // repack — the bytes reaching the GPU are the bytes that were in the file.
    const std::vector<VertexAttribute> attrs = DescribeM3Vertex(model_.vertices);
    const std::size_t stride = model_.vertices.vertexSize();
    const std::vector<u8>& blob = model_.vertices.data;

    // Regions, in file order — see the header for why regions rather than
    // batches, and for the REGN v2 gap this counts. Which regions survive is
    // decided once in BuildEmittedRegions so every per-geoset accessor agrees
    // on what `geosetId` means.
    std::size_t noFaceRange = 0;
    for (const auto& region : div.regions)
        if (region.vertexCount != 0 && region.indexCount == 0)
            ++noFaceRange;

    out.reserve(emittedRegions_.size());
    for (std::size_t g = 0; g < emittedRegions_.size(); ++g) {
        const auto& region = div.regions[emittedRegions_[g]];

        const std::size_t vBegin = region.firstVertex;
        const std::size_t vEnd = vBegin + region.vertexCount;
        const std::size_t iBegin = region.firstIndex;
        const std::size_t iEnd = iBegin + region.indexCount;

        MeshData mesh;
        mesh.geosetId = static_cast<i32>(g);
        // The MATM index of the first batch naming this region. Multiple
        // batches can name one region with different materials (multi-pass);
        // the simple material system draws the first and only the first.
        mesh.materialId = -1;
        for (const auto& batch : div.batches) {
            if (batch.regionIndex == emittedRegions_[g]) {
                if (batch.materialIndex < model_.materialMaps.size())
                    mesh.materialId = static_cast<i32>(batch.materialIndex);
                break;
            }
        }
        mesh.lod = 0;
        mesh.positions.assign(positions.begin() + static_cast<std::ptrdiff_t>(vBegin),
                              positions.begin() + static_cast<std::ptrdiff_t>(vEnd));

        const std::size_t byteBegin = vBegin * stride;
        const std::size_t byteEnd = vEnd * stride;
        if (byteEnd > blob.size())
            continue; // vertexCount disagrees with the blob; skip the region
        mesh.baked.stride = static_cast<u32>(stride);
        mesh.baked.attributes = attrs;
        mesh.baked.data.assign(blob.begin() + static_cast<std::ptrdiff_t>(byteBegin),
                               blob.begin() + static_cast<std::ptrdiff_t>(byteEnd));

        mesh.indices.reserve(region.indexCount);
        for (std::size_t i = iBegin; i < iEnd; ++i)
            mesh.indices.push_back(static_cast<u32>(div.faces[i]));
        RewriteClothSkin(g, mesh);
        out.push_back(std::move(mesh));
    }
    if (noFaceRange > 0) {
        // One line, not one per region: a portrait model has six of these and
        // the cause is the same for all of them.
        std::fprintf(stderr,
                     "[m3] '%s': %zu of %zu regions carry no face range (REGN v%d — the parser "
                     "reads firstIndex/indexCount only from v3)\n",
                     model_.name.c_str(), noFaceRange, div.regions.size(),
                     div.regions.empty() ? -1 : div.regions[0].getVersion());
    }
    return out;
}

void M3ModelAdapter::RewriteClothSkin([[maybe_unused]] std::size_t geoset,
                                      [[maybe_unused]] MeshData& mesh) const {
#if WDX_HAS_PHYSICS
    if (!cloth_ || geoset >= geosetClothPiece_.size() || geosetClothPiece_[geoset] < 0)
        return;
    const auto& piece = cloth_->pieces[static_cast<std::size_t>(geosetClothPiece_[geoset])];
    const auto& c = model_.clothPhysics[piece.chunkIndex];
    const std::size_t region = emittedRegions_[geoset];

    const ::whiteout::m3::ClothProxy* proxy = nullptr;
    for (const auto& p : c.proxies) {
        if (p.proxyIndex == region && p.clothIndex == piece.simRegion)
            proxy = &p;
    }
    const std::size_t stride = mesh.baked.stride;
    const std::size_t count = mesh.baked.data.size() / (stride != 0 ? stride : 1);
    if (proxy == nullptr || stride < 20 || proxy->proxyVertices.size() != count ||
        proxy->proxyWeights.size() != count) {
        return;
    }

    // Offsets 12 and 16 are `BoneWeights` and `BoneIndices` — see
    // `DescribeM3Vertex`. Overwriting them in place is the whole rewrite: the
    // geoset's palette becomes the cloth's particles (`GetSkinWeights`), and a
    // vertex that named a bone now names the particle that carries it.
    for (std::size_t v = 0; v < count; ++v) {
        const ::whiteout::u64 slots = proxy->proxyVertices[v];
        const ::whiteout::u32 weights = proxy->proxyWeights[v];
        ::whiteout::u8* rec = mesh.baked.data.data() + v * stride;
        for (int k = 0; k < 4; ++k) {
            const auto src = static_cast<std::size_t>((slots >> (16 * k)) & 0xFFFFu);
            ::whiteout::u8 w = static_cast<::whiteout::u8>((weights >> (8 * k)) & 0xFFu);
            // 0xFFFF is the format's "no influence"; so is a source vertex the
            // cloth build dropped. Both become slot 0 at weight 0, which is what
            // the solver's own absent-anchor lanes do.
            const ::whiteout::i16 particle =
                (src < piece.oldToNew.size()) ? piece.oldToNew[src] : ::whiteout::i16{-1};
            const bool live = particle >= 0 &&
                              static_cast<std::size_t>(particle) < piece.particleCount;
            if (!live)
                w = 0;
            rec[16 + k] = live ? static_cast<::whiteout::u8>(particle) : 0u;
            rec[12 + k] = w;
        }
    }
#endif
}

::whiteout::flakes::ModelBounds M3ModelAdapter::GetBounds() {
    ::whiteout::flakes::ModelBounds b;
    const auto& e = model_.bounds;
    const bool degenerate = e.max.x <= e.min.x && e.max.y <= e.min.y && e.max.z <= e.min.z;
    if (!degenerate) {
        b.min = {e.min.x, e.min.y, e.min.z};
        b.max = {e.max.x, e.max.y, e.max.z};
        b.valid = true;
        return b;
    }
    // Fall through to the interface's union-over-positions default when the
    // model carries no usable box of its own. Common in practice: MODL.bounds
    // is the *animated* extent and a model with no sequences leaves it zeroed.
    return IModelSource::GetBounds();
}

namespace {

/// @brief What an `SDEV` key's name means.
///
/// Decided by **name**, not by `Event::eventType`. The census over 3607 corpus
/// models says the type is not a discriminator: `Evt_Sound` ships as type 2 and
/// as type 65283, `Evt_Simulate` as 2, 15619 and 65283. Only `Evt_SeqEnd` is
/// consistently type 4, and it is the one that needs no decoding.
///
/// The whole shipped vocabulary is those three names over 7092 keys. That is
/// not a sample — it is every event StarCraft II and Heroes author into a
/// model.
enum class M3EventKind { Sound, SequenceEnd, Simulate, Unknown };

M3EventKind M3DecodeEventKind(const std::string& rawName) {
    // The `Ref<CHAR>` keeps the terminator, so `name.size()` is one past the
    // text and a plain `==` against a literal never matches. Silently: the
    // strings print identically. Every comparison against an M3 name has to
    // go through the c_str() round-trip.
    const std::string name(rawName.c_str());
    if (name == "Evt_Sound")
        return M3EventKind::Sound;
    if (name == "Evt_SeqEnd")
        return M3EventKind::SequenceEnd;
    if (name == "Evt_Simulate")
        return M3EventKind::Simulate;
    return M3EventKind::Unknown;
}

} // namespace

std::vector<EventObjectConfig> M3ModelAdapter::GetEventObjects() {
    std::vector<EventObjectConfig> out;
    if (tables_.SequenceCount() == 0)
        return out;

    // One config per (sequence, kind, id, bone). `EventObjectConfig` carries a
    // single payload and a list of times, so distinct payloads have to become
    // distinct configs — a container firing two different sounds is two.
    struct Key {
        i32 sequence;
        i32 bone;
        std::string id;
        bool operator==(const Key&) const = default;
    };
    std::vector<std::pair<Key, std::vector<::whiteout::u32>>> groups;

    bool warnedUnknown = false;
    for (std::size_t s = 0; s < tables_.SequenceCount(); ++s) {
        // The model's own sequences only. An event key carries a bone *index*,
        // and an attached `.m3a`'s indices are in its own bone array — a
        // differently-ordered subset that can name bones the model does not
        // have — so anchoring one against this skeleton would fire the cue on
        // whatever bone happened to land at that index. Tracks bind by animId
        // and survive the merge; bone indices do not, so these are dropped
        // rather than mis-anchored. (Moot today: the M3 spawn path never asks
        // for event configs, and it reads them once at spawn either way.)
        if (tables_.AssetOfSequence(static_cast<i32>(s)) != 0)
            continue;
        for (const auto& def : tables_.LayersFor(static_cast<i32>(s))) {
            const ::whiteout::m3::SubTrackContainer* stcPtr = tables_.StcAt(def.stc);
            if (!stcPtr)
                continue;
            const auto& stc = *stcPtr;
            for (const auto& blk : stc.sdev) {
                for (std::size_t ki = 0; ki < blk.keys.size(); ++ki) {
                    const auto& ev = blk.keys[ki];
                    const M3EventKind kind = M3DecodeEventKind(ev.name);
                    if (kind == M3EventKind::Unknown) {
                        if (!warnedUnknown) {
                            std::fprintf(stderr,
                                         "[m3 events] unhandled event name '%s' in %s — ignored\n",
                                         std::string(ev.name.c_str()).c_str(),
                                         std::string(model_.name.c_str()).c_str());
                            warnedUnknown = true;
                        }
                        continue;
                    }
                    // Recognised and deliberately not dispatched:
                    //   Evt_SeqEnd  — a playback marker on bone 0xFFFF at the
                    //                 track's last frame. The playlist already
                    //                 knows where a sequence ends.
                    //   Evt_Simulate — the ragdoll hand-off (it appears in
                    //                 *DeathRagdoll / *PhysicsDeath models on a
                    //                 real bone). Nothing consumes it until
                    //                 physics lands; emitting it now would only
                    //                 add configs the pool skips.
                    if (kind != M3EventKind::Sound)
                        continue;
                    if (ki >= blk.timestamps.size())
                        continue;

                    const Key k{static_cast<i32>(s),
                                ev.boneIndex == 0xFFFFu ? -1 : static_cast<i32>(ev.boneIndex),
                                std::string(ev.optionString.c_str())};
                    auto it = std::find_if(groups.begin(), groups.end(),
                                           [&](const auto& g) { return g.first == k; });
                    if (it == groups.end()) {
                        groups.push_back({k, {}});
                        it = groups.end() - 1;
                    }
                    it->second.push_back(static_cast<::whiteout::u32>(blk.timestamps[ki]));
                }
            }
        }
    }

    out.reserve(groups.size());
    for (auto& [k, times] : groups) {
        std::sort(times.begin(), times.end());
        times.erase(std::unique(times.begin(), times.end()), times.end());
        EventObjectConfig c;
        c.name = "Evt_Sound";
        c.kind = EventObjectConfig::Kind::SND;
        // The option string is the cue name ("Terran_ExplosionLarge"), which
        // resolves through StarCraft II's own sound tables rather than through
        // anything in the model. Carried verbatim so a host that has those
        // tables can dispatch it; without them the pool's SND path finds
        // nothing and says so once.
        c.id = k.id;
        c.nodeIndex = k.bone;
        c.sequenceIndex = k.sequence;
        c.eventTrackTimes = std::move(times);
        out.push_back(std::move(c));
    }
    return out;
}

void M3ModelAdapter::CreatePoseStages(renderer::animation::PoseStageList& out) const {
    // ---- IKJT ------------------------------------------------------------
    for (const auto& jt : model_.ikJoints) {
        // The chunk names the two ends; the chain between them is the parent
        // walk from the effector up to the root, which is exactly what
        // `CJTIKSolver_Init` does before marking each bone.
        const std::size_t root = jt.boneIndex1;
        const std::size_t tip = jt.boneIndex2;
        if (root >= model_.bones.size() || tip >= model_.bones.size())
            continue;

        std::vector<i32> chain;
        std::size_t cur = tip;
        // Bounded by the bone count: a malformed parent link that cycles would
        // otherwise walk forever, and a damaged chunk should cost a skipped
        // solver rather than a hang.
        for (std::size_t guard = 0; guard <= model_.bones.size(); ++guard) {
            chain.push_back(static_cast<i32>(cur));
            if (cur == root)
                break;
            const ::whiteout::u16 p = model_.bones[cur].parentIndex;
            if (p == 0xFFFFu || p >= model_.bones.size())
                break;
            cur = p;
        }
        // Only a walk that actually reached the named root is a chain.
        if (chain.empty() || static_cast<std::size_t>(chain.back()) != root)
            continue;
        std::reverse(chain.begin(), chain.end()); // root-first
        if (chain.size() < 2)
            continue;

        out.push_back(std::make_unique<M3JtIkStage>(std::move(chain), jt.raycastUp,
                                                    jt.raycastDown, jt.maxSpeed,
                                                    jt.goalThreshold));
    }

    // ---- PATU ------------------------------------------------------------
    for (const auto& tb : model_.turretBehaviors) {
        if (tb.boneIndex >= model_.bones.size())
            continue;
        // The permitted axis, from the descriptor's third basis row.
        //
        // Every PATU descriptor in the corpus is an identity transform with
        // zero weights and zero limits (checked, not assumed — see
        // m3_solver_test's corpus sweep), so this reduces to +Z, which is the
        // yaw axis in StarCraft II's Z-up space and the right answer for a
        // turret. The chunk names the bone and essentially nothing else; the
        // axis, limits and turn rate a game would use live in SC2's Actor data,
        // which is not in the model and not something we have.
        const Vector3f axis{tb.transform.data[2][0], tb.transform.data[2][1],
                            tb.transform.data[2][2]};
        out.push_back(std::make_unique<M3TurretStage>(
            static_cast<i32>(tb.boneIndex), axis,
            // No turn rate is parsed; `yawWeight` is the closest authored
            // quantity and behaves as one (larger = swings faster). A zero
            // weight would freeze the turret, so it falls back to a rate that
            // crosses 180 degrees in about a second.
            tb.yawWeight > 0.0f ? tb.yawWeight : 3.0f, tb.yawLimited != 0, tb.yawMin,
            tb.yawMax));
    }

#if WDX_HAS_PHYSICS
    // ---- PHRB/PHYJ -------------------------------------------------------
    // Physics is last in the list by contract: solvers correct the animated
    // pose and physics consumes the corrected one (`pose_stage.h`). Nothing
    // for the overwhelming majority of models, whose rigid bodies are
    // kinematic hit proxies that never become dynamic.
    if (auto stage = renderer::profiles::sc2_heroes::CreateSc2PhysicsStage(model_)) {
        out.push_back(std::move(stage));
    }

    // ---- PHCL ------------------------------------------------------------
    // After the bodies, which is the same "consumes the corrected pose" rule
    // one step further along: a cloth anchored to a ragdoll's bone has to read
    // where the ragdoll put it, not where the animation did.
    if (auto stage = renderer::profiles::sc2_heroes::CreateSc2ClothStage(
            model_, cloth_, static_cast<i32>(model_.bones.size()))) {
        out.push_back(std::move(stage));
    }
#endif
}

SkeletonData M3ModelAdapter::GetSkeleton() {
    SkeletonData sk;
    const auto& bones = model_.bones;
    sk.nodeCount = static_cast<i32>(bones.size());
    if (bones.empty())
        return sk;

    sk.nodeParents.resize(bones.size());
    sk.billboardFlags.assign(bones.size(), 0u);
    for (std::size_t i = 0; i < bones.size(); ++i) {
        const ::whiteout::u16 p = bones[i].parentIndex;
        // 0xFFFF is the documented root marker; anything else out of range is a
        // damaged chunk and is treated as a root rather than followed.
        sk.nodeParents[i] =
            (p == 0xFFFFu || static_cast<std::size_t>(p) >= bones.size()) ? -1 : static_cast<i32>(p);

        // Raw BONE flags, masked to the billboard bits. Not decoded into the
        // renderer's billboard vocabulary because where StarCraft II applies
        // these is still unresolved — its BBSC solver's Solve is a stub — and
        // inventing a mapping now would be a guess wearing a type.
        const auto f = static_cast<::whiteout::u32>(bones[i].flags);
        sk.billboardFlags[i] = f & (static_cast<::whiteout::u32>(::whiteout::m3::BoneFlag::Billboard1) |
                                    static_cast<::whiteout::u32>(::whiteout::m3::BoneFlag::Billboard2));
    }

    sk.inverseBindMatrices.assign(bones.size(), Matrix44f::identity());
    const std::size_t n = (std::min)(bones.size(), model_.initialReference.size());
    for (std::size_t i = 0; i < n; ++i)
        sk.inverseBindMatrices[i] = model_.initialReference[i].matrix;
    if (model_.initialReference.size() < bones.size()) {
        std::fprintf(stderr, "[m3] '%s': IREF has %zu matrices for %zu bones; the rest bind as identity\n",
                     model_.name.c_str(), model_.initialReference.size(), bones.size());
    }

#if WDX_HAS_PHYSICS
    // One node per cloth particle, appended after the skeleton. Their inverse
    // bind is a pure translation because the rotation half already lives in the
    // output frame the solver writes — see `sc2_cloth.h`. Parentless, and not
    // billboards: nothing samples or composes them, the cloth stage writes each
    // one outright.
    if (cloth_) {
        for (const auto& piece : cloth_->pieces) {
            for (const Vector3f& rest : piece.restPositions) {
                sk.inverseBindMatrices.push_back(Matrix44f::translation({-rest.x, -rest.y, -rest.z}));
                sk.nodeParents.push_back(-1);
                sk.billboardFlags.push_back(0u);
            }
        }
        sk.nodeCount = static_cast<i32>(sk.inverseBindMatrices.size());
    }
#endif
    return sk;
}

std::vector<SkinWeightData> M3ModelAdapter::GetSkinWeights() {
    std::vector<SkinWeightData> out;
    if (divisionIndex_ >= model_.divisions.size() || model_.bones.empty())
        return out;
    const auto& div = model_.divisions[divisionIndex_];
    const auto& lookup = model_.boneLookup;
    const i32 boneCount = static_cast<i32>(model_.bones.size());

    out.reserve(emittedRegions_.size());
    for (std::size_t g = 0; g < emittedRegions_.size(); ++g) {
        const auto& region = div.regions[emittedRegions_[g]];

        SkinWeightData sw;
        sw.geosetId = static_cast<i32>(g);
#if WDX_HAS_PHYSICS
        // A cloth-influenced region's palette is the cloth, not the skeleton:
        // `GetMeshes` has already repointed its per-vertex indices at particle
        // slots, so the window it reads from has to be the particle nodes in
        // particle order.
        if (cloth_ != nullptr && g < geosetClothPiece_.size() && geosetClothPiece_[g] >= 0) {
            const auto& piece = cloth_->pieces[static_cast<std::size_t>(geosetClothPiece_[g])];
            sw.paletteLocalVertexIndices = true;
            const i32 base = boneCount + static_cast<i32>(piece.firstParticle);
            sw.subsetNodeIndices.reserve(piece.particleCount);
            for (std::size_t k = 0; k < piece.particleCount; ++k)
                sw.subsetNodeIndices.push_back(base + static_cast<i32>(k));
            out.push_back(std::move(sw));
            continue;
        }
#endif
        // `influences` stays empty, and that is the entire point: the weights
        // and indices are already in the baked vertex blob, described at
        // offsets 12 and 16, and go to the GPU untouched. Filling this in
        // would decode four bytes per vertex only to have the loader repack
        // them into a second stream holding the same numbers.
        sw.paletteLocalVertexIndices = true;

        // REGN's bone-lookup window *is* the region's palette. A vertex stores
        // its bone as an index into that window, so slot i of the palette must
        // hold the bone the window's entry i names — no remap, and nothing to
        // rewrite.
        const std::size_t first = region.firstBoneLookup;
        const std::size_t count = region.boneLookupCount;
        sw.subsetNodeIndices.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t slot = first + i;
            i32 gb = (slot < lookup.size()) ? static_cast<i32>(lookup[slot]) : 0;
            if (gb < 0 || gb >= boneCount)
                gb = 0;
            sw.subsetNodeIndices.push_back(gb);
        }
        // A region with no window still needs one slot for its vertices to
        // point at; its own root bone is the honest choice.
        if (sw.subsetNodeIndices.empty()) {
            const i32 rootBone =
                (region.rootBone < model_.bones.size()) ? static_cast<i32>(region.rootBone) : 0;
            sw.subsetNodeIndices.push_back(rootBone);
        }
        out.push_back(std::move(sw));
    }
    return out;
}

std::vector<SequenceInfo> M3ModelAdapter::GetSequences() const {
    std::vector<SequenceInfo> out;
    // The model's own sequences first, then each attached `.m3a`'s — the same
    // global index space `BuildLayers` decodes, so a host can hand an index
    // straight back without knowing which file it came from. Names are left
    // exactly as authored: a `.m3a` may well redefine one of the model's (we
    // measured `Stand` in both halves of a shipped pair), and StarCraft II only
    // hides the earlier one when the file is loaded with `EAnimLoadFlag`
    // `Override`, which is not the default and which nothing here sets.
    out.reserve(tables_.SequenceCount());
    for (std::size_t q = 0; q < tables_.SequenceCount(); ++q) {
        const ::whiteout::m3::Sequence& s = *tables_.SequenceAt(static_cast<i32>(q));
        SequenceInfo info;
        info.name = s.name.empty() ? "Sequence" : s.name;
        // Kept as authored rather than rebased to zero: the sub-track keys are
        // stamped in this same frame space, so shifting the window here would
        // desynchronise every track that starts at a non-zero frame.
        info.startMs = static_cast<i32>(s.startFrame);
        info.endMs = static_cast<i32>(s.endFrame);
        info.moveSpeed = s.moveSpeed;
        info.nonLooping = (s.flags & ::whiteout::m3::SequenceFlag::NotLooping) !=
                          ::whiteout::m3::SequenceFlag::None;
        out.push_back(std::move(info));
    }
    if (out.empty()) {
        // Only for a model with no SEQS at all — the actor still needs a clock.
        SequenceInfo s;
        s.name = "Stand";
        s.startMs = 0;
        s.endMs = 1000;
        out.push_back(std::move(s));
    }
    return out;
}

void M3ModelAdapter::BuildLayers(const PoseRequest& req, std::vector<M3Layer>& out) const {
    out.clear();
    const auto clips = req.clips;
    for (std::size_t c = 0; c < clips.size() && c < 64; ++c) {
        const ClipRef& clip = clips[c];
        if (tables_.SequenceCount() == 0)
            break;
        // Wrapped rather than dropped. Hosts pass a raw, unbounded index and
        // rely on the wrap — it is what `ClipPlaylist::Advance` does — and a
        // dropped layer here is invisible: the model renders in bind pose and
        // nothing reports a miss.
        const i32 n = static_cast<i32>(tables_.SequenceCount());
        const i32 seqIdx = ((clip.sequence % n) + n) % n;
        const auto& seq = *tables_.SequenceAt(seqIdx);
        // Absolute frame time, as the keys are stamped. The unwrapped elapsed
        // is what arrives, because a looping track wraps on its own duration
        // and the sequence-windowed time has already lost that.
        const i32 t = static_cast<i32>(seq.startFrame) + clip.elapsedMs;

        for (const auto& d : tables_.LayersFor(seqIdx)) {
            M3Layer l;
            l.play = static_cast<::whiteout::u16>(c);
            l.stc = d.stc;
            l.priority = d.priority;
            l.transparent = d.transparent;
            l.timeMs = t;
            l.weight = clip.weight;
            l.loop = clip.loop;
            out.push_back(l);
        }
    }
    // Priority first, and stable so that layers of equal priority keep clip
    // order — newest clip wins a tie, matching the insertion-ordered player
    // list StarCraft II walks.
    std::stable_sort(out.begin(), out.end(),
                     [](const M3Layer& a, const M3Layer& b) { return a.priority > b.priority; });
}

renderer::model::FrameState M3ModelAdapter::Evaluate(const PoseRequest& req) const {
    renderer::model::FrameState fs;
    const std::size_t boneCount = model_.bones.size();
    if (boneCount == 0)
        return fs;

    std::vector<M3Layer> layers;
    BuildLayers(req, layers);

    // Bind pose when nothing is playing: every channel falls back to its
    // AnimRef default, which is exactly what an empty layer list produces.
    fs.boneWorldMatrices.resize(boneCount);
#if WDX_HAS_PHYSICS
    // The cloth particles' nodes, seeded so an actor whose stage has not run
    // yet — or a build with physics compiled out of the *stage* but not the
    // palette — draws the cloth region in bind pose rather than collapsed on
    // the origin. `translate(+rest)` is the exact inverse of the particle's
    // inverse bind.
    if (cloth_) {
        fs.boneWorldMatrices.reserve(boneCount + cloth_->particleCount);
        for (const auto& piece : cloth_->pieces)
            for (const Vector3f& rest : piece.restPositions)
                fs.boneWorldMatrices.push_back(Matrix44f::translation(rest));
    }
#endif
    std::vector<::whiteout::u8> visible(boneCount, 1);

    // A `replace` override is the host saying "this bone's model-space matrix
    // is mine: do not sample it and do not compose the parent chain into it".
    // That is how a pose stage's claims come back — the ragdoll's bit-0 flag in
    // StarCraft II's own runtime, which is read by the animation system for
    // exactly this purpose (`DOMINO_GLUE.md` §6.6). Without it the sampler
    // re-poses a simulated bone every frame and the stage overwrites it again,
    // which lands in the same place but means a claim buys nothing.
    //
    // A driven bone is still a parent: bones below it compose onto the driven
    // matrix exactly as they would onto a sampled one, which is what carries a
    // ragdoll's hands and attachment points with its arms. Same rule as
    // `M2ModelAdapter::EvaluateBones`.
    std::vector<const ::whiteout::flakes::NodeOverride*> over;
    if (!req.overrides.empty()) {
        over.assign(boneCount, nullptr);
        for (const auto& o : req.overrides) {
            if (o.node >= 0 && static_cast<std::size_t>(o.node) < boneCount)
                over[static_cast<std::size_t>(o.node)] = &o;
        }
    }

    for (std::size_t i = 0; i < boneCount; ++i) {
        const auto& b = model_.bones[i];
        const ::whiteout::flakes::NodeOverride* ov = over.empty() ? nullptr : over[i];

        const ::whiteout::u16 p = b.parentIndex;
        // Bones are stored parents-first, so one linear pass resolves the
        // hierarchy; a forward reference would read an unwritten matrix, so it
        // is treated as a root instead.
        const bool hasParent = p != 0xFFFFu && p < i;

        if (ov != nullptr && ov->replace) {
            fs.boneWorldMatrices[i] = ov->m;
        } else {
            const Vector3f t = SampleRef(b.position, layers);
            const Quaternion r = SampleRef(b.rotation, layers);
            const Vector3f s = SampleRef(b.scale, layers);

            Matrix44f local = M3ComposeLocal(t, r, s);
            // A non-replace override composes *after* the local TRS and before
            // the parent multiply, which is where a turret or look-at correction
            // belongs (`pose_request.h`). Nothing in-tree writes one — the
            // solvers are pose stages instead — but a silent no-op here is the
            // same failure the claims had.
            if (ov != nullptr)
                local = local * ov->m;

            if (hasParent)
                fs.boneWorldMatrices[i] = local * fs.boneWorldMatrices[p];
            else
                fs.boneWorldMatrices[i] = local;
        }

        // Visibility is hierarchical: a bone under an invisible parent is
        // invisible whatever its own track says, and the engine never even
        // samples it in that case (M3Anim_EvaluateBoneVisibility tests the
        // parent's visible bit before doing any work).
        if (hasParent && !visible[p]) {
            visible[i] = 0;
        } else {
            // Discrete channel — sampled in override mode, so the first play
            // past the filters wins outright rather than blending.
            visible[i] = SampleRefOverride(b.visibility, layers) != 0 ? 1 : 0;
        }
    }

    EvaluateGeosetVisibility(visible, fs);
    EvaluateLights(layers, visible, req.world, fs);
    EvaluatePhysics(layers, fs);
    return fs;
}

void M3ModelAdapter::EvaluatePhysics(std::span<const M3Layer> layers,
                                     renderer::model::FrameState& fs) const {
    // `PHCL.active` gates the cloth's whole contribution — StarCraft II skips
    // the write-back outright when it reads zero, so the mesh keeps the pose it
    // last had rather than simulating unseen. Sampled here, and on the same flag
    // bit as `dynamicState`: 69 of the corpus's 392 cloth records ask for it.
    if (!model_.clothPhysics.empty()) {
        fs.clothActive.resize(model_.clothPhysics.size());
        for (std::size_t i = 0; i < model_.clothPhysics.size(); ++i) {
            const auto& c = model_.clothPhysics[i];
            ::whiteout::u32 v = c.active.initValue;
            if ((c.active.flags & 0x2u) != 0u)
                v = SampleRefOverride(c.active, layers);
            fs.clothActive[i] = v != 0 ? 1 : 0;
        }
    }

    if (model_.rigidBodies.empty())
        return;

    fs.physicsBodyDynamic.resize(model_.rigidBodies.size());
    for (std::size_t i = 0; i < model_.rigidBodies.size(); ++i) {
        const auto& rb = model_.rigidBodies[i];
        // `initValue` is the *base*, not a fallback for the unbound case:
        // `M3Physics_UpdateBodyDrivenState` loads PHRB+40 into its scratch and
        // only lets the sampler overwrite it. And the gate on that sampler is
        // the AnimRef's flag bit 1 rather than its `animId` — every shipped
        // `dynamicState` has a non-zero id, so reading the id as the gate sends
        // nine bodies in ten looking for keys that are not theirs.
        ::whiteout::u32 v = rb.dynamicState.initValue;
        if ((rb.dynamicState.flags & 0x2u) != 0u)
            v = SampleRefOverride(rb.dynamicState, layers);
        fs.physicsBodyDynamic[i] = v != 0 ? 1 : 0;
    }

#if WDX_HAS_PHYSICS
    // One frame behind for a simulated bone: this runs before the pose stages,
    // so a claimed bone still holds the animated pose here. The physics stage
    // overwrites these with the poses it actually produced — this fill is what
    // covers the models that have no stage at all, whose bodies are kinematic
    // proxies that never simulate.
    renderer::profiles::sc2_heroes::Sc2PlaceCollisionShapes(physicsShapeBones_,
                                                            physicsShapeLocals_,
                                                            physicsShapeAniso_,
                                                            fs.boneWorldMatrices,
                                                            fs.collisionTransforms);
#endif
}

std::vector<renderer::model::CollisionShapeData> M3ModelAdapter::GetCollisionShapes() {
    physicsShapeBones_.clear();
    physicsShapeLocals_.clear();
    physicsShapeAniso_.clear();
#if WDX_HAS_PHYSICS
    auto built = renderer::profiles::sc2_heroes::Sc2BuildCollisionShapes(model_);
    physicsShapeBones_ = std::move(built.bones);
    physicsShapeLocals_ = std::move(built.locals);
    physicsShapeAniso_ = std::move(built.anisotropic);
    return std::move(built.shapes);
#else
    return {};
#endif
}

std::vector<renderer::model::ClothOverlayData> M3ModelAdapter::GetClothOverlays() {
#if WDX_HAS_PHYSICS
    if (!cloth_) {
        return {};
    }
    namespace sc2 = renderer::profiles::sc2_heroes;
    std::vector<renderer::model::ClothOverlayData> out;
    out.reserve(cloth_->pieces.size());
    const auto boneCount = static_cast<i32>(model_.bones.size());
    for (const auto& piece : cloth_->pieces) {
        renderer::model::ClothOverlayData d;
        // The palette layout `sc2_cloth.h` promises: particle `i` of this piece
        // is one node, appended after the real skeleton, and its live position
        // is that node's origin.
        d.particleNodes.reserve(piece.particleCount);
        for (std::size_t i = 0; i < piece.particleCount; ++i) {
            d.particleNodes.push_back(boneCount + static_cast<i32>(piece.firstParticle + i));
        }
        d.pinnedCount = piece.def.pinnedCount;
        d.links.reserve(piece.def.edges.size() * 2);
        for (const auto& e : piece.def.edges) {
            if (e.a >= piece.particleCount || e.b >= piece.particleCount) {
                continue;
            }
            d.links.push_back(e.a);
            d.links.push_back(e.b);
        }
        d.colliders.reserve(piece.def.capsules.size());
        for (const auto& c : piece.def.capsules) {
            renderer::model::ClothColliderData cd;
            cd.node = c.anchor >= 0 && c.anchor < boneCount ? c.anchor : -1;
            cd.local = sc2::Sc2ComposeBone(
                Quaternion{c.localRotation.x, c.localRotation.y, c.localRotation.z,
                           c.localRotation.w},
                Vector3f{c.localPosition.x, c.localPosition.y, c.localPosition.z});
            cd.radius0 = c.radius0;
            cd.radius1 = c.radius1;
            cd.length = c.fullLength;
            d.colliders.push_back(cd);
        }
        d.activeIndex = static_cast<i32>(piece.chunkIndex);
        out.push_back(std::move(d));
    }
    return out;
#else
    return {};
#endif
}

void M3ModelAdapter::EvaluateGeosetVisibility(std::span<const ::whiteout::u8> visible,
                                              renderer::model::FrameState& fs) const {
    if (emittedRegions_.empty() || divisionIndex_ >= model_.divisions.size())
        return;
    const auto& div = model_.divisions[divisionIndex_];
    fs.geosetAlphas.assign(emittedRegions_.size(), 1.0f);
    fs.geosetHidden.assign(emittedRegions_.size(), 0);
    for (std::size_t g = 0; g < emittedRegions_.size(); ++g) {
        // A region is gated by the bone it hangs off. The chain walk is already
        // folded into `visible`, so this is a single lookup.
        const auto& region = div.regions[emittedRegions_[g]];
        const std::size_t root = region.rootBone;
        if (root < visible.size() && !visible[root])
            fs.geosetAlphas[g] = 0.0f;
        // A cloth's simulated region is a coarse invisible proxy — the visible
        // surface is the region bound to it. Taken out of the draw list rather
        // than faded, because it is not part of the model at all (the same
        // distinction `geosetHidden` exists for).
        if (g < geosetClothProxy_.size() && geosetClothProxy_[g])
            fs.geosetHidden[g] = 1;
    }
}

void M3ModelAdapter::EvaluateLights(std::span<const M3Layer> layers,
                                    std::span<const ::whiteout::u8> visible,
                                    const Matrix44f& world,
                                    renderer::model::FrameState& fs) const {
    if (model_.lights.empty())
        return;
    fs.lights.reserve(model_.lights.size());

    for (const auto& l : model_.lights) {
        renderer::model::FrameState::LightState st;
        switch (l.lightType) {
        case ::whiteout::m3::LightType::Directional:
            st.kind = renderer::model::FrameState::LightKind::Directional;
            break;
        default:
            // Spot lights carry a cone the shared LightState cannot express;
            // treated as omni until a shading model needs the cone, which is
            // strictly better than dropping the light.
            st.kind = renderer::model::FrameState::LightKind::Omni;
            break;
        }

        const std::size_t bone = l.boneIndex;
        Matrix44f m = Matrix44f::identity();
        if (bone < fs.boneWorldMatrices.size())
            m = fs.boneWorldMatrices[bone] * world;
        else
            m = world;

        st.worldPos = {m.data[3][0], m.data[3][1], m.data[3][2]};
        // Row 2 is the bone's forward axis in this row-vector convention.
        st.worldDir = {m.data[2][0], m.data[2][1], m.data[2][2]};

        const Vector3f colour = SampleRef(l.diffuseColor, layers);
        const f32 intensity = SampleRef(l.intensityMultiplier, layers);
        // Premultiplied, matching what LightState documents as shader-ready.
        st.diffuse = {colour.x * intensity, colour.y * intensity, colour.z * intensity};
        st.dirIntensity = intensity;
        // The highlight is its own colour, not a scaling of the diffuse —
        // deferredlight.fx:220 multiplies the specular term by the light's own
        // constant, and LightFlag::Specular is the axis that enables it.
        const Vector3f spec = SampleRef(l.specularColor, layers);
        const f32 specMul = SampleRef(l.specularMultiplier, layers);
        st.specular = {spec.x * specMul, spec.y * specMul, spec.z * specMul};
        st.useSpecular = (static_cast<u32>(l.flags) &
                          static_cast<u32>(::whiteout::m3::LightFlag::Specular)) != 0;
        st.attenStart = SampleRef(l.attenuationStart, layers);
        st.attenEnd = l.attenuationEnd;
        st.enabled = bone >= visible.size() || visible[bone] != 0;
        fs.lights.push_back(st);
    }
}

} // namespace whiteout::flakes::io
