// MetalDevice ctor / dtor / accessors. The heavy IGFXDevice work lives
// in the per-domain files (metal_buffer.mm, metal_texture.mm,
// metal_pipeline.mm, metal_swap_chain.mm). Mirrors webgpu_device.cpp.

#include "metal_command_list.h"
#include "metal_device.h"
#include "metal_device_state.h"
#include "metal_handles.h"

#import <Metal/Metal.h>

namespace whiteout::flakes::gfx::metal {

MetalDevice::MetalDevice()
    : state_(std::make_unique<MetalDeviceState>()),
      immediate_(std::make_unique<MetalCommandList>(*this)) {}

MetalDevice::~MetalDevice() {
    @autoreleasepool {
        auto& state = *state_;
        if (state.commandQueue) {
            WaitIdle();
            {
                std::lock_guard<std::mutex> lock(state.pendingDeletesMutex);
                for (auto& pending : state.pendingDeletes)
                    if (pending.deleter)
                        pending.deleter();
                state.pendingDeletes.clear();
            }
        }

        // SlotMap::Clear drops every entry's id<MTL...> strong refs; ARC
        // takes care of the actual release. The Metal device + queue
        // release when state_ unwinds.
        state.buffers.Clear();
        state.textures.Clear();
        state.shaders.Clear();
        state.pipelines.Clear();
        state.samplers.Clear();
        state.swapchains.Clear();

        for (auto& f : state.frames) {
            f.commandBuffer = nil;
            f.renderEncoder = nil;
            f.computeEncoder = nil;
            f.blitEncoder = nil;
        }
        state.sharedCb = nil;
        state.commandQueue = nil;
        state.device = nil;
    }
}

MetalDeviceState& MetalDevice::State() {
    return *state_;
}
const MetalDeviceState& MetalDevice::State() const {
    return *state_;
}

const char* MetalDevice::GetDeviceName() const {
    return deviceName_.c_str();
}

u64 MetalDevice::LiveGpuBytes() const {
    const u64 a = state_->gpuBytesAlloc.load(std::memory_order_relaxed);
    const u64 f = state_->gpuBytesFreed.load(std::memory_order_relaxed);
    return (a > f) ? (a - f) : 0;
}

IGFXCommandList* MetalDevice::GetImmediateContext() {
    return immediate_.get();
}

void MetalDevice::WaitIdle() {
    @autoreleasepool {
        auto& state = *state_;
        if (!state.commandQueue)
            return;
        // Empty command buffer used as a sync barrier: the only way to
        // wait for "every prior submission" on a queue without keeping a
        // shadow list of every committed buffer.
        id<MTLCommandBuffer> sync = [state.commandQueue commandBuffer];
        sync.label = @"wf.WaitIdle";
        [sync commit];
        [sync waitUntilCompleted];
    }
}

Format MetalDevice::PreferredDepthStencilFormat() const {
    // D24S8 is not natively supported on Apple Silicon ([device
    // depth24Stencil8PixelFormatSupported] returns NO). D32-Float + S8
    // is universally available. Returning the gfx-side enum that maps
    // to MTLPixelFormatDepth32Float_Stencil8 keeps shipping shaders
    // working without any extra translation.
    return Format::D32_FLOAT_S8_UINT;
}

// ---- IGFXDevice stubs (Phase A skeleton) ----
// Concrete implementations land in metal_buffer.mm / metal_texture.mm /
// metal_pipeline.mm / metal_swap_chain.mm in phases (c)..(f). Returning
// Invalid handles + ignoring writes lets the host code at least call
// these without crashing while the rest of the backend is being built.

BufferHandle MetalDevice::CreateBuffer(const BufferDesc&, const void*) {
    return BufferHandle::Invalid;
}
TextureHandle MetalDevice::CreateTexture(const TextureDesc&, const void*) {
    return TextureHandle::Invalid;
}
ShaderHandle MetalDevice::CreateShader(ShaderStage, const void*, usize) {
    return ShaderHandle::Invalid;
}
PipelineHandle MetalDevice::CreateGraphicsPipeline(const GraphicsPipelineDesc&) {
    return PipelineHandle::Invalid;
}
PipelineHandle MetalDevice::CreateComputePipeline(const ComputePipelineDesc&) {
    return PipelineHandle::Invalid;
}
SamplerHandle MetalDevice::CreateSampler(const SamplerDesc&) {
    return SamplerHandle::Invalid;
}

void MetalDevice::Destroy(BufferHandle) {}
void MetalDevice::Destroy(TextureHandle) {}
void MetalDevice::Destroy(ShaderHandle) {}
void MetalDevice::Destroy(PipelineHandle) {}
void MetalDevice::Destroy(SamplerHandle) {}

void MetalDevice::UpdateBuffer(BufferHandle, const void*, usize) {}
void* MetalDevice::MapBuffer(BufferHandle) {
    return nullptr;
}
void MetalDevice::UnmapBuffer(BufferHandle) {}

// Swap-chain methods live in metal_swap_chain.mm.

TextureHandle MetalDevice::CreateColorTarget(i32, i32, Format) {
    return TextureHandle::Invalid;
}
TextureHandle MetalDevice::CreateDepthTarget(i32, i32, Format) {
    return TextureHandle::Invalid;
}

} // namespace whiteout::flakes::gfx::metal
