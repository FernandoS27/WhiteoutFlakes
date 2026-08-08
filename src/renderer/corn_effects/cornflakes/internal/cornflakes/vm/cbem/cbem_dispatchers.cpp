#include "cbem_internal.hpp"

#include <cornflakes/core/determinism.hpp>
#include <cornflakes/diagnostics/issue_codes.hpp>
#include <cornflakes/sampler/mesh.hpp>
#include <cornflakes/sampler/shape_geometry.hpp>
#include <cornflakes/sampler/texture.hpp>
#include <cornflakes/sampler/turbulence.hpp>
#include <cornflakes/sampler/vector_field.hpp>
#include <cornflakes/vm/cbem_interpreter.hpp>
#include <cornflakes/vm/math_functions.hpp>
#include <cornflakes/interface/binding/ir_to_cbem_lowerer.hpp>
#include <cornflakes/interface/schema/opcodes.hpp>
#include <cornflakes/interface/vm/bytecode_exec_context.hpp>
#include <cornflakes/interface/vm/bytecode_trace.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace whiteout::cornflakes {
namespace {
std::optional<u32> resolveKickEventIdFromObjSlot(const CBEMInstruction& ins,
                                                 const BytecodeExecContext& ctx) noexcept {
    const u32 extFunc = ins.operands[2];
    if (extFunc >= ctx.functions.size()) {
        return std::nullopt;
    }
    u32 objSlot = ctx.functions[extFunc].symbolSlot;
    if (objSlot == kSymbolSlotUnbound) {
        objSlot = ins.operands[1];
    }
    if (objSlot == kSymbolSlotUnbound) {
        return std::nullopt;
    }

    for (const auto& b : ctx.externalBindings) {
        if (b.slot == static_cast<u16>(objSlot)) {
            const u16 canonical =
                (b.canonicalSlot == 0U && b.slot != 0U) ? b.slot : b.canonicalSlot;
            if (canonical >= ctx.externals.size()) {
                return std::nullopt;
            }
            return laneAsU32(ctx.externals[canonical], 0);
        }
    }
    return std::nullopt;
}

std::string_view resolveEventChannelName(const CBEMInstruction& ins,
                                         const BytecodeExecContext& ctx) noexcept {
    const u32 extFunc = ins.operands[2];
    if (extFunc >= ctx.functions.size()) {
        return {};
    }
    const u32 symSlot = ctx.functions[extFunc].symbolSlot;
    if (symSlot == kSymbolSlotUnbound) {
        return {};
    }
    for (const auto& b : ctx.externalBindings) {
        if (b.slot == static_cast<u16>(symSlot)) {
            return b.name;
        }
    }
    return {};
}
bool readFnArg(const CBEMInstruction& ins, std::size_t i, BytecodeExecContext& ctx,
               RegisterValue& out, IssueBag& issues) noexcept {
    const std::size_t argRegIndex = (i * 2U) + 1U;
    if (argRegIndex >= ins.extraOperands.size()) {
        issues.push(vmFatal(issues::vm::kOperandCount, "IR: FunctionCall arg index out of range"));
        return false;
    }
    return readSrc(ctx, ins.extraOperands[argRegIndex], out, issues);
}

bool isHandleArg(u32 reg) noexcept {
    return reg != kRegVoid && decodeRegId(reg).bank == bank::kHandle;
}

u32 valueArgCount(const CBEMInstruction& ins) noexcept {
    u32 count = 0U;
    for (u32 i = 0; i < ins.operands[3]; ++i) {
        const std::size_t idx = (static_cast<std::size_t>(i) * 2U) + 1U;
        if (idx < ins.extraOperands.size() && !isHandleArg(ins.extraOperands[idx])) {
            ++count;
        }
    }
    return count;
}

bool readValueArg(const CBEMInstruction& ins, u32 n, BytecodeExecContext& ctx, RegisterValue& out,
                  IssueBag& issues) noexcept {
    u32 seen = 0U;
    for (u32 i = 0; i < ins.operands[3]; ++i) {
        const std::size_t idx = (static_cast<std::size_t>(i) * 2U) + 1U;
        if (idx >= ins.extraOperands.size() || isHandleArg(ins.extraOperands[idx])) {
            continue;
        }
        if (seen == n) {
            return readSrc(ctx, ins.extraOperands[idx], out, issues);
        }
        ++seen;
    }
    return false;
}

bool dispatchRand(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                  IssueBag& issues) noexcept {

    if (ins.operands[3] < 2U) {
        issues.push(vmFatal(issues::vm::kOperandCount, "IR: rand requires at least 2 args"));
        return false;
    }
    RegisterValue lo;
    RegisterValue hi;
    if (!readFnArg(ins, 0, ctx, lo, issues) || !readFnArg(ins, 1, ctx, hi, issues)) {
        return false;
    }

    const u32 retReg = ins.operands[4];
    u8 outComponents = 1;
    if (retReg != kRegVoid) {
        const auto d = decodeRegId(retReg);
        outComponents = componentCountForBank(d.bank);
        if (outComponents == 0U)
            outComponents = 1U;
    } else {
        outComponents = std::max<u8>(lo.componentCount, hi.componentCount);
        if (outComponents == 0U)
            outComponents = 1U;
    }
    out = RegisterValue{};
    out.componentCount = outComponents;
    out.typeBank = floatBankForComponentCount(outComponents);
    auto drawUnit12 = [&]() -> f32 {
        if (ctx.rng == nullptr)
            return 1.0F;
        const u32 raw = ctx.rng->advance();
        const u32 bits = (raw >> fpbits::kRandMantissaShift) | fpbits::kOneF32;
        f32 v;
        std::memcpy(&v, &bits, sizeof(f32));
        return v;
    };
    for (u8 i = 0; i < outComponents; ++i) {
        const f32 t12 = drawUnit12();
        const f32 a = lo.lanes[i];
        const f32 b = hi.lanes[i];
        const f32 d = b - a;
        out.lanes[i] = t12 * d + (a - d);
    }
    return true;
}

bool dispatchVrand(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                   IssueBag& issues) noexcept {
    auto drawUnit = [&]() -> f32 {
        if (ctx.rng == nullptr) {
            return 0.0F;
        }
        const u32 raw = ctx.rng->advance();

        const u32 bits = (raw >> fpbits::kRandMantissaShift) | fpbits::kOneF32;
        f32 v;
        std::memcpy(&v, &bits, sizeof(f32));
        return v - 1.0F;
    };

    const f32 u_phi = drawUnit();
    const f32 u_cos = drawUnit();
    constexpr f32 kTwoPi = 6.28318530717958647692F;
    const f32 phi = u_phi * kTwoPi;
    const f32 cosTheta = 1.0F - 2.0F * u_cos;
    const f32 sinTheta = std::sqrt(std::max(0.0F, 1.0F - cosTheta * cosTheta));

    f32 r = 1.0F;
    if (ins.operands[3] >= 2U) {

        RegisterValue arg0;
        RegisterValue arg1;
        if (!readFnArg(ins, 0, ctx, arg0, issues) || !readFnArg(ins, 1, ctx, arg1, issues)) {
            return false;
        }
        const f32 a = std::max(0.0F, arg0.lanes[0]);
        const f32 b = std::max(0.0F, arg1.lanes[0]);
        const f32 rmax = std::max(a, b);
        const f32 rmin = std::min(a, b);
        const f32 u_r = drawUnit();
        const f32 r3min = rmin * rmin * rmin;
        const f32 r3max = rmax * rmax * rmax;
        r = std::cbrt(r3min + (r3max - r3min) * u_r);
    }

    out = RegisterValue{};
    out.componentCount = 3;
    out.typeBank = bank::kFloat3;

    out.lanes[0] = sinTheta * std::cos(phi) * r;
    out.lanes[1] = cosTheta * r;
    out.lanes[2] = sinTheta * std::sin(phi) * r;
    return true;
}

bool dispatchEffectAge(const CBEMInstruction&, BytecodeExecContext& ctx, RegisterValue& out,
                       IssueBag&) noexcept {

    const f32 age = ctx.effectAge - ctx.timeWindowEnd;
    out = RegisterValue::scalar(age > 0.0F ? age : 0.0F);
    return true;
}

bool dispatchHsv2Rgb(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                     IssueBag& issues) noexcept {
    if (ins.operands[3] < 1U) {
        issues.push(vmFatal(issues::vm::kOperandCount, "IR: hsv2rgb requires 1 vec3 arg"));
        return false;
    }
    RegisterValue arg;
    if (!readFnArg(ins, 0, ctx, arg, issues)) {
        return false;
    }
    const f32 h = arg.lanes[0];

    const f32 s = std::min(1.0F, std::max(0.0F, arg.lanes[1]));
    const f32 v = std::max(0.0F, arg.lanes[2]);

    auto frac = [](f32 x) { return x - std::floor(x); };
    auto clamp01 = [](f32 x) { return std::min(1.0F, std::max(0.0F, x)); };

    const f32 px = std::fabs(frac(h + 1.0F) * 6.0F - 3.0F);
    const f32 py = std::fabs(frac(h + 2.0F / 3.0F) * 6.0F - 3.0F);
    const f32 pz = std::fabs(frac(h + 1.0F / 3.0F) * 6.0F - 3.0F);

    const f32 rx = clamp01(px - 1.0F);
    const f32 ry = clamp01(py - 1.0F);
    const f32 rz = clamp01(pz - 1.0F);

    out = RegisterValue{};
    out.componentCount = 4;
    out.typeBank = bank::kFloat4;
    out.lanes[3] = (arg.componentCount >= 4) ? arg.lanes[3] : 1.0F;
    out.lanes[0] = v * (1.0F + (rx - 1.0F) * s);
    out.lanes[1] = v * (1.0F + (ry - 1.0F) * s);
    out.lanes[2] = v * (1.0F + (rz - 1.0F) * s);
    return true;
}

bool dispatchRgb2Hsv(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                     IssueBag& issues) noexcept {
    if (ins.operands[3] < 1U) {
        issues.push(vmFatal(issues::vm::kOperandCount, "IR: rgb2hsv requires 1 vec3 arg"));
        return false;
    }
    RegisterValue arg;
    if (!readFnArg(ins, 0, ctx, arg, issues)) {
        return false;
    }
    const f32 r = arg.lanes[0];
    const f32 g = arg.lanes[1];
    const f32 b = arg.lanes[2];

    constexpr f32 kGreyEpsilon = 1.0e-6F;

    const f32 vMax = std::max({r, g, b});
    const f32 vMin = std::min({r, g, b});
    const f32 d = vMax - vMin;
    f32 h = 0.0F;
    f32 s = 0.0F;
    const f32 v = vMax;
    if (d > kGreyEpsilon) {
        const f32 invD = 1.0F / d;
        if (vMax == r) {
            h = (g - b) * invD;
            if (h < 0.0F)
                h += 6.0F;
        } else if (vMax == g) {
            h = 2.0F + (b - r) * invD;
        } else {
            h = 4.0F + (r - g) * invD;
        }
        h /= 6.0F;
        s = d / vMax;
    }

    out = RegisterValue{};
    out.componentCount = 4;
    out.typeBank = bank::kFloat4;
    out.lanes[0] = h;
    out.lanes[1] = s;
    out.lanes[2] = v;
    out.lanes[3] = (arg.componentCount >= 4) ? arg.lanes[3] : 1.0F;
    return true;
}

bool dispatchEffectIsRunning(const CBEMInstruction&, BytecodeExecContext& ctx, RegisterValue& out,
                             IssueBag&) noexcept {

    out = RegisterValue::scalarI(ctx.effectIsRunning ? -1 : 0);
    return true;
}

bool dispatchEffectIsRenderingEnabled(const CBEMInstruction&, BytecodeExecContext&,
                                      RegisterValue& out, IssueBag&) noexcept {
    out = RegisterValue::scalarI(-1);
    return true;
}

bool dispatchEffectIsTeleporting(const CBEMInstruction&, BytecodeExecContext&, RegisterValue& out,
                                 IssueBag&) noexcept {
    out = RegisterValue::scalarI(0);
    return true;
}

bool dispatchSimLod(const CBEMInstruction&, BytecodeExecContext& ctx, RegisterValue& out,
                    IssueBag&) noexcept {
    const f32 v = ctx.simLod + ctx.simLodBias;
    out = RegisterValue::scalar(std::min(1.0F, std::max(0.0F, v)));
    return true;
}

bool dispatchSimLodBias(const CBEMInstruction&, BytecodeExecContext& ctx, RegisterValue& out,
                        IssueBag&) noexcept {
    out = RegisterValue::scalar(ctx.simLodBias);
    return true;
}

bool dispatchSimLodDistanceMin(const CBEMInstruction&, BytecodeExecContext& ctx, RegisterValue& out,
                               IssueBag&) noexcept {
    out = RegisterValue::scalar(ctx.simLodDistanceMin);
    return true;
}

bool dispatchSimLodDistanceMax(const CBEMInstruction&, BytecodeExecContext& ctx, RegisterValue& out,
                               IssueBag&) noexcept {
    out = RegisterValue::scalar(ctx.simLodDistanceMax);
    return true;
}

bool dispatchDuration(const CBEMInstruction&, BytecodeExecContext&, RegisterValue& out,
                      IssueBag&) noexcept {

    out = RegisterValue::scalar(1.0e6F);
    return true;
}

bool dispatchSelfKill(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                      IssueBag& issues) noexcept {

    if (ins.operands[3] >= 1U) {
        RegisterValue cond;
        if (!readFnArg(ins, 0, ctx, cond, issues)) {
            return false;
        }
        const bool truthy = (laneAsI32(cond, 0) != 0) || (cond.lanes[0] != 0.0F);
        if (truthy) {
            ctx.selfKillRequested = true;
        }
    } else {
        ctx.selfKillRequested = true;
    }
    out = RegisterValue::scalarI(0);
    return true;
}

const SamplerResource* resolveTargetSampler(const CBEMInstruction& ins,
                                            const BytecodeExecContext& ctx) noexcept;

inline u32 eventCountBetweenTimestamps(const SamplerEventStream& stream, f32 t0, f32 t1) noexcept {
    u32 count = 0;
    for (f32 t : stream.times) {
        if (t >= t0 && t < t1) {
            ++count;
        }
    }
    return count;
}

bool dispatchEventStreamGenerate(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                                 RegisterValue& out, IssueBag& issues,
                                 const SamplerEventStream& stream) noexcept {
    RegisterValue accum;
    RegisterValue prevAgeReg;
    RegisterValue currentAgeReg;
    if (!readFnArg(ins, 0, ctx, accum, issues) || !readFnArg(ins, 1, ctx, prevAgeReg, issues) ||
        !readFnArg(ins, 2, ctx, currentAgeReg, issues)) {
        return false;
    }
    const f32 prevAge = prevAgeReg.lanes[0];
    const f32 currentAge = currentAgeReg.lanes[0];

    const u32 count = eventCountBetweenTimestamps(stream, prevAge, currentAge);

    const u32 key = ctx.simUnitScratchCounter;
    ctx.simUnitScratchCounter = key + 1U;

    const i32 priorTotal = laneAsI32(accum, 1);
    const i32 newTotal = priorTotal + static_cast<i32>(count);
    out = RegisterValue{};
    out.componentCount = 3;
    out.typeBank = bank::kInt3;
    setLaneI32(out, 0, 0);
    setLaneI32(out, 1, newTotal);
    setLaneI32(out, 2, static_cast<i32>(key));
    ctx.lastGenerateCount = count;
    ctx.lastGenerateValid = true;

    const u32 cappedCount =
        std::min<u32>(count, static_cast<u32>(BytecodeExecContext::kMaxPendingPositions));
    for (u32 i = 0; i < cappedCount; ++i) {
        ctx.lastGenerateTs[i] = 1.0F;

        ctx.lastGenerateLerpedTimes[i] = ctx.timeWindowEnd;
    }

    if (auto* entry = allocEventCacheEntry(ctx, key)) {
        entry->count = count;
        entry->countDup = count;
        entry->currentElementIdx = 0;
        entry->forwardFlag = 0;
        for (u32 i = 0; i < cappedCount; ++i) {
            entry->particleIndices[i] = i;
            entry->tFractions[i] = ctx.lastGenerateTs[i];
            entry->lerpedTimes[i] = ctx.lastGenerateLerpedTimes[i];
        }
    }
    return true;
}

bool dispatchGenerate(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                      IssueBag& issues) noexcept {
    if (ins.operands[3] < 5U) {
        issues.push(vmFatal(issues::vm::kOperandCount, "IR: generate requires 5+ args"));
        return false;
    }

    if (const SamplerResource* res = resolveTargetSampler(ins, ctx);
        res != nullptr && res->kind == SamplerKind::EventStream) {
        return dispatchEventStreamGenerate(ins, ctx, out, issues, res->eventStream);
    }
    RegisterValue accum;
    RegisterValue offsetsReg;
    RegisterValue intervalsReg;
    if (!readFnArg(ins, 0, ctx, accum, issues) || !readFnArg(ins, 1, ctx, offsetsReg, issues) ||
        !readFnArg(ins, 2, ctx, intervalsReg, issues)) {
        return false;
    }

    const f32 offsets = offsetsReg.lanes[0];
    const f32 intervals = intervalsReg.lanes[0];
    const f32 carry = accum.lanes[0];
    const i32 priorTotal = laneAsI32(accum, 1);
    const f32 advance = (intervals > 1e-12F) ? (offsets / intervals) : 0.0F;
    const f32 totalF = carry + advance;
    const i32 count = (totalF > 0.0F) ? static_cast<i32>(totalF) : 0;
    const f32 newCarry = totalF - static_cast<f32>(count);
    const i32 newTotal = priorTotal + count;

    const u32 key = ctx.simUnitScratchCounter;
    ctx.simUnitScratchCounter = key + 1U;
    out = RegisterValue{};
    out.componentCount = 3;
    out.typeBank = bank::kInt3;
    out.lanes[0] = newCarry;
    setLaneI32(out, 1, newTotal);
    setLaneI32(out, 2, static_cast<i32>(key));

    ctx.lastGenerateCount = static_cast<u32>(count);
    ctx.lastGenerateValid = true;

    const u32 cappedCount = std::min<u32>(
        static_cast<u32>(count), static_cast<u32>(BytecodeExecContext::kMaxPendingPositions));
    if (advance > 0.0F) {
        const f32 step = 1.0F / advance;
        for (u32 i = 0; i < cappedCount; ++i) {
            const f32 raw = step * (static_cast<f32>(i + 1U) - carry);
            const f32 clamped = (raw > 1.0F) ? 1.0F : (raw < 0.0F ? 0.0F : raw);
            ctx.lastGenerateTs[i] = clamped;
            ctx.lastGenerateLerpedTimes[i] =
                ctx.timeWindowStart + (ctx.timeWindowEnd - ctx.timeWindowStart) * clamped;
        }
    } else {
        for (u32 i = 0; i < cappedCount; ++i) {
            ctx.lastGenerateTs[i] = 1.0F;
            ctx.lastGenerateLerpedTimes[i] = ctx.timeWindowEnd;
        }
    }

    if (auto* entry = allocEventCacheEntry(ctx, key)) {
        entry->count = static_cast<u32>(count);
        entry->countDup = static_cast<u32>(count);
        entry->currentElementIdx = 0;
        entry->forwardFlag = 0;
        for (u32 i = 0; i < cappedCount; ++i) {
            entry->particleIndices[i] = i;
            entry->tFractions[i] = ctx.lastGenerateTs[i];
            entry->lerpedTimes[i] = ctx.lastGenerateLerpedTimes[i];
        }
    }
    return true;
}

bool dispatchTrigger(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                     IssueBag& issues) noexcept {
    if (ins.operands[3] < 2U) {
        issues.push(vmFatal(issues::vm::kOperandCount,
                            "IR: trigger requires 2+ args (condition, fraction)"));
        return false;
    }
    RegisterValue conditionReg;
    RegisterValue fractionReg;
    if (!readFnArg(ins, 0, ctx, conditionReg, issues) ||
        !readFnArg(ins, 1, ctx, fractionReg, issues)) {
        return false;
    }
    const i32 cond = laneAsI32(conditionReg, 0);
    const u32 isTriggered = (cond != 0) ? 1U : 0U;

    const u32 key = ctx.simUnitScratchCounter;
    ctx.simUnitScratchCounter = key + 1U;

    out = RegisterValue{};
    out.componentCount = 3;
    out.typeBank = bank::kInt3;
    setLaneI32(out, 0, 0);
    setLaneI32(out, 1, static_cast<i32>(isTriggered));
    setLaneI32(out, 2, static_cast<i32>(key));

    ctx.lastGenerateCount = isTriggered;
    ctx.lastGenerateValid = true;
    if (isTriggered != 0U) {
        const f32 frac = fractionReg.lanes[0];
        const f32 clamped = (frac > 1.0F) ? 1.0F : (frac < 0.0F ? 0.0F : frac);
        ctx.lastGenerateTs[0] = clamped;

        ctx.lastGenerateLerpedTimes[0] =
            ctx.timeWindowStart + (ctx.timeWindowEnd - ctx.timeWindowStart) * clamped;
    }

    if (auto* entry = allocEventCacheEntry(ctx, key)) {
        entry->count = isTriggered;
        entry->countDup = isTriggered;
        entry->currentElementIdx = 0;
        entry->forwardFlag = 0;
        if (isTriggered != 0U) {
            entry->particleIndices[0] = 0;
            entry->tFractions[0] = ctx.lastGenerateTs[0];
            entry->lerpedTimes[0] = ctx.lastGenerateLerpedTimes[0];
        }
    }
    return true;
}

BytecodeExecContext::PendingPayloadElement* findOrCreatePendingPayload(BytecodeExecContext& ctx,
                                                                       u32 eventId) noexcept {
    for (auto& s : ctx.pendingPayloadElements) {
        if (s.valid && s.eventId == eventId) {
            return &s;
        }
    }
    for (auto& s : ctx.pendingPayloadElements) {
        if (!s.valid) {
            s = BytecodeExecContext::PendingPayloadElement{};
            s.eventId = eventId;
            s.valid = true;
            return &s;
        }
    }
    return nullptr;
}

void stashBuiltPayloadFloat(BytecodeExecContext& ctx, u32 elementId, u8 width,
                            const std::array<f32, 4>& value) noexcept {
    for (auto& b : ctx.builtPayloadFloats) {
        if (b.valid && b.elementId == elementId) {
            b.width = width;
            b.value = value;
            return;
        }
    }
    for (auto& b : ctx.builtPayloadFloats) {
        if (!b.valid) {
            b.valid = true;
            b.elementId = elementId;
            b.width = width;
            b.value = value;
            return;
        }
    }
    ctx.builtPayloadFloats[0] = {true, elementId, width, value};
}

bool dispatchInitPayload(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                         IssueBag& issues) noexcept {
    if (ins.operands[3] < 2U) {
        issues.push(vmFatal(issues::vm::kOperandCount, "IR: initPayload requires 2+ args"));
        return false;
    }

    u32 eventId = 0U;
    if (auto resolved = resolveKickEventIdFromObjSlot(ins, ctx); resolved) {
        eventId = *resolved;
    } else {
        RegisterValue eventReg;
        if (!readFnArg(ins, 0, ctx, eventReg, issues)) {
            return false;
        }
        eventId = laneAsU32(eventReg, 0);
    }

    u32 count = 0;
    const u32 nargs = valueArgCount(ins);
    RegisterValue eventKeyReg;
    if (nargs >= 1U && readValueArg(ins, nargs - 1U, ctx, eventKeyReg, issues)) {
        const u32 genKey = laneAsU32(eventKeyReg, 2);
        for (const auto& e : ctx.eventCaches) {
            if (e.valid && e.key == genKey) {
                count = e.count;
                break;
            }
        }
    }
    setPendingKickCount(ctx, eventId, count);
    out = RegisterValue::scalarI(0);
    return true;
}

bool dispatchKick(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                  IssueBag& issues) noexcept {
    if (ins.operands[3] < 1U) {
        issues.push(vmFatal(issues::vm::kOperandCount, "IR: kick requires 1+ args"));
        return false;
    }

    u32 eventId = 0U;
    if (auto resolved = resolveKickEventIdFromObjSlot(ins, ctx); resolved) {
        eventId = *resolved;
    } else {
        RegisterValue eventIdReg;
        if (!readFnArg(ins, 0, ctx, eventIdReg, issues)) {
            return false;
        }
        eventId = laneAsU32(eventIdReg, 0);
    }
    const u32 count = takePendingKickCount(ctx, eventId);

    BytecodeExecContext::PendingPayloadElement pendingPayload{};
    for (auto& slot : ctx.pendingPayloadElements) {
        if (slot.valid && slot.eventId == eventId) {
            pendingPayload = slot;
            slot.valid = false;
            slot.positionCount = 0;
            slot.hasOrientation = false;
            break;
        }
    }

    if (ctx.spawnQueue != nullptr) {

        const u64 parentSelfId = ctx.currentSelfId;
        for (u32 i = 0; i < count; ++i) {
            if (ctx.spawnQueue->capacity != 0U &&
                ctx.spawnQueue->events.size() >= ctx.spawnQueue->capacity) {
                ctx.spawnQueue->dropped += (count - i);
                break;
            }

            const u32 parentRng = (ctx.rng != nullptr) ? ctx.rng->advance() : 0U;
            SpawnEvent ev;
            ev.eventId = eventId;
            ev.sequenceIndex = i;
            ev.parentSelfId = parentSelfId;
            ev.parentRngState = parentRng;
            if (i < pendingPayload.positionCount) {
                ev.hasSpawnPosition = true;
                ev.spawnPosition = pendingPayload.positions[i];
                ev.spawnPositionPayloadId = pendingPayload.positionPayloadId;
            }
            if (pendingPayload.hasOrientation) {
                ev.hasSpawnOrientation = true;
                ev.spawnOrientation = pendingPayload.orientation;
                ev.spawnOrientationPayloadId = pendingPayload.orientationPayloadId;
            }
            if (pendingPayload.hasIntPayload) {
                ev.hasIntPayload = true;
                ev.intPayloadWidth = pendingPayload.intPayloadWidth;
                ev.intPayload = pendingPayload.intPayload;
                ev.intPayloadId = pendingPayload.intPayloadId;
            }
            if (pendingPayload.hasSpawnIndexPayload) {
                ev.hasIntPayload = true;
                ev.intPayloadWidth = 1U;
                ev.intPayloadId = pendingPayload.spawnIndexPayloadId;
                ev.intPayload = {pendingPayload.spawnIndexBase + static_cast<i32>(i), 0, 0, 0};
            }
            if (pendingPayload.hasBoolPayload) {
                ev.hasBoolPayload = true;
                ev.boolPayloadWidth = pendingPayload.boolPayloadWidth;
                ev.boolPayload = pendingPayload.boolPayload;
                ev.boolPayloadId = pendingPayload.boolPayloadId;
            }
            ev.floatSlots = pendingPayload.floatSlots;

            if (i < BytecodeExecContext::kMaxPendingPositions) {
                ev.subFrameFraction = ctx.lastGenerateTs[i];
                ev.lerpedTime = ctx.lastGenerateLerpedTimes[i];
            }
            ctx.spawnQueue->events.push_back(ev);
        }
    }
    out = RegisterValue::scalarI(0);
    return true;
}

bool dispatchBuildPayloadElement(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                                 RegisterValue& out, IssueBag& issues) noexcept {
    const u32 payloadElementId = ctx.nextPayloadElementId++;
    out = RegisterValue::scalarI(static_cast<i32>(payloadElementId));
    const u32 argc = ins.operands[3];
    if (argc < 3U) {
        return true;
    }

    RegisterValue payloadA;
    RegisterValue payloadB;
    if (!readFnArg(ins, 1, ctx, payloadA, issues) || !readFnArg(ins, 2, ctx, payloadB, issues)) {
        return false;
    }
    bool hasDerivatives = false;
    RegisterValue payloadAd{};
    RegisterValue payloadBd{};
    u32 packedSemantic = 1U;
    if (argc >= 6U) {
        if (!readFnArg(ins, 3, ctx, payloadAd, issues) ||
            !readFnArg(ins, 4, ctx, payloadBd, issues)) {
            return false;
        }
        hasDerivatives = true;
        RegisterValue sem;
        if (readFnArg(ins, 5, ctx, sem, issues)) {
            packedSemantic = laneAsU32(sem, 0);
        }
    } else if (argc == 5U) {
        if (readFnArg(ins, 3, ctx, payloadAd, issues) &&
            readFnArg(ins, 4, ctx, payloadBd, issues)) {
            hasDerivatives = true;
            packedSemantic = 2U;
        }
    } else if (argc == 4U) {
        RegisterValue sem;
        if (readFnArg(ins, 3, ctx, sem, issues)) {
            packedSemantic = laneAsU32(sem, 0);
        }
    }
    const u8 semByte = static_cast<u8>(packedSemantic & 0xFFU);

    const u8 bnk = payloadA.typeBank;
    const bool isIntBank =
        (bnk == bank::kInt || bnk == bank::kInt2 || bnk == bank::kInt2Alt ||
         bnk == bank::kInt2Alt2 || bnk == bank::kInt3 || bnk == bank::kInt4 || bnk == bank::kPtr);
    const bool isScalarBool = (bnk == bank::kBool && payloadA.componentCount == 1U);
    const bool isQuaternion = (bnk == bank::kIntAlt);
    const u8 intWidth = std::min<u8>(payloadA.componentCount, 4U);
    const u8 fWidth =
        std::min<u8>(std::max<u8>(payloadA.componentCount, payloadB.componentCount), 4U);

    if (!isIntBank && !isScalarBool) {
        std::array<f32, 4> fval{};
        for (u8 lane = 0; lane < 4; ++lane) {
            fval[lane] = (semByte == 0U) ? payloadA.lanes[lane] : payloadB.lanes[lane];
        }
        stashBuiltPayloadFloat(ctx, payloadElementId, fWidth, fval);
    } else if (isIntBank) {
        RegisterValue keys;
        i32 base = 0;
        if (readFnArg(ins, 0, ctx, keys, issues)) {
            base = laneAsI32(keys, 1);
        }
        ctx.builtPayloadIndex = {true, payloadElementId, base};
    }

    const auto resolved = resolveKickEventIdFromObjSlot(ins, ctx);
    if (!resolved) {
        return true;
    }
    BytecodeExecContext::PendingPayloadElement* slot = findOrCreatePendingPayload(ctx, *resolved);
    if (slot == nullptr) {
        return true;
    }

    if (isScalarBool) {
        slot->hasBoolPayload = true;
        slot->boolPayloadWidth = 1U;
        slot->boolPayloadId = payloadElementId;
        const i32 src = (semByte == 0U) ? laneAsI32(payloadA, 0) : laneAsI32(payloadB, 0);
        slot->boolPayload[0] = (src != 0) ? 1 : 0;
        return true;
    }
    if (isIntBank && intWidth >= 1U && intWidth <= 4U) {
        slot->hasIntPayload = true;
        slot->intPayloadWidth = intWidth;
        slot->intPayloadId = payloadElementId;
        for (u8 lane = 0; lane < intWidth; ++lane) {
            slot->intPayload[lane] =
                (semByte == 0U) ? laneAsI32(payloadA, lane) : laneAsI32(payloadB, lane);
        }
        return true;
    }
    if (isQuaternion) {
        slot->hasOrientation = true;
        slot->orientationPayloadId = payloadElementId;
        for (int i = 0; i < 4; ++i) {
            slot->orientation[i] = (semByte == 0U) ? payloadA.lanes[i] : payloadB.lanes[i];
        }
        return true;
    }
    if (fWidth == 3U) {
        const u32 count =
            ctx.lastGenerateValid
                ? std::min<u32>(ctx.lastGenerateCount,
                                static_cast<u32>(BytecodeExecContext::kMaxPendingPositions))
                : 0U;
        slot->positionCount = count;
        slot->positionPayloadId = payloadElementId;
        for (u32 i = 0; i < count; ++i) {
            const f32 t = ctx.lastGenerateTs[i];
            for (u8 lane = 0; lane < 3; ++lane) {
                const f32 a = payloadA.lanes[lane];
                const f32 b = payloadB.lanes[lane];
                f32 v;
                if (semByte == 0U) {
                    v = a;
                } else if (semByte == 2U && hasDerivatives) {
                    const f32 t2 = t * t;
                    const f32 t3 = t2 * t;
                    const f32 h10 = t3 - 2.0F * t2 + t;
                    const f32 h11 = t3 - t2;
                    const f32 ad = payloadAd.lanes[lane];
                    const f32 bd = payloadBd.lanes[lane];
                    v = a + (b - a) * t + h10 * ad + h11 * bd;
                } else {
                    v = a + (b - a) * t;
                }
                slot->positions[i][lane] = v;
            }
        }
    }
    return true;
}

struct ResolvedSpatialLayer {
    const SpatialLayerResource* resource = nullptr;
    i32 hashIndex = -1;
};
ResolvedSpatialLayer resolveSpatialLayer(const CBEMInstruction& ins,
                                         const BytecodeExecContext& ctx) noexcept;

u32 spatialPayloadNameHashById(const SpatialLayerResource& layer, u32 payloadId) noexcept;

bool dispatchAppendPayload(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                           IssueBag& issues) noexcept {
    out = RegisterValue::scalarI(0);
    const u32 argc = ins.operands[3];

    if (const ResolvedSpatialLayer spatial = resolveSpatialLayer(ins, ctx);
        spatial.resource != nullptr) {

        RegisterValue keyReg;
        RegisterValue valReg;
        if (argc >= 1U && readFnArg(ins, 0, ctx, keyReg, issues)) {
            out = keyReg;
        }
        if (argc >= 2U && readFnArg(ins, 1, ctx, valReg, issues)) {
            const i32 key = laneAsI32(keyReg, 0);
            u32 nameHash = 0U;
            if (argc >= 3U) {
                RegisterValue idReg;
                if (readFnArg(ins, 2, ctx, idReg, issues)) {
                    nameHash = spatialPayloadNameHashById(*spatial.resource, laneAsU32(idReg, 0));
                }
            }
            for (auto& slot : ctx.spatialAppendStaged) {
                if (!slot.valid || (slot.key == key && slot.nameHash == nameHash)) {
                    slot.valid = true;
                    slot.key = key;
                    slot.nameHash = nameHash;
                    slot.components = (valReg.componentCount > 0U) ? valReg.componentCount : 3U;
                    slot.value = {valReg.lanes[0], valReg.lanes[1], valReg.lanes[2],
                                  valReg.lanes[3]};
                    break;
                }
            }
        }
        return true;
    }

    if (argc >= 1U) {
        RegisterValue arg0;
        if (readFnArg(ins, 0, ctx, arg0, issues)) {
            out = arg0;
        }
    }
    if (argc < 3U) {
        return true;
    }
    const auto resolved = resolveKickEventIdFromObjSlot(ins, ctx);
    if (!resolved) {
        return true;
    }
    RegisterValue slotReg;
    RegisterValue elemReg;
    if (!readFnArg(ins, 1, ctx, slotReg, issues) || !readFnArg(ins, 2, ctx, elemReg, issues)) {
        return true;
    }
    const u32 payloadElementId = laneAsU32(slotReg, 0);
    const u32 elementId = laneAsU32(elemReg, 0);

    u32 nameId = 0U;
    const std::string_view channel = resolveEventChannelName(ins, ctx);
    for (const auto& decl : ctx.kickedEventDecls) {
        if (decl.channel == channel && payloadElementId < decl.elements.size()) {
            nameId = decl.elements[payloadElementId].nameId;
            break;
        }
    }
    if (ctx.builtPayloadIndex.valid && ctx.builtPayloadIndex.elementId == elementId) {
        if (auto* slot = findOrCreatePendingPayload(ctx, *resolved)) {
            slot->hasSpawnIndexPayload = true;
            slot->spawnIndexPayloadId = (nameId != 0U) ? nameId : payloadElementId;
            slot->spawnIndexBase = ctx.builtPayloadIndex.base;
        }
        return true;
    }
    for (const auto& b : ctx.builtPayloadFloats) {
        if (!b.valid || b.elementId != elementId) {
            continue;
        }
        BytecodeExecContext::PendingPayloadElement* slot =
            findOrCreatePendingPayload(ctx, *resolved);
        if (slot == nullptr) {
            break;
        }
        if (nameId != 0U) {
            PayloadFloatSlot* dst = nullptr;
            for (auto& fs : slot->floatSlots) {
                if (fs.valid && fs.nameId == nameId) {
                    dst = &fs;
                    break;
                }
            }
            if (dst == nullptr) {
                for (auto& fs : slot->floatSlots) {
                    if (!fs.valid) {
                        dst = &fs;
                        break;
                    }
                }
            }
            if (dst != nullptr) {
                *dst = PayloadFloatSlot{nameId, true, b.width, b.value};
            }
        }
        if (nameId == payloadNameId("Position") && b.width == 3U) {
            slot->positionCount = static_cast<u32>(BytecodeExecContext::kMaxPendingPositions);
            slot->positionPayloadId = nameId;
            for (std::size_t i = 0; i < BytecodeExecContext::kMaxPendingPositions; ++i) {
                slot->positions[i] = {b.value[0], b.value[1], b.value[2]};
            }
        }
        if (nameId == payloadNameId("Orientation") && b.width == 4U) {
            slot->hasOrientation = true;
            slot->orientationPayloadId = nameId;
            slot->orientation = {b.value[0], b.value[1], b.value[2], b.value[3]};
        }
        break;
    }
    return true;
}

struct SamplerMemo {
    const ExternalBinding* bindings;
    std::size_t bindingCount;
    const SamplerResource* samplers;
    std::size_t samplerCount;
    u32 slot;
    const SamplerResource* result;
    u32 generation;
    u32 valid;
};
inline constexpr std::size_t kSamplerMemoWays = 8U;

SamplerMemo* samplerMemoRows() noexcept {
    static thread_local SamplerMemo rows[kSamplerMemoWays];
    return rows;
}

const SamplerResource* resolveTargetSampler(const CBEMInstruction& ins,
                                            const BytecodeExecContext& ctx) noexcept {
    const u32 extFunc = ins.operands[2];
    if (extFunc >= ctx.functions.size()) {
        return nullptr;
    }
    u32 extSlot = ctx.functions[extFunc].symbolSlot;
    if (extSlot == kSymbolSlotUnbound) {
        extSlot = ins.operands[1];
    }
    if (extSlot == kSymbolSlotUnbound) {
        return nullptr;
    }

    const u32 generation = samplerBindGeneration();
    SamplerMemo& row = samplerMemoRows()[extSlot & (kSamplerMemoWays - 1U)];
    if (row.valid != 0U && row.generation == generation && row.slot == extSlot &&
        row.bindings == ctx.externalBindings.data() &&
        row.bindingCount == ctx.externalBindings.size() && row.samplers == ctx.samplers.data() &&
        row.samplerCount == ctx.samplers.size()) {
        return row.result;
    }

    std::string_view name;
    for (const auto& b : ctx.externalBindings) {
        if (b.slot == static_cast<u16>(extSlot)) {
            name = b.name;
            break;
        }
    }
    const SamplerResource* const result =
        name.empty() ? nullptr : findSamplerByName(ctx.samplers, name);

    row.bindings = ctx.externalBindings.data();
    row.bindingCount = ctx.externalBindings.size();
    row.samplers = ctx.samplers.data();
    row.samplerCount = ctx.samplers.size();
    row.slot = extSlot;
    row.result = result;
    row.generation = generation;
    row.valid = 1U;
    return result;
}

bool sampleTurbulenceToReg(const SamplerResource& res, const CBEMInstruction& ins,
                           BytecodeExecContext& ctx, RegisterValue& out,
                           IssueBag& issues) noexcept {
    Float3 queryPos{0.0F, 0.0F, 0.0F};
    if (ins.operands[3] >= 1U) {
        RegisterValue arg;
        if (readFnArg(ins, 0, ctx, arg, issues)) {
            queryPos = Float3{arg.lanes[0], arg.lanes[1], arg.lanes[2]};
        }
    }
    f32 animTime = 0.0F;
    bool hasAnimTime = false;
    if (ins.operands[3] >= 2U) {
        RegisterValue arg;
        if (readFnArg(ins, 1, ctx, arg, issues)) {
            animTime = arg.lanes[0];
            hasAnimTime = true;
        }
    }
    const Float3 vel = (res.turbulence.dataSource == TurbulenceDataSource::External)
                           ? sampleVectorFieldVelocity(res.turbulence.vectorField, queryPos,
                                                       hasAnimTime ? &animTime : nullptr)
                           : sampleTurbulenceVelocity(res.turbulence, queryPos, ctx.effectAge);
    out = RegisterValue{};
    out.componentCount = 3;
    out.typeBank = bank::kFloat3;
    out.lanes[0] = vel.x;
    out.lanes[1] = vel.y;
    out.lanes[2] = vel.z;
    return true;
}

bool sampleTextureToReg(const SamplerResource& res, const CBEMInstruction& ins,
                        BytecodeExecContext& ctx, RegisterValue& out, IssueBag& issues) noexcept {
    const SamplerTexture& tex = res.texture;
    const u8 comps = (tex.scriptOutputType == 1U) ? 1U : 4U;

    out = RegisterValue{};
    out.componentCount = comps;
    out.typeBank = floatBankForComponentCount(comps);

    if (ins.operands[3] < 1U) {
        issues.push(vmFatal(issues::vm::kOperandCount, "IR: texture sample requires a uv arg"));
        return false;
    }
    RegisterValue uv;
    if (!readFnArg(ins, 0, ctx, uv, issues)) {
        return false;
    }
    auto filter = TextureFilter::Point;
    auto address = TextureAddressMode::Clamp;
    if (ins.operands[3] >= 2U) {
        RegisterValue f;
        if (readFnArg(ins, 1, ctx, f, issues) && f.lanes[0] != 0.0F) {
            filter = TextureFilter::Linear;
        }
    }
    if (ins.operands[3] >= 3U) {
        RegisterValue a;
        if (readFnArg(ins, 2, ctx, a, issues) && a.lanes[0] != 0.0F) {
            address = TextureAddressMode::Wrap;
        }
    }
    if (tex.image == nullptr) {
        return true;
    }
    const Float4 c = sampleTexture2D(*tex.image, uv.lanes[0], uv.lanes[1], filter, address);
    out.lanes[0] = c.x;
    if (comps == 4U) {
        out.lanes[1] = c.y;
        out.lanes[2] = c.z;
        out.lanes[3] = c.w;
    }
    return true;
}

bool dispatchTextureDimensions(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                               RegisterValue& out, IssueBag& ) noexcept {
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr || res->kind != SamplerKind::Texture) {
        return false;
    }
    out = RegisterValue{};
    out.componentCount = 2;
    out.typeBank = bank::kFloat2;
    if (res->texture.image != nullptr) {
        out.lanes[0] = static_cast<f32>(res->texture.image->width);
        out.lanes[1] = static_cast<f32>(res->texture.image->height);
    }
    return true;
}

