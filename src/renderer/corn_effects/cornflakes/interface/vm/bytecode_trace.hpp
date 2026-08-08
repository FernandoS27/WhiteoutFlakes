#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/schema/opcodes.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <cstddef>
#include <string_view>
#include <vector>

namespace whiteout::cornflakes {

enum class TraceDstKind : u8 {
    None,
    Register,
    External,
};

struct TraceEvent {
    u32 streamOffset = 0;
    Opcode opcode{};
    TraceDstKind dstKind = TraceDstKind::None;
    u32 dstId = 0;
    RegisterValue value{};

    std::string_view symbol;
};

struct BytecodeTrace {
    std::vector<TraceEvent> events;
    std::size_t capacity = 0;
    std::size_t dropped = 0;

    void clear() noexcept {
        events.clear();
        dropped = 0;
    }
};

std::size_t formatTraceEvent(const TraceEvent& ev, char* out, std::size_t outCap) noexcept;

}
