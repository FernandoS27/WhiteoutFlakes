// Deferred-delete drain + swap-chain acquire helper + per-frame submit.
//
// WebGPU has no timeline semaphores: we approximate with a monotonically
// increasing DeleteEpoch, bumped by queue.Submit and acknowledged by
// OnSubmittedWorkDone. Destroy() tags entries with `pendingEpoch`;
// DrainPendingDeletes pops everything with epoch <= completedEpoch on
// the next frame's first BeginRenderPass.

#include "webgpu_device_state.h"
#include "webgpu_handles.h"

#include <utility>

namespace whiteout::flakes::gfx::webgpu {

void DrainPendingDeletes(WebGPUDeviceState& state) {
    const DeleteEpoch completed = state.completedEpoch.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lock(state.deleteMutex);
    while (!state.pendingDeletes.empty() && state.pendingDeletes.front().epoch <= completed) {
        auto& entry = state.pendingDeletes.front();
        if (entry.deleter)
            entry.deleter();
        state.pendingDeletes.pop_front();
    }
}

void SubmitFrameAndBumpEpoch(WebGPUDeviceState& state) {
    auto& frame = state.frames[state.frameIndex];
    if (!frame.recording)
        return;

    wgpu::CommandBuffer cb = frame.encoder.Finish();
    frame.encoder = nullptr;
    frame.recording = false;
    if (!cb)
        return;

    state.queue.Submit(1, &cb);
    ++state.pendingEpoch;
    const DeleteEpoch epoch = state.pendingEpoch;
    // OnSubmittedWorkDone fires on Dawn's worker thread once the GPU
    // retires this submit. The atomic store gives the renderer thread a
    // monotonic high-water mark to drain against.
    // Callback signature evolved across Dawn versions: native Dawn accepts
    // (status), emdawnwebgpu (web) requires (status, message). Both accept
    // the wider signature, so we use it unconditionally.
    state.queue.OnSubmittedWorkDone(
        wgpu::CallbackMode::AllowSpontaneous,
        [&state, epoch](wgpu::QueueWorkDoneStatus, wgpu::StringView) {
            DeleteEpoch prev = state.completedEpoch.load(std::memory_order_relaxed);
            while (epoch > prev &&
                   !state.completedEpoch.compare_exchange_weak(
                       prev, epoch, std::memory_order_release, std::memory_order_relaxed)) {
            }
        });

    // Rotate frame slot. We don't gate on inflight (Dawn caps in-flight
    // submits internally); the frame slot is purely so the renderer
    // observes the same triple-buffer cadence as Vulkan/D3D12.
    state.frameIndex = (state.frameIndex + 1) % kFramesInFlight;
}

} // namespace whiteout::flakes::gfx::webgpu