bool dispatchTextureAtlasRectCount(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                                   RegisterValue& out, IssueBag& ) noexcept {
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr || res->kind != SamplerKind::Texture) {
        return false;
    }
    out = RegisterValue{};
    out.componentCount = 1;
    out.typeBank = bank::kInt;
    out.lanes[0] = 0.0F;
    return true;
}

bool dispatchSample(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                    IssueBag& issues) noexcept {
    static const bool kAblateSample = std::getenv("CF_ABLATE_SAMPLE") != nullptr;
    if (kAblateSample) [[unlikely]] {
        out = RegisterValue{};
        out.componentCount = 1;
        out.typeBank = bank::kFloat;
        return true;
    }
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr) {
        return false;
    }
    if (res->kind == SamplerKind::Turbulence) {
        return sampleTurbulenceToReg(*res, ins, ctx, out, issues);
    }
    if (res->kind == SamplerKind::Texture) {
        return sampleTextureToReg(*res, ins, ctx, out, issues);
    }
    if (res->kind != SamplerKind::Curve) {
        return false;
    }
    if (ins.operands[3] < 1U) {
        issues.push(vmFatal(issues::vm::kOperandCount, "IR: sample requires a t arg"));
        return false;
    }
    RegisterValue arg;
    if (!readFnArg(ins, 0, ctx, arg, issues)) {
        return false;
    }
    const u8 comps = res->curve.components;
    if (comps < 1U || comps > 4U) {
        return false;
    }
    out = RegisterValue{};
    out.componentCount = comps;
    out.typeBank = floatBankForComponentCount(comps);
    const u8 written = evalSamplerCurveVec(res->curve, arg.lanes[0], out.lanes, 4);
    if (written == 0U) {
        return false;
    }
    return true;
}

