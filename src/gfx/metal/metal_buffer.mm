// Buffer create / destroy / map / update. Mirrors the ring-slot model in
// src/gfx/webgpu/webgpu_buffer.cpp — CpuWritable+Constant buffers get N
// slots and rotate on every Map/Update so back-to-back writes don't
// stomp data the GPU is still reading.
//
// Apple Silicon's unified memory simplifies things vs. WebGPU/Vulkan:
//   • MTLStorageModeShared exposes a host pointer ([buffer contents])
//     that writes directly into GPU-visible memory. No staging /
//     queue::WriteBuffer round-trip is needed.
//   • Every buffer is host-mappable when the storage mode allows it,
//     so there's no need for a separate CPU shadow on the
//     dedicated-buffer overflow path (unlike webgpu_buffer.cpp's
//     dedicatedShadow).
//   • The shared-CB ring is just a single big MTLBuffer; sub-allocs
//     alias state.sharedCb with an offset.

#include "metal_device.h"
#include "metal_device_state.h"
#include "metal_handles.h"
#include "metal_translate.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace whiteout::flakes::gfx::metal {

namespace {

MTLResourceOptions ChooseStorageMode(BufferUsage usage) {
    // CpuWritable / CpuReadable need host-mappable storage. On Apple
    // Silicon Shared is unified memory; on Intel/AMD-discrete Macs the
    // user-visible perf hit is the synchronisation, not the alloc. We
    // ship Apple-Silicon-only so Shared is always the right pick for
    // host-touched buffers.
    if (hasFlag(usage, BufferUsage::CpuWritable) ||
        hasFlag(usage, BufferUsage::CpuReadable))
        return MTLResourceStorageModeShared;
    // Private buffers: GPU-only, lowest latency for static VB/IB and
    // structured storage buffers. Initial uploads use a one-shot blit
    // from a temp shared buffer (see InitialUploadToPrivate).
    return MTLResourceStorageModePrivate;
}

void AccountAlloc(MetalDeviceState& state, u64 bytes) {
    state.gpuBytesAlloc.fetch_add(bytes, std::memory_order_relaxed);
}
void AccountFree(MetalDeviceState& state, u64 bytes) {
    state.gpuBytesFreed.fetch_add(bytes, std::memory_order_relaxed);
}

// One-shot upload of `initial` into a private-storage buffer. Allocates
// a temp shared MTLBuffer, copies the bytes, then queues a blit copy on
// a one-shot command buffer. The temp buffer is captured by the command
// buffer's retain set, so ARC keeps it alive until the GPU finishes.
bool InitialUploadToPrivate(MetalDeviceState& state, id<MTLBuffer> dst,
                            const void* initial, u64 size) {
    if (!dst || !initial || size == 0)
        return true;
    id<MTLBuffer> staging =
        [state.device newBufferWithBytes:initial
                                  length:size
                                 options:MTLResourceStorageModeShared];
    if (!staging)
        return false;

    id<MTLCommandBuffer> cb = [state.commandQueue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromBuffer:staging sourceOffset:0 toBuffer:dst destinationOffset:0 size:size];
    [blit endEncoding];
    // Don't wait — ARC keeps `staging` alive via the command buffer's
    // internal retain set until completion.
    [cb commit];
    return true;
}

} // namespace

BufferHandle MetalDevice::CreateBuffer(const BufferDesc& desc, const void* initial) {
    @autoreleasepool {
        auto& state = *state_;

        BufferEntry entry{};
        entry.desc = desc;
        entry.slotStride = desc.size;
        entry.slotCount = 1;
        entry.currentSlot = 0;

        if (desc.size == 0)
            return BufferHandle::Invalid;

        // ---- Ring layout for CpuWritable buffers ----
        const bool ring = hasFlag(desc.usage, BufferUsage::CpuWritable);
        if (ring) {
            const u64 align = std::max<u64>(1, state.minUniformBufferAlign);
            entry.slotStride = (desc.size + align - 1) / align * align;
            const bool isConstant = hasFlag(desc.usage, BufferUsage::Constant);
            const u32 defaultSlots = isConstant ? kCbRingSlots : kFramesInFlight;
            entry.slotCount = (desc.ringSlotsHint > 0) ? desc.ringSlotsHint : defaultSlots;
            if (entry.slotCount < kFramesInFlight)
                entry.slotCount = kFramesInFlight;
        }
        const u64 totalSize = entry.slotStride * entry.slotCount;

        // ---- Shared-CB sub-alloc ----
        // The shared ring is the path dynamic-offset bindings take. The
        // bound buffer is always state.sharedCb; the offset is the
        // sub-alloc base + ring-slot stride. Falls back to a dedicated
        // shared buffer when the ring is full.
        if (ring && state.sharedCb) {
            const u64 align = std::max<u64>(1, state.minUniformBufferAlign);
            const u64 base = (state.sharedCbCursor + align - 1) / align * align;
            if (base + totalSize <= state.sharedCbCapacity) {
                entry.buffer = state.sharedCb;
                entry.isSharedRingAlias = true;
                entry.mapped = state.sharedCbMapped;
                entry.baseOffset = base;
                state.sharedCbCursor = base + totalSize;
                if (initial)
                    std::memcpy(entry.mapped + base, initial, desc.size);
                return static_cast<BufferHandle>(state.buffers.Insert(std::move(entry)));
            }
        }

        // ---- Dedicated buffer ----
        MTLResourceOptions options = ChooseStorageMode(desc.usage);
        id<MTLBuffer> mtl = nil;
        if (initial && options == MTLResourceStorageModeShared) {
            // Shared storage + initial data: one allocation, host-side
            // memcpy after the fact (matches webgpu's mappedAtCreation
            // shape).
            mtl = [state.device newBufferWithLength:totalSize options:options];
            if (mtl)
                std::memcpy([mtl contents], initial, desc.size);
        } else if (options == MTLResourceStorageModePrivate && initial) {
            // Private storage + initial data: allocate empty, blit from
            // a one-shot staging buffer.
            mtl = [state.device newBufferWithLength:totalSize options:options];
            if (mtl && !InitialUploadToPrivate(state, mtl, initial, desc.size))
                mtl = nil;
        } else {
            mtl = [state.device newBufferWithLength:totalSize options:options];
        }
        if (!mtl)
            return BufferHandle::Invalid;
        if (state.validationRequested)
            mtl.label =
                hasFlag(desc.usage, BufferUsage::Constant) ? @"wf.cb"
                : hasFlag(desc.usage, BufferUsage::Vertex) ? @"wf.vb"
                : hasFlag(desc.usage, BufferUsage::Index)  ? @"wf.ib"
                                                           : @"wf.buf";

        entry.buffer = mtl;
        // For dedicated CpuWritable buffers we expose the unified-memory
        // host pointer directly. CpuReadable behaves the same way — Map
        // returns [buffer contents] unconditionally.
        if (options == MTLResourceStorageModeShared)
            entry.mapped = static_cast<uint8_t*>([mtl contents]);
        entry.byteSize = totalSize;
        AccountAlloc(state, entry.byteSize);

        return static_cast<BufferHandle>(state.buffers.Insert(std::move(entry)));
    }
}

