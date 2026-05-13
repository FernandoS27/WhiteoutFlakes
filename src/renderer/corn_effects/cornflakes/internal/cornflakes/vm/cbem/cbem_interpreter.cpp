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

std::map<std::string, u64>& getMutableFunctionCallCounts() noexcept {
    static std::map<std::string, u64> kCounts;
    return kCounts;
}

const std::map<std::string, u64>& vmFunctionCallCounts() noexcept {
    return getMutableFunctionCallCounts();
}

void vmResetFunctionCallCounts() noexcept {
    getMutableFunctionCallCounts().clear();
}

Issue vmFatal(u32 code, std::string_view message) noexcept {
    Issue issue;
    issue.severity = Severity::Fatal;
    issue.category = Category::Vm;
    issue.code = code;
    issue.message = message;
    return issue;
}

Issue vmWarn(u32 code, std::string_view message) noexcept {
    Issue issue;
    issue.severity = Severity::Warning;
    issue.category = Category::Vm;
    issue.code = code;
    issue.message = message;
    return issue;
}

namespace {
bool readConst(const BytecodeExecContext& ctx, u32 slot, u8 bankByte, RegisterValue& out,
               IssueBag& issues) noexcept {
    constexpr std::size_t kSlotBytes = 32U;
    const std::size_t off = static_cast<std::size_t>(slot) * kSlotBytes;
    if (off + 16U > ctx.constantsPool.size()) {
        issues.push(vmFatal(issues::vm::kConstPoolOob, "VM: constant-pool slot out of range"));
        return false;
    }
    out = RegisterValue{};
    out.typeBank = bankByte;
    out.componentCount = componentCountForBank(bankByte);
    for (u8 lane = 0; lane < 4; ++lane) {
        std::memcpy(&out.lanes[lane], ctx.constantsPool.data() + off + lane * sizeof(f32),
                    sizeof(f32));
    }
    return true;
}

bool isConstPoolHit(const BytecodeExecContext& ctx, const DecodedRegId& d) noexcept {
    if (d.scope != scope::kConstPool && d.scope != scope::kInput) {
        return false;
    }
    constexpr std::size_t kSlotBytes = 32U;
    const std::size_t off = static_cast<std::size_t>(d.localIdx) * kSlotBytes;
    return off + 16U <= ctx.constantsPool.size();
}
} // namespace

bool readSrc(const BytecodeExecContext& ctx, u32 regId, RegisterValue& out,
             IssueBag& issues) noexcept {
    if (regId == kRegVoid) {
        out = RegisterValue{};
        out.componentCount = 1;
        out.typeBank = bank::kFloat;
        return true;
    }
    const auto d = decodeRegId(regId);
    if (isConstPoolHit(ctx, d)) {
        return readConst(ctx, d.localIdx, d.bank, out, issues);
    }
    if (d.scope >= kScopeRegisterBuckets) {
        issues.push(vmFatal(issues::vm::kRegisterOob, "VM: register scope out of range"));
        return false;
    }
    auto bank = ctx.scopeRegisters[d.scope];
    if (d.localIdx >= bank.size()) {
        issues.push(vmFatal(issues::vm::kRegisterOob, "VM: register index out of bounds"));
        return false;
    }
    out = bank[d.localIdx];
    if (out.componentCount == 0) {
        out.componentCount = componentCountForBank(d.bank);
        out.typeBank = d.bank;
    }
    return true;
}

bool writeDst(BytecodeExecContext& ctx, u32 regId, const RegisterValue& v,
              IssueBag& issues) noexcept {
    const auto d = decodeRegId(regId);
    if (isConstPoolHit(ctx, d) || d.scope == scope::kInput) {
        issues.push(
            vmFatal(issues::vm::kRegisterOob, "VM: write to constant-pool or input register"));
        return false;
    }
    if (d.scope >= kScopeRegisterBuckets) {
        issues.push(vmFatal(issues::vm::kRegisterOob, "VM: register scope out of range"));
        return false;
    }
    auto bank = ctx.scopeRegisters[d.scope];
    if (d.localIdx >= bank.size()) {
        issues.push(vmFatal(issues::vm::kRegisterOob, "VM: register index out of bounds"));
        return false;
    }
    bank[d.localIdx] = v;
    if (bank[d.localIdx].componentCount == 0) {
        bank[d.localIdx].componentCount = componentCountForBank(d.bank);
    }
    if (bank[d.localIdx].typeBank == 0) {
        bank[d.localIdx].typeBank = d.bank;
    }
    return true;
}

u16 canonicalExternalSlot(const BytecodeExecContext& ctx, u16 byteSlot) noexcept {
    if (byteSlot < ctx.externalBindings.size()) {
        const auto& b = ctx.externalBindings[byteSlot];

        if (b.canonicalSlot == 0U && b.slot != 0U) {
            return b.slot;
        }
        return b.canonicalSlot;
    }
    return byteSlot;
}

