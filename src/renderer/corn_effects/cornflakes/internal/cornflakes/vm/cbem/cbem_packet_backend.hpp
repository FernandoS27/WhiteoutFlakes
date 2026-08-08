#pragma once

#include "cbem_backend.hpp"
#include "cbem_vector_arith.hpp"

#include <cornflakes/interface/simt/packet_register_file.hpp>
#include <cornflakes/interface/simt/scratch_validator.hpp>
#include <cornflakes/interface/vm/bytecode_exec_context.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <array>
#include <cstddef>

namespace whiteout::cornflakes::cbem {

template <std::size_t N>
struct PacketState {
    simt::PacketRegisterFile<N> file;

    const simt::PacketRegisterLayout* layout = nullptr;

    std::array<BytecodeExecContext*, N> contexts{};

    std::size_t constantsPoolBytes = 0U;

    void beginScope(const simt::PacketRegisterLayout& programLayout, std::size_t poolBytes) {
        layout = &programLayout;
        constantsPoolBytes = poolBytes;
        file.reset(programLayout);
    }
};

template <std::size_t N>
struct PacketBackend {
    static constexpr std::size_t kLanes = N;

    using Value = RegPack<N>;
    using Mask = PacketMask;
    using State = PacketState<N>;

    static BytecodeExecContext& lane(State& s, std::size_t laneIndex) noexcept {
        return *s.contexts[laneIndex];
    }
    static bool live(Mask m, std::size_t laneIndex) noexcept {
        return m.live(laneIndex);
    }

    static bool readSrc(State& s, std::size_t laneIndex, u32 regId, RegisterValue& out,
                        IssueBag& issues) noexcept {
        if (regId == kRegVoid) {
            out = RegisterValue{};
            out.componentCount = 1;
            out.typeBank = bank::kFloat;
            return true;
        }
        const auto d = decodeRegId(regId);
        if (simt::constPoolHit(d, s.constantsPoolBytes)) {
            return readConst(*s.contexts[laneIndex], d.localIdx, d.bank, out, issues);
        }
        const u32 slot = s.layout->slotFor(d.scope, d.localIdx);
        if (slot == simt::PacketRegisterLayout::kInvalidSlot) {
            issues.push(vmFatal(issues::vm::kRegisterOob, "VM: register index out of bounds"));
            return false;
        }
        out = s.file.loadLane(slot, laneIndex);
        if (out.componentCount == 0) {
            out.componentCount = componentCountForBank(d.bank);
            out.typeBank = d.bank;
        }
        return true;
    }

    static bool writeDst(State& s, std::size_t laneIndex, u32 regId, const RegisterValue& v,
                         IssueBag& issues) noexcept {
        const auto d = decodeRegId(regId);
        if (simt::constPoolHit(d, s.constantsPoolBytes)) {
            issues.push(
                vmFatal(issues::vm::kRegisterOob, "VM: write to constant-pool or input register"));
            return false;
        }
        const u32 slot = s.layout->slotFor(d.scope, d.localIdx);
        if (slot == simt::PacketRegisterLayout::kInvalidSlot) {
            issues.push(vmFatal(issues::vm::kRegisterOob, "VM: register index out of bounds"));
            return false;
        }
        RegisterValue stored = v;
        if (stored.componentCount == 0) {
            stored.componentCount = componentCountForBank(d.bank);
        }
        if (stored.typeBank == 0) {
            stored.typeBank = d.bank;
        }
        s.file.storeLane(slot, laneIndex, stored);
        return true;
    }

    static void buildKeep(Mask m, u32 (&keep)[N]) noexcept {
        for (std::size_t l = 0; l < N; ++l) {
            keep[l] = m.live(l) ? 0xFFFFFFFFU : 0U;
        }
    }

