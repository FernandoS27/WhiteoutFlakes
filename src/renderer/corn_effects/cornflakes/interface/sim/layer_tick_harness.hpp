#pragma once

#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/core/fast_rand.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/sim/external_store.hpp>
#include <cornflakes/interface/sim/proximity_hash.hpp>
#include <cornflakes/interface/vm/bytecode_exec_context.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace whiteout::cornflakes {

class LayerTickHarness {
public:
    static constexpr std::size_t kFallbackExternalCount = 1024;

    static constexpr std::size_t kFallbackRegisterCount = 1024;

    struct Config {
        std::array<std::size_t, kScopeRegisterBuckets> registersPerBank{
            kFallbackRegisterCount, kFallbackRegisterCount, kFallbackRegisterCount,
            kFallbackRegisterCount};

        std::size_t externalCount = kFallbackExternalCount;
    };

    LayerTickHarness();
    explicit LayerTickHarness(const Config& cfg);

    void resizeForLayer(const LayerProgram& layer);

    static std::size_t externalStorageSizeFor(const LayerProgram& layer) noexcept;

    static Config layoutFor(const LayerProgram& layer) noexcept;

    bool initParticle(const LayerProgram& layer, IArena& arena, IssueBag& issues);

    bool tick(const LayerProgram& layer, IArena& arena, IssueBag& issues);

    std::span<RegisterValue> scopeRegs(std::size_t scope) noexcept {
        return (scope < scopeRegisters_.size())
                   ? std::span<RegisterValue>{scopeRegisters_[scope].data(),
                                              scopeRegisters_[scope].size()}
                   : std::span<RegisterValue>{};
    }
    std::span<const RegisterValue> scopeRegs(std::size_t scope) const noexcept {
        return (scope < scopeRegisters_.size())
                   ? std::span<const RegisterValue>{scopeRegisters_[scope].data(),
                                                    scopeRegisters_[scope].size()}
                   : std::span<const RegisterValue>{};
    }
    ExternalView externals() const noexcept {
        return ExternalView{externalStore_, externalIndex_, externalCount_};
    }

    void bindExternals(ExternalStore* store, std::size_t index, std::size_t count) noexcept {
        externalStore_ = store;
        externalIndex_ = index;
        externalCount_ = count;
    }

    std::size_t lastExecuted() const noexcept {
        return lastExecuted_;
    }
    std::size_t lastInstructions() const noexcept {
        return lastInstructions_;
    }

    void setRngSeed(u32 seed) noexcept {
        rng_ = TFastRandU32{seed};
    }
    TFastRandU32& rng() noexcept {
        return rng_;
    }
    u32 rngState() const noexcept {
        return rng_.state();
    }

    void setEffectAge(f32 age) noexcept {
        effectAge_ = age;
    }

    void setInitSceneTime(f32 t) noexcept {
        initSceneTime_ = t;
    }

    void setEffectIsRunning(bool running) noexcept {
        effectIsRunning_ = running;
    }

    void setTimeWindowEnd(f32 end) noexcept {
        timeWindowEnd_ = end;
    }

    void setTimeWindowStart(f32 start) noexcept {
        timeWindowStart_ = start;
    }

    void resetLifeRatio() noexcept {
        lifeRatio_ = 0.0F;
    }
    void advanceLifeRatio(f32 dtTimesInvLife) noexcept {
        lifeRatio_ += dtTimesInvLife;

        if (!(lifeRatio_ < 1.0F)) {
            lifeRatio_ = 1.0F;
        }
    }

    void markDead() noexcept {
        lifeRatio_ = 1.0F;
    }
    f32 lifeRatio() const noexcept {
        return lifeRatio_;
    }

    bool isDead() const noexcept {
        return std::bit_cast<u32>(lifeRatio_) >= 0x3F800000U;
    }

    void noteFrameStartDeadState() noexcept {
        wasDeadAtFrameStart_ = isDead();
    }
    bool wasDeadAtFrameStart() const noexcept {
        return wasDeadAtFrameStart_;
    }

    void setSceneL2W(const Mat4x3& m) noexcept {
        sceneL2W_ = m;
    }
    const Mat4x3& sceneL2W() const noexcept {
        return sceneL2W_;
    }

