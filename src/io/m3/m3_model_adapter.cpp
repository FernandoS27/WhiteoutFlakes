#include "io/m3/m3_model_adapter.h"

#include <cstdio>

namespace whiteout::flakes::io {

using renderer::model::MeshBuffer;
using renderer::model::MeshData;
using renderer::model::SequenceInfo;
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

} // namespace

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
    // batches, and for the REGN v2 gap this counts.
    std::size_t noFaceRange = 0;
    out.reserve(div.regions.size());
    for (std::size_t r = 0; r < div.regions.size(); ++r) {
        const auto& region = div.regions[r];
        if (region.vertexCount == 0)
            continue;
        if (region.indexCount == 0) {
            ++noFaceRange;
            continue;
        }

        const std::size_t vBegin = region.firstVertex;
        const std::size_t vEnd = vBegin + region.vertexCount;
        const std::size_t iBegin = region.firstIndex;
        const std::size_t iEnd = iBegin + region.indexCount;
        if (vEnd > positions.size() || iEnd > div.faces.size())
            continue; // truncated chunk; skip rather than read out of bounds

        MeshData mesh;
        mesh.geosetId = static_cast<i32>(r);
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

std::vector<SequenceInfo> M3ModelAdapter::GetSequences() const {
    SequenceInfo s;
    s.name = "Stand";
    s.startMs = 0;
    s.endMs = 1000;
    s.nonLooping = false;
    return {s};
}

renderer::model::FrameState M3ModelAdapter::Evaluate(const PoseRequest& req) const {
    (void)req;
    // Bind pose, every frame — same as `.m2`. Bone tracks are a later phase;
    // an empty FrameState leaves boneWorldMatrices empty, which is what a
    // skinning-free actor wants.
    return {};
}

} // namespace whiteout::flakes::io
