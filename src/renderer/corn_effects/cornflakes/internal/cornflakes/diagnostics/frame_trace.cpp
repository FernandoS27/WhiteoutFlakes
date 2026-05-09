#include <cornflakes/core/determinism.hpp>
#include <cornflakes/diagnostics/frame_trace.hpp>

namespace whiteout::cornflakes {

void FrameTraceRecorder::beginFrame(FrameId frame, f32 dt) noexcept {
    m_current = {};
    m_current.frame = frame;
    m_current.tickDt = dt;
}

void FrameTraceRecorder::addEmitter() noexcept {
    ++m_current.emittersTicked;
}

void FrameTraceRecorder::addPage(u32 particleCount) noexcept {
    ++m_current.pagesTicked;
    m_current.particlesTouched += particleCount;
}

void FrameTraceRecorder::addPacket() noexcept {
    ++m_current.packetsProduced;
}

void FrameTraceRecorder::endFrame() noexcept {
    m_last = m_current;
}

const FrameTrace& FrameTraceRecorder::lastFrame() const noexcept {
    return m_last;
}

} // namespace whiteout::cornflakes