    void setCameras(std::span<const SceneCamera> cameras) noexcept {
        cameras_ = cameras;
    }

    void setSimLod(f32 level) noexcept {
        simLod_ = level;
    }
    f32 effectAge() const noexcept {
        return effectAge_;
    }
    bool effectIsRunning() const noexcept {
        return effectIsRunning_;
    }
    f32 simLodDistanceMin() const noexcept {
        return simLodDistanceMin_;
    }
    f32 simLodDistanceMax() const noexcept {
        return simLodDistanceMax_;
    }

    f32 simLod() const noexcept {
        return simLod_;
    }

    void setSimLodDistances(f32 minDist, f32 maxDist) noexcept {
        simLodDistanceMin_ = minDist;
        simLodDistanceMax_ = maxDist;
    }

    void setSpawnTRS(const std::array<f32, 3>& translate, const std::array<f32, 4>& quaternion,
                     const std::array<f32, 3>& scale) noexcept {
        spawnTranslate_ = translate;
        spawnQuat_ = quaternion;
        spawnScale_ = scale;
    }
    const std::array<f32, 3>& spawnTranslate() const noexcept {
        return spawnTranslate_;
    }
    const std::array<f32, 4>& spawnQuat() const noexcept {
        return spawnQuat_;
    }
    const std::array<f32, 3>& spawnScale() const noexcept {
        return spawnScale_;
    }

    void setSpawnPositionPayloadId(u32 id) noexcept {
        spawnPositionPayloadId_ = id;
    }
    void setSpawnOrientationPayloadId(u32 id) noexcept {
        spawnOrientationPayloadId_ = id;
    }
    u32 spawnPositionPayloadId() const noexcept {
        return spawnPositionPayloadId_;
    }
    u32 spawnOrientationPayloadId() const noexcept {
        return spawnOrientationPayloadId_;
    }

    void setSpawnFloatSlots(
        const std::array<PayloadFloatSlot, kMaxPayloadFloatSlots>& slots) noexcept {
        spawnFloatSlots_ = slots;
    }
    void clearSpawnFloatSlots() noexcept {
        spawnFloatSlots_ = {};
    }
    const std::array<PayloadFloatSlot, kMaxPayloadFloatSlots>& spawnFloatSlots() const noexcept {
        return spawnFloatSlots_;
    }

    void setSpawnIntPayload(u8 width, const std::array<i32, 4>& value, u32 payloadId) noexcept {
        spawnIntPayloadWidth_ = width;
        spawnIntPayload_ = value;
        spawnIntPayloadId_ = payloadId;
        hasSpawnIntPayload_ = true;
    }
    void clearSpawnIntPayload() noexcept {
        hasSpawnIntPayload_ = false;
        spawnIntPayloadWidth_ = 0;
        spawnIntPayload_ = {};
        spawnIntPayloadId_ = 0;
    }
    bool hasSpawnIntPayload() const noexcept {
        return hasSpawnIntPayload_;
    }
    u8 spawnIntPayloadWidth() const noexcept {
        return spawnIntPayloadWidth_;
    }
    const std::array<i32, 4>& spawnIntPayload() const noexcept {
        return spawnIntPayload_;
    }
    u32 spawnIntPayloadId() const noexcept {
        return spawnIntPayloadId_;
    }
    void setSpawnBoolPayload(u8 width, const std::array<i32, 4>& value, u32 payloadId) noexcept {
        spawnBoolPayloadWidth_ = width;
        spawnBoolPayload_ = value;
        spawnBoolPayloadId_ = payloadId;
        hasSpawnBoolPayload_ = true;
    }
    void clearSpawnBoolPayload() noexcept {
        hasSpawnBoolPayload_ = false;
        spawnBoolPayloadWidth_ = 0;
        spawnBoolPayload_ = {};
        spawnBoolPayloadId_ = 0;
    }
    bool hasSpawnBoolPayload() const noexcept {
        return hasSpawnBoolPayload_;
    }
    u8 spawnBoolPayloadWidth() const noexcept {
        return spawnBoolPayloadWidth_;
    }
    const std::array<i32, 4>& spawnBoolPayload() const noexcept {
        return spawnBoolPayload_;
    }
    u32 spawnBoolPayloadId() const noexcept {
        return spawnBoolPayloadId_;
    }

