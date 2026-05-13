#pragma once

// Aggregate device state + free-function declarations for the
// lifetime utilities (pipeline-cache I/O, deferred-delete drains,
// swap-chain acquire).

#include "gfx/common/slot_map.h"
#include "vulkan_handles.h"

#if defined(TRACY_ENABLE)
#  include <tracy/TracyVulkan.hpp>
#endif

#include <array>
#include <deque>
#include <filesystem>
#include <vector>

namespace whiteout::flakes::gfx::vulkan {

struct VulkanDeviceState {
    vk::raii::Context                ctx;
    vk::raii::Instance               instance       = nullptr;
    vk::raii::DebugUtilsMessengerEXT debugMessenger = nullptr;
    vk::raii::PhysicalDevice         physicalDevice = nullptr;
    vk::raii::Device                 device         = nullptr;
    u32                              queueFamily    = 0;
    vk::raii::Queue                  queue          = nullptr;
    VmaAllocator                     allocator      = VK_NULL_HANDLE;

    // Async-transfer queue. Dedicated DMA family on discrete NVIDIA/AMD;
    // aliases the graphics queue on Intel iGPU / mobile / MoltenVK.
    // Upload submits signal transferTimelineSem; the graphics submit
    // waits on transferLastSignaled.
    u32                              transferQueueFamily = 0;
    vk::raii::Queue                  transferQueue       = nullptr;
    bool                             hasAsyncTransfer    = false;
    vk::raii::Semaphore              transferTimelineSem = nullptr;
    u64                              transferNextValue    = 0;
    u64                              transferLastSignaled = 0;

    vk::raii::CommandPool            transferCommandPool = nullptr;
    std::deque<PendingDelete>        pendingTransferDeletes;

    u64                              minUniformBufferAlign = 256;

    // Default slot count for CpuWritable buffers when the caller
    // doesn't pass BufferDesc::ringSlotsHint.
    static constexpr u32             kCbRingSlots         = 256;

    // Shared backing buffer that every CpuWritable CreateBuffer
    // sub-allocates from. Falls back to per-CB VMA when the cursor
    // overflows kSharedCbCapacity.
    static constexpr u64             kSharedCbCapacity    = 64ull * 1024 * 1024;
    VkBuffer                         sharedCbBuffer       = VK_NULL_HANDLE;
    VmaAllocation                    sharedCbAllocation   = VK_NULL_HANDLE;
    void*                            sharedCbMapped       = nullptr;
    u64                              sharedCbCursor       = 0;

    vk::raii::DescriptorSetLayout    cbSetLayout       = nullptr;
    vk::raii::DescriptorSetLayout    srvSetLayout      = nullptr;
    vk::raii::DescriptorSetLayout    samplerSetLayout  = nullptr;
    vk::raii::PipelineLayout         pipelineLayout    = nullptr;

    // Pipeline cache persisted to pipelineCachePath; empty path = none.
    // The driver embeds a UUID prefix, so a stale blob is rejected
    // safely on driver/device change.
    vk::raii::PipelineCache          pipelineCache  = nullptr;
    std::filesystem::path            pipelineCachePath;

    // Graphics-timeline deferred-delete tracking. Each submit signals
    // timelineSem at nextSubmitValue then increments; Destroy() tags
    // entries with the pending value.
    vk::raii::Semaphore        timelineSem    = nullptr;
    u64                        nextSubmitValue = 1;
    std::deque<PendingDelete>  pendingDeletes;

    SlotMap<BufferEntry>    buffers;
    SlotMap<TextureEntry>   textures;
    SlotMap<ShaderEntry>    shaders;
    SlotMap<PipelineEntry>  pipelines;
    SlotMap<SamplerEntry>   samplers;
    SlotMap<SwapChainEntry> swapchains;

    // CreateTexture queues new sampled textures here; EnsureRecording
    // drains with one batched pipelineBarrier2 before the first render
    // pass (layout transitions are illegal inside dynamic rendering).
    std::vector<u64>           pendingSrvTransitions;

    std::array<FrameContext, kFramesInFlight> frames{};
    u32                                       frameIndex = 0;

#if defined(TRACY_ENABLE)
    tracy::VkCtx* tracyCtx = nullptr;
#endif
};

// Each set covers bindings 0..N-1, split per-stage: VS at offset 0,
// PS at offset kStageBindingShift.
inline constexpr u32 kCbSetIndex        = 0;
inline constexpr u32 kSrvSetIndex       = 1;
inline constexpr u32 kSamplerSetIndex   = 2;
inline constexpr u32 kStageBindingShift = 16;
inline constexpr u32 kCbBindingCount      = 32;
inline constexpr u32 kSrvBindingCount     = 32;
inline constexpr u32 kSamplerBindingCount = 32;

void DrainPendingDeletes(VulkanDeviceState& state);
void DrainPendingTransferDeletes(VulkanDeviceState& state);

void LoadPipelineCache(VulkanDeviceState& state);
void SavePipelineCache(VulkanDeviceState& state);

// Acquires the next image (if not already this frame) and re-points
// the proxy texture entries.
u32 AcquireSwapChainImageIfNeeded(VulkanDeviceState& state, SwapChainEntry& sc,
                                   FrameContext& frame);

}  // namespace whiteout::flakes::gfx::vulkan
