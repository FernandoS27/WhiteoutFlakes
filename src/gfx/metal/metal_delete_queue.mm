// Drain the deferred-delete queue when the GPU has retired enough work
// that pending entries are safe to release. Called from BeginRenderPass
// (the per-frame fast path) and explicitly from the device destructor.
//
// Pending entries are tagged with the pendingEpoch value at queue time;
// the addCompletedHandler in Present bumps completedEpoch when the
// matching command buffer retires. Anything whose retireAfter is <=
// completedEpoch is safe to release.

#include "metal_device.h"
#include "metal_device_state.h"
#include "metal_handles.h"

namespace whiteout::flakes::gfx::metal {

// Public so metal_command_list.mm can call it at BeginRenderPass /
// CopyBuffer / Dispatch — anywhere we open a new frame's command
// buffer. Keeps the queue from growing unbounded if the renderer
// destroys resources between submits.
void DrainPendingDeletes(MetalDeviceState& state) {
    const DeleteEpoch retired = state.completedEpoch.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(state.pendingDeletesMutex);
    while (!state.pendingDeletes.empty() &&
           state.pendingDeletes.front().retireAfter <= retired) {
        auto del = std::move(state.pendingDeletes.front());
        state.pendingDeletes.pop_front();
        // Run outside the lock would be nicer (the deleter might allocate
        // / log), but the queue is small (<100 entries typical) and the
        // critical section is short — easier to keep correctness than
        // shave nanoseconds here.
        if (del.deleter)
            del.deleter();
    }
}

} // namespace whiteout::flakes::gfx::metal
