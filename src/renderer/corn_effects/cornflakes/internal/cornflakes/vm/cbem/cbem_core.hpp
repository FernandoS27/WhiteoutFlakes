#pragma once

#include "cbem_backend.hpp"
#include "cbem_engine_math.hpp"
#include "cbem_internal.hpp"

#include <cornflakes/diagnostics/issue_codes.hpp>
#include <cornflakes/interface/binding/ir_to_cbem_lowerer.hpp>
#include <cornflakes/interface/schema/opcodes.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <cmath>
#include <cstring>
#include <set>
#include <string>

#if defined(_MSC_VER) || defined(__clang__)
#define CF_CBEM_OP __declspec(noinline)
#else
#define CF_CBEM_OP
#endif

namespace whiteout::cornflakes::cbem {

enum class ScalarFamily : u8 { Float, Int, Bool, Other };

constexpr ScalarFamily scalarFamilyForBank(u8 b) noexcept {
    if (b == bank::kBool || b == bank::kBool3) {
        return ScalarFamily::Bool;
    }
    if (bankIsIntegral(b)) {
        return ScalarFamily::Int;
    }
    if (b == bank::kFloat || b == bank::kFloat2 || b == bank::kFloat3 || b == bank::kFloat4 ||
        b == bank::kIntAlt) {
        return ScalarFamily::Float;
    }
    return ScalarFamily::Other;
}

template <CbemBackend B, class Body>
inline bool forEachLane(typename B::State& s, typename B::Mask mask, Body&& body) {
    (void)s;
    for (std::size_t lane = 0; lane < B::kLanes; ++lane) {
        if (!B::live(mask, lane)) {
            continue;
        }
        if (!body(lane)) {
            return false;
        }
    }
    return true;
}

template <CbemBackend B>
inline bool execNop(const CBEMInstruction&, typename B::State&, typename B::Mask, IssueBag&) {
    return true;
}

template <CbemBackend B>
CF_CBEM_OP inline bool execLoadExternal(const CBEMInstruction& ins, typename B::State& s,
                             typename B::Mask mask, IssueBag& issues) {
    const u32 dstReg = ins.operands[0];
    const u16 byteSlot = static_cast<u16>(ins.operands[1]);
    const auto d = decodeRegId(dstReg);

    return forEachLane<B>(s, mask, [&](std::size_t lane) {
        auto& ctx = B::lane(s, lane);
        const u16 slot = canonicalExternalSlot(ctx, byteSlot);
        if (slot >= ctx.externals.size()) {
            issues.push(vmFatal(issues::vm::kExternalOob, "IR: LoadExternal slot out of bounds"));
            return false;
        }
        RegisterValue v = ctx.externals[slot];
        if (v.componentCount == 0) {
            v.componentCount = componentCountForBank(d.bank);
            v.typeBank = d.bank;
        }
        if (d.bank == bank::kHandle) {
            bool replaced = false;
            for (u8 i = 0; i < ctx.handleRegisterCount; ++i) {
                if (ctx.handleRegisterSlots[i].reg == dstReg) {
                    ctx.handleRegisterSlots[i].slot = byteSlot;
                    replaced = true;
                    break;
                }
            }
            if (!replaced && ctx.handleRegisterCount < ctx.handleRegisterSlots.size()) {
                ctx.handleRegisterSlots[ctx.handleRegisterCount] = {dstReg, byteSlot};
                ++ctx.handleRegisterCount;
            }
        }
        return B::writeDst(s, lane, dstReg, v, issues);
    });
}

template <CbemBackend B>
CF_CBEM_OP inline bool execStoreToExternal(const CBEMInstruction& ins, typename B::State& s,
                                typename B::Mask mask, IssueBag& issues) {
    const u32 srcReg = ins.operands[0];
    const u16 byteSlot = static_cast<u16>(ins.operands[1]);

    return forEachLane<B>(s, mask, [&](std::size_t lane) {
        auto& ctx = B::lane(s, lane);
        const u16 slot = canonicalExternalSlot(ctx, byteSlot);
        if (slot >= ctx.externals.size()) {
            issues.push(
                vmFatal(issues::vm::kExternalOob, "IR: StoreToExternal slot out of bounds"));
            return false;
        }
        RegisterValue v;
        if (!B::readSrc(s, lane, srcReg, v, issues)) {
            return false;
        }
        ctx.externals.set(slot, v);
        return true;
    });
}

template <CbemBackend B>
CF_CBEM_OP inline bool execExternalClear(const CBEMInstruction& ins, typename B::State& s,
                              typename B::Mask mask, IssueBag& issues) {
    if (ins.operandCount != 1) {
        issues.push(vmFatal(issues::vm::kOperandCount, "CBEM: ExternalClear operand count"));
        return false;
    }
    const u16 byteSlot = static_cast<u16>(ins.operands[0]);

    return forEachLane<B>(s, mask, [&](std::size_t lane) {
        auto& ctx = B::lane(s, lane);
        const u16 slot = canonicalExternalSlot(ctx, byteSlot);
        if (slot >= ctx.externals.size()) {
            issues.push(vmFatal(issues::vm::kExternalOob, "CBEM: external slot out of bounds"));
            return false;
        }
        ctx.externals.set(slot, RegisterValue::scalar(0.0F));
        return true;
    });
}

template <CbemBackend B>
CF_CBEM_OP inline bool execMove(const CBEMInstruction& ins, typename B::State& s, typename B::Mask mask,
                     IssueBag& issues) {
    const u32 dstReg = ins.operands[0];
    const u32 srcReg = ins.operands[1];
    const auto d = decodeRegId(dstReg);
    const u8 dstComps = componentCountForBank(d.bank);
    const bool isTypeConvert = (ins.opcode == Opcode::TypeConverter);
    const ScalarFamily dstFam = scalarFamilyForBank(d.bank);

    if constexpr (requires { B::vectorReinterpret(s, mask, dstReg, srcReg, dstComps, issues); }) {
        if (!isTypeConvert && B::vectorReinterpret(s, mask, dstReg, srcReg, dstComps, issues)) {
            return true;
        }
    }

    return forEachLane<B>(s, mask, [&](std::size_t lane) {
        RegisterValue v;
        if (!B::readSrc(s, lane, srcReg, v, issues)) {
            return false;
        }
        if (!isTypeConvert) {
            v.typeBank = d.bank;
            v.componentCount = dstComps;
            return B::writeDst(s, lane, dstReg, v, issues);
        }

        const ScalarFamily srcFam = scalarFamilyForBank(v.typeBank);

        RegisterValue out;
        out.typeBank = d.bank;
        out.componentCount = dstComps;
        const u8 lanesToConvert = (dstComps < v.componentCount) ? dstComps : v.componentCount;
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
        return B::writeDst(s, lane, dstReg, out, issues);
    });
}

template <CbemBackend B>
CF_CBEM_OP inline bool execVecCtor(const CBEMInstruction& ins, typename B::State& s, typename B::Mask mask,
                        IssueBag& issues) {
    const u8 argc = static_cast<u8>(ins.operands[0]);
    const u32 dstReg = ins.operands[1];
    const u32 srcCount = static_cast<u32>(argc) + 1U;
    if (srcCount > 4U || ins.extraOperands.size() != srcCount) {
        issues.push(vmFatal(issues::vm::kOperandCount, "IR: VecCtor source count mismatch"));
        return false;
    }
    const auto d = decodeRegId(dstReg);
    const u8 dstComps = componentCountForBank(d.bank);

    return forEachLane<B>(s, mask, [&](std::size_t lane) {
        f32 flat[4] = {0.0F, 0.0F, 0.0F, 0.0F};
        u8 flatCount = 0;
        for (u32 i = 0; i < srcCount; ++i) {
            RegisterValue src;
            if (!B::readSrc(s, lane, ins.extraOperands[i], src, issues)) {
                return false;
            }
            const u8 srcComps = (src.componentCount > 0U) ? src.componentCount : 1U;
            for (u8 j = 0; j < srcComps && flatCount < 4U; ++j) {
                flat[flatCount++] = src.lanes[j];
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
        return B::writeDst(s, lane, dstReg, v, issues);
    });
}

template <CbemBackend B>
CF_CBEM_OP inline bool execVecSwizzle(const CBEMInstruction& ins, typename B::State& s, typename B::Mask mask,
                           IssueBag& issues) {
    const u32 maskOperand = ins.operands[0];
    const u32 dstReg = ins.operands[1];
    const u32 srcReg = ins.operands[2];
    const u8 b2 = static_cast<u8>((maskOperand >> 8U) & 0xFFU);
    const u8 b3 = static_cast<u8>((maskOperand >> 16U) & 0xFFU);
    const u32 packed = (static_cast<u32>(b3 & 0xF0U) << 4U) | static_cast<u32>(b2);
    const auto d = decodeRegId(dstReg);
    const u8 dstComps = componentCountForBank(d.bank);

    return forEachLane<B>(s, mask, [&](std::size_t lane) {
        RegisterValue src;
        if (!B::readSrc(s, lane, srcReg, src, issues)) {
            return false;
        }
        RegisterValue out;
        out.typeBank = d.bank;
        out.componentCount = dstComps;

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
                out.lanes[i] = src.lanes[rawCode];
                break;
            case SwizzleCode::LiteralZero:
                out.lanes[i] = 0.0F;
                break;
            case SwizzleCode::LiteralOne:
                writeLiteralOne(i);
                break;
            case SwizzleCode::Count:
            default:
                issues.push(vmFatal(issues::vm::kSwizzleMaskOob,
                                    "IR: VecSwizzle component code out of range"));
                return false;
            }
        }
        return B::writeDst(s, lane, dstReg, out, issues);
    });
}

template <CbemBackend B>
CF_CBEM_OP inline bool execMathOp(const CBEMInstruction& ins, typename B::State& s, typename B::Mask mask,
                       IssueBag& issues) {
    const u8 op = static_cast<u8>(ins.operands[0]);
    const u32 dstReg = ins.operands[1];
    const u32 src0Reg = ins.operands[2];
    const u32 src1Reg = ins.operands[3];
    const auto d = decodeRegId(dstReg);
    const u8 components = componentCountForBank(d.bank);
    const bool integerOp = bankIsIntegral(d.bank);

    if constexpr (requires { B::vectorBinary(s, mask, dstReg, src0Reg, src1Reg, op, components,
                                             integerOp, issues); }) {
        if (B::vectorBinary(s, mask, dstReg, src0Reg, src1Reg, op, components, integerOp,
                            issues)) {
            return true;
        }
    }

    return forEachLane<B>(s, mask, [&](std::size_t lane) {
        RegisterValue a;
        RegisterValue b;
        if (!B::readSrc(s, lane, src0Reg, a, issues) || !B::readSrc(s, lane, src1Reg, b, issues)) {
            return false;
        }
        RegisterValue out;
        out.typeBank = d.bank;
        if (!applyMathOp(op, a, b, components, integerOp, out, issues)) {
            return false;
        }
        return B::writeDst(s, lane, dstReg, out, issues);
    });
}

template <CbemBackend B>
CF_CBEM_OP inline bool execBinaryArith(const CBEMInstruction& ins, typename B::State& s, typename B::Mask mask,
                            IssueBag& issues, MathOp op) {
    if (ins.operandCount != 3) {
        issues.push(vmFatal(issues::vm::kOperandCount, "CBEM: MathOp* operand count"));
        return false;
    }
    const u32 dstReg = ins.operands[0];
    const u32 src0Reg = ins.operands[1];
    const u32 src1Reg = ins.operands[2];
    const auto d = decodeRegId(dstReg);
    const u8 components = componentCountForBank(d.bank);
    const bool integerOp = bankIsIntegral(d.bank);
    const u8 subOp = static_cast<u8>(op);

    if constexpr (requires { B::vectorBinary(s, mask, dstReg, src0Reg, src1Reg, subOp, components,
                                             integerOp, issues); }) {
        if (B::vectorBinary(s, mask, dstReg, src0Reg, src1Reg, subOp, components, integerOp,
                            issues)) {
            return true;
        }
    }

    return forEachLane<B>(s, mask, [&](std::size_t lane) {
        RegisterValue a;
        RegisterValue b;
        if (!B::readSrc(s, lane, src0Reg, a, issues) || !B::readSrc(s, lane, src1Reg, b, issues)) {
            return false;
        }
        RegisterValue out;
        out.typeBank = d.bank;
        if (!applyMathOp(subOp, a, b, components, integerOp, out, issues)) {
            return false;
        }
        return B::writeDst(s, lane, dstReg, out, issues);
    });
}

template <CbemBackend B>
CF_CBEM_OP inline bool execMathFunc1(const CBEMInstruction& ins, typename B::State& s, typename B::Mask mask,
                          IssueBag& issues) {
    const u8 fn = static_cast<u8>(ins.operands[0]);
    const u32 dstReg = ins.operands[1];
    const u32 srcReg = ins.operands[2];
    const auto d = decodeRegId(dstReg);
    const u8 dstComps = componentCountForBank(d.bank);
    const auto fnId = static_cast<MathFunc1>(fn);

    if constexpr (requires { B::vectorMathFunc1(s, mask, dstReg, srcReg, fn, dstComps, issues); }) {
        if (B::vectorMathFunc1(s, mask, dstReg, srcReg, fn, dstComps, issues)) {
            return true;
        }
    }

    return forEachLane<B>(s, mask, [&](std::size_t lane) {
        RegisterValue a;
        if (!B::readSrc(s, lane, srcReg, a, issues)) {
            return false;
        }
        RegisterValue out;
        out.typeBank = d.bank;
        out.componentCount = dstComps;

        auto vectorLength = [&]() -> f32 {
            f32 acc = 0.0F;
            for (u8 i = 0; i < a.componentCount; ++i) {
                acc += a.lanes[i] * a.lanes[i];
            }
            return std::sqrt(acc);
        };

        if (fnId == MathFunc1::Length || fnId == MathFunc1::FastLength) {
            out.lanes[0] = vectorLength();
            for (u8 i = 1; i < out.componentCount; ++i) {
                out.lanes[i] = 0.0F;
            }
            return B::writeDst(s, lane, dstReg, out, issues);
        }
        if (fnId == MathFunc1::Normalize || fnId == MathFunc1::FastNormalize) {
            const f32 len = vectorLength();
            const f32 invLen = (len > 1e-12F) ? (1.0F / len) : 0.0F;
            for (u8 i = 0; i < out.componentCount; ++i) {
                out.lanes[i] = (i < a.componentCount) ? a.lanes[i] * invLen : 0.0F;
            }
            return B::writeDst(s, lane, dstReg, out, issues);
        }
        if (fnId == MathFunc1::SinCos || fnId == MathFunc1::FastSinCos) {
            const u8 dstLanes = out.componentCount;
            const u8 srcLanes = a.componentCount;
            for (u8 i = 0; i < dstLanes; ++i) {
                const u8 srcIdx = static_cast<u8>(i / 2U);
                const f32 x = (srcIdx < srcLanes) ? a.lanes[srcIdx] : 0.0F;
                f32 sinV = 0.0F;
                f32 cosV = 0.0F;
                enginemath::sinCos(x, sinV, cosV);
                out.lanes[i] = (i & 1U) ? cosV : sinV;
            }
            return B::writeDst(s, lane, dstReg, out, issues);
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
            return B::writeDst(s, lane, dstReg, out, issues);
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
            return B::writeDst(s, lane, dstReg, out, issues);
        }

        for (u8 i = 0; i < out.componentCount; ++i) {
            const f32 x = a.lanes[i];
            f32 r = 0.0F;
            switch (fnId) {
            case MathFunc1::Sqrt:
            case MathFunc1::FastSqrt:
                r = enginemath::sqrt(x);
                break;
            case MathFunc1::RSqrt:
            case MathFunc1::FastRSqrt:
                r = enginemath::rsqrt(x);
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
                r = enginemath::exp(x);
                break;
            case MathFunc1::Exp2:
            case MathFunc1::FastExp2:
                r = enginemath::exp2(x);
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
                r = enginemath::rcp(x);
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
                    const auto [mit, unused] = stubMessages.insert(std::move(msg));
                    issues.push(vmWarn(issues::vm::kUnknownMathFunc1, std::string_view{*mit}));
                }
                r = x;
                break;
            }
            }
            out.lanes[i] = r;
        }
        return B::writeDst(s, lane, dstReg, out, issues);
    });
}