    static bool prepareSource(State& s, u32 regId, PlaneSource<N>& out,
                              IssueBag& issues) noexcept {
        if (regId == kRegVoid) {
            for (u8 c = 0; c < 4U; ++c) {
                out.setUniform(c, 0U);
            }
            out.components = 1U;
            return true;
        }
        const auto d = decodeRegId(regId);
        if (simt::constPoolHit(d, s.constantsPoolBytes)) {
            RegisterValue v;
            if (!readConst(*firstContext(s), d.localIdx, d.bank, v, issues)) {
                return false;
            }
            for (u8 c = 0; c < 4U; ++c) {
                out.setUniform(c, f2u(v.lanes[c]));
            }
            out.components = v.componentCount;
            return true;
        }
        const u32 slot = s.layout->slotFor(d.scope, d.localIdx);
        if (slot == simt::PacketRegisterLayout::kInvalidSlot) {
            return false;
        }
        if (s.file.componentCount(slot) == 0U) {
            return false;
        }
        for (u8 c = 0; c < 4U; ++c) {
            out.plane[c] = s.file.plane(slot, c).data();
        }
        out.components = s.file.componentCount(slot);
        return true;
    }

    static u32* destPlane(State& s, u32 regId, u8 component, u32& slotOut) noexcept {
        const auto d = decodeRegId(regId);
        if (regId == kRegVoid || simt::constPoolHit(d, s.constantsPoolBytes)) {
            return nullptr;
        }
        const u32 slot = s.layout->slotFor(d.scope, d.localIdx);
        if (slot == simt::PacketRegisterLayout::kInvalidSlot) {
            return nullptr;
        }
        slotOut = slot;
        return s.file.plane(slot, component).data();
    }

    static void stampTag(State& s, u32 slot, u8 components, u8 bankByte) noexcept {
        u8 cc = components;
        u8 tb = bankByte;
        if (cc == 0U) {
            cc = componentCountForBank(bankByte);
        }
        if (tb == 0U) {
            tb = bankByte;
        }
        s.file.setTag(slot, cc, tb);
    }

    static BytecodeExecContext* firstContext(State& s) noexcept {
        for (std::size_t l = 0; l < N; ++l) {
            if (s.contexts[l] != nullptr) {
                return s.contexts[l];
            }
        }
        return nullptr;
    }

    static bool vectorOpFor(u8 subOp, bool integerOp, VectorOp& out) noexcept {
        const auto op = static_cast<MathOp>(subOp);
        if (!integerOp) {
            switch (op) {
            case MathOp::Add: out = VectorOp::FAdd; return true;
            case MathOp::Sub: out = VectorOp::FSub; return true;
            case MathOp::Mul: out = VectorOp::FMul; return true;
            case MathOp::Div: out = VectorOp::FDiv; return true;
            case MathOp::Neg: out = VectorOp::FNeg; return true;
            case MathOp::Lt:  out = VectorOp::FLt;  return true;
            case MathOp::Le:  out = VectorOp::FLe;  return true;
            case MathOp::Gt:  out = VectorOp::FGt;  return true;
            case MathOp::Ge:  out = VectorOp::FGe;  return true;
            case MathOp::Eq:  out = VectorOp::FEq;  return true;
            case MathOp::Ne:  out = VectorOp::FNe;  return true;
            default: return false;
            }
        }
        switch (op) {
        case MathOp::Add:    out = VectorOp::IAdd; return true;
        case MathOp::Sub:    out = VectorOp::ISub; return true;
        case MathOp::Mul:    out = VectorOp::IMul; return true;
        case MathOp::Div:    out = VectorOp::IDiv; return true;
        case MathOp::Neg:    out = VectorOp::INeg; return true;
        case MathOp::Shl:    out = VectorOp::IShl; return true;
        case MathOp::Shr:    out = VectorOp::IShr; return true;
        case MathOp::BitAnd: out = VectorOp::IAnd; return true;
        case MathOp::BitOr:  out = VectorOp::IOr;  return true;
        case MathOp::BitXor: out = VectorOp::IXor; return true;
        case MathOp::BitNot: out = VectorOp::INot; return true;
        case MathOp::Lt:     out = VectorOp::ILt;  return true;
        case MathOp::Le:     out = VectorOp::ILe;  return true;
        case MathOp::Gt:     out = VectorOp::IGt;  return true;
        case MathOp::Ge:     out = VectorOp::IGe;  return true;
        case MathOp::Eq:     out = VectorOp::IEq;  return true;
        case MathOp::Ne:     out = VectorOp::INe;  return true;
        default: return false;
        }
    }

