#include <cornflakes/vm/bytecode_decoder.hpp>
#include <cornflakes/vm/cbem_interpreter.hpp>
#include <cornflakes/interface/binding/external_binding.hpp>
#include <cornflakes/interface/sim/layer_tick_harness.hpp>
#include <cornflakes/interface/simt/register_census.hpp>

#include <algorithm>

namespace whiteout::cornflakes {

LayerTickHarness::LayerTickHarness() : LayerTickHarness(Config{}) {}

LayerTickHarness::LayerTickHarness(const Config& cfg) {
    ownExternals_ = std::make_unique<ExternalStore>();
    ownExternals_->resize(cfg.externalCount, 1U);
    externalStore_ = ownExternals_.get();
    externalIndex_ = 0U;
    externalCount_ = cfg.externalCount;
    for (std::size_t b = 0; b < kScopeRegisterBuckets; ++b) {
        scopeRegisters_[b].resize(cfg.registersPerBank[b]);
    }
}

std::size_t LayerTickHarness::externalStorageSizeFor(const LayerProgram& layer) noexcept {
    std::size_t needed = 0;
    bool sawAnything = false;

    for (const auto* s : layerScopePrograms(layer)) {
        if (!s->externals.empty()) {
            sawAnything = true;
            needed = std::max(needed, s->externals.size());
            for (const auto& b : s->externals) {
                needed = std::max(needed, static_cast<std::size_t>(resolveExternalSlot(b)) + 1U);
            }
        }

        if (s->decodedInstructions.empty() && !s->cbemBytecode.empty()) {
            return kFallbackExternalCount;
        }
        for (const auto& ins : s->decodedInstructions) {
            u32 byteSlot = 0;
            switch (ins.opcode) {
            case Opcode::LoadExternal:
            case Opcode::StoreToExternal:
                if (ins.operandCount < 2) {
                    continue;
                }
                byteSlot = ins.operands[1];
                break;
            case Opcode::ExternalClear:
                if (ins.operandCount < 1) {
                    continue;
                }
                byteSlot = ins.operands[0];
                break;
            default:
                continue;
            }
            sawAnything = true;
            needed = std::max(needed, static_cast<std::size_t>(static_cast<u16>(byteSlot)) + 1U);
        }
    }

    if (!sawAnything) {
        return kFallbackExternalCount;
    }
    return std::min(needed, kFallbackExternalCount);
}

LayerTickHarness::Config LayerTickHarness::layoutFor(const LayerProgram& layer) noexcept {
    Config cfg;
    std::array<std::size_t, kScopeRegisterBuckets> perBank{};
    bool anyCount = false;
    for (const auto* s : layerScopePrograms(layer)) {
        for (std::size_t scopeIx = 0; scopeIx < kScopeRegisterBuckets; ++scopeIx) {
            const std::size_t count =
                (scopeIx + 1U < s->registerCounts.size()) ? s->registerCounts[scopeIx + 1U] : 0U;
            if (count > perBank[scopeIx]) {
                perBank[scopeIx] = count;
                anyCount = true;
            }
        }
    }
    if (anyCount) {
        cfg.registersPerBank = perBank;
    }
    cfg.externalCount = externalStorageSizeFor(layer);
    return cfg;
}

void LayerTickHarness::resizeForLayer(const LayerProgram& layer) {
    std::array<std::size_t, kScopeRegisterBuckets> maxPerScope{};
    bool anyCount = false;
    for (const auto* s : layerScopePrograms(layer)) {
        for (std::size_t scopeIx = 0; scopeIx < kScopeRegisterBuckets; ++scopeIx) {
            const u32 count =
                (scopeIx + 1U < s->registerCounts.size()) ? s->registerCounts[scopeIx + 1U] : 0U;
            if (count > maxPerScope[scopeIx]) {
                maxPerScope[scopeIx] = count;
                anyCount = true;
            }
        }
    }

    const std::size_t wantExternals = externalStorageSizeFor(layer);
    externalCount_ = wantExternals;
    if (ownExternals_ != nullptr && externalStore_ == ownExternals_.get()) {
        ownExternals_->resize(wantExternals, 1U);
    }

    if (!anyCount) {
        return;
    }
    for (std::size_t scopeIx = 0; scopeIx < kScopeRegisterBuckets; ++scopeIx) {
        scopeRegisters_[scopeIx].resize(maxPerScope[scopeIx]);
        scopeRegisters_[scopeIx].shrink_to_fit();
    }
}

bool LayerTickHarness::runScope(const VMProgramDescriptor& scope, const LayerProgram& layer,
                                IArena& arena, IssueBag& issues) {
    if (scope.cbemBytecode.empty()) {
        return true;
    }

    std::span<const CBEMInstruction> instructions = scope.decodedInstructions;
    if (instructions.empty()) {
        const auto prog = decodeBytecodeStream(scope.cbemBytecode, arena, issues);
        if (issues.hasFatal()) {
            return false;
        }
        instructions = prog.instructions;
    }
    lastInstructions_ += instructions.size();

    static thread_local BytecodeExecContext ctx;
    bindContext(scope, layer, ctx);

    auto* observer = simt::scopeRunObserver();
    if (observer != nullptr) {
        observer->onScopeBegin(layer, scope, ctx);
    }

    const CBEMInterpreter vm;
    const auto executed = vm.run(instructions, ctx, issues);
    lastExecuted_ += executed;
    if (observer != nullptr) {
        observer->onScopeEnd(layer, scope, ctx);
    }
    finishScope(ctx);
    return !issues.hasFatal();
}

void LayerTickHarness::bindContext(const VMProgramDescriptor& scope, const LayerProgram& layer,
                                   BytecodeExecContext& ctx) {
    ctx.resetPerScopeRun();
    for (std::size_t s = 0; s < kScopeRegisterBuckets; ++s) {
        ctx.scopeRegisters[s] =
            std::span<RegisterValue>{scopeRegisters_[s].data(), scopeRegisters_[s].size()};
    }
    ctx.externals = externals();
    ctx.constantsPool = scope.constantsPool;
    ctx.functions = scope.functions;
    ctx.externalBindings = scope.externals;
    ctx.samplers = layer.samplers;
    ctx.spatialLayers = layer.spatialLayers;
    ctx.kickedEventDecls = layer.kickedEventDecls;
    ctx.rootEventDecl = layer.rootEventDecl;
    ctx.spatialHashes =
        std::span<ProximityHash* const>{spatialHashes_.data(), spatialHashes_.size()};
    ctx.spatialHashesWrite =
        std::span<ProximityHash* const>{spatialHashesWrite_.data(), spatialHashesWrite_.size()};
    ctx.rng = &rng_;
    ctx.effectAge = effectAge_;
    ctx.effectIsRunning = effectIsRunning_;
    ctx.sceneL2W = sceneL2W_;
    ctx.cameras = cameras_;
    ctx.simLod = simLod_;
    ctx.simLodDistanceMin = simLodDistanceMin_;
    ctx.simLodDistanceMax = simLodDistanceMax_;
    ctx.spawnTranslate = spawnTranslate_;
    ctx.spawnQuat = spawnQuat_;
    ctx.spawnScale = spawnScale_;
    ctx.inInitScope = inInitScope_;
    ctx.trace = trace_;
    ctx.spawnQueue = spawnQueue_;
    ctx.timeWindowEnd = timeWindowEnd_;
    ctx.timeWindowStart = timeWindowStart_;
    ctx.currentSelfId = selfId_;
    ctx.hasSpawnIntPayload = hasSpawnIntPayload_;
    ctx.spawnIntPayloadWidth = spawnIntPayloadWidth_;
    ctx.spawnIntPayload = spawnIntPayload_;
    ctx.spawnIntPayloadId = spawnIntPayloadId_;
    ctx.hasSpawnBoolPayload = hasSpawnBoolPayload_;
    ctx.spawnBoolPayloadWidth = spawnBoolPayloadWidth_;
    ctx.spawnBoolPayload = spawnBoolPayload_;
    ctx.spawnBoolPayloadId = spawnBoolPayloadId_;
    ctx.spawnPositionPayloadId = spawnPositionPayloadId_;
    ctx.spawnOrientationPayloadId = spawnOrientationPayloadId_;
    ctx.spawnFloatSlots = spawnFloatSlots_;

}

void LayerTickHarness::finishScope(const BytecodeExecContext& ctx) {
    if (ctx.selfKillRequested) {
        markDead();
    }
}

bool LayerTickHarness::initParticle(const LayerProgram& layer, IArena& arena, IssueBag& issues) {
    for (auto& bank : scopeRegisters_) {
        std::fill(bank.begin(), bank.end(), RegisterValue{});
    }
    externals().clear();

    for (const auto& attr : layer.attributeDefaults) {
        const auto* hit = findBindingAcrossScopes(layer, attr.name);
        if (hit == nullptr) {
            continue;
        }
        const u16 slot = resolveExternalSlot(*hit);
        if (slot >= externalCount_) {
            continue;
        }
        RegisterValue dst;
        dst.lanes[0] = attr.defaultValue[0];
        dst.lanes[1] = attr.defaultValue[1];
        dst.lanes[2] = attr.defaultValue[2];
        dst.lanes[3] = attr.defaultValue[3];
        dst.componentCount = 4;
        externals().set(slot, dst);
    }

    for (const auto& evt : layer.eventExternals) {
        const auto* hit = findBindingAcrossScopes(layer, evt.externalName);
        if (hit == nullptr) {
            continue;
        }
        const u16 slot = resolveExternalSlot(*hit);
        if (slot >= externalCount_) {
            continue;
        }
        externals().set(slot, RegisterValue::scalarI(static_cast<i32>(evt.globalEventSlotId)));
    }

    if (const auto* st = findBindingByName(layer.initProgram.externals, "scene.time")) {
        const u16 slot = resolveExternalSlot(*st);
        if (slot < externalCount_) {
            externals().set(slot, RegisterValue::scalar(initSceneTime_));
        }
    }

    lastExecuted_ = 0;
    lastInstructions_ = 0;
    lifeRatio_ = 0.0F;
    inInitScope_ = true;
    const bool ok = runScope(layer.initProgram, layer, arena, issues);
    inInitScope_ = false;
    return ok;
}

bool LayerTickHarness::tick(const LayerProgram& layer, IArena& arena, IssueBag& issues) {
    lastExecuted_ = 0;
    lastInstructions_ = 0;

    return runScope(layer.evolveProgram(), layer, arena, issues);
}

}