template <CbemBackend B>
CF_CBEM_OP inline bool execMathFunc2(const CBEMInstruction& ins, typename B::State& s, typename B::Mask mask,
                          IssueBag& issues) {
    const u8 fn = static_cast<u8>(ins.operands[0]);
    const u32 dstReg = ins.operands[1];
    const u32 src0Reg = ins.operands[2];
    const u32 src1Reg = ins.operands[3];
    const auto d = decodeRegId(dstReg);
    const u8 dstComps = componentCountForBank(d.bank);
    const auto fnId = static_cast<MathFunc2>(fn);

    return forEachLane<B>(s, mask, [&](std::size_t lane) {
        RegisterValue a;
        RegisterValue b;
        if (!B::readSrc(s, lane, src0Reg, a, issues) || !B::readSrc(s, lane, src1Reg, b, issues)) {
            return false;
        }
        RegisterValue out;
        out.typeBank = d.bank;
        out.componentCount = dstComps;
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
                return B::writeDst(s, lane, dstReg, out, issues);
            }
            case MathFunc2::Cross: {
                out.componentCount = 3;
                out.lanes[0] = a.lanes[1] * b.lanes[2] - a.lanes[2] * b.lanes[1];
                out.lanes[1] = a.lanes[2] * b.lanes[0] - a.lanes[0] * b.lanes[2];
                out.lanes[2] = a.lanes[0] * b.lanes[1] - a.lanes[1] * b.lanes[0];
                return B::writeDst(s, lane, dstReg, out, issues);
            }
            default: {
                static std::set<u32> reportedFns;
                const auto [it, inserted] = reportedFns.insert(fn);
                if (inserted) {
                    static std::set<std::string> stubMessages;
                    std::string msg = "IR: MathFunc2 sub-id not implemented (engine no-op): ";
                    msg.append(std::to_string(fn));
                    const auto [mit, unused] = stubMessages.insert(std::move(msg));
                    issues.push(vmWarn(issues::vm::kUnknownMathFunc2, std::string_view{*mit}));
                }
                r = x;
                break;
            }
            }
            out.lanes[i] = r;
        }
        return B::writeDst(s, lane, dstReg, out, issues);
    });
}