    void setTrace(BytecodeTrace* trace) noexcept {
        trace_ = trace;
    }

    void setSpawnQueue(SpawnEventQueue* queue) noexcept {
        spawnQueue_ = queue;
    }

    void setSpatialHashes(std::span<ProximityHash* const> hashes) noexcept {
        spatialHashes_ = hashes;
        spatialHashesWrite_ = {};
    }
    void setSpatialHashes(std::span<ProximityHash* const> read,
                          std::span<ProximityHash* const> write) noexcept {
        spatialHashes_ = read;
        spatialHashesWrite_ = write;
    }

    void setSelfId(u64 id) noexcept {
        selfId_ = id;
    }
    u64 selfId() const noexcept {
        return selfId_;
    }

    void setParentIdentity(u64 parentSelfId, u32 parentRngState) noexcept {
        parentSelfId_ = parentSelfId;
        parentRngState_ = parentRngState;
    }
    u64 parentSelfId() const noexcept {
        return parentSelfId_;
    }
    u32 parentRngState() const noexcept {
        return parentRngState_;
    }

    void bindContext(const VMProgramDescriptor& scope, const LayerProgram& layer,
                     BytecodeExecContext& ctx);

    void finishScope(const BytecodeExecContext& ctx);

private:
    bool runScope(const VMProgramDescriptor& scope, const LayerProgram& layer, IArena& arena,
                  IssueBag& issues);

    std::array<std::vector<RegisterValue>, kScopeRegisterBuckets> scopeRegisters_;
    std::unique_ptr<ExternalStore> ownExternals_;
    ExternalStore* externalStore_ = nullptr;
    std::size_t externalIndex_ = 0U;
    std::size_t externalCount_ = 0U;
    TFastRandU32 rng_{};
    f32 effectAge_ = 0.0F;
    f32 initSceneTime_ = 0.0F;
    bool effectIsRunning_ = true;
    f32 timeWindowEnd_ = 0.0F;
    f32 timeWindowStart_ = 0.0F;
    Mat4x3 sceneL2W_ = Mat4x3::identity();
    std::span<const SceneCamera> cameras_;
    f32 simLod_ = 0.0F;
    f32 simLodDistanceMin_ = 5.0F;
    f32 simLodDistanceMax_ = 200.0F;
    std::array<f32, 3> spawnTranslate_{0.0F, 0.0F, 0.0F};
    std::array<f32, 4> spawnQuat_{0.0F, 0.0F, 0.0F, 1.0F};
    std::array<f32, 3> spawnScale_{1.0F, 1.0F, 1.0F};
    bool hasSpawnIntPayload_ = false;
    u8 spawnIntPayloadWidth_ = 0;
    std::array<i32, 4> spawnIntPayload_{};
    u32 spawnIntPayloadId_ = 0;
    bool hasSpawnBoolPayload_ = false;
    u8 spawnBoolPayloadWidth_ = 0;
    std::array<i32, 4> spawnBoolPayload_{};
    u32 spawnBoolPayloadId_ = 0;
    u32 spawnPositionPayloadId_ = 0;
    u32 spawnOrientationPayloadId_ = 0;
    std::array<PayloadFloatSlot, kMaxPayloadFloatSlots> spawnFloatSlots_{};
    BytecodeTrace* trace_ = nullptr;
    SpawnEventQueue* spawnQueue_ = nullptr;
    std::span<ProximityHash* const> spatialHashes_{};
    std::span<ProximityHash* const> spatialHashesWrite_{};
    u64 selfId_ = 0U;
    u64 parentSelfId_ = 0U;
    u32 parentRngState_ = 0U;
    bool inInitScope_ = false;
    bool wasDeadAtFrameStart_ = false;
    f32 lifeRatio_ = 0.0F;
    std::size_t lastExecuted_ = 0;
    std::size_t lastInstructions_ = 0;
};

}
