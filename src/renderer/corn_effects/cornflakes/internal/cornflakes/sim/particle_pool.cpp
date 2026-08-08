#include <cornflakes/interface/sim/particle_pool.hpp>
#include <cornflakes/interface/simt/simt_interpreter.hpp>

#include <cornflakes/interface/simt/packet_register_file.hpp>

#include <algorithm>
#include <vector>

#if defined(CORNFLAKES_SIMT_PROBE)
#include <cornflakes/interface/simt/scratch_validator.hpp>

#include <atomic>
#include <cstdio>
#include <span>
#endif

namespace whiteout::cornflakes {

#if defined(CORNFLAKES_SIMT_PROBE)
namespace {

std::atomic<unsigned long long> g_probeLayerTicks;
std::atomic<unsigned long long> g_probePoolSlots;
std::atomic<unsigned long long> g_probeLiveSlots;
std::atomic<unsigned long long> g_probePackets;
std::atomic<unsigned long long> g_probePacketsContig;
std::atomic<unsigned long long> g_probeLanesUsed;
std::atomic<unsigned long long> g_probeLanesIssued;
std::atomic<unsigned long long> g_probeSlotBlocks;
std::atomic<unsigned long long> g_probeSlotBlockLive;
std::atomic<unsigned long long> g_probeSlotBlockIssued;
std::atomic<unsigned long long> g_probeSlotBlocksEmpty;

std::atomic<unsigned long long> g_probeUnionRegsMax;
std::atomic<unsigned long long> g_probeEvolveRegsMax;
std::atomic<unsigned long long> g_probeRawSpanMax;
std::atomic<unsigned long long> g_probePoolBytesMax;
std::atomic<unsigned long long> g_probePoolBytesLive;
std::atomic<unsigned long long> g_probePoolBytesPeak;
std::atomic<unsigned long long> g_probeAosBytesLive;
std::atomic<unsigned long long> g_probeAosBytesPeak;
std::atomic<unsigned long long> g_probeDeclaredRegsMax;
std::atomic<unsigned long long> g_probeLocalUnionRegsMax;
std::atomic<unsigned long long> g_probeLocalDeclaredRegsMax;

void probeMax(std::atomic<unsigned long long>& cell, unsigned long long v) noexcept {
    auto seen = cell.load(std::memory_order_relaxed);
    while (v > seen && !cell.compare_exchange_weak(seen, v, std::memory_order_relaxed)) {
    }
}

}

void reportSimtProbe() noexcept {
    const auto packets = g_probePackets.load();
    const auto blocks = g_probeSlotBlocks.load();
    const auto pool = g_probePoolSlots.load();
    const auto live = g_probeLiveSlots.load();
    std::printf("[simt-probe] layerTicks=%llu poolSlots=%llu liveSlots=%llu liveFrac=%.4f\n",
                g_probeLayerTicks.load(), pool, live,
                pool == 0U ? 0.0 : static_cast<double>(live) / static_cast<double>(pool));
    std::printf("[simt-probe] CURRENT  packets=%llu contiguous=%llu (%.4f) laneOccupancy=%.4f\n",
                packets, g_probePacketsContig.load(),
                packets == 0U ? 0.0
                              : static_cast<double>(g_probePacketsContig.load()) /
                                    static_cast<double>(packets),
                g_probeLanesIssued.load() == 0U
                    ? 0.0
                    : static_cast<double>(g_probeLanesUsed.load()) /
                          static_cast<double>(g_probeLanesIssued.load()));
    std::printf("[simt-probe] OPTION1  blocks=%llu emptyBlocks=%llu (%.4f) laneOccupancy=%.4f "
                "packetRatio=%.4f\n",
                blocks, g_probeSlotBlocksEmpty.load(),
                blocks == 0U ? 0.0
                             : static_cast<double>(g_probeSlotBlocksEmpty.load()) /
                                   static_cast<double>(blocks),
                g_probeSlotBlockIssued.load() == 0U
                    ? 0.0
                    : static_cast<double>(g_probeSlotBlockLive.load()) /
                          static_cast<double>(g_probeSlotBlockIssued.load()),
                packets == 0U ? 0.0
                              : static_cast<double>(blocks) / static_cast<double>(packets));
    std::printf("[simt-probe] SIZING   bank3 regs: evolveMax=%llu unionMax=%llu declaredMax=%llu "
                "rawSpanMax=%llu\n",
                g_probeEvolveRegsMax.load(), g_probeUnionRegsMax.load(),
                g_probeDeclaredRegsMax.load(), g_probeRawSpanMax.load());
    std::printf("[simt-probe] SIZING   bank1 regs: unionMax=%llu declaredMax=%llu\n",
                g_probeLocalUnionRegsMax.load(), g_probeLocalDeclaredRegsMax.load());
    std::printf("[simt-probe] SIZING   banks1+3 bytes: poolFilePerLayerMax=%.2f MB "
                "poolFilePeak=%.2f MB aosTodayPeak=%.2f MB\n",
                static_cast<double>(g_probePoolBytesMax.load()) / (1024.0 * 1024.0),
                static_cast<double>(g_probePoolBytesPeak.load()) / (1024.0 * 1024.0),
                static_cast<double>(g_probeAosBytesPeak.load()) / (1024.0 * 1024.0));
}
#endif

void ParticlePool::rebindExternals() {
    for (std::size_t i = 0; i < particles_.size(); ++i) {
        particles_[i].bindExternals(&externals_, i, externals_.slotCount());
    }
}

void ParticlePool::resize(std::size_t count) {
    if (count <= particles_.size() || !layoutKnown_) {
        particles_.resize(count);
        externals_.resize(std::max(externals_.slotCount(), layout_.externalCount),
                          particles_.size());
        rebindExternals();
        syncPoolRegisterFiles();
        return;
    }
    particles_.reserve(count);
    while (particles_.size() < count) {
        particles_.emplace_back(layout_);
    }
    externals_.resize(std::max(externals_.slotCount(), layout_.externalCount),
                      particles_.size());
    rebindExternals();
    syncPoolRegisterFiles();
}

void ParticlePool::enablePoolRegisters(const LayerProgram& layer) {
#if defined(CORNFLAKES_NO_POOL_REGISTERS)
    (void)layer;
    return;
#else
    const auto& scope = layer.evolveProgram();
    if (scope.decodedInstructions.empty()) {
        return;
    }
    if (simt::containsUnorderableSideEffects(scope.decodedInstructions, scope.functions)) {
        return;
    }
    evolveLayout_.build(scope.decodedInstructions, scope.constantsPool.size());
    poolRegistersEnabled_ = true;
    syncPoolRegisterFiles();
    for (std::size_t i = 0; i < particles_.size(); ++i) {
        seedPoolRegisters(i);
    }
#endif
}

void ParticlePool::seedPoolRegisters(std::size_t particle) {
    if (!poolRegistersEnabled_ || particle >= particles_.size()) {
        return;
    }
    const auto sources = evolveLayout_.slotSources();
    for (const u8 b : {static_cast<u8>(scope::kLocal), static_cast<u8>(scope::kStream)}) {
        auto* file = poolTargets_.banks[b];
        if (file == nullptr) {
            continue;
        }
        const auto aos = particles_[particle].scopeRegs(b);
        const u32 base = evolveLayout_.bankBase(b);
        for (u32 slot = 0; slot < sources.size(); ++slot) {
            const auto [bankIx, regIx] = sources[slot];
            if (bankIx != b || regIx >= aos.size()) {
                continue;
            }
            file->store(slot - base, particle, aos[regIx]);
        }
    }
}

void ParticlePool::syncPoolRegisterFiles() {
    if (!poolRegistersEnabled_) {
        return;
    }
    for (const u8 b : {static_cast<u8>(scope::kLocal), static_cast<u8>(scope::kStream)}) {
        const std::size_t count = evolveLayout_.bankCount(b);
        poolRegisters_[b].resize(count, particles_.size());
        poolTargets_.banks[b] = (count == 0U) ? nullptr : &poolRegisters_[b];
    }
}

std::span<const RegisterValue> ParticlePool::materialiseBank(std::size_t particle, u8 bank) const {
    if (particle >= particles_.size() || bank >= kScopeRegisterBuckets) {
        return {};
    }
    const auto aos = particles_[particle].scopeRegs(bank);
    if (!poolRegistersEnabled_ || poolTargets_.banks[bank] == nullptr) {
        return aos;
    }
    const auto& file = poolRegisters_[bank];
    auto& scratch = materialiseScratch_[bank];
    scratch.assign(aos.begin(), aos.end());

    const auto sources = evolveLayout_.slotSources();
    const u32 base = evolveLayout_.bankBase(bank);
    for (u32 slot = 0; slot < sources.size(); ++slot) {
        const auto [bankIx, regIx] = sources[slot];
        if (bankIx != bank) {
            continue;
        }
        if (regIx < scratch.size()) {
            scratch[regIx] = file.load(slot - base, particle);
        }
    }
    return {scratch.data(), scratch.size()};
}

void ParticlePool::resizeForLayer(const LayerProgram& layer) {
    externals_.resize(LayerTickHarness::externalStorageSizeFor(layer), particles_.size());
    rebindExternals();
    for (auto& p : particles_) {
        p.resizeForLayer(layer);
    }
    layout_ = LayerTickHarness::layoutFor(layer);
    layoutKnown_ = true;
    syncPoolRegisterFiles();
}

bool ParticlePool::initBatch(const LayerProgram& layer, u32 baseSeed, IArena& arena,
                             IssueBag& issues) {
    for (std::size_t i = 0; i < particles_.size(); ++i) {
        particles_[i].setRngSeed(baseSeed + static_cast<u32>(i));
        if (!particles_[i].initParticle(layer, arena, issues)) {
            return false;
        }
        seedPoolRegisters(i);
    }
    return true;
}

bool ParticlePool::initRange(const LayerProgram& layer, u32 baseSeed, std::size_t startIdx,
                             std::size_t count, IArena& arena, IssueBag& issues) {
    if (startIdx >= particles_.size() || count == 0U) {
        return true;
    }
    const std::size_t end = std::min(startIdx + count, particles_.size());
    for (std::size_t i = startIdx; i < end; ++i) {
        particles_[i].setRngSeed(baseSeed + static_cast<u32>(i));
        if (!particles_[i].initParticle(layer, arena, issues)) {
            return false;
        }
        seedPoolRegisters(i);
    }
    return true;
}

bool ParticlePool::tickBatch(const LayerProgram& layer, IArena& arena, IssueBag& issues) {
    for (auto& p : particles_) {

        if (p.wasDeadAtFrameStart()) {
            continue;
        }
        if (!p.tick(layer, arena, issues)) {
            return false;
        }
    }
    return true;
}

ParticlePool::SimtTickResult ParticlePool::tickBatchSimt(const LayerProgram& layer, IArena& arena,
                                                         IssueBag& issues) {
    SimtTickResult result;
    const auto& scope = layer.evolveProgram();
    if (scope.cbemBytecode.empty()) {
        result.ranSimt = true;
        return result;
    }

    if (scope.decodedInstructions.empty() ||
        simt::containsUnorderableSideEffects(scope.decodedInstructions, scope.functions)) {
        result.ok = tickBatch(layer, arena, issues);
        result.ranSimt = false;
        return result;
    }

    std::vector<std::size_t> liveSlots;
    liveSlots.reserve(particles_.size());
    for (std::size_t i = 0; i < particles_.size(); ++i) {
        if (!particles_[i].wasDeadAtFrameStart()) {
            liveSlots.push_back(i);
        }
    }
    if (liveSlots.empty()) {
        result.ranSimt = true;
        return result;
    }

#if defined(CORNFLAKES_SIMT_PROBE)
    {
        g_probeLayerTicks.fetch_add(1U, std::memory_order_relaxed);
        g_probePoolSlots.fetch_add(particles_.size(), std::memory_order_relaxed);
        g_probeLiveSlots.fetch_add(liveSlots.size(), std::memory_order_relaxed);

        unsigned long long packets = 0U;
        unsigned long long contiguous = 0U;
        unsigned long long lanesUsed = 0U;
        for (std::size_t base = 0; base < liveSlots.size(); base += simt::kSimtLanes) {
            const std::size_t count = std::min(simt::kSimtLanes, liveSlots.size() - base);
            ++packets;
            lanesUsed += count;
            bool consecutive = true;
            for (std::size_t k = 1; k < count; ++k) {
                if (liveSlots[base + k] != liveSlots[base + k - 1] + 1) {
                    consecutive = false;
                    break;
                }
            }
            contiguous += consecutive ? 1U : 0U;
        }
        g_probePackets.fetch_add(packets, std::memory_order_relaxed);
        g_probePacketsContig.fetch_add(contiguous, std::memory_order_relaxed);
        g_probeLanesUsed.fetch_add(lanesUsed, std::memory_order_relaxed);
        g_probeLanesIssued.fetch_add(packets * simt::kSimtLanes, std::memory_order_relaxed);

        unsigned long long blocks = 0U;
        unsigned long long emptyBlocks = 0U;
        unsigned long long blockLive = 0U;
        std::size_t cursor = 0;
        for (std::size_t base = 0; base < particles_.size(); base += simt::kSimtLanes) {
            const std::size_t end = std::min(particles_.size(), base + simt::kSimtLanes);
            std::size_t inBlock = 0;
            while (cursor < liveSlots.size() && liveSlots[cursor] < end) {
                ++inBlock;
                ++cursor;
            }
            if (inBlock == 0U) {
                ++emptyBlocks;
                continue;
            }
            ++blocks;
            blockLive += inBlock;
        }
        g_probeSlotBlocks.fetch_add(blocks, std::memory_order_relaxed);
        g_probeSlotBlockLive.fetch_add(blockLive, std::memory_order_relaxed);
        g_probeSlotBlockIssued.fetch_add(blocks * simt::kSimtLanes, std::memory_order_relaxed);
        g_probeSlotBlocksEmpty.fetch_add(emptyBlocks, std::memory_order_relaxed);
    }

    {
        const auto countBank = [](u8 bank, std::span<const VMProgramDescriptor* const> progs,
                                  std::size_t& rawSpan) {
            u32 lo = 0xFFFFFFFFU;
            u32 hi = 0U;
            bool any = false;
            const auto span = [&](u32 regId) {
                if (regId == kRegVoid) {
                    return;
                }
                const auto d = decodeRegId(regId);
                if (d.scope != bank) {
                    return;
                }
                any = true;
                const u32 idx = d.localIdx;
                lo = std::min(lo, idx);
                hi = std::max(hi, idx);
            };
            for (const auto* prog : progs) {
                for (const auto& ins : prog->decodedInstructions) {
                    simt::forEachRegisterOperand(ins, span, span);
                }
            }
            rawSpan = 0U;
            if (!any) {
                return std::size_t{0};
            }
            std::vector<u8> present(static_cast<std::size_t>(hi - lo) + 1U, 0U);
            const auto mark = [&](u32 regId) {
                if (regId == kRegVoid) {
                    return;
                }
                const auto d = decodeRegId(regId);
                if (d.scope == bank) {
                    present[d.localIdx - lo] = 1U;
                }
            };
            for (const auto* prog : progs) {
                for (const auto& ins : prog->decodedInstructions) {
                    simt::forEachRegisterOperand(ins, mark, mark);
                }
            }
            std::size_t count = 0U;
            for (const u8 p : present) {
                count += p;
            }
            rawSpan = static_cast<std::size_t>(hi) + 1U;
            return count;
        };

        const auto all = layerScopePrograms(layer);
        const VMProgramDescriptor* evolveOnly[1] = {&scope};
        const std::size_t stride = ((particles_.size() + 15U) / 16U) * 16U;
        unsigned long long poolBytes = 0U;
        unsigned long long aosBytes = 0U;
        for (const u8 bank : {static_cast<u8>(scope::kLocal), static_cast<u8>(scope::kStream)}) {
            std::size_t rawSpan = 0U;
            std::size_t evolveSpan = 0U;
            const std::size_t unionCount = countBank(bank, {all.data(), all.size()}, rawSpan);
            const std::size_t evolveCount = countBank(bank, {evolveOnly, 1U}, evolveSpan);

            poolBytes += static_cast<unsigned long long>(unionCount) * 4ULL *
                         static_cast<unsigned long long>(stride) * 4ULL;

            std::size_t declared = 0U;
            for (const auto* prog : all) {
                const std::size_t c = (prog->registerCounts.size() > std::size_t{bank} + 1U)
                                          ? prog->registerCounts[std::size_t{bank} + 1U]
                                          : 0U;
                declared = std::max(declared, c);
            }
            aosBytes += static_cast<unsigned long long>(declared) *
                        static_cast<unsigned long long>(sizeof(RegisterValue)) *
                        static_cast<unsigned long long>(particles_.size());

            if (bank == scope::kStream) {
                probeMax(g_probeEvolveRegsMax, evolveCount);
                probeMax(g_probeUnionRegsMax, unionCount);
                probeMax(g_probeRawSpanMax, rawSpan);
                probeMax(g_probeDeclaredRegsMax, declared);
            } else {
                probeMax(g_probeLocalUnionRegsMax, unionCount);
                probeMax(g_probeLocalDeclaredRegsMax, declared);
            }
        }
        probeMax(g_probePoolBytesMax, poolBytes);
        const auto prev = probeLastPoolBytes_;
        probeLastPoolBytes_ = poolBytes;
        const auto live = g_probePoolBytesLive.fetch_add(poolBytes - prev,
                                                         std::memory_order_relaxed) +
                          poolBytes - prev;
        probeMax(g_probePoolBytesPeak, live);

        const auto aosPrev = probeLastAosBytes_;
        probeLastAosBytes_ = aosBytes;
        const auto aosLive = g_probeAosBytesLive.fetch_add(aosBytes - aosPrev,
                                                           std::memory_order_relaxed) +
                             aosBytes - aosPrev;
        probeMax(g_probeAosBytesPeak, aosLive);
    }
#endif

    static thread_local std::vector<BytecodeExecContext> laneContexts;
    static thread_local std::vector<BytecodeExecContext*> lanePointers;
    static thread_local std::vector<SpawnEventQueue> laneQueues;
    static thread_local std::vector<std::size_t> laneParticles;
    laneContexts.resize(simt::kSimtLanes);
    lanePointers.assign(simt::kSimtLanes, nullptr);
    laneQueues.resize(simt::kSimtLanes);
    laneParticles.assign(simt::kSimtLanes, simt::SimtPacket::kNoParticle);

    const simt::SimtInterpreter vm;
    static thread_local simt::PacketRegisterLayout registerLayout;
    const simt::PacketRegisterLayout* registerLayoutPtr = &registerLayout;
    bool layoutBuilt = false;

    SpawnEventQueue* realQueue = nullptr;
    for (std::size_t base = 0; base < liveSlots.size(); base += simt::kSimtLanes) {
        const std::size_t count = std::min(simt::kSimtLanes, liveSlots.size() - base);

        for (std::size_t lane = 0; lane < simt::kSimtLanes; ++lane) {
            if (lane >= count) {
                lanePointers[lane] = nullptr;
                laneParticles[lane] = simt::SimtPacket::kNoParticle;
                continue;
            }
            laneParticles[lane] = liveSlots[base + lane];
            auto& particle = particles_[liveSlots[base + lane]];
            particle.bindContext(scope, layer, laneContexts[lane]);
            laneQueues[lane].events.clear();
            laneQueues[lane].dropped = 0U;
            laneQueues[lane].capacity = 0U;
            realQueue = laneContexts[lane].spawnQueue;
            laneContexts[lane].spawnQueue = &laneQueues[lane];
            lanePointers[lane] = &laneContexts[lane];
        }

        if (poolRegistersEnabled_) {
            registerLayoutPtr = &evolveLayout_;
            layoutBuilt = true;
        }
        if (!layoutBuilt) {
            for (std::size_t lane = 0; lane < simt::kSimtLanes; ++lane) {
                if (lanePointers[lane] != nullptr) {
                    registerLayout.build(scope.decodedInstructions,
                                         lanePointers[lane]->constantsPool.size());
                    layoutBuilt = true;
                    break;
                }
            }
        }

        simt::SimtPacket packet;
        packet.laneContexts =
            std::span<BytecodeExecContext* const>{lanePointers.data(), lanePointers.size()};
        packet.executionMask = simt::maskForLiveCount(count);
        packet.baseParticleIndex = base;
        packet.laneParticles =
            std::span<const std::size_t>{laneParticles.data(), laneParticles.size()};

        const auto run = vm.run(scope.decodedInstructions, packet, issues,
                                layoutBuilt ? registerLayoutPtr : nullptr,
                                poolRegistersEnabled_ ? &poolTargets_ : nullptr);
        ++result.packetsRun;
        result.particlesRun += count;
        if (!run.ok) {
            result.ok = false;
            result.ranSimt = true;
            return result;
        }

        if (realQueue != nullptr) {
            for (std::size_t lane = 0; lane < count; ++lane) {
                for (const auto& ev : laneQueues[lane].events) {
                    if (realQueue->capacity != 0U &&
                        realQueue->events.size() >= realQueue->capacity) {
                        ++realQueue->dropped;
                        continue;
                    }
                    realQueue->events.push_back(ev);
                }
                realQueue->dropped += laneQueues[lane].dropped;
            }
        }
        for (std::size_t lane = 0; lane < count; ++lane) {
            particles_[liveSlots[base + lane]].finishScope(laneContexts[lane]);
        }
    }

    result.ranSimt = true;
    return result;
}

}
