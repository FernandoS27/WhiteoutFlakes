#pragma once

// ============================================================================
// FrameCapture — optional GPU frame grabber for PNG / GIF export.
//
// When enabled, the renderer's final composite is redirected to an off-screen
// target; a compute shader copies it into a ring of CPU-readable buffers, and
// a full-screen blit mirrors it back onto the swap chain so the frame still
// displays. Off and zero-cost on the normal render path.
//
// Owned by RenderPipeline. The per-frame contract is BeginFrame (before the
// composite) + EndFrame (after it); DownloadSlot pulls a finished frame back
// to the CPU once its GPU work has been waited on.
// ============================================================================

#include "gfx/gfx.h"
#include "render_target.h"

#include <array>
#include <vector>

namespace whiteout::flakes::renderer {

class FrameCapture {
public:
    // Several captured frames can be in flight before the consumer syncs the
    // GPU and drains the ring — one {UAV, readback} buffer pair per slot.
    static constexpr i32 kRingSize = 4;

    // Builds the device-lifetime shaders + compute PSO. `api` selects the
    // shader bytecode variant; `depthFormat` feeds the lazily-built blit PSO.
    void Init(gfx::IGFXDevice& gfx, gfx::GfxApi api, gfx::Format depthFormat);
    void Shutdown();

    void SetEnabled(bool enable);
    bool IsEnabled() const {
        return enabled_;
    }
    i32 RingSize() const {
        return kRingSize;
    }
    // Ring slot the most recent BeginFrame/EndFrame pair captured into, or -1.
    i32 LastSlot() const {
        return lastSlot_;
    }

    // Per-frame hooks. BeginFrame returns the texture the composite should
    // render into — the off-screen capture target when capturing, otherwise
    // the swap-chain back buffer. EndFrame runs the copy compute + blits the
    // result onto the swap chain; it is a no-op unless BeginFrame captured.
    gfx::TextureHandle BeginFrame(const RenderTarget& target);
    void EndFrame(const RenderTarget& target);

    // Copy a ring slot out as tightly-packed RGBA8 (row pitch = width*4). The
    // slot's GPU work must already have completed (caller WaitIdle's first).
    bool DownloadSlot(i32 slot, std::vector<u8>& outRgba, i32& width, i32& height);

private:
    bool EnsureResources(const RenderTarget& target);
    void ReleaseResources();

    gfx::IGFXDevice* gfx_ = nullptr;
    gfx::Format depthFormat_ = gfx::Format::D24_UNORM_S8_UINT;

    bool enabled_ = false;
    bool frameCapturing_ = false; // BeginFrame redirected this frame
    bool resourcesLogged_ = false; // one-shot gate for the failure diagnostic
    i32 width_ = 0;
    i32 height_ = 0;
    i32 ringCursor_ = 0; // next ring slot a captured frame writes
    i32 lastSlot_ = -1;

    // Device-lifetime: shaders + the compute PSO. The blit PSO is rebuilt
    // lazily once the swap-chain format is known.
    gfx::ShaderHandle blitVS_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle blitPS_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle captureCS_ = gfx::ShaderHandle::Invalid;
    gfx::PipelineHandle capturePSO_ = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle blitPSO_ = gfx::PipelineHandle::Invalid;
    gfx::Format blitPsoFormat_ = gfx::Format::Unknown;
    gfx::SamplerHandle sampler_ = gfx::SamplerHandle::Invalid;
    gfx::BufferHandle paramsCb_ = gfx::BufferHandle::Invalid;

    // Per-surface: the off-screen target + the readback ring.
    gfx::TextureHandle color_ = gfx::TextureHandle::Invalid;
    struct Slot {
        gfx::BufferHandle uav = gfx::BufferHandle::Invalid;      // compute write target
        gfx::BufferHandle readback = gfx::BufferHandle::Invalid; // CPU-mappable copy
    };
    std::array<Slot, kRingSize> ring_{};
};

} // namespace whiteout::flakes::renderer