bool dispatchSampleCDF(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                       IssueBag& issues) noexcept {
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr || res->kind != SamplerKind::Curve) {
        return false;
    }
    if (ins.operands[3] < 1U) {
        issues.push(vmFatal(issues::vm::kOperandCount, "IR: sampleCDF requires a cursor arg"));
        return false;
    }
    RegisterValue arg;
    if (!readFnArg(ins, 0, ctx, arg, issues)) {
        return false;
    }
    out = RegisterValue{};
    out.componentCount = 1;
    out.typeBank = bank::kFloat;
    out.lanes[0] = evalSamplerCurveCdf(res->curve, arg.lanes[0]);
    return true;
}

bool dispatchHasPayloadElement(const CBEMInstruction&, BytecodeExecContext& ctx, RegisterValue& out,
                               IssueBag&) noexcept {
    out = RegisterValue{};
    out.componentCount = 1;
    out.typeBank = bank::kInt;

    const i32 found = (ctx.currentSelfId != 0U) ? -1 : 0;
    setLaneI32(out, 0, found);
    return true;
}

bool dispatchExtractPayloadElement(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                                   RegisterValue& out, IssueBag& issues,
                                   std::string_view symbol) noexcept {
    out = RegisterValue{};

    const char suffix = symbol.size() > std::string_view{"extractPayloadElement"}.size()
                            ? symbol[std::string_view{"extractPayloadElement"}.size()]
                            : '\0';
    const char widthCh = symbol.size() > std::string_view{"extractPayloadElement"}.size() + 1U
                             ? symbol[std::string_view{"extractPayloadElement"}.size() + 1U]
                             : '\0';

    u32 payloadIndex = 0U;
    if (ins.operands[3] >= 1U) {
        RegisterValue idReg;
        if (readFnArg(ins, 0, ctx, idReg, issues)) {
            payloadIndex = laneAsU32(idReg, 0);
        }
    }
    const u32 declNameId =
        (payloadIndex < ctx.rootEventDecl.size()) ? ctx.rootEventDecl[payloadIndex].nameId : 0U;
    auto matches = [payloadIndex, declNameId](u32 stagedId) {
        return (declNameId != 0U && declNameId == stagedId) || payloadIndex == 0U ||
               payloadIndex == stagedId;
    };

    if (suffix == 'O') {

        out.componentCount = 4;
        out.typeBank = bank::kFloat4;
        if (matches(ctx.spawnOrientationPayloadId)) {
            out.lanes[0] = ctx.spawnQuat[0];
            out.lanes[1] = ctx.spawnQuat[1];
            out.lanes[2] = ctx.spawnQuat[2];
            out.lanes[3] = ctx.spawnQuat[3];
        }
        return true;
    }

    const u8 width = (widthCh >= '1' && widthCh <= '4') ? static_cast<u8>(widthCh - '0') : 1U;
    if (suffix == 'F') {
        out.componentCount = width;
        out.typeBank = floatBankForComponentCount(width);
        const u32 nameId = declNameId;
        if (nameId != 0U) {
            for (const auto& fs : ctx.spawnFloatSlots) {
                if (fs.valid && fs.nameId == nameId) {
                    for (u8 i = 0; i < width; ++i) {
                        out.lanes[i] = fs.value[i];
                    }
                    return true;
                }
            }
        }
        return true;
    }

    out.componentCount = width;
    out.typeBank = intBankForComponentCount(width);
    if (suffix == 'I' && ctx.hasSpawnIntPayload && matches(ctx.spawnIntPayloadId)) {
        const u8 staged = std::min<u8>(ctx.spawnIntPayloadWidth, width);
        for (u8 i = 0; i < staged; ++i) {
            setLaneI32(out, i, ctx.spawnIntPayload[i]);
        }
        for (u8 i = staged; i < width; ++i) {
            setLaneI32(out, i, 0);
        }
        return true;
    }
    if (suffix == 'B' && ctx.hasSpawnBoolPayload && matches(ctx.spawnBoolPayloadId)) {
        const u8 staged = std::min<u8>(ctx.spawnBoolPayloadWidth, width);
        for (u8 i = 0; i < staged; ++i) {
            setLaneI32(out, i, ctx.spawnBoolPayload[i]);
        }
        for (u8 i = staged; i < width; ++i) {
            setLaneI32(out, i, 0);
        }
        return true;
    }
    for (u8 i = 0; i < width; ++i) {
        setLaneI32(out, i, 0);
    }
    return true;
}

ResolvedSpatialLayer resolveSpatialLayer(const CBEMInstruction& ins,
                                         const BytecodeExecContext& ctx) noexcept {
    const u32 extFunc = ins.operands[2];
    if (extFunc >= ctx.functions.size()) {
        return {};
    }
    const u32 extSlot = ctx.functions[extFunc].symbolSlot;
    if (extSlot == kSymbolSlotUnbound) {
        return {};
    }
    std::string_view name;
    for (const auto& b : ctx.externalBindings) {
        if (b.slot == static_cast<u16>(extSlot)) {
            name = b.name;
            break;
        }
    }
    if (name.empty()) {
        return {};
    }

    for (std::size_t i = 0; i < ctx.spatialLayers.size(); ++i) {
        if (ctx.spatialLayers[i].name == name) {
            return ResolvedSpatialLayer{
                &ctx.spatialLayers[i],
                static_cast<i32>(i),
            };
        }
    }
    return {};
}

u32 spatialPayloadNameHashById(const SpatialLayerResource& layer, u32 payloadId) noexcept {
    if (payloadId >= layer.payloads.size()) {
        return 0U;
    }
    return spatialPayloadNameHash(layer.payloads[payloadId].name);
}

const SamplerResource* resolveHandleArgSampler(const CBEMInstruction& ins,
                                               const BytecodeExecContext& ctx,
                                               SamplerKind kind) noexcept {
    for (u32 i = 0; i < ins.operands[3]; ++i) {
        const std::size_t idx = (static_cast<std::size_t>(i) * 2U) + 1U;
        if (idx >= ins.extraOperands.size()) {
            break;
        }
        const u32 reg = ins.extraOperands[idx];
        if (!isHandleArg(reg)) {
            continue;
        }
        for (u8 h = 0; h < ctx.handleRegisterCount; ++h) {
            if (ctx.handleRegisterSlots[h].reg != reg) {
                continue;
            }
            const u16 slot = ctx.handleRegisterSlots[h].slot;
            if (slot < ctx.externalBindings.size()) {
                const SamplerResource* res =
                    findSamplerByName(ctx.samplers, ctx.externalBindings[slot].name);
                if (res != nullptr && res->kind == kind) {
                    return res;
                }
            }
            break;
        }
    }
    return nullptr;
}

bool dispatchAllocatePayload(const CBEMInstruction&, BytecodeExecContext& ctx, RegisterValue& out,
                             IssueBag&) noexcept {
    const u32 key = ctx.simUnitScratchCounter;
    ctx.simUnitScratchCounter = key + 1U;
    out = RegisterValue::scalarI(static_cast<i32>(key));
    return true;
}

bool dispatchSpatialInsert(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                           IssueBag& issues) noexcept {
    out = RegisterValue::scalarI(0);
    if (ins.operands[3] < 2U) {
        issues.push(
            vmFatal(issues::vm::kOperandCount, "IR: spatial insert requires (key, position)"));
        return false;
    }
    const ResolvedSpatialLayer resolved = resolveSpatialLayer(ins, ctx);
    const std::span<ProximityHash* const> target =
        ctx.spatialHashesWrite.empty() ? ctx.spatialHashes : ctx.spatialHashesWrite;
    if (resolved.resource == nullptr || resolved.hashIndex < 0 ||
        static_cast<std::size_t>(resolved.hashIndex) >= target.size()) {

        return true;
    }
    ProximityHash* hash = target[static_cast<std::size_t>(resolved.hashIndex)];
    if (hash == nullptr) {
        return true;
    }

    RegisterValue keyReg;
    RegisterValue posReg;
    if (!readFnArg(ins, 0, ctx, keyReg, issues) || !readFnArg(ins, 1, ctx, posReg, issues)) {
        return false;
    }
    const std::array<f32, 3> pos{posReg.lanes[0], posReg.lanes[1], posReg.lanes[2]};
    const i32 key = laneAsI32(keyReg, 0);
    std::array<f32, 3> payload = pos;
    std::array<ProximityPayload, kMaxProximityPayloads> named{};
    std::size_t namedCount = 0U;
    bool haveLegacy = false;
    for (auto& slot : ctx.spatialAppendStaged) {
        if (!slot.valid || slot.key != key) {
            continue;
        }
        if (!haveLegacy) {
            payload = {slot.value[0], slot.value[1], slot.value[2]};
            haveLegacy = true;
        }
        if (namedCount < kMaxProximityPayloads) {
            named[namedCount++] = ProximityPayload{slot.nameHash, slot.components, slot.value};
        }
        slot.valid = false;
    }
    hash->insert(pos, payload, ctx.currentSelfId,
                 std::span<const ProximityPayload>{named.data(), namedCount});
    return true;
}

void readSpatialPayload(const ProximityEntry& entry, u32 nameHash, u8 width,
                        RegisterValue& out) noexcept {
    const ProximityPayload* p = (nameHash != 0U) ? entry.findPayload(nameHash) : nullptr;
    for (u8 i = 0; i < width && i < 4U; ++i) {
        if (p != nullptr) {
            out.lanes[i] = (i < p->components) ? p->value[i] : 0.0F;
        } else {
            out.lanes[i] = (i < 3U) ? entry.payload[i] : 0.0F;
        }
    }
}

bool dispatchSpatialClosest(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                            RegisterValue& out, IssueBag& issues,
                            std::string_view symbol) noexcept {
    out = RegisterValue{};

    const std::string_view base{"closest"};
    const char suffix = symbol.size() > base.size() ? symbol[base.size()] : '\0';
    const char widthCh = symbol.size() > base.size() + 1U ? symbol[base.size() + 1U] : '\0';
    const u8 width = (widthCh >= '1' && widthCh <= '4') ? static_cast<u8>(widthCh - '0') : 1U;
    if (suffix == 'F') {
        const u8 banks[5] = {0, bank::kFloat, bank::kFloat2, bank::kFloat3, bank::kFloat4};
        out.componentCount = width;
        out.typeBank = banks[width];
    } else {
        const u8 banks[5] = {0, bank::kInt, bank::kInt2, bank::kInt3, bank::kInt4};
        out.componentCount = width;
        out.typeBank = banks[width];
    }

    const ResolvedSpatialLayer resolved = resolveSpatialLayer(ins, ctx);
    if (resolved.resource == nullptr || resolved.hashIndex < 0 ||
        static_cast<std::size_t>(resolved.hashIndex) >= ctx.spatialHashes.size()) {

        return true;
    }
    const ProximityHash* hash = ctx.spatialHashes[static_cast<std::size_t>(resolved.hashIndex)];
    if (hash == nullptr) {
        return true;
    }

    const u32 values = valueArgCount(ins);
    if (values < 3U) {
        return true;
    }
    RegisterValue centerReg;
    RegisterValue radiusReg;
    if (!readValueArg(ins, 0, ctx, centerReg, issues) ||
        !readValueArg(ins, 1, ctx, radiusReg, issues)) {
        return false;
    }

    u32 nIndex = 0U;
    u32 payloadIndex = 2U;
    if (values >= 6U) {
        RegisterValue nReg;
        if (readValueArg(ins, 2, ctx, nReg, issues)) {
            nIndex = laneAsU32(nReg, 0);
        }
        payloadIndex = 5U;
    } else if (values == 4U) {
        RegisterValue outerReg;
        if (readValueArg(ins, 2, ctx, outerReg, issues)) {
            radiusReg = outerReg;
        }
        payloadIndex = 3U;
    }

    u32 nameHash = 0U;
    if (RegisterValue payloadReg; readValueArg(ins, payloadIndex, ctx, payloadReg, issues)) {
        nameHash = spatialPayloadNameHashById(*resolved.resource, laneAsU32(payloadReg, 0));
    }

    const std::array<f32, 3> target{centerReg.lanes[0], centerReg.lanes[1], centerReg.lanes[2]};
    const f32 radius = radiusReg.lanes[0];
    const ProximityEntry* hit = hash->closestN(target, radius, nIndex);
    if (hit == nullptr) {
        if (suffix == 'F') {
            f32 infv = 0.0F;
            const u32 bits = fpbits::kInfF32;
            std::memcpy(&infv, &bits, sizeof(infv));
            for (u8 i = 0; i < out.componentCount && i < 4U; ++i) {
                out.lanes[i] = infv;
            }
        }
        return true;
    }

    if (suffix == 'F') {
        readSpatialPayload(*hit, nameHash, width, out);
        return true;
    }
    RegisterValue tmp{};
    readSpatialPayload(*hit, nameHash, width, tmp);
    for (u8 i = 0; i < width && i < 4U; ++i) {
        setLaneI32(out, i, static_cast<i32>(tmp.lanes[i]));
    }
    return true;
}

bool dispatchSpatialReduce(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                           IssueBag& issues, std::string_view symbol) noexcept {
    out = RegisterValue{};

    std::string_view base;
    bool wantAverage = false;
    bool wantKernel = false;
    if (symbol.starts_with("sumKernel")) {
        base = "sumKernel";
        wantKernel = true;
    } else if (symbol.starts_with("averageKernel")) {
        base = "averageKernel";
        wantAverage = true;
        wantKernel = true;
    } else if (symbol.starts_with("average")) {
        base = "average";
        wantAverage = true;
    } else if (symbol.starts_with("sum")) {
        base = "sum";
    } else {
        return false;
    }
    if (symbol.size() < base.size() + 2U) {
        return false;
    }
    const char suffix = symbol[base.size()];
    const char widthCh = symbol[base.size() + 1U];
    if ((suffix != 'F' && suffix != 'I') || widthCh < '1' || widthCh > '4') {
        return false;
    }
    const u8 width = static_cast<u8>(widthCh - '0');
    out.componentCount = width;
    out.typeBank =
        (suffix == 'F') ? floatBankForComponentCount(width) : intBankForComponentCount(width);

    const ResolvedSpatialLayer resolved = resolveSpatialLayer(ins, ctx);
    if (resolved.resource == nullptr || resolved.hashIndex < 0 ||
        static_cast<std::size_t>(resolved.hashIndex) >= ctx.spatialHashes.size()) {
        return true;
    }
    const ProximityHash* hash = ctx.spatialHashes[static_cast<std::size_t>(resolved.hashIndex)];
    if (hash == nullptr) {
        return true;
    }
    if (valueArgCount(ins) < 3U) {
        return true;
    }
    RegisterValue centerReg;
    RegisterValue radiusReg;
    RegisterValue payloadReg;
    if (!readValueArg(ins, 0, ctx, centerReg, issues) ||
        !readValueArg(ins, 1, ctx, radiusReg, issues) ||
        !readValueArg(ins, 2, ctx, payloadReg, issues)) {
        return false;
    }
    const u32 nameHash = spatialPayloadNameHashById(*resolved.resource, laneAsU32(payloadReg, 0));

    const SamplerCurve* kernelCurve = nullptr;
    if (wantKernel) {
        const SamplerResource* res = resolveHandleArgSampler(ins, ctx, SamplerKind::Curve);
        if (res == nullptr) {
            return false;
        }
        kernelCurve = &res->curve;
    }

    const std::array<f32, 3> target{centerReg.lanes[0], centerReg.lanes[1], centerReg.lanes[2]};
    const f32 radius = radiusReg.lanes[0];
    const f32 invRadius = (radius != 0.0F) ? (1.0F / radius) : 0.0F;

    std::array<f32, 4> sum{0.0F, 0.0F, 0.0F, 0.0F};
    f32 count = 0.0F;
    hash->forEachInRadius(target, radius, [&](const ProximityEntry& e, f32 dSq) {
        f32 w = 1.0F;
        if (kernelCurve != nullptr) {
            std::array<f32, 4> k{0.0F, 0.0F, 0.0F, 0.0F};
            const f32 cursor = std::sqrt(dSq) * invRadius;
            if (evalSamplerCurveVec(*kernelCurve, cursor, k.data(), 4) == 0U) {
                return;
            }
            w = k[0];
        }
        RegisterValue v{};
        readSpatialPayload(e, nameHash, width, v);
        for (u8 i = 0; i < width && i < 4U; ++i) {
            sum[i] += v.lanes[i] * w;
        }
        count += w;
    });

    if (count == 0.0F) {
        f32 infv = 0.0F;
        const u32 bits = fpbits::kInfF32;
        std::memcpy(&infv, &bits, sizeof(infv));
        for (u8 i = 0; i < width && i < 4U; ++i) {
            if (suffix == 'F') {
                out.lanes[i] = infv;
            } else {
                setLaneI32(out, i, 0);
            }
        }
        return true;
    }
    const f32 scale = wantAverage ? (1.0F / count) : 1.0F;
    for (u8 i = 0; i < width && i < 4U; ++i) {
        if (suffix == 'F') {
            out.lanes[i] = sum[i] * scale;
        } else {
            setLaneI32(out, i, static_cast<i32>(sum[i] * scale));
        }
    }
    return true;
}

bool dispatchSpatialNeighborCount(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                                  RegisterValue& out, IssueBag& issues) noexcept {
    out = RegisterValue::scalarI(0);
    const ResolvedSpatialLayer resolved = resolveSpatialLayer(ins, ctx);
    if (resolved.resource == nullptr || resolved.hashIndex < 0 ||
        static_cast<std::size_t>(resolved.hashIndex) >= ctx.spatialHashes.size()) {
        return true;
    }
    const ProximityHash* hash = ctx.spatialHashes[static_cast<std::size_t>(resolved.hashIndex)];
    if (hash == nullptr || ins.operands[3] < 2U) {
        return true;
    }
    RegisterValue centerReg;
    RegisterValue radiusReg;
    if (!readFnArg(ins, 0, ctx, centerReg, issues) || !readFnArg(ins, 1, ctx, radiusReg, issues)) {
        return false;
    }
    const std::array<f32, 3> target{centerReg.lanes[0], centerReg.lanes[1], centerReg.lanes[2]};
    const u32 count = hash->neighborCount(target, radiusReg.lanes[0]);
    out = RegisterValue::scalarI(static_cast<i32>(count));
    return true;
}

bool dispatchSceneOrientation(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                              RegisterValue& out, IssueBag& issues, std::string_view) noexcept {
    if (ins.operands[3] < 1U) {
        issues.push(
            vmFatal(issues::vm::kOperandCount, "IR: scene.orientation requires forward arg"));
        return false;
    }
    RegisterValue forward;
    if (!readFnArg(ins, 0, ctx, forward, issues)) {
        return false;
    }
    const f32 fx = forward.lanes[0];
    const f32 fy = forward.lanes[1];
    const f32 fz = forward.lanes[2];

    f32 qx = -fy;
    f32 qy = fx;
    f32 qz = 0.0F;
    f32 qw = 1.0F + fz;

    const f32 mag2 = qx * qx + qy * qy + qz * qz + qw * qw;
    if (mag2 < 1e-12F) {
        qx = 1.0F;
        qy = 0.0F;
        qz = 0.0F;
        qw = 0.0F;
    } else {
        const f32 invMag = 1.0F / std::sqrt(mag2);
        qx *= invMag;
        qy *= invMag;
        qz *= invMag;
        qw *= invMag;
    }
    out = RegisterValue{};
    out.componentCount = 4;
    out.typeBank = bank::kFloat4;
    out.lanes[0] = qx;
    out.lanes[1] = qy;
    out.lanes[2] = qz;
    out.lanes[3] = qw;
    return true;
}

