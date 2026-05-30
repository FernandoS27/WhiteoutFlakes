#pragma once

// Slot-map payloads + per-frame state for the Metal backend.
//
// This header is included from .mm translation units only — it pulls in
// <Metal/Metal.h> and uses Obj-C types (id<MTLBuffer> etc.) directly.
// Pure C++ TUs interact with the backend through metal_device.h, which
// keeps every Obj-C type behind std::unique_ptr<MetalDeviceState>.

#include "gfx/common/slot_map.h"
#include "gfx/gfx.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace whiteout::flakes::gfx::metal {

// Three frames in flight is the modern-Metal sweet spot (Apple's own
// MetalKit samples ship with 3). The CPU stays one frame ahead of the
// GPU; the third buffers presentation latency on ProMotion displays.
inline constexpr u32 kFramesInFlight = 3;

// Per-stage binding namespace size. Slang's Metal emit uses independent
// per-stage argument tables — [[buffer(N)]] on the vertex function is
// distinct from [[buffer(N)]] on the fragment function — so we encode
// (stage, slot) into a single index space by reserving the lower half
// for VS and the upper half for PS. The Wc3-Shaders slang module pins
// PsCbBindingOffset / PsSamplerBindingOffset to 16 (cb_structs.slang),
// which is also our shift. FlushBindings strips the shift back off when
// dispatching to per-stage Metal setters.
inline constexpr u32 kStageBindingShift = 16;
inline constexpr u32 kCbBindingCount = 16;
inline constexpr u32 kSrvBindingCount = 16;
inline constexpr u32 kSamplerBindingCount = 16;

// Vertex-buffer slot N is bound at Metal vertex-stage buffer index
// (kVertexBufferIndexBase + N). The base sits just above the CB range so
// it can't collide with slangc's [[buffer(0..15)]] CB indices on the
// vertex function. The matching MTLVertexDescriptor.layouts[idx] picks
// up the same index. Tunable in Phase F if slangc emits CBs past
// [[buffer(15)]] for any Wc3 family.
inline constexpr u32 kVertexBufferIndexBase = kStageBindingShift;
inline constexpr u32 kMaxVertexBufferSlots = 8;

// Phantom vertex-attribute buffers — see ShaderEntry::declaredVertexAttrs
// + CreateGraphicsPipeline. Slang declares every attribute on the
// input struct even when specialization eliminates the readers; Metal
// validates the [[stage_in]] declaration strictly, so the renderer's
// PSO build path synthesizes a one-attribute MTLVertexBufferLayout per
// missing attribute and binds them all to a device-wide zero buffer at
// draw time. Phantoms occupy buffer indices above the real vertex-slot
// range; Metal supports up to 31 vertex-stage buffer indices, leaving
// (31 - kPhantomVertexBufferIndexBase) phantom slots.
inline constexpr u32 kPhantomVertexBufferIndexBase =
    kVertexBufferIndexBase + kMaxVertexBufferSlots;  // 16 + 8 = 24
inline constexpr u32 kMaxPhantomVertexAttrs = 7;     // 24..30 inclusive

// 64 MiB shared upload ring for CpuWritable+Constant buffers. Matches
// the Vulkan / WebGPU backends' kSharedCbCapacity so per-CB sub-alloc
// math is identical across backends. Apple Silicon has unified memory
// — MTLStorageModeShared is essentially free here.
inline constexpr u64 kSharedCbCapacity = 64ull * 1024 * 1024;

// Map* rotation depth on a CpuWritable+Constant buffer. See the WebGPU
// backend comment for the worst-case-draw rationale; 1024 is the same
// floor and matches the Wc3 per-frame BLS CB map count.
inline constexpr u32 kCbRingSlots = 1024;

// Submit-epoch sequencing. Every Present commits the per-frame command
// buffer and increments pendingEpoch; the addCompletedHandler callback
// bumps completedEpoch once the GPU retires the work. Destroy() tags
// the entry with the current pendingEpoch and the delete-queue drains
// when completedEpoch catches up. Same shape as the WebGPU backend.
using DeleteEpoch = uint64_t;

struct BufferEntry {
    id<MTLBuffer> buffer = nil;
    BufferDesc desc{};

    // Sub-alloc bookkeeping mirrors the Vulkan / WebGPU backends. For
    // dedicated buffers slotCount=1, baseOffset=0. For shared-CB
    // sub-allocs `buffer` aliases MetalDeviceState::sharedCb and
    // baseOffset / slotStride define the window inside it.
    u64 slotStride = 0;
    u32 slotCount = 1;
    u32 currentSlot = 0;
    u64 baseOffset = 0;
    u64 byteSize = 0;
    bool isSharedRingAlias = false;

    // Mapped pointer when host-visible; nullptr otherwise. For shared-CB
    // sub-allocs this points into MetalDeviceState::sharedCbMapped.
    uint8_t* mapped = nullptr;

    u64 currentOffset() const {
        return baseOffset + slotStride * currentSlot;
    }
};

struct TextureEntry {
    id<MTLTexture> texture = nil;     // sRGB view (or default-aliased view)
    id<MTLTexture> viewLinear = nil;  // linear partner; nil when N/A
    MTLPixelFormat format = MTLPixelFormatInvalid;
    i32 width = 0;
    i32 height = 0;
    i32 mipLevels = 1;
    i32 arraySize = 1;
    bool ownsTexture = true;
    bool isDepth = false;

    // Swap-chain proxies re-point `texture` to [drawable texture] each
    // frame at AcquireSwapChainImageIfNeeded.
    SwapChainHandle swapChainProxy = SwapChainHandle::Invalid;
    bool isLinearView = false;

    u64 byteSize = 0;
};

