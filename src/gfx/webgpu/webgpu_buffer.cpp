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

    // GPU→CPU readback buffer. WebGPU's MapRead usage is only combinable
    // with CopyDst, so it gets a dedicated buffer and the async map path
    // below — no ring, no CPU shadow.
    if (hasFlag(desc.usage, BufferUsage::CpuReadable) && desc.size > 0) {
        wgpu::BufferDescriptor bd{};
        bd.size = desc.size;
        bd.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
        bd.mappedAtCreation = false;
        entry.buffer = state.device.CreateBuffer(&bd);
        if (!entry.buffer)
            return BufferHandle::Invalid;
        return static_cast<BufferHandle>(state.buffers.Insert(std::move(entry)));
    }

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
        entry.slotCount = (desc.ringSlotsHint > 0) ? desc.ringSlotsHint : defaultSlots;
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
    // Dedicated CpuWritable buffers (shared-CB ring overflow) need a CPU
    // shadow so MapBuffer/UnmapBuffer behave like the shared-ring path —
    // WebGPU's sync API has no MapWrite for Uniform/Vertex/Index, so
    // without a shadow the renderer's Map call returns null and the
    // upload silently drops on the floor (corrupt CBs → bad transforms →
    // GPU device hung).
    if (ring) {
        entry.dedicatedShadow.assign(static_cast<usize>(totalSize), 0);
        entry.mapped = entry.dedicatedShadow.data();
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
    } else if (!buffer->dedicatedShadow.empty()) {
        // Dedicated CpuWritable with CPU shadow: rotate slot, write
        // shadow, then push to its own wgpu::Buffer.
        if (buffer->slotCount > 1) {
            buffer->currentSlot = (buffer->currentSlot + 1) % buffer->slotCount;
        }
        const u64 off = buffer->currentOffset();
        std::memcpy(buffer->mapped + off, data, size);
        state.queue.WriteBuffer(buffer->buffer, off, data, size);
    } else {
        // Static dedicated buffer (no CpuWritable): single-slot write.
        state.queue.WriteBuffer(buffer->buffer, 0, data, size);
    }
}

void* WebGPUDevice::MapBuffer(BufferHandle h) {
    auto& state = *state_;
    auto* buffer = state.buffers.Get(static_cast<u64>(h));
    if (!buffer)
        return nullptr;

    // Readback buffer: synchronously map for reading. The caller invokes
    // this after the GPU copy into the buffer has retired (frame-capture
    // download), so the WaitAny below returns promptly.
    if (hasFlag(buffer->desc.usage, BufferUsage::CpuReadable)) {
        if (!buffer->buffer)
            return nullptr;
        wgpu::Future f = buffer->buffer.MapAsync(wgpu::MapMode::Read, 0, buffer->desc.size,
                                                 wgpu::CallbackMode::WaitAnyOnly,
                                                 [](wgpu::MapAsyncStatus, wgpu::StringView) {});
        wgpu::FutureWaitInfo wait{f};
        state.instance.WaitAny(1, &wait, UINT64_MAX);
        return const_cast<void*>(buffer->buffer.GetConstMappedRange(0, buffer->desc.size));
    }

    if (!buffer->mapped)
        return nullptr;
    if (buffer->slotCount > 1) {
        buffer->currentSlot = (buffer->currentSlot + 1) % buffer->slotCount;
    }
    return buffer->mapped + buffer->currentOffset();
}

void WebGPUDevice::UnmapBuffer(BufferHandle h) {
    auto& state = *state_;
    auto* buffer = state.buffers.Get(static_cast<u64>(h));
    if (!buffer)
        return;

    // Readback buffer: release the host mapping so the next CopyBuffer
    // into it is legal again.
    if (hasFlag(buffer->desc.usage, BufferUsage::CpuReadable)) {
        if (buffer->buffer)
            buffer->buffer.Unmap();
        return;
    }

    if (!buffer->mapped)
        return;
    // Flush the slot just written by Map. The shared-ring path writes
    // to the device-wide shared CB; the dedicated-shadow path writes to
    // the buffer's own wgpu::Buffer. Only desc.size bytes get pushed
    // (the slot-stride padding past desc.size is never read by shaders).
    const u64 off = buffer->currentOffset();
    const u64 size = buffer->desc.size;
    if (size == 0)
        return;
    if (buffer->isSharedRingAlias)
        state.queue.WriteBuffer(state.sharedCbBuffer, off, buffer->mapped + off, size);
    else
        state.queue.WriteBuffer(buffer->buffer, off, buffer->mapped + off, size);
}

} // namespace whiteout::flakes::gfx::webgpu