namespace {
void recordTrace(const CBEMInstruction& ins, BytecodeExecContext& ctx) noexcept {
    BytecodeTrace* trace = ctx.trace;
    if (trace == nullptr) {
        return;
    }
    if (trace->capacity > 0 && trace->events.size() >= trace->capacity) {
        ++trace->dropped;
        return;
    }

    TraceEvent ev;
    ev.streamOffset = ins.streamOffset;
    ev.opcode = ins.opcode;
    ev.dstKind = TraceDstKind::None;

    auto recordRegister = [&](u32 regId) {
        const auto d = decodeRegId(regId);
        if (d.scope < kScopeRegisterBuckets) {
            auto bank = ctx.scopeRegisters[d.scope];
            if (d.localIdx < bank.size()) {
                ev.dstKind = TraceDstKind::Register;
                ev.dstId = regId;
                ev.value = bank[d.localIdx];
            }
        }
    };
    auto recordExternal = [&](u32 byteSlot) {
        const u16 canonical = canonicalExternalSlot(ctx, static_cast<u16>(byteSlot));
        if (canonical < ctx.externals.size()) {
            ev.dstKind = TraceDstKind::External;
            ev.dstId = byteSlot;
            ev.value = ctx.externals[canonical];
        }
    };

    switch (ins.opcode) {

    case Opcode::Nop:
    case Opcode::FunctionProlog:
    case Opcode::FunctionEpilog:
        break;

    case Opcode::StoreToExternal:
        recordExternal(ins.operands[1] & 0xFFFFU);
        break;
    case Opcode::ExternalClear:
        recordExternal(ins.operands[0]);
        break;

    case Opcode::LoadExternal:
    case Opcode::Reinterpret:
    case Opcode::TypeConverter:
    case Opcode::VecSwizzle:
    case Opcode::Select:
    case Opcode::Broadcast:
    case Opcode::MathOpAdd:
    case Opcode::MathOpSub:
    case Opcode::MathOpMul:
    case Opcode::MathOpDiv:
    case Opcode::IDivMulInv:
    case Opcode::Madd:

        recordRegister(ins.opcode == Opcode::VecSwizzle ? ins.operands[1] : ins.operands[0]);
        break;

    case Opcode::VecCtor:
    case Opcode::MathOp:
    case Opcode::MathFunc1:
    case Opcode::MathFunc2:
    case Opcode::MathFunc3:
        recordRegister(ins.operands[1]);
        break;

    case Opcode::FunctionCall: {
        const u32 retReg = ins.operands[4];
        if (retReg != kRegVoid) {
            recordRegister(retReg);
        }
        const u32 extFunc = ins.operands[2];
        if (extFunc < ctx.functions.size()) {
            ev.symbol = ctx.functions[extFunc].symbolName;
        }
        break;
    }

    default:

        break;
    }

    trace->events.push_back(ev);
}
} // namespace

bool CBEMInterpreter::step(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                           IssueBag& issues) const {
    auto run = [&]() -> bool {
        switch (ins.opcode) {
        case Opcode::Nop:
            return execNop(ins, ctx, issues);
        case Opcode::LoadExternal:
            return execLoadExternal(ins, ctx, issues);
        case Opcode::StoreToExternal:
            return execStoreToExternal(ins, ctx, issues);
        case Opcode::Reinterpret:
        case Opcode::TypeConverter:
            return execMove(ins, ctx, issues);
        case Opcode::VecCtor:
            return execVecCtor(ins, ctx, issues);
        case Opcode::VecSwizzle:
            return execVecSwizzle(ins, ctx, issues);
        case Opcode::MathOp:
        case Opcode::MathOpCMeta:
            return execMathOp(ins, ctx, issues);
        case Opcode::MathFunc1:
            return execMathFunc1(ins, ctx, issues);
        case Opcode::MathFunc2:
            return execMathFunc2(ins, ctx, issues);
        case Opcode::MathFunc3:
            return execMathFunc3(ins, ctx, issues);
        case Opcode::Select:
            return execSelect(ins, ctx, issues);
        case Opcode::FunctionCall:
            return execFunctionCall(ins, ctx, issues);
        case Opcode::ExternalClear:
            return execExternalClear(ins, ctx, issues);
        case Opcode::Broadcast:
            return execBroadcast(ins, ctx, issues);
        case Opcode::MathOpAdd:
            return execBinaryArith(ins, ctx, issues, MathOp::Add);
        case Opcode::MathOpSub:
            return execBinaryArith(ins, ctx, issues, MathOp::Sub);
        case Opcode::MathOpMul:
            return execBinaryArith(ins, ctx, issues, MathOp::Mul);
        case Opcode::MathOpDiv:
            return execBinaryArith(ins, ctx, issues, MathOp::Div);
        case Opcode::IDivMulInv:
            return execIDivMulInv(ins, ctx, issues);
        case Opcode::Madd:
            return execMadd(ins, ctx, issues);
        case Opcode::FunctionProlog:
            return execFunctionProlog(ins, ctx, issues);
        case Opcode::FunctionEpilog:
            return execFunctionEpilog(ins, ctx, issues);
        }
        issues.push(vmFatal(issues::vm::kUnknownOpcode, "VM: opcode not in IR or CBEM range"));
        return false;
    };
    const bool ok = run();
    if (ok) {
        recordTrace(ins, ctx);
    }
    return ok;
}

std::size_t CBEMInterpreter::run(std::span<const CBEMInstruction> program, BytecodeExecContext& ctx,
                                 IssueBag& issues) const {
    std::size_t executed = 0;
    for (const auto& ins : program) {
        if (!step(ins, ctx, issues)) {
            break;
        }
        ++executed;
    }
    return executed;
}
} // namespace whiteout::cornflakes