struct ShaderEntry {
    id<MTLLibrary> library = nil;
    id<MTLFunction> function = nil;
    ShaderStage stage = ShaderStage::Vertex;
    std::string entryPoint;

    // Declared vertex attributes (index + Metal format), captured from
    // [function vertexAttributes] at CreateShader time. Slang's Metal
    // emit always declares every attribute in the input struct even
    // when specialization eliminates the bodies that read them — Metal
    // validates the [[stage_in]] declaration strictly against the
    // MTLVertexDescriptor, so any declared attribute not in the
    // engine's InputLayout has to be synthesized as a phantom layout
    // (zero-buffer source). See CreateGraphicsPipeline.
    struct VertexAttr {
        u32 index = 0;
        MTLVertexFormat format = MTLVertexFormatInvalid;
    };
    std::vector<VertexAttr> declaredVertexAttrs;
};

// Default threadgroup size for compute pipelines until ComputePipelineDesc
// gains an explicit field for it (Phase H or whenever a second compute
// shader lands). The only compute consumer today is the framebuffer
// capture kernel — its slang source declares `[numthreads(8, 8, 1)]`,
// which slang's Metal emit drops on the floor (the metallib doesn't
// carry the size as something the runtime can query). Picking (8, 8, 1)
// here matches the implicit divisor the renderer uses in
// frame_capture.cpp: `Dispatch((w + 7) / 8, (h + 7) / 8, 1)`.
inline constexpr u32 kDefaultComputeThreadsX = 8;
inline constexpr u32 kDefaultComputeThreadsY = 8;
inline constexpr u32 kDefaultComputeThreadsZ = 1;

struct PipelineEntry {
    // Metal validates a PSO's attachment formats strictly against the
    // active render pass: a PSO whose depthAttachmentPixelFormat is set
    // can ONLY be bound inside a pass that has a depth attachment, and
    // vice versa. The renderer doesn't track per-pass attachment shape
    // for the BindPipeline path — it just sets a single dsvFormat on
    // every PSO it builds and binds it everywhere. We bridge that by
    // building two PSO variants on every PSO that wants depth: one
    // depth-aware, one color-only. At BindPipeline time the command
    // list picks the matching variant based on whether the active
    // render encoder has a depth attachment.
    //
    // `graphics` is the depth-having variant (or the only variant for
    // color-only PSOs). `graphicsColorOnly` is the variant with depth
    // attachment format forced to MTLPixelFormatInvalid; nil when the
    // PSO has no depth format to begin with.
    id<MTLRenderPipelineState> graphics = nil;
    id<MTLRenderPipelineState> graphicsColorOnly = nil;
    id<MTLComputePipelineState> compute = nil;
    id<MTLDepthStencilState> depthStencil = nil;
    bool isCompute = false;
    MTLPixelFormat colorFormat = MTLPixelFormatInvalid;
    MTLPixelFormat depthFormat = MTLPixelFormatInvalid;
    MTLPrimitiveType primitive = MTLPrimitiveTypeTriangle;
    MTLCullMode cull = MTLCullModeBack;
    MTLWinding winding = MTLWindingClockwise;
    // Used by FlushBindings to pick the right indexing convention when
    // (eventually) emitting argument-buffer binds. Not consumed yet.
    bool hasFragment = false;

    // Phantom-attribute layout indices the PSO expects to be bound to
    // the device zero buffer at draw time. Empty when every declared
    // vertex attribute had a matching InputLayout entry. Indices are
    // raw Metal buffer indices (kPhantomVertexBufferIndexBase + i).
    std::vector<u32> phantomBufferIndices;

    // Threads per threadgroup for compute PSOs. Defaults to
    // (kDefaultComputeThreadsX, …Y, …Z) — see the constants above for
    // the "where did 8x8x1 come from" rationale. Renderer-side
    // Dispatch(gx, gy, gz) passes threadgroup counts; we multiply by
    // this to get per-axis thread counts for Metal's
    // dispatchThreadgroups:threadsPerThreadgroup:.
    u32 computeThreadsX = kDefaultComputeThreadsX;
    u32 computeThreadsY = kDefaultComputeThreadsY;
    u32 computeThreadsZ = kDefaultComputeThreadsZ;
};

struct SamplerEntry {
    id<MTLSamplerState> sampler = nil;
};

struct SwapChainEntry {
    CAMetalLayer* layer = nil;
    id<CAMetalDrawable> currentDrawable = nil;
    MTLPixelFormat formatSrgb = MTLPixelFormatInvalid;
    MTLPixelFormat formatLinear = MTLPixelFormatInvalid;
    u32 width = 0;
    u32 height = 0;
    TextureHandle proxySrgb = TextureHandle::Invalid;
    TextureHandle proxyLinear = TextureHandle::Invalid;
    bool acquiredThisFrame = false;
};

// One command-buffer + encoder pair per in-flight frame slot.
struct FrameContext {
    id<MTLCommandBuffer> commandBuffer = nil;
    id<MTLRenderCommandEncoder> renderEncoder = nil;
    id<MTLComputeCommandEncoder> computeEncoder = nil;
    id<MTLBlitCommandEncoder> blitEncoder = nil;
    DeleteEpoch epoch = 0;
    bool recording = false;
    // Whether the active render pass has a depth/stencil attachment. Set
    // by BeginRenderPass; read by BindPipeline to pick the depth-aware
    // vs color-only PSO variant. See PipelineEntry::graphicsColorOnly.
    bool passHasDepth = false;
};

struct PendingDelete {
    DeleteEpoch retireAfter = 0;
    // Captured by strong ref so ARC keeps the id<MTL...> alive across the
    // wait. Running deleter() drops the ref; the object frees when the
    // command buffer's internal retain also releases.
    std::function<void()> deleter;
};

} // namespace whiteout::flakes::gfx::metal
