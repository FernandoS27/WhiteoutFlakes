#include <cornflakes/interface/binding/effect_binder.hpp>
#include <cornflakes/interface/sim/effect_runtime.hpp>
#include <cornflakes/sim/spawn_processor.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace whiteout::cornflakes {

namespace {

inline constexpr u32 kFibonacciHashStride = 0x9E3779B1U;

inline u32 layerSeedFor(u32 baseRngSeed, std::size_t layerIdx) noexcept {
    return baseRngSeed + static_cast<u32>(layerIdx) * kFibonacciHashStride;
}

}

EffectRuntime::EffectRuntime(const EffectAssetModel& model, EffectId effectId, IArena& bindArena,
                             IArena& frameArena, IssueBag& issues,
                             IMeshResourceProvider* meshProvider,
                             ITextureResourceProvider* textureProvider,
                             IVectorFieldProvider* vectorFieldProvider)
    : bindArena_(bindArena), frameArena_(frameArena) {
    EffectBinder binder;
    binder.setMeshProvider(meshProvider);
    binder.setTextureProvider(textureProvider);
    binder.setVectorFieldProvider(vectorFieldProvider);
    ownedPlan_ = binder.bind(model, effectId, bindArena_, issues);
    if (!ownedPlan_) {
        return;
    }
    plan_ = &*ownedPlan_;
    const std::size_t layerCount = plan_->layers.size();
    pools_.resize(layerCount);
    inputMaps_.resize(layerCount);
    perRendererInputMaps_.resize(layerCount);
    layerModels_.assign(layerCount, LayerModel::ModelA);
    spawnQueues_.resize(layerCount);
    spawnHeads_.assign(layerCount, 0U);
    spawnedTotals_.assign(layerCount, 0U);
    invLifeSlots_.assign(layerCount, kSlotUnbound);
    lifeRatioSlots_.assign(layerCount, kSlotUnbound);
    emitterScopeStates_.resize(layerCount);
    spatialHashesPerLayer_.resize(layerCount);
    spatialHashesAltPerLayer_.resize(layerCount);

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

std::span<ProximityHash* const> EffectRuntime::spatialReadHashes(
    std::size_t layerIdx) const noexcept {
    const auto& v =
        spatialReadIsAlt_ ? spatialHashesAltPerLayer_[layerIdx] : spatialHashesPerLayer_[layerIdx];
    return std::span<ProximityHash* const>{v.data(), v.size()};
}

std::span<ProximityHash* const> EffectRuntime::spatialWriteHashes(
    std::size_t layerIdx) const noexcept {
    const auto& v =
        spatialReadIsAlt_ ? spatialHashesPerLayer_[layerIdx] : spatialHashesAltPerLayer_[layerIdx];
    return std::span<ProximityHash* const>{v.data(), v.size()};
}

void EffectRuntime::setupSpatialHashes(std::size_t layerIdx) {
    const auto& lp = plan_->layers[layerIdx];
    spatialHashesPerLayer_[layerIdx].clear();
    spatialHashesPerLayer_[layerIdx].reserve(lp.spatialLayers.size());
    spatialHashesAltPerLayer_[layerIdx].clear();
    spatialHashesAltPerLayer_[layerIdx].reserve(lp.spatialLayers.size());

    for (const auto& sl : lp.spatialLayers) {
        const std::string_view identity = sl.fullName.empty() ? sl.name : sl.fullName;
        std::size_t slot = spatialHashNames_.size();
        for (std::size_t j = 0; j < spatialHashNames_.size(); ++j) {
            if (spatialHashNames_[j] == identity) {
                slot = j;
                break;
            }
        }
        if (slot == spatialHashNames_.size()) {
            spatialHashNames_.emplace_back(identity);
            spatialHashesOwned_.push_back(std::make_unique<ProximityHash>(sl.cellSize));
            spatialHashesOwnedAlt_.push_back(std::make_unique<ProximityHash>(sl.cellSize));
        } else {
            spatialHashesOwned_[slot]->setCellSize(sl.cellSize);
            spatialHashesOwnedAlt_[slot]->setCellSize(sl.cellSize);
        }
        spatialHashesPerLayer_[layerIdx].push_back(spatialHashesOwned_[slot].get());
        spatialHashesAltPerLayer_[layerIdx].push_back(spatialHashesOwnedAlt_[slot].get());
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

std::size_t EffectRuntime::aliveCount(std::size_t layerIdx) const noexcept {
    if (layerIdx >= pools_.size()) {
        return 0U;
    }
    const auto& pool = pools_[layerIdx];
    std::size_t n = 0U;
    for (std::size_t i = 0; i < pool.size(); ++i) {
        if (!pool.particle(i).isDead()) {
            ++n;
        }
    }
    return n;
}

u64 EffectRuntime::spawnedTotal(std::size_t layerIdx) const noexcept {
    return layerIdx < spawnedTotals_.size() ? spawnedTotals_[layerIdx] : 0U;
}

std::size_t EffectRuntime::poolSize(std::size_t layerIdx) const noexcept {
    return layerIdx < pools_.size() ? pools_[layerIdx].size() : 0U;
}

std::span<const SpawnEvent> EffectRuntime::spawnQueueEvents(std::size_t layerIdx) const noexcept {
    if (layerIdx >= spawnQueues_.size()) {
        return {};
    }
    const auto& q = spawnQueues_[layerIdx].events;
    return std::span<const SpawnEvent>{q.data(), q.size()};
}

std::size_t EffectRuntime::spatialHashCount() const noexcept {
    return spatialHashesOwned_.size();
}

std::string_view EffectRuntime::spatialHashName(std::size_t idx) const noexcept {
    return idx < spatialHashNames_.size() ? std::string_view{spatialHashNames_[idx]}
                                          : std::string_view{};
}

std::vector<ProximityEntry> EffectRuntime::spatialHashEntries(std::size_t idx) const {
    const auto& side = spatialReadIsAlt_ ? spatialHashesOwnedAlt_ : spatialHashesOwned_;
    if (idx >= side.size() || side[idx] == nullptr) {
        return {};
    }
    return side[idx]->entriesInInsertionOrder();
}

ParticleInspection EffectRuntime::inspectParticle(std::size_t layerIdx,
                                                  std::size_t slotIdx) const noexcept {
    ParticleInspection out;
    if (layerIdx >= pools_.size()) {
        return out;
    }
    const auto& pool = pools_[layerIdx];
    if (slotIdx >= pool.size()) {
        return out;
    }
    const auto& particle = pool.particle(slotIdx);

    out.valid = true;
    out.layerIndex = layerIdx;
    out.slotIndex = slotIdx;
    out.externals = particle.externals();
    out.streamRegisters = pool.materialiseBank(slotIdx, scope::kStream);
    out.localRegisters = pool.materialiseBank(slotIdx, scope::kLocal);
    out.sceneL2W = particle.sceneL2W();
    out.simLod = particle.simLod();
    out.simLodDistanceMin = particle.simLodDistanceMin();
    out.simLodDistanceMax = particle.simLodDistanceMax();
    out.effectAge = particle.effectAge();
    out.effectIsRunning = particle.effectIsRunning();
    out.rngState = particle.rngState();
    out.lifeRatio = particle.lifeRatio();
    out.selfId = particle.selfId();
    out.parentSelfId = particle.parentSelfId();
    out.dead = particle.isDead();
    out.wasDeadAtFrameStart = particle.wasDeadAtFrameStart();
    return out;
}

u16 EffectRuntime::positionSlotFor(std::size_t layerIdx) const noexcept {
    if (plan_ == nullptr || layerIdx >= plan_->layers.size()) {
        return kSlotUnbound;
    }
    const auto& layer = plan_->layers[layerIdx];
    const ExternalBinding* pos = nullptr;
    for (const auto* scope :
         {&layer.initProgram.externals, &layer.physicsProgram.externals,
          &layer.timeFixedProgram.externals, &layer.timeVaryingProgram.externals}) {
        for (const auto& b : *scope) {
            const bool suffix =
                b.name.size() >= 10U && b.name.substr(b.name.size() - 10U) == "__Position";
            const bool prefix = b.name.size() >= 9U && b.name.substr(0U, 9U) == "Position_";
            if (suffix || prefix) {
                pos = &b;
                break;
            }
        }
        if (pos != nullptr) {
            break;
        }
    }
    if (pos == nullptr) {
        return kSlotUnbound;
    }
    return (pos->canonicalSlot == 0U && pos->slot != 0U) ? pos->slot : pos->canonicalSlot;
}

u32 EffectRuntime::positionSpread(std::size_t layerIdx, f32 outMin[3],
                                  f32 outMax[3]) const noexcept {
    if (plan_ == nullptr || layerIdx >= plan_->layers.size() || layerIdx >= pools_.size()) {
        return 0U;
    }
    const u16 slot = positionSlotFor(layerIdx);
    if (slot == kSlotUnbound) {
        return 0U;
    }
    const auto& pool = pools_[layerIdx];
    u32 count = 0U;
    f32 lo[3] = {1e30F, 1e30F, 1e30F};
    f32 hi[3] = {-1e30F, -1e30F, -1e30F};
    for (std::size_t i = 0; i < pool.size(); ++i) {
        const auto& particle = pool.particle(i);
        if (particle.isDead()) {
            continue;
        }
        const auto exts = particle.externals();
        if (slot >= exts.size()) {
            continue;
        }
        const RegisterValue v = exts[slot];
        for (int c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], v.lanes[c]);
            hi[c] = std::max(hi[c], v.lanes[c]);
        }
        ++count;
    }
    if (count > 0U) {
        for (int c = 0; c < 3; ++c) {
            outMin[c] = lo[c];
            outMax[c] = hi[c];
        }
    }
    return count;
}

void EffectRuntime::setLayerModel(std::size_t layerIdx, LayerModel model) {
    if (layerIdx >= layerModels_.size()) {
        return;
    }
    layerModels_[layerIdx] = model;
    if (model == LayerModel::ModelB && plan_ != nullptr && layerIdx < plan_->layers.size() &&
        layerIdx < pools_.size()) {
        pools_[layerIdx].enablePoolRegisters(plan_->layers[layerIdx]);
    }
}

EffectRuntime::LayerModel EffectRuntime::layerModel(std::size_t layerIdx) const noexcept {
    return layerIdx < layerModels_.size() ? layerModels_[layerIdx] : LayerModel::ModelA;
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
    sceneTime_ = 0.0F;
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
            packet.rendererIndex = static_cast<u32>(r);
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
            pools_[i].particle(p).setSpatialHashes(spatialReadHashes(i), spatialWriteHashes(i));
            pools_[i].particle(p).setInitSceneTime(sceneTime_);
        }
        pools_[i].initBatch(layer, seed, frameArena_, issues);
        for (std::size_t p = 0; p < pools_[i].size(); ++p) {
            pools_[i].particle(p).setSceneL2W(inputs.emitterL2W);
            pools_[i].particle(p).setCameras(cameras());
            pools_[i].particle(p).setSimLod(layerLodFor(i));
            pools_[i].particle(p).setSimLodDistances(lodConfig_.minDist, lodConfig_.maxDist);
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

    std::vector<SpawnEvent> incoming;
    incoming.swap(q.events);

    const auto& layer = plan_->layers[i];
    u32 cap32 = static_cast<u32>(cap);
    const u32 layerRSM = layerSeedFor(inputs.baseRngSeed, i);

    static constexpr std::size_t kMaxPoolPerLayer = 1U << 17U;

    for (const auto& ev : incoming) {
        u32 slot = spawnHeads_[i] % cap32;
        u32 probed = 0U;
        while (probed < cap32 && !pools_[i].particle(slot).isDead()) {
            slot = (slot + 1U) % cap32;
            ++probed;
        }
        if (probed >= cap32) {
            const std::size_t oldCap = pools_[i].size();
            if (oldCap >= kMaxPoolPerLayer) {
                static bool warnedCap = false;
                if (!warnedCap) {
                    warnedCap = true;
                    std::fprintf(
                        stderr,
                        "[cornflakes] layer %zu pool hit cap %zu; dropping further spawns\n", i,
                        kMaxPoolPerLayer);
                }
                ++q.dropped;
                continue;
            }
            const std::size_t newCap =
                std::min(kMaxPoolPerLayer, std::max(oldCap * 2U, oldCap + 1U));
            pools_[i].resize(newCap);
            pools_[i].resizeForLayer(layer);
            for (std::size_t np = oldCap; np < newCap; ++np) {
                pools_[i].particle(np).markDead();
            }
            cap32 = static_cast<u32>(newCap);
            slot = static_cast<u32>(oldCap);
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
        particle.setSpawnFloatSlots(ev.floatSlots);
        particle.setSceneL2W(inputs.emitterL2W);
        particle.setCameras(cameras());
        particle.setSimLod(layerLodFor(i));
        particle.setSimLodDistances(lodConfig_.minDist, lodConfig_.maxDist);
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

        particle.setSpatialHashes(spatialReadHashes(i), spatialWriteHashes(i));

        particle.setSpawnQueue(&spawnQueues_[i]);

        particle.setInitSceneTime(sceneTime_);

        const u32 seed = ev.parentRngState + layerRSM + kRandStateSpawnAddend;
        pools_[i].initRange(layer, seed, slot, 1U, frameArena_, issues);
        ++spawnedTotals_[i];
    }
}

void EffectRuntime::prepareParticlesForTick(std::size_t i, const EffectFrameInputs& inputs) {
    const u16 invLifeSlot = invLifeSlots_[i];
    const u16 lifeRatioSlot = lifeRatioSlots_[i];
    for (std::size_t p = 0; p < pools_[i].size(); ++p) {
        auto& particle = pools_[i].particle(p);
        particle.setSceneL2W(inputs.emitterL2W);
        particle.setCameras(cameras());
        particle.setSimLod(layerLodFor(i));
        particle.setSimLodDistances(lodConfig_.minDist, lodConfig_.maxDist);
        particle.setEffectAge(inputs.effectAge);
        particle.setEffectIsRunning(inputs.effectIsRunning);
        particle.setSpawnQueue(&spawnQueues_[i]);
        particle.setSpatialHashes(spatialReadHashes(i), spatialWriteHashes(i));
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
                exts.set(lifeRatioSlot, RegisterValue::scalar(particle.lifeRatio()));
            }
        }
    }
}

