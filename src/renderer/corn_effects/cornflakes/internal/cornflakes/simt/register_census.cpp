#include <cornflakes/interface/simt/register_census.hpp>

#include <cornflakes/interface/sim/layer_tick_harness.hpp>
#include <cornflakes/interface/simt/packet_register_file.hpp>
#include <cornflakes/interface/simt/scratch_validator.hpp>

#include <algorithm>
#include <cstdio>
#include <unordered_map>
#include <unordered_set>

namespace whiteout::cornflakes::simt {

namespace {

struct Span {
    u32 first = 0U;
    u32 last = 0U;
};

const char* kScopeNames[] = {"init", "physics", "timeFixed", "timeVarying", "?"};
const char* kBankNames[] = {"cr", "vr", "ir", "sr"};

u8 scopeIndexOf(const LayerProgram& layer, const VMProgramDescriptor& scope) noexcept {
    const auto programs = layerScopePrograms(layer);
    for (u8 i = 0; i < 4U; ++i) {
        if (programs[i] == &scope) {
            return i;
        }
    }
    return 4U;
}

}

ScopeSizing analyseProgramSizing(std::span<const CBEMInstruction> program,
                                 std::size_t constantsPoolBytes,
                                 const std::array<u32, 5>& registerCounts) {
    ScopeSizing out;
    out.instructions = program.size();
    for (std::size_t b = 0; b < kScopeRegisterBuckets; ++b) {
        out.banks[b].declaredCount = registerCounts[b + 1U];
    }

    std::array<std::unordered_set<u32>, kScopeRegisterBuckets> written;
    std::array<std::unordered_set<u32>, kScopeRegisterBuckets> liveIn;
    std::array<std::unordered_map<u32, Span>, kScopeRegisterBuckets> spans;
    std::unordered_set<u32> externalSlots;

    const auto touch = [&](u8 bucket, u32 idx, u32 at) {
        auto [it, fresh] = spans[bucket].try_emplace(idx, Span{at, at});
        if (!fresh) {
            it->second.last = std::max(it->second.last, at);
        }
    };

    for (std::size_t i = 0; i < program.size(); ++i) {
        const auto& ins = program[i];
        const auto at = static_cast<u32>(i);

        forEachRegisterOperand(
            ins,
            [&](u32 regId) {
                const auto d = decodeRegId(regId);
                if (regId == kRegVoid || constPoolHit(d, constantsPoolBytes) ||
                    d.scope >= kScopeRegisterBuckets) {
                    return;
                }
                auto& bank = out.banks[d.scope];
                ++bank.reads;
                touch(d.scope, d.localIdx, at);
                if (written[d.scope].count(d.localIdx) == 0U &&
                    liveIn[d.scope].insert(d.localIdx).second) {
                    ++bank.readBeforeWrite;
                }
            },
            [](u32) {});

        forEachRegisterOperand(
            ins, [](u32) {},
            [&](u32 regId) {
                const auto d = decodeRegId(regId);
                if (regId == kRegVoid || constPoolHit(d, constantsPoolBytes) ||
                    d.scope >= kScopeRegisterBuckets) {
                    return;
                }
                auto& bank = out.banks[d.scope];
                ++bank.writes;
                touch(d.scope, d.localIdx, at);
                written[d.scope].insert(d.localIdx);
            });

        u32 byteSlot = 0U;
        switch (ins.opcode) {
        case Opcode::LoadExternal:
        case Opcode::StoreToExternal:
            if (ins.operandCount < 2U) {
                continue;
            }
            byteSlot = ins.operands[1] & 0xFFFFU;
            break;
        case Opcode::ExternalClear:
            if (ins.operandCount < 1U) {
                continue;
            }
            byteSlot = ins.operands[0] & 0xFFFFU;
            break;
        default:
            continue;
        }
        externalSlots.insert(byteSlot);
        out.highestExternalSlot = std::max(out.highestExternalSlot, byteSlot + 1U);
    }

    out.distinctExternalSlots = static_cast<u32>(externalSlots.size());

    for (std::size_t b = 0; b < kScopeRegisterBuckets; ++b) {
        auto& bank = out.banks[b];
        bank.distinctIndices = static_cast<u32>(spans[b].size());
        if (spans[b].empty()) {
            continue;
        }

        std::vector<std::pair<u32, int>> events;
        events.reserve(spans[b].size() * 2U);
        for (const auto& [idx, span] : spans[b]) {
            bank.highestIndexUsed = std::max(bank.highestIndexUsed, idx + 1U);
            const u32 from = (liveIn[b].count(idx) != 0U) ? 0U : span.first;
            events.emplace_back(from, +1);
            events.emplace_back(span.last + 1U, -1);
        }
        std::sort(events.begin(), events.end(), [](const auto& a, const auto& c) {
            return a.first != c.first ? a.first < c.first : a.second < c.second;
        });
        int live = 0;
        for (const auto& [pos, delta] : events) {
            (void)pos;
            live += delta;
            bank.maxLive = std::max(bank.maxLive, static_cast<u32>(live));
        }
    }

    return out;
}

LayerSizing analyseLayerSizing(const LayerProgram& layer) {
    LayerSizing out;
    out.id = layer.id;
    out.externalStorageSize = LayerTickHarness::externalStorageSizeFor(layer);

    const auto programs = layerScopePrograms(layer);
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto* s = programs[i];
        if (s->decodedInstructions.empty() && !s->cbemBytecode.empty()) {
            out.scopes[i].undecoded = true;
            continue;
        }
        out.scopes[i] = analyseProgramSizing(s->decodedInstructions, s->constantsPool.size(),
                                             s->registerCounts);
    }
    return out;
}

