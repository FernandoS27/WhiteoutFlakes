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
#include <unordered_map>

namespace whiteout::cornflakes {
namespace {
std::optional<u32> resolveKickEventIdFromObjSlot(const CBEMInstruction& ins,
                                                 const BytecodeExecContext& ctx) noexcept {
    const u32 extFunc = ins.operands[2];
    if (extFunc >= ctx.functions.size()) {
        return std::nullopt;
    }
    const u32 symSlot = ctx.functions[extFunc].symbolSlot;
    if (symSlot == kSymbolSlotUnbound) {
        return std::nullopt;
    }

    for (const auto& b : ctx.externalBindings) {
        if (b.slot == static_cast<u16>(symSlot)) {
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
bool readFnArg(const CBEMInstruction& ins, std::size_t i, BytecodeExecContext& ctx,
               RegisterValue& out, IssueBag& issues) noexcept {
    const std::size_t argRegIndex = (i * 2U) + 1U;
    if (argRegIndex >= ins.extraOperands.size()) {
        issues.push(vmFatal(issues::vm::kOperandCount, "IR: FunctionCall arg index out of range"));
        return false;
    }
    return readSrc(ctx, ins.extraOperands[argRegIndex], out, issues);
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
    auto drawUnit = [&]() -> f32 {
        if (ctx.rng == nullptr)
            return 0.0F;
        const u32 raw = ctx.rng->advance();

        const u32 bits = (raw >> fpbits::kRandMantissaShift) | fpbits::kOneF32;
        f32 v;
        std::memcpy(&v, &bits, sizeof(f32));
        return v - 1.0F;
    };
    for (u8 i = 0; i < outComponents; ++i) {
        const f32 t = drawUnit();
        const f32 a = lo.lanes[i];
        const f32 b = hi.lanes[i];
        out.lanes[i] = a + (b - a) * t;
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

    RegisterValue payload;
    const std::size_t payloadArgIdx = resolveKickEventIdFromObjSlot(ins, ctx).has_value() ? 0U : 1U;
    if (!readFnArg(ins, payloadArgIdx, ctx, payload, issues)) {
        return false;
    }

    u32 count = 0;
    if (ctx.lastGenerateValid) {
        count = ctx.lastGenerateCount;
        ctx.lastGenerateValid = false;
    } else {
        count = laneAsU32(payload, 0);
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
            if (pendingPayload.hasBoolPayload) {
                ev.hasBoolPayload = true;
                ev.boolPayloadWidth = pendingPayload.boolPayloadWidth;
                ev.boolPayload = pendingPayload.boolPayload;
                ev.boolPayloadId = pendingPayload.boolPayloadId;
            }

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
    auto resolved = resolveKickEventIdFromObjSlot(ins, ctx);
    if (!resolved) {

        return true;
    }
    const u32 eventId = *resolved;

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

    BytecodeExecContext::PendingPayloadElement* slot = nullptr;
    for (auto& s : ctx.pendingPayloadElements) {
        if (s.valid && s.eventId == eventId) {
            slot = &s;
            break;
        }
    }
    if (slot == nullptr) {
        for (auto& s : ctx.pendingPayloadElements) {
            if (!s.valid) {
                slot = &s;
                slot->eventId = eventId;
                slot->valid = true;
                slot->positionCount = 0;
                slot->hasOrientation = false;
                break;
            }
        }
    }
    if (slot == nullptr) {
        return true;
    }

    const u8 bnk = payloadA.typeBank;
    const bool isIntBank = (bnk == bank::kInt || bnk == bank::kInt2 || bnk == bank::kInt2Alt ||
                            bnk == bank::kInt2Alt2 || bnk == bank::kInt3 || bnk == bank::kInt4);
    const bool isScalarBool = (bnk == bank::kBool && payloadA.componentCount == 1U);
    const u8 intWidth = std::min<u8>(payloadA.componentCount, 4U);
    if (isScalarBool) {

        slot->hasBoolPayload = true;
        slot->boolPayloadWidth = 1U;
        slot->boolPayloadId = payloadElementId;
        const i32 src = (semByte == 0U) ? laneAsI32(payloadA, 0) : laneAsI32(payloadB, 0);
        slot->boolPayload[0] = (src != 0) ? 1 : 0;
    } else if (isIntBank && intWidth >= 1U && intWidth <= 4U) {

        slot->hasIntPayload = true;
        slot->intPayloadWidth = intWidth;
        slot->intPayloadId = payloadElementId;
        for (u8 lane = 0; lane < intWidth; ++lane) {
            slot->intPayload[lane] =
                (semByte == 0U) ? laneAsI32(payloadA, lane) : laneAsI32(payloadB, lane);
        }
    } else if (payloadA.componentCount >= 4U || payloadB.componentCount >= 4U) {

        slot->hasOrientation = true;
        slot->orientationPayloadId = payloadElementId;
        for (int i = 0; i < 4; ++i) {
            slot->orientation[i] = (semByte == 0U) ? payloadA.lanes[i] : payloadB.lanes[i];
        }
    } else {

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

bool dispatchAppendPayload(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                           IssueBag& issues) noexcept {
    out = RegisterValue::scalarI(0);
    if (ins.operands[3] >= 1U) {
        RegisterValue arg0;
        if (readFnArg(ins, 0, ctx, arg0, issues)) {
            out = arg0;
        }
    }
    return true;
}

const SamplerResource* resolveTargetSampler(const CBEMInstruction& ins,
                                            const BytecodeExecContext& ctx) noexcept {
    const u32 extFunc = ins.operands[2];
    if (extFunc >= ctx.functions.size()) {
        return nullptr;
    }
    const u32 extSlot = ctx.functions[extFunc].symbolSlot;
    if (extSlot == kSymbolSlotUnbound) {
        return nullptr;
    }

    std::string_view name;
    for (const auto& b : ctx.externalBindings) {
        if (b.slot == static_cast<u16>(extSlot)) {
            name = b.name;
            break;
        }
    }
    if (name.empty()) {
        return nullptr;
    }
    return findSamplerByName(ctx.samplers, name);
}

bool dispatchSample(const CBEMInstruction& ins, BytecodeExecContext& ctx, RegisterValue& out,
                    IssueBag& issues) noexcept {
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr || res->kind != SamplerKind::Curve) {
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
    auto matches = [payloadIndex](u32 stagedId) {
        return payloadIndex == 0U || payloadIndex == stagedId;
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
        if (matches(ctx.spawnPositionPayloadId)) {
            for (u8 i = 0; i < width; ++i) {
                out.lanes[i] = (i < 3U) ? ctx.spawnTranslate[i] : 0.0F;
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

struct ResolvedSpatialLayer {
    const SpatialLayerResource* resource = nullptr;
    i32 hashIndex = -1;
};
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
    if (resolved.resource == nullptr || resolved.hashIndex < 0 ||
        static_cast<std::size_t>(resolved.hashIndex) >= ctx.spatialHashes.size()) {

        return true;
    }
    ProximityHash* hash = ctx.spatialHashes[static_cast<std::size_t>(resolved.hashIndex)];
    if (hash == nullptr) {
        return true;
    }

    RegisterValue keyReg;
    RegisterValue posReg;
    if (!readFnArg(ins, 0, ctx, keyReg, issues) || !readFnArg(ins, 1, ctx, posReg, issues)) {
        return false;
    }
    const std::array<f32, 3> pos{posReg.lanes[0], posReg.lanes[1], posReg.lanes[2]};
    hash->insert(pos, ctx.currentSelfId);
    return true;
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

    if (ins.operands[3] < 2U) {
        return true;
    }
    RegisterValue centerReg;
    RegisterValue radiusReg;
    if (!readFnArg(ins, 0, ctx, centerReg, issues) || !readFnArg(ins, 1, ctx, radiusReg, issues)) {
        return false;
    }

    u32 nIndex = 0U;
    if (ins.operands[3] >= 4U) {

        RegisterValue nReg;
        if (readFnArg(ins, 3, ctx, nReg, issues)) {
            nIndex = laneAsU32(nReg, 0);
        }
    }

    const std::array<f32, 3> target{centerReg.lanes[0], centerReg.lanes[1], centerReg.lanes[2]};
    const f32 radius = radiusReg.lanes[0];
    const ProximityEntry* hit = hash->closestN(target, radius, nIndex);
    if (hit == nullptr) {

        return true;
    }

    if (suffix == 'F' && width == 3U) {
        out.lanes[0] = hit->position[0];
        out.lanes[1] = hit->position[1];
        out.lanes[2] = hit->position[2];
        return true;
    }
    if (suffix == 'F' && width == 1U) {

        const f32 dx = hit->position[0] - target[0];
        const f32 dy = hit->position[1] - target[1];
        const f32 dz = hit->position[2] - target[2];
        out.lanes[0] = std::sqrt(dx * dx + dy * dy + dz * dz);
        return true;
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
} // namespace xform_mask

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
        const bool usePayloadPath = tryPayload && hasPositionPayload;

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

bool dispatchSamplePosition(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                            RegisterValue& out, IssueBag&) noexcept {
    const SamplerResource* res = resolveTargetSampler(ins, ctx);
    if (res == nullptr || res->kind != SamplerKind::Shape || ctx.rng == nullptr) {
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

        f32 sx = lx * sh.nonUniformScale[0];
        f32 sy = ly * sh.nonUniformScale[1];
        f32 sz = lz * sh.nonUniformScale[2];

        if (sh.transformRotate &&
            (sh.eulerOrientation[0] != 0.0F || sh.eulerOrientation[1] != 0.0F ||
             sh.eulerOrientation[2] != 0.0F)) {
            const f32 cx = std::cos(sh.eulerOrientation[0]);
            const f32 sxn = std::sin(sh.eulerOrientation[0]);
            const f32 cy = std::cos(sh.eulerOrientation[1]);
            const f32 syn = std::sin(sh.eulerOrientation[1]);
            const f32 cz = std::cos(sh.eulerOrientation[2]);
            const f32 szn = std::sin(sh.eulerOrientation[2]);

            const f32 x1 = sx;
            const f32 y1 = cx * sy - sxn * sz;
            const f32 z1 = sxn * sy + cx * sz;
            const f32 x2 = cy * x1 + syn * z1;
            const f32 y2 = y1;
            const f32 z2 = -syn * x1 + cy * z1;
            sx = cz * x2 - szn * y2;
            sy = szn * x2 + cz * y2;
            sz = z2;
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

    switch (res->shape.type) {
    case ShapeType::Sphere: {

        const f32 u1 = drawUnit();
        const f32 u2 = drawUnit();
        const f32 u3 = drawUnit();
        constexpr f32 kTwoPi = 6.28318530717958647692F;
        const f32 phi = u1 * kTwoPi;
        const f32 cosTheta = 2.0F * u2 - 1.0F;
        const f32 sinTheta = std::sqrt(std::max(0.0F, 1.0F - cosTheta * cosTheta));

        const f32 outerR = res->shape.radius;
        const f32 innerR = res->shape.innerRadius;
        const f32 r3min = innerR * innerR * innerR;
        const f32 r3max = outerR * outerR * outerR;
        const f32 r = std::cbrt(r3min + (r3max - r3min) * u3);
        f32 x = r * sinTheta * std::cos(phi);
        f32 y = r * sinTheta * std::sin(phi);
        f32 z = r * cosTheta;
        if (res->shape.hemisphere && z < 0.0F) {
            z = -z;
        }
        applyShapeTrs(x, y, z);
        return true;
    }
    case ShapeType::Box: {

        const f32 hx = res->shape.boxDimensions[0] * 0.5F;
        const f32 hy = res->shape.boxDimensions[1] * 0.5F;
        const f32 hz = res->shape.boxDimensions[2] * 0.5F;
        const f32 u0 = drawUnit() * 2.0F - 1.0F;
        const f32 u1 = drawUnit() * 2.0F - 1.0F;
        const f32 u2 = drawUnit() * 2.0F - 1.0F;
        applyShapeTrs(u0 * hx, u2 * hz, u1 * hy);
        return true;
    }
    case ShapeType::Cylinder: {

        const f32 outerR = res->shape.radius;
        const f32 innerR = res->shape.innerRadius;
        const f32 u_r = drawUnit();
        const f32 ratio = (outerR > 0.0F) ? innerR / outerR : 0.0F;
        const f32 expo = (ratio + 1.0F) * 0.5F;
        const f32 r = std::pow(u_r, expo) * (outerR - innerR) + innerR;
        const f32 angle = drawUnit() * 6.28318530717958647692F;
        const f32 h = (drawUnit() - 0.5F) * res->shape.height;
        applyShapeTrs(r * std::cos(angle), r * std::sin(angle), h);
        return true;
    }
    case ShapeType::Cone: {

        const f32 h = drawUnit() * res->shape.height;
        const f32 t = (res->shape.height > 0.0F) ? (h / res->shape.height) : 0.0F;
        const f32 maxR = res->shape.innerRadius + (res->shape.radius - res->shape.innerRadius) * t;
        const f32 angle = drawUnit() * 6.28318530717958647692F;
        const f32 r = std::sqrt(drawUnit()) * maxR;
        applyShapeTrs(r * std::cos(angle), r * std::sin(angle), h);
        return true;
    }
    default:

        return false;
    }
}
// FunctionCall dispatch -------------------------------------------------------
//
// The bulk of resolved external symbols map 1:1 to a dispatcher with the
// uniform signature `(ins, ctx, out, issues) -> bool`. A few have an
// irregular shape (rotate splits on argc, orientation_axis* picks an axis,
// some prefix-matched dispatchers want the full symbol string) and are
// handled in `dispatchSpecial` before the table lookup.

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
};

// Exact-match table — O(1) hash lookup. Holds the bulk of resolved symbols.
const std::unordered_map<std::string_view, ExactDispatch>& exactDispatchTable() {
    static const std::unordered_map<std::string_view, ExactDispatch> kTable = {
        {"rand", {dispatchRand}},
        {"vrand", {dispatchVrand}},
        {"effect.age", {dispatchEffectAge}},
        {"effect.isRunning", {dispatchEffectIsRunning}},
        {"effect.position", {dispatchEffectPosition}},
        {"duration", {dispatchDuration}},
        {"self.kill", {dispatchSelfKill}},
        {"generate", {dispatchGenerate}},
        {"trigger", {dispatchTrigger}},
        {"initPayload", {dispatchInitPayload}},
        {"kick", {dispatchKick}},
        {"hasPayloadElement", {dispatchHasPayloadElement}},
        {"sample", {dispatchSample, FailMode::Stub}},
        {"samplePosition", {dispatchSamplePosition, FailMode::Stub}},
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
    };
    return kTable;
}

// Prefix-match tables — only a handful of entries each, linear scan is faster
// than hashing once `find()` overhead is paid.
constexpr PrefixDispatch kPrefixDispatch[] = {
    {"buildPayloadElement", dispatchBuildPayloadElement},
    {"appendPayload", dispatchAppendPayload},
    {"scene.intersect", dispatchSceneIntersect},
};

constexpr SymPrefixDispatch kSymPrefixDispatch[] = {
    {"extractPayloadElement", dispatchExtractPayloadElement},
    {"scene.orientation", dispatchSceneOrientation},
    {"closest", dispatchSpatialClosest},
};

// Returns nullopt if no special-case rule applies; otherwise the dispatcher's
// success/fail bool (false propagates as a fatal abort).
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
    return std::nullopt;
}

// Looks up `symbol` first in the exact-match hash table (O(1)), then falls
// back to the small prefix table (linear scan over ~3 entries). Returns
// nullopt on miss. On Fatal failure returns false; on Stub failure returns
// nullopt (caller falls through to the unresolved-symbol stub path).
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

// Looks up `symbol` in the symbol-aware prefix-dispatch table.
std::optional<bool> dispatchSymTable(std::string_view symbol, const CBEMInstruction& ins,
                                     BytecodeExecContext& ctx, RegisterValue& out,
                                     IssueBag& issues) noexcept {
    for (const auto& e : kSymPrefixDispatch) {
        if (symbol.starts_with(e.prefix)) {
            return e.fn(ins, ctx, out, issues, symbol);
        }
    }
    return std::nullopt;
}

// Stub path for symbols the VM hasn't implemented yet. Warns once per
// distinct symbol and writes a typed zero to the return register.
bool stubFunctionCall(std::string_view symbol, const CBEMInstruction& ins, BytecodeExecContext& ctx,
                      IssueBag& issues) noexcept {
    static std::set<std::string> stubMessages;
    std::string msg = "IR: FunctionCall stub: ";
    msg.append(symbol.empty() ? std::string_view{"(unresolved)"} : symbol);
    const auto [it, _] = stubMessages.insert(std::move(msg));
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
} // namespace

bool execFunctionCall(const CBEMInstruction& ins, BytecodeExecContext& ctx,
                      IssueBag& issues) noexcept {
    const u32 extFunc = ins.operands[2];
    const u32 retReg = ins.operands[4];

    std::string_view symbol;
    if (extFunc < ctx.functions.size()) {
        symbol = ctx.functions[extFunc].symbolName;
    }
    if (!symbol.empty()) {
        ++getMutableFunctionCallCounts()[std::string{symbol}];
    }

    RegisterValue out;
    out.componentCount = 1;
    out.typeBank = bank::kFloat;

    auto dispatched = dispatchSpecial(symbol, ins, ctx, out, issues);
    if (!dispatched) {
        dispatched = dispatchPlainTable(symbol, ins, ctx, out, issues);
    }
    if (!dispatched) {
        dispatched = dispatchSymTable(symbol, ins, ctx, out, issues);
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
} // namespace whiteout::cornflakes
