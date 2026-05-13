#pragma once

// Per-resource entry types stored in VulkanDeviceState's SlotMaps, plus
// the per-frame sync bundle, swap-chain state, and the deferred-delete
// queue entry shape.

#include "gfx/gfx.h"

#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>

#include <memory>
#include <utility>
#include <vector>

namespace whiteout::flakes::gfx::vulkan {

inline constexpr u32 kFramesInFlight = 3;

// CpuWritable buffers ring `slotCount` slots of `slotStride` bytes;
// MapBuffer rotates so each draw sees its own write even when the
// same logical buffer is mapped many times per frame.
struct BufferEntry {
    VkBuffer        buffer     = VK_NULL_HANDLE;   // own VMA buffer OR alias of sharedCb
    VmaAllocation   allocation = VK_NULL_HANDLE;   // null for shared-ring sub-allocs
    void*           mapped     = nullptr;
    BufferDesc      desc{};

    u64             slotStride = 0;
    u32             slotCount  = 1;
    u32             currentSlot = 0;
    u64             baseOffset = 0;  // non-zero only for shared-ring sub-allocs
    u64             currentOffset() const { return baseOffset + slotStride * currentSlot; }
};

// Swap-chain proxy mode: the renderer caches one handle at swap-chain
// creation and we repoint image/view every Acquire.
struct TextureEntry {
    VkImage         image      = VK_NULL_HANDLE;
    VmaAllocation   allocation = VK_NULL_HANDLE;
    bool            ownsImage  = true;

    // For swap-chain proxies the view is borrowed from SwapChainEntry —
    // `view` is the raw handle and `ownedView` stays null.
    vk::raii::ImageView ownedView = nullptr;
    VkImageView         view      = VK_NULL_HANDLE;

    vk::Format          format        = vk::Format::eUndefined;
    vk::ImageLayout     currentLayout = vk::ImageLayout::eUndefined;
    vk::ImageAspectFlags aspect       = vk::ImageAspectFlagBits::eColor;
    i32                 width         = 0;
    i32                 height        = 0;

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
    // Cross-checked against the active render pass's color format
    // in VulkanCommandList::BindPipeline.
    vk::Format         colorFormat = vk::Format::eUndefined;
};

struct SamplerEntry {
    vk::raii::Sampler sampler = nullptr;
};

struct FrameContext {
    vk::raii::CommandPool    commandPool   = nullptr;
    vk::raii::CommandBuffer  commandBuffer = nullptr;
    vk::raii::Fence          inFlightFence = nullptr;
    // SRV/sampler sets are pool-allocated (only one push-descriptor
    // set is allowed per pipeline layout, taken by the CB set).
    // Pool reset at frame start frees every set in one shot.
    vk::raii::DescriptorPool descriptorPool = nullptr;
    // Aliased to the per-image semaphores in SwapChainEntry by
    // AcquireSwapChainImageIfNeeded.
    VkSemaphore              acquireWaitSem = VK_NULL_HANDLE;
    VkSemaphore              renderDoneSem  = VK_NULL_HANDLE;
    bool                     recording      = false;
};

struct SwapChainEntry {
    vk::raii::SurfaceKHR    surface      = nullptr;
    vk::raii::SwapchainKHR  swapchain    = nullptr;
    vk::Extent2D            extent{};
    vk::Format              formatSrgb   = vk::Format::eUndefined;
    vk::Format              formatLinear = vk::Format::eUndefined;

    // Images are swapchain-owned (no destroy); views are ours.
    std::vector<vk::Image>           images;
    std::vector<vk::raii::ImageView> viewsSrgb;
    std::vector<vk::raii::ImageView> viewsLinear;

    // Per-image acquire + render-done semaphores. Pinned per image so
    // binary-semaphore reuse rules don't fire when the host frame slot
    // rotates faster than the swap chain. Acquire uses a swap-with-
    // spare dance (we don't know the image index until acquire returns).
    std::vector<vk::raii::Semaphore> imageAcquireSems;
    vk::raii::Semaphore              spareAcquireSem = nullptr;
    std::vector<vk::raii::Semaphore> imageRenderDoneSems;

    u32             imageIndex        = 0;
    bool            acquiredThisFrame = false;

    TextureHandle   proxySrgb    = TextureHandle::Invalid;
    TextureHandle   proxyLinear  = TextureHandle::Invalid;
};

// Type-erased move-only deleter. std::function rejects the move-only
// vk::raii::* captures; std::move_only_function is C++23.
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

}  // namespace whiteout::flakes::gfx::vulkan
