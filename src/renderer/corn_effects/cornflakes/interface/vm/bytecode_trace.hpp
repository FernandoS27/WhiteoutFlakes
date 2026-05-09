#pragma once

/// @file
/// @brief Per-instruction VM trace records — one event per writeback, plus formatting helper.

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/schema/opcodes.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

namespace whiteout::cornflakes {

/// @brief Where a trace event wrote — a register, an external, or nowhere.
enum class TraceDstKind : u8 {
    None,
    Register,
    External,
};

/// @brief One VM-write trace record.
struct TraceEvent {
    u32 streamOffset = 0;
    Opcode opcode{};
    TraceDstKind dstKind = TraceDstKind::None;
    u32 dstId = 0;
    RegisterValue value{};

    std::string_view symbol; ///< For FunctionCall events; empty otherwise.
};

/// @brief Append-only trace buffer with a soft `capacity`; overflow increments `dropped`.
struct BytecodeTrace {
    std::vector<TraceEvent> events;
    std::size_t capacity = 0;
    std::size_t dropped = 0;

    void clear() noexcept {
        events.clear();
        dropped = 0;
    }
};

/// @brief Format `ev` into `out` (NUL-terminated). Returns characters written (excluding NUL).
std::size_t formatTraceEvent(const TraceEvent& ev, char* out, std::size_t outCap) noexcept;

} // namespace whiteout::cornflakes
