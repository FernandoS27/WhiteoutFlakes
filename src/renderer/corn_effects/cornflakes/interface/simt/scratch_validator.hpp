#pragma once

#include <cornflakes/interface/binding/ir_to_cbem_lowerer.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace whiteout::cornflakes::simt {

template <typename OnRead, typename OnWrite>
void forEachRegisterOperand(const CBEMInstruction& ins, OnRead onRead, OnWrite onWrite) {
    const auto read = [&](std::size_t i) {
        if (i < ins.operands.size()) {
            onRead(ins.operands[i]);
        }
    };
    const auto write = [&](std::size_t i) {
        if (i < ins.operands.size() && ins.operands[i] != kRegVoid) {
            onWrite(ins.operands[i]);
        }
    };

    switch (ins.opcode) {
    case Opcode::Nop:
    case Opcode::FunctionProlog:
    case Opcode::FunctionEpilog:
    case Opcode::ExternalClear:
        break;

    case Opcode::LoadExternal:
        write(0);
        break;
    case Opcode::StoreToExternal:
        read(0);
        break;

    case Opcode::Reinterpret:
    case Opcode::TypeConverter:
    case Opcode::Broadcast:
        read(1);
        write(0);
        break;

    case Opcode::VecSwizzle:
        read(2);
        write(1);
        break;

    case Opcode::VecCtor:
        for (const u32 src : ins.extraOperands) {
            onRead(src);
        }
        write(1);
        break;

    case Opcode::MathOp:
    case Opcode::MathOpCMeta:
    case Opcode::MathFunc2:
        read(2);
        read(3);
        write(1);
        break;

    case Opcode::MathFunc1:
        read(2);
        write(1);
        break;

    case Opcode::MathFunc3:
        read(2);
        read(3);
        read(4);
        write(1);
        break;

    case Opcode::Select:
        read(1);
        read(2);
        read(3);
        write(0);
        break;

    case Opcode::MathOpAdd:
    case Opcode::MathOpSub:
    case Opcode::MathOpMul:
    case Opcode::MathOpDiv:
        read(1);
        read(2);
        write(0);
        break;

    case Opcode::Madd:
        read(1);
        read(2);
        read(3);
        write(0);
        break;

    case Opcode::IDivMulInv:
        read(1);
        read(2);
        read(3);
        read(4);
        write(0);
        break;

    case Opcode::FunctionCall:
        for (std::size_t i = 1; i < ins.extraOperands.size(); i += 2) {
            onRead(ins.extraOperands[i]);
        }
        write(4);
        break;
    }
}

struct LocalReadBeforeWrite {
    std::size_t instructionIndex = 0U;
    u32 streamOffset = 0U;
    Opcode opcode{};
    u32 registerId = 0U;
    u32 localIndex = 0U;
};

struct ScratchReport {
    std::vector<LocalReadBeforeWrite> findings;
    std::size_t instructionsScanned = 0U;
    std::size_t localReads = 0U;
    std::size_t localWrites = 0U;

    bool clean() const noexcept {
        return findings.empty();
    }
};

ScratchReport analyseLocalScratch(std::span<const CBEMInstruction> program);

u64 boundProgramViolationCount() noexcept;

const char* firstBoundProgramViolation() noexcept;

void resetBoundProgramViolations() noexcept;

void checkBoundProgram(std::span<const CBEMInstruction> program) noexcept;

}
