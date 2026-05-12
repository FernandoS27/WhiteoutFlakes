#pragma once

// ============================================================================
// Vulkan resource entry types and the per-frame sync bundle.
//
// Stored in shared SlotMap<>s on VulkanDeviceState. Public BufferHandle /
// TextureHandle / etc. encode (slot index + generation) — see
// gfx/common/slot_map.h.
//
// RAII via vulkan_raii.hpp: every Vulkan-owned object that doesn't go
// through VMA (shader modules, samplers, pipelines, image views,
// command pools, sync primitives, surface, swapchain, etc.) is held in
// a vk::raii::* wrapper that destroys itself on scope exit. VMA-owned
// resources (VkBuffer, VkImage) stay raw with paired VmaAllocation;
// they're destroyed via vmaDestroyBuffer / vmaDestroyImage in the
// device's Destroy(...) / SlotMap removal path.
//
// Swap-chain-proxy mode for TextureEntry: the renderer caches the
// TextureHandle returned from GetSwapChainBackBuffer once at swap-chain
// creation and reuses it every frame. To keep the handle stable but
// route to whichever VkImage was just acquired, the proxy entry's
// `image` / `view` fields are repointed inside the swap-chain entry's
// per-frame Acquire step. The proxy's view is a borrowed reference to
// a vk::raii::ImageView owned by the SwapChainEntry — the proxy never
// destroys it.
// ============================================================================

#include "gfx/common/slot_map.h"
#include "gfx/gfx.h"

#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>

#if defined(TRACY_ENABLE)
#  include <tracy/TracyVulkan.hpp>
#endif

#include <array>
#include <filesystem>
#include <memory>
#include <utility>
#include <deque>
#include <vector>

namespace whiteout::flakes::gfx::vulkan {

// Number of frames the host can have queued on the GPU before
// vkWaitForFences in EnsureRecording stalls. 3 trades one extra
// frame of input latency (~6 ms at 165 Hz) for additional CPU/GPU
// pipelining headroom — useful when a heavy frame's GPU work runs
// long and the host wants to start recording the next frame anyway.
// Don't go higher without thinking about per-actor bone-palette CB
// memory: that ring uses ringSlotsHint=4 which already exceeds
// kFramesInFlight=3.
inline constexpr u32 kFramesInFlight = 3;

// CpuWritable buffers (constant buffers, dynamic vertex buffers) back
// onto a ring of `slotCount` slots, each `slotStride` bytes. MapBuffer
// rotates to the next slot and returns its pointer; BindConstantBuffer
// captures the current offset so each draw reads its own data even
// when the renderer maps the same logical buffer multiple times per
// frame. Non-CpuWritable buffers use a single slot at offset 0.
struct BufferEntry {
    VkBuffer        buffer     = VK_NULL_HANDLE;   // own VMA buffer OR alias of sharedCb
    VmaAllocation   allocation = VK_NULL_HANDLE;   // null for shared-ring sub-allocs
    void*           mapped     = nullptr;          // persistent map for CpuWritable
    BufferDesc      desc{};                        // .size is the LOGICAL slot size

    u64             slotStride = 0;                // bytes between ring slots (aligned)
    u32             slotCount  = 1;                // 1 for non-ring buffers
    u32             currentSlot = 0;               // last MapBuffer wrote into this slot
    // Base offset within `buffer`. Zero for stand-alone (own-allocation)
    // entries; non-zero for CpuWritable rings that sub-allocated into
    // the shared CB pool (see VulkanDeviceState::sharedCbBuffer).
    u64             baseOffset = 0;
    u64             currentOffset() const { return baseOffset + slotStride * currentSlot; }
};

struct TextureEntry {
    // VMA-owned image (ownsImage=true) OR swap-chain-owned image
    // (ownsImage=false, image is a non-owning copy of the swap-chain image).
    VkImage         image      = VK_NULL_HANDLE;
    VmaAllocation   allocation = VK_NULL_HANDLE;     // null for non-owned
    bool            ownsImage  = true;

    // Owned views: vk::raii::ImageView destroys on Remove. For swap-chain
    // proxies the view is borrowed from the SwapChainEntry — `view` is a
    // raw handle and `ownedView` stays null.
    vk::raii::ImageView ownedView = nullptr;
    VkImageView         view      = VK_NULL_HANDLE;