bool dispatchSceneIntersect(const CBEMInstruction&, BytecodeExecContext&, RegisterValue& out,
                            IssueBag&) noexcept {
    out = RegisterValue{};
    out.componentCount = 4;
    out.typeBank = bank::kFloat4;
    out.lanes[0] = 0.0F;
    out.lanes[1] = 0.0F;
    out.lanes[2] = 0.0F;
    out.lanes[3] = 0.0F;
    return true;
}

namespace xform_mask {
constexpr u32 kFilter = 0x07U;
constexpr u32 kFilterT = 0x01U;
constexpr u32 kFilterQ = 0x02U;
[[maybe_unused]] constexpr u32 kFilterS = 0x04U;

constexpr u32 kSpaceMask = 0x3U;
constexpr u32 kSpaceLocalBit = 0x01U;
constexpr u32 kSpacePayloadBit = 0x02U;

constexpr u32 kSpaceEnterShift = 3U;
constexpr u32 kSpaceLeaveShift = 5U;
}

static bool dispatchXformMaskedShared(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                                      RegisterValue& out, IssueBag& issues, bool isPoint) noexcept {
    if (ins.operands[3] < 1U) {
        issues.push(vmFatal(issues::vm::kOperandCount, "IR: xform_l2w_*_masked requires arg"));
        return false;
    }

    RegisterValue p;
    if (!readFnArg(ins, 0, ctx, p, issues)) {
        return false;
    }

    u32 mask = xform_mask::kFilter | (xform_mask::kSpaceLocalBit << xform_mask::kSpaceEnterShift);
    if (ins.operands[3] >= 2U) {
        RegisterValue m;
        if (!readFnArg(ins, 1, ctx, m, issues)) {
            return false;
        }
        const i32 mv = laneAsI32(m, 0);
        if (mv != 0) {
            mask = static_cast<u32>(mv);
        }
    }

    const u32 filter = mask & xform_mask::kFilter;
    const u32 spaceEnter = (mask >> xform_mask::kSpaceEnterShift) & xform_mask::kSpaceMask;
    const u32 spaceLeave = (mask >> xform_mask::kSpaceLeaveShift) & xform_mask::kSpaceMask;

    const f32 inV[3]{p.lanes[0], p.lanes[1], p.lanes[2]};
    f32 outV[3]{inV[0], inV[1], inV[2]};

    const bool bothPayload = (spaceEnter & xform_mask::kSpacePayloadBit) != 0U &&
                             (spaceLeave & xform_mask::kSpacePayloadBit) != 0U;
    const bool noop = (spaceEnter == spaceLeave) || bothPayload;

    if (!noop) {

        const bool wantPayloadEnter = (spaceEnter & xform_mask::kSpacePayloadBit) != 0U;
        const bool wantPayloadLeave = (spaceLeave & xform_mask::kSpacePayloadBit) != 0U;
        const bool tryPayload = wantPayloadEnter && !wantPayloadLeave;
        const bool hasPositionPayload = (ctx.spawnPositionPayloadId != 0U);
        const bool hasOrientationPayload = (ctx.spawnOrientationPayloadId != 0U);
        const bool usePayloadPath = tryPayload && (hasPositionPayload || hasOrientationPayload);

        const bool wantQ = (filter & xform_mask::kFilterQ) != 0U;
        const bool wantT = isPoint && (filter & xform_mask::kFilterT) != 0U;

        if (usePayloadPath) {

            if (wantQ && hasOrientationPayload) {
                const f32 qx = ctx.spawnQuat[0];
                const f32 qy = ctx.spawnQuat[1];
                const f32 qz = ctx.spawnQuat[2];
                const f32 qw = ctx.spawnQuat[3];

                const f32 tx = qy * inV[2] - qz * inV[1] + qw * inV[0];
                const f32 ty = qz * inV[0] - qx * inV[2] + qw * inV[1];
                const f32 tz = qx * inV[1] - qy * inV[0] + qw * inV[2];
                outV[0] = inV[0] + 2.0F * (qy * tz - qz * ty);
                outV[1] = inV[1] + 2.0F * (qz * tx - qx * tz);
                outV[2] = inV[2] + 2.0F * (qx * ty - qy * tx);
            }
            if (wantT) {
                outV[0] += ctx.spawnTranslate[0];
                outV[1] += ctx.spawnTranslate[1];
                outV[2] += ctx.spawnTranslate[2];
            }
        } else {

            const f32 emitterT[3]{
                ctx.sceneL2W.m[0][3],
                ctx.sceneL2W.m[1][3],
                ctx.sceneL2W.m[2][3],
            };
            if (wantQ) {
                const f32 rx = ctx.sceneL2W.m[0][0] * inV[0] + ctx.sceneL2W.m[0][1] * inV[1] +
                               ctx.sceneL2W.m[0][2] * inV[2];
                const f32 ry = ctx.sceneL2W.m[1][0] * inV[0] + ctx.sceneL2W.m[1][1] * inV[1] +
                               ctx.sceneL2W.m[1][2] * inV[2];
                const f32 rz = ctx.sceneL2W.m[2][0] * inV[0] + ctx.sceneL2W.m[2][1] * inV[1] +
                               ctx.sceneL2W.m[2][2] * inV[2];
                outV[0] = rx;
                outV[1] = ry;
                outV[2] = rz;
            }
            if (wantT) {
                outV[0] += emitterT[0];
                outV[1] += emitterT[1];
                outV[2] += emitterT[2];
            }
        }
    }

    out = RegisterValue{};
    out.componentCount = 3;
    out.typeBank = bank::kFloat3;
    out.lanes[0] = outV[0];
    out.lanes[1] = outV[1];
    out.lanes[2] = outV[2];
    return true;
}

bool dispatchXformL2WPoint(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                           IssueBag& issues) noexcept {
    return dispatchXformMaskedShared(ins, ctx, out, issues, true);
}

bool dispatchXformL2WDirection(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                               RegisterValue& out, IssueBag& issues) noexcept {
    return dispatchXformMaskedShared(ins, ctx, out, issues, false);
}

bool dispatchXformW2LShared(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                            RegisterValue& out, IssueBag& issues, bool isPoint) noexcept {
    if (ins.operands[3] < 1U) {
        issues.push(vmFatal(issues::vm::kOperandCount, "IR: xform_w2l_*_masked requires arg"));
        return false;
    }
    RegisterValue p;
    if (!readFnArg(ins, 0, ctx, p, issues)) {
        return false;
    }
    f32 v0 = p.lanes[0];
    f32 v1 = p.lanes[1];
    f32 v2 = p.lanes[2];
    if (isPoint) {
        v0 -= ctx.sceneL2W.m[0][3];
        v1 -= ctx.sceneL2W.m[1][3];
        v2 -= ctx.sceneL2W.m[2][3];
    }

    const f32 lx =
        ctx.sceneL2W.m[0][0] * v0 + ctx.sceneL2W.m[1][0] * v1 + ctx.sceneL2W.m[2][0] * v2;
    const f32 ly =
        ctx.sceneL2W.m[0][1] * v0 + ctx.sceneL2W.m[1][1] * v1 + ctx.sceneL2W.m[2][1] * v2;
    const f32 lz =
        ctx.sceneL2W.m[0][2] * v0 + ctx.sceneL2W.m[1][2] * v1 + ctx.sceneL2W.m[2][2] * v2;
    out = RegisterValue{};
    out.componentCount = 3;
    out.typeBank = bank::kFloat3;
    out.lanes[0] = lx;
    out.lanes[1] = ly;
    out.lanes[2] = lz;
    return true;
}

bool dispatchXformW2LPoint(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                           IssueBag& issues) noexcept {
    return dispatchXformW2LShared(ins, ctx, out, issues, true);
}

bool dispatchXformW2LDirection(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                               RegisterValue& out, IssueBag& issues) noexcept {
    return dispatchXformW2LShared(ins, ctx, out, issues, false);
}

bool dispatchRotateAxisAngle(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                             RegisterValue& out, IssueBag& issues) noexcept {
    if (ins.operands[3] < 3U) {
        issues.push(vmFatal(issues::vm::kOperandCount, "IR: rotate(axis,angle) requires 3 args"));
        return false;
    }
    RegisterValue v;
    RegisterValue axis;
    RegisterValue ang;
    if (!readFnArg(ins, 0, ctx, v, issues) || !readFnArg(ins, 1, ctx, axis, issues) ||
        !readFnArg(ins, 2, ctx, ang, issues)) {
        return false;
    }

    f32 ax = axis.lanes[0];
    f32 ay = axis.lanes[1];
    f32 az = axis.lanes[2];
    const f32 len = std::sqrt(ax * ax + ay * ay + az * az);
    if (len > 0.0F) {
        ax /= len;
        ay /= len;
        az /= len;
    }
    const f32 c = std::cos(ang.lanes[0]);
    const f32 s = std::sin(ang.lanes[0]);
    const f32 oneMinusC = 1.0F - c;
    const f32 vx = v.lanes[0];
    const f32 vy = v.lanes[1];
    const f32 vz = v.lanes[2];

    const f32 dot = ax * vx + ay * vy + az * vz;
    const f32 cx = ay * vz - az * vy;
    const f32 cy = az * vx - ax * vz;
    const f32 cz = ax * vy - ay * vx;
    out = RegisterValue{};
    out.componentCount = 3;
    out.typeBank = bank::kFloat3;
    out.lanes[0] = vx * c + cx * s + ax * dot * oneMinusC;
    out.lanes[1] = vy * c + cy * s + ay * dot * oneMinusC;
    out.lanes[2] = vz * c + cz * s + az * dot * oneMinusC;
    return true;
}

bool dispatchRotateOrientation(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                               RegisterValue& out, IssueBag& issues) noexcept {
    if (ins.operands[3] < 2U) {
        issues.push(vmFatal(issues::vm::kOperandCount, "IR: rotate(orientation) requires 2 args"));
        return false;
    }
    RegisterValue v;
    RegisterValue q;
    if (!readFnArg(ins, 0, ctx, v, issues) || !readFnArg(ins, 1, ctx, q, issues)) {
        return false;
    }
    const f32 qx = q.lanes[0];
    const f32 qy = q.lanes[1];
    const f32 qz = q.lanes[2];
    const f32 qw = q.lanes[3];
    const f32 vx = v.lanes[0];
    const f32 vy = v.lanes[1];
    const f32 vz = v.lanes[2];

    const f32 tx = qy * vz - qz * vy + qw * vx;
    const f32 ty = qz * vx - qx * vz + qw * vy;
    const f32 tz = qx * vy - qy * vx + qw * vz;
    out = RegisterValue{};
    out.componentCount = 3;
    out.typeBank = bank::kFloat3;
    out.lanes[0] = vx + 2.0F * (qy * tz - qz * ty);
    out.lanes[1] = vy + 2.0F * (qz * tx - qx * tz);
    out.lanes[2] = vz + 2.0F * (qx * ty - qy * tx);
    return true;
}

bool dispatchOrientationMult(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                             RegisterValue& out, IssueBag& issues) noexcept {
    RegisterValue a;
    RegisterValue b;
    if (!readValueArg(ins, 0U, ctx, a, issues) || !readValueArg(ins, 1U, ctx, b, issues)) {
        return false;
    }
    const f32 ax = a.lanes[0];
    const f32 ay = a.lanes[1];
    const f32 az = a.lanes[2];
    const f32 aw = a.lanes[3];
    const f32 bx = b.lanes[0];
    const f32 by = b.lanes[1];
    const f32 bz = b.lanes[2];
    const f32 bw = b.lanes[3];
    out = RegisterValue{};
    out.componentCount = 4;
    out.typeBank = bank::kFloat4;
    out.lanes[0] = (aw * bx) + (ax * bw) + (ay * bz) - (az * by);
    out.lanes[1] = (aw * by) - (ax * bz) + (ay * bw) + (az * bx);
    out.lanes[2] = (aw * bz) + (ax * by) - (ay * bx) + (az * bw);
    out.lanes[3] = (aw * bw) - (ax * bx) - (ay * by) - (az * bz);
    return true;
}

constexpr std::array<u8, 256> kNoisePermutations = {
    0x97, 0xA0, 0x89, 0x5B, 0x5A, 0x0F, 0x83, 0x0D, 0xC9, 0x5F, 0x60, 0x35, 0xC2, 0xE9, 0x07, 0xE1,
    0x8C, 0x24, 0x67, 0x1E, 0x45, 0x8E, 0x08, 0x63, 0x25, 0xF0, 0x15, 0x0A, 0x17, 0xBE, 0x06, 0x94,
    0xF7, 0x78, 0xEA, 0x4B, 0x00, 0x1A, 0xC5, 0x3E, 0x5E, 0xFC, 0xDB, 0xCB, 0x75, 0x23, 0x0B, 0x20,
    0x39, 0xB1, 0x21, 0x58, 0xED, 0x95, 0x38, 0x57, 0xAE, 0x14, 0x7D, 0x88, 0xAB, 0xA8, 0x44, 0xAF,
    0x4A, 0xA5, 0x47, 0x86, 0x8B, 0x30, 0x1B, 0xA6, 0x4D, 0x92, 0x9E, 0xE7, 0x53, 0x6F, 0xE5, 0x7A,
    0x3C, 0xD3, 0x85, 0xE6, 0xDC, 0x69, 0x5C, 0x29, 0x37, 0x2E, 0xF5, 0x28, 0xF4, 0x66, 0x8F, 0x36,
    0x41, 0x19, 0x3F, 0xA1, 0x01, 0xD8, 0x50, 0x49, 0xD1, 0x4C, 0x84, 0xBB, 0xD0, 0x59, 0x12, 0xA9,
    0xC8, 0xC4, 0x87, 0x82, 0x74, 0xBC, 0x9F, 0x56, 0xA4, 0x64, 0x6D, 0xC6, 0xAD, 0xBA, 0x03, 0x40,
    0x34, 0xD9, 0xE2, 0xFA, 0x7C, 0x7B, 0x05, 0xCA, 0x26, 0x93, 0x76, 0x7E, 0xFF, 0x52, 0x55, 0xD4,
    0xCF, 0xCE, 0x3B, 0xE3, 0x2F, 0x10, 0x3A, 0x11, 0xB6, 0xBD, 0x1C, 0x2A, 0xDF, 0xB7, 0xAA, 0xD5,
    0x77, 0xF8, 0x98, 0x02, 0x2C, 0x9A, 0xA3, 0x46, 0xDD, 0x99, 0x65, 0x9B, 0xA7, 0x2B, 0xAC, 0x09,
    0x81, 0x16, 0x27, 0xFD, 0x13, 0x62, 0x6C, 0x6E, 0x4F, 0x71, 0xE0, 0xE8, 0xB2, 0xB9, 0x70, 0x68,
    0xDA, 0xF6, 0x61, 0xE4, 0xFB, 0x22, 0xF2, 0xC1, 0xEE, 0xD2, 0x90, 0x0C, 0xBF, 0xB3, 0xA2, 0xF1,
    0x51, 0x33, 0x91, 0xEB, 0xF9, 0x0E, 0xEF, 0x6B, 0x31, 0xC0, 0xD6, 0x1F, 0xB5, 0xC7, 0x6A, 0x9D,
    0xB8, 0x54, 0xCC, 0xB0, 0x73, 0x79, 0x32, 0x2D, 0x7F, 0x04, 0x96, 0xFE, 0x8A, 0xEC, 0xCD, 0x5D,
    0xDE, 0x72, 0x43, 0x1D, 0x18, 0x48, 0xF3, 0x8D, 0x80, 0xC3, 0x4E, 0x42, 0xD7, 0x3D, 0x9C, 0xB4,
};

constexpr u8 perm(u32 i) noexcept {
    return kNoisePermutations[i & 255U];
}

constexpr f32 noiseGrad1(u8 hash) noexcept {
    const f32 mag = static_cast<f32>(hash & 7U) + 1.0F;
    return ((hash & 8U) != 0U) ? -mag : mag;
}

f32 simplexNoise1(f32 x) noexcept {
    const f32 i0 = std::floor(x);
    const f32 x0 = x - i0;
    const f32 x1 = x0 - 1.0F;
    const auto u0 = static_cast<u8>(static_cast<i32>(i0));
    const f32 t0 = 1.0F - (x0 * x0);
    const f32 t1 = 1.0F - (x1 * x1);
    const f32 t0sqr = t0 * t0;
    const f32 t1sqr = t1 * t1;
    const f32 n0 = t0sqr * t0sqr * x0 * noiseGrad1(perm(u0));
    const f32 n1 = t1sqr * t1sqr * x1 * noiseGrad1(perm(u0 + 1U));
    return 0.39500001F * (n0 + n1);
}

f32 noiseGrad2(u8 hash, f32 x, f32 y) noexcept {
    const u32 h = hash & 7U;
    const f32 u = (h >= 4U) ? y : x;
    const f32 v = (h >= 4U) ? x : y;
    const f32 su = ((hash & 1U) != 0U) ? -u : u;
    const f32 sv = ((hash & 2U) != 0U) ? -v : v;
    return su + (2.0F * sv);
}

f32 simplexNoise2(f32 x, f32 y) noexcept {
    constexpr f32 kF2 = 0.36602539F;
    constexpr f32 kG2 = 0.21132487F;
    constexpr f32 kG2x2 = 0.42264974F;
    auto fastFloor = [](f32 v) noexcept {
        return (v <= 0.0F) ? (static_cast<i32>(v) - 1) : static_cast<i32>(v);
    };

    const f32 s = (x + y) * kF2;
    const i32 i = fastFloor(x + s);
    const i32 j = fastFloor(y + s);
    const f32 t = static_cast<f32>(i + j) * kG2;
    const f32 x0 = x - (static_cast<f32>(i) - t);
    const f32 y0 = y - (static_cast<f32>(j) - t);

    const i32 i1 = (x0 <= y0) ? 0 : 1;
    const i32 j1 = (x0 <= y0) ? 1 : 0;
    const f32 x1 = (x0 - static_cast<f32>(i1)) + kG2;
    const f32 y1 = (y0 - static_cast<f32>(j1)) + kG2;
    const f32 x2 = (x0 - 1.0F) + kG2x2;
    const f32 y2 = (y0 - 1.0F) + kG2x2;

    const auto ii = static_cast<u32>(static_cast<u8>(i));
    const auto jj = static_cast<u32>(static_cast<u8>(j));

    auto corner = [](f32 t_, u8 hash, f32 cx, f32 cy) noexcept {
        if (t_ < 0.0F) {
            return 0.0F;
        }
        const f32 t2 = t_ * t_;
        return t2 * t2 * noiseGrad2(hash, cx, cy);
    };
    const f32 n0 = corner((0.5F - (x0 * x0)) - (y0 * y0), perm(perm(jj) + ii), x0, y0);
    const f32 n1 = corner((0.5F - (x1 * x1)) - (y1 * y1),
                          perm(perm(static_cast<u32>(j1) + jj) + static_cast<u32>(i1) + ii), x1,
                          y1);
    const f32 n2 =
        corner((0.5F - (x2 * x2)) - (y2 * y2), perm(ii + 1U + perm(jj + 1U)), x2, y2);
    return 40.0F * ((n0 + n1) + n2);
}

f32 noiseGrad3(u8 hash, f32 x, f32 y, f32 z) noexcept {
    const u32 h = hash & 15U;
    const f32 u = (h < 8U) ? x : y;
    const f32 b = (h == 12U || h == 14U) ? x : z;
    const f32 v = (h < 4U) ? y : b;
    const f32 su = ((h & 1U) != 0U) ? -u : u;
    const f32 sv = ((h & 2U) != 0U) ? -v : v;
    return su + sv;
}

constexpr u8 kOrder3[8][6] = {
    {1, 0, 0, 1, 1, 0},
    {0, 1, 0, 0, 1, 1},
    {0, 0, 1, 1, 0, 1},
    {0, 0, 1, 0, 1, 1},
    {1, 0, 0, 1, 1, 0},
    {0, 1, 0, 1, 1, 0},
    {1, 0, 0, 1, 0, 1},
    {0, 0, 1, 0, 1, 1},
};

f32 simplexNoise3(f32 x, f32 y, f32 z) noexcept {
    constexpr f32 kF3 = 0.33333334F;
    constexpr f32 kG3 = 0.16666667F;
    constexpr f32 kStep1[2] = {kG3, -1.0F + kG3};
    constexpr f32 kStep2[2] = {kG3 + kG3, -1.0F + (kG3 + kG3)};
    constexpr f32 kStep3 = -0.5F;

    const f32 s = ((x + y) + z) * kF3;
    const f32 fi = std::floor(x + s);
    const f32 fj = std::floor(y + s);
    const f32 fk = std::floor(z + s);
    const f32 t = ((fi + fj) + fk) * kG3;
    const f32 x0 = (x - fi) + t;
    const f32 y0 = (y - fj) + t;
    const f32 z0 = (z - fk) + t;

    const u32 mask = (x0 < y0 ? 1U : 0U) | (y0 < z0 ? 2U : 0U) | (z0 < x0 ? 4U : 0U);
    const auto& o = kOrder3[mask];

    const auto ii = static_cast<u32>(static_cast<u8>(static_cast<i32>(fi)));
    const auto jj = static_cast<u32>(static_cast<u8>(static_cast<i32>(fj)));
    const auto kk = static_cast<u32>(static_cast<u8>(static_cast<i32>(fk)));

    const f32 cx[4] = {x0, x0 + kStep1[o[0]], x0 + kStep2[o[3]], x0 + kStep3};
    const f32 cy[4] = {y0, y0 + kStep1[o[1]], y0 + kStep2[o[4]], y0 + kStep3};
    const f32 cz[4] = {z0, z0 + kStep1[o[2]], z0 + kStep2[o[5]], z0 + kStep3};
    const u8 h[4] = {
        perm(perm(perm(kk) + jj) + ii),
        perm(perm(perm(kk + o[2]) + jj + o[1]) + ii + o[0]),
        perm(perm(perm(kk + o[5]) + jj + o[4]) + ii + o[3]),
        perm(perm(perm(kk + 1U) + 1U + jj) + 1U + ii),
    };

    f32 sum = 0.0F;
    for (u32 c = 0; c < 4U; ++c) {
        const f32 raw = ((-0.60000002F + (cx[c] * cx[c])) + (cy[c] * cy[c])) + (cz[c] * cz[c]);
        const f32 tc = (raw < 0.0F) ? raw : 0.0F;
        const f32 tc2 = tc * tc;
        sum += tc2 * tc2 * noiseGrad3(h[c], cx[c], cy[c], cz[c]);
    }
    return 32.0F * sum;
}