template <CbemBackend B>
CF_CBEM_OP inline bool execMathFunc3(const CBEMInstruction& ins, typename B::State& s, typename B::Mask mask,
                          IssueBag& issues) {
    const u8 fn = static_cast<u8>(ins.operands[0]);
    const u32 dstReg = ins.operands[1];
    const auto d = decodeRegId(dstReg);
    const u8 dstComps = componentCountForBank(d.bank);
    const auto fnId = static_cast<MathFunc3>(fn);

    return forEachLane<B>(s, mask, [&](std::size_t lane) {
        RegisterValue a;
        RegisterValue b;
        RegisterValue c;
        if (!B::readSrc(s, lane, ins.operands[2], a, issues) ||
            !B::readSrc(s, lane, ins.operands[3], b, issues) ||
            !B::readSrc(s, lane, ins.operands[4], c, issues)) {
            return false;
        }
        RegisterValue out;
        out.typeBank = d.bank;
        out.componentCount = dstComps;
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
        return B::writeDst(s, lane, dstReg, out, issues);
    });
}

template <CbemBackend B>
CF_CBEM_OP inline bool execSelect(const CBEMInstruction& ins, typename B::State& s, typename B::Mask mask,
                       IssueBag& issues) {
    const u32 dstReg = ins.operands[0];
    const auto d = decodeRegId(dstReg);
    const u8 dstComps = componentCountForBank(d.bank);

    if constexpr (requires { B::vectorSelect(s, mask, dstReg, ins.operands[1], ins.operands[2],
                                             ins.operands[3], dstComps, issues); }) {
        if (B::vectorSelect(s, mask, dstReg, ins.operands[1], ins.operands[2], ins.operands[3],
                            dstComps, issues)) {
            return true;
        }
    }

    return forEachLane<B>(s, mask, [&](std::size_t lane) {
        RegisterValue cond;
        RegisterValue tv;
        RegisterValue fv;
        if (!B::readSrc(s, lane, ins.operands[3], cond, issues) ||
            !B::readSrc(s, lane, ins.operands[2], tv, issues) ||
            !B::readSrc(s, lane, ins.operands[1], fv, issues)) {
            return false;
        }
        RegisterValue out;
        out.typeBank = d.bank;
        out.componentCount = dstComps;
        for (u8 i = 0; i < out.componentCount; ++i) {
            const u8 cl = (i < cond.componentCount) ? i : 0;
            const bool truthy = (laneAsI32(cond, cl) != 0) || (cond.lanes[cl] != 0.0F);
            out.lanes[i] = truthy ? tv.lanes[i] : fv.lanes[i];
        }
        return B::writeDst(s, lane, dstReg, out, issues);
    });
}

