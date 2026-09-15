#pragma once

// Per-resource entry types stored in WebGPUDeviceState's SlotMaps, plus
// the per-frame sync bundle and swap-chain state.

#include "gfx/gfx.h"

#include <webgpu/webgpu_cpp.h>

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace whiteout::flakes::gfx::webgpu {

inline constexpr u32 kFramesInFlight = 3;

// Map* on a CpuWritable+Constant buffer rotates through this many slots
// so per-draw writes don't stall the GPU. Sized for the worst-case
// in-flight draw count: kFramesInFlight × (per-frame draw count). With
// the corn-fx batching + splat coalescing landed in the perf pass, the
// realistic per-frame CB map count per buffer is in the low hundreds —
// 1024 leaves comfortable headroom without bloating the ring. Was 4096
// previously, which made each per-CB allocation a 1 MiB sub-alloc and
// blew up VRAM on Firefox.
inline constexpr u32 kCbRingSlots = 1024;

// Single shared CB upload buffer. CpuWritable buffers sub-alloc here;
// fall back to dedicated CreateBuffer (with per-buffer CPU shadow) when
// the cursor overflows. Was 256 MiB (right at the WebGPU spec default
// maxBufferSize), which Firefox refuses or honors-and-OOMs on lower-VRAM
// devices because the buffer is created with every usage bit set
// (Uniform|Storage|Vertex|Index|CopySrc|CopyDst) — drivers tend to
// allocate that out of the most restrictive heap. 64 MiB matches the
// Vulkan backend's `kSharedCbCapacity` and is plenty for our workload.
inline constexpr u64 kSharedCbCapacity = 64ull * 1024 * 1024;

// Binding numbers.
//
// The WGSL shaders put CBs in group 0, textures and storage buffers in group
// 1, samplers in group 2. VS resources use @binding(register); PS resources
// are shifted: CBs by kPsCbBindingOffsetWgsl (PsCbBindingOffset in
// cb_structs.slang), textures, buffers and samplers by kStageBindingShift.
// Bind* applies the same shift, so the pending arrays are indexed by the
// shader's @binding number.
//
// Bind-group layouts are not fixed: each pipeline gets the layouts its two
// WGSL modules declare (see ScanWgslBindings), so a shader's slot types and
// per-stage counts are whatever it says.
inline constexpr u32 kStageBindingShift = 12;
inline constexpr u32 kPsCbBindingOffsetWgsl = 4;
inline constexpr u32 kMaxBindGroups = 4;
inline constexpr u32 kMaxBindingIndex = 48;
// Zero-filled buffer bound to storage declarations nobody filled; covers a
// 256-bone palette of 48-byte transforms.
inline constexpr u64 kDefaultStorageBufferBytes = 16384;
// Size of the all-zero buffer behind phantom vertex attributes: 16 lanes.
inline constexpr u64 kZeroVertexBufferBytes = 256;

enum class WgslBindingKind : u8 {
    Uniform,
    ReadOnlyStorage,
    Storage,
    Texture,
    Sampler,
    ComparisonSampler,
};

// Which Bind* families feed a bind-group layout; a group is rebuilt only
// when one of its families changed.
inline constexpr u8 kBindKindConstant = 1 << 0;
inline constexpr u8 kBindKindResource = 1 << 1;
inline constexpr u8 kBindKindSampler = 1 << 2;

// One `@group(G) @binding(N) var ...` declaration of a WGSL module.
struct WgslBinding {
    u32 group = 0;
    u32 binding = 0;
    WgslBindingKind kind = WgslBindingKind::Uniform;
    wgpu::TextureSampleType sampleType = wgpu::TextureSampleType::Float;
    wgpu::TextureViewDimension viewDimension = wgpu::TextureViewDimension::e2D;
    bool multisampled = false;
};

// WebGPU has no notion of timeline semaphores. Every Present submits the
// frame's encoder and increments `pendingEpoch`; OnSubmittedWorkDone bumps
// `completedEpoch` once the GPU finishes. Destroy() tags entries with the
// current pendingEpoch and they drain when completedEpoch catches up.
using DeleteEpoch = u64;

struct BufferEntry {
    wgpu::Buffer buffer; // own buffer OR alias of shared CB
    BufferDesc desc{};
    u64 byteSize = 0; // ownership-bytes for GPU accounting (0 for ring aliases).

    // Ring-buffer slots (mirrors VulkanDevice's BufferEntry). Map* rotates
    // through `slotCount` slots of `slotStride` bytes; the active draw's
    // bind group picks the slot via currentOffset().
    u64 slotStride = 0;
    u32 slotCount = 1;
    u32 currentSlot = 0;
    u64 baseOffset = 0; // non-zero only for shared-CB sub-allocs
    bool isSharedRingAlias = false;

    // Mapped pointer when host-visible; null otherwise. For sub-allocs
    // this aliases WebGPUDeviceState::sharedCbMapped. For dedicated
    // CpuWritable buffers (shared-CB ring overflow) this points at
    // dedicatedShadow.data().
    u8* mapped = nullptr;

    // CPU shadow for the dedicated-buffer fallback path. WebGPU's sync
    // API has no MapWrite for Uniform/Vertex/Index, so dedicated
    // CpuWritable buffers go shadow → Queue::WriteBuffer on Unmap.
    std::vector<u8> dedicatedShadow;

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
    // Dimension of `view`. A binding that declares another dimension gets a
    // view from `altViews` (index = wgpu::TextureViewDimension value).
    wgpu::TextureViewDimension viewDimension = wgpu::TextureViewDimension::e2D;
    std::array<wgpu::TextureView, 7> altViews{};
    // Single-layer 2D views for BeginDepthSlicePass, created on first use.
    std::vector<wgpu::TextureView> sliceViews;
    wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
    i32 width = 0;
    i32 height = 0;
    i32 mipLevels = 1;
    i32 arraySize = 1;
    bool ownsTexture = true;
    bool isDepth = false;

    SwapChainHandle swapChainProxy = SwapChainHandle::Invalid;
    bool isLinearView = false;

    // Live GPU byte accounting (TextureBytes(desc) at create time).
    // Carried so the deferred-delete lambda can subtract on actual drop.
    u64 byteSize = 0;
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
    // the shader's declared type (see PipelineEntry::phantomVertexSlot).
    std::vector<VertexInputLocation> vertexLocations;

    // Every resource the module declares; the pipeline layout is built from these.
    std::vector<WgslBinding> bindings;
};

struct PipelineEntry {
    wgpu::RenderPipeline graphics;
    wgpu::ComputePipeline compute;
    bool isCompute = false;
    // Cross-checked against the active render pass's color format inside
    // WebGPUCommandList::BindPipeline.
    wgpu::TextureFormat colorFormat = wgpu::TextureFormat::Undefined;

    // Vertex slot carrying every attribute we added for VS @location()
    // declarations the renderer's InputLayout didn't cover, or -1. It sits
    // right after the real slots, so it can be a slot another pipeline binds
    // real data to: BindPipeline puts the shared zero buffer there and
    // restores the renderer's buffer when the next pipeline uses the slot.
    i32 phantomVertexSlot = -1;

    // Index into WebGPUDeviceState::bindLayouts for each group the layout has.
    u32 groupCount = 0;
    std::array<u32, kMaxBindGroups> groupLayouts{};
};

struct SamplerEntry {
    wgpu::Sampler sampler;
    bool comparison = false;
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
