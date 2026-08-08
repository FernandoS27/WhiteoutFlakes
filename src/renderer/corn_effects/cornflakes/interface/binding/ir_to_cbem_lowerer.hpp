#pragma once

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/schema/opcodes.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace whiteout::cornflakes {

struct IRInstruction {
    Opcode opcode{};
    u8 operandCount = 0;
    std::array<u32, 5> operands{};
    std::span<const u32> extraOperands;
};

struct CBEMInstruction {
    Opcode opcode{};
    u8 operandCount = 0;
    std::array<u32, 5> operands{};
    std::span<const u32> extraOperands;

    u32 streamOffset = 0;
};

class IRToCBEMLowerer {
public:
    IRToCBEMLowerer() = default;

    std::optional<CBEMInstruction> lowerOne(const IRInstruction& ir, IssueBag& issues) const;
};

class CBEMEncoder {
public:
    static constexpr std::size_t kMaddDstIndex = 0;
    static constexpr std::size_t kMaddSrc0Index = 1;
    static constexpr std::size_t kMaddSrc1Index = 2;
    static constexpr std::size_t kMaddSrc2Index = 3;
    static constexpr u8 kMaddOperandCount = 4;

    static CBEMInstruction encodeMadd(u32 dst, u32 src0, u32 src1, u32 src2) noexcept;
};

}
