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
        // Constant buffers rotate per-draw and need a deep ring (kCbRingSlots).
        // Vertex / Index buffers get one write per frame, so kFramesInFlight
        // is enough — and at multi-MB sizes (ImGui's VB) the default 256-slot
        // ring overflows the shared CB and forces a dedicated alloc that
        // MapBuffer can't write to.
        const bool isConstant = hasFlag(desc.usage, BufferUsage::Constant);
        const u32 defaultSlots = isConstant ? kCbRingSlots : kFramesInFlight;
        entry.slotCount =
            (desc.ringSlotsHint > 0) ? desc.ringSlotsHint : defaultSlots;
        if (entry.slotCount < kFramesInFlight)
            entry.slotCount = kFramesInFlight;
    }
    const u64 totalSize = entry.slotStride * entry.slotCount;

    // Sub-alloc from the shared CB ring when we can — that's the path
    // dynamic-offset bindings take. Falls back to a dedicated buffer when
    // the ring is exhausted (cursor pinned at capacity).
    if (ring && state.sharedCbBuffer) {
        const u64 align = std::max<u64>(1, state.minUniformBufferAlign);
        const u64 base = (state.sharedCbCursor + align - 1) / align * align;
        if (base + totalSize <= kSharedCbCapacity) {
            entry.buffer = state.sharedCbBuffer;
            entry.isSharedRingAlias = true;
            entry.mapped = state.sharedCbShadow.data();
            entry.baseOffset = base;
            state.sharedCbCursor = base + totalSize;
            if (initial && desc.size > 0) {
                std::memcpy(entry.mapped + base, initial, desc.size);
                state.queue.WriteBuffer(state.sharedCbBuffer, base, initial, desc.size);
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
    if (buffer->isSharedRingAlias) {
        // Rotate to the next ring slot so the upcoming Bind* captures
        // this write. The shadow copy mirrors what the GPU buffer holds;
        // Queue::WriteBuffer pushes the actual bytes to the shared CB
        // (which is *not* mapped — incompatible with Uniform usage).
        if (buffer->slotCount > 1) {
            buffer->currentSlot = (buffer->currentSlot + 1) % buffer->slotCount;
        }
        const u64 off = buffer->currentOffset();
        std::memcpy(buffer->mapped + off, data, size);
        state.queue.WriteBuffer(state.sharedCbBuffer, off, data, size);
    } else {
        // Dedicated buffer: same path, no shadow needed.
        state.queue.WriteBuffer(buffer->buffer, 0, data, size);
    }
}

void* WebGPUDevice::MapBuffer(BufferHandle h) {
    auto& state = *state_;
    auto* buffer = state.buffers.Get(static_cast<u64>(h));
    if (!buffer)
        return nullptr;
    if (buffer->isSharedRingAlias) {
        // Rotate slot and hand back the CPU shadow pointer. The caller
        // writes through the returned pointer; UnmapBuffer pushes the
        // slot's bytes to the GPU.
        if (buffer->slotCount > 1) {
            buffer->currentSlot = (buffer->currentSlot + 1) % buffer->slotCount;
        }
        return buffer->mapped + buffer->currentOffset();
    }
    // Non-shared CpuWritable buffers don't support map-on-demand in
    // WebGPU's sync API (MapWrite is incompatible with Uniform). The
    // renderer's CpuWritable buffers always come back through the shared
    // CB ring above. Return null to signal "use UpdateBuffer".
    return nullptr;
}

void WebGPUDevice::UnmapBuffer(BufferHandle h) {
    auto& state = *state_;
    auto* buffer = state.buffers.Get(static_cast<u64>(h));
    if (!buffer || !buffer->isSharedRingAlias)
        return;
    // Flush the slot just written by Map → caller-write. We push the
    // full slot size (desc.size) from the shadow to the GPU buffer; the
    // bytes past desc.size in the slot (padding to slotStride) are
    // never read so we don't bother flushing them.
    const u64 off = buffer->currentOffset();
    const u64 size = buffer->desc.size;
    if (size > 0)
        state.queue.WriteBuffer(state.sharedCbBuffer, off, buffer->mapped + off, size);
}

} // namespace whiteout::flakes::gfx::webgpu
