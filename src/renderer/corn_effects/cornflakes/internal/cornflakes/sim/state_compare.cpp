#include <cornflakes/interface/sim/effect_runtime.hpp>
#include <cornflakes/interface/sim/state_compare.hpp>

#include <cstdio>
#include <cstring>

namespace whiteout::cornflakes {

namespace {

std::string hexU32(u32 v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08x", v);
    return buf;
}

std::string hexU64(u64 v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string showF32(f32 v) {
    u32 bits = 0U;
    std::memcpy(&bits, &v, sizeof(bits));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.9g (0x%08x)", static_cast<double>(v), bits);
    return buf;
}

std::string showSize(std::size_t v) {
    return std::to_string(v);
}

bool sameF32Bits(f32 x, f32 y) noexcept {
    u32 bx = 0U;
    u32 by = 0U;
    std::memcpy(&bx, &x, sizeof(bx));
    std::memcpy(&by, &y, sizeof(by));
    return bx == by;
}

RuntimeDivergence make(DivergenceKind kind, std::size_t layer, std::size_t slot, std::string a,
                       std::string b) {
    RuntimeDivergence d;
    d.diverged = true;
    d.kind = kind;
    d.layerIndex = layer;
    d.slotIndex = slot;
    d.valueA = std::move(a);
    d.valueB = std::move(b);
    return d;
}

template <class Regs>
RuntimeDivergence compareBank(DivergenceKind countKind, DivergenceKind valueKind, std::size_t layer,
                              std::size_t slot, const Regs& a, const Regs& b) {
    if (a.size() != b.size()) {
        const auto& longer = (a.size() > b.size()) ? a : b;
        for (std::size_t i = (a.size() < b.size() ? a.size() : b.size()); i < longer.size(); ++i) {
            const RegisterValue rv = longer[i];
            bool written = rv.componentCount != 0U || rv.typeBank != 0U;
            for (const f32 lane : rv.lanes) {
                written = written || !sameF32Bits(lane, 0.0F);
            }
            if (written) {
                auto d = make(countKind, layer, slot, showSize(a.size()), showSize(b.size()));
                d.elementIndex = i;
                return d;
            }
        }
    }
    const std::size_t n = (a.size() < b.size()) ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t lane = 0; lane < 4; ++lane) {
            if (!sameF32Bits(a[i].lanes[lane], b[i].lanes[lane])) {
                auto d = make(valueKind, layer, slot, showF32(a[i].lanes[lane]),
                              showF32(b[i].lanes[lane]));
                d.elementIndex = i;
                d.lane = lane;
                return d;
            }
        }
        if (a[i].componentCount != b[i].componentCount) {
            auto d = make(valueKind, layer, slot, "componentCount=" + showSize(a[i].componentCount),
                          "componentCount=" + showSize(b[i].componentCount));
            d.elementIndex = i;
            return d;
        }
        if (a[i].typeBank != b[i].typeBank) {
            auto d = make(valueKind, layer, slot, "typeBank=" + showSize(a[i].typeBank),
                          "typeBank=" + showSize(b[i].typeBank));
            d.elementIndex = i;
            return d;
        }
    }
    return {};
}

RuntimeDivergence compareSpawnEvent(std::size_t layer, std::size_t idx, const SpawnEvent& a,
                                    const SpawnEvent& b) {
    const auto fail = [&](const std::string& what, const std::string& va, const std::string& vb) {
        auto d = make(DivergenceKind::SpawnEventValue, layer, RuntimeDivergence::kNoIndex,
                      what + "=" + va, what + "=" + vb);
        d.elementIndex = idx;
        return d;
    };
    if (a.eventId != b.eventId) {
        return fail("eventId", showSize(a.eventId), showSize(b.eventId));
    }
    if (a.sequenceIndex != b.sequenceIndex) {
        return fail("sequenceIndex", showSize(a.sequenceIndex), showSize(b.sequenceIndex));
    }
    if (a.parentSelfId != b.parentSelfId) {
        return fail("parentSelfId", hexU64(a.parentSelfId), hexU64(b.parentSelfId));
    }
    if (a.parentRngState != b.parentRngState) {
        return fail("parentRngState", hexU32(a.parentRngState), hexU32(b.parentRngState));
    }
    for (std::size_t i = 0; i < 3; ++i) {
        if (!sameF32Bits(a.spawnPosition[i], b.spawnPosition[i])) {
            return fail("spawnPosition[" + showSize(i) + "]", showF32(a.spawnPosition[i]),
                        showF32(b.spawnPosition[i]));
        }
    }
    if (!sameF32Bits(a.lerpedTime, b.lerpedTime)) {
        return fail("lerpedTime", showF32(a.lerpedTime), showF32(b.lerpedTime));
    }
    return {};
}

}

