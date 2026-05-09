#include "cbem_internal.hpp"

#include <cornflakes/interface/binding/ir_to_cbem_lowerer.hpp>
#include <cornflakes/core/determinism.hpp>
#include <cornflakes/diagnostics/issue_codes.hpp>
#include <cornflakes/interface/schema/opcodes.hpp>
#include <cornflakes/interface/vm/bytecode_exec_context.hpp>
#include <cornflakes/interface/vm/bytecode_trace.hpp>
#include <cornflakes/vm/cbem_interpreter.hpp>
#include <cornflakes/vm/math_functions.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

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
namespace {
enum class ScalarFamily : u8 { Float, Int, Bool, Other };

constexpr ScalarFamily scalarFamilyForBank(u8 b) noexcept {
    if (b == bank::kBool) {
        return ScalarFamily::Bool;
    }
    if (b == bank::kInt || b == bank::kInt2 || b == bank::kInt2Alt || b == bank::kInt2Alt2 ||
        b == bank::kInt3 || b == bank::kInt4 || b == bank::kIntAlt) {
        return ScalarFamily::Int;
    }
    if (b == bank::kFloat || b == bank::kFloat2 || b == bank::kFloat3 || b == bank::kFloat4) {
        return ScalarFamily::Float;
    }
    return ScalarFamily::Other;
}
} // namespace

bool execNop(const CBEMInstruction&, BytecodeExecContext&, IssueBag&) noexcept {
    return true;
}
bool execLoadExternal(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                      IssueBag& issues) noexcept {
    const u32 dstReg = ins.operands[0];
    const u16 byteSlot = static_cast<u16>(ins.operands[1]);
    const u16 slot = canonicalExternalSlot(ctx, byteSlot);
    if (slot >= ctx.externals.size()) {
        issues.push(vmFatal(issues::vm::kExternalOob, "IR: LoadExternal slot out of bounds"));
        return false;
    }
    const auto d = decodeRegId(dstReg);
    RegisterValue v = ctx.externals[slot];
    if (v.componentCount == 0) {
        v.componentCount = componentCountForBank(d.bank);
        v.typeBank = d.bank;
    }
    return writeDst(ctx, dstReg, v, issues);
}

