#include <cornflakes/interface/binding/effect_binder.hpp>
#include <cornflakes/interface/sim/effect_runtime.hpp>
#include <cornflakes/sim/spawn_processor.hpp>

#include <algorithm>
#include <cmath>

namespace whiteout::cornflakes {

namespace {

inline constexpr u32 kFibonacciHashStride = 0x9E3779B1U;

inline u32 layerSeedFor(u32 baseRngSeed, std::size_t layerIdx) noexcept {
    return baseRngSeed + static_cast<u32>(layerIdx) * kFibonacciHashStride;
}

} // namespace

EffectRuntime::EffectRuntime(const EffectAssetModel& model, EffectId effectId, IArena& bindArena,
                             IArena& frameArena, IssueBag& issues)
    : bindArena_(bindArena), frameArena_(frameArena) {
    EffectBinder binder;
    ownedPlan_ = binder.bind(model, effectId, bindArena_, issues);
    if (!ownedPlan_) {
        return;
    }
    plan_ = &*ownedPlan_;
    const std::size_t layerCount = plan_->layers.size();
    pools_.resize(layerCount);
    inputMaps_.resize(layerCount);
    perRendererInputMaps_.resize(layerCount);
    spawnQueues_.resize(layerCount);
    spawnHeads_.assign(layerCount, 0U);
    invLifeSlots_.assign(layerCount, kSlotUnbound);
    lifeRatioSlots_.assign(layerCount, kSlotUnbound);
    emitterScopeStates_.resize(layerCount);
    spatialHashesPerLayer_.resize(layerCount);

    for (std::size_t i = 0; i < layerCount; ++i) {
        setupLayerStorage(i);
        setupSelfLifeSlots(i);
        setupSpatialHashes(i);
    }
}

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
EffectRuntime::EffectRuntime(const EffectAssetModel& model, EffectId effectId, IArena& arena,
                             IssueBag& issues)
    : EffectRuntime(model, effectId, arena, arena, issues) {}
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

void EffectRuntime::setupLayerStorage(std::size_t layerIdx) {
    pools_[layerIdx].resize(1);
    pools_[layerIdx].resizeForLayer(plan_->layers[layerIdx]);

    const auto scopes = layerScopePrograms(plan_->layers[layerIdx]);
    for (std::size_t bucket = 0; bucket < kScopeRegisterBuckets; ++bucket) {
        std::size_t maxCount = 0;
        for (const auto* s : scopes) {
            const std::size_t c =
                (bucket + 1U < s->registerCounts.size()) ? s->registerCounts[bucket + 1U] : 0U;
            maxCount = std::max(maxCount, c);
        }
        emitterScopeStates_[layerIdx].registers[bucket].assign(maxCount, RegisterValue{});
    }

    std::size_t maxExternals = 0;
    for (const auto* s : scopes) {
        for (const auto& b : s->externals) {
            maxExternals = std::max(maxExternals, static_cast<std::size_t>(b.canonicalSlot) + 1U);
        }
    }
    emitterScopeStates_[layerIdx].externals.assign(maxExternals, RegisterValue{});
}

void EffectRuntime::setupSelfLifeSlots(std::size_t layerIdx) {
    const auto& lp = plan_->layers[layerIdx];

    if (auto* b = findBindingByName(lp.initProgram.externals, "self.invLife")) {
        invLifeSlots_[layerIdx] = b->canonicalSlot;
    } else if (auto* b = findBindingByName(lp.physicsProgram.externals, "self.invLife")) {
        invLifeSlots_[layerIdx] = b->canonicalSlot;
    }

    if (auto* b = findBindingByName(lp.physicsProgram.externals, "self.lifeRatio")) {
        lifeRatioSlots_[layerIdx] = b->canonicalSlot;
    } else if (auto* b = findBindingByName(lp.timeFixedProgram.externals, "self.lifeRatio")) {
        lifeRatioSlots_[layerIdx] = b->canonicalSlot;
    } else if (auto* b = findBindingByName(lp.timeVaryingProgram.externals, "self.lifeRatio")) {
        lifeRatioSlots_[layerIdx] = b->canonicalSlot;
    } else if (auto* b = findBindingByName(lp.initProgram.externals, "self.lifeRatio")) {
        lifeRatioSlots_[layerIdx] = b->canonicalSlot;
    }
}

void EffectRuntime::setupSpatialHashes(std::size_t layerIdx) {
    const auto& lp = plan_->layers[layerIdx];
    spatialHashesPerLayer_[layerIdx].clear();
    spatialHashesPerLayer_[layerIdx].reserve(lp.spatialLayers.size());

    for (const auto& sl : lp.spatialLayers) {
        ProximityHash* found = nullptr;
        for (std::size_t j = 0; j < spatialHashNames_.size(); ++j) {
            if (spatialHashNames_[j] == sl.name) {
                found = spatialHashesOwned_[j].get();
                break;
            }
        }
        if (found == nullptr) {
            spatialHashNames_.emplace_back(sl.name);
            spatialHashesOwned_.push_back(std::make_unique<ProximityHash>(sl.cellSize));
            found = spatialHashesOwned_.back().get();
        } else {
            found->setCellSize(sl.cellSize);
        }
        spatialHashesPerLayer_[layerIdx].push_back(found);
    }
}

bool EffectRuntime::isKickTarget(LayerId id) const noexcept {
    for (const auto& route : plan_->eventRouting.routes) {
        if (route.target.value == id.value && !route.channel.empty()) {
            return true;
        }
    }
    return false;
}

std::size_t EffectRuntime::layerCount() const noexcept {
    return plan_ != nullptr ? plan_->layers.size() : 0U;
}

void EffectRuntime::setPoolSize(std::size_t layerIdx, std::size_t count) {
    if (layerIdx >= pools_.size()) {
        return;
    }
    pools_[layerIdx].resize(count);
    if (plan_ != nullptr && layerIdx < plan_->layers.size()) {
        pools_[layerIdx].resizeForLayer(plan_->layers[layerIdx]);
    }
}

void EffectRuntime::setRenderInputMap(std::size_t layerIdx, const LayerRenderInputMap& mapping) {
    if (layerIdx >= inputMaps_.size()) {
        return;
    }
    inputMaps_[layerIdx] = mapping;
}

void EffectRuntime::setRenderInputMap(std::size_t layerIdx, std::size_t rendererIdx,
                                      const LayerRenderInputMap& mapping) {
    if (layerIdx >= perRendererInputMaps_.size()) {
        return;
    }
    auto& bucket = perRendererInputMaps_[layerIdx];
    if (rendererIdx >= bucket.size()) {
        bucket.resize(rendererIdx + 1U);
    }
    bucket[rendererIdx] = mapping;
}

void EffectRuntime::setAttribute(std::string_view name, const std::array<f32, 4>& value) {
    for (auto& o : attributeOverrides_) {
        if (o.first == name) {
            o.second = value;
            return;
        }
    }
    attributeOverrides_.emplace_back(std::string{name}, value);
}

bool EffectRuntime::setBackend(IRenderBackend* backend, IssueBag& issues) {
    backend_ = backend;
    backendPrepared_ = false;
    if (backend_ == nullptr) {
        return true;
    }
    return ensureBackendPrepared(issues);
}

void EffectRuntime::reset() noexcept {
    for (auto& pool : pools_) {
        for (std::size_t p = 0; p < pool.size(); ++p) {
            pool.particle(p).markDead();
        }
    }
    for (auto& q : spawnQueues_) {
        q.events.clear();
        q.dropped = 0U;
    }
    std::fill(spawnHeads_.begin(), spawnHeads_.end(), 0U);
    for (auto& state : emitterScopeStates_) {
        for (auto& bucket : state.registers) {
            std::fill(bucket.begin(), bucket.end(), RegisterValue{});
        }
        std::fill(state.externals.begin(), state.externals.end(), RegisterValue{});
    }
    nextSelfId_ = 1U;
    initialized_ = false;
}

bool EffectRuntime::ensureBackendPrepared(IssueBag& issues) {
    if (backendPrepared_ || backend_ == nullptr || plan_ == nullptr) {
        return backend_ == nullptr || backendPrepared_;
    }
    const bool ok = backend_->prepare(plan_->layers, issues);
    backendPrepared_ = ok;
    return ok;
}

void EffectRuntime::buildPackets(IArena& arena, IssueBag& issues) {
    lastPackets_.clear();
    if (plan_ == nullptr) {
        return;
    }
    lastPackets_.reserve(plan_->layers.size());
    for (std::size_t i = 0; i < plan_->layers.size(); ++i) {
        const auto& layer = plan_->layers[i];
        const auto& pool = pools_[i];

        if (layer.renderers.empty()) {
            continue;
        }

        const auto& perRendererMaps = perRendererInputMaps_[i];
        for (std::size_t r = 0; r < layer.renderers.size(); ++r) {
            const auto& renderer = layer.renderers[r];
            if (!renderer.isRenderingEnabled) {
                continue;
            }
            const LayerRenderInputMap& mapping =
                (r < perRendererMaps.size()) ? perRendererMaps[r] : inputMaps_[i];
            auto packet = extractFromPool(pool, layer, EmitterId{static_cast<u32>(i)}, renderer.cls,
                                          mapping, arena, issues);
            packet.blendMode = renderer.blendMode;
            packet.billboardingMode = static_cast<u8>(renderer.billboardingMode);
            lastPackets_.push_back(std::move(packet));
        }
    }
}

void EffectRuntime::initializeOnFirstTick(const EffectFrameInputs& inputs, IssueBag& issues) {
    for (std::size_t i = 0; i < plan_->layers.size(); ++i) {
        const auto& layer = plan_->layers[i];
        const bool isSpawner = layer.renderers.empty();
        const bool isRoot = !isKickTarget(layer.id);
        if (!(isSpawner && isRoot)) {
            for (std::size_t p = 0; p < pools_[i].size(); ++p) {
                pools_[i].particle(p).markDead();
            }
            continue;
        }

        if (pools_[i].size() != 1U) {
            pools_[i].resize(1);
            pools_[i].resizeForLayer(layer);
        }

        const u32 seed = layerSeedFor(inputs.baseRngSeed, i) + kRandStateSpawnAddend;
        for (std::size_t p = 0; p < pools_[i].size(); ++p) {
            pools_[i].particle(p).setSpatialHashes(std::span<ProximityHash* const>{
                spatialHashesPerLayer_[i].data(), spatialHashesPerLayer_[i].size()});
        }
        pools_[i].initBatch(layer, seed, frameArena_, issues);
        for (std::size_t p = 0; p < pools_[i].size(); ++p) {
            pools_[i].particle(p).setSceneL2W(inputs.emitterL2W);
            pools_[i].particle(p).setEffectAge(inputs.effectAge);
            pools_[i].particle(p).setEffectIsRunning(inputs.effectIsRunning);
        }
    }
}

void EffectRuntime::drainPendingSpawns(std::size_t i, const EffectFrameInputs& inputs,
                                       IssueBag& issues) {
    auto& q = spawnQueues_[i];
    if (q.events.empty()) {
        return;
    }
    const std::size_t cap = pools_[i].size();
    if (cap == 0U) {
        q.dropped += q.events.size();
        q.events.clear();
        return;
    }

    const auto& layer = plan_->layers[i];
    const u32 cap32 = static_cast<u32>(cap);
    const u32 layerRSM = layerSeedFor(inputs.baseRngSeed, i);

    for (const auto& ev : q.events) {
        // Linear-probe for the next dead slot starting from spawnHeads_.
        u32 slot = spawnHeads_[i] % cap32;
        u32 probed = 0U;
        while (probed < cap32 && !pools_[i].particle(slot).isDead()) {
            slot = (slot + 1U) % cap32;
            ++probed;
        }
        if (probed >= cap32) {
            ++q.dropped;
            continue;
        }
        spawnHeads_[i] = (slot + 1U) % cap32;

        auto& particle = pools_[i].particle(slot);
        particle.setSelfId(nextSelfId_++);
        particle.setParentIdentity(ev.parentSelfId, ev.parentRngState);

        if (ev.hasIntPayload) {
            particle.setSpawnIntPayload(ev.intPayloadWidth, ev.intPayload, ev.intPayloadId);
        } else {
            particle.clearSpawnIntPayload();
        }
        if (ev.hasBoolPayload) {
            particle.setSpawnBoolPayload(ev.boolPayloadWidth, ev.boolPayload, ev.boolPayloadId);
        } else {
            particle.clearSpawnBoolPayload();
        }

        particle.setSpawnPositionPayloadId(ev.hasSpawnPosition ? ev.spawnPositionPayloadId : 0U);
        particle.setSpawnOrientationPayloadId(ev.hasSpawnOrientation ? ev.spawnOrientationPayloadId
                                                                     : 0U);
        particle.setSceneL2W(inputs.emitterL2W);
        particle.setEffectAge(inputs.effectAge);
        particle.setTimeWindowEnd(ev.lerpedTime);
        particle.setEffectIsRunning(inputs.effectIsRunning);

        if (ev.hasSpawnPosition) {
            const std::array<f32, 4> spawnQuat = ev.hasSpawnOrientation
                                                     ? ev.spawnOrientation
                                                     : std::array<f32, 4>{0.0F, 0.0F, 0.0F, 1.0F};
            particle.setSpawnTRS(ev.spawnPosition, spawnQuat, {1.0F, 1.0F, 1.0F});
        } else if (ev.hasSpawnOrientation) {
            particle.setSpawnTRS({0.0F, 0.0F, 0.0F}, ev.spawnOrientation, {1.0F, 1.0F, 1.0F});
        }

        particle.setSpatialHashes(std::span<ProximityHash* const>{
            spatialHashesPerLayer_[i].data(), spatialHashesPerLayer_[i].size()});

        const u32 seed = ev.parentRngState + layerRSM + kRandStateSpawnAddend;
        pools_[i].initRange(layer, seed, slot, 1U, frameArena_, issues);
    }
    q.events.clear();
}

void EffectRuntime::prepareParticlesForTick(std::size_t i, const EffectFrameInputs& inputs) {
    const u16 invLifeSlot = invLifeSlots_[i];
    const u16 lifeRatioSlot = lifeRatioSlots_[i];
    for (std::size_t p = 0; p < pools_[i].size(); ++p) {
        auto& particle = pools_[i].particle(p);
        particle.setSceneL2W(inputs.emitterL2W);
        particle.setEffectAge(inputs.effectAge);
        particle.setEffectIsRunning(inputs.effectIsRunning);
        particle.setSpawnQueue(&spawnQueues_[i]);
        particle.setSpatialHashes(std::span<ProximityHash* const>{
            spatialHashesPerLayer_[i].data(), spatialHashesPerLayer_[i].size()});
        particle.noteFrameStartDeadState();

        if (invLifeSlot != kSlotUnbound) {
            const auto exts = particle.externals();
            if (invLifeSlot < exts.size()) {
                const f32 invLife = exts[invLifeSlot].lanes[0];
                particle.advanceLifeRatio(inputs.dt * invLife);
            }
        }
        if (lifeRatioSlot != kSlotUnbound) {
            auto exts = particle.externals();
            if (lifeRatioSlot < exts.size()) {
                exts[lifeRatioSlot] = RegisterValue::scalar(particle.lifeRatio());
            }
        }
    }
}

void EffectRuntime::injectSceneDt(std::size_t i, f32 dt) {
    const auto& layer = plan_->layers[i];
    const std::span<const ExternalBinding> sceneDtScopes[] = {
        layer.physicsProgram.externals,
        layer.timeFixedProgram.externals,
        layer.timeVaryingProgram.externals,
    };
    for (const auto& scope : sceneDtScopes) {
        const auto* hit = findBindingByName(scope, "scene.dt");
        if (hit == nullptr) {
            continue;
        }
        for (std::size_t p = 0; p < pools_[i].size(); ++p) {
            auto exts = pools_[i].particle(p).externals();
            if (hit->canonicalSlot < exts.size()) {
                exts[hit->canonicalSlot] = RegisterValue::scalar(dt);
            }
        }
    }
}

void EffectRuntime::applyAttributeOverrides(std::size_t i) {
    if (attributeOverrides_.empty()) {
        return;
    }
    const auto& layer = plan_->layers[i];
    for (const auto& [name, value] : attributeOverrides_) {
        const auto* hit = findBindingAcrossScopes(layer, name);
        if (hit == nullptr) {
            continue;
        }
        const u16 slot = resolveExternalSlot(*hit);
        for (std::size_t p = 0; p < pools_[i].size(); ++p) {
            auto exts = pools_[i].particle(p).externals();
            if (slot < exts.size()) {
                RegisterValue& dst = exts[slot];
                dst = RegisterValue{};
                dst.lanes[0] = value[0];
                dst.lanes[1] = value[1];
                dst.lanes[2] = value[2];
                dst.lanes[3] = value[3];
                dst.componentCount = 4;
            }
        }
    }
}

void EffectRuntime::routeEventsForLayer(std::size_t i) {
    auto& srcQ = spawnQueues_[i];
    if (srcQ.events.empty()) {
        return;
    }
    for (const auto& ev : srcQ.events) {
        for (const auto& route : plan_->eventRouting.routes) {
            if (route.globalEventSlotId != ev.eventId) {
                continue;
            }
            const std::uint32_t tgtIdx = route.target.value;
            if (tgtIdx >= spawnQueues_.size()) {
                continue;
            }
            auto& dstQ = spawnQueues_[tgtIdx];
            if (dstQ.capacity != 0U && dstQ.events.size() >= dstQ.capacity) {
                ++dstQ.dropped;
                continue;
            }
            SpawnEvent routed = ev;
            routed.sequenceIndex = static_cast<u32>(dstQ.events.size());
            dstQ.events.push_back(routed);
        }
    }
    srcQ.events.clear();
}

bool EffectRuntime::tick(const EffectFrameInputs& inputs, IssueBag& issues) {
    if (plan_ == nullptr) {
        return false;
    }

    for (auto& h : spatialHashesOwned_) {
        h->clear();
    }

    if (!initialized_) {
        initializeOnFirstTick(inputs, issues);
        initialized_ = true;
    }

    for (std::size_t i = 0; i < plan_->layers.size(); ++i) {
        const auto& layer = plan_->layers[i];
        const bool isSpawner = layer.renderers.empty();

        if (isKickTarget(layer.id)) {
            drainPendingSpawns(i, inputs, issues);
        } else {
            spawnQueues_[i].clear();
        }

        prepareParticlesForTick(i, inputs);
        injectSceneDt(i, inputs.dt);
        applyAttributeOverrides(i);

        const bool skipTick = isSpawner && !spawnerEnabled_;
        if (!skipTick) {
            (void)pools_[i].tickBatch(layer, frameArena_, issues);
        }

        routeEventsForLayer(i);
    }

    buildPackets(frameArena_, issues);
    if (backend_ != nullptr) {
        if (!ensureBackendPrepared(issues)) {
            return false;
        }
        backend_->submit(std::span<const RenderPacket>{lastPackets_.data(), lastPackets_.size()},
                         inputs.view, issues);
    }
    return true;
}

} // namespace whiteout::cornflakes
