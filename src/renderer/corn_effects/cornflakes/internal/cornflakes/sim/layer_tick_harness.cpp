#include <cornflakes/interface/binding/external_binding.hpp>
#include <cornflakes/interface/sim/layer_tick_harness.hpp>
#include <cornflakes/vm/bytecode_decoder.hpp>
#include <cornflakes/vm/cbem_interpreter.hpp>

#include <algorithm>

namespace whiteout::cornflakes {

LayerTickHarness::LayerTickHarness() : LayerTickHarness(Config{}) {}

LayerTickHarness::LayerTickHarness(const Config& cfg) : externals_(cfg.externalCount) {
    for (auto& bank : scopeRegisters_) {
        bank.resize(cfg.initialRegistersPerScope);
    }
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

    BytecodeExecContext ctx;
    for (std::size_t s = 0; s < kScopeRegisterBuckets; ++s) {
        ctx.scopeRegisters[s] =
            std::span<RegisterValue>{scopeRegisters_[s].data(), scopeRegisters_[s].size()};
    }
    ctx.externals = std::span<RegisterValue>{externals_.data(), externals_.size()};
    ctx.constantsPool = scope.constantsPool;
    ctx.functions = scope.functions;
    ctx.externalBindings = scope.externals;
    ctx.samplers = layer.samplers;
    ctx.spatialLayers = layer.spatialLayers;
    ctx.spatialHashes =
        std::span<ProximityHash* const>{spatialHashes_.data(), spatialHashes_.size()};
    ctx.rng = &rng_;
    ctx.effectAge = effectAge_;
    ctx.effectIsRunning = effectIsRunning_;
    ctx.sceneL2W = sceneL2W_;
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

    CBEMInterpreter vm;
    const auto executed = vm.run(instructions, ctx, issues);
    lastExecuted_ += executed;
    if (ctx.selfKillRequested) {

        markDead();
    }
    return !issues.hasFatal();
}

bool LayerTickHarness::initParticle(const LayerProgram& layer, IArena& arena, IssueBag& issues) {
    for (auto& bank : scopeRegisters_) {
        std::fill(bank.begin(), bank.end(), RegisterValue{});
    }
    std::fill(externals_.begin(), externals_.end(), RegisterValue{});

    for (const auto& attr : layer.attributeDefaults) {
        const auto* hit = findBindingAcrossScopes(layer, attr.name);
        if (hit == nullptr) {
            continue;
        }
        const u16 slot = resolveExternalSlot(*hit);
        if (slot >= externals_.size()) {
            continue;
        }
        RegisterValue& dst = externals_[slot];
        dst = RegisterValue{};
        dst.lanes[0] = attr.defaultValue[0];
        dst.lanes[1] = attr.defaultValue[1];
        dst.lanes[2] = attr.defaultValue[2];
        dst.lanes[3] = attr.defaultValue[3];
        dst.componentCount = 4;
    }

    for (const auto& evt : layer.eventExternals) {
        const auto* hit = findBindingAcrossScopes(layer, evt.externalName);
        if (hit == nullptr) {
            continue;
        }
        const u16 slot = resolveExternalSlot(*hit);
        if (slot >= externals_.size()) {
            continue;
        }
        externals_[slot] = RegisterValue::scalarI(static_cast<i32>(evt.globalEventSlotId));
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

    if (!layer.timeFixedProgram.cbemBytecode.empty()) {
        return runScope(layer.timeFixedProgram, layer, arena, issues);
    }
    if (!layer.timeVaryingProgram.cbemBytecode.empty()) {
        return runScope(layer.timeVaryingProgram, layer, arena, issues);
    }
    return runScope(layer.physicsProgram, layer, arena, issues);
}

} // namespace whiteout::cornflakes
