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

#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace whiteout::flakes::gfx::vulkan {

inline constexpr u32 kFramesInFlight = 2;

struct BufferEntry {
    VkBuffer        buffer     = VK_NULL_HANDLE;   // VMA-owned
    VmaAllocation   allocation = VK_NULL_HANDLE;
    void*           mapped     = nullptr;          // persistent map for CpuWritable
    BufferDesc      desc{};
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

    vk::raii::DescriptorSetLayout    pushDescSetLayout = nullptr;
    vk::raii::PipelineLayout         pipelineLayout    = nullptr;

    // Timeline semaphore + deferred-delete queue. The submit signals
    // `timelineSem` at `nextSubmitValue` then increments it; Destroy()
    // queues entries tagged with that pending value so they free
    // automatically once the GPU reaches that point.
    vk::raii::Semaphore        timelineSem    = nullptr;
    u64                        nextSubmitValue = 1;
    std::vector<PendingDelete> pendingDeletes;

    SlotMap<BufferEntry>    buffers;
    SlotMap<TextureEntry>   textures;
    SlotMap<ShaderEntry>    shaders;
    SlotMap<PipelineEntry>  pipelines;
    SlotMap<SamplerEntry>   samplers;
    SlotMap<SwapChainEntry> swapchains;

    std::array<FrameContext, kFramesInFlight> frames{};
    u32                                       frameIndex = 0;
};

// Drains every pending-delete entry whose timeline value the GPU has
// reached. Called once per frame just before the host starts recording
// the next command buffer.
void DrainPendingDeletes(VulkanDeviceState& s);

// Push-descriptor binding layout used by every PSO. Slang's SPIR-V
// emit puts everything into set 0 with sequential bindings starting at
// 0 (CB → SRVs → samplers). We mirror that contiguous numbering: CBs
// 0..3, SRVs 4..7, samplers 8..11. The viewcube shader (debug
// renderer) uses CB[0] / SRV[0] / sampler[0] — i.e. bindings 0, 4, 8.
//
// NOTE: Slang's default emit numbers them 0, 1, 2 (without a gap).
// We resolve that mismatch by passing -fvk-bind-globals or per-shader
// [vk::binding] annotations once we revisit the shader build. For
// Phase 1 the line shader uses only CB[0] which lands at binding 0
// either way — the viewcube needs the layout below to match its
// 0/1/2 default emit.
inline constexpr u32 kCbBindingBase     = 0;
inline constexpr u32 kCbBindingCount    = 1;
inline constexpr u32 kSrvBindingBase    = kCbBindingBase + kCbBindingCount;     // 1
inline constexpr u32 kSrvBindingCount   = 1;
inline constexpr u32 kSamplerBindingBase  = kSrvBindingBase + kSrvBindingCount; // 2
inline constexpr u32 kSamplerBindingCount = 1;

// Defined in vulkan_swap_chain.cpp. Called from the command list when
// a swap-chain proxy texture is bound as a render target — acquires
// the next image (if not already acquired this frame) and re-points
// the proxy texture entries at the corresponding VkImage / VkImageView.
u32 AcquireSwapChainImageIfNeeded(VulkanDeviceState& s, SwapChainEntry& sc,
                                   FrameContext& frame);

}  // namespace whiteout::flakes::gfx::vulkan