template <CbemBackend B>
CF_CBEM_OP inline bool execBroadcast(const CBEMInstruction& ins, typename B::State& s, typename B::Mask mask,
                          IssueBag& issues) {
    if (ins.operandCount != 2) {
        issues.push(vmFatal(issues::vm::kOperandCount, "CBEM: Broadcast operand count"));
        return false;
    }
    const u32 dstReg = ins.operands[0];
    const u32 srcReg = ins.operands[1];
    const auto d = decodeRegId(dstReg);
    const u8 dstComps = componentCountForBank(d.bank);

    if constexpr (requires { B::vectorBroadcast(s, mask, dstReg, srcReg, dstComps, issues); }) {
        if (B::vectorBroadcast(s, mask, dstReg, srcReg, dstComps, issues)) {
            return true;
        }
    }

    return forEachLane<B>(s, mask, [&](std::size_t lane) {
        RegisterValue src;
        if (!B::readSrc(s, lane, srcReg, src, issues)) {
            return false;
        }
        RegisterValue v;
        v.typeBank = d.bank;
        v.componentCount = dstComps;
        const f32 scalar = src.lanes[0];
        for (u8 i = 0; i < dstComps; ++i) {
            v.lanes[i] = scalar;
        }
        return B::writeDst(s, lane, dstReg, v, issues);
    });
}

