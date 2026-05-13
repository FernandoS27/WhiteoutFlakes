// Drains for the graphics- and transfer-timeline deferred-delete
// queues. Called from EnsureRecording every frame.

#include "vulkan_device_state.h"
#include "vulkan_handles.h"

namespace whiteout::flakes::gfx::vulkan {

// Entries are appended in submit order — the ready prefix is contiguous.
void DrainPendingDeletes(VulkanDeviceState& state) {
    if (state.pendingDeletes.empty())
        return;
    auto vR = state.timelineSem.getCounterValue();
    if (vR.result != vk::Result::eSuccess)
        return;
    const u64 completed = vR.value;
    while (!state.pendingDeletes.empty() &&
           state.pendingDeletes.front().timelineValue <= completed) {
        state.pendingDeletes.front().deleter->Run(state);
        state.pendingDeletes.pop_front();
    }
}

void DrainPendingTransferDeletes(VulkanDeviceState& state) {
    if (state.pendingTransferDeletes.empty())
        return;
    if (!*state.transferTimelineSem)
        return;
    auto vR = state.transferTimelineSem.getCounterValue();
    if (vR.result != vk::Result::eSuccess)
        return;
    const u64 completed = vR.value;
    while (!state.pendingTransferDeletes.empty() &&
           state.pendingTransferDeletes.front().timelineValue <= completed) {
        state.pendingTransferDeletes.front().deleter->Run(state);
        state.pendingTransferDeletes.pop_front();
    }
}

} // namespace whiteout::flakes::gfx::vulkan