const char* divergenceKindName(DivergenceKind kind) noexcept {
    switch (kind) {
    case DivergenceKind::None:
        return "none";
    case DivergenceKind::LayerCount:
        return "layer-count";
    case DivergenceKind::PoolSize:
        return "pool-size";
    case DivergenceKind::SlotValidity:
        return "slot-validity";
    case DivergenceKind::RngState:
        return "rng-state";
    case DivergenceKind::LifeRatio:
        return "life-ratio";
    case DivergenceKind::SelfId:
        return "self-id";
    case DivergenceKind::ParentSelfId:
        return "parent-self-id";
    case DivergenceKind::DeadFlag:
        return "dead-flag";
    case DivergenceKind::FrameStartDeadFlag:
        return "frame-start-dead-flag";
    case DivergenceKind::FrameUniform:
        return "frame-uniform";
    case DivergenceKind::ExternalCount:
        return "external-count";
    case DivergenceKind::ExternalValue:
        return "external";
    case DivergenceKind::StreamRegisterCount:
        return "stream-register-count";
    case DivergenceKind::StreamRegisterValue:
        return "stream-register";
    case DivergenceKind::LocalRegisterCount:
        return "local-register-count";
    case DivergenceKind::LocalRegisterValue:
        return "local-register";
    case DivergenceKind::SpawnQueueLength:
        return "spawn-queue-length";
    case DivergenceKind::SpawnEventValue:
        return "spawn-event";
    case DivergenceKind::SpatialHashCount:
        return "spatial-hash-count";
    case DivergenceKind::SpatialHashName:
        return "spatial-hash-name";
    case DivergenceKind::SpatialEntryCount:
        return "spatial-entry-count";
    case DivergenceKind::SpatialEntryValue:
        return "spatial-entry";
    }
    return "?";
}

std::string RuntimeDivergence::describe() const {
    if (!diverged) {
        return "identical";
    }
    std::string out = "tick=" + std::to_string(tick) + " layer=" + std::to_string(layerIndex);
    if (slotIndex != kNoIndex) {
        out += " slot=" + std::to_string(slotIndex);
    }
    out += " ";
    out += divergenceKindName(kind);
    if (elementIndex != kNoIndex) {
        out += "[" + std::to_string(elementIndex) + "]";
    }
    if (lane != kNoIndex) {
        out += ".lane" + std::to_string(lane);
    }
    out += "  A=" + valueA + "  B=" + valueB;
    return out;
}