template <CbemBackend B>
CF_CBEM_OP inline bool execMadd(const CBEMInstruction& ins, typename B::State& s, typename B::Mask mask,
                     IssueBag& issues) {
    if (ins.operandCount != CBEMEncoder::kMaddOperandCount) {
        issues.push(vmFatal(issues::vm::kOperandCount, "CBEM: Madd operand count"));
        return false;
    }
    const u32 dstReg = ins.operands[CBEMEncoder::kMaddDstIndex];
    const auto d = decodeRegId(dstReg);
    const u8 components = componentCountForBank(d.bank);

    if constexpr (requires { B::vectorMadd(s, mask, dstReg, ins.operands[1], ins.operands[2],
                                           ins.operands[3], components, issues); }) {
        if (B::vectorMadd(s, mask, dstReg, ins.operands[CBEMEncoder::kMaddSrc0Index],
                          ins.operands[CBEMEncoder::kMaddSrc1Index],
                          ins.operands[CBEMEncoder::kMaddSrc2Index], components, issues)) {
            return true;
        }
    }

    return forEachLane<B>(s, mask, [&](std::size_t lane) {
        RegisterValue a;
        RegisterValue b;
        RegisterValue c;
        if (!B::readSrc(s, lane, ins.operands[CBEMEncoder::kMaddSrc0Index], a, issues) ||
            !B::readSrc(s, lane, ins.operands[CBEMEncoder::kMaddSrc1Index], b, issues) ||
            !B::readSrc(s, lane, ins.operands[CBEMEncoder::kMaddSrc2Index], c, issues)) {
            return false;
        }
        RegisterValue out;
        out.typeBank = d.bank;
        out.componentCount = components;
        for (u8 i = 0; i < components; ++i) {
            out.lanes[i] = a.lanes[i] * b.lanes[i] + c.lanes[i];
        }
        return B::writeDst(s, lane, dstReg, out, issues);
    });
}

