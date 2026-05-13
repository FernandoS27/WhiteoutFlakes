// Buffer create/destroy/map/update.

#include "vulkan_device.h"
#include "vulkan_device_state.h"
#include "vulkan_handles.h"
#include "vulkan_transfer.h"
#include "vulkan_translate.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace whiteout::flakes::gfx::vulkan {

BufferHandle VulkanDevice::CreateBuffer(const BufferDesc& desc, const void* initial) {
    auto& state = *state_;

    BufferEntry entry{};
    entry.desc       = desc;
    entry.slotStride = desc.size;
    entry.slotCount  = 1;
    entry.currentSlot = 0;

    const bool ring = hasFlag(desc.usage, BufferUsage::CpuWritable) && desc.size > 0;
    if (ring) {
        const u64 align = std::max<u64>(1, state.minUniformBufferAlign);
        entry.slotStride = (desc.size + align - 1) / align * align;
        entry.slotCount = (desc.ringSlotsHint > 0)
                        ? desc.ringSlotsHint
                        : VulkanDeviceState::kCbRingSlots;
        if (entry.slotCount < kFramesInFlight) entry.slotCount = kFramesInFlight;
    }
    const u64 totalSize = entry.slotStride * entry.slotCount;

    // Try to sub-allocate from the shared CB ring; fall through to a
    // dedicated VMA buffer when it's exhausted.
    if (ring && state.sharedCbBuffer != VK_NULL_HANDLE) {
        const u64 align = std::max<u64>(1, state.minUniformBufferAlign);
        const u64 base  = (state.sharedCbCursor + align - 1) / align * align;
        if (base + totalSize <= VulkanDeviceState::kSharedCbCapacity) {
            entry.buffer     = state.sharedCbBuffer;
            entry.allocation = VK_NULL_HANDLE;  // shared sub-alloc
            entry.mapped     = state.sharedCbMapped;
            entry.baseOffset = base;
            state.sharedCbCursor = base + totalSize;
            if (initial && desc.size > 0) {
                std::memcpy(static_cast<u8*>(entry.mapped) + base, initial, desc.size);
                vmaFlushAllocation(state.allocator, state.sharedCbAllocation,
                                   base, desc.size);
            }
            return static_cast<BufferHandle>(state.buffers.Insert(std::move(entry)));
        }
    }

    VkBufferCreateInfo bci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size  = totalSize;
    bci.usage = ToVkBufferUsage(desc.usage);
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo aci{};
    if (hasFlag(desc.usage, BufferUsage::CpuWritable)) {
        aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                  | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else {
        aci.usage = VMA_MEMORY_USAGE_AUTO;
    }

    VmaAllocationInfo info{};
    if (vmaCreateBuffer(state.allocator, &bci, &aci, &entry.buffer, &entry.allocation, &info)
            != VK_SUCCESS) {
        return BufferHandle::Invalid;
    }
    entry.mapped = info.pMappedData;

    if (initial && desc.size > 0) {
        if (entry.mapped) {
            std::memcpy(entry.mapped, initial, desc.size);
            vmaFlushAllocation(state.allocator, entry.allocation, 0, desc.size);
        } else {
            // Non-mappable: stage through the transfer queue. The
            // graphics submit waits on transferLastSignaled.
            VkBufferCreateInfo sbci{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            sbci.size  = desc.size;
            sbci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            sbci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VmaAllocationCreateInfo saci{};
            saci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
            saci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT
                       | VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VkBuffer      stagingBuf{};
            VmaAllocation stagingAlloc{};
            VmaAllocationInfo si{};
            vmaCreateBuffer(state.allocator, &sbci, &saci, &stagingBuf, &stagingAlloc, &si);
            std::memcpy(si.pMappedData, initial, desc.size);
            vmaFlushAllocation(state.allocator, stagingAlloc, 0, desc.size);

            const VkBuffer dstBuf = entry.buffer;
            const u64      copySize = desc.size;
            SubmitTransferAndDeferStaging(state, stagingBuf, stagingAlloc,
                [dstBuf, stagingBuf, copySize](vk::raii::CommandBuffer& cb) {
                    vk::BufferCopy region{ .size = copySize };
                    cb.copyBuffer(vk::Buffer(stagingBuf), vk::Buffer(dstBuf), region);
                });
        }
    }

    return static_cast<BufferHandle>(state.buffers.Insert(std::move(entry)));
}

// Destroy moves the entry out of the slot map (invalidating the handle
// immediately) and queues a timeline-tagged deleter. The drain runs it
// once the GPU has reached that submit's value.
void VulkanDevice::Destroy(BufferHandle h) {
    auto& state = *state_;
    auto* buffer = state.buffers.Get(static_cast<u64>(h));
    if (!buffer) return;
    // Shared-ring sub-allocs own no VkDeviceMemory — skip the queue.
    if (buffer->allocation == VK_NULL_HANDLE) {
        state.buffers.Remove(static_cast<u64>(h));
        return;
    }
    BufferEntry moved = std::move(*buffer);
    state.buffers.Remove(static_cast<u64>(h));
    state.pendingDeletes.push_back(MakePendingDelete(
        state.nextSubmitValue,
        [owned = std::move(moved)](VulkanDeviceState& st) mutable {
            vmaDestroyBuffer(st.allocator, owned.buffer, owned.allocation);
        }));
}

void VulkanDevice::UpdateBuffer(BufferHandle h, const void* data, usize size) {
    auto& state = *state_;
    auto* buffer = state.buffers.Get(static_cast<u64>(h));
    if (!buffer) return;
    if (buffer->mapped) {
        // Rotate to a fresh ring slot so the next Bind* captures this write.
        if (buffer->slotCount > 1) {
            buffer->currentSlot = (buffer->currentSlot + 1) % buffer->slotCount;
        }
        const u64 off = buffer->currentOffset();
        std::memcpy(static_cast<u8*>(buffer->mapped) + off, data, size);
        VmaAllocation alloc = buffer->allocation
                                  ? buffer->allocation
                                  : state.sharedCbAllocation;
        vmaFlushAllocation(state.allocator, alloc, off, size);
    } else {
        void* dst = nullptr;
        if (vmaMapMemory(state.allocator, buffer->allocation, &dst) == VK_SUCCESS && dst) {
            std::memcpy(dst, data, size);
            vmaUnmapMemory(state.allocator, buffer->allocation);
            vmaFlushAllocation(state.allocator, buffer->allocation, 0, size);
        }
    }
}

void* VulkanDevice::MapBuffer(BufferHandle h) {
    auto& state = *state_;
    auto* buffer = state.buffers.Get(static_cast<u64>(h));
    if (!buffer) return nullptr;
    if (buffer->mapped) {
        if (buffer->slotCount > 1) {
            buffer->currentSlot = (buffer->currentSlot + 1) % buffer->slotCount;
        }
        return static_cast<u8*>(buffer->mapped) + buffer->currentOffset();
    }
    void* mapped = nullptr;
    vmaMapMemory(state.allocator, buffer->allocation, &mapped);
    return mapped;
}

void VulkanDevice::UnmapBuffer(BufferHandle h) {
    auto& state = *state_;
    auto* buffer = state.buffers.Get(static_cast<u64>(h));
    if (!buffer) return;
    if (buffer->mapped) {
        VmaAllocation alloc = buffer->allocation
                                  ? buffer->allocation
                                  : state.sharedCbAllocation;
        vmaFlushAllocation(state.allocator, alloc,
                           buffer->currentOffset(), buffer->desc.size);
    } else {
        vmaUnmapMemory(state.allocator, buffer->allocation);
    }
}

}  // namespace whiteout::flakes::gfx::vulkan
