#include "cbem_internal.hpp"

#include <cornflakes/core/determinism.hpp>
#include <cornflakes/diagnostics/issue_codes.hpp>
#include <cornflakes/interface/binding/ir_to_cbem_lowerer.hpp>
#include <cornflakes/interface/schema/opcodes.hpp>
#include <cornflakes/interface/vm/bytecode_exec_context.hpp>
#include <cornflakes/interface/vm/bytecode_trace.hpp>
#include <cornflakes/interface/vm/register_value.hpp>
#include <cornflakes/vm/cbem_interpreter.hpp>
#include <cornflakes/vm/math_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>

namespace whiteout::cornflakes {

bool execExternalClear(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                       IssueBag& issues) noexcept {
    if (ins.operandCount != 1) {
        issues.push(vmFatal(issues::vm::kOperandCount, "CBEM: ExternalClear operand count"));
        return false;
    }
    const u16 byteSlot = static_cast<u16>(ins.operands[0]);
    const u16 slot = canonicalExternalSlot(ctx, byteSlot);
    if (slot >= ctx.externals.size()) {
        issues.push(vmFatal(issues::vm::kExternalOob, "CBEM: external slot out of bounds"));
        return false;
    }
    ctx.externals[slot] = RegisterValue::scalar(0.0F);
    return true;
}

bool execBroadcast(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                   IssueBag& issues) noexcept {
    if (ins.operandCount != 2) {
        issues.push(vmFatal(issues::vm::kOperandCount, "CBEM: Broadcast operand count"));
        return false;
    }
    const u32 dstReg = ins.operands[0];
    const u32 srcReg = ins.operands[1];
    RegisterValue s;
    if (!readSrc(ctx, srcReg, s, issues)) {
        return false;
    }
    const auto d = decodeRegId(dstReg);
    const u8 dstComps = componentCountForBank(d.bank);
    RegisterValue v;
    v.typeBank = d.bank;
    v.componentCount = dstComps;
    const f32 scalar = s.lanes[0];
    for (u8 i = 0; i < dstComps; ++i) {
        v.lanes[i] = scalar;
    }
    return writeDst(ctx, dstReg, v, issues);
}

bool execBinaryArith(const CBEMInstruction& ins, BytecodeExecContext& ctx, IssueBag& issues,
                     MathOp op) noexcept {
    if (ins.operandCount != 3) {
        issues.push(vmFatal(issues::vm::kOperandCount, "CBEM: MathOp* operand count"));
        return false;
    }
    const u32 dstReg = ins.operands[0];
    RegisterValue a;
    RegisterValue b;
    if (!readSrc(ctx, ins.operands[1], a, issues) || !readSrc(ctx, ins.operands[2], b, issues)) {
        return false;
    }
    const auto d = decodeRegId(dstReg);
    const u8 components = componentCountForBank(d.bank);
    const bool integerOp = bankIsIntegral(d.bank);
    RegisterValue out;
    out.typeBank = d.bank;
    if (!applyMathOp(static_cast<u8>(op), a, b, components, integerOp, out, issues)) {
        return false;
    }
    return writeDst(ctx, dstReg, out, issues);
}

bool execMadd(const CBEMInstruction& ins, BytecodeExecContext& ctx, IssueBag& issues) noexcept {
    if (ins.operandCount != CBEMEncoder::kMaddOperandCount) {
        issues.push(vmFatal(issues::vm::kOperandCount, "CBEM: Madd operand count"));
        return false;
    }
    const u32 dstReg = ins.operands[CBEMEncoder::kMaddDstIndex];
    RegisterValue a;
    RegisterValue b;
    RegisterValue c;
    if (!readSrc(ctx, ins.operands[CBEMEncoder::kMaddSrc0Index], a, issues) ||
        !readSrc(ctx, ins.operands[CBEMEncoder::kMaddSrc1Index], b, issues) ||
        !readSrc(ctx, ins.operands[CBEMEncoder::kMaddSrc2Index], c, issues)) {
        return false;
    }
    const auto d = decodeRegId(dstReg);
    const u8 components = componentCountForBank(d.bank);
    RegisterValue out;
    out.typeBank = d.bank;
    out.componentCount = components;
    for (u8 i = 0; i < components; ++i) {
        out.lanes[i] = a.lanes[i] * b.lanes[i] + c.lanes[i];
    }
    return writeDst(ctx, dstReg, out, issues);
}

bool execIDivMulInv(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                    IssueBag& issues) noexcept {
    if (ins.operandCount != 5) {
        issues.push(vmFatal(issues::vm::kOperandCount, "CBEM: IDivMulInv operand count"));
        return false;
    }
    const u32 dstReg = ins.operands[0];
    RegisterValue src0;
    RegisterValue mInvSignReg;
    RegisterValue magicReg;
    RegisterValue shiftReg;
    if (!readSrc(ctx, ins.operands[1], src0, issues) ||
        !readSrc(ctx, ins.operands[2], mInvSignReg, issues) ||
        !readSrc(ctx, ins.operands[3], magicReg, issues) ||
        !readSrc(ctx, ins.operands[4], shiftReg, issues)) {
        return false;
    }
    const auto d = decodeRegId(dstReg);
    const u8 components = componentCountForBank(d.bank);
    RegisterValue out;
    out.typeBank = d.bank;
    out.componentCount = components;
    const i32 mInvSign = laneAsI32(mInvSignReg, 0);
    const i32 magic = laneAsI32(magicReg, 0);
    const u32 shiftAmt = static_cast<u32>(laneAsI32(shiftReg, 0)) & 31U;
    const u32 m_signbit = static_cast<u32>(mInvSign >> 31);
    const u32 add_signbit = static_cast<u32>(magic >> 31);
    const u32 v15 = m_signbit & ~add_signbit;
    for (u8 i = 0; i < components; ++i) {
        const i32 x = laneAsI32(src0, i);
        const u32 v16 = static_cast<u32>(x) & add_signbit & ~m_signbit;
        const i64 prod = static_cast<i64>(magic) * static_cast<i64>(x);
        const u32 hi32 = static_cast<u32>(static_cast<u64>(prod) >> 32);
        const u32 adjusted = hi32 + v16 - (static_cast<u32>(x) & v15);
        const i32 shifted = static_cast<i32>(adjusted) >> shiftAmt;
        const i32 result = shifted + static_cast<i32>(static_cast<u32>(shifted) >> 31);
        setLaneI32(out, i, result);
    }
    return writeDst(ctx, dstReg, out, issues);
}

bool execFunctionProlog(const CBEMInstruction&, BytecodeExecContext& ctx, IssueBag&) noexcept {
    ++ctx.functionDepth;
    return true;
}

bool execFunctionEpilog(const CBEMInstruction&, BytecodeExecContext& ctx,
                        IssueBag& issues) noexcept {
    if (ctx.functionDepth == 0) {
        issues.push(
            vmFatal(issues::vm::kUnmatchedEpilog, "CBEM: FunctionEpilog without matching Prolog"));
        return false;
    }
    --ctx.functionDepth;
    return true;
}
} // namespace whiteout::cornflakes