f32 noiseGrad4(u8 hash, f32 x, f32 y, f32 z, f32 w) noexcept {
    const u32 h = hash & 31U;
    const f32 u = (h < 24U) ? x : y;
    const f32 v = (h < 16U) ? y : z;
    const f32 t = (h < 8U) ? z : w;
    const f32 su = ((hash & 1U) != 0U) ? -u : u;
    const f32 sv = ((hash & 2U) != 0U) ? -v : v;
    const f32 st = ((hash & 4U) != 0U) ? -t : t;
    return (su + sv) + st;
}

constexpr u8 kOrder4[64][4] = {
    {0, 1, 2, 3}, {1, 0, 2, 3}, {0, 2, 1, 3}, {0, 0, 0, 0}, {0, 1, 3, 2}, {1, 0, 3, 2},
    {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {2, 0, 1, 3}, {1, 2, 0, 3}, {2, 1, 0, 3},
    {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
    {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {2, 0, 3, 1}, {0, 0, 0, 0}, {0, 0, 0, 0},
    {0, 0, 0, 0}, {3, 0, 1, 2}, {0, 0, 0, 0}, {3, 1, 0, 2}, {0, 0, 0, 0}, {3, 0, 2, 1},
    {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 3, 1, 2}, {0, 0, 0, 0},
    {0, 2, 3, 1}, {0, 0, 0, 0}, {0, 3, 2, 1}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
    {1, 3, 0, 2}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
    {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 2, 3, 0}, {2, 1, 3, 0},
    {1, 3, 2, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {2, 3, 0, 1}, {3, 2, 0, 1},
    {0, 0, 0, 0}, {3, 1, 2, 0}, {2, 3, 1, 0}, {3, 2, 1, 0},
};

f32 simplexNoise4(f32 x, f32 y, f32 z, f32 w) noexcept {
    constexpr f32 kF4 = 0.309017F;
    constexpr f32 kG4 = 0.1381966F;
    constexpr f32 kG4x2 = 0.2763932F;
    constexpr f32 kG4x3 = 0.41458982F;
    constexpr f32 kG4x4m1 = -0.44721359F;

    const f32 s = (((x + y) + z) + w) * kF4;
    const f32 fi = std::floor(x + s);
    const f32 fj = std::floor(y + s);
    const f32 fk = std::floor(z + s);
    const f32 fl = std::floor(w + s);
    const f32 t = (((fi + fj) + fk) + fl) * kG4;
    const f32 p0[4] = {(x - fi) + t, (y - fj) + t, (z - fk) + t, (w - fl) + t};

    const u32 xc = (p0[0] > p0[1] ? 1U : 0U) | (p0[1] > p0[2] ? 2U : 0U) |
                   (p0[2] > p0[3] ? 4U : 0U) | (p0[0] > p0[2] ? 8U : 0U) |
                   (p0[0] > p0[3] ? 16U : 0U) | (p0[1] > p0[3] ? 32U : 0U);
    const auto& rank = kOrder4[xc];

    const u32 base[4] = {
        static_cast<u32>(static_cast<u8>(static_cast<i32>(fi))),
        static_cast<u32>(static_cast<u8>(static_cast<i32>(fj))),
        static_cast<u32>(static_cast<u8>(static_cast<i32>(fk))),
        static_cast<u32>(static_cast<u8>(static_cast<i32>(fl))),
    };

    f32 c[5][4];
    u32 idx[5][4];
    for (u32 a = 0; a < 4U; ++a) {
        const u32 s1 = (rank[a] > 2U) ? 1U : 0U;
        const u32 s2 = (rank[a] > 1U) ? 1U : 0U;
        const u32 s3 = (rank[a] > 0U) ? 1U : 0U;
        c[0][a] = p0[a];
        c[1][a] = (p0[a] - static_cast<f32>(s1)) + kG4;
        c[2][a] = (p0[a] - static_cast<f32>(s2)) + kG4x2;
        c[3][a] = (p0[a] - static_cast<f32>(s3)) + kG4x3;
        c[4][a] = p0[a] + kG4x4m1;
        idx[0][a] = base[a];
        idx[1][a] = base[a] + s1;
        idx[2][a] = base[a] + s2;
        idx[3][a] = base[a] + s3;
        idx[4][a] = base[a] + 1U;
    }

    f32 sum = 0.0F;
    for (u32 k = 0; k < 5U; ++k) {
        const f32 tc = 0.60000002F - ((((c[k][0] * c[k][0]) + (c[k][1] * c[k][1])) +
                                       (c[k][2] * c[k][2])) +
                                      (c[k][3] * c[k][3]));
        if (tc < 0.0F) {
            continue;
        }
        const u8 h = perm(perm(perm(perm(idx[k][3]) + idx[k][2]) + idx[k][1]) + idx[k][0]);
        const f32 tc2 = tc * tc;
        sum += tc2 * tc2 * noiseGrad4(h, c[k][0], c[k][1], c[k][2], c[k][3]);
    }
    return 27.0F * sum;
}

bool dispatchNoise(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                   IssueBag& issues) noexcept {
    if (valueArgCount(ins) < 1U) {
        return false;
    }
    RegisterValue arg;
    if (!readValueArg(ins, 0U, ctx, arg, issues)) {
        return false;
    }
    f32 value = 0.0F;
    switch (arg.componentCount) {
    case 1:
        value = simplexNoise1(arg.lanes[0]);
        break;
    case 2:
        value = simplexNoise2(arg.lanes[0], arg.lanes[1]);
        break;
    case 3:
        value = simplexNoise3(arg.lanes[0], arg.lanes[1], arg.lanes[2]);
        break;
    case 4:
        value = simplexNoise4(arg.lanes[0], arg.lanes[1], arg.lanes[2], arg.lanes[3]);
        break;
    default:
        return false;
    }
    out = RegisterValue{};
    out.componentCount = 1;
    out.typeBank = bank::kFloat;
    out.lanes[0] = value;
    return true;
}

enum class QuatAxis : u8 {
    Side = 0,
    Up = 1,
    Forward = 2,
};

bool dispatchOrientationAxis(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                             RegisterValue& out, IssueBag& issues, QuatAxis axis) noexcept {
    if (ins.operands[3] < 1U) {
        issues.push(vmFatal(issues::vm::kOperandCount, "IR: orientation_axis* requires arg"));
        return false;
    }
    RegisterValue q;
    if (!readFnArg(ins, 0, ctx, q, issues)) {
        return false;
    }
    const f32 qx = q.lanes[0];
    const f32 qy = q.lanes[1];
    const f32 qz = q.lanes[2];
    const f32 qw = q.lanes[3];
    f32 ax = 0.0F;
    f32 ay = 0.0F;
    f32 az = 0.0F;
    switch (axis) {
    case QuatAxis::Side:
        ax = 1.0F - 2.0F * (qy * qy + qz * qz);
        ay = 2.0F * (qx * qy + qw * qz);
        az = 2.0F * (qx * qz - qw * qy);
        break;
    case QuatAxis::Up:
        ax = 2.0F * (qx * qy - qw * qz);
        ay = 1.0F - 2.0F * (qx * qx + qz * qz);
        az = 2.0F * (qy * qz + qw * qx);
        break;
    case QuatAxis::Forward:
        ax = 2.0F * (qx * qz + qw * qy);
        ay = 2.0F * (qy * qz - qw * qx);
        az = 1.0F - 2.0F * (qx * qx + qy * qy);
        break;
    }
    out = RegisterValue{};
    out.componentCount = 3;
    out.typeBank = bank::kFloat3;
    out.lanes[0] = ax;
    out.lanes[1] = ay;
    out.lanes[2] = az;
    return true;
}

bool dispatchEffectPosition(const CBEMInstruction&, BytecodeExecContext& ctx, RegisterValue& out,
                            IssueBag&) noexcept {
    out = RegisterValue{};
    out.componentCount = 3;
    out.typeBank = bank::kFloat3;
    out.lanes[0] = ctx.sceneL2W.m[0][3];
    out.lanes[1] = ctx.sceneL2W.m[1][3];
    out.lanes[2] = ctx.sceneL2W.m[2][3];
    return true;
}

bool shapePositionFromPCoords(const SamplerShape& sh, const std::array<f32, 3>& pc, bool volume,
                              std::array<f32, 3>& outPos) noexcept;
bool readShapePCoordsArg(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                         std::array<f32, 3>& out) noexcept;
bool dispatchShapeProjectPCoordsPrimitive(const SamplerShape& sh, const std::array<f32, 3>& local,
                                          RegisterValue& out) noexcept;
std::array<f32, 3> shapeWorldToLocal(const SamplerShape& sh, const std::array<f32, 3>& p) noexcept;
std::array<f32, 3> shapeRotateDirection(const SamplerShape& sh, f32 x, f32 y, f32 z) noexcept;
void writeFloat3(RegisterValue& out, const std::array<f32, 3>& v) noexcept;

inline f32 halfToUnitBits(u32 half, f32 bias) noexcept {
    const u32 bits = ((half & 0x7FFFU) << 8U) | fpbits::kOneF32;
    f32 v = 0.0F;
    std::memcpy(&v, &bits, sizeof(f32));
    return v - bias;
}

bool readMeshPCoordsArg(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                        const MeshShapeData& mesh, MeshParametricCoords& out) noexcept {
    RegisterValue arg;
    IssueBag sink;
    if (valueArgCount(ins) < 1U || !readValueArg(ins, 0U, ctx, arg, sink) ||
        arg.componentCount < 3U) {
        return false;
    }
    u32 offset = 0U;
    u32 uBits = 0U;
    u32 vBits = 0U;
    std::memcpy(&offset, &arg.lanes[0], sizeof(u32));
    std::memcpy(&uBits, &arg.lanes[1], sizeof(u32));
    std::memcpy(&vBits, &arg.lanes[2], sizeof(u32));
    const u32 triangle = offset / 3U;
    out.triangle = triangle < mesh.triangleCount
                       ? triangle
                       : (mesh.triangleCount > 0U ? mesh.triangleCount - 1U : 0U);
    std::memcpy(&out.u, &uBits, sizeof(f32));
    std::memcpy(&out.v, &vBits, sizeof(f32));
    return true;
}

void writeMeshPCoords(const MeshParametricCoords& pc, RegisterValue& out) noexcept {
    out = RegisterValue{};
    out.componentCount = 3;
    out.typeBank = bank::kInt3;
    const u32 offset = pc.triangle * 3U;
    u32 uBits = 0U;
    u32 vBits = 0U;
    std::memcpy(&uBits, &pc.u, sizeof(u32));
    std::memcpy(&vBits, &pc.v, sizeof(u32));
    std::memcpy(&out.lanes[0], &offset, sizeof(f32));
    std::memcpy(&out.lanes[1], &uBits, sizeof(f32));
    std::memcpy(&out.lanes[2], &vBits, sizeof(f32));
}

bool dispatchSamplePosition(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                            RegisterValue& out, IssueBag& issues) noexcept {
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr) {
        return false;
    }

    if (res->kind == SamplerKind::Turbulence) {
        return sampleTurbulenceToReg(*res, ins, ctx, out, issues);
    }

    if (res->kind != SamplerKind::Shape || ctx.rng == nullptr) {
        return false;
    }
    auto drawUnit = [&]() -> f32 {
        const u32 raw = ctx.rng->advance();

        const u32 bits = (raw >> fpbits::kRandMantissaShift) | fpbits::kOneF32;
        f32 v;
        std::memcpy(&v, &bits, sizeof(f32));
        return v - 1.0F;
    };

    out = RegisterValue{};
    out.componentCount = 3;
    out.typeBank = bank::kFloat3;

    auto applyShapeTrs = [&](f32 lx, f32 ly, f32 lz) {
        const auto& sh = res->shape;

        const bool scaled = sh.type == ShapeType::ComplexEllipsoid;
        f32 sx = scaled ? lx * sh.nonUniformScale[0] : lx;
        f32 sy = scaled ? ly * sh.nonUniformScale[1] : ly;
        f32 sz = scaled ? lz * sh.nonUniformScale[2] : lz;

        if (sh.transformRotate &&
            (sh.eulerOrientation[0] != 0.0F || sh.eulerOrientation[1] != 0.0F ||
             sh.eulerOrientation[2] != 0.0F)) {
            const f32 cx = std::cos(sh.eulerOrientation[0]);
            const f32 sxn = std::sin(sh.eulerOrientation[0]);
            const f32 cy = std::cos(sh.eulerOrientation[1]);
            const f32 syn = std::sin(sh.eulerOrientation[1]);
            const f32 cz = std::cos(sh.eulerOrientation[2]);
            const f32 szn = std::sin(sh.eulerOrientation[2]);

            const f32 ix = sx;
            const f32 iy = sz;
            const f32 iz = -sy;
            const f32 x1 = ix;
            const f32 y1 = cx * iy - sxn * iz;
            const f32 z1 = sxn * iy + cx * iz;
            const f32 x2 = cy * x1 + syn * z1;
            const f32 y2 = y1;
            const f32 z2 = -syn * x1 + cy * z1;
            const f32 x3 = cz * x2 - szn * y2;
            const f32 y3 = szn * x2 + cz * y2;
            const f32 z3 = z2;
            sx = x3;
            sy = -z3;
            sz = y3;
        }
        if (sh.transformTranslate) {
            sx += sh.position[0];
            sy += sh.position[1];
            sz += sh.position[2];
        }
        out.lanes[0] = sx;
        out.lanes[1] = sy;
        out.lanes[2] = sz;
    };

    constexpr f32 kTwoPi = 6.28318530717958647692F;
    constexpr f32 kPi = 3.14159265358979323846F;

    const bool volume = res->shape.dimensionality == SampleDimensionality::Volume;

    auto drawU32 = [&]() -> u32 { return ctx.rng->advance(); };
    auto halfToUnit = [](u32 half, f32 bias) { return halfToUnitBits(half, bias); };

    auto sphereVolumePoint = [&](f32 outerR, f32 innerR) {
        const f32 angle = drawUnit() * kTwoPi;
        const u32 packed = drawU32();
        const f32 cosTheta = 1.0F - 2.0F * halfToUnit(packed, 1.0F);
        const f32 sinTheta = std::sqrt(std::max(0.0F, 1.0F - cosTheta * cosTheta));
        const f32 r3min = innerR * innerR * innerR;
        const f32 r3max = outerR * outerR * outerR;
        const f32 r = std::cbrt(r3min + (r3max - r3min) * halfToUnit(packed >> 16U, 1.0F));
        return std::array<f32, 3>{r * sinTheta * std::cos(angle), r * sinTheta * std::sin(angle),
                                  r * cosTheta};
    };
    auto sphereSurfacePoint = [&](f32 outerR, f32 innerR) {
        const f32 angle = drawUnit() * kTwoPi;
        const u32 packed = drawU32();
        const f32 cosTheta = 1.0F - 2.0F * halfToUnit(packed, 1.0F);
        const f32 sinTheta = std::sqrt(std::max(0.0F, 1.0F - cosTheta * cosTheta));
        f32 shell = outerR;
        if (innerR > 0.0F) {
            const f32 outerA = outerR * outerR;
            const f32 total = outerA + innerR * innerR;
            if (total > 0.0F && halfToUnit(packed >> 16U, 1.0F) * total >= outerA) {
                shell = innerR;
            }
        }
        return std::array<f32, 3>{shell * sinTheta * std::cos(angle),
                                  shell * sinTheta * std::sin(angle), shell * cosTheta};
    };

    auto cylinderRadius = [](f32 outerR, f32 innerR, f32 u) {
        const f32 expo = 0.5F * ((outerR > 0.0F ? innerR / outerR : 0.0F) + 1.0F);
        return std::pow(u, expo) * (outerR - innerR) + innerR;
    };
    auto cylinderVolumePoint = [&](f32 outerR, f32 innerR, f32 height) {
        const f32 angle = drawUnit() * kTwoPi;
        const u32 packed = drawU32();
        const f32 h = halfToUnit(packed, 1.5F) * height;
        const f32 r = cylinderRadius(outerR, innerR, halfToUnit(packed >> 16U, 1.0F));
        return std::array<f32, 3>{r * std::sin(angle), r * std::cos(angle), h};
    };
    auto cylinderSurfacePoint = [&](f32 outerR, f32 innerR, f32 height) {
        const f32 angle = drawUnit() * kTwoPi;
        f32 shell = outerR;
        if (innerR > 0.0F && (outerR + innerR) > 0.0F) {
            const f32 innerSurfaceSelector = innerR / (outerR + innerR);
            shell = drawUnit() > innerSurfaceSelector ? outerR : innerR;
        }
        const f32 h = height * drawUnit() - 0.5F * height;
        return std::array<f32, 3>{shell * std::sin(angle), shell * std::cos(angle), h};
    };

    if (res->shape.type != ShapeType::Mesh) {
        std::array<f32, 3> pc{};
        if (readShapePCoordsArg(ins, ctx, pc)) {
            std::array<f32, 3> p{};
            if (!shapePositionFromPCoords(res->shape, pc, volume, p)) {
                return false;
            }
            applyShapeTrs(p[0], p[1], p[2]);
            return true;
        }
    }

    switch (res->shape.type) {
    case ShapeType::Mesh: {
        const MeshShapeData* mesh = res->shape.mesh;
        if (mesh == nullptr || mesh->triangleCount == 0U) {
            return false;
        }
        MeshParametricCoords pc;
        if (readMeshPCoordsArg(ins, ctx, *mesh, pc)) {
            std::array<f32, 4> p{};
            if (sampleMeshField(*mesh, MeshField::Position, -1, pc, p.data()) == 0U) {
                return false;
            }
            applyShapeTrs(p[0] * res->shape.meshScale[0], p[1] * res->shape.meshScale[1],
                          p[2] * res->shape.meshScale[2]);
            return true;
        }
        if (res->shape.dimensionality == SampleDimensionality::Vertex && mesh->vertexCount > 0U) {
            const auto vi = static_cast<u32>(drawUnit() * static_cast<f32>(mesh->vertexCount));
            const std::size_t o =
                static_cast<std::size_t>(std::min(vi, mesh->vertexCount - 1U)) * 3U;
            if (o + 3U <= mesh->positions.size()) {
                applyShapeTrs(mesh->positions[o] * res->shape.meshScale[0],
                              mesh->positions[o + 1U] * res->shape.meshScale[1],
                              mesh->positions[o + 2U] * res->shape.meshScale[2]);
                return true;
            }
        }
        if (volume) {
            return false;
        }
        const auto mode = static_cast<MeshSamplingDistribution>(res->shape.meshSamplingMode);
        const f32 r0 = drawUnit();
        const f32 r1 = drawUnit();
        const f32 r2 = drawUnit();
        const f32 r3 = meshSampleRandomCount(mode) > 3U ? drawUnit() : 0.0F;
        const MeshSurfaceSample s = sampleMeshSurface(*mesh, mode, r0, r1, r2, r3);
        applyShapeTrs(s.position[0] * res->shape.meshScale[0],
                      s.position[1] * res->shape.meshScale[1],
                      s.position[2] * res->shape.meshScale[2]);
        return true;
    }
    case ShapeType::Capsule: {
        const f32 outerR = res->shape.radius;
        const f32 innerR = res->shape.innerRadius;
        const f32 height = res->shape.height;
        f32 cylPart = 0.0F;
        f32 sphPart = 0.0F;
        if (volume) {
            cylPart = kPi * std::max(0.0F, outerR * outerR - innerR * innerR) * height;
            sphPart = (4.0F / 3.0F) * kPi *
                      std::max(0.0F, outerR * outerR * outerR - innerR * innerR * innerR);
        } else {
            cylPart = kTwoPi * (outerR + innerR) * height;
            sphPart = 2.0F * kTwoPi * (outerR * outerR + (innerR > 0.0F ? innerR * innerR : 0.0F));
        }
        const f32 total = cylPart + sphPart;
        if (total > 0.0F && drawUnit() * total < cylPart) {
            const auto p = volume ? cylinderVolumePoint(outerR, innerR, height)
                                  : cylinderSurfacePoint(outerR, innerR, height);
            applyShapeTrs(p[0], p[1], p[2]);
        } else {
            auto p =
                volume ? sphereVolumePoint(outerR, innerR) : sphereSurfacePoint(outerR, innerR);
            p[2] += (p[2] >= 0.0F ? 0.5F : -0.5F) * height;
            applyShapeTrs(p[0], p[1], p[2]);
        }
        return true;
    }
    case ShapeType::Sphere:
    case ShapeType::ComplexEllipsoid: {
        auto p = volume ? sphereVolumePoint(res->shape.radius, res->shape.innerRadius)
                        : sphereSurfacePoint(res->shape.radius, res->shape.innerRadius);
        if (res->shape.type == ShapeType::ComplexEllipsoid && res->shape.hemisphere &&
            p[2] < 0.0F) {
            p[2] = -p[2];
        }
        applyShapeTrs(p[0], p[1], p[2]);
        return true;
    }
    case ShapeType::Box: {
        const std::array<f32, 3> dim = res->shape.boxDimensions;
        const std::array<f32, 3> extent{dim[0] * 0.5F, dim[1] * 0.5F, dim[2] * 0.5F};
        const f32 r0 = drawUnit() * 2.0F - 1.0F;
        const f32 r1 = drawUnit() * 2.0F - 1.0F;
        const f32 r2 = drawUnit() * 2.0F - 1.0F;

        const f32 surf0 = dim[1] * dim[2];
        const f32 surf1 = dim[2] * dim[0];
        const f32 surf2 = dim[0] * dim[1];
        const f32 total = surf0 + surf1 + surf2;
        if (volume || total == 0.0F) {
            applyShapeTrs(r0 * extent[0], r1 * extent[1], r2 * extent[2]);
            return true;
        }

        u32 r2Bits = 0U;
        std::memcpy(&r2Bits, &r2, sizeof(u32));
        const u32 signFlip = r2Bits & 0x80000000U;
        const u32 surfBits = r2Bits & 0x7FFFFFFFU;
        const f32 limit0 = surf0 / total;
        const f32 limit1 = (surf0 + surf1) / total;
        u32 limit0Bits = 0U;
        u32 limit1Bits = 0U;
        std::memcpy(&limit0Bits, &limit0, sizeof(u32));
        std::memcpy(&limit1Bits, &limit1, sizeof(u32));
        const u32 id0 = ((limit1Bits - surfBits) >> 31U) + ((limit0Bits - surfBits) >> 31U);
        static constexpr std::array<u32, 4> kWrapLookup{1U, 2U, 0U, 1U};
        const u32 id1 = kWrapLookup[id0];
        const u32 id2 = kWrapLookup[id0 + 1U];

        std::array<f32, 3> p{};
        u32 pinned = 0U;
        std::memcpy(&pinned, &extent[id0], sizeof(u32));
        pinned ^= signFlip;
        std::memcpy(&p[id0], &pinned, sizeof(f32));
        p[id1] = r0 * extent[id1];
        p[id2] = r1 * extent[id2];
        applyShapeTrs(p[0], p[1], p[2]);
        return true;
    }
    case ShapeType::Cylinder: {
        const auto p = volume ? cylinderVolumePoint(res->shape.radius, res->shape.innerRadius,
                                                    res->shape.height)
                              : cylinderSurfacePoint(res->shape.radius, res->shape.innerRadius,
                                                     res->shape.height);
        applyShapeTrs(p[0], p[1], p[2]);
        return true;
    }
    case ShapeType::Cone: {
        const f32 radius = res->shape.radius;
        const f32 height = res->shape.height;
        const f32 angle = drawUnit() * kTwoPi;
        if (volume) {
            const f32 v = std::cbrt(drawUnit());
            const f32 r = radius * v * std::sqrt(drawUnit());
            applyShapeTrs(r * std::sin(angle), r * std::cos(angle), height * (1.0F - v));
            return true;
        }
        const f32 s = std::sqrt(drawUnit());
        applyShapeTrs(radius * s * std::sin(angle), radius * s * std::cos(angle),
                      height * (1.0F - s));
        return true;
    }
    default:

        return false;
    }
}

bool sampleMeshChannel(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                       MeshField field, i32 streamIndex) noexcept {
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr || res->kind != SamplerKind::Shape || res->shape.type != ShapeType::Mesh ||
        res->shape.mesh == nullptr) {
        return false;
    }
    const MeshShapeData& mesh = *res->shape.mesh;
    if (mesh.triangleCount == 0U) {
        return false;
    }

    MeshParametricCoords pc;
    if (!readMeshPCoordsArg(ins, ctx, mesh, pc)) {
        if (ctx.rng == nullptr) {
            return false;
        }
        auto drawUnit = [&]() -> f32 {
            const u32 raw = ctx.rng->advance();
            const u32 bits = (raw >> fpbits::kRandMantissaShift) | fpbits::kOneF32;
            f32 v;
            std::memcpy(&v, &bits, sizeof(f32));
            return v - 1.0F;
        };
        const auto mode = static_cast<MeshSamplingDistribution>(res->shape.meshSamplingMode);
        const f32 r0 = drawUnit();
        const f32 r1 = drawUnit();
        const f32 r2 = drawUnit();
        const f32 r3 = meshSampleRandomCount(mode) > 3U ? drawUnit() : 0.0F;
        const MeshSurfaceSample s = sampleMeshSurface(mesh, mode, r0, r1, r2, r3);
        pc = MeshParametricCoords{s.triangle, s.u, s.v};
    }

    std::array<f32, 4> value{};
    const u8 comps = sampleMeshField(mesh, field, streamIndex, pc, value.data());
    if (comps == 0U) {
        return false;
    }
    out = RegisterValue{};
    out.componentCount = comps;
    out.typeBank = comps == 2U ? bank::kFloat2 : (comps == 4U ? bank::kFloat4 : bank::kFloat3);
    for (u8 c = 0U; c < comps; ++c) {
        out.lanes[c] = value[c];
    }
    return true;
}

std::array<f32, 3> shapeRotateDirection(const SamplerShape& sh, f32 x, f32 y, f32 z) noexcept {
    if (sh.eulerOrientation[0] == 0.0F && sh.eulerOrientation[1] == 0.0F &&
        sh.eulerOrientation[2] == 0.0F) {
        return {x, y, z};
    }
    const f32 cx = std::cos(sh.eulerOrientation[0]);
    const f32 sx = std::sin(sh.eulerOrientation[0]);
    const f32 cy = std::cos(sh.eulerOrientation[1]);
    const f32 sy = std::sin(sh.eulerOrientation[1]);
    const f32 cz = std::cos(sh.eulerOrientation[2]);
    const f32 sz = std::sin(sh.eulerOrientation[2]);
    const f32 ix = x;
    const f32 iy = z;
    const f32 iz = -y;
    const f32 y1 = cx * iy - sx * iz;
    const f32 z1 = sx * iy + cx * iz;
    const f32 x2 = cy * ix + sy * z1;
    const f32 z2 = -sy * ix + cy * z1;
    const f32 x3 = cz * x2 - sz * y1;
    const f32 y3 = sz * x2 + cz * y1;
    return {x3, -z2, y3};
}

std::array<f32, 3> shapeAxisColumn(const SamplerShape& sh, u32 axis) noexcept {
    return shapeRotateDirection(sh, axis == 0U ? 1.0F : 0.0F, axis == 1U ? 1.0F : 0.0F,
                                axis == 2U ? 1.0F : 0.0F);
}

void writeScalar(RegisterValue& out, f32 v) noexcept {
    out = RegisterValue{};
    out.componentCount = 1;
    out.typeBank = bank::kFloat;
    out.lanes[0] = v;
}

void writeInt(RegisterValue& out, i32 v) noexcept {
    out = RegisterValue{};
    out.componentCount = 1;
    out.typeBank = bank::kPtr;
    std::memcpy(&out.lanes[0], &v, sizeof(v));
}

void writeFloat3(RegisterValue& out, const std::array<f32, 3>& v) noexcept {
    out = RegisterValue{};
    out.componentCount = 3;
    out.typeBank = bank::kFloat3;
    out.lanes[0] = v[0];
    out.lanes[1] = v[1];
    out.lanes[2] = v[2];
}

constexpr f32 kShapePi = 3.14159265358979323846F;

f32 shapeSurface(const SamplerShape& sh) noexcept {
    const f32 r = sh.radius;
    const f32 ir = sh.innerRadius;
    const f32 h = sh.height;
    switch (sh.type) {
    case ShapeType::Box: {
        const auto& d = sh.boxDimensions;
        return 2.0F * (d[0] * (d[1] + d[2]) + d[1] * d[2]);
    }
    case ShapeType::Sphere: {
        f32 s = 4.0F * kShapePi * r * r;
        if (ir > 0.0F) {
            s += 4.0F * kShapePi * ir * ir;
        }
        return s;
    }
    case ShapeType::ComplexEllipsoid: {
        const f32 cutPlane = sh.hemisphere ? 0.5F : 1.0F;
        f32 s = 4.0F * kShapePi * r * r * cutPlane;
        if (ir > 0.0F) {
            s += 4.0F * kShapePi * ir * ir * cutPlane;
        }
        return s;
    }
    case ShapeType::Cylinder:
        return 2.0F * kShapePi * (r * (h + r) + ir * (h - ir));
    case ShapeType::Capsule:
        return 2.0F * kShapePi * (r + ir) * h + 4.0F * kShapePi * r * r +
               (ir > 0.0F ? 4.0F * kShapePi * ir * ir : 0.0F);
    case ShapeType::Cone:
        return kShapePi * r * (std::sqrt(r * r + h * h) + r);
    case ShapeType::Mesh:
        return sh.mesh != nullptr ? sh.mesh->surfaceArea : 0.0F;
    }
    return 0.0F;
}

f32 shapeVolume(const SamplerShape& sh) noexcept {
    const f32 r = sh.radius;
    const f32 ir = sh.innerRadius;
    const f32 h = sh.height;
    auto sphereVol = [](f32 rad) { return (4.0F / 3.0F) * kShapePi * rad * rad * rad; };
    auto cylVol = [](f32 rad, f32 hh) { return kShapePi * rad * rad * hh; };
    switch (sh.type) {
    case ShapeType::Box: {
        const auto& d = sh.boxDimensions;
        return d[0] * d[1] * d[2];
    }
    case ShapeType::Sphere:
        return sphereVol(r) - (ir > 0.0F ? sphereVol(ir) : 0.0F);
    case ShapeType::ComplexEllipsoid: {
        const f32 cutPlane = sh.hemisphere ? 0.5F : 1.0F;
        const f32 prod =
            sh.nonUniformScale[0] * sh.nonUniformScale[1] * sh.nonUniformScale[2];
        const f32 v = sphereVol(r) - (ir > 0.0F ? sphereVol(ir) : 0.0F);
        return v * cutPlane * prod;
    }
    case ShapeType::Cylinder:
        return cylVol(r, h) - (ir > 0.0F ? cylVol(ir, h) : 0.0F);
    case ShapeType::Capsule:
        return cylVol(r, h) - (ir > 0.0F ? cylVol(ir, h) : 0.0F) + sphereVol(r) -
               (ir > 0.0F ? sphereVol(ir) : 0.0F);
    case ShapeType::Cone:
        return (1.0F / 3.0F) * kShapePi * r * r * h;
    case ShapeType::Mesh:
        return sh.mesh != nullptr ? sh.mesh->volume : 0.0F;
    }
    return 0.0F;
}

struct AbsoluteAxisMap {
    u32 column;
    f32 sign;
};

constexpr AbsoluteAxisMap kAbsoluteAxisMap[6] = {
    {0U, -1.0F},
    {0U, +1.0F},
    {2U, -1.0F},
    {2U, +1.0F},
    {1U, -1.0F},
    {1U, +1.0F},
};

bool dispatchShapeGetterSym(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                            RegisterValue& out, IssueBag&, std::string_view symbol) noexcept {
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr || res->kind != SamplerKind::Shape) {
        return false;
    }
    const SamplerShape& sh = res->shape;

    if (symbol == "surface") {
        writeScalar(out, shapeSurface(sh));
        return true;
    }
    if (symbol == "volume") {
        writeScalar(out, shapeVolume(sh));
        return true;
    }
    if (symbol == "radius") {
        writeScalar(out, sh.radius);
        return true;
    }
    if (symbol == "innerRadius") {
        writeScalar(out, sh.innerRadius);
        return true;
    }
    if (symbol == "height") {
        writeScalar(out, sh.height);
        return true;
    }
    if (symbol == "vertexCount") {
        writeInt(out, sh.mesh != nullptr ? static_cast<i32>(sh.mesh->vertexCount) : 0);
        return true;
    }
    if (symbol == "triangleCount") {
        writeInt(out, sh.mesh != nullptr ? static_cast<i32>(sh.mesh->triangleCount) : 0);
        return true;
    }
    if (symbol == "tetraCount") {
        writeInt(out, 0);
        return true;
    }
    if (symbol == "meshScale") {
        writeFloat3(out, sh.meshScale);
        return true;
    }
    if (symbol == "boxDim") {
        writeFloat3(out, sh.boxDimensions);
        return true;
    }
    if (symbol == "type") {
        writeInt(out, static_cast<i32>(sh.type));
        return true;
    }
    if (symbol == "position") {
        writeFloat3(out, sh.position);
        return true;
    }
    if (symbol == "axisSide") {
        writeFloat3(out, shapeAxisColumn(sh, 0U));
        return true;
    }
    if (symbol == "axisVertical") {
        writeFloat3(out, shapeAxisColumn(sh, 2U));
        return true;
    }
    if (symbol == "axisDepth") {
        writeFloat3(out, shapeAxisColumn(sh, 1U));
        return true;
    }
    static constexpr std::string_view kAxisNames[6] = {"axisLeft",     "axisRight", "axisDown",
                                                       "axisUp",       "axisBackward",
                                                       "axisForward"};
    for (u32 i = 0U; i < 6U; ++i) {
        if (symbol == kAxisNames[i]) {
            const auto col = shapeAxisColumn(sh, kAbsoluteAxisMap[i].column);
            const f32 s = kAbsoluteAxisMap[i].sign;
            writeFloat3(out, {col[0] * s, col[1] * s, col[2] * s});
            return true;
        }
    }
    return false;
}

bool dispatchProjectPCoords(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                            RegisterValue& out, IssueBag& issues) noexcept {
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr || res->kind != SamplerKind::Shape || valueArgCount(ins) < 1U) {
        return false;
    }
    RegisterValue arg;
    if (!readValueArg(ins, 0U, ctx, arg, issues)) {
        return false;
    }
    const auto& sh = res->shape;
    const auto local = shapeWorldToLocal(sh, {arg.lanes[0], arg.lanes[1], arg.lanes[2]});
    if (sh.type != ShapeType::Mesh) {
        return dispatchShapeProjectPCoordsPrimitive(sh, local, out);
    }
    if (sh.mesh == nullptr) {
        return false;
    }
    const auto divide = [](f32 v, f32 d) { return d != 0.0F ? v / d : 0.0F; };
    const std::array<f32, 3> meshSpace{
        divide(divide(local[0], sh.nonUniformScale[0]), sh.meshScale[0]),
        divide(divide(local[1], sh.nonUniformScale[1]), sh.meshScale[1]),
        divide(divide(local[2], sh.nonUniformScale[2]), sh.meshScale[2]),
    };
    MeshParametricCoords pc;
    if (!projectMeshPointEngine(*sh.mesh, meshSpace, pc) &&
        !projectMeshPoint(*sh.mesh, meshSpace, pc)) {
        return false;
    }
    writeMeshPCoords(pc, out);
    return true;
}

