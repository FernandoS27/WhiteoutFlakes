#include "bls_pso_builder.h"

#include <array>
#include <cstddef>

namespace WhiteoutDex::bls {

namespace {

constexpr gfx::InputElement kMeshSD[] = {
    { "ATTR", 0, gfx::Format::R32G32B32_FLOAT, 0,  0 },
    { "ATTR", 1, gfx::Format::R32G32B32_FLOAT, 12, 0 },
    { "ATTR", 3, gfx::Format::R32G32_FLOAT,    24, 0 },
};

constexpr gfx::InputElement kMeshSDTc2[] = {
    { "ATTR", 0, gfx::Format::R32G32B32_FLOAT, 0,  0 },
    { "ATTR", 1, gfx::Format::R32G32B32_FLOAT, 12, 0 },
    { "ATTR", 3, gfx::Format::R32G32_FLOAT,    24, 0 },
    { "ATTR", 4, gfx::Format::R32G32_FLOAT,    32, 0 },
};

constexpr gfx::InputElement kMeshSDSkinned[] = {
    { "ATTR", 0, gfx::Format::R32G32B32_FLOAT, 0,  0 },
    { "ATTR", 1, gfx::Format::R32G32B32_FLOAT, 12, 0 },
    { "ATTR", 3, gfx::Format::R32G32_FLOAT,    24, 0 },
    { "ATTR", 5, gfx::Format::R8G8B8A8_UNORM,  0,  1 },
    { "ATTR", 6, gfx::Format::R8G8B8A8_UINT,   4,  1 },
};

constexpr gfx::InputElement kParticleSD[] = {
    { "ATTR", 0, gfx::Format::R32G32B32_FLOAT,    0,  0 },
    { "ATTR", 1, gfx::Format::R32G32B32_FLOAT,    12, 0 },
    { "ATTR", 2, gfx::Format::R32G32B32A32_FLOAT, 24, 0 },
    { "ATTR", 3, gfx::Format::R32G32_FLOAT,       40, 0 },
};

constexpr gfx::InputElement kParticleSDSkinned[] = {
    { "ATTR", 0, gfx::Format::R32G32B32_FLOAT,    0,  0 },
    { "ATTR", 1, gfx::Format::R32G32B32_FLOAT,    12, 0 },
    { "ATTR", 2, gfx::Format::R32G32B32A32_FLOAT, 24, 0 },
    { "ATTR", 3, gfx::Format::R32G32_FLOAT,       40, 0 },
    { "ATTR", 5, gfx::Format::R8G8B8A8_UNORM,     0,  1 },
    { "ATTR", 6, gfx::Format::R8G8B8A8_UINT,      4,  1 },
};

constexpr gfx::InputElement kMeshHDTangent[] = {
    { "ATTR", 0, gfx::Format::R32G32B32_FLOAT,    0,  0 },
    { "ATTR", 1, gfx::Format::R32G32B32_FLOAT,    12, 0 },
    { "ATTR", 2, gfx::Format::R32G32B32A32_FLOAT, 24, 0 },
    { "ATTR", 3, gfx::Format::R32G32_FLOAT,       40, 0 },
    { "ATTR", 7, gfx::Format::R32G32B32A32_FLOAT, 0,  1 },
};

constexpr gfx::InputElement kMeshHDSkinned[] = {
    { "ATTR", 0, gfx::Format::R32G32B32_FLOAT,    0,  0 },
    { "ATTR", 1, gfx::Format::R32G32B32_FLOAT,    12, 0 },
    { "ATTR", 2, gfx::Format::R32G32B32A32_FLOAT, 24, 0 },
    { "ATTR", 3, gfx::Format::R32G32_FLOAT,       40, 0 },
    { "ATTR", 7, gfx::Format::R32G32B32A32_FLOAT, 0,  1 },
    { "ATTR", 5, gfx::Format::R8G8B8A8_UNORM,     0,  2 },
    { "ATTR", 6, gfx::Format::R8G8B8A8_UINT,      4,  2 },
};

constexpr gfx::InputElement kMeshHDSkinnedNoTangent[] = {
    { "ATTR", 0, gfx::Format::R32G32B32_FLOAT,    0,  0 },
    { "ATTR", 1, gfx::Format::R32G32B32_FLOAT,    12, 0 },
    { "ATTR", 2, gfx::Format::R32G32B32A32_FLOAT, 24, 0 },
    { "ATTR", 3, gfx::Format::R32G32_FLOAT,       40, 0 },
    { "ATTR", 5, gfx::Format::R8G8B8A8_UNORM,     0,  1 },
    { "ATTR", 6, gfx::Format::R8G8B8A8_UINT,      4,  1 },
};

}

std::span<const gfx::InputElement> LayoutFor(VertexLayoutKind k) {
    switch (k) {
        case VertexLayoutKind::MeshSD:        return {kMeshSD,        std::size(kMeshSD)};
        case VertexLayoutKind::MeshSDTc2:     return {kMeshSDTc2,     std::size(kMeshSDTc2)};
        case VertexLayoutKind::MeshSDSkinned: return {kMeshSDSkinned, std::size(kMeshSDSkinned)};
        case VertexLayoutKind::ParticleSD:    return {kParticleSD,    std::size(kParticleSD)};
        case VertexLayoutKind::ParticleSDSkinned:
            return {kParticleSDSkinned, std::size(kParticleSDSkinned)};
        case VertexLayoutKind::MeshHDTangent: return {kMeshHDTangent, std::size(kMeshHDTangent)};
        case VertexLayoutKind::MeshHDSkinned: return {kMeshHDSkinned, std::size(kMeshHDSkinned)};
        case VertexLayoutKind::MeshHDSkinnedNoTangent:
            return {kMeshHDSkinnedNoTangent, std::size(kMeshHDSkinnedNoTangent)};
    }
    return {kMeshSD, std::size(kMeshSD)};
}

namespace {

gfx::BlendDesc BlendFor(GxMatAlpha alpha) {

    gfx::BlendDesc bd{};
    switch (alpha) {
        case GxMatAlpha::Opaque:
        case GxMatAlpha::AlphaKey:

            bd.enable = false;
            break;
        case GxMatAlpha::Blend:

            bd.enable   = true;
            bd.srcColor = gfx::BlendFactor::SrcAlpha;
            bd.dstColor = gfx::BlendFactor::InvSrcAlpha;
            bd.srcAlpha = gfx::BlendFactor::One;
            bd.dstAlpha = gfx::BlendFactor::Zero;
            break;
        case GxMatAlpha::Add:

            bd.enable   = true;
            bd.srcColor = gfx::BlendFactor::SrcAlpha;
            bd.dstColor = gfx::BlendFactor::One;
            bd.srcAlpha = gfx::BlendFactor::Zero;
            bd.dstAlpha = gfx::BlendFactor::One;
            break;
        case GxMatAlpha::Modulate:

            bd.enable   = true;
            bd.srcColor = gfx::BlendFactor::DstColor;
            bd.dstColor = gfx::BlendFactor::Zero;
            bd.srcAlpha = gfx::BlendFactor::DstAlpha;
            bd.dstAlpha = gfx::BlendFactor::Zero;
            break;
        case GxMatAlpha::Modulate2X:

            bd.enable   = true;
            bd.srcColor = gfx::BlendFactor::DstColor;
            bd.dstColor = gfx::BlendFactor::SrcColor;
            bd.srcAlpha = gfx::BlendFactor::DstAlpha;
            bd.dstAlpha = gfx::BlendFactor::SrcAlpha;
            break;
    }
    return bd;
}

gfx::DepthStencilDesc DepthFor(const MatParams& m) {
    gfx::DepthStencilDesc ds{};
    ds.depthTest    = m.DepthTestEnabled();
    ds.depthWrite   = m.DepthWriteEnabled();
    ds.depthCompare = gfx::CompareOp::LessEqual;
    return ds;
}

gfx::RasterizerDesc RasterFor(const MatParams& m, bool wireframe, bool lhClipSpace) {
    (void)lhClipSpace;
    gfx::RasterizerDesc r{};
    r.cull     = m.CullEnabled() ? gfx::CullMode::Back : gfx::CullMode::None;
    r.fill     = wireframe ? gfx::FillMode::Wireframe : gfx::FillMode::Solid;

    r.frontCCW = true;
    return r;
}

u64 HashRequest(const PsoRequest& r) {
    u64 k = reinterpret_cast<uintptr_t>(r.program);
    k ^= u64(r.vsIndex) * 0x9E3779B185EBCA87ull;
    k ^= u64(r.psIndex) * 0xC2B2AE3D27D4EB4Full;
    u32 bits =
        (u32(r.material.alpha)      & 0x07u)        |
        ((r.material.disables            & 0x1Fu) << 3)  |
        ((u32(r.layout)             & 0x03u) << 8)  |
        ((u32(r.topology)           & 0x03u) << 10) |
        ((u32(r.rtvFormat)          & 0xFFu) << 12) |
        ((u32(r.dsvFormat)          & 0xFFu) << 20) |
        ((r.wireframe ? 1u : 0u)                  << 28) |
        ((r.lhClipSpace ? 1u : 0u)                << 29) |

        ((r.material.ColorWriteEnabled() ? 0u : 1u) << 30);
    k ^= u64(bits) * 0xFF51AFD7ED558CCDull;
    return k;
}

}

BlsPsoBuilder::BlsPsoBuilder(gfx::IGFXDevice* device) : device_(device) {}

BlsPsoBuilder::~BlsPsoBuilder() { Clear(); }

gfx::PipelineHandle BlsPsoBuilder::GetOrBuild(const PsoRequest& request) {
    if (!device_ || !request.program || !request.program->IsValid()) {
        return gfx::PipelineHandle::Invalid;
    }
    if (request.vsIndex >= request.program->vs->PermuteCount() ||
        request.psIndex >= request.program->ps->PermuteCount()) {
        return gfx::PipelineHandle::Invalid;
    }

    const u64 key = HashRequest(request);
    if (auto it = cache_.find(key); it != cache_.end()) {
        return it->second;
    }

    gfx::GraphicsPipelineDesc desc{};
    desc.vs           = request.program->vs->permuteHandles[request.vsIndex];
    desc.ps           = request.program->ps->permuteHandles[request.psIndex];
    desc.inputLayout  = LayoutFor(request.layout);
    desc.topology     = request.topology;
    desc.blend        = BlendFor(request.material.alpha);
    desc.blend.colorWrite = request.material.ColorWriteEnabled();
    desc.depthStencil = DepthFor(request.material);
    desc.rasterizer   = RasterFor(request.material, request.wireframe, request.lhClipSpace);
    desc.rtvFormat    = request.rtvFormat;
    desc.dsvFormat    = request.dsvFormat;

    gfx::PipelineHandle pso = device_->CreateGraphicsPipeline(desc);
    if (pso != gfx::PipelineHandle::Invalid) {
        cache_.emplace(key, pso);
    }
    return pso;
}

void BlsPsoBuilder::Clear() {
    if (!device_) { cache_.clear(); return; }
    for (auto& [k, pso] : cache_) {
        device_->Destroy(pso);
    }
    cache_.clear();
}

}
