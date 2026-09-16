#include "renderer/mesh_overlay/mesh_overlay_renderer.h"

#include "renderer/bls/bls_frame.h" // PackBoneVertex
#include "renderer/core/render_detail.h"
#include "renderer/model/render_model.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/types.h"

#include "compiled_shaders.h"

#include <algorithm>
#include <cstring>

namespace whiteout::flakes::renderer::mesh_overlay {

namespace {

// Of the eye distance. Enough for a one-pixel-wide edge to clear its own
// triangles on a surface seen at a steep angle, too little to lift it off a
// surface in front of it at any depth the viewer frames a model at.
constexpr f32 kDepthPullFraction = 0.0025f;
constexpr f32 kEdgeWidthPx = 1.25f;
constexpr f32 kVertexDotPx = 5.0f;

void Copy4(f32 dst[4], const f32 src[4]) {
    std::memcpy(dst, src, sizeof(f32) * 4);
}

} // namespace

void MeshOverlayRenderer::Init() {
    if (initTried_)
        return;
    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return;
    initTried_ = true;

    using namespace whiteout::flakes::Shaders;
    switch (gfxDev->GetApi()) {
    case gfx::GfxApi::Vulkan:
        ps_ = gfxDev->CreateShader(gfx::ShaderStage::Pixel, kMeshOverlayPSSpv,
                                   sizeof(kMeshOverlayPSSpv));
        break;
    case gfx::GfxApi::WebGPU:
        ps_ = gfxDev->CreateShader(gfx::ShaderStage::Pixel, kMeshOverlayPSWgsl,
                                   sizeof(kMeshOverlayPSWgsl));
        break;
    case gfx::GfxApi::Metal:
        ps_ = gfxDev->CreateShader(gfx::ShaderStage::Pixel, kMeshOverlayPSMtl,
                                   sizeof(kMeshOverlayPSMtl));
        break;
    default:
        ps_ = gfxDev->CreateShader(gfx::ShaderStage::Pixel, kMeshOverlayPS, sizeof(kMeshOverlayPS));
        break;
    }
    // Up to three draws per geoset per frame.
    cb_ = gfxDev->CreateBuffer({
        .size = sizeof(core::MeshOverlayCbData),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = 4096,
    });
}

void MeshOverlayRenderer::Release() {
    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return;
    for (auto& [key, pso] : psos_)
        gfxDev->Destroy(pso);
    psos_.clear();
    gfxDev->Destroy(ps_);
    gfxDev->Destroy(cb_);
    ps_ = gfx::ShaderHandle::Invalid;
    cb_ = gfx::BufferHandle::Invalid;
    initTried_ = false;
}

void MeshOverlayRenderer::BeginFrame(const core::DebugFrame& frame,
                                     const core::DebugTargetInfo& target, i32 width, i32 height,
                                     const Matrix44f& projection, u32 backgroundRgb) {
    frame_ = frame;
    targetEncodesSrgb_ = target.targetEncodesSrgb;
    halfViewport_[0] = 0.5f * static_cast<f32>(width > 0 ? width : 1);
    halfViewport_[1] = 0.5f * static_cast<f32>(height > 0 ? height : 1);
    depthPull_ = core::MeshOverlayDepthPull(projection, kDepthPullFraction);
    background_ = backgroundRgb;
    emitted_.clear();
}

bool MeshOverlayRenderer::Wants(core::PassSlot pass) const {
    if (!frame_.overlay.Any())
        return false;
    return pass == core::PassSlot::OpaqueColor || pass == core::PassSlot::TransparentScene ||
           pass == core::PassSlot::GBuffer;
}

gfx::PipelineHandle MeshOverlayRenderer::Pso(const PsoKey& key) {
    if (auto it = psos_.find(key); it != psos_.end())
        return it->second;
    auto* gfxDev = rs_.Pipeline().Gfx();

    gfx::GraphicsPipelineDesc desc{};
    desc.vs = static_cast<gfx::ShaderHandle>(key.vs);
    desc.ps = ps_;
    desc.topology = gfx::PrimitiveTopology::TriangleList;
    const bool opaqueFaces =
        key.pass == static_cast<u32>(core::MeshOverlayPass::Faces) && !key.markedOnly;
    if (!opaqueFaces) {
        desc.blend.enable = true;
        desc.blend.srcColor = gfx::BlendFactor::SrcAlpha;
        desc.blend.dstColor = gfx::BlendFactor::InvSrcAlpha;
        desc.blend.srcAlpha = gfx::BlendFactor::One;
        desc.blend.dstAlpha = gfx::BlendFactor::InvSrcAlpha;
    }
    desc.depthStencil.depthTest = key.occluded;
    desc.depthStencil.depthWrite = key.occluded && opaqueFaces;
    desc.depthStencil.depthCompare = gfx::CompareOp::LessEqual;
    desc.rasterizer.cull = gfx::CullMode::None;
    desc.rtvFormat = key.rtv;
    desc.extraRtvFormats[0] = key.extra0;
    desc.extraRtvFormats[1] = key.extra1;
    desc.extraRtvFormats[2] = key.extra2;
    desc.extraRtvCount = key.extraCount;
    desc.extraColorWrite = false;
    desc.dsvFormat = key.dsv;

    const gfx::PipelineHandle pso = gfxDev->CreateGraphicsPipeline(desc);
    psos_.emplace(key, pso);
    return pso;
}

bool MeshOverlayRenderer::Prepare(const render_detail::RenderableView& view, i32 geoIdx) {
    auto& rm = *view.model;
    const model::GPUGeoset& geo = rm.gpuGeosets[static_cast<usize>(geoIdx)];
    const model::MeshOverlaySource* source = geo.overlaySource.get();
    if (!source || source->positions.empty())
        return false;

    auto& state = rm.overlay;
    if (state.geosets.size() != rm.gpuGeosets.size())
        state.geosets.resize(rm.gpuGeosets.size());
    auto& g = state.geosets[static_cast<usize>(geoIdx)];
    if (g.source == source && g.revision == state.revision && !geo.deformActive &&
        g.elements != gfx::BufferHandle::Invalid)
        return true;

    const u32 vertexCount = static_cast<u32>(source->positions.size());

    // A cloth's positions this frame, read back out of the bytes its deform
    // buffer was just filled from.
    std::vector<Vector3f> deformed;
    std::span<const Vector3f> positions = source->positions;
    if (geo.deformActive) {
        auto it = rm.deformStaging.find(geo.geosetId);
        if (it != rm.deformStaging.end() && geo.baseStride != 0) {
            for (const auto& a : rs_.Pipeline().VertexLayouts().Attributes(geo.layoutId)) {
                if (a.semantic != core::VertexSemantic::Position || a.semanticIndex != 0 ||
                    a.format != gfx::Format::R32G32B32_FLOAT ||
                    a.offset + sizeof(Vector3f) > geo.baseStride)
                    continue;
                const usize count =
                    std::min<usize>(it->second.size() / geo.baseStride, vertexCount);
                deformed.assign(source->positions.begin(), source->positions.end());
                for (usize i = 0; i < count; ++i)
                    std::memcpy(&deformed[i], it->second.data() + i * geo.baseStride + a.offset,
                                sizeof(Vector3f));
                positions = deformed;
                break;
            }
        }
    }

    // The bone bytes the vertex buffer was built with: in the record, or packed
    // from the actor's weights exactly as the upload packed its bone stream.
    std::vector<u8> weights;
    std::vector<u8> indices;
    std::span<const u8> weightSpan = source->boneWeights;
    std::span<const u8> indexSpan = source->boneIndices;
    if (weightSpan.empty() && view.skinning) {
        const auto* info = view.skinning->GetGeosetWeights(geo.geosetId);
        if (info && info->vertices.size() == vertexCount) {
            weights.resize(static_cast<usize>(vertexCount) * 4);
            indices.resize(static_cast<usize>(vertexCount) * 4);
            for (u32 v = 0; v < vertexCount; ++v) {
                const auto& inf = info->vertices[v];
                const i32 idx[4] = {inf.boneIdx[0], inf.boneIdx[1], inf.boneIdx[2], inf.boneIdx[3]};
                const f32 wt[4] = {inf.weight[0], inf.weight[1], inf.weight[2], inf.weight[3]};
                BoneVertex bv{};
                bls::PackBoneVertex(bv, idx, wt);
                std::memcpy(weights.data() + v * 4, bv.weights, 4);
                std::memcpy(indices.data() + v * 4, bv.indices, 4);
            }
            weightSpan = weights;
            indexSpan = indices;
        }
    }

    const auto& edges = source->Edges();
    const auto layout = core::MeshOverlayLayoutFor(vertexCount, static_cast<u32>(edges.size()),
                                                   static_cast<u32>(source->indices.size() / 3));
    if (layout.elementCount == 0)
        return false;

    const core::MeshElementStates* states = nullptr;
    if (auto it = state.states.find(geo.geosetId); it != state.states.end())
        states = &it->second;

    auto* gfxDev = rs_.Pipeline().Gfx();
    if (g.elements == gfx::BufferHandle::Invalid || g.capacity != layout.elementCount) {
        gfxDev->Destroy(g.elements);
        g.elements = gfxDev->CreateBuffer({
            .size = static_cast<u64>(layout.elementCount) * sizeof(f32) * 4,
            .elementStride = sizeof(f32) * 4,
            .usage = gfx::BufferUsage::ShaderResource | gfx::BufferUsage::CpuWritable,
            .ringSlotsHint = 4,
        });
        g.capacity = g.elements != gfx::BufferHandle::Invalid ? layout.elementCount : 0;
    }
    if (g.elements == gfx::BufferHandle::Invalid)
        return false;

    void* mapped = gfxDev->MapBuffer(g.elements);
    if (!mapped)
        return false;
    core::PackMeshOverlay(
        std::span<f32>(static_cast<f32*>(mapped), static_cast<usize>(layout.elementCount) * 4),
        {.positions = positions,
         .boneWeights = weightSpan,
         .boneIndices = indexSpan,
         .indices = source->indices,
         .edges = edges},
        states, layout);
    gfxDev->UnmapBuffer(g.elements);

    g.source = source;
    g.revision = state.revision;
    g.layout = layout;
    g.markedFaces = states && core::AnyMarkedFace(*states);
    return true;
}

void MeshOverlayRenderer::Emit(gfx::IGFXCommandList* cmd, const OverlayDraw& draw) {
    const auto* view = draw.view;
    if (!cmd || !view || !view->model || draw.geoIdx < 0 || draw.vs == gfx::ShaderHandle::Invalid ||
        !frame_.overlay.Any() || static_cast<usize>(draw.geoIdx) >= view->model->gpuGeosets.size())
        return;
    if (!emitted_.emplace(view->model, draw.geoIdx).second)
        return;
    Init();
    if (ps_ == gfx::ShaderHandle::Invalid || cb_ == gfx::BufferHandle::Invalid)
        return;
    if (!Prepare(*view, draw.geoIdx))
        return;

    auto* gfxDev = rs_.Pipeline().Gfx();
    const auto& g = view->model->overlay.geosets[static_cast<usize>(draw.geoIdx)];
    const core::MeshOverlayStyle style = frame_.overlay;
    const core::MeshOverlayColors colors =
        core::MeshOverlayColorsFor(frame_.view, view->teamColor, background_);

    cmd->BindShaderResource(gfx::ShaderStage::Vertex, 17, g.elements);

    auto issue = [&](core::MeshOverlayPass pass, bool markedOnly, u32 vertexCount) {
        if (vertexCount == 0)
            return;
        PsoKey key;
        key.vs = static_cast<u64>(draw.vs);
        key.pass = static_cast<u32>(pass);
        key.occluded = style.occluded;
        key.markedOnly = markedOnly;
        key.rtv = draw.target.rtv;
        key.extra0 = draw.target.extra[0];
        key.extra1 = draw.target.extra[1];
        key.extra2 = draw.target.extra[2];
        key.extraCount = draw.target.extraCount;
        key.dsv = draw.target.dsv;
        const gfx::PipelineHandle pso = Pso(key);
        if (pso == gfx::PipelineHandle::Invalid)
            return;

        if (auto* c = static_cast<core::MeshOverlayCbData*>(gfxDev->MapBuffer(cb_))) {
            *c = core::MeshOverlayCbData{};
            c->pass = static_cast<u32>(pass);
            c->flags = (targetEncodesSrgb_ ? core::kMeshOverlayTargetEncodesSrgb : 0u) |
                       (markedOnly ? core::kMeshOverlayMarkedFacesOnly : 0u);
            c->vertexBase = g.layout.vertexBase;
            c->edgeBase = g.layout.edgeBase;
            c->faceBase = g.layout.faceBase;
            c->halfViewport[0] = halfViewport_[0];
            c->halfViewport[1] = halfViewport_[1];
            c->edgeWidth = kEdgeWidthPx;
            c->vertexSize = kVertexDotPx;
            c->depthPull = depthPull_;
            Copy4(c->faceColor, colors.face);
            Copy4(c->edgeColor, colors.edge);
            Copy4(c->vertexColor, colors.vertex);
            Copy4(c->selectedColor, colors.selected);
            Copy4(c->hoveredColor, colors.hovered);
            gfxDev->UnmapBuffer(cb_);
        }
        cmd->BindPipeline(pso);
        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, draw.cbSlot, cb_);
        cmd->Draw(vertexCount);
    };

    if (style.faces)
        issue(core::MeshOverlayPass::Faces, false, g.layout.faceCount * 3);
    else if (g.markedFaces)
        issue(core::MeshOverlayPass::Faces, true, g.layout.faceCount * 3);
    if (style.edges)
        issue(core::MeshOverlayPass::Edges, false, g.layout.edgeCount * 6);
    if (style.vertices)
        issue(core::MeshOverlayPass::Vertices, false, g.layout.vertexCount * 6);
}

} // namespace whiteout::flakes::renderer::mesh_overlay