template <CbemBackend B>
CF_CBEM_OP inline bool execIDivMulInv(const CBEMInstruction& ins, typename B::State& s, typename B::Mask mask,
                           IssueBag& issues) {
    if (ins.operandCount != 5) {
        issues.push(vmFatal(issues::vm::kOperandCount, "CBEM: IDivMulInv operand count"));
        return false;
    }
    const u32 dstReg = ins.operands[0];
    const auto d = decodeRegId(dstReg);
    const u8 components = componentCountForBank(d.bank);

    return forEachLane<B>(s, mask, [&](std::size_t lane) {
        RegisterValue src0;
        RegisterValue mInvSignReg;
        RegisterValue magicReg;
        RegisterValue shiftReg;
        if (!B::readSrc(s, lane, ins.operands[1], src0, issues) ||
            !B::readSrc(s, lane, ins.operands[2], mInvSignReg, issues) ||
            !B::readSrc(s, lane, ins.operands[3], magicReg, issues) ||
            !B::readSrc(s, lane, ins.operands[4], shiftReg, issues)) {
            return false;
        }
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
        return B::writeDst(s, lane, dstReg, out, issues);
    });
}

template <CbemBackend B>
inline bool execFunctionProlog(const CBEMInstruction&, typename B::State& s, typename B::Mask mask,
                               IssueBag&) {
    return forEachLane<B>(s, mask, [&](std::size_t lane) {
        ++B::lane(s, lane).functionDepth;
        return true;
    });
}

