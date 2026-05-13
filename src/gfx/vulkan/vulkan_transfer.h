#pragma once

// Shared transfer-queue submit-and-defer helper used by buffer + texture
// upload paths. `record` populates a one-shot command buffer; on success
// the helper submits, signals the transfer timeline, and queues the
// staging buffer + cmdbuf for deferred cleanup. On failure the staging
// buffer is freed synchronously and `false` is returned.

#include "vulkan_device_state.h"
#include "vulkan_handles.h"

#include <utility>

namespace whiteout::flakes::gfx::vulkan {

template <typename RecordFn>
inline bool SubmitTransferAndDeferStaging(VulkanDeviceState& state,
                                          VkBuffer stagingBuf,
                                          VmaAllocation stagingAlloc,
                                          RecordFn&& record)
{
    auto cbsR = state.device.allocateCommandBuffers({
        .commandPool        = *state.transferCommandPool,
        .level              = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = 1,
    });
    if (cbsR.result != vk::Result::eSuccess) {
        vmaDestroyBuffer(state.allocator, stagingBuf, stagingAlloc);
        return false;
    }
    vk::raii::CommandBuffer cmdBuf = std::move(cbsR.value[0]);
    (void)cmdBuf.begin({ .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
    record(cmdBuf);
    (void)cmdBuf.end();

    const u64 signalValue = ++state.transferNextValue;
    vk::CommandBufferSubmitInfo cbInfo{ .commandBuffer = *cmdBuf };
    vk::SemaphoreSubmitInfo signalInfo{
        .semaphore = *state.transferTimelineSem,
        .value     = signalValue,
        .stageMask = vk::PipelineStageFlagBits2::eAllCommands,
    };
    vk::SubmitInfo2 submit{
        .commandBufferInfoCount   = 1,
        .pCommandBufferInfos      = &cbInfo,
        .signalSemaphoreInfoCount = 1,
        .pSignalSemaphoreInfos    = &signalInfo,
    };
    (void)state.transferQueue.submit2(submit);
    state.transferLastSignaled = signalValue;

    state.pendingTransferDeletes.push_back(MakePendingDelete(
        signalValue,
        [stagingBuf, stagingAlloc, ownedCb = std::move(cmdBuf)]
        (VulkanDeviceState& st) mutable {
            vmaDestroyBuffer(st.allocator, stagingBuf, stagingAlloc);
            (void)ownedCb;  // raii returns CB to pool
        }));
    return true;
}

}  // namespace whiteout::flakes::gfx::vulkan