void EffectRuntime::updateCameras(const EffectFrameInputs& inputs) noexcept {
    const auto& m = inputs.view.view.m;
    const f32 invScale =
        (inputs.renderUnitsPerSimUnit > 0.0F) ? (1.0F / inputs.renderUnitsPerSimUnit) : 1.0F;
    for (std::size_t j = 0; j < 3U; ++j) {
        camera_.position[j] = -((m[(j * 4U) + 0U] * m[12]) + (m[(j * 4U) + 1U] * m[13]) +
                                (m[(j * 4U) + 2U] * m[14])) *
                              invScale;
    }
    const f32 w = inputs.view.viewport[2];
    const f32 h = inputs.view.viewport[3];
    camera_.resolution[0] = (w >= 1.0F) ? static_cast<i32>(w) : 1;
    camera_.resolution[1] = (h >= 1.0F) ? static_cast<i32>(h) : 1;

    const auto row = [&m](std::size_t i) { return std::array<f32, 3>{m[i], m[4U + i], m[8U + i]}; };
    const auto negate = [](std::array<f32, 3> v) {
        return std::array<f32, 3>{-v[0], -v[1], -v[2]};
    };
    camera_.basis[0] = row(0);
    camera_.basis[1] = negate(row(2));
    camera_.basis[2] = row(1);

    const auto& pm = inputs.view.proj.m;
    std::array<f32, 16> clip{};
    for (std::size_t c = 0; c < 4U; ++c) {
        for (std::size_t r = 0; r < 4U; ++r) {
            f32 sum = 0.0F;
            for (std::size_t k = 0; k < 4U; ++k) {
                sum += pm[(k * 4U) + r] * m[(c * 4U) + k];
            }
            clip[(c * 4U) + r] = sum;
        }
    }
    const auto clipRow = [&clip](std::size_t r) {
        return std::array<f32, 4>{clip[r], clip[4U + r], clip[8U + r], clip[12U + r]};
    };
    const std::array<f32, 4> rw = clipRow(3);
    std::array<std::array<f32, 4>, 6> planes{};
    for (std::size_t axis = 0; axis < 3U; ++axis) {
        const std::array<f32, 4> ra = clipRow(axis);
        for (std::size_t lane = 0; lane < 4U; ++lane) {
            planes[axis * 2U][lane] = rw[lane] + ra[lane];
            planes[(axis * 2U) + 1U][lane] = rw[lane] - ra[lane];
        }
    }
    bool frustumOk = true;
    for (auto& p : planes) {
        const f32 len = std::sqrt((p[0] * p[0]) + (p[1] * p[1]) + (p[2] * p[2]));
        if (!(len > 0.0F)) {
            frustumOk = false;
            break;
        }
        const f32 inv = 1.0F / len;
        p = {p[0] * inv, p[1] * inv, p[2] * inv, p[3] * inv * invScale};
    }
    camera_.frustum = planes;
    camera_.hasFrustum = frustumOk;
}