bool primitiveClearedChannel(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                             RegisterValue& out, u8 typeBank, u8 components) noexcept {
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr || res->kind != SamplerKind::Shape || res->shape.type == ShapeType::Mesh) {
        return false;
    }
    out = RegisterValue{};
    out.componentCount = components;
    out.typeBank = typeBank;
    return true;
}

bool dispatchSampleNormal(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                          IssueBag& issues) noexcept {
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res != nullptr && res->kind == SamplerKind::Shape && res->shape.type != ShapeType::Mesh) {
        if (!dispatchSamplePosition(ins, ctx, out, issues)) {
            return false;
        }
        const SamplerShape& sh = res->shape;
        const auto local = shapeWorldToLocal(sh, {out.lanes[0], out.lanes[1], out.lanes[2]});
        const auto n = shapeSurfaceNormal(sh, local);
        writeFloat3(out, sh.transformRotate ? shapeRotateDirection(sh, n[0], n[1], n[2]) : n);
        return true;
    }
    return sampleMeshChannel(ins, ctx, out, MeshField::Normal, -1);
}

bool dispatchSampleTangent(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                           IssueBag&) noexcept {
    if (primitiveClearedChannel(ins, ctx, out, bank::kFloat4, 4)) {
        return true;
    }
    return sampleMeshChannel(ins, ctx, out, MeshField::Tangent, -1);
}

bool dispatchSampleVelocity(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                            RegisterValue& out, IssueBag&) noexcept {
    if (primitiveClearedChannel(ins, ctx, out, bank::kFloat3, 3)) {
        return true;
    }
    return sampleMeshChannel(ins, ctx, out, MeshField::Velocity, -1);
}

bool dispatchSampleChannelSym(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                              RegisterValue& out, IssueBag&, std::string_view symbol) noexcept {
    const bool isColor = symbol.rfind("sampleColor", 0) == 0U;
    const std::size_t baseLen =
        isColor ? sizeof("sampleColor") - 1U : sizeof("sampleTexcoord") - 1U;
    i32 stream = -1;
    if (symbol.size() == baseLen + 1U) {
        const char c = symbol[baseLen];
        if (c < '0' || c > '9') {
            return false;
        }
        stream = c - '0';
    } else if (symbol.size() != baseLen) {
        return false;
    }
    const bool isFloat4 = isColor && stream < 0;
    if (primitiveClearedChannel(ins, ctx, out, isFloat4 ? bank::kFloat4 : bank::kFloat2,
                                isFloat4 ? 4 : 2)) {
        return true;
    }
    return sampleMeshChannel(ins, ctx, out, isColor ? MeshField::Color : MeshField::Texcoord,
                             stream);
}

std::array<f32, 3> shapeUnrotateDirection(const SamplerShape& sh, f32 x, f32 y, f32 z) noexcept {
    if (sh.eulerOrientation[0] == 0.0F && sh.eulerOrientation[1] == 0.0F &&
        sh.eulerOrientation[2] == 0.0F) {
        return {x, y, z};
    }
    const f32 cx = std::cos(sh.eulerOrientation[0]);
    const f32 sx = std::sin(sh.eulerOrientation[0]);
    const f32 cy = std::cos(sh.eulerOrientation[1]);
    const f32 sy = std::sin(sh.eulerOrientation[1]);
    const f32 cz = std::cos(sh.eulerOrientation[2]);
    const f32 sz = std::sin(sh.eulerOrientation[2]);
    const f32 ix = x;
    const f32 iy = z;
    const f32 iz = -y;
    const f32 x1 = cz * ix + sz * iy;
    const f32 y1 = -sz * ix + cz * iy;
    const f32 x2 = cy * x1 - sy * iz;
    const f32 z2 = sy * x1 + cy * iz;
    const f32 y3 = cx * y1 + sx * z2;
    const f32 z3 = -sx * y1 + cx * z2;
    return {x2, -z3, y3};
}

std::array<f32, 3> shapeWorldToLocal(const SamplerShape& sh, const std::array<f32, 3>& p) noexcept {
    f32 x = p[0];
    f32 y = p[1];
    f32 z = p[2];
    if (sh.transformTranslate) {
        x -= sh.position[0];
        y -= sh.position[1];
        z -= sh.position[2];
    }
    if (!sh.transformRotate) {
        return {x, y, z};
    }
    return shapeUnrotateDirection(sh, x, y, z);
}

std::array<f32, 3> shapeLocalToWorld(const SamplerShape& sh,
                                     const std::array<f32, 3>& p) noexcept {
    f32 x = p[0];
    f32 y = p[1];
    f32 z = p[2];
    if (sh.transformRotate) {
        const auto r = shapeRotateDirection(sh, x, y, z);
        x = r[0];
        y = r[1];
        z = r[2];
    }
    if (sh.transformTranslate) {
        x += sh.position[0];
        y += sh.position[1];
        z += sh.position[2];
    }
    return {x, y, z};
}

bool readPoint3(const CBEMInstruction& ins, u32 idx, BytecodeExecContext& ctx,
                std::array<f32, 3>& out) noexcept {
    RegisterValue arg;
    IssueBag sink;
    if (!readValueArg(ins, idx, ctx, arg, sink)) {
        return false;
    }
    out = {arg.lanes[0], arg.lanes[1], arg.lanes[2]};
    return true;
}

bool dispatchShapeContains(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                           IssueBag&) noexcept {
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr || res->kind != SamplerKind::Shape) {
        return false;
    }
    std::array<f32, 3> world{};
    if (!readPoint3(ins, 0U, ctx, world)) {
        return false;
    }
    const bool inside = shapeContains(res->shape, shapeWorldToLocal(res->shape, world));
    out = RegisterValue{};
    out.componentCount = 1;
    out.typeBank = bank::kBool;
    const u32 mask = inside ? 0xFFFFFFFFU : 0U;
    std::memcpy(&out.lanes[0], &mask, sizeof(mask));
    return true;
}

bool dispatchShapeDistanceField(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                                RegisterValue& out, IssueBag&) noexcept {
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr || res->kind != SamplerKind::Shape) {
        return false;
    }
    std::array<f32, 3> world{};
    if (!readPoint3(ins, 0U, ctx, world)) {
        return false;
    }
    writeScalar(out, shapeDistanceField(res->shape, shapeWorldToLocal(res->shape, world)));
    return true;
}