    vk::Format          format        = vk::Format::eUndefined;
    vk::ImageLayout     currentLayout = vk::ImageLayout::eUndefined;
    vk::ImageAspectFlags aspect       = vk::ImageAspectFlagBits::eColor;
    i32                 width         = 0;
    i32                 height        = 0;

    // Swap-chain proxy: when non-zero, the swap chain re-points
    // image/view on every Acquire. Two proxies per swap chain — one
    // bound to the sRGB view, one to the linear view.
    SwapChainHandle swapChainProxy = SwapChainHandle::Invalid;
    bool            isLinearView   = false;
};

struct ShaderEntry {
    vk::raii::ShaderModule module = nullptr;
    ShaderStage            stage  = ShaderStage::Vertex;
};

struct PipelineEntry {
    vk::raii::Pipeline pipeline  = nullptr;
    bool               isCompute = false;
    // Stored alongside the VkPipeline so VulkanCommandList::Draw can
    // sanity-check at submit time that the bound pipeline's expected
    // color-attachment format matches the active render pass's
    // imageView format — easier to diagnose than VUID-vkCmdDraw-08910
    // alone (which doesn't name the failing C++ call site).
    vk::Format         colorFormat = vk::Format::eUndefined;
};

struct SamplerEntry {
    vk::raii::Sampler sampler = nullptr;
};

// One bundle per frame in flight. The command buffer is reset (not
// freed) at the start of its frame; the fence limits in-flight depth.
// Acquire AND render-done semaphores are owned by the swap chain
// (per-image, not per frame slot) — binary semaphore reuse rules
// require both to be pinned to the swap-chain image's acquire/present
// cycle, not the host-side frame-slot rotation.
struct FrameContext {
    vk::raii::CommandPool   commandPool      = nullptr;
    vk::raii::CommandBuffer commandBuffer    = nullptr;
    vk::raii::Fence         inFlightFence    = nullptr;
    // SRV + sampler descriptor sets are still pool-allocated per draw
    // (only the CB set is on push descriptors — Vulkan only allows one
    // push set per pipeline layout). Pool reset at frame start; sets
    // are owned by the pool so we never call vkFreeDescriptorSets.
    vk::raii::DescriptorPool descriptorPool  = nullptr;
    // Set by AcquireSwapChainImageIfNeeded — point at the per-image
    // acquire / render-done semaphores inside the SwapChainEntry. Used
    // as the wait + signal semaphores on submit2 / presentKHR.
    VkSemaphore             acquireWaitSem   = VK_NULL_HANDLE;
    VkSemaphore             renderDoneSem    = VK_NULL_HANDLE;
    bool                    recording        = false;
};

struct SwapChainEntry {
    vk::raii::SurfaceKHR    surface      = nullptr;
    vk::raii::SwapchainKHR  swapchain    = nullptr;
    vk::Extent2D            extent{};
    vk::Format              formatSrgb   = vk::Format::eUndefined;
    vk::Format              formatLinear = vk::Format::eUndefined;

    // Swap-chain images are owned by the swapchain (no destroy). Views
    // are owned by us via raii.
    std::vector<vk::Image>           images;
    std::vector<vk::raii::ImageView> viewsSrgb;
    std::vector<vk::raii::ImageView> viewsLinear;

    // Per-image acquire + render-done semaphores. Both are pinned to
    // the swap-chain image's acquire/present cycle so binary-semaphore
    // reuse rules don't fire when the host-side frame slot rotates
    // faster than the swap chain. Acquire uses a swap-with-spare
    // dance because we don't know the image index until acquire
    // returns; render-done is straightforward per-image.
    std::vector<vk::raii::Semaphore> imageAcquireSems;
    vk::raii::Semaphore              spareAcquireSem = nullptr;
    std::vector<vk::raii::Semaphore> imageRenderDoneSems;

    u32             imageIndex        = 0;       // last acquired index
    bool            acquiredThisFrame = false;