    static bool vectorBinary(State& s, Mask m, u32 dstReg, u32 src0Reg, u32 src1Reg, u8 subOp,
                             u8 components, bool integerOp, IssueBag& issues) noexcept {
        VectorOp op{};
        if (m.bits == 0U || !vectorOpFor(subOp, integerOp, op)) {
            return false;
        }
        const auto d = decodeRegId(dstReg);
        u32 dstSlot = 0U;
        if (destPlane(s, dstReg, 0U, dstSlot) == nullptr) {
            return false;
        }
        PlaneSource<N> a;
        PlaneSource<N> b;
        if (!prepareSource(s, src0Reg, a, issues) || !prepareSource(s, src1Reg, b, issues)) {
            return false;
        }

        alignas(64) u32 keep[N];
        buildKeep(m, keep);
        for (u8 c = 0; c < 4U; ++c) {
            u32* dst = s.file.plane(dstSlot, c).data();
            if (c < components) {
                applyPlane<N>(op, a.plane[c], b.plane[c], keep, dst);
            } else {
                fillPlane<N>(0U, keep, dst);
            }
        }
        stampTag(s, dstSlot, components, d.bank);
        return true;
    }

    static bool vectorFunc1For(u8 fn, VectorFunc1& out) noexcept {
        switch (static_cast<MathFunc1>(fn)) {
        case MathFunc1::Sqrt:
        case MathFunc1::FastSqrt:     out = VectorFunc1::Sqrt; return true;
        case MathFunc1::RSqrt:
        case MathFunc1::FastRSqrt:    out = VectorFunc1::RSqrt; return true;
        case MathFunc1::Rcp:
        case MathFunc1::FastRcp:      out = VectorFunc1::Rcp; return true;
        case MathFunc1::Exp:
        case MathFunc1::FastExp:      out = VectorFunc1::Exp; return true;
        case MathFunc1::Exp2:
        case MathFunc1::FastExp2:     out = VectorFunc1::Exp2; return true;
        case MathFunc1::Abs:          out = VectorFunc1::Abs; return true;
        case MathFunc1::Sign:         out = VectorFunc1::Sign; return true;
        case MathFunc1::Ceil:         out = VectorFunc1::Ceil; return true;
        case MathFunc1::Floor:        out = VectorFunc1::Floor; return true;
        case MathFunc1::FracUnsigned: out = VectorFunc1::FracUnsigned; return true;
        case MathFunc1::Frac:         out = VectorFunc1::Frac; return true;
        case MathFunc1::Saturate:     out = VectorFunc1::Saturate; return true;
        case MathFunc1::IsFinite:     out = VectorFunc1::IsFinite; return true;
        case MathFunc1::IsInfinite:   out = VectorFunc1::IsInfinite; return true;
        default: return false;
        }
    }

    static bool vectorMathFunc1(State& s, Mask m, u32 dstReg, u32 srcReg, u8 fn, u8 components,
                                IssueBag& issues) noexcept {
        VectorFunc1 op{};
        if (m.bits == 0U || !vectorFunc1For(fn, op)) {
            return false;
        }
        const auto d = decodeRegId(dstReg);
        u32 dstSlot = 0U;
        if (destPlane(s, dstReg, 0U, dstSlot) == nullptr) {
            return false;
        }
        PlaneSource<N> a;
        if (!prepareSource(s, srcReg, a, issues)) {
            return false;
        }
        alignas(64) u32 keep[N];
        buildKeep(m, keep);
        for (u8 c = 0; c < 4U; ++c) {
            u32* dst = s.file.plane(dstSlot, c).data();
            if (c < components) {
                applyFunc1Plane<N>(op, a.plane[c], keep, dst);
            } else {
                fillPlane<N>(0U, keep, dst);
            }
        }
        stampTag(s, dstSlot, components, d.bank);
        return true;
    }

