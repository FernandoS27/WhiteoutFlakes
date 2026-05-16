// WebGPUDevice ctor / dtor / accessors. Heavy work lives in the other
// webgpu_*.cpp files. Mirrors src/gfx/vulkan/vulkan_device.cpp.

#include "webgpu_command_list.h"
#include "webgpu_device.h"
#include "webgpu_device_state.h"
#include "webgpu_handles.h"

namespace whiteout::flakes::gfx::webgpu {

WebGPUDevice::WebGPUDevice()
    : state_(std::make_unique<WebGPUDeviceState>()),
      immediate_(std::make_unique<WebGPUCommandList>(*this)) {}

WebGPUDevice::~WebGPUDevice() {
    auto& state = *state_;
    if (state.device) {
        // Make sure every in-flight submit retires before resources go.
        // wgpu::Device::Tick + wait-on-OnSubmittedWorkDone is the
        // portable "device-wait-idle" — Dawn additionally exposes a
        // private API but we stick to the public surface.
        wgpu::Future done = state.queue.OnSubmittedWorkDone(
            wgpu::CallbackMode::WaitAnyOnly,
            [](wgpu::QueueWorkDoneStatus) {});
        wgpu::FutureWaitInfo wait{done};
        state.instance.WaitAny(1, &wait, UINT64_MAX);

        // Run every still-queued deleter — completedEpoch may not have
        // caught up yet on the worker thread, but we just waited above.
        for (auto& pending : state.pendingDeletes)
            if (pending.deleter)
                pending.deleter();
        state.pendingDeletes.clear();
    }

    // SlotMaps clear their entries on Clear(); the wgpu::* members'
    // destructors drop refs and free the underlying objects.
    state.buffers.Clear();
    state.textures.Clear();
    state.shaders.Clear();
    state.pipelines.Clear();
    state.samplers.Clear();
    state.swapchains.Clear();
}

WebGPUDeviceState& WebGPUDevice::State() {
    return *state_;
}
const WebGPUDeviceState& WebGPUDevice::State() const {
    return *state_;
}

const char* WebGPUDevice::GetDeviceName() const {
    return deviceName_.c_str();
}

IGFXCommandList* WebGPUDevice::GetImmediateContext() {
    return immediate_.get();
}

Format WebGPUDevice::PreferredDepthStencilFormat() const {
    // Depth24PlusStencil8 is universally supported; Depth32FloatStencil8
    // requires the matching feature. We pick at Init() and cache in state_;
    // hand back the gfx-side enum here.
    return Format::D24_UNORM_S8_UINT;
}

} // namespace whiteout::flakes::gfx::webgpu