bool dispatchShapeProject(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                          IssueBag&) noexcept {
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr || res->kind != SamplerKind::Shape) {
        return false;
    }
    std::array<f32, 3> world{};
    if (!readPoint3(ins, 0U, ctx, world)) {
        return false;
    }
    const SamplerShape& sh = res->shape;
    if (sh.type == ShapeType::Mesh) {
        const MeshShapeData* mesh = sh.mesh;
        if (mesh == nullptr || mesh->triangleCount == 0U) {
            return false;
        }
        const auto local = shapeWorldToLocal(sh, world);
        const std::array<f32, 3> meshSpace{
            sh.meshScale[0] != 0.0F ? local[0] / sh.meshScale[0] : local[0],
            sh.meshScale[1] != 0.0F ? local[1] / sh.meshScale[1] : local[1],
            sh.meshScale[2] != 0.0F ? local[2] / sh.meshScale[2] : local[2]};
        MeshParametricCoords pc{};
        if (!projectMeshPointEngine(*mesh, meshSpace, pc) &&
            !projectMeshPoint(*mesh, meshSpace, pc)) {
            return false;
        }
        std::array<f32, 4> p{};
        if (sampleMeshField(*mesh, MeshField::Position, -1, pc, p.data()) == 0U) {
            return false;
        }
        const auto scaled = std::array<f32, 3>{p[0] * sh.meshScale[0], p[1] * sh.meshScale[1],
                                               p[2] * sh.meshScale[2]};
        writeFloat3(out, shapeLocalToWorld(sh, scaled));
        return true;
    }
    const auto local = shapeWorldToLocal(sh, world);
    writeFloat3(out, shapeLocalToWorld(sh, shapeProject(sh, local)));
    return true;
}

bool dispatchShapeIntersect(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                            RegisterValue& out, IssueBag&) noexcept {
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr || res->kind != SamplerKind::Shape) {
        return false;
    }
    std::array<f32, 3> origin{};
    std::array<f32, 3> dir{};
    if (!readPoint3(ins, 0U, ctx, origin) || !readPoint3(ins, 1U, ctx, dir)) {
        return false;
    }
    RegisterValue lenReg;
    RegisterValue twoSidedReg;
    IssueBag sink;
    const f32 length = readValueArg(ins, 2U, ctx, lenReg, sink) ? lenReg.lanes[0] : 1.0F;
    const bool twoSided =
        readValueArg(ins, 3U, ctx, twoSidedReg, sink) && twoSidedReg.lanes[0] != 0.0F;

    const SamplerShape& sh = res->shape;
    const auto localO = shapeWorldToLocal(sh, origin);
    const auto localD =
        sh.transformRotate ? shapeUnrotateDirection(sh, dir[0], dir[1], dir[2]) : dir;

    out = RegisterValue{};
    out.componentCount = 4;
    out.typeBank = bank::kFloat4;
    f32 t = 0.0F;
    if (!shapeIntersect(sh, localO, localD, length, twoSided, t)) {
        return true;
    }
    const std::array<f32, 3> hitLocal{localO[0] + localD[0] * length * t,
                                      localO[1] + localD[1] * length * t,
                                      localO[2] + localD[2] * length * t};
    const auto hit = shapeLocalToWorld(sh, hitLocal);
    out.lanes[0] = hit[0];
    out.lanes[1] = hit[1];
    out.lanes[2] = hit[2];
    out.lanes[3] = t;
    return true;
}

u32 unitToHalf(f32 v) noexcept {
    f32 maxOne = 0.0F;
    const u32 maxOneBits = 0x3FFFFFFFU;
    std::memcpy(&maxOne, &maxOneBits, sizeof(f32));
    const f32 one = std::min(v + 1.0F, maxOne);
    u32 bits = 0U;
    std::memcpy(&bits, &one, sizeof(u32));
    return (bits & 0x00FFFF00U) >> 8U;
}

u32 packHalves(f32 low, f32 high) noexcept {
    return (unitToHalf(high) << 16U) | unitToHalf(low);
}

bool readShapePCoordsArg(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                         std::array<f32, 3>& out) noexcept {
    RegisterValue arg;
    IssueBag sink;
    if (valueArgCount(ins) < 1U || !readValueArg(ins, 0U, ctx, arg, sink) ||
        arg.componentCount < 3U) {
        return false;
    }
    out = {arg.lanes[0], arg.lanes[1], arg.lanes[2]};
    return true;
}

void writeShapePCoords(RegisterValue& out, const std::array<f32, 3>& pc) noexcept {
    out = RegisterValue{};
    out.componentCount = 3;
    out.typeBank = bank::kInt3;
    out.lanes[0] = pc[0];
    out.lanes[1] = pc[1];
    out.lanes[2] = pc[2];
}

std::array<f32, 7> boxFaceLimits(const std::array<f32, 3>& dim) noexcept {
    const f32 lr = dim[1] * dim[2];
    const f32 tb = dim[2] * dim[0];
    const f32 fb = dim[0] * dim[1];
    const f32 total = lr + tb + fb;
    if (total <= 0.0F) {
        return {-1.0F, -0.5F, -0.1F, 0.1F, 0.1F, 0.5F, 1.0F};
    }
    const f32 midX = (lr * 0.5F) / total;
    const f32 midY = (lr + tb * 0.5F) / total;
    const f32 midZ = (lr + tb + fb * 0.5F) / total;
    return {-midZ, -midY, -midX, midX, midX, midY, midZ};
}

bool shapePositionFromPCoords(const SamplerShape& sh, const std::array<f32, 3>& pc, bool volume,
                              std::array<f32, 3>& outPos) noexcept {
    const f32 r = sh.radius;
    const f32 ir = sh.innerRadius;
    const f32 h = sh.height;
    switch (sh.type) {
    case ShapeType::Box: {
        const std::array<f32, 3> extent{sh.boxDimensions[0] * 0.5F, sh.boxDimensions[1] * 0.5F,
                                        sh.boxDimensions[2] * 0.5F};
        if (volume) {
            outPos = {pc[0] * extent[0], pc[1] * extent[1], pc[2] * extent[2]};
            return true;
        }
        const std::array<f32, 3>& d = sh.boxDimensions;
        const f32 surf0 = d[1] * d[2];
        const f32 surf1 = d[2] * d[0];
        const f32 surf2 = d[0] * d[1];
        const f32 total = surf0 + surf1 + surf2;
        if (total == 0.0F) {
            outPos = {pc[0] * extent[0], pc[1] * extent[1], pc[2] * extent[2]};
            return true;
        }
        u32 r2Bits = 0U;
        std::memcpy(&r2Bits, &pc[2], sizeof(u32));
        const u32 signFlip = r2Bits & 0x80000000U;
        const u32 surfBits = r2Bits & 0x7FFFFFFFU;
        const f32 limit0 = surf0 / total;
        const f32 limit1 = (surf0 + surf1) / total;
        u32 limit0Bits = 0U;
        u32 limit1Bits = 0U;
        std::memcpy(&limit0Bits, &limit0, sizeof(u32));
        std::memcpy(&limit1Bits, &limit1, sizeof(u32));
        const u32 id0 = ((limit1Bits - surfBits) >> 31U) + ((limit0Bits - surfBits) >> 31U);
        static constexpr std::array<u32, 4> kWrapLookup{1U, 2U, 0U, 1U};
        const u32 id1 = kWrapLookup[id0];
        const u32 id2 = kWrapLookup[id0 + 1U];
        std::array<f32, 3> p{};
        u32 pinned = 0U;
        std::memcpy(&pinned, &extent[id0], sizeof(u32));
        pinned ^= signFlip;
        std::memcpy(&p[id0], &pinned, sizeof(f32));
        p[id1] = pc[0] * extent[id1];
        p[id2] = pc[1] * extent[id2];
        outPos = p;
        return true;
    }
    case ShapeType::Sphere:
    case ShapeType::ComplexEllipsoid: {
        u32 packed = 0U;
        std::memcpy(&packed, &pc[1], sizeof(u32));
        const f32 cosTheta = 1.0F - 2.0F * halfToUnitBits(packed, 1.0F);
        const f32 sinTheta = std::sqrt(std::max(0.0F, 1.0F - cosTheta * cosTheta));
        f32 shell = r;
        if (volume) {
            const f32 r3min = ir * ir * ir;
            const f32 r3max = r * r * r;
            shell = std::cbrt(r3min +
                              (r3max - r3min) * halfToUnitBits(packed >> 16U, 1.0F));
        } else if (ir > 0.0F) {
            const f32 outerA = r * r;
            const f32 total = outerA + ir * ir;
            if (total > 0.0F && halfToUnitBits(packed >> 16U, 1.0F) * total >= outerA) {
                shell = ir;
            }
        }
        outPos = {shell * sinTheta * pc[0], shell * sinTheta * pc[2], shell * cosTheta};
        if (sh.type == ShapeType::ComplexEllipsoid && sh.hemisphere && outPos[2] < 0.0F) {
            outPos[2] = -outPos[2];
        }
        return true;
    }
    case ShapeType::Cylinder: {
        u32 packed = 0U;
        std::memcpy(&packed, &pc[1], sizeof(u32));
        const f32 z = halfToUnitBits(packed, 1.5F) * h;
        f32 rad = 0.0F;
        if (volume) {
            const f32 expo = 0.5F * ((r > 0.0F ? ir / r : 0.0F) + 1.0F);
            rad = std::pow(halfToUnitBits(packed >> 16U, 1.0F), expo) * (r - ir) + ir;
        } else {
            rad = r;
            if (ir > 0.0F && (r + ir) > 0.0F &&
                halfToUnitBits(packed >> 16U, 1.0F) <= ir / (r + ir)) {
                rad = ir;
            }
        }
        outPos = {rad * pc[0], rad * pc[2], z};
        return true;
    }
    case ShapeType::Capsule: {
        u32 packed = 0U;
        std::memcpy(&packed, &pc[1], sizeof(u32));
        const f32 z = halfToUnitBits(packed, 1.5F) * h;
        const f32 u = halfToUnitBits(packed >> 16U, 1.0F);
        const f32 rad = volume ? std::sqrt(std::max(0.0F, ir * ir + (r * r - ir * ir) * u)) : r;
        outPos = {rad * pc[0], rad * pc[2], z};
        return true;
    }
    case ShapeType::Cone: {
        if (volume) {
            outPos = {r * pc[0], r * pc[2], h * pc[1]};
            return true;
        }
        const f32 s = 1.0F - pc[1];
        outPos = {r * s * pc[0], r * s * pc[2], h * pc[1]};
        return true;
    }
    case ShapeType::Mesh:
        break;
    }
    return false;
}

bool dispatchSamplePCoords(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                           IssueBag&) noexcept {
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr || res->kind != SamplerKind::Shape || ctx.rng == nullptr) {
        return false;
    }
    const SamplerShape& sh = res->shape;
    const bool volume = sh.dimensionality == SampleDimensionality::Volume;
    auto drawUnit = [&]() -> f32 {
        const u32 raw = ctx.rng->advance();
        const u32 bits = (raw >> fpbits::kRandMantissaShift) | fpbits::kOneF32;
        f32 v = 0.0F;
        std::memcpy(&v, &bits, sizeof(f32));
        return v - 1.0F;
    };
    constexpr f32 kTwoPiLocal = 6.28318530717958647692F;

    std::array<f32, 3> pc{};
    switch (sh.type) {
    case ShapeType::Box: {
        pc = {drawUnit() * 2.0F - 1.0F, drawUnit() * 2.0F - 1.0F, drawUnit() * 2.0F - 1.0F};
        break;
    }
    case ShapeType::Sphere:
    case ShapeType::ComplexEllipsoid:
    case ShapeType::Cylinder:
    case ShapeType::Capsule: {
        const f32 angle = drawUnit() * kTwoPiLocal;
        const u32 packed = ctx.rng->advance();
        f32 asFloat = 0.0F;
        std::memcpy(&asFloat, &packed, sizeof(f32));
        pc = {std::sin(angle), asFloat, std::cos(angle)};
        break;
    }
    case ShapeType::Cone: {
        if (volume) {
            const f32 angle = drawUnit() * kTwoPiLocal;
            const f32 v = std::cbrt(drawUnit());
            const f32 t = std::sqrt(drawUnit()) * v;
            pc = {t * std::sin(angle), 1.0F - v, t * std::cos(angle)};
        } else {
            const f32 u = drawUnit();
            const f32 angle = drawUnit() * kTwoPiLocal;
            pc = {std::sin(angle), 1.0F - std::sqrt(u), std::cos(angle)};
        }
        break;
    }
    case ShapeType::Mesh: {
        const MeshShapeData* mesh = sh.mesh;
        if (mesh == nullptr || mesh->triangleCount == 0U) {
            return false;
        }
        const auto mode = static_cast<MeshSamplingDistribution>(sh.meshSamplingMode);
        const f32 r0 = drawUnit();
        const f32 r1 = drawUnit();
        const f32 r2 = drawUnit();
        const f32 r3 = meshSampleRandomCount(mode) > 3U ? drawUnit() : 0.0F;
        const MeshSurfaceSample s = sampleMeshSurface(*mesh, mode, r0, r1, r2, r3);
        writeMeshPCoords(MeshParametricCoords{s.triangle, s.u, s.v}, out);
        return true;
    }
    }
    writeShapePCoords(out, pc);
    return true;
}

bool dispatchShapeProjectPCoordsPrimitive(const SamplerShape& sh,
                                          const std::array<f32, 3>& local,
                                          RegisterValue& out) noexcept {
    const auto p = shapeProject(sh, local);
    const f32 rad = std::sqrt(p[0] * p[0] + p[1] * p[1]);
    const f32 invRad = rad > 1e-20F ? 1.0F / rad : 0.0F;
    const f32 sinA = rad > 1e-20F ? p[0] * invRad : 0.0F;
    const f32 cosA = rad > 1e-20F ? p[1] * invRad : 1.0F;
    std::array<f32, 3> pc{};
    switch (sh.type) {
    case ShapeType::Box: {
        const std::array<f32, 3> e{sh.boxDimensions[0] * 0.5F, sh.boxDimensions[1] * 0.5F,
                                   sh.boxDimensions[2] * 0.5F};
        u32 axis = 0U;
        f32 best = e[0] > 0.0F ? std::fabs(std::fabs(p[0]) - e[0]) : 1e30F;
        for (u32 c = 1U; c < 3U; ++c) {
            const f32 d = e[c] > 0.0F ? std::fabs(std::fabs(p[c]) - e[c]) : 1e30F;
            if (d < best) {
                best = d;
                axis = c;
            }
        }
        static constexpr std::array<u32, 4> kWrapLookup{1U, 2U, 0U, 1U};
        const u32 id1 = kWrapLookup[axis];
        const u32 id2 = kWrapLookup[axis + 1U];
        const auto limits = boxFaceLimits(sh.boxDimensions);
        const f32 limit = limits[p[axis] < 0.0F ? (2U - axis) : (3U + axis)];
        pc[0] = e[id1] > 0.0F ? p[id1] / e[id1] : 0.0F;
        pc[1] = e[id2] > 0.0F ? p[id2] / e[id2] : 0.0F;
        pc[2] = limit;
        break;
    }
    case ShapeType::Sphere:
    case ShapeType::ComplexEllipsoid: {
        const f32 len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        const f32 cosTheta = len > 1e-20F ? p[2] / len : 1.0F;
        const u32 packed = packHalves((1.0F - cosTheta) * 0.5F, 0.0F);
        f32 asFloat = 0.0F;
        std::memcpy(&asFloat, &packed, sizeof(f32));
        pc = {len > 1e-20F ? p[0] / (len * std::sqrt(std::max(0.0F, 1.0F - cosTheta * cosTheta)) +
                                     1e-20F)
                           : 0.0F,
              asFloat,
              len > 1e-20F ? p[1] / (len * std::sqrt(std::max(0.0F, 1.0F - cosTheta * cosTheta)) +
                                     1e-20F)
                           : 1.0F};
        break;
    }
    case ShapeType::Cylinder:
    case ShapeType::Capsule: {
        const f32 heightFrac = sh.height > 0.0F ? p[2] / sh.height + 0.5F : 0.5F;
        const f32 radialFrac = sh.radius > 0.0F ? std::clamp(rad / sh.radius, 0.0F, 1.0F) : 0.0F;
        const u32 packed = packHalves(std::clamp(heightFrac, 0.0F, 1.0F), radialFrac);
        f32 asFloat = 0.0F;
        std::memcpy(&asFloat, &packed, sizeof(f32));
        pc = {sinA, asFloat, cosA};
        break;
    }
    case ShapeType::Cone: {
        const f32 heightFrac = sh.height > 0.0F ? std::clamp(p[2] / sh.height, 0.0F, 1.0F) : 0.0F;
        pc = {sinA, heightFrac, cosA};
        break;
    }
    case ShapeType::Mesh:
        return false;
    }
    writeShapePCoords(out, pc);
    return true;
}

constexpr std::array<f32, 7> kUnitBoxFaceLimits{
    -5.0F / 6.0F, -0.5F, -1.0F / 6.0F, 1.0F / 6.0F, 1.0F / 6.0F, 0.5F, 5.0F / 6.0F};

f32 buildArgScalar(const CBEMInstruction& ins, u32 idx, BytecodeExecContext& ctx,
                   f32 fallback) noexcept {
    RegisterValue arg;
    IssueBag sink;
    if (!readValueArg(ins, idx, ctx, arg, sink)) {
        return fallback;
    }
    return arg.lanes[0];
}

bool dispatchBuildPCoordsSym(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                             RegisterValue& out, IssueBag&, std::string_view symbol) noexcept {
    constexpr f32 kTwoPiLocal = 6.28318530717958647692F;
    constexpr std::size_t kPrefix = sizeof("shape.buildPCoords") - 1U;
    if (symbol.size() <= kPrefix) {
        return false;
    }
    const std::string_view which = symbol.substr(kPrefix);
    const bool regular = which.ends_with("Regular");
    const std::string_view shapeName = regular ? which.substr(0, which.size() - 7U) : which;

    const f32 a0 = buildArgScalar(ins, 0U, ctx, 0.0F);
    const f32 a1 = buildArgScalar(ins, 1U, ctx, 0.0F);
    const f32 a2 = buildArgScalar(ins, 2U, ctx, 0.0F);
    f32 g0 = a0;
    f32 g1 = a1;
    if (regular) {
        const f32 count = a1 > 0.0F ? a1 : 1.0F;
        g0 = a0 / count;
        g1 = std::fmod(a0 * 0.6180339887F, 1.0F);
    }

    std::array<f32, 3> pc{};
    if (shapeName.starts_with("Box")) {
        if (regular) {
            pc = {g0 * 2.0F - 1.0F, g1 * 2.0F - 1.0F,
                  kUnitBoxFaceLimits[static_cast<std::size_t>(
                      std::clamp(static_cast<int>(a0) % 6 + 1, 0, 6))]};
        } else {
            const int faceId = static_cast<int>(a0);
            pc = {a1 * 2.0F - 1.0F, a2 * 2.0F - 1.0F,
                  kUnitBoxFaceLimits[static_cast<std::size_t>(std::clamp(faceId + 3, 0, 6))]};
        }
    } else if (shapeName.starts_with("Sphere")) {
        const f32 angle = (regular ? g0 : a0) * kTwoPiLocal;
        const u32 packed = packHalves(regular ? g1 : a1, regular ? 1.0F : a2);
        f32 asFloat = 0.0F;
        std::memcpy(&asFloat, &packed, sizeof(f32));
        pc = {std::cos(angle), asFloat, std::sin(angle)};
    } else if (shapeName.starts_with("Cylinder") || shapeName.starts_with("Capsule")) {
        const f32 height = regular ? g0 : a0;
        const f32 theta = regular ? g1 : a1;
        const f32 radial = regular ? 1.0F : a2;
        const f32 angle = theta * kTwoPiLocal;
        const u32 packed = packHalves(height, radial);
        f32 asFloat = 0.0F;
        std::memcpy(&asFloat, &packed, sizeof(f32));
        pc = {std::sin(angle), asFloat, std::cos(angle)};
    } else if (shapeName.starts_with("Cone")) {
        const bool volumeForm = !regular && valueArgCount(ins) >= 3U;
        const f32 heightParam = regular ? g0 : a0;
        const f32 theta = regular ? g1 : a1;
        const f32 angle = theta * kTwoPiLocal;
        if (volumeForm) {
            const f32 v = std::cbrt(std::clamp(heightParam, 0.0F, 1.0F));
            const f32 t = std::sqrt(std::clamp(a2, 0.0F, 1.0F)) * v;
            pc = {t * std::sin(angle), 1.0F - v, t * std::cos(angle)};
        } else {
            pc = {std::sin(angle), 1.0F - std::sqrt(std::clamp(heightParam, 0.0F, 1.0F)),
                  std::cos(angle)};
        }
    } else if (shapeName.starts_with("Mesh")) {
        MeshParametricCoords mpc{};
        mpc.triangle = static_cast<u32>(std::max(0.0F, a0));
        mpc.u = a1;
        mpc.v = a2;
        writeMeshPCoords(mpc, out);
        return true;
    } else {
        return false;
    }
    writeShapePCoords(out, pc);
    return true;
}

