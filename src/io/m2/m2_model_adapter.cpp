#include "io/m2/m2_model_adapter.h"

#include <whiteout/models/m2/parser.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <type_traits>

namespace whiteout::flakes::io {

using renderer::model::MeshBuffer;
using renderer::model::MeshData;
using renderer::model::SequenceInfo;
using renderer::model::VertexAttribute;
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
                                                     IContentProvider* provider) {
    if (bytes.empty())
        return nullptr;
    ::whiteout::m2::Parser parser;
    ::whiteout::m2::Model model;
    try {
        // The ref's discriminant picks the route, because it is the same
        // question: a model named by id has id-named siblings, a model named
        // by path has its siblings on disk beside it.
        if (ref.IsFileId()) {
            ContentProviderCascFs fs(provider);
            model = parser.parse(fs, bytes);
        } else {
            ContentProviderPathFs fs(provider);
            model = parser.parse(fs, ref.path);
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
    return std::make_shared<M2ModelAdapter>(std::move(model));
}

M2ModelAdapter::M2ModelAdapter(::whiteout::m2::Model model) : model_(std::move(model)) {
    // Profile 0 is the highest detail level. LOD selection is a later phase;
    // taking one and saying so beats taking whichever happens to be first
    // without noticing there were others.
    profileIndex_ = 0;
    if (profileIndex_ < model_.skinProfiles.size())
        submeshCount_ = model_.skinProfiles[profileIndex_].submeshes.size();
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

std::vector<SequenceInfo> M2ModelAdapter::GetSequences() const {
    SequenceInfo s;
    s.name = "Stand";
    s.startMs = 0;
    s.endMs = 1000;
    s.nonLooping = false;
    return {s};
}

renderer::model::FrameState M2ModelAdapter::Evaluate(const PoseRequest& req) const {
    (void)req;
    // Bind pose, every frame. Bone tracks are a later phase; returning an
    // empty FrameState leaves boneWorldMatrices empty, which is what a
    // skinning-free actor wants — the geometry draws with its world transform
    // and nothing else.
    return {};
}

} // namespace whiteout::flakes::io