bool execStoreToExternal(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                         IssueBag& issues) noexcept {
    const u32 srcReg = ins.operands[0];
    const u16 byteSlot = static_cast<u16>(ins.operands[1]);
    const u16 slot = canonicalExternalSlot(ctx, byteSlot);
    if (slot >= ctx.externals.size()) {
        issues.push(vmFatal(issues::vm::kExternalOob, "IR: StoreToExternal slot out of bounds"));
        return false;
    }
    RegisterValue v;
    if (!readSrc(ctx, srcReg, v, issues)) {
        return false;
    }
    ctx.externals[slot] = v;
    return true;
}
bool execMove(const CBEMInstruction& ins, BytecodeExecContext& ctx, IssueBag& issues) noexcept {
    const u32 dstReg = ins.operands[0];
    const u32 srcReg = ins.operands[1];
    RegisterValue v;
    if (!readSrc(ctx, srcReg, v, issues)) {
        return false;
    }
    const auto d = decodeRegId(dstReg);
    const u8 dstComps = componentCountForBank(d.bank);

    const bool isTypeConvert = (ins.opcode == Opcode::TypeConverter);
    if (!isTypeConvert) {
        v.typeBank = d.bank;
        v.componentCount = dstComps;
        return writeDst(ctx, dstReg, v, issues);
    }

    const ScalarFamily srcFam = scalarFamilyForBank(v.typeBank);
    const ScalarFamily dstFam = scalarFamilyForBank(d.bank);

    RegisterValue out;
    out.typeBank = d.bank;
    out.componentCount = dstComps;
    const u8 lanesToConvert = std::min(dstComps, v.componentCount);
    for (u8 i = 0; i < lanesToConvert; ++i) {
        if (srcFam == ScalarFamily::Float && dstFam == ScalarFamily::Int) {
            setLaneI32(out, i, static_cast<i32>(v.lanes[i]));
        } else if (srcFam == ScalarFamily::Int && dstFam == ScalarFamily::Float) {
            out.lanes[i] = static_cast<f32>(laneAsI32(v, i));
        } else if (srcFam == ScalarFamily::Bool && dstFam == ScalarFamily::Float) {
            out.lanes[i] = (laneAsI32(v, i) != 0) ? 1.0F : 0.0F;
        } else if (srcFam == ScalarFamily::Bool && dstFam == ScalarFamily::Int) {
            setLaneI32(out, i, (laneAsI32(v, i) != 0) ? 1 : 0);
        } else {

            std::memcpy(&out.lanes[i], &v.lanes[i], sizeof(f32));
        }
    }
    return writeDst(ctx, dstReg, out, issues);
}
bool execVecCtor(const CBEMInstruction& ins, BytecodeExecContext& ctx, IssueBag& issues) noexcept {
    const u8 argc = static_cast<u8>(ins.operands[0]);
    const u32 dstReg = ins.operands[1];
    const u32 srcCount = static_cast<u32>(argc) + 1U;
    if (srcCount > 4U || ins.extraOperands.size() != srcCount) {
        issues.push(vmFatal(issues::vm::kOperandCount, "IR: VecCtor source count mismatch"));
        return false;
    }
    const auto d = decodeRegId(dstReg);
    const u8 dstComps = componentCountForBank(d.bank);

    f32 flat[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    u8 flatCount = 0;
    for (u32 i = 0; i < srcCount; ++i) {
        RegisterValue s;
        if (!readSrc(ctx, ins.extraOperands[i], s, issues)) {
            return false;
        }
        const u8 srcComps = (s.componentCount > 0U) ? s.componentCount : 1U;
        for (u8 j = 0; j < srcComps && flatCount < 4U; ++j) {
            flat[flatCount++] = s.lanes[j];
        }
    }

    RegisterValue v;
    v.typeBank = d.bank;
    v.componentCount = dstComps;
    if (flatCount == 1U && dstComps > 1U) {

        for (u8 i = 0; i < dstComps; ++i) {
            v.lanes[i] = flat[0];
        }
    } else {
        for (u8 i = 0; i < dstComps; ++i) {
            v.lanes[i] = (i < flatCount) ? flat[i] : 0.0F;
        }
    }
    return writeDst(ctx, dstReg, v, issues);
}
bool execVecSwizzle(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                    IssueBag& issues) noexcept {
    const u32 maskOperand = ins.operands[0];
    const u32 dstReg = ins.operands[1];
    const u32 srcReg = ins.operands[2];
    RegisterValue s;
    if (!readSrc(ctx, srcReg, s, issues)) {
        return false;
    }
    const u8 b2 = static_cast<u8>((maskOperand >> 8U) & 0xFFU);
    const u8 b3 = static_cast<u8>((maskOperand >> 16U) & 0xFFU);
    const u32 packed = (static_cast<u32>(b3 & 0xF0U) << 4U) | static_cast<u32>(b2);
    const auto d = decodeRegId(dstReg);
    RegisterValue out;
    out.typeBank = d.bank;
    out.componentCount = componentCountForBank(d.bank);

    auto writeLiteralOne = [&](u8 lane) noexcept {
        if (d.bank == bank::kBool) {
            const u32 bits = fpbits::kBoolTrue;
            std::memcpy(&out.lanes[lane], &bits, sizeof(u32));
        } else if (bankIsIntegral(d.bank)) {
            setLaneI32(out, lane, 1);
        } else {
            out.lanes[lane] = 1.0F;
        }
    };

    for (u8 i = 0; i < out.componentCount && i < kSwizzleMaxLanes; ++i) {
        const u8 rawCode =
            static_cast<u8>((packed >> (kSwizzleBitsPerCode * i)) & kSwizzleCodeMask);
        switch (static_cast<SwizzleCode>(rawCode)) {
        case SwizzleCode::LaneX:
        case SwizzleCode::LaneY:
        case SwizzleCode::LaneZ:
        case SwizzleCode::LaneW:
            out.lanes[i] = s.lanes[rawCode];
            break;
        case SwizzleCode::LiteralZero:

            out.lanes[i] = 0.0F;
            break;
        case SwizzleCode::LiteralOne:
            writeLiteralOne(i);
            break;
        case SwizzleCode::Count:
        default:

            issues.push(
                vmFatal(issues::vm::kSwizzleMaskOob, "IR: VecSwizzle component code out of range"));
            return false;
        }
    }
    return writeDst(ctx, dstReg, out, issues);
}
bool applyMathOp(u8 op, const RegisterValue& a, const RegisterValue& b, u8 components,
                 bool integerOp, RegisterValue& out, IssueBag& issues) noexcept {
    out.componentCount = components;
    auto warnUnknownOnce = [&]() {
        static std::set<u32> reportedOps;
        const auto [it, inserted] = reportedOps.insert(op);
        if (inserted) {
            static std::set<std::string> stubMessages;
            std::string msg = "IR: MathOp sub-id not implemented (engine no-op): ";
            msg.append(std::to_string(op));
            const auto [mit, _] = stubMessages.insert(std::move(msg));
            issues.push(vmWarn(issues::vm::kUnknownMathOp, std::string_view{*mit}));
        }
    };
    for (u8 i = 0; i < components; ++i) {
        if (!integerOp) {
            const f32 av = a.lanes[i];
            const f32 bv = b.lanes[i];
            f32 r = 0.0F;
            switch (static_cast<MathOp>(op)) {
            case MathOp::Add:
                r = av + bv;
                break;
            case MathOp::Sub:
                r = av - bv;
                break;
            case MathOp::Mul:
                r = av * bv;
                break;
            case MathOp::Div:
                r = av / bv;
                break;
            case MathOp::Mod:
                r = std::fmod(av, bv);
                break;
            case MathOp::Neg:
                r = -av;
                break;
            case MathOp::Lt:
                setLaneI32(out, i, av < bv ? 1 : 0);
                continue;
            case MathOp::Le:
                setLaneI32(out, i, av <= bv ? 1 : 0);
                continue;
            case MathOp::Gt:
                setLaneI32(out, i, av > bv ? 1 : 0);
                continue;
            case MathOp::Ge:
                setLaneI32(out, i, av >= bv ? 1 : 0);
                continue;
            case MathOp::Eq:
                setLaneI32(out, i, av == bv ? 1 : 0);
                continue;
            case MathOp::Ne:
                setLaneI32(out, i, av != bv ? 1 : 0);
                continue;
            default:
                warnUnknownOnce();
                r = av;
                break;
            }
            out.lanes[i] = r;
        } else {
            const i32 av = laneAsI32(a, i);
            const i32 bv = laneAsI32(b, i);
            i32 r = 0;
            switch (static_cast<MathOp>(op)) {
            case MathOp::Add:
                r = av + bv;
                break;
            case MathOp::Sub:
                r = av - bv;
                break;
            case MathOp::Mul:
                r = av * bv;
                break;
            case MathOp::Div:
                r = (bv == 0) ? 0 : av / bv;
                break;
            case MathOp::Mod:
                r = (bv == 0) ? 0 : av % bv;
                break;
            case MathOp::Neg:
                r = -av;
                break;
            case MathOp::Shl:
                r = av << (bv & 31);
                break;
            case MathOp::Shr:
                r = av >> (bv & 31);
                break;
            case MathOp::BitAnd:
                r = av & bv;
                break;
            case MathOp::BitOr:
                r = av | bv;
                break;
            case MathOp::BitXor:
                r = av ^ bv;
                break;
            case MathOp::BitNot:
                r = ~av;
                break;
            case MathOp::Lt:
                r = av < bv ? 1 : 0;
                break;
            case MathOp::Le:
                r = av <= bv ? 1 : 0;
                break;
            case MathOp::Gt:
                r = av > bv ? 1 : 0;
                break;
            case MathOp::Ge:
                r = av >= bv ? 1 : 0;
                break;
            case MathOp::Eq:
                r = av == bv ? 1 : 0;
                break;
            case MathOp::Ne:
                r = av != bv ? 1 : 0;
                break;
            default:
                warnUnknownOnce();
                r = av;
                break;
            }
            setLaneI32(out, i, r);
        }
    }
    return true;
}
bool execMathOp(const CBEMInstruction& ins, BytecodeExecContext& ctx, IssueBag& issues) noexcept {
    const u8 op = static_cast<u8>(ins.operands[0]);
    const u32 dstReg = ins.operands[1];
    const u32 src0Reg = ins.operands[2];
    const u32 src1Reg = ins.operands[3];
    RegisterValue a;
    RegisterValue b;
    if (!readSrc(ctx, src0Reg, a, issues) || !readSrc(ctx, src1Reg, b, issues)) {
        return false;
    }
    const auto d = decodeRegId(dstReg);
    const u8 components = componentCountForBank(d.bank);
    const bool integerOp = bankIsIntegral(d.bank);
    RegisterValue out;
    out.typeBank = d.bank;
    if (!applyMathOp(op, a, b, components, integerOp, out, issues)) {
        return false;
    }
    return writeDst(ctx, dstReg, out, issues);
}
bool execMathFunc1(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                   IssueBag& issues) noexcept {
    const u8 fn = static_cast<u8>(ins.operands[0]);
    const u32 dstReg = ins.operands[1];
    const u32 srcReg = ins.operands[2];
    RegisterValue a;
    if (!readSrc(ctx, srcReg, a, issues)) {
        return false;
    }
    const auto d = decodeRegId(dstReg);
    RegisterValue out;
    out.typeBank = d.bank;
    out.componentCount = componentCountForBank(d.bank);

    auto vectorLength = [&]() -> f32 {
        f32 s = 0.0F;
        for (u8 i = 0; i < a.componentCount; ++i) {
            s += a.lanes[i] * a.lanes[i];
        }
        return std::sqrt(s);
    };
    const auto fnId = static_cast<MathFunc1>(fn);
    if (fnId == MathFunc1::Length || fnId == MathFunc1::FastLength) {

        out.lanes[0] = vectorLength();
        for (u8 i = 1; i < out.componentCount; ++i) {
            out.lanes[i] = 0.0F;
        }
        return writeDst(ctx, dstReg, out, issues);
    }
    if (fnId == MathFunc1::Normalize || fnId == MathFunc1::FastNormalize) {

        const f32 len = vectorLength();
        const f32 invLen = (len > 1e-12F) ? (1.0F / len) : 0.0F;
        for (u8 i = 0; i < out.componentCount; ++i) {
            out.lanes[i] = (i < a.componentCount) ? a.lanes[i] * invLen : 0.0F;
        }
        return writeDst(ctx, dstReg, out, issues);
    }
    if (fnId == MathFunc1::SinCos || fnId == MathFunc1::FastSinCos) {

        const u8 dstLanes = out.componentCount;
        const u8 srcLanes = a.componentCount;
        for (u8 i = 0; i < dstLanes; ++i) {
            const u8 srcIdx = static_cast<u8>(i / 2U);
            const f32 x = (srcIdx < srcLanes) ? a.lanes[srcIdx] : 0.0F;
            out.lanes[i] = (i & 1U) ? std::cos(x) : std::sin(x);
        }
        return writeDst(ctx, dstReg, out, issues);
    }
    if (fnId == MathFunc1::All) {

        bool allTrue = true;
        for (u8 i = 0; i < a.componentCount; ++i) {
            if (a.lanes[i] == 0.0F) {
                allTrue = false;
                break;
            }
        }
        out.lanes[0] = allTrue ? 1.0F : 0.0F;
        for (u8 i = 1; i < out.componentCount; ++i) {
            out.lanes[i] = 0.0F;
        }
        return writeDst(ctx, dstReg, out, issues);
    }
    if (fnId == MathFunc1::Any) {

        bool anyTrue = false;
        for (u8 i = 0; i < a.componentCount; ++i) {
            if (a.lanes[i] != 0.0F) {
                anyTrue = true;
                break;
            }
        }
        out.lanes[0] = anyTrue ? 1.0F : 0.0F;
        for (u8 i = 1; i < out.componentCount; ++i) {
            out.lanes[i] = 0.0F;
        }
        return writeDst(ctx, dstReg, out, issues);
    }

    for (u8 i = 0; i < out.componentCount; ++i) {
        const f32 x = a.lanes[i];
        f32 r = 0.0F;
        switch (fnId) {
        case MathFunc1::Sqrt:
        case MathFunc1::FastSqrt:
            r = std::sqrt(x);
            break;
        case MathFunc1::RSqrt:
        case MathFunc1::FastRSqrt:
            r = (x > 0.0F) ? 1.0F / std::sqrt(x) : 0.0F;
            break;
        case MathFunc1::Cbrt:
        case MathFunc1::FastCbrt:
            r = std::cbrt(x);
            break;
        case MathFunc1::Sin:
        case MathFunc1::FastSin:
            r = std::sin(x);
            break;
        case MathFunc1::Cos:
        case MathFunc1::FastCos:
            r = std::cos(x);
            break;
        case MathFunc1::Tan:
        case MathFunc1::FastTan:
            r = std::tan(x);
            break;
        case MathFunc1::Asin:
        case MathFunc1::FastAsin:
            r = std::asin(x);
            break;
        case MathFunc1::Acos:
        case MathFunc1::FastAcos:
            r = std::acos(x);
            break;
        case MathFunc1::Atan:
        case MathFunc1::FastAtan:
            r = std::atan(x);
            break;
        case MathFunc1::Exp:
        case MathFunc1::FastExp:
            r = std::exp(x);
            break;
        case MathFunc1::Exp2:
        case MathFunc1::FastExp2:
            r = std::exp2(x);
            break;
        case MathFunc1::Log:
        case MathFunc1::FastLog:
            r = std::log(x);
            break;
        case MathFunc1::Log2:
        case MathFunc1::FastLog2:
            r = std::log2(x);
            break;
        case MathFunc1::Rcp:
        case MathFunc1::FastRcp:
            r = (x != 0.0F) ? 1.0F / x : 0.0F;
            break;
        case MathFunc1::Abs:
            r = std::fabs(x);
            break;
        case MathFunc1::Sign:
            r = (x > 0.0F) - (x < 0.0F);
            break;
        case MathFunc1::Ceil:
            r = std::ceil(x);
            break;
        case MathFunc1::Floor:
            r = std::floor(x);
            break;
        case MathFunc1::FracUnsigned:
            r = x - std::floor(x);
            break;
        case MathFunc1::Frac:
            r = x - std::trunc(x);
            break;
        case MathFunc1::Saturate:
            r = (x < 0.0F) ? 0.0F : (x > 1.0F ? 1.0F : x);
            break;
        case MathFunc1::IsFinite:

            r = std::isfinite(x) ? 1.0F : 0.0F;
            break;
        case MathFunc1::IsInfinite:

            r = std::isinf(x) ? 1.0F : 0.0F;
            break;
        default: {

            static std::set<u32> reportedFns;
            const auto [it, inserted] = reportedFns.insert(fn);
            if (inserted) {
                static std::set<std::string> stubMessages;
                std::string msg = "IR: MathFunc1 sub-id not implemented (engine no-op): ";
                msg.append(std::to_string(fn));
                const auto [mit, _] = stubMessages.insert(std::move(msg));
                issues.push(vmWarn(issues::vm::kUnknownMathFunc1, std::string_view{*mit}));
            }
            r = x;
            break;
        }
        }
        out.lanes[i] = r;
    }
    return writeDst(ctx, dstReg, out, issues);
}
bool execMathFunc2(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                   IssueBag& issues) noexcept {
    const u8 fn = static_cast<u8>(ins.operands[0]);
    const u32 dstReg = ins.operands[1];
    const u32 src0Reg = ins.operands[2];
    const u32 src1Reg = ins.operands[3];
    RegisterValue a;
    RegisterValue b;
    if (!readSrc(ctx, src0Reg, a, issues) || !readSrc(ctx, src1Reg, b, issues)) {
        return false;
    }
    const auto d = decodeRegId(dstReg);
    RegisterValue out;
    out.typeBank = d.bank;
    out.componentCount = componentCountForBank(d.bank);
    const auto fnId = static_cast<MathFunc2>(fn);
    for (u8 i = 0; i < out.componentCount; ++i) {
        const f32 x = a.lanes[i];
        const f32 y = b.lanes[i];
        f32 r = 0.0F;
        switch (fnId) {
        case MathFunc2::Atan2:
        case MathFunc2::FastAtan2:
            r = std::atan2(x, y);
            break;
        case MathFunc2::Step:
            r = (y < x) ? 0.0F : 1.0F;
            break;
        case MathFunc2::Discretize:
            r = (y != 0.0F) ? std::trunc(x / y) * y : 0.0F;
            break;
        case MathFunc2::Min:
            r = (x < y) ? x : y;
            break;
        case MathFunc2::Max:
            r = (x > y) ? x : y;
            break;
        case MathFunc2::Dot: {
            f32 acc = 0.0F;
            for (u8 j = 0; j < a.componentCount && j < b.componentCount; ++j) {
                acc += a.lanes[j] * b.lanes[j];
            }
            out.componentCount = 1;
            out.lanes[0] = acc;
            return writeDst(ctx, dstReg, out, issues);
        }
        case MathFunc2::Cross: {
            out.componentCount = 3;
            out.lanes[0] = a.lanes[1] * b.lanes[2] - a.lanes[2] * b.lanes[1];
            out.lanes[1] = a.lanes[2] * b.lanes[0] - a.lanes[0] * b.lanes[2];
            out.lanes[2] = a.lanes[0] * b.lanes[1] - a.lanes[1] * b.lanes[0];
            return writeDst(ctx, dstReg, out, issues);
        }
        default: {

            static std::set<u32> reportedFns;
            const auto [it, inserted] = reportedFns.insert(fn);
            if (inserted) {
                static std::set<std::string> stubMessages;
                std::string msg = "IR: MathFunc2 sub-id not implemented (engine no-op): ";
                msg.append(std::to_string(fn));
                const auto [mit, _] = stubMessages.insert(std::move(msg));
                issues.push(vmWarn(issues::vm::kUnknownMathFunc2, std::string_view{*mit}));
            }
            r = x;
            break;
        }
        }
        out.lanes[i] = r;
    }
    return writeDst(ctx, dstReg, out, issues);
}
bool execMathFunc3(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                   IssueBag& issues) noexcept {
    const u8 fn = static_cast<u8>(ins.operands[0]);
    const u32 dstReg = ins.operands[1];
    RegisterValue a;
    RegisterValue b;
    RegisterValue c;
    if (!readSrc(ctx, ins.operands[2], a, issues) || !readSrc(ctx, ins.operands[3], b, issues) ||
        !readSrc(ctx, ins.operands[4], c, issues)) {
        return false;
    }
    const auto d = decodeRegId(dstReg);
    RegisterValue out;
    out.typeBank = d.bank;
    out.componentCount = componentCountForBank(d.bank);
    const auto fnId = static_cast<MathFunc3>(fn);
    for (u8 i = 0; i < out.componentCount; ++i) {
        const f32 x = a.lanes[i];
        const f32 y = b.lanes[i];
        const f32 z = c.lanes[i];
        f32 r = 0.0F;
        switch (fnId) {
        case MathFunc3::Lerp:
            r = x + (y - x) * z;
            break;
        case MathFunc3::Clamp:
            r = (x < y) ? y : (x > z ? z : x);
            break;
        case MathFunc3::Within:
            setLaneI32(out, i, (x >= y && x <= z) ? 1 : 0);
            continue;
        case MathFunc3::Count:
        default:
            issues.push(
                vmFatal(issues::vm::kUnknownMathFunc3, "IR: MathFunc3 sub-id not implemented"));
            return false;
        }
        out.lanes[i] = r;
    }
    return writeDst(ctx, dstReg, out, issues);
}
bool execSelect(const CBEMInstruction& ins, BytecodeExecContext& ctx, IssueBag& issues) noexcept {
    const u32 dstReg = ins.operands[0];
    RegisterValue cond;
    RegisterValue tv;
    RegisterValue fv;
    if (!readSrc(ctx, ins.operands[1], cond, issues) ||
        !readSrc(ctx, ins.operands[2], tv, issues) || !readSrc(ctx, ins.operands[3], fv, issues)) {
        return false;
    }
    const auto d = decodeRegId(dstReg);
    RegisterValue out;
    out.typeBank = d.bank;
    out.componentCount = componentCountForBank(d.bank);
    for (u8 i = 0; i < out.componentCount; ++i) {
        const u8 cl = (i < cond.componentCount) ? i : 0;
        const bool truthy = (laneAsI32(cond, cl) != 0) || (cond.lanes[cl] != 0.0F);
        out.lanes[i] = truthy ? tv.lanes[i] : fv.lanes[i];
    }
    return writeDst(ctx, dstReg, out, issues);
}
} // namespace whiteout::cornflakes