RuntimeDivergence compareRuntimes(const EffectRuntime& a, const EffectRuntime& b) {
    if (a.layerCount() != b.layerCount()) {
        return make(DivergenceKind::LayerCount, RuntimeDivergence::kNoIndex,
                    RuntimeDivergence::kNoIndex, showSize(a.layerCount()),
                    showSize(b.layerCount()));
    }

    for (std::size_t layer = 0; layer < a.layerCount(); ++layer) {
        if (a.poolSize(layer) != b.poolSize(layer)) {
            return make(DivergenceKind::PoolSize, layer, RuntimeDivergence::kNoIndex,
                        showSize(a.poolSize(layer)), showSize(b.poolSize(layer)));
        }

        for (std::size_t slot = 0; slot < a.poolSize(layer); ++slot) {
            const auto pa = a.inspectParticle(layer, slot);
            const auto pb = b.inspectParticle(layer, slot);
            if (pa.valid != pb.valid) {
                return make(DivergenceKind::SlotValidity, layer, slot, showSize(pa.valid),
                            showSize(pb.valid));
            }
            if (!pa.valid) {
                continue;
            }
            if (pa.rngState != pb.rngState) {
                return make(DivergenceKind::RngState, layer, slot, hexU32(pa.rngState),
                            hexU32(pb.rngState));
            }
            if (!sameF32Bits(pa.lifeRatio, pb.lifeRatio)) {
                return make(DivergenceKind::LifeRatio, layer, slot, showF32(pa.lifeRatio),
                            showF32(pb.lifeRatio));
            }
            if (pa.selfId != pb.selfId) {
                return make(DivergenceKind::SelfId, layer, slot, hexU64(pa.selfId),
                            hexU64(pb.selfId));
            }
            if (pa.parentSelfId != pb.parentSelfId) {
                return make(DivergenceKind::ParentSelfId, layer, slot, hexU64(pa.parentSelfId),
                            hexU64(pb.parentSelfId));
            }
            if (pa.dead != pb.dead) {
                return make(DivergenceKind::DeadFlag, layer, slot, showSize(pa.dead),
                            showSize(pb.dead));
            }
            if (pa.wasDeadAtFrameStart != pb.wasDeadAtFrameStart) {
                return make(DivergenceKind::FrameStartDeadFlag, layer, slot,
                            showSize(pa.wasDeadAtFrameStart), showSize(pb.wasDeadAtFrameStart));
            }

            if (!sameF32Bits(pa.effectAge, pb.effectAge)) {
                return make(DivergenceKind::FrameUniform, layer, slot,
                            "effectAge=" + showF32(pa.effectAge),
                            "effectAge=" + showF32(pb.effectAge));
            }
            if (pa.effectIsRunning != pb.effectIsRunning) {
                return make(DivergenceKind::FrameUniform, layer, slot,
                            "effectIsRunning=" + showSize(pa.effectIsRunning),
                            "effectIsRunning=" + showSize(pb.effectIsRunning));
            }
            if (!sameF32Bits(pa.simLod, pb.simLod)) {
                return make(DivergenceKind::FrameUniform, layer, slot,
                            "simLod=" + showF32(pa.simLod), "simLod=" + showF32(pb.simLod));
            }
            if (!sameF32Bits(pa.simLodDistanceMin, pb.simLodDistanceMin) ||
                !sameF32Bits(pa.simLodDistanceMax, pb.simLodDistanceMax)) {
                return make(DivergenceKind::FrameUniform, layer, slot,
                            "simLodDistance=" + showF32(pa.simLodDistanceMin) + ".." +
                                showF32(pa.simLodDistanceMax),
                            "simLodDistance=" + showF32(pb.simLodDistanceMin) + ".." +
                                showF32(pb.simLodDistanceMax));
            }
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 4; ++c) {
                    if (!sameF32Bits(pa.sceneL2W.m[r][c], pb.sceneL2W.m[r][c])) {
                        auto d = make(DivergenceKind::FrameUniform, layer, slot,
                                      showF32(pa.sceneL2W.m[r][c]), showF32(pb.sceneL2W.m[r][c]));
                        d.elementIndex =
                            (static_cast<std::size_t>(r) * 4U) + static_cast<std::size_t>(c);
                        return d;
                    }
                }
            }

            if (auto d = compareBank(DivergenceKind::ExternalCount, DivergenceKind::ExternalValue,
                                     layer, slot, pa.externals, pb.externals);
                d.diverged) {
                return d;
            }
            if (auto d = compareBank(DivergenceKind::StreamRegisterCount,
                                     DivergenceKind::StreamRegisterValue, layer, slot,
                                     pa.streamRegisters, pb.streamRegisters);
                d.diverged) {
                return d;
            }
            if (auto d = compareBank(DivergenceKind::LocalRegisterCount,
                                     DivergenceKind::LocalRegisterValue, layer, slot,
                                     pa.localRegisters, pb.localRegisters);
                d.diverged) {
                return d;
            }
        }

        const auto qa = a.spawnQueueEvents(layer);
        const auto qb = b.spawnQueueEvents(layer);
        if (qa.size() != qb.size()) {
            return make(DivergenceKind::SpawnQueueLength, layer, RuntimeDivergence::kNoIndex,
                        showSize(qa.size()), showSize(qb.size()));
        }
        for (std::size_t i = 0; i < qa.size(); ++i) {
            if (auto d = compareSpawnEvent(layer, i, qa[i], qb[i]); d.diverged) {
                return d;
            }
        }
    }

    if (a.spatialHashCount() != b.spatialHashCount()) {
        return make(DivergenceKind::SpatialHashCount, RuntimeDivergence::kNoIndex,
                    RuntimeDivergence::kNoIndex, showSize(a.spatialHashCount()),
                    showSize(b.spatialHashCount()));
    }
    for (std::size_t h = 0; h < a.spatialHashCount(); ++h) {
        if (a.spatialHashName(h) != b.spatialHashName(h)) {
            auto d = make(DivergenceKind::SpatialHashName, RuntimeDivergence::kNoIndex,
                          RuntimeDivergence::kNoIndex, std::string{a.spatialHashName(h)},
                          std::string{b.spatialHashName(h)});
            d.elementIndex = h;
            return d;
        }
        const auto ea = a.spatialHashEntries(h);
        const auto eb = b.spatialHashEntries(h);
        if (ea.size() != eb.size()) {
            auto d = make(DivergenceKind::SpatialEntryCount, RuntimeDivergence::kNoIndex,
                          RuntimeDivergence::kNoIndex, showSize(ea.size()), showSize(eb.size()));
            d.elementIndex = h;
            return d;
        }
        for (std::size_t i = 0; i < ea.size(); ++i) {
            if (ea[i].insertSeq != eb[i].insertSeq) {
                auto d = make(DivergenceKind::SpatialEntryValue, RuntimeDivergence::kNoIndex,
                              RuntimeDivergence::kNoIndex, "insertSeq=" + showSize(ea[i].insertSeq),
                              "insertSeq=" + showSize(eb[i].insertSeq));
                d.elementIndex = i;
                return d;
            }
            for (std::size_t c = 0; c < 3; ++c) {
                if (!sameF32Bits(ea[i].position[c], eb[i].position[c])) {
                    auto d = make(DivergenceKind::SpatialEntryValue, RuntimeDivergence::kNoIndex,
                                  RuntimeDivergence::kNoIndex, showF32(ea[i].position[c]),
                                  showF32(eb[i].position[c]));
                    d.elementIndex = i;
                    d.lane = c;
                    return d;
                }
            }
            if (ea[i].sourceSelfId != eb[i].sourceSelfId) {
                auto d = make(DivergenceKind::SpatialEntryValue, RuntimeDivergence::kNoIndex,
                              RuntimeDivergence::kNoIndex, hexU64(ea[i].sourceSelfId),
                              hexU64(eb[i].sourceSelfId));
                d.elementIndex = i;
                return d;
            }
        }
    }

    return {};
}

}