    TextureHandle   proxySrgb    = TextureHandle::Invalid;
    TextureHandle   proxyLinear  = TextureHandle::Invalid;
};

// Deferred-delete entry. Each Destroy(handle) call moves the entry's
// VMA / vk::raii::* state into a lambda and pushes it here, tagged
// with `timelineValue` — the timeline semaphore value the next submit
// will signal. The drain pass at frame start frees every queued
// deleter whose value the GPU has reached.
//
// We can't use std::function: the captured vk::raii::* objects are
// move-only, but std::function requires copy-constructible callables
// (and C++20 has no std::move_only_function). A unique_ptr to a
// virtual base is the simplest move-only type-erased callable.
struct VulkanDeviceState;
struct PendingDelete {
    struct DeleterBase {
        virtual ~DeleterBase() = default;
        virtual void Run(VulkanDeviceState& s) = 0;
    };
    template <typename F>
    struct DeleterImpl : DeleterBase {
        F fn;
        explicit DeleterImpl(F&& f) : fn(std::move(f)) {}
        void Run(VulkanDeviceState& s) override { fn(s); }
    };

    u64                          timelineValue = 0;
    std::unique_ptr<DeleterBase> deleter;
};

template <typename F>
inline PendingDelete MakePendingDelete(u64 v, F&& f) {
    PendingDelete pd;
    pd.timelineValue = v;
    pd.deleter = std::make_unique<PendingDelete::DeleterImpl<std::decay_t<F>>>(
        std::forward<F>(f));
    return pd;
}

// Aggregates every Vulkan-owned object the device manages. Everything
// non-VMA is RAII; VMA's allocator handle is raw and destroyed
// manually in VulkanDevice's destructor (after the SlotMaps clear so
// any owned VkBuffer/VkImage have already been freed).
//
// Phase 1 uses a single shared pipeline layout for every PSO. The
// layout has a push-descriptor set (no pool needed) with bindings
// shaped to match the D3D-style "BindConstantBuffer(stage, slot)"
// model. Binding 0..3 carry VS/PS-shared CBs; later milestones
// extend this with SRV / sampler bindings.
struct VulkanDeviceState {
    vk::raii::Context                ctx;
    vk::raii::Instance               instance       = nullptr;
    vk::raii::DebugUtilsMessengerEXT debugMessenger = nullptr;
    vk::raii::PhysicalDevice         physicalDevice = nullptr;
    vk::raii::Device                 device         = nullptr;
    u32                              queueFamily    = 0;
    vk::raii::Queue                  queue          = nullptr;
    VmaAllocator                     allocator      = VK_NULL_HANDLE;

    // Async-transfer queue. On discrete NVIDIA (Maxwell 2+) / AMD
    // (GCN+) we pick the dedicated DMA family (queueFlags ==
    // TRANSFER, no graphics bit), so staged buffer/texture uploads
    // drain on a queue that runs in parallel with the renderer. On
    // Intel integrated, mobile (Mali / Adreno / PowerVR), and
    // MoltenVK the same family handles everything; in that fallback
    // `hasAsyncTransfer == false`, `transferQueueFamily == queueFamily`,
    // and `transferQueue` aliases `queue`. The upload helpers route
    // through `transferQueue` either way.
    //
    // Sync model: every upload submit signals `transferTimelineSem`
    // at `++transferNextValue` and stores the value in
    // `transferLastSignaled`. The next graphics-queue submit (Present
    // path) waits on `transferLastSignaled` so any draw it carries
    // sees the uploaded data. Staging buffers + one-shot command
    // buffers go onto `pendingTransferDeletes`, which `EnsureRecording`
    // drains by polling the transfer-timeline counter — no waitIdle
    // anywhere on the upload path.
    u32                              transferQueueFamily = 0;
    vk::raii::Queue                  transferQueue       = nullptr;
    bool                             hasAsyncTransfer    = false;
    vk::raii::Semaphore              transferTimelineSem = nullptr;
    u64                              transferNextValue    = 0;
    u64                              transferLastSignaled = 0;

    // Transient command pool on the transfer queue family. The upload
    // helpers (CreateBuffer staged path, UploadTexturePixels) allocate
    // one-shot command buffers from here. Buffers are moved into
    // `pendingTransferDeletes` so their raii destruction happens
    // after the transfer timeline reaches their signal value.
    vk::raii::CommandPool            transferCommandPool = nullptr;