void MetalDevice::Destroy(BufferHandle h) {
    auto& state = *state_;
    auto* buffer = state.buffers.Get(static_cast<u64>(h));
    if (!buffer)
        return;

    // Shared-ring sub-allocs don't own GPU memory; the ring lives for
    // the device's lifetime. Just drop the slot.
    if (buffer->isSharedRingAlias) {
        state.buffers.Remove(static_cast<u64>(h));
        return;
    }

    BufferEntry moved = std::move(*buffer);
    state.buffers.Remove(static_cast<u64>(h));

    // Defer the ARC drop until every in-flight submit retires — Metal's
    // command buffers retain bound resources internally, but the safe
    // pattern is "GPU finishes the work that referenced this, then
    // release". Capture by strong ref; the deleter just lets the
    // captured `id<MTLBuffer>` go out of scope.
    const u64 retireAfter = state.pendingEpoch + 1;
    const u64 bytes = moved.byteSize;
    id<MTLBuffer> mtl = moved.buffer;
    {
        std::lock_guard<std::mutex> lock(state.pendingDeletesMutex);
        state.pendingDeletes.push_back(PendingDelete{
            retireAfter,
            [mtl, &state, bytes]() {
                (void)mtl;  // strong-ref drops here
                AccountFree(state, bytes);
            },
        });
    }
}

void MetalDevice::UpdateBuffer(BufferHandle h, const void* data, usize size) {
    auto& state = *state_;
    (void)state;
    auto* buffer = state.buffers.Get(static_cast<u64>(h));
    if (!buffer || !data || size == 0)
        return;

    // Rotate to the next ring slot so the upcoming Bind* captures this
    // write (mirrors WebGPU's UpdateBuffer behaviour). The slot rotation
    // is a no-op for slotCount=1 (static dedicated buffers).
    if (buffer->slotCount > 1)
        buffer->currentSlot = (buffer->currentSlot + 1) % buffer->slotCount;
    const u64 off = buffer->currentOffset();

    if (buffer->mapped) {
        // Shared / shared-ring path: host write lands directly in
        // GPU-visible memory; no flush needed on Apple Silicon.
        std::memcpy(buffer->mapped + off, data, size);
        return;
    }
    // Static private buffer with no host map: blit-upload from a
    // one-shot staging buffer at the slot offset.
    if (buffer->buffer) {
        @autoreleasepool {
            id<MTLBuffer> staging =
                [state.device newBufferWithBytes:data
                                          length:size
                                         options:MTLResourceStorageModeShared];
            if (!staging)
                return;
            id<MTLCommandBuffer> cb = [state.commandQueue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromBuffer:staging
                    sourceOffset:0
                        toBuffer:buffer->buffer
               destinationOffset:off
                            size:size];
            [blit endEncoding];
            [cb commit];
        }
    }
}

void* MetalDevice::MapBuffer(BufferHandle h) {
    auto& state = *state_;
    auto* buffer = state.buffers.Get(static_cast<u64>(h));
    if (!buffer || !buffer->mapped)
        return nullptr;

    if (buffer->slotCount > 1)
        buffer->currentSlot = (buffer->currentSlot + 1) % buffer->slotCount;
    return buffer->mapped + buffer->currentOffset();
}

void MetalDevice::UnmapBuffer(BufferHandle h) {
    // On Apple Silicon MTLStorageModeShared is unified memory: writes
    // land in GPU-visible memory without an explicit flush. The
    // function exists for API parity with the other backends.
    (void)h;
}

} // namespace whiteout::flakes::gfx::metal
