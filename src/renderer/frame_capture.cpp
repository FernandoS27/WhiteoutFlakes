// ============================================================================
// FrameCapture — see frame_capture.h.
// ============================================================================

#include "renderer/frame_capture.h"

#include "compiled_shaders.h"

#include <cstdio>
#include <cstring>

namespace whiteout::flakes::renderer {

void FrameCapture::Init(gfx::IGFXDevice& gfx, gfx::GfxApi api, gfx::Format depthFormat) {
    gfx_ = &gfx;
    depthFormat_ = depthFormat;

    // Per-backend bytecode selection (mirrors RenderPipeline::CreateShaders):
    // Vulkan → SPIR-V, WebGPU → WGSL, D3D11/12 → DXBC.
    using namespace whiteout::flakes::Shaders;
    const u8* blitVs = kBlitVS;
    usize blitVsN = sizeof(kBlitVS);
    const u8* blitPs = kBlitPS;
    usize blitPsN = sizeof(kBlitPS);
    const u8* capCs = kCaptureCS;
    usize capCsN = sizeof(kCaptureCS);
    if (api == gfx::GfxApi::Vulkan) {
        blitVs = kBlitVSSpv;
        blitVsN = sizeof(kBlitVSSpv);
        blitPs = kBlitPSSpv;
        blitPsN = sizeof(kBlitPSSpv);
        capCs = kCaptureCSSpv;
        capCsN = sizeof(kCaptureCSSpv);
    } else if (api == gfx::GfxApi::WebGPU) {
        blitVs = kBlitVSWgsl;
        blitVsN = sizeof(kBlitVSWgsl);
        blitPs = kBlitPSWgsl;
        blitPsN = sizeof(kBlitPSWgsl);
        capCs = kCaptureCSWgsl;
        capCsN = sizeof(kCaptureCSWgsl);
    }
    blitVS_ = gfx_->CreateShader(gfx::ShaderStage::Vertex, blitVs, blitVsN);
    blitPS_ = gfx_->CreateShader(gfx::ShaderStage::Pixel, blitPs, blitPsN);
    captureCS_ = gfx_->CreateShader(gfx::ShaderStage::Compute, capCs, capCsN);

    // The copy compute PSO has no render-target dependency, so build it now;
    // the blit PSO needs the swap-chain format and is built in EnsureResources.
    if (captureCS_ != gfx::ShaderHandle::Invalid) {
        gfx::ComputePipelineDesc cpd;
        cpd.cs = captureCS_;
        capturePSO_ = gfx_->CreateComputePipeline(cpd);
    }
    paramsCb_ = gfx_->CreateBuffer({
        .size = 16,
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });
    gfx::SamplerDesc sd;
    sd.minFilter = gfx::Filter::Linear;
    sd.magFilter = gfx::Filter::Linear;
    sd.addressU = gfx::AddressMode::Clamp;
    sd.addressV = gfx::AddressMode::Clamp;
    sd.addressW = gfx::AddressMode::Clamp;
    sampler_ = gfx_->CreateSampler(sd);
}

void FrameCapture::Shutdown() {
    if (!gfx_)
        return;
    ReleaseResources();
    gfx_->Destroy(blitVS_);
    gfx_->Destroy(blitPS_);
    gfx_->Destroy(captureCS_);
    gfx_->Destroy(capturePSO_);
    gfx_->Destroy(blitPSO_);
    gfx_->Destroy(sampler_);
    gfx_->Destroy(paramsCb_);
    blitVS_ = blitPS_ = captureCS_ = gfx::ShaderHandle::Invalid;
    capturePSO_ = blitPSO_ = gfx::PipelineHandle::Invalid;
    blitPsoFormat_ = gfx::Format::Unknown;
    sampler_ = gfx::SamplerHandle::Invalid;
    paramsCb_ = gfx::BufferHandle::Invalid;
    gfx_ = nullptr;
}

void FrameCapture::SetEnabled(bool enable) {
    if (enabled_ == enable)
        return;
    enabled_ = enable;
    // Resources are allocated lazily by BeginFrame; free them on disable.
    if (!enable)
        ReleaseResources();
}

void FrameCapture::ReleaseResources() {
    if (!gfx_)
        return;
    gfx_->Destroy(color_);
    color_ = gfx::TextureHandle::Invalid;
    for (auto& slot : ring_) {
        gfx_->Destroy(slot.uav);
        gfx_->Destroy(slot.readback);
        slot.uav = gfx::BufferHandle::Invalid;
        slot.readback = gfx::BufferHandle::Invalid;
    }
    width_ = 0;
    height_ = 0;
    ringCursor_ = 0;
    lastSlot_ = -1;
}

bool FrameCapture::EnsureResources(const RenderTarget& target) {
    // One-shot diagnostic: capture silently no-ops if a resource is missing,
    // so log the first failure rather than leaving the user guessing.
    auto fail = [&](const char* why) -> bool {
        if (!resourcesLogged_) {
            std::fprintf(stderr, "[capture] disabled — %s\n", why);
            resourcesLogged_ = true;
        }
        return false;
    };

    if (!gfx_)
        return false;
    if (capturePSO_ == gfx::PipelineHandle::Invalid)
        return fail("copy compute pipeline failed to build");
    if (blitVS_ == gfx::ShaderHandle::Invalid || blitPS_ == gfx::ShaderHandle::Invalid)
        return fail("blit shaders failed to load");
    if (target.swap == gfx::SwapChainHandle::Invalid || target.width <= 0 || target.height <= 0)
        return fail("primary target is not a sized swap chain");

    // The blit PSO writes the swap-chain back buffer, so its rtvFormat must
    // match — built (and rebuilt) lazily once that format is known.
    const gfx::Format swapFmt = gfx_->GetSwapChainFormat(target.swap);
    if (blitPSO_ == gfx::PipelineHandle::Invalid || blitPsoFormat_ != swapFmt) {
        gfx_->Destroy(blitPSO_);
        gfx::GraphicsPipelineDesc bd;
        bd.vs = blitVS_;
        bd.ps = blitPS_;
        bd.topology = gfx::PrimitiveTopology::TriangleList;
        bd.blend.enable = false;
        bd.depthStencil.depthTest = false;
        bd.depthStencil.depthWrite = false;
        bd.rasterizer.cull = gfx::CullMode::None;
        bd.rasterizer.frontCCW = true;
        bd.rtvFormat = swapFmt;
        bd.dsvFormat = depthFormat_;
        blitPSO_ = gfx_->CreateGraphicsPipeline(bd);
        blitPsoFormat_ = swapFmt;
    }
    if (blitPSO_ == gfx::PipelineHandle::Invalid)
        return fail("blit pipeline failed to build");

    if (color_ != gfx::TextureHandle::Invalid && width_ == target.width && height_ == target.height)
        return true;

    // First use or surface resize — rebuild the capture target + ring.
    ReleaseResources();
    const i32 w = target.width, h = target.height;
    const u64 bytes = static_cast<u64>(w) * static_cast<u64>(h) * 4u;
    // The capture target matches the swap-chain format so the composite PSOs
    // (built for that format) stay valid when redirected to it.
    color_ = gfx_->CreateColorTarget(w, h, swapFmt);
    bool ok = (color_ != gfx::TextureHandle::Invalid);
    for (auto& slot : ring_) {
        slot.uav = gfx_->CreateBuffer({
            .size = bytes,
            .elementStride = 4,
            .usage = gfx::BufferUsage::UnorderedAccess,
        });
        slot.readback = gfx_->CreateBuffer({
            .size = bytes,
            .usage = gfx::BufferUsage::CpuReadable,
        });
        ok = ok && slot.uav != gfx::BufferHandle::Invalid &&
             slot.readback != gfx::BufferHandle::Invalid;
    }
    width_ = w;
    height_ = h;
    ringCursor_ = 0;
    if (!ok) {
        ReleaseResources();
        return fail("capture target / ring buffer allocation failed");
    }
    return true;
}

gfx::TextureHandle FrameCapture::BeginFrame(const RenderTarget& target) {
    frameCapturing_ = enabled_ && EnsureResources(target);
    lastSlot_ = -1; // EndFrame sets it if this frame captures
    return frameCapturing_ ? color_ : target.color;
}

void FrameCapture::EndFrame(const RenderTarget& target) {
    if (!frameCapturing_)
        return;
    auto* cmd = gfx_->GetImmediateContext();
    const u32 w = static_cast<u32>(width_);
    const u32 h = static_cast<u32>(height_);

    // Round-robin into the next ring slot so several captured frames can be
    // in flight before the consumer syncs and drains them.
    const i32 slot = ringCursor_;
    ringCursor_ = (slot + 1) % kRingSize;
    lastSlot_ = slot;
    const Slot& ring = ring_[slot];

    // Copy compute: capture target (SRV) → tightly-packed RGBA8 UAV buffer.
    if (paramsCb_ != gfx::BufferHandle::Invalid) {
        if (void* p = gfx_->MapBuffer(paramsCb_)) {
            const u32 dims[4] = {w, h, 0, 0};
            std::memcpy(p, dims, sizeof(dims));
            gfx_->UnmapBuffer(paramsCb_);
        }
    }
    cmd->BindPipeline(capturePSO_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Compute, 0, paramsCb_);
    cmd->BindShaderResource(gfx::ShaderStage::Compute, 0, color_);
    cmd->BindUnorderedAccess(0, ring.uav);
    cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

    // Stage into this slot's CPU-readable buffer for DownloadSlot.
    cmd->CopyBuffer(ring.readback, ring.uav);

    // Mirror the off-screen composite onto the swap chain so the frame still
    // displays while capture is redirecting it.
    const f32 clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    cmd->BeginRenderPass(target.color, gfx::TextureHandle::Invalid, clear, 1.0f, 0);
    cmd->SetViewport({0, 0, (f32)target.width, (f32)target.height, 0, 1});
    cmd->BindPipeline(blitPSO_);
    cmd->BindShaderResource(gfx::ShaderStage::Pixel, 0, color_);
    cmd->BindSampler(gfx::ShaderStage::Pixel, 0, sampler_);
    cmd->Draw(3, 0);
    cmd->EndRenderPass();
}

bool FrameCapture::DownloadSlot(i32 slot, std::vector<u8>& outRgba, i32& width, i32& height) {
    if (!gfx_ || slot < 0 || slot >= kRingSize)
        return false;
    const gfx::BufferHandle readback = ring_[slot].readback;
    if (readback == gfx::BufferHandle::Invalid || width_ <= 0 || height_ <= 0)
        return false;
    width = width_;
    height = height_;
    const usize bytes = static_cast<usize>(width) * static_cast<usize>(height) * 4u;
    void* p = gfx_->MapBuffer(readback);
    if (!p)
        return false;
    outRgba.resize(bytes);
    std::memcpy(outRgba.data(), p, bytes);
    gfx_->UnmapBuffer(readback);
    return true;
}

} // namespace whiteout::flakes::renderer