    // Deferred-delete queue keyed on the *transfer* timeline. Mirrors
    // `pendingDeletes` (which is keyed on the graphics timeline) but
    // exists so staging buffers + transfer-side command buffers can
    // be cleaned up promptly without needing to ride a graphics submit.
    std::deque<PendingDelete>        pendingTransferDeletes;

    // Cached at Init() time from VkPhysicalDeviceProperties. Drives the
    // slot-stride alignment used by the per-CB ring buffer in
    // CreateBuffer / MapBuffer (CpuWritable path).
    u64                              minUniformBufferAlign = 256;
    // Default ring slots per CpuWritable buffer when the caller
    // doesn't pass a BufferDesc::ringSlotsHint. The renderer can
    // update a single CB up to this many times per frame before the
    // ring wraps; with 2 frames in flight, slots N/2..N-1 belong to
    // last frame's GPU reads, so we need at least
    // (max_updates_per_frame * kFramesInFlight) slots.
    //
    // 256 is enough for cold CBs but the BLS frame-uniform CBs
    // (HdVsCb/HdPsCb/...) get mapped once per draw and modern scenes
    // exceed 1000 draws/frame; those callsites pass an explicit
    // ringSlotsHint instead of relying on this default. 256 stays as
    // a per-instance-friendly default so per-actor CBs (mapped once
    // per actor per frame) don't blow per-instance memory.
    static constexpr u32             kCbRingSlots         = 256;

    // Shared backing buffer for every CpuWritable CreateBuffer. The
    // renderer makes ~15-20 small dynamic CBs (per-frame uniforms,
    // BLS material CBs, tonemap, etc.); coalescing them into one
    // persistently-mapped VkBuffer saves ~15 VMA allocations + 15
    // mapped regions and means every descriptor write for a dynamic
    // CB references the same VkBuffer with just a different offset
    // (drivers cache the VkBuffer→VkDeviceMemory binding lookup, so
    // this still helps for the SRV path that doesn't use push
    // descriptors). The bump cursor never reclaims sub-allocs; CB
    // creation is a load-time / one-shot operation so a simple
    // monotonic allocator is plenty. If the cursor overflows
    // `sharedCbCapacity`, CreateBuffer falls back to a per-CB VMA
    // allocation so we never hard-fail.
    static constexpr u64             kSharedCbCapacity    = 64ull * 1024 * 1024;
    VkBuffer                         sharedCbBuffer       = VK_NULL_HANDLE;
    VmaAllocation                    sharedCbAllocation   = VK_NULL_HANDLE;
    void*                            sharedCbMapped       = nullptr;
    u64                              sharedCbCursor       = 0;

    // Three push-descriptor set layouts, one per resource type, matching
    // the [[vk::binding(N, S)]] convention applied to the Slang sources:
    //   set 0 = constant buffers (uniformBuffer, bindings 0..N-1)
    //   set 1 = shader resources  (sampledImage,  bindings 0..N-1)
    //   set 2 = samplers         (sampler,       bindings 0..N-1)
    // The split avoids type collisions across HLSL's b/t/s register
    // namespaces (which are flattened to a single binding-number space
    // in SPIR-V) without exceeding any single set's push-descriptor cap.
    vk::raii::DescriptorSetLayout    cbSetLayout       = nullptr;
    vk::raii::DescriptorSetLayout    srvSetLayout      = nullptr;
    vk::raii::DescriptorSetLayout    samplerSetLayout  = nullptr;
    vk::raii::PipelineLayout         pipelineLayout    = nullptr;

    // Driver pipeline cache, serialized to disk on Shutdown and rehydrated
    // on the next Init. Vulkan stores a driver/device UUID prefix in the
    // blob — if it mismatches (driver update, different GPU), the driver
    // rejects the load on its own and we start with an empty cache, so
    // we don't have to validate.
    vk::raii::PipelineCache          pipelineCache  = nullptr;
    // Filesystem path for the pipeline-cache blob, supplied by the host
    // via gfx::SetPipelineCachePath before CreateDevice. Empty (the
    // default) means "no persistence" — we still build a runtime cache,
    // we just don't load/save it. The gfx layer never resolves paths
    // on its own; finding the right directory is a host concern.
    std::filesystem::path            pipelineCachePath;