    static bool vectorMadd(State& s, Mask m, u32 dstReg, u32 src0Reg, u32 src1Reg, u32 src2Reg,
                           u8 components, IssueBag& issues) noexcept {
        if (m.bits == 0U) {
            return false;
        }
        const auto d = decodeRegId(dstReg);
        u32 dstSlot = 0U;
        if (destPlane(s, dstReg, 0U, dstSlot) == nullptr) {
            return false;
        }
        PlaneSource<N> a;
        PlaneSource<N> b;
        PlaneSource<N> c2;
        if (!prepareSource(s, src0Reg, a, issues) || !prepareSource(s, src1Reg, b, issues) ||
            !prepareSource(s, src2Reg, c2, issues)) {
            return false;
        }
        alignas(64) u32 keep[N];
        buildKeep(m, keep);
        for (u8 c = 0; c < 4U; ++c) {
            u32* dst = s.file.plane(dstSlot, c).data();
            if (c < components) {
                applyMaddPlane<N>(a.plane[c], b.plane[c], c2.plane[c], keep, dst);
            } else {
                fillPlane<N>(0U, keep, dst);
            }
        }
        stampTag(s, dstSlot, components, d.bank);
        return true;
    }

    static bool vectorSelect(State& s, Mask m, u32 dstReg, u32 falseReg, u32 trueReg, u32 condReg,
                             u8 components, IssueBag& issues) noexcept {
        if (m.bits == 0U) {
            return false;
        }
        const auto d = decodeRegId(dstReg);
        u32 dstSlot = 0U;
        if (destPlane(s, dstReg, 0U, dstSlot) == nullptr) {
            return false;
        }
        PlaneSource<N> fv;
        PlaneSource<N> tv;
        PlaneSource<N> cond;
        if (!prepareSource(s, falseReg, fv, issues) || !prepareSource(s, trueReg, tv, issues) ||
            !prepareSource(s, condReg, cond, issues)) {
            return false;
        }
        if (cond.components < components) {
            return false;
        }

        alignas(64) u32 keep[N];
        buildKeep(m, keep);
        for (u8 c = 0; c < 4U; ++c) {
            u32* dst = s.file.plane(dstSlot, c).data();
            if (c < components) {
                applySelectPlane<N>(fv.plane[c], tv.plane[c], cond.plane[c], keep, dst);
            } else {
                fillPlane<N>(0U, keep, dst);
            }
        }
        stampTag(s, dstSlot, components, d.bank);
        return true;
    }

    static bool vectorBroadcast(State& s, Mask m, u32 dstReg, u32 srcReg, u8 components,
                                IssueBag& issues) noexcept {
        if (m.bits == 0U) {
            return false;
        }
        const auto d = decodeRegId(dstReg);
        u32 dstSlot = 0U;
        if (destPlane(s, dstReg, 0U, dstSlot) == nullptr) {
            return false;
        }
        PlaneSource<N> src;
        if (!prepareSource(s, srcReg, src, issues)) {
            return false;
        }
        alignas(64) u32 scalarPlane[N];
        for (std::size_t l = 0; l < N; ++l) {
            scalarPlane[l] = src.plane[0][l];
        }
        alignas(64) u32 keep[N];
        buildKeep(m, keep);
        for (u8 c = 0; c < 4U; ++c) {
            u32* dst = s.file.plane(dstSlot, c).data();
            if (c < components) {
                copyPlane<N>(scalarPlane, keep, dst);
            } else {
                fillPlane<N>(0U, keep, dst);
            }
        }
        stampTag(s, dstSlot, components, d.bank);
        return true;
    }

    static bool vectorReinterpret(State& s, Mask m, u32 dstReg, u32 srcReg, u8 components,
                                  IssueBag& issues) noexcept {
        if (m.bits == 0U) {
            return false;
        }
        const auto d = decodeRegId(dstReg);
        u32 dstSlot = 0U;
        if (destPlane(s, dstReg, 0U, dstSlot) == nullptr) {
            return false;
        }
        PlaneSource<N> src;
        if (!prepareSource(s, srcReg, src, issues)) {
            return false;
        }
        alignas(64) u32 keep[N];
        buildKeep(m, keep);
        for (u8 c = 0; c < 4U; ++c) {
            copyPlane<N>(src.plane[c], keep, s.file.plane(dstSlot, c).data());
        }
        stampTag(s, dstSlot, components, d.bank);
        return true;
    }