template <CbemBackend B>
inline bool execFunctionEpilog(const CBEMInstruction&, typename B::State& s, typename B::Mask mask,
                               IssueBag& issues) {
    return forEachLane<B>(s, mask, [&](std::size_t lane) {
        auto& ctx = B::lane(s, lane);
        if (ctx.functionDepth == 0) {
            issues.push(vmFatal(issues::vm::kUnmatchedEpilog,
                                "CBEM: FunctionEpilog without matching Prolog"));
            return false;
        }
        --ctx.functionDepth;
        return true;
    });
}

template <CbemBackend B>
CF_CBEM_OP inline bool execFunctionCall(const CBEMInstruction& ins, typename B::State& s,
                             typename B::Mask mask, IssueBag& issues) {
#if !defined(CORNFLAKES_NO_PLANAR_SAMPLE)
    if constexpr (requires { B::vectorCurveSample(s, mask, ins, issues); }) {
        if (B::vectorCurveSample(s, mask, ins, issues)) {
            return true;
        }
    }
#endif
    return forEachLane<B>(s, mask, [&](std::size_t lane) {
        B::beforeCall(s, lane, ins);
        const bool ok = whiteout::cornflakes::execFunctionCall(ins, B::lane(s, lane), issues);
        B::afterCall(s, lane, ins);
        return ok;
    });
}

template <CbemBackend B>
inline bool step(const CBEMInstruction& ins, typename B::State& s, typename B::Mask mask,
                 IssueBag& issues) {
    switch (ins.opcode) {
    case Opcode::Nop:             return execNop<B>(ins, s, mask, issues);
    case Opcode::LoadExternal:    return execLoadExternal<B>(ins, s, mask, issues);
    case Opcode::StoreToExternal: return execStoreToExternal<B>(ins, s, mask, issues);
    case Opcode::Reinterpret:
    case Opcode::TypeConverter:   return execMove<B>(ins, s, mask, issues);
    case Opcode::VecCtor:         return execVecCtor<B>(ins, s, mask, issues);
    case Opcode::VecSwizzle:      return execVecSwizzle<B>(ins, s, mask, issues);
    case Opcode::MathOp:
    case Opcode::MathOpCMeta:     return execMathOp<B>(ins, s, mask, issues);
    case Opcode::MathFunc1:       return execMathFunc1<B>(ins, s, mask, issues);
    case Opcode::MathFunc2:       return execMathFunc2<B>(ins, s, mask, issues);
    case Opcode::MathFunc3:       return execMathFunc3<B>(ins, s, mask, issues);
    case Opcode::Select:          return execSelect<B>(ins, s, mask, issues);
    case Opcode::FunctionCall:    return execFunctionCall<B>(ins, s, mask, issues);
    case Opcode::ExternalClear:   return execExternalClear<B>(ins, s, mask, issues);
    case Opcode::Broadcast:       return execBroadcast<B>(ins, s, mask, issues);
    case Opcode::MathOpAdd:       return execBinaryArith<B>(ins, s, mask, issues, MathOp::Add);
    case Opcode::MathOpSub:       return execBinaryArith<B>(ins, s, mask, issues, MathOp::Sub);
    case Opcode::MathOpMul:       return execBinaryArith<B>(ins, s, mask, issues, MathOp::Mul);
    case Opcode::MathOpDiv:       return execBinaryArith<B>(ins, s, mask, issues, MathOp::Div);
    case Opcode::IDivMulInv:      return execIDivMulInv<B>(ins, s, mask, issues);
    case Opcode::Madd:            return execMadd<B>(ins, s, mask, issues);
    case Opcode::FunctionProlog:  return execFunctionProlog<B>(ins, s, mask, issues);
    case Opcode::FunctionEpilog:  return execFunctionEpilog<B>(ins, s, mask, issues);
    }
    issues.push(vmFatal(issues::vm::kUnknownOpcode, "VM: opcode not in IR or CBEM range"));
    return false;
}

}