    // Timeline semaphore + deferred-delete queue. The submit signals
    // `timelineSem` at `nextSubmitValue` then increments it; Destroy()
    // queues entries tagged with that pending value so they free
    // automatically once the GPU reaches that point.
    vk::raii::Semaphore        timelineSem    = nullptr;
    u64                        nextSubmitValue = 1;
    std::deque<PendingDelete>  pendingDeletes;

    SlotMap<BufferEntry>    buffers;
    SlotMap<TextureEntry>   textures;
    SlotMap<ShaderEntry>    shaders;
    SlotMap<PipelineEntry>  pipelines;
    SlotMap<SamplerEntry>   samplers;
    SlotMap<SwapChainEntry> swapchains;

    // Newly-created sampled textures sit in VK_IMAGE_LAYOUT_UNDEFINED and
    // can't be sampled from until they're transitioned to
    // eShaderReadOnlyOptimal. The renderer's gfx layer has no explicit
    // barrier API, and image-layout transitions are illegal inside an
    // active dynamic-rendering scope, so CreateTexture stages the
    // transition here and EnsureRecording (frame start, before any
    // render pass begins) drains the queue with one batched
    // pipelineBarrier2.
    std::vector<u64>           pendingSrvTransitions;

    std::array<FrameContext, kFramesInFlight> frames{};
    u32                                       frameIndex = 0;

#if defined(TRACY_ENABLE)
    // Tracy GPU context — created at Init via TracyVkContext with a
    // one-shot calibration command buffer, destroyed in Shutdown via
    // TracyVkDestroy. The renderer's BeginGpuZone / EndGpuZone calls
    // forward into this context, and one TracyVkCollect per frame
    // (just before submit) drains the query pool to Tracy's profiler.
    // The pointer is owned by Tracy internals; we just store the
    // handle for the per-frame collect + the eventual destroy.
    tracy::VkCtx* tracyCtx = nullptr;
#endif
};

// Drains every pending-delete entry whose timeline value the GPU has
// reached. Called once per frame just before the host starts recording
// the next command buffer.
void DrainPendingDeletes(VulkanDeviceState& s);
// Same shape as DrainPendingDeletes but polls the transfer timeline.
// Called at frame start; safe to call when hasAsyncTransfer is false
// (the unified-queue path keeps the list empty by never signaling).
void DrainPendingTransferDeletes(VulkanDeviceState& s);

// Per-set binding counts. Each set covers bindings 0..N-1, split into a
// per-stage range. The slang sources annotate each VS resource at its
// raw HLSL register index (b<N>/t<N>/s<N>) and each PS resource at
// register-index + kStageBindingShift, so a (set, binding) pair is
// unique per stage. The Vulkan backend mirrors that shift when
// translating BindConstantBuffer / BindShaderResource / BindSampler
// (stage, slot) calls into descriptor writes.
//
// Set layout:
//   set 0 (Cb)     : bindings 0..N-1 — VS  CBs (b<slot> + 0)
//                    bindings N..2N-1 — PS  CBs (b<slot> + 16)
//   set 1 (Srv)    : bindings 0..N-1 — VS  SRVs
//                    bindings N..2N-1 — PS  SRVs
//   set 2 (Sampler): bindings 0..N-1 — VS  samplers
//                    bindings N..2N-1 — PS  samplers
//
// Future GS/TS/RT stages get their own shift (kStageBindingShift * k)
// so per-stage slot conflicts can never reappear.
inline constexpr u32 kCbSetIndex        = 0;
inline constexpr u32 kSrvSetIndex       = 1;
inline constexpr u32 kSamplerSetIndex   = 2;
inline constexpr u32 kStageBindingShift = 16;
inline constexpr u32 kCbBindingCount      = 32;  // 16 VS + 16 PS
inline constexpr u32 kSrvBindingCount     = 32;  // 16 VS + 16 PS
inline constexpr u32 kSamplerBindingCount = 32;  // 16 VS + 16 PS

// Defined in vulkan_swap_chain.cpp. Called from the command list when
// a swap-chain proxy texture is bound as a render target — acquires
// the next image (if not already acquired this frame) and re-points
// the proxy texture entries at the corresponding VkImage / VkImageView.
u32 AcquireSwapChainImageIfNeeded(VulkanDeviceState& s, SwapChainEntry& sc,
                                   FrameContext& frame);

}  // namespace whiteout::flakes::gfx::vulkan
