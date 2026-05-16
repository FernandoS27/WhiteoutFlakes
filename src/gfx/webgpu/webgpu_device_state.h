#pragma once

// Aggregate device state + free-function declarations for the lifetime
// utilities (deferred-delete drain, swap-chain acquire, shared CB ring).
//
// Mirrors src/gfx/vulkan/vulkan_device_state.h.

#include "gfx/common/slot_map.h"
#include "webgpu_handles.h"

#include <webgpu/webgpu_cpp.h>

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace whiteout::flakes::gfx::webgpu {

struct WebGPUDeviceState {
    // Public WebGPU handles only — no Dawn-private types. The prebuilt
    // WebGPU-distribution doesn't expose dawn::native::*.
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Queue queue;

    // Single shared CB upload buffer — sub-allocated by CreateBuffer for
    // CpuWritable+Constant buffers. Persistently mapped via MapAtCreation
    // and queue.WriteBuffer; we hand out (buffer, offset) pairs to
    // SetBindGroup's dynamic-offset machinery.
    wgpu::Buffer sharedCbBuffer;
    u8* sharedCbMapped = nullptr;
    u64 sharedCbCursor = 0;

    // Uniform-buffer minimum offset alignment, queried from the adapter.
    // WebGPU spec floor is 256 bytes (matches D3D12 / Vulkan AMD).
    u64 minUniformBufferAlign = 256;

    // Shared pipeline layout — every renderer PSO uses the same three
    // bind-group layouts (CB / SRV / sampler), one slot per binding.
    // Mirrors the descriptor-set-layout split in
    // src/gfx/vulkan/vulkan_device_state.h.
    wgpu::BindGroupLayout cbBgLayout;
    wgpu::BindGroupLayout srvBgLayout;
    wgpu::BindGroupLayout samplerBgLayout;
    wgpu::PipelineLayout pipelineLayout;

    // Default sampler / SRV used to fill any bind slot the renderer
    // didn't populate — WebGPU rejects bind groups with holes, so we
    // back-fill at FlushBindings time.
    wgpu::Sampler defaultSampler;
    wgpu::Texture defaultTexture;
    wgpu::TextureView defaultTextureView;

    SlotMap<BufferEntry> buffers;
    SlotMap<TextureEntry> textures;
    SlotMap<ShaderEntry> shaders;
    SlotMap<PipelineEntry> pipelines;
    SlotMap<SamplerEntry> samplers;
    SlotMap<SwapChainEntry> swapchains;

    // Per-frame command encoders. Unlike Vulkan we don't reset pools —
    // each frame opens a fresh encoder via device.CreateCommandEncoder
    // and submits it via queue.Submit(encoder.Finish()) at Present().
    std::array<FrameContext, kFramesInFlight> frames{};
    u32 frameIndex = 0;

    // Deferred-delete state. Destroy() tags entries with pendingEpoch;
    // EnsureRecording drains those with epoch <= completedEpoch on the
    // next frame's first BeginRenderPass.
    //
    // `completedEpoch` is written from Dawn's worker thread inside the
    // OnSubmittedWorkDone callback; reading it on the renderer thread is
    // safe because std::atomic gives us the acquire/release pair we need.
    DeleteEpoch pendingEpoch = 0;
    std::atomic<DeleteEpoch> completedEpoch{0};
    std::deque<PendingDelete> pendingDeletes;
    std::mutex deleteMutex; // pendingDeletes is touched from Destroy() (main) only

    bool enableValidation = false;
};

// Drain entries whose epoch the GPU has already acked. Called at the top
// of each frame from EnsureEncoderOpen (webgpu_command_list.cpp).
void DrainPendingDeletes(WebGPUDeviceState& state);

// Acquire the surface's current texture and re-point the proxy entries.
// No-op if already acquired this frame.
void AcquireSwapChainImageIfNeeded(WebGPUDeviceState& state, SwapChainEntry& sc);

// Submit the current frame's encoder (if any), bump `pendingEpoch`, and
// post an OnSubmittedWorkDone callback that bumps `completedEpoch`. Used
// by both Present() (with `present=true`) and the implicit end-of-frame
// flush when the renderer never touches the swap chain.
void SubmitFrameAndBumpEpoch(WebGPUDeviceState& state);

} // namespace whiteout::flakes::gfx::webgpu
