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
    id<MTLBuffer> sharedCb = nil;
    uint8_t* sharedCbMapped = nullptr;
    u64 sharedCbCursor = 0;
    u64 sharedCbCapacity = 0;

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
