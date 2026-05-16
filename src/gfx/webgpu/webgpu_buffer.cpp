// Buffer create / destroy / map / update. Mirrors the ring-slot model in
// src/gfx/vulkan/vulkan_buffer.cpp — CpuWritable+Constant buffers get N
// slots and rotate on every Map/Update so back-to-back writes don't
// stomp data the GPU is still reading.

#include "webgpu_device.h"
#include "webgpu_device_state.h"
#include "webgpu_handles.h"
#include "webgpu_translate.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace whiteout::flakes::gfx::webgpu {

BufferHandle WebGPUDevice::CreateBuffer(const BufferDesc& desc, const void* initial) {
    auto& state = *state_;

    BufferEntry entry{};
    entry.desc = desc;
    entry.slotStride = desc.size;
    entry.slotCount = 1;
    entry.currentSlot = 0;

    const bool ring = hasFlag(desc.usage, BufferUsage::CpuWritable) && desc.size > 0;
    if (ring) {
        const u64 align = std::max<u64>(1, state.minUniformBufferAlign);
        entry.slotStride = (desc.size + align - 1) / align * align;
        entry.slotCount =
            (desc.ringSlotsHint > 0) ? desc.ringSlotsHint : kCbRingSlots;
        if (entry.slotCount < kFramesInFlight)
            entry.slotCount = kFramesInFlight;
    }
    const u64 totalSize = entry.slotStride * entry.slotCount;

    // Sub-alloc from the shared CB ring when we can — that's the path
    // dynamic-offset bindings take. Falls back to a dedicated buffer when
    // the ring is exhausted (cursor pinned at capacity).
    if (ring && state.sharedCbMapped != nullptr) {
        const u64 align = std::max<u64>(1, state.minUniformBufferAlign);
        const u64 base = (state.sharedCbCursor + align - 1) / align * align;
        if (base + totalSize <= kSharedCbCapacity) {
            entry.buffer = state.sharedCbBuffer;
            entry.isSharedRingAlias = true;
            entry.mapped = state.sharedCbMapped;
            entry.baseOffset = base;
            state.sharedCbCursor = base + totalSize;
            if (initial && desc.size > 0) {
                std::memcpy(entry.mapped + base, initial, desc.size);
            }
            return static_cast<BufferHandle>(state.buffers.Insert(std::move(entry)));
        }
    }

    wgpu::BufferDescriptor bd{};
    bd.size = totalSize;
    bd.usage = ToWgpuBufferUsage(desc.usage);
    bd.mappedAtCreation = (initial != nullptr) && (desc.size > 0);
    entry.buffer = state.device.CreateBuffer(&bd);
    if (!entry.buffer)
        return BufferHandle::Invalid;

    if (initial && desc.size > 0) {
        // mappedAtCreation hands us host-writable memory regardless of the
        // underlying heap type — single-shot upload without a staging
        // buffer is the documented path.
        void* dst = entry.buffer.GetMappedRange(0, desc.size);
        if (dst) {
            std::memcpy(dst, initial, desc.size);
            entry.buffer.Unmap();
        } else {
            // Fallback: queue.WriteBuffer — Dawn handles staging internally.
            state.queue.WriteBuffer(entry.buffer, 0, initial, desc.size);
        }
    }
    return static_cast<BufferHandle>(state.buffers.Insert(std::move(entry)));
}

// Destroy moves the entry out of the slot map (handle invalidates
// immediately) and queues a lambda that drops the wgpu::Buffer ref when
// the GPU has retired every submit pending at queue time.
void WebGPUDevice::Destroy(BufferHandle h) {
    auto& state = *state_;
    auto* buffer = state.buffers.Get(static_cast<u64>(h));
    if (!buffer)
        return;
    // Shared-ring sub-allocs own no GPU memory; the ring buffer lives for
    // the device's lifetime.
    if (buffer->isSharedRingAlias) {
        state.buffers.Remove(static_cast<u64>(h));
        return;
    }
    BufferEntry moved = std::move(*buffer);
    state.buffers.Remove(static_cast<u64>(h));
    std::lock_guard<std::mutex> lock(state.deleteMutex);
    state.pendingDeletes.push_back(PendingDelete{
        state.pendingEpoch + 1,
        [owned = std::move(moved)]() mutable {
            // ~BufferEntry drops the wgpu::Buffer ref.
            (void)owned;
        },
    });
}

void WebGPUDevice::UpdateBuffer(BufferHandle h, const void* data, usize size) {
    auto& state = *state_;
    auto* buffer = state.buffers.Get(static_cast<u64>(h));
    if (!buffer)
        return;
    if (buffer->mapped) {
        // Rotate to the next ring slot so the upcoming Bind* captures
        // this write (matches VulkanDevice::UpdateBuffer).
        if (buffer->slotCount > 1) {
            buffer->currentSlot = (buffer->currentSlot + 1) % buffer->slotCount;
        }
        const u64 off = buffer->currentOffset();
        std::memcpy(buffer->mapped + off, data, size);
        // Persistently-mapped Dawn buffers don't need an explicit flush;
        // Dawn handles cache coherency via the heap type it picked.
    } else {
        // Dedicated buffer with no host pointer: route through queue.
        state.queue.WriteBuffer(buffer->buffer, 0, data, size);
    }
}

void* WebGPUDevice::MapBuffer(BufferHandle h) {
    auto& state = *state_;
    auto* buffer = state.buffers.Get(static_cast<u64>(h));
    if (!buffer)
        return nullptr;
    if (buffer->mapped) {
        if (buffer->slotCount > 1) {
            buffer->currentSlot = (buffer->currentSlot + 1) % buffer->slotCount;
        }
        return buffer->mapped + buffer->currentOffset();
    }
    // Non-mapped buffers don't support map-on-demand in WebGPU's sync API
    // — the renderer's CpuWritable buffers always come back through the
    // shared CB ring above. Return null to signal "use UpdateBuffer".
    return nullptr;
}

void WebGPUDevice::UnmapBuffer(BufferHandle h) {
    // Nothing to do — shared-ring writes are visible without an explicit
    // unmap (Dawn uses a coherent host-visible heap for mappedAtCreation
    // buffers on every backend). Symmetric with VulkanDevice::UnmapBuffer
    // where the flush is implicit for the shared ring.
    (void)h;
}

} // namespace whiteout::flakes::gfx::webgpu