namespace {
IScopeRunObserver* g_observer = nullptr;
}

IScopeRunObserver* scopeRunObserver() noexcept {
    return g_observer;
}

void setScopeRunObserver(IScopeRunObserver* observer) noexcept {
    g_observer = observer;
}

const RegisterTagCensus::TouchedRegisters&
RegisterTagCensus::touchedFor(const VMProgramDescriptor& scope) {
    const auto [it, fresh] = touched_.try_emplace(&scope);
    if (!fresh) {
        return it->second;
    }
    auto& entry = it->second;

    if (scope.decodedInstructions.empty() && !scope.cbemBytecode.empty()) {
        entry.unscannable = true;
        return entry;
    }

    std::array<std::unordered_set<u32>, kScopeRegisterBuckets> seen;
    const auto add = [&](u32 regId) {
        const auto d = decodeRegId(regId);
        if (regId == kRegVoid || constPoolHit(d, scope.constantsPool.size()) ||
            d.scope >= kScopeRegisterBuckets) {
            return;
        }
        seen[d.scope].insert(d.localIdx);
    };
    for (const auto& ins : scope.decodedInstructions) {
        forEachRegisterOperand(ins, add, add);
    }
    for (std::size_t b = 0; b < kScopeRegisterBuckets; ++b) {
        entry.banks[b].assign(seen[b].begin(), seen[b].end());
        std::sort(entry.banks[b].begin(), entry.banks[b].end());
    }
    return entry;
}

void RegisterTagCensus::note(std::string line) {
    if (detail_.size() < 32U) {
        detail_.push_back(std::move(line));
    }
}

void RegisterTagCensus::onScopeBegin(const LayerProgram& layer, const VMProgramDescriptor& scope,
                                     BytecodeExecContext& ctx) {
    (void)layer;
    (void)scope;
    if (ctx.trace != nullptr) {
        traceInstalled_ = false;
        ++totals_.traceUnavailableScopeRuns;
        return;
    }
    trace_.clear();
    trace_.capacity = 0U;
    ctx.trace = &trace_;
    traceInstalled_ = true;
}

