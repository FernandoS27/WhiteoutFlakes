#include "profiles/wc3/wc3_debug_programs.h"

#include "renderer/bls/bls_program.h"
#include "renderer/bls/bls_shader_cache.h"
#include "renderer/types.h" // Vertex, BoneVertex

#include "compiled_shaders.h"

#include <cstddef>

namespace whiteout::flakes::renderer::profiles::wc3 {

void Wc3DebugPrograms::Init() {
    if (initTried_ || !gfx_)
        return;
    initTried_ = true;

    using namespace whiteout::flakes::Shaders;
    const gfx::GfxApi api = gfx_->GetApi();
    auto make = [&](gfx::ShaderStage stage, const u8* dxbc, usize dxbcN, const u8* spv, usize spvN,
                    const u8* wgsl, usize wgslN, const u8* mtl, usize mtlN) {
        switch (api) {
        case gfx::GfxApi::Vulkan:
            return gfx_->CreateShader(stage, spv, spvN);
        case gfx::GfxApi::WebGPU:
            return gfx_->CreateShader(stage, wgsl, wgslN);
        case gfx::GfxApi::Metal:
            return gfx_->CreateShader(stage, mtl, mtlN);
        default:
            return gfx_->CreateShader(stage, dxbc, dxbcN);
        }
    };
    const auto ps = gfx::ShaderStage::Pixel;
    const auto vs = gfx::ShaderStage::Vertex;
#define WDX_DEBUG_BLOB(name)                                                                       \
    k##name, sizeof(k##name), k##name##Spv, sizeof(k##name##Spv), k##name##Wgsl,                   \
        sizeof(k##name##Wgsl), k##name##Mtl, sizeof(k##name##Mtl)
    hd_[0] = make(ps, WDX_DEBUG_BLOB(Wc3HdDebugPS));
    hd_[1] = make(ps, WDX_DEBUG_BLOB(Wc3HdDebugCsmPS));
    hd_[2] = make(ps, WDX_DEBUG_BLOB(Wc3HdDebugCubePS));
    hd_[3] = make(ps, WDX_DEBUG_BLOB(Wc3HdDebugCsmCubePS));
    sd_ = make(ps, WDX_DEBUG_BLOB(Wc3SdDebugPS));
    sdNormalVs_ = make(vs, WDX_DEBUG_BLOB(Wc3SdDebugVS));
    sdNormalVsSkinned_ = make(vs, WDX_DEBUG_BLOB(Wc3SdDebugSkinnedVS));
    sdNormalPs_ = make(ps, WDX_DEBUG_BLOB(Wc3SdDebugNormalPS));
    hdOverlay_[0] = make(vs, WDX_DEBUG_BLOB(Wc3HdOverlayVS));
    hdOverlay_[1] = make(vs, WDX_DEBUG_BLOB(Wc3HdOverlayPaletteVS));
    hdOverlay_[2] = make(vs, WDX_DEBUG_BLOB(Wc3HdOverlayBoneBufferVS));
    sdOverlay_[0] = make(vs, WDX_DEBUG_BLOB(Wc3SdOverlayVS));
    sdOverlay_[1] = make(vs, WDX_DEBUG_BLOB(Wc3SdOverlaySkinnedVS));
#undef WDX_DEBUG_BLOB
    if (api == gfx::GfxApi::D3D12) {
        // A one-byte blob is the embed script's stub: no dxc at build time.
        auto dxil = [&](const u8* bytes, usize size) {
            return size > 1 ? gfx_->CreateShader(ps, bytes, size) : gfx::ShaderHandle::Invalid;
        };
        hdDxil_[0] = dxil(kWc3HdDebugPSDxil, sizeof(kWc3HdDebugPSDxil));
        hdDxil_[1] = dxil(kWc3HdDebugCsmPSDxil, sizeof(kWc3HdDebugCsmPSDxil));
        hdDxil_[2] = dxil(kWc3HdDebugCubePSDxil, sizeof(kWc3HdDebugCubePSDxil));
        hdDxil_[3] = dxil(kWc3HdDebugCsmCubePSDxil, sizeof(kWc3HdDebugCsmCubePSDxil));
        sdDxil_ = dxil(kWc3SdDebugPSDxil, sizeof(kWc3SdDebugPSDxil));
    }

    // Written per draw, like the BLS per-draw banks it sits beside.
    constexpr u32 kCbRingSlots = 4096;
    cb_ = gfx_->CreateBuffer({
        .size = sizeof(core::DebugViewCbData),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kCbRingSlots,
    });
}

void Wc3DebugPrograms::Release() {
    if (!gfx_)
        return;
    for (auto& [key, pso] : sdNormalPsos_) {
        if (pso != gfx::PipelineHandle::Invalid)
            gfx_->Destroy(pso);
    }
    sdNormalPsos_.clear();
    for (auto* h : {&sdNormalVs_, &sdNormalVsSkinned_, &sdNormalPs_}) {
        if (*h != gfx::ShaderHandle::Invalid)
            gfx_->Destroy(*h);
        *h = gfx::ShaderHandle::Invalid;
    }
    for (auto& h : hd_) {
        if (h != gfx::ShaderHandle::Invalid)
            gfx_->Destroy(h);
        h = gfx::ShaderHandle::Invalid;
    }
    for (auto& h : hdDxil_) {
        if (h != gfx::ShaderHandle::Invalid)
            gfx_->Destroy(h);
        h = gfx::ShaderHandle::Invalid;
    }
    if (sdDxil_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(sdDxil_);
    sdDxil_ = gfx::ShaderHandle::Invalid;
    for (auto& h : hdOverlay_) {
        if (h != gfx::ShaderHandle::Invalid)
            gfx_->Destroy(h);
        h = gfx::ShaderHandle::Invalid;
    }
    for (auto& h : sdOverlay_) {
        if (h != gfx::ShaderHandle::Invalid)
            gfx_->Destroy(h);
        h = gfx::ShaderHandle::Invalid;
    }
    if (sd_ != gfx::ShaderHandle::Invalid)
        gfx_->Destroy(sd_);
    if (cb_ != gfx::BufferHandle::Invalid)
        gfx_->Destroy(cb_);
    sd_ = gfx::ShaderHandle::Invalid;
    cb_ = gfx::BufferHandle::Invalid;
    initTried_ = false;
}

gfx::ShaderHandle Wc3DebugPrograms::Hd(bool cascades, bool pointShadows, bool dxil) {
    Init();
    const u32 i = (cascades ? 1u : 0u) | (pointShadows ? 2u : 0u);
    return dxil ? hdDxil_[i] : hd_[i];
}

gfx::ShaderHandle Wc3DebugPrograms::Sd(bool dxil) {
    Init();
    return dxil ? sdDxil_ : sd_;
}

bool Wc3DebugPrograms::IsDxil(const bls::BlsProgram* program) {
    return program && program->vs &&
           program->vs->container.PlatformTag() == bls::kPlatformTag_DX6;
}

gfx::ShaderHandle Wc3DebugPrograms::HdOverlay(bool skinned, bool boneBuffer) {
    Init();
    return hdOverlay_[skinned ? (boneBuffer ? 2 : 1) : 0];
}

gfx::ShaderHandle Wc3DebugPrograms::SdOverlay(bool skinned) {
    Init();
    return sdOverlay_[skinned ? 1 : 0];
}

gfx::PipelineHandle Wc3DebugPrograms::SdNormalPso(const bls::PsoRequest& req, bool skinned) {
    Init();
    const gfx::ShaderHandle vsHandle = skinned ? sdNormalVsSkinned_ : sdNormalVs_;
    if (vsHandle == gfx::ShaderHandle::Invalid || sdNormalPs_ == gfx::ShaderHandle::Invalid)
        return gfx::PipelineHandle::Invalid;

    SdNormalKey key;
    key.skinned = skinned;
    key.alpha = static_cast<u32>(req.material.alpha);
    key.disables = req.material.disables;
    key.rtv = req.rtvFormat;
    key.dsv = req.dsvFormat;
    key.extraRtvCount = req.extraRtvCount;
    key.extra0 = req.extraRtvFormats[0];
    key.extra1 = req.extraRtvFormats[1];
    key.extra2 = req.extraRtvFormats[2];
    if (auto it = sdNormalPsos_.find(key); it != sdNormalPsos_.end())
        return it->second;

    // Wc3SdDebugVSIn's field order: Vulkan, WebGPU and Metal derive a
    // location from where an element sits in this array.
    const gfx::InputElement elements[] = {
        {"POSITION", 0, gfx::Format::R32G32B32_FLOAT, offsetof(Vertex, position), 0},
        {"NORMAL", 0, gfx::Format::R32G32B32_FLOAT, offsetof(Vertex, normal), 0},
        {"COLOR", 0, gfx::Format::R32G32B32A32_FLOAT, offsetof(Vertex, color), 0},
        {"TEXCOORD", 0, gfx::Format::R32G32_FLOAT, offsetof(Vertex, uv), 0},
        {"BLENDWEIGHT", 0, gfx::Format::R8G8B8A8_UNORM, offsetof(BoneVertex, weights), 1},
        {"BLENDINDICES", 0, gfx::Format::R8G8B8A8_UINT, offsetof(BoneVertex, indices), 1},
    };

    gfx::GraphicsPipelineDesc desc{};
    desc.vs = vsHandle;
    desc.ps = sdNormalPs_;
    desc.inputLayout = std::span<const gfx::InputElement>(elements, skinned ? 6u : 4u);
    desc.inputSlotStrides[0] = sizeof(Vertex);
    desc.inputSlotStrides[1] = sizeof(BoneVertex);
    desc.topology = gfx::PrimitiveTopology::TriangleList;
    desc.blend = bls::BlendFor(req.material.alpha);
    desc.blend.colorWrite = req.material.ColorWriteEnabled();
    desc.depthStencil = bls::DepthFor(req.material);
    desc.rasterizer.cull = req.material.CullEnabled() ? gfx::CullMode::Back : gfx::CullMode::None;
    desc.rasterizer.frontCCW = true;
    desc.rtvFormat = req.rtvFormat;
    desc.dsvFormat = req.dsvFormat;
    desc.extraRtvCount = req.extraRtvCount;
    for (u32 i = 0; i < gfx::GraphicsPipelineDesc::kMaxExtraColorAttachments; ++i)
        desc.extraRtvFormats[i] = req.extraRtvFormats[i];

    const gfx::PipelineHandle pso = gfx_->CreateGraphicsPipeline(desc);
    sdNormalPsos_.emplace(key, pso);
    return pso;
}

gfx::BufferHandle Wc3DebugPrograms::Write(const core::DebugViewCbData& data) {
    Init();
    if (cb_ == gfx::BufferHandle::Invalid)
        return cb_;
    if (auto* p = static_cast<core::DebugViewCbData*>(gfx_->MapBuffer(cb_))) {
        *p = data;
        gfx_->UnmapBuffer(cb_);
    }
    return cb_;
}

} // namespace whiteout::flakes::renderer::profiles::wc3
