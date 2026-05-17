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
// CBs: VS CBs live at @binding(0..3); PS CBs live at @binding(4..11).
// The slang side mirrors this split via PsCbBindingOffset (= 4 for
// WGSL_TARGET, 16 otherwise) in cb_structs.slang — BindConstantBuffer
// applies the same offset for the Pixel stage so the runtime and the
// shader agree on which BindGroup entry feeds each cbuffer. The total
// (12) matches the spec floor for maxUniformBuffersPerShaderStage so
// the layout fits on mobile (Mali / Adreno) WebGPU.
inline constexpr u32 kStageBindingShift = 12;
inline constexpr u32 kCbBindingCount = 12;
inline constexpr u32 kPsCbBindingOffsetWgsl = 4;
// 12 VS + 16 PS = 28. PS half maxes the spec-floor cap
// (maxSampledTexturesPerShaderStage = maxSamplersPerShaderStage = 16),
// which is enough to land slang's register(t15) (binding kStageBindingShift+15 = 27).
inline constexpr u32 kSrvBindingCount = 28;
inline constexpr u32 kSamplerBindingCount = 28;

// PS slots within the WGSL bind groups (group 1/2 indices = kStageBindingShift + register).
// Shadow maps and IBL cubemap arrays need distinct layout metadata, so
// the layout builder hardcodes their slots here.
inline constexpr u32 kPsShadowStartBinding = kStageBindingShift + 10; // register t10..t12 → 22..24
inline constexpr u32 kPsShadowEndBinding = kStageBindingShift + 13;   // exclusive
inline constexpr u32 kPsIblCubeFromBinding = kStageBindingShift + 13; // register t13 → 25
inline constexpr u32 kPsIblCubeToBinding = kStageBindingShift + 14;   // register t14 → 26

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

struct VertexInputLocation {
    u32 location = 0;
    std::string typeName; // raw WGSL type token, e.g. "vec4<u32>" / "vec3<f32>"
};

struct ShaderEntry {
    wgpu::ShaderModule module;
    ShaderStage stage = ShaderStage::Vertex;
    std::string entryPoint; // "main" by default; overridable for WGSL multi-entry modules

    // VS only: every @location(N) declared on the VS entry input struct,
    // with its WGSL type token. Populated at CreateShader time so
    // CreateGraphicsPipeline can spot gaps the InputLayout doesn't
    // cover and pad them with phantom attributes whose format matches
    // the shader's declared type (see PipelineEntry::phantomVertexSlots).
    std::vector<VertexInputLocation> vertexLocations;
};

struct PipelineEntry {
    wgpu::RenderPipeline graphics;
    wgpu::ComputePipeline compute;
    bool isCompute = false;
    // Cross-checked against the active render pass's color format inside
    // WebGPUCommandList::BindPipeline.
    wgpu::TextureFormat colorFormat = wgpu::TextureFormat::Undefined;

    // Vertex slot indices we added to satisfy VS @location() declarations
    // the renderer's InputLayout didn't cover. BindPipeline binds the
    // shared zero vertex buffer to each of these slots so missing
    // attributes don't crash the GPU (they just read zeros).
    std::vector<u32> phantomVertexSlots;
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
