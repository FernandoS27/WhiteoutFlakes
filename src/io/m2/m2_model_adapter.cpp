#include "io/m2/m2_model_adapter.h"

#include <whiteout/models/m2/parser.h>

#include <cstdio>

namespace whiteout::flakes::io {

using renderer::model::MeshData;
using renderer::model::SequenceInfo;

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
        mesh.positions.reserve(sec.vertexCount);
        for (std::size_t v = vBegin; v < vEnd; ++v) {
            const std::size_t gv = skin.vertices[v];
            if (gv >= model_.vertices.size()) {
                mesh.positions.push_back({0.0f, 0.0f, 0.0f});
                continue;
            }
            mesh.positions.push_back(model_.vertices[gv].position);
        }
        // Normals and UVs are sized to match because the upload path builds a
        // fully interleaved vertex and reads all four arrays. Left at zero:
        // UnlitShading reads position alone, and inventing plausible-looking
        // normals would make a later lighting bug harder to spot than a
        // visibly flat one.
        mesh.normals.assign(mesh.positions.size(), {0.0f, 0.0f, 1.0f});
        mesh.uvs.assign(mesh.positions.size(), {0.0f, 0.0f});

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