void EffectRuntime::refreshLayerLods() noexcept {
    if (plan_ == nullptr) {
        return;
    }
    const std::size_t layerCount = plan_->layers.size();
    if (!layerLodSetupDone_) {
        layerLod_.assign(layerCount, lodConfig_.defaultLod);
        layerUsesSimLod_.assign(layerCount, 0U);
        for (std::size_t i = 0; i < layerCount; ++i) {
            const auto& layer = plan_->layers[i];
            for (const auto* scope :
                 {&layer.initProgram.functions, &layer.physicsProgram.functions,
                  &layer.timeFixedProgram.functions, &layer.timeVaryingProgram.functions}) {
                for (const auto& fn : *scope) {
                    if (fn.symbolName.rfind("sim.lod", 0) == 0U) {
                        layerUsesSimLod_[i] = 1U;
                        break;
                    }
                }
                if (layerUsesSimLod_[i] != 0U) {
                    break;
                }
            }
        }
        layerLodSetupDone_ = true;
    }

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    static const bool kLodTrace = std::getenv("CF_LOD_TRACE") != nullptr;
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

    for (std::size_t i = 0; i < layerCount && i < pools_.size(); ++i) {
        if (layerUsesSimLod_[i] == 0U) {
            continue;
        }
        std::array<f32, 3> lo{};
        std::array<f32, 3> hi{};
        const bool boxValid = positionSpread(i, lo.data(), hi.data()) > 0U;
        layerLod_[i] = computeLodLevel(lodConfig_, cameras(), lo, hi, boxValid);
        if (kLodTrace) {
            std::fprintf(stderr,
                         "[lod] L%zu valid=%d box=(%.2f,%.2f,%.2f)..(%.2f,%.2f,%.2f) eye=(%.2f,"
                         "%.2f,%.2f) lod=%.4f\n",
                         i, boxValid ? 1 : 0, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2],
                         camera_.position[0], camera_.position[1], camera_.position[2],
                         static_cast<double>(layerLod_[i]));
        }
    }
}

