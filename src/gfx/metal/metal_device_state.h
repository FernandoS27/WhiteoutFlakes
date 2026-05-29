#pragma once

// MetalDeviceState — the PIMPL aggregate that hides every Obj-C / Metal
// type from the rest of the gfx layer. Included only from .mm files
// under src/gfx/metal/; the rest of the codebase reaches the device
// through metal_device.h (pure C++).

#include "gfx/common/slot_map.h"
#include "metal_handles.h"

#import <Metal/Metal.h>

#include <array>
#include <atomic>
#include <deque>
#include <mutex>

namespace whiteout::flakes::gfx::metal {

struct MetalDeviceState {
    // ---- Core Metal objects ----
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> commandQueue = nil;

    // Optional: validation labels and verbose logging. Set at Init when
    // the caller asks for `enableValidation`.
    bool validationRequested = false;

    // ---- Slot maps ----
    SlotMap<BufferEntry> buffers;
    SlotMap<TextureEntry> textures;
    SlotMap<ShaderEntry> shaders;
    SlotMap<PipelineEntry> pipelines;
    SlotMap<SamplerEntry> samplers;
    SlotMap<SwapChainEntry> swapchains;

    // ---- Per-frame ring ----
    std::array<FrameContext, kFramesInFlight> frames{};
    u32 currentFrame = 0;
    DeleteEpoch pendingEpoch = 0;
    std::atomic<DeleteEpoch> completedEpoch{0};

    // ---- Shared upload ring ----
    // 64 MiB shared MTLBuffer (MTLStorageModeShared on Apple Silicon = no
    // upload round-trip, host writes land in GPU-visible memory directly).
    // CpuWritable buffers sub-alloc here; fallback to dedicated when the
    // cursor overflows. See metal_buffer.mm.
    id<MTLBuffer> sharedCb = nil;
    uint8_t* sharedCbMapped = nullptr;
    u64 sharedCbCursor = 0;
    u64 sharedCbCapacity = 0;

    // Slot-stride rounding floor. Metal accepts any alignment for
    // setVertexBuffer:offset:atIndex: but slangc emits std140-aligned
    // structs (16-byte padding everywhere) — 256 matches the
    // d3d11 / d3d12 / vulkan / webgpu floor so the same renderer code
    // produces aligned offsets across every backend.
    u64 minUniformBufferAlign = 256;

    // Zero-filled vertex buffer used to back phantom attributes
    // (see CreateGraphicsPipeline + ShaderEntry::declaredVertexAttrs).
    // Stride = 16 — enough room for the widest single-attribute Metal
    // vertex format (Float4 / UInt4 / Half4); stepFunction=Constant
    // means every vertex reads from the same 16-byte window. 16 bytes
    // total is enough for the device's lifetime.
    id<MTLBuffer> zeroVertexBuffer = nil;

    // ---- GPU-byte accounting (diagnostic; LiveGpuBytes reports this) ----
    std::atomic<u64> gpuBytesAlloc{0};
    std::atomic<u64> gpuBytesFreed{0};

    // ---- Deferred deletes ----
    // Strong-ref captures in `deleter` keep id<MTL...> alive until the
    // matching command buffer retires (completedEpoch >= retireAfter).
    std::mutex pendingDeletesMutex;
    std::deque<PendingDelete> pendingDeletes;
};

} // namespace whiteout::flakes::gfx::metal
