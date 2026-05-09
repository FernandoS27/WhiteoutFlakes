#pragma once

/// @file
/// @brief IR instruction model and the lowerer that emits the CBEM form executed by the VM.

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/schema/opcodes.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace whiteout::cornflakes {

/// @brief Decoded IR instruction (compiler-emitted, opcode in 0x42..0x53).
struct IRInstruction {
    Opcode opcode{};
    u8 operandCount = 0;
    std::array<u32, 5> operands{};
    std::span<const u32> extraOperands;
};

/// @brief Decoded CBEM/IR instruction as fed to the VM.
struct CBEMInstruction {
    Opcode opcode{};
    u8 operandCount = 0;
    std::array<u32, 5> operands{};
    std::span<const u32> extraOperands;

    u32 streamOffset = 0; ///< Byte offset in the original stream (used for issue context).
};

/// @brief Lowers a single IR instruction to its CBEM-equivalent form.
class IRToCBEMLowerer {
public:
    IRToCBEMLowerer() = default;

    /// @brief Lower one IR instruction; pushes a fatal issue and returns nullopt on failure.
    std::optional<CBEMInstruction> lowerOne(const IRInstruction& ir, IssueBag& issues) const;
};

/// @brief Helpers to synthesise CBEM instructions in the canonical operand layout.
class CBEMEncoder {
public:
    static constexpr std::size_t kMaddDstIndex = 0;
    static constexpr std::size_t kMaddSrc0Index = 1;
    static constexpr std::size_t kMaddSrc1Index = 2;
    static constexpr std::size_t kMaddSrc2Index = 3;
    static constexpr u8 kMaddOperandCount = 4;

    /// @brief Build a Madd: `dst = src0 * src1 + src2`.
    static CBEMInstruction encodeMadd(u32 dst, u32 src0, u32 src1, u32 src2) noexcept;
};

} // namespace whiteout::cornflakes