    static bool vectorCurveSample(State& s, Mask m, const CBEMInstruction& ins,
                                  IssueBag& issues) noexcept {
        if (m.bits == 0U) {
            return false;
        }
        const u32 retReg = ins.operands[4];
        if (retReg == kRegVoid) {
            return false;
        }
        auto* ctx = firstContext(s);
        if (ctx == nullptr) {
            return false;
        }
        const u32 extFunc = ins.operands[2];
        if (extFunc >= ctx->functions.size()) {
            return false;
        }
        const auto& fn = ctx->functions[extFunc];
        const std::string_view canon =
            !fn.canonicalName.empty() ? fn.canonicalName : canonicalizeSymbol(fn.symbolName);
        if (canon != "sample") {
            return false;
        }
        CurveSampleTarget target;
        if (!resolveCurveSample(ins, *ctx, target)) {
            return false;
        }
        if (ins.extraOperands.size() < 2U) {
            return false;
        }
        const u32 argReg = ins.extraOperands[1];

        const auto d = decodeRegId(retReg);
        u8 components = target.components;
        if (components == 0U) {
            components = componentCountForBank(d.bank);
        }

        for (std::size_t lane = 0; lane < N; ++lane) {
            if (!m.live(lane)) {
                continue;
            }
            RegisterValue arg;
            if (!readSrc(s, lane, argReg, arg, issues)) {
                return false;
            }
            RegisterValue out;
            out.componentCount = components;
            out.typeBank = d.bank;
            if (evalSamplerCurveVec(*target.curve, arg.lanes[0], out.lanes, 4) == 0U) {
                return false;
            }
            if (!writeDst(s, lane, retReg, out, issues)) {
                return false;
            }
        }
        return true;
    }

    static void beforeCall(State& s, std::size_t laneIndex, const CBEMInstruction& ins) noexcept {
        bridge(s, laneIndex, ins, true);
    }

    static void afterCall(State& s, std::size_t laneIndex, const CBEMInstruction& ins) noexcept {
#if defined(CORNFLAKES_ABLATE_AFTERCALL)
        (void)s;
        (void)laneIndex;
        (void)ins;
        return;
#else
        bridge(s, laneIndex, ins, false);
#endif
    }

private:
    static void bridge(State& s, std::size_t laneIndex, const CBEMInstruction& ins,
                       bool intoContext) noexcept {
        auto& ctx = *s.contexts[laneIndex];
        const auto move = [&](u32 regId) {
            if (regId == kRegVoid) {
                return;
            }
            const auto d = decodeRegId(regId);
            if (simt::constPoolHit(d, s.constantsPoolBytes) ||
                d.scope >= kScopeRegisterBuckets) {
                return;
            }
            const u32 slot = s.layout->slotFor(d.scope, d.localIdx);
            if (slot == simt::PacketRegisterLayout::kInvalidSlot) {
                return;
            }
            auto bankSpan = ctx.scopeRegisters[d.scope];
            if (d.localIdx >= bankSpan.size()) {
                return;
            }
            if (intoContext) {
                bankSpan[d.localIdx] = s.file.loadLane(slot, laneIndex);
            } else {
                s.file.storeLane(slot, laneIndex, bankSpan[d.localIdx]);
            }
        };
        if (intoContext) {
            simt::forEachRegisterOperand(ins, move, [](u32) {});
        } else {
            simt::forEachRegisterOperand(ins, [](u32) {}, move);
        }
    }
};

template <std::size_t N>
inline PacketMask liveFor(const PacketState<N>& s, simt::LaneMask requested) noexcept {
    simt::LaneMask m = 0U;
    for (std::size_t lane = 0; lane < N; ++lane) {
        if (((requested >> lane) & 1U) != 0U && s.contexts[lane] != nullptr) {
            m |= (static_cast<simt::LaneMask>(1U) << lane);
        }
    }
    return PacketMask{m};
}

}
