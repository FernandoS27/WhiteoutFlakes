#pragma once

// Per-resource entry types stored in WebGPUDeviceState's SlotMaps, plus
// the per-frame sync bundle and swap-chain state.

#include "gfx/gfx.h"

#include <webgpu/webgpu_cpp.h>

#include <functional>
#include <string>
#include <vector>

namespace whiteout::flakes::gfx::webgpu {

inline constexpr u32 kFramesInFlight = 3;

// Mirrors VulkanDeviceState::kCbRingSlots — Map* on a CpuWritable+Constant
// buffer rotates through this many slots so per-frame writes don't stall.
// WebGPU exposes the dynamic-offset model directly; we sub-alloc out of a
// single shared buffer and hand a fresh offset to SetBindGroup.
inline constexpr u32 kCbRingSlots = 256;

// Single shared CB upload buffer — sized to match the Vulkan backend.
// CpuWritable buffers sub-alloc here; they fall back to dedicated
// CreateBuffer when the cursor overflows.
inline constexpr u64 kSharedCbCapacity = 64ull * 1024 * 1024;

// Binding slot counts.
//
// SRV / sampler: VS uses the lower half ([0, kStageBindingShift)); PS
// uses the upper half — slangc emits PS textures at @binding(16+) when
// kStageBindingShift==12 it'd use @binding(12+). We honor that split
// with per-binding visibility flags in the layout (see CreateSharedBindLayouts).
//
// CBs: slangc unifies uniform-buffer bindings across stages — both VS
// and PS reference the SAME @binding(N) for shared globals (the
// viewcube debug shader does this for its per-frame CB). So CBs use
// ONE binding namespace, sized to the per-stage uniform cap WebGPU
// enforces (12 — a spec ceiling). BindConstantBuffer ignores `stage`
// and writes to pendingCBs_[slot] directly.
inline constexpr u32 kStageBindingShift = 12;
inline constexpr u32 kCbBindingCount = 12;
inline constexpr u32 kSrvBindingCount = 24;
inline constexpr u32 kSamplerBindingCount = 24;

// WebGPU has no notion of timeline semaphores. Every Present submits the
// frame's encoder and increments `pendingEpoch`; OnSubmittedWorkDone bumps
// `completedEpoch` once the GPU finishes. Destroy() tags entries with the
// current pendingEpoch and they drain when completedEpoch catches up.
using DeleteEpoch = u64;

struct BufferEntry {
    wgpu::Buffer buffer;       // own buffer OR alias of shared CB
    BufferDesc desc{};

    // Ring-buffer slots (mirrors VulkanDevice's BufferEntry). Map* rotates
    // through `slotCount` slots of `slotStride` bytes; the active draw's
    // bind group picks the slot via currentOffset().
    u64 slotStride = 0;
    u32 slotCount = 1;
    u32 currentSlot = 0;
    u64 baseOffset = 0; // non-zero only for shared-CB sub-allocs
    bool isSharedRingAlias = false;

    // Mapped pointer when host-visible; null otherwise. For sub-allocs
    // this aliases WebGPUDeviceState::sharedCbMapped.
    u8* mapped = nullptr;

    u64 currentOffset() const {
        return baseOffset + slotStride * currentSlot;
    }
};

// Swap-chain back buffers are proxy textures: the renderer caches one
// handle at CreateSwapChain time and we re-point `texture`/`view` every
// frame inside Present-prep.
struct TextureEntry {
    wgpu::Texture texture;
    wgpu::TextureView view;       // sRGB view (or matching unaliased view)
    wgpu::TextureView viewLinear; // linear partner view; null when N/A
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    i32 width = 0;
    i32 height = 0;
    i32 mipLevels = 1;
    i32 arraySize = 1;
    bool ownsTexture = true;
    bool isDepth = false;

    SwapChainHandle swapChainProxy = SwapChainHandle::Invalid;
    bool isLinearView = false;
};

struct ShaderEntry {
    wgpu::ShaderModule module;
    ShaderStage stage = ShaderStage::Vertex;
    std::string entryPoint; // "main" by default; overridable for WGSL multi-entry modules
};

struct PipelineEntry {
    wgpu::RenderPipeline graphics;
    wgpu::ComputePipeline compute;
    bool isCompute = false;
    // Cross-checked against the active render pass's color format inside
    // WebGPUCommandList::BindPipeline.
    wgpu::TextureFormat colorFormat = wgpu::TextureFormat::Undefined;
};

struct SamplerEntry {
    wgpu::Sampler sampler;
};

struct SwapChainEntry {
    wgpu::Surface surface;
    wgpu::TextureFormat formatSrgb = wgpu::TextureFormat::Undefined;
    wgpu::TextureFormat formatLinear = wgpu::TextureFormat::Undefined;
    u32 width = 0;
    u32 height = 0;

    // The proxy TextureHandles the renderer caches at CreateSwapChain.
    // GetSwapChainBackBuffer returns these; AcquireIfNeeded re-points
    // them at the surface's current texture.
    TextureHandle proxySrgb = TextureHandle::Invalid;
    TextureHandle proxyLinear = TextureHandle::Invalid;

    // Surface::GetCurrentTexture is mutable per-frame; we hold the
    // wgpu::Texture so it stays alive until Present().
    wgpu::Texture currentTexture;
    bool acquiredThisFrame = false;
};

// Per-frame state. WebGPU's command-encoder model is simpler than
// Vulkan's: one encoder spans a frame, ends with `Finish()` at Present.
struct FrameContext {
    wgpu::CommandEncoder encoder;
    bool recording = false;
    DeleteEpoch epoch = 0;
};

// Deferred deletion entry. We don't store the wgpu::* handle directly —
// it's already moved into the lambda capture, so dropping the deleter
// after the GPU acks the epoch destroys the object.
struct PendingDelete {
    DeleteEpoch epoch = 0;
    std::function<void()> deleter;
};

} // namespace whiteout::flakes::gfx::webgpu
