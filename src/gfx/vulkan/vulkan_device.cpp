// VulkanDevice ctor / dtor / accessors. Heavy work lives in the
// other vulkan_*.cpp files.

#include "vulkan_command_list.h"
#include "vulkan_device.h"
#include "vulkan_device_state.h"
#include "vulkan_handles.h"

#if defined(TRACY_ENABLE)
#include <tracy/TracyVulkan.hpp>
#endif

namespace whiteout::flakes::gfx::vulkan {

VulkanDevice::VulkanDevice()
    : state_(std::make_unique<VulkanDeviceState>()),
      immediate_(std::make_unique<VulkanCommandList>(*this)) {}

VulkanDevice::~VulkanDevice() {
    auto& state = *state_;
    if (*state.device) {
        state.device.waitIdle();
        SavePipelineCache(state);
    }

#if defined(TRACY_ENABLE)
    if (state.tracyCtx) {
        TracyVkDestroy(state.tracyCtx);
        state.tracyCtx = nullptr;
    }
#endif

    // waitIdle above made everything safe — bypass the timeline gate.
    for (auto& pending : state.pendingDeletes)
        pending.deleter->Run(state);
    state.pendingDeletes.clear();
    for (auto& pending : state.pendingTransferDeletes)
        pending.deleter->Run(state);
    state.pendingTransferDeletes.clear();

    // Reap anything the renderer never explicitly destroyed. Shared-
    // ring sub-allocs (allocation==NULL) are skipped; the ring buffer
    // is freed once after this loop.
    state.buffers.ForEach([&](BufferEntry& buffer) {
        if (buffer.buffer && buffer.allocation)
            vmaDestroyBuffer(state.allocator, buffer.buffer, buffer.allocation);
        buffer.buffer = VK_NULL_HANDLE;
        buffer.allocation = VK_NULL_HANDLE;
    });
    state.textures.ForEach([&](TextureEntry& texture) {
        if (texture.ownsImage && texture.image && texture.allocation)
            vmaDestroyImage(state.allocator, texture.image, texture.allocation);
        texture.image = VK_NULL_HANDLE;
        texture.allocation = VK_NULL_HANDLE;
    });
    state.buffers.Clear();
    state.textures.Clear();

    if (state.sharedCbBuffer && state.sharedCbAllocation) {
        vmaDestroyBuffer(state.allocator, state.sharedCbBuffer, state.sharedCbAllocation);
        state.sharedCbBuffer = VK_NULL_HANDLE;
        state.sharedCbAllocation = VK_NULL_HANDLE;
        state.sharedCbMapped = nullptr;
    }

    if (state.allocator) {
        vmaDestroyAllocator(state.allocator);
        state.allocator = VK_NULL_HANDLE;
    }
}

VulkanDeviceState& VulkanDevice::State() {
    return *state_;
}
const VulkanDeviceState& VulkanDevice::State() const {
    return *state_;
}

const char* VulkanDevice::GetDeviceName() const {
    return deviceName_.c_str();
}
IGFXCommandList* VulkanDevice::GetImmediateContext() {
    return immediate_.get();
}

} // namespace whiteout::flakes::gfx::vulkan