void RegisterTagCensus::onScopeEnd(const LayerProgram& layer, const VMProgramDescriptor& scope,
                                   const BytecodeExecContext& ctx) {
    ++totals_.scopeRuns;
    const u32 layerId = layer.id.value;
    const u8 scopeIx = scopeIndexOf(layer, scope);

    const auto compare = [&](auto& map, const auto& key, Tag tag, std::size_t& counter,
                             auto&& describe) {
        const auto [it, fresh] = map.try_emplace(key, tag);
        if (fresh || it->second == tag) {
            return;
        }
        ++counter;
        note(describe(tag, it->second));
    };

    if (traceInstalled_) {
        for (const auto& ev : trace_.events) {
            if (ev.dstKind == TraceDstKind::None) {
                continue;
            }
            ++totals_.writesObserved;
            const SiteKey key{layerId, scopeIx, ev.streamOffset, static_cast<u8>(ev.dstKind),
                              ev.dstId};
            compare(siteThisTick_, key, Tag{ev.value.componentCount, ev.value.typeBank},
                    totals_.writeSiteIntraTickVariances, [&](Tag now, Tag before) {
                        char buf[224];
                        std::snprintf(buf, sizeof(buf),
                                      "t%d L%u %s site off=0x%x %s dst=0x%08x wrote (%u,0x%02x) "
                                      "but an earlier particle wrote (%u,0x%02x)",
                                      tickIndex_, layerId, kScopeNames[scopeIx], ev.streamOffset,
                                      opcodeName(ev.opcode), ev.dstId,
                                      static_cast<unsigned>(now.componentCount),
                                      static_cast<unsigned>(now.typeBank),
                                      static_cast<unsigned>(before.componentCount),
                                      static_cast<unsigned>(before.typeBank));
                        return std::string{buf};
                    });
        }
        trace_.clear();
    }

    const auto& touched = touchedFor(scope);
    for (u8 b = 0; b < kScopeRegisterBuckets; ++b) {
        const auto bankSpan = ctx.scopeRegisters[b];
        const std::size_t n = touched.unscannable ? bankSpan.size() : touched.banks[b].size();
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t i = touched.unscannable ? k : touched.banks[b][k];
            if (i >= bankSpan.size()) {
                continue;
            }
            const auto& rv = bankSpan[i];
            if (rv.typeBank == 0U && rv.componentCount == 0U) {
                continue;
            }
            ++totals_.registerCellsObserved;
            const RegKey key{layerId, scopeIx, b, static_cast<u32>(i)};
            compare(regThisTick_, key, Tag{rv.componentCount, rv.typeBank},
                    totals_.registerIntraTickVariances, [&](Tag now, Tag before) {
                        char buf[224];
                        std::snprintf(buf, sizeof(buf),
                                      "t%d L%u %s %s%zu holds (%u,0x%02x) but an earlier particle "
                                      "held (%u,0x%02x)",
                                      tickIndex_, layerId, kScopeNames[scopeIx], kBankNames[b], i,
                                      static_cast<unsigned>(now.componentCount),
                                      static_cast<unsigned>(now.typeBank),
                                      static_cast<unsigned>(before.componentCount),
                                      static_cast<unsigned>(before.typeBank));
                        return std::string{buf};
                    });
        }
    }

    for (std::size_t i = 0; i < ctx.externals.size(); ++i) {
        const auto& rv = ctx.externals[i];
        if (rv.typeBank == 0U && rv.componentCount == 0U) {
            continue;
        }
        ++totals_.externalCellsObserved;
        const ExtKey key{layerId, static_cast<u32>(i)};
        compare(extThisTick_, key, Tag{rv.componentCount, rv.typeBank},
                totals_.externalIntraTickVariances, [&](Tag now, Tag before) {
                    char buf[224];
                    std::snprintf(buf, sizeof(buf),
                                  "t%d L%u ext[%zu] holds (%u,0x%02x) but an earlier particle held "
                                  "(%u,0x%02x)",
                                  tickIndex_, layerId, i,
                                  static_cast<unsigned>(now.componentCount),
                                  static_cast<unsigned>(now.typeBank),
                                  static_cast<unsigned>(before.componentCount),
                                  static_cast<unsigned>(before.typeBank));
                    return std::string{buf};
                });
    }
}

void RegisterTagCensus::beginTick() {
    finish();
    ++tickIndex_;
}

void RegisterTagCensus::finish() {
    for (const auto& [key, tag] : regThisTick_) {
        const auto [it, fresh] = regAcrossTicks_.try_emplace(key, tag);
        if (!fresh && !(it->second == tag)) {
            ++totals_.registerCrossTickChanges;
            it->second = tag;
        }
    }
    for (const auto& [key, tag] : extThisTick_) {
        const auto [it, fresh] = extAcrossTicks_.try_emplace(key, tag);
        if (!fresh && !(it->second == tag)) {
            ++totals_.externalCrossTickChanges;
            it->second = tag;
        }
    }
    regThisTick_.clear();
    extThisTick_.clear();
    siteThisTick_.clear();
}

}
