#include <cornflakes/interface/simt/simt_interpreter.hpp>

#include <cornflakes/vm/cbem/cbem_core.hpp>
#include <cornflakes/vm/cbem/cbem_internal.hpp>
#include <cornflakes/vm/cbem/cbem_packet_backend.hpp>

#include <cornflakes/interface/simt/packet_register_file.hpp>
#include <cornflakes/interface/simt/pool_register_file.hpp>

#include <cstring>

namespace whiteout::cornflakes::simt {

namespace {

bool symbolIsOrdered(std::string_view canon) noexcept {
    return canon == "kick" || canon == "insert";
}

bool symbolIsUnorderable(std::string_view canon) noexcept {
    return canon == "insert";
}

#if defined(CORNFLAKES_SIMT_NOINLINE_WRITEBACK)
[[gnu::noinline]]
#endif
void publishPacketRegisters(const PacketRegisterLayout& layout,
                            cbem::PacketState<kSimtLanes>& state, cbem::PacketMask mask,
                            const PoolPublishTargets* pool,
                            std::span<const std::size_t> laneParticles) noexcept {
#if defined(CORNFLAKES_ABLATE_WRITEBACK)
    (void)layout;
    (void)state;
    (void)mask;
    (void)pool;
    (void)laneParticles;
    return;
#else
#if defined(CORNFLAKES_ABLATE_WRITEBACK_LOCAL)
    constexpr u8 kLocalBank = 1U;
#endif
    const auto sources = layout.slotSources();

    bool poolHandled = false;
    if (pool != nullptr) {
        std::size_t first = SimtPacket::kNoParticle;
        std::size_t count = 0U;
        bool contiguousPrefix = true;
        for (std::size_t lane = 0; lane < kSimtLanes; ++lane) {
            const bool live = mask.live(lane);
            if (!live) {
                for (std::size_t rest = lane; rest < kSimtLanes; ++rest) {
                    if (mask.live(rest)) {
                        contiguousPrefix = false;
                        break;
                    }
                }
                break;
            }
            if (lane >= laneParticles.size()) {
                contiguousPrefix = false;
                break;
            }
            const std::size_t p = laneParticles[lane];
            if (p == SimtPacket::kNoParticle) {
                contiguousPrefix = false;
                break;
            }
            if (count == 0U) {
                first = p;
            } else if (p != first + count) {
                contiguousPrefix = false;
                break;
            }
            ++count;
        }

        if (contiguousPrefix && count != 0U) {
            for (u32 slot = 0; slot < sources.size(); ++slot) {
                const auto [bankIx, regIx] = sources[slot];
                auto* file = pool->banks[bankIx];
                if (file == nullptr) {
                    continue;
                }
                const std::size_t fileSlot = slot - layout.bankBase(bankIx);
                if (fileSlot >= file->registerCount() || first + count > file->particleStride()) {
                    continue;
                }
                for (u8 c = 0; c < 4U; ++c) {
                    std::memcpy(file->plane(fileSlot, c).data() + first,
                                state.file.plane(slot, c).data(), count * sizeof(u32));
                }
                file->setTagRange(fileSlot, first, count, state.file.componentCount(slot),
                                  state.file.typeBank(slot));
            }
            poolHandled = true;
        }
    }

    for (std::size_t lane = 0; lane < kSimtLanes; ++lane) {
        if (!mask.live(lane)) {
            continue;
        }
        auto* ctx = state.contexts[lane];
        const std::size_t particle = (pool != nullptr && lane < laneParticles.size())
                                         ? laneParticles[lane]
                                         : SimtPacket::kNoParticle;
        for (u32 slot = 0; slot < sources.size(); ++slot) {
            const auto [bankIx, regIx] = sources[slot];
#if defined(CORNFLAKES_ABLATE_WRITEBACK_LOCAL)
            if (bankIx == kLocalBank) {
                continue;
            }
#endif
            if (pool != nullptr) {
                if (auto* file = pool->banks[bankIx]; file != nullptr) {
                    if (poolHandled) {
                        continue;
                    }
                    const std::size_t fileSlot = slot - layout.bankBase(bankIx);
                    if (particle != SimtPacket::kNoParticle &&
                        fileSlot < file->registerCount() &&
                        particle < file->particleStride()) {
                        file->store(fileSlot, particle, state.file.loadLane(slot, lane));
                    }
                    continue;
                }
            }
            auto bankSpan = ctx->scopeRegisters[bankIx];
            if (regIx < bankSpan.size()) {
                bankSpan[regIx] = state.file.loadLane(slot, lane);
            }
        }
    }
#endif
}

bool anyCallMatches(std::span<const CBEMInstruction> program,
                    std::span<const FunctionBinding> functions,
                    bool (*pred)(std::string_view)) noexcept {
    for (const auto& ins : program) {
        if (ins.opcode != Opcode::FunctionCall) {
            continue;
        }
        const u32 extFunc = ins.operands[2];
        if (extFunc >= functions.size()) {
            return true;
        }
        if (pred(canonicalizeSymbol(functions[extFunc].symbolName))) {
            return true;
        }
    }
    return false;
}

}

bool instructionHasSideEffects(const CBEMInstruction& ins,
                               std::span<const FunctionBinding> functions) noexcept {
    if (ins.opcode != Opcode::FunctionCall) {
        return false;
    }
    const u32 extFunc = ins.operands[2];
    if (extFunc >= functions.size()) {
        return true;
    }
    const auto symbol = canonicalizeSymbol(functions[extFunc].symbolName);

    return symbolIsOrdered(symbol);
}

bool containsSideEffects(std::span<const CBEMInstruction> program,
                         std::span<const FunctionBinding> functions) noexcept {
    for (const auto& ins : program) {
        if (instructionHasSideEffects(ins, functions)) {
            return true;
        }
    }
    return false;
}

bool containsUnorderableSideEffects(std::span<const CBEMInstruction> program,
                                    std::span<const FunctionBinding> functions) noexcept {
    return anyCallMatches(program, functions, symbolIsUnorderable);
}

SimtRunResult SimtInterpreter::run(std::span<const CBEMInstruction> program, SimtPacket& packet,
                                   IssueBag& issues, const PacketRegisterLayout* preBuiltLayout,
                                   const PoolPublishTargets* poolTargets) const {
    SimtRunResult result;
    result.finalMask = packet.executionMask;

    static thread_local cbem::PacketState<kSimtLanes> state;
    static thread_local PacketRegisterLayout ownLayout;

    const BytecodeExecContext* scopeCtx = nullptr;
    for (std::size_t lane = 0; lane < kSimtLanes; ++lane) {
        if (packet.isLive(lane)) {
            scopeCtx = packet.laneContexts[lane];
            break;
        }
    }
    if (scopeCtx == nullptr) {
        result.instructionsExecuted = program.size();
        return result;
    }
    const std::size_t poolBytes = scopeCtx->constantsPool.size();

    const PacketRegisterLayout* layout = preBuiltLayout;
    if (layout == nullptr) {
        ownLayout.build(program, poolBytes);
        layout = &ownLayout;
    }
    const PoolPublishTargets* pool =
        (poolTargets != nullptr && poolTargets->any()) ? poolTargets : nullptr;

    state.beginScope(*layout, poolBytes);
    for (std::size_t lane = 0; lane < kSimtLanes; ++lane) {
        state.contexts[lane] =
            (lane < packet.laneContexts.size()) ? packet.laneContexts[lane] : nullptr;
    }

    {
        const auto liveIns = layout->liveInSlots();
        if (!liveIns.empty()) {
            const auto sources = layout->slotSources();
            for (std::size_t lane = 0; lane < kSimtLanes; ++lane) {
                if (!packet.isLive(lane)) {
                    continue;
                }
                auto* ctx = packet.laneContexts[lane];
                const std::size_t particle = (pool != nullptr && lane < packet.laneParticles.size())
                                                 ? packet.laneParticles[lane]
                                                 : SimtPacket::kNoParticle;
                for (const u32 slot : liveIns) {
                    const auto [bankIx, regIx] = sources[slot];
                    if (pool != nullptr) {
                        if (auto* file = pool->banks[bankIx]; file != nullptr) {
                            const std::size_t fileSlot = slot - layout->bankBase(bankIx);
                            if (particle != SimtPacket::kNoParticle &&
                                fileSlot < file->registerCount() &&
                                particle < file->particleStride()) {
                                state.file.storeLane(slot, lane, file->load(fileSlot, particle));
                            }
                            continue;
                        }
                    }
                    const auto bankSpan = ctx->scopeRegisters[bankIx];
                    if (regIx < bankSpan.size()) {
                        state.file.storeLane(slot, lane, bankSpan[regIx]);
                    }
                }
            }
        }
    }

    const auto mask = cbem::liveFor<kSimtLanes>(state, packet.executionMask);
    const std::size_t liveCount = liveLaneCount(mask.bits);

    const auto writeBack = [&]() {
        publishPacketRegisters(*layout, state, mask, pool, packet.laneParticles);
    };

    for (const auto& ins : program) {
        ++result.instructionsExecuted;
        if (!cbem::step<cbem::PacketBackend<kSimtLanes>>(ins, state, mask, issues)) {
            writeBack();
            result.ok = false;
            result.finalMask = packet.executionMask;
            return result;
        }
        result.laneInstructionsExecuted += liveCount;
    }
    writeBack();

    for (std::size_t lane = 0; lane < kSimtLanes; ++lane) {
        if (!packet.isLive(lane)) {
            continue;
        }
        if (packet.laneContexts[lane]->selfKillRequested) {
            result.killedMask |= (1U << lane);
        }
    }
    result.finalMask = packet.executionMask & ~result.killedMask;
    return result;
}

}
