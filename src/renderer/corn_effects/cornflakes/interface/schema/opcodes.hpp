#pragma once

/// @file
/// @brief Opcode enum + IR/CBEM range predicates and a name table.
///
/// Two opcode families share the same VM:
///  - **IR** (0x42..0x53) — emitted by the CornFx compiler.
///  - **CBEM** (0x6B..0x7D) — runtime/CBEM-rewriter optimisations layered on top.
/// Both appear in baked .pkb streams.

#include <cornflakes/interface/core/types.hpp>

namespace whiteout::cornflakes {

inline constexpr u8 kIROpcodeMin = 0x42U;
inline constexpr u8 kIROpcodeMax = 0x53U;

constexpr bool isIROpcode(u8 raw) noexcept {
    return raw >= kIROpcodeMin && raw <= kIROpcodeMax;
}

inline constexpr u8 kCBEMOpcodeMin = 0x69U;
inline constexpr u8 kCBEMOpcodeMax = 0x7DU;

constexpr bool isCBEMOpcode(u8 raw) noexcept {
    return raw >= kCBEMOpcodeMin && raw <= kCBEMOpcodeMax;
}

/// @brief All VM opcodes — IR (0x42..0x53) and CBEM (0x6B..0x7D) in one enum.
enum class Opcode : u8 {
    Nop = 0x42U,
    LoadExternal = 0x43U,
    StoreToExternal = 0x44U,
    Reinterpret = 0x4AU,
    TypeConverter = 0x4BU,
    VecCtor = 0x4CU,
    VecSwizzle = 0x4DU,
    MathOp = 0x4EU,
    MathFunc1 = 0x4FU,
    MathFunc2 = 0x50U,
    MathFunc3 = 0x51U,
    Select = 0x52U,
    FunctionCall = 0x53U,

    ExternalClear = 0x6BU,
    Broadcast = 0x6FU,
    MathOpCMeta = 0x70U,
    MathOpAdd = 0x71U,
    MathOpSub = 0x72U,
    MathOpMul = 0x73U,
    MathOpDiv = 0x74U,
    Madd = 0x75U,
    IDivMulInv = 0x76U,
    FunctionProlog = 0x7CU,
    FunctionEpilog = 0x7DU,
};

constexpr bool isIROpcode(Opcode op) noexcept {
    return isIROpcode(static_cast<u8>(op));
}

constexpr bool isCBEMOpcode(Opcode op) noexcept {
    return isCBEMOpcode(static_cast<u8>(op));
}

constexpr const char* opcodeName(Opcode op) noexcept {
    switch (op) {
    case Opcode::Nop:
        return "NOP";
    case Opcode::LoadExternal:
        return "LoadExternal";
    case Opcode::StoreToExternal:
        return "StoreToExternal";
    case Opcode::Reinterpret:
        return "Reinterpret";
    case Opcode::TypeConverter:
        return "TypeConverter";
    case Opcode::VecCtor:
        return "VecCtor";
    case Opcode::VecSwizzle:
        return "VecSwizzle";
    case Opcode::MathOp:
        return "MathOp";
    case Opcode::MathFunc1:
        return "MathFunc1";
    case Opcode::MathFunc2:
        return "MathFunc2";
    case Opcode::MathFunc3:
        return "MathFunc3";
    case Opcode::Select:
        return "Select";
    case Opcode::FunctionCall:
        return "FunctionCall";
    case Opcode::ExternalClear:
        return "ExternalClear";
    case Opcode::Broadcast:
        return "Broadcast";
    case Opcode::MathOpCMeta:
        return "MathOp(CBEM)";
    case Opcode::MathOpAdd:
        return "MathOpAdd";
    case Opcode::MathOpSub:
        return "MathOpSub";
    case Opcode::MathOpMul:
        return "MathOpMul";
    case Opcode::MathOpDiv:
        return "MathOpDiv";
    case Opcode::Madd:
        return "Madd";
    case Opcode::IDivMulInv:
        return "IDivMulInv";
    case Opcode::FunctionProlog:
        return "FunctionProlog";
    case Opcode::FunctionEpilog:
        return "FunctionEpilog";
    }
    return "?";
}

constexpr const char* opcodeName(u8 raw) noexcept {
    return opcodeName(static_cast<Opcode>(raw));
}

} // namespace whiteout::cornflakes