void EffectRuntime::injectSceneScalar(std::size_t i, const char* name, f32 value) {
    const auto& layer = plan_->layers[i];
    const std::span<const ExternalBinding> scopes[] = {
        layer.physicsProgram.externals,
        layer.timeFixedProgram.externals,
        layer.timeVaryingProgram.externals,
    };
    std::array<u16, std::size(scopes)> slots{};
    std::size_t slotCount = 0;
    for (const auto& scope : scopes) {
        const auto* hit = findBindingByName(scope, name);
        if (hit == nullptr) {
            continue;
        }
        bool seen = false;
        for (std::size_t k = 0; k < slotCount; ++k) {
            if (slots[k] == hit->canonicalSlot) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            slots[slotCount++] = hit->canonicalSlot;
        }
    }
    if (slotCount == 0) {
        return;
    }

    const RegisterValue scalar = RegisterValue::scalar(value);
    for (std::size_t p = 0; p < pools_[i].size(); ++p) {
        auto exts = pools_[i].particle(p).externals();
        for (std::size_t k = 0; k < slotCount; ++k) {
            if (slots[k] < exts.size()) {
                exts.set(slots[k], scalar);
            }
        }
    }
}

void EffectRuntime::injectSceneDt(std::size_t i, f32 dt) {
    injectSceneScalar(i, "scene.dt", dt);
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
                RegisterValue dst;
                dst.lanes[0] = value[0];
                dst.lanes[1] = value[1];
                dst.lanes[2] = value[2];
                dst.lanes[3] = value[3];
                dst.componentCount = 4;
                exts.set(slot, dst);
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
    simtLayersRanLastTick_ = 0U;
    simtLayersFellBackLastTick_ = 0U;

    spatialReadIsAlt_ = !spatialReadIsAlt_;
    for (auto& h : (spatialReadIsAlt_ ? spatialHashesOwned_ : spatialHashesOwnedAlt_)) {
        h->clear();
    }

    updateCameras(inputs);

    refreshLayerLods();

    if (!initialized_) {
        initializeOnFirstTick(inputs, issues);
        initialized_ = true;
    }

    sceneTime_ += inputs.dt;

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    static const bool kSpawnTrace = std::getenv("CF_SPAWN_TRACE") != nullptr;
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    for (std::size_t i = 0; i < plan_->layers.size(); ++i) {
        const auto& layer = plan_->layers[i];
        const bool isSpawner = layer.renderers.empty();

        const bool kickTarget = isKickTarget(layer.id);
        const std::size_t incoming = spawnQueues_[i].events.size();
        if (kickTarget) {
            drainPendingSpawns(i, inputs, issues);
        } else {
            spawnQueues_[i].clear();
        }

        prepareParticlesForTick(i, inputs);
        injectSceneDt(i, inputs.dt);
        injectSceneScalar(i, "scene.time", sceneTime_);
        applyAttributeOverrides(i);

        const bool skipTick = isSpawner && !spawnerEnabled_;
        if (!skipTick) {
            if (layerModel(i) == LayerModel::ModelB) {
                const auto simt = pools_[i].tickBatchSimt(layer, frameArena_, issues);
                if (simt.ranSimt) {
                    ++simtLayersRanLastTick_;
                } else {
                    ++simtLayersFellBackLastTick_;
                }
            } else {
                (void)pools_[i].tickBatch(layer, frameArena_, issues);
            }
        }

        if (kSpawnTrace) {
            std::size_t alive = 0;
            for (std::size_t p = 0; p < pools_[i].size(); ++p) {
                if (!pools_[i].particle(p).isDead()) {
                    ++alive;
                }
            }
            std::fprintf(
                stderr,
                "[spawn] L%zu id=%u pool=%zu alive=%zu kickTgt=%d incoming=%zu kicked=%zu\n", i,
                layer.id.value, pools_[i].size(), alive, kickTarget ? 1 : 0, incoming,
                spawnQueues_[i].events.size());
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

}
