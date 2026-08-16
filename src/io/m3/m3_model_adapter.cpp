#include "io/m3/m3_model_adapter.h"

#include <algorithm>
#include <cstdio>

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
//   20 normal        i8  x4  (/127; .w is the tangent handedness sign)
//   24 colour        u8  x4  BGRA, only when the VertexColor flag is set
//   .. uv0..uv4      i16 x2  each, one per UV flag
//   -4 tangent       i8  x4  (/127), always the last four bytes
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
    attrs.push_back({VertexSemantic::Normal, 0, gfx::Format::R8G8B8A8_SNORM, 20});
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
        {VertexSemantic::Tangent, 0, gfx::Format::R8G8B8A8_SNORM, static_cast<u16>(stride - 4)});
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
        const ::whiteout::m3::AnimBlock<T>* blk =
            h.Valid() ? BlockOf(model_.subTrackCollections[l.stc], h, static_cast<const T*>(nullptr))
                      : nullptr;

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
        const bool has = h.Valid() && h.slot == M3SdSlot::U32 &&
                         !model_.subTrackCollections[l.stc].sdu3[h.block].keys.empty();
        if (!has && l.transparent)
            continue;
        if (l.play < 64)
            visited |= (::whiteout::u64{1} << l.play);
        if (!has)
            return ref.initValue;

        const auto& blk = model_.subTrackCollections[l.stc].sdu3[h.block];
        // Discrete: never interpolated, whatever the ref's interp type says.
        const M3KeySpan sp = M3LocateKey(blk.timestamps, l.timeMs, l.loop, /*interpolate*/ false);
        if (!sp.valid)
            return ref.initValue;
        return blk.keys[(std::min)(sp.i0, blk.keys.size() - 1)];
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
    tables_.Build(model_);
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
        mesh.materialId = -1; // no materials in this phase; UnlitShading draws it
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
    out.reserve(model_.sequences.size());
    for (const auto& s : model_.sequences) {
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
        if (model_.sequences.empty())
            break;
        // Wrapped rather than dropped. Hosts pass a raw, unbounded index and
        // rely on the wrap — it is what `ClipPlaylist::Advance` does — and a
        // dropped layer here is invisible: the model renders in bind pose and
        // nothing reports a miss.
        const i32 n = static_cast<i32>(model_.sequences.size());
        const i32 seqIdx = ((clip.sequence % n) + n) % n;
        const auto& seq = model_.sequences[seqIdx];
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
    std::vector<::whiteout::u8> visible(boneCount, 1);

    for (std::size_t i = 0; i < boneCount; ++i) {
        const auto& b = model_.bones[i];

        const Vector3f t = SampleRef(b.position, layers);
        const Quaternion r = SampleRef(b.rotation, layers);
        const Vector3f s = SampleRef(b.scale, layers);

        Matrix44f local = M3ComposeLocal(t, r, s);

        const ::whiteout::u16 p = b.parentIndex;
        // Bones are stored parents-first, so one linear pass resolves the
        // hierarchy; a forward reference would read an unwritten matrix, so it
        // is treated as a root instead.
        const bool hasParent = p != 0xFFFFu && p < i;
        if (hasParent)
            fs.boneWorldMatrices[i] = local * fs.boneWorldMatrices[p];
        else
            fs.boneWorldMatrices[i] = local;

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
    return fs;
}

void M3ModelAdapter::EvaluateGeosetVisibility(std::span<const ::whiteout::u8> visible,
                                              renderer::model::FrameState& fs) const {
    if (emittedRegions_.empty() || divisionIndex_ >= model_.divisions.size())
        return;
    const auto& div = model_.divisions[divisionIndex_];
    fs.geosetAlphas.assign(emittedRegions_.size(), 1.0f);
    for (std::size_t g = 0; g < emittedRegions_.size(); ++g) {
        // A region is gated by the bone it hangs off. The chain walk is already
        // folded into `visible`, so this is a single lookup.
        const auto& region = div.regions[emittedRegions_[g]];
        const std::size_t root = region.rootBone;
        if (root < visible.size() && !visible[root])
            fs.geosetAlphas[g] = 0.0f;
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
        st.attenStart = SampleRef(l.attenuationStart, layers);
        st.attenEnd = l.attenuationEnd;
        st.enabled = bone >= visible.size() || visible[bone] != 0;
        fs.lights.push_back(st);
    }
}

} // namespace whiteout::flakes::io