bool dispatchSampleSurfacePCoordsFromUV(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                                        RegisterValue& out, IssueBag&) noexcept {
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr || res->kind != SamplerKind::Shape ||
        res->shape.type == ShapeType::Mesh) {
        return false;
    }
    constexpr f32 kTwoPiLocal = 6.28318530717958647692F;
    const f32 u = buildArgScalar(ins, 0U, ctx, 0.0F);
    RegisterValue arg;
    IssueBag sink;
    const f32 v = readValueArg(ins, 0U, ctx, arg, sink) ? arg.lanes[1] : 0.0F;
    const SamplerShape& sh = res->shape;
    const f32 angle = u * kTwoPiLocal;
    std::array<f32, 3> pc{};
    switch (sh.type) {
    case ShapeType::Box:
        pc = {u * 2.0F - 1.0F, v * 2.0F - 1.0F, kUnitBoxFaceLimits[5]};
        break;
    case ShapeType::Cone:
        pc = {std::sin(angle), std::clamp(v, 0.0F, 1.0F), std::cos(angle)};
        break;
    case ShapeType::Sphere:
    case ShapeType::ComplexEllipsoid: {
        const u32 packed = packHalves(std::clamp(v, 0.0F, 1.0F), 1.0F);
        f32 asFloat = 0.0F;
        std::memcpy(&asFloat, &packed, sizeof(f32));
        pc = {std::cos(angle), asFloat, std::sin(angle)};
        break;
    }
    default: {
        const u32 packed = packHalves(std::clamp(v, 0.0F, 1.0F), 1.0F);
        f32 asFloat = 0.0F;
        std::memcpy(&asFloat, &packed, sizeof(f32));
        pc = {std::sin(angle), asFloat, std::cos(angle)};
        break;
    }
    }
    writeShapePCoords(out, pc);
    return true;
}

bool dispatchShapeIntersectPCoords(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                                   RegisterValue& out, IssueBag& issues) noexcept {
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr || res->kind != SamplerKind::Shape ||
        res->shape.type == ShapeType::Mesh) {
        return false;
    }
    RegisterValue hit;
    if (!dispatchShapeIntersect(ins, ctx, hit, issues)) {
        return false;
    }
    if (hit.lanes[3] == 0.0F) {
        writeShapePCoords(out, {0.0F, 0.0F, 0.0F});
        return true;
    }
    const SamplerShape& sh = res->shape;
    const auto local = shapeWorldToLocal(sh, {hit.lanes[0], hit.lanes[1], hit.lanes[2]});
    return dispatchShapeProjectPCoordsPrimitive(sh, local, out);
}

void writeInt2(RegisterValue& out, i32 x, i32 y) noexcept {
    out = RegisterValue{};
    out.componentCount = 2;
    out.typeBank = bank::kInt2Alt;
    std::memcpy(&out.lanes[0], &x, sizeof(x));
    std::memcpy(&out.lanes[1], &y, sizeof(y));
}

u32 readCameraIndex(const CBEMInstruction& ins, u32 n, BytecodeExecContext& ctx,
                    IssueBag& issues) noexcept {
    if (valueArgCount(ins) <= n) {
        return 0U;
    }
    RegisterValue arg;
    if (!readValueArg(ins, n, ctx, arg, issues)) {
        return 0U;
    }
    return static_cast<u32>(laneAsI32(arg, 0));
}

bool dispatchViewCount(const CBEMInstruction&, BytecodeExecContext& ctx, RegisterValue& out,
                       IssueBag&) noexcept {
    writeInt(out, static_cast<i32>(ctx.cameras.size()));
    return true;
}

bool dispatchViewResolution(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                            RegisterValue& out, IssueBag& issues) noexcept {
    const u32 idx = readCameraIndex(ins, 0U, ctx, issues);
    if (idx >= ctx.cameras.size()) {
        writeInt2(out, 1, 1);
        return true;
    }
    const auto& res = ctx.cameras[idx].resolution;
    writeInt2(out, res[0], res[1]);
    return true;
}

bool dispatchViewAspect(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                        IssueBag& issues) noexcept {
    const u32 idx = readCameraIndex(ins, 0U, ctx, issues);
    const std::array<i32, 2> res =
        (idx < ctx.cameras.size()) ? ctx.cameras[idx].resolution : std::array<i32, 2>{1, 1};
    writeScalar(out, static_cast<f32>(res[0]) / static_cast<f32>(res[1]));
    return true;
}

bool dispatchViewPosition(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                          IssueBag& issues) noexcept {
    const u32 idx = readCameraIndex(ins, 0U, ctx, issues);
    if (idx >= ctx.cameras.size()) {
        const f32 inf = std::numeric_limits<f32>::infinity();
        writeFloat3(out, {inf, inf, inf});
        return true;
    }
    writeFloat3(out, ctx.cameras[idx].position);
    return true;
}

bool dispatchViewDistance(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                          IssueBag& issues) noexcept {
    RegisterValue pos;
    if (!readValueArg(ins, 0U, ctx, pos, issues)) {
        return false;
    }
    const u32 idx = readCameraIndex(ins, 1U, ctx, issues);
    if (idx >= ctx.cameras.size()) {
        writeScalar(out, std::numeric_limits<f32>::infinity());
        return true;
    }
    const auto& eye = ctx.cameras[idx].position;
    const f32 dx = pos.lanes[0] - eye[0];
    const f32 dy = pos.lanes[1] - eye[1];
    const f32 dz = pos.lanes[2] - eye[2];
    writeScalar(out, std::sqrt((dx * dx) + (dy * dy) + (dz * dz)));
    return true;
}

bool dispatchViewAxis(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                      IssueBag& issues, const AbsoluteAxisMap& map) noexcept {
    const u32 idx = readCameraIndex(ins, 0U, ctx, issues);
    static constexpr SceneCamera kIdentityCamera{};
    const SceneCamera& cam = (idx < ctx.cameras.size()) ? ctx.cameras[idx] : kIdentityCamera;
    const auto& axis = cam.basis[map.column];
    writeFloat3(out, {axis[0] * map.sign, axis[1] * map.sign, axis[2] * map.sign});
    return true;
}

bool dispatchViewDirection(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                           IssueBag& issues) noexcept {
    return dispatchViewAxis(ins, ctx, out, issues, kAbsoluteAxisMap[5]);
}

bool dispatchEffectAxis(BytecodeExecContext& ctx, RegisterValue& out,
                        const AbsoluteAxisMap& map) noexcept {
    std::array<f32, 3> axis{ctx.sceneL2W.m[0][map.column], ctx.sceneL2W.m[1][map.column],
                            ctx.sceneL2W.m[2][map.column]};
    const f32 lenSq = (axis[0] * axis[0]) + (axis[1] * axis[1]) + (axis[2] * axis[2]);
    if (lenSq > 0.0F) {
        const f32 inv = 1.0F / std::sqrt(lenSq);
        axis = {axis[0] * inv, axis[1] * inv, axis[2] * inv};
    } else {
        axis = {0.0F, 0.0F, 0.0F};
        axis[map.column] = 1.0F;
    }
    writeFloat3(out, {axis[0] * map.sign, axis[1] * map.sign, axis[2] * map.sign});
    return true;
}

using FnDispatch = bool (*)(const CBEMInstruction&, BytecodeExecContext&, RegisterValue&,
                            IssueBag&);
using FnDispatchSym = bool (*)(const CBEMInstruction&, BytecodeExecContext&, RegisterValue&,
                               IssueBag&, std::string_view);

enum class FailMode : u8 { Fatal, Stub };

struct ExactDispatch {
    FnDispatch fn;
    FailMode failMode = FailMode::Fatal;
};

struct PrefixDispatch {
    std::string_view prefix;
    FnDispatch fn;
};

struct SymPrefixDispatch {
    std::string_view prefix;
    FnDispatchSym fn;
    FailMode failMode = FailMode::Fatal;
};

const std::unordered_map<std::string_view, ExactDispatch>& exactDispatchTable() {
    static const std::unordered_map<std::string_view, ExactDispatch> kTable = {
        {"rand", {dispatchRand}},
        {"vrand", {dispatchVrand}},
        {"effect.age", {dispatchEffectAge}},
        {"effect.isRunning", {dispatchEffectIsRunning}},
        {"effect.isRenderingEnabled", {dispatchEffectIsRenderingEnabled}},
        {"effect.isTeleporting", {dispatchEffectIsTeleporting}},
        {"effect.position", {dispatchEffectPosition}},
        {"sim.lod", {dispatchSimLod}},
        {"sim.lodBias", {dispatchSimLodBias}},
        {"sim.lodDistanceMin", {dispatchSimLodDistanceMin}},
        {"sim.lodDistanceMax", {dispatchSimLodDistanceMax}},
        {"duration", {dispatchDuration}},
        {"self.kill", {dispatchSelfKill}},
        {"generate", {dispatchGenerate}},
        {"trigger", {dispatchTrigger}},
        {"initPayload", {dispatchInitPayload}},
        {"kick", {dispatchKick}},
        {"hasPayloadElement", {dispatchHasPayloadElement}},
        {"sample", {dispatchSample, FailMode::Stub}},
        {"sampleCDF", {dispatchSampleCDF, FailMode::Stub}},
        {"dimensions", {dispatchTextureDimensions, FailMode::Stub}},
        {"atlasRectCount", {dispatchTextureAtlasRectCount, FailMode::Stub}},
        {"samplePosition", {dispatchSamplePosition, FailMode::Stub}},
        {"sampleNormal", {dispatchSampleNormal, FailMode::Stub}},
        {"sampleTangent", {dispatchSampleTangent, FailMode::Stub}},
        {"sampleVelocity", {dispatchSampleVelocity, FailMode::Stub}},
        {"projectPCoords", {dispatchProjectPCoords, FailMode::Stub}},
        {"xform_l2w_f_masked", {dispatchXformL2WPoint}},
        {"xform_l2w_d_masked", {dispatchXformL2WDirection}},
        {"xform_w2l_f_masked", {dispatchXformW2LPoint}},
        {"xform_w2l_d_masked", {dispatchXformW2LDirection}},
        {"allocatePayload", {dispatchAllocatePayload}},
        {"insert", {dispatchSpatialInsert}},
        {"neighborCount", {dispatchSpatialNeighborCount}},
        {"neighborCount2", {dispatchSpatialNeighborCount}},
        {"hsv2rgb", {dispatchHsv2Rgb}},
        {"rgb2hsv", {dispatchRgb2Hsv}},
        {"orientation_mult", {dispatchOrientationMult}},
        {"noise", {dispatchNoise, FailMode::Stub}},
        {"view.count", {dispatchViewCount}},
        {"view.resolution", {dispatchViewResolution}},
        {"view.aspect", {dispatchViewAspect}},
        {"view.position", {dispatchViewPosition}},
        {"view.distance", {dispatchViewDistance}},
        {"view.direction", {dispatchViewDirection}},
        {"contains", {dispatchShapeContains, FailMode::Stub}},
        {"sampleDistanceField", {dispatchShapeDistanceField, FailMode::Stub}},
        {"project", {dispatchShapeProject, FailMode::Stub}},
        {"intersect", {dispatchShapeIntersect, FailMode::Stub}},
        {"samplePCoords", {dispatchSamplePCoords, FailMode::Stub}},
        {"sampleSurfacePCoordsFromUV", {dispatchSampleSurfacePCoordsFromUV, FailMode::Stub}},
        {"intersectPCoords", {dispatchShapeIntersectPCoords, FailMode::Stub}},
    };
    return kTable;
}

constexpr PrefixDispatch kPrefixDispatch[] = {
    {"buildPayloadElement", dispatchBuildPayloadElement},
    {"appendPayload", dispatchAppendPayload},
    {"scene.intersect", dispatchSceneIntersect},
};

const std::unordered_set<std::string_view>& kShapeGetterNames() {
    static const std::unordered_set<std::string_view> kNames = {
        "surface",
        "volume",
        "radius",
        "innerRadius",
        "height",
        "vertexCount",
        "triangleCount",
        "tetraCount",
        "meshScale",
        "boxDim",
        "type",
        "position",
        "axisSide",
        "axisVertical",
        "axisDepth",
        "axisLeft",
        "axisRight",
        "axisDown",
        "axisUp",
        "axisBackward",
        "axisForward",
    };
    return kNames;
}

constexpr SymPrefixDispatch kSymPrefixDispatch[] = {
    {"shape.buildPCoords", dispatchBuildPCoordsSym, FailMode::Stub},
    {"extractPayloadElement", dispatchExtractPayloadElement},
    {"scene.orientation", dispatchSceneOrientation},
    {"closest", dispatchSpatialClosest},
    {"sumKernel", dispatchSpatialReduce, FailMode::Stub},
    {"averageKernel", dispatchSpatialReduce, FailMode::Stub},
    {"sum", dispatchSpatialReduce, FailMode::Stub},
    {"average", dispatchSpatialReduce, FailMode::Stub},
    {"sampleTexcoord", dispatchSampleChannelSym, FailMode::Stub},
    {"sampleColor", dispatchSampleChannelSym, FailMode::Stub},
};

bool isMangleToken(std::string_view tok) noexcept {
    static const std::unordered_set<std::string_view> kTokens = {
        "int",
        "int2",
        "int3",
        "int4",
        "uint",
        "uint2",
        "uint3",
        "uint4",
        "float",
        "float2",
        "float3",
        "float4",
        "bool",
        "bool2",
        "bool3",
        "bool4",
        "half",
        "half2",
        "half3",
        "half4",
        "quaternion",
        "orientation",
        "pCtxS",
        "pCtxI",
        "SceneCtx",
        "RandCtx",
        "SI",
    };
    return kTokens.find(tok) != kTokens.end();
}

std::optional<bool> dispatchSpecial(std::string_view symbol, const CBEMInstruction& ins,
                                    BytecodeExecContext& ctx, RegisterValue& out,
                                    IssueBag& issues) noexcept {
    if (symbol == "rotate" || symbol == "radians.rotate") {
        return (ins.operands[3] >= 3U) ? dispatchRotateAxisAngle(ins, ctx, out, issues)
                                       : dispatchRotateOrientation(ins, ctx, out, issues);
    }
    if (symbol == "orientation_axisSide") {
        return dispatchOrientationAxis(ins, ctx, out, issues, QuatAxis::Side);
    }
    if (symbol == "orientation_axisUp") {
        return dispatchOrientationAxis(ins, ctx, out, issues, QuatAxis::Up);
    }
    if (symbol == "orientation_axisForward") {
        return dispatchOrientationAxis(ins, ctx, out, issues, QuatAxis::Forward);
    }
    static constexpr std::string_view kViewAxisNames[6] = {
        "view.axisLeft", "view.axisRight",    "view.axisDown",
        "view.axisUp",   "view.axisBackward", "view.axisForward",
    };
    for (u32 i = 0U; i < 6U; ++i) {
        if (symbol == kViewAxisNames[i]) {
            return dispatchViewAxis(ins, ctx, out, issues, kAbsoluteAxisMap[i]);
        }
    }
    static constexpr std::string_view kEffectAxisNames[6] = {
        "effect.axisLeft", "effect.axisRight",    "effect.axisDown",
        "effect.axisUp",   "effect.axisBackward", "effect.axisForward",
    };
    for (u32 i = 0U; i < 6U; ++i) {
        if (symbol == kEffectAxisNames[i]) {
            return dispatchEffectAxis(ctx, out, kAbsoluteAxisMap[i]);
        }
    }
    if (symbol == "effect.axisSide") {
        return dispatchEffectAxis(ctx, out, AbsoluteAxisMap{0U, +1.0F});
    }
    if (symbol == "effect.axisVertical") {
        return dispatchEffectAxis(ctx, out, AbsoluteAxisMap{2U, +1.0F});
    }
    if (symbol == "effect.axisDepth") {
        return dispatchEffectAxis(ctx, out, AbsoluteAxisMap{1U, +1.0F});
    }
    return std::nullopt;
}

std::optional<bool> dispatchPlainTable(std::string_view symbol, const CBEMInstruction& ins,
                                       BytecodeExecContext& ctx, RegisterValue& out,
                                       IssueBag& issues) noexcept {
    const auto& table = exactDispatchTable();
    if (auto it = table.find(symbol); it != table.end()) {
        const auto& e = it->second;
        const bool ok = e.fn(ins, ctx, out, issues);
        if (!ok && e.failMode == FailMode::Stub) {
            return std::nullopt;
        }
        return ok;
    }
    for (const auto& e : kPrefixDispatch) {
        if (symbol.starts_with(e.prefix)) {
            return e.fn(ins, ctx, out, issues);
        }
    }
    return std::nullopt;
}

std::optional<bool> dispatchSymTable(std::string_view symbol, const CBEMInstruction& ins,
                                     BytecodeExecContext& ctx, RegisterValue& out,
                                     IssueBag& issues) noexcept {
    if (kShapeGetterNames().contains(symbol)) {
        const bool ok = dispatchShapeGetterSym(ins, ctx, out, issues, symbol);
        return ok ? std::optional<bool>{true} : std::nullopt;
    }
    for (const auto& e : kSymPrefixDispatch) {
        if (symbol.starts_with(e.prefix)) {
            const bool ok = e.fn(ins, ctx, out, issues, symbol);
            if (!ok && e.failMode == FailMode::Stub) {
                return std::nullopt;
            }
            return ok;
        }
    }
    return std::nullopt;
}

bool stubFunctionCall(std::string_view symbol, const CBEMInstruction& ins, BytecodeExecContext& ctx,
                      IssueBag& issues) noexcept {
    static std::set<std::string> stubMessages;
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    static const bool kListStubs = std::getenv("CF_STUB_LIST") != nullptr;
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    std::string msg = "IR: FunctionCall stub: ";
    msg.append(symbol.empty() ? std::string_view{"(unresolved)"} : symbol);
    const auto [it, inserted] = stubMessages.insert(std::move(msg));
    if (inserted && kListStubs) {
        std::fprintf(stderr, "[stub] %s\n", it->c_str());
    }
    issues.push(vmWarn(issues::vm::kFunctionCallStub, std::string_view{*it}));
    const u32 retReg = ins.operands[4];
    if (retReg == kRegVoid) {
        return true;
    }
    const auto d = decodeRegId(retReg);
    RegisterValue zero;
    zero.componentCount = componentCountForBank(d.bank);
    zero.typeBank = d.bank;
    return writeDst(ctx, retReg, zero, issues);
}
}

std::string_view canonicalizeSymbol(std::string_view sym) noexcept {
    if (const auto colon = sym.rfind(':'); colon != std::string_view::npos) {
        sym.remove_prefix(colon + 1);
    }
    for (;;) {
        const auto us = sym.rfind('_');
        if (us == std::string_view::npos || !isMangleToken(sym.substr(us + 1))) {
            break;
        }
        sym = sym.substr(0, us);
    }
    return sym;
}

bool resolveCurveSample(const CBEMInstruction& ins, const BytecodeExecContext& ctx,
                        CurveSampleTarget& out) noexcept {
    static const bool kAblateSample = std::getenv("CF_ABLATE_SAMPLE") != nullptr;
    if (kAblateSample) [[unlikely]] {
        return false;
    }
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr || res->kind != SamplerKind::Curve) {
        return false;
    }
    if (ins.operands[3] < 1U) {
        return false;
    }
    const u8 comps = res->curve.components;
    if (comps < 1U || comps > 4U) {
        return false;
    }
    out.curve = &res->curve;
    out.components = comps;
    return true;
}

ResolvedCallFn resolveExactCall(std::string_view canonicalName) noexcept {
    if (canonicalName.empty()) {
        return nullptr;
    }
    static const std::set<std::string_view> kSpecialNames = {
        "rotate",         "radians.rotate",       "orientation_axisSide",
        "orientation_axisUp", "orientation_axisForward",
    };
    if (kSpecialNames.contains(canonicalName)) {
        return nullptr;
    }
    const auto& table = exactDispatchTable();
    const auto it = table.find(canonicalName);
    if (it == table.end() || it->second.failMode != FailMode::Fatal) {
        return nullptr;
    }
    return it->second.fn;
}

bool execFunctionCall(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                      IssueBag& issues) noexcept {
    const u32 extFunc = ins.operands[2];
    const u32 retReg = ins.operands[4];

    std::string_view symbol;
    std::string_view cachedCanon;
    if (extFunc < ctx.functions.size()) {
        symbol = ctx.functions[extFunc].symbolName;
        cachedCanon = ctx.functions[extFunc].canonicalName;
    }
    if (vmFunctionCallCountingEnabled() && !symbol.empty()) {
        ++getMutableFunctionCallCounts()[std::string{symbol}];
    }

    const std::string_view canon = !cachedCanon.empty() ? cachedCanon : canonicalizeSymbol(symbol);

    RegisterValue out;
    out.componentCount = 1;
    out.typeBank = bank::kFloat;

    auto dispatched = dispatchSpecial(canon, ins, ctx, out, issues);
    if (!dispatched) {
        dispatched = dispatchPlainTable(canon, ins, ctx, out, issues);
    }
    if (!dispatched) {
        dispatched = dispatchSymTable(canon, ins, ctx, out, issues);
    }
    if (!dispatched) {
        return stubFunctionCall(symbol, ins, ctx, issues);
    }
    if (!*dispatched) {
        return false;
    }

    if (retReg == kRegVoid) {
        return true;
    }
    const auto d = decodeRegId(retReg);
    out.typeBank = d.bank;
    if (out.componentCount == 0) {
        out.componentCount = componentCountForBank(d.bank);
    }
    return writeDst(ctx, retReg, out, issues);
}
}
