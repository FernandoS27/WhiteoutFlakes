#pragma once

/// @file
/// @brief Lightweight per-frame counters (emitters/pages/packets) and the recorder that fills them.

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/schema/handles.hpp>

#include <cstddef>

namespace whiteout::cornflakes {

/// @brief Aggregate counters for one finished frame.
struct FrameTrace {
    FrameId frame{};
    f32 tickDt = 0.0F;
    u32 emittersTicked = 0;
    u32 pagesTicked = 0;
    u32 packetsProduced = 0;
    u64 particlesTouched = 0;
};

/// @brief Accumulates a `FrameTrace` between `beginFrame` / `endFrame`.
class FrameTraceRecorder {
public:
    FrameTraceRecorder() = default;

    void beginFrame(FrameId frame, f32 dt) noexcept;

    void addEmitter() noexcept;
    void addPage(u32 particleCount) noexcept;
    void addPacket() noexcept;

    void endFrame() noexcept;

    const FrameTrace& lastFrame() const noexcept;

private:
    FrameTrace m_current{};
    FrameTrace m_last{};
};

} // namespace whiteout::cornflakes
