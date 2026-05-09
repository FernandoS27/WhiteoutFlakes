#pragma once

/// @file
/// @brief Per-particle execution context: register banks, RNG, life state, payload buffers.
///
/// One `LayerTickHarness` exists per particle slot in a `ParticlePool`. It owns
/// the four per-scope register vectors, the externals vector, and all the
/// per-tick scratch the VM and event/spawn code need. The harness is reused
/// across ticks; only `BytecodeExecContext` views into it are rebuilt per call.

#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/interface/core/fast_rand.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/interface/sim/proximity_hash.hpp>
#include <cornflakes/interface/vm/bytecode_exec_context.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace whiteout::cornflakes {

/// @brief One particle's worth of VM state — registers, RNG, life, payloads.
class LayerTickHarness {
public:
    /// @brief Initial sizing for the per-scope register vectors and the externals vector.
    struct Config {

        std::size_t initialRegistersPerScope = 1024;
        std::size_t externalCount = 1024;
    };

    LayerTickHarness();
    explicit LayerTickHarness(const Config& cfg);

    /// @brief Resize register/external storage to fit `layer`'s declared counts.
    void resizeForLayer(const LayerProgram& layer);

    /// @brief Run the layer's init scope on this slot.
    bool initParticle(const LayerProgram& layer, IArena& arena, IssueBag& issues);

    /// @brief Run the per-tick scopes (physics + timeFixed/timeVarying) on this slot.
    bool tick(const LayerProgram& layer, IArena& arena, IssueBag& issues);

    std::span<RegisterValue> scopeRegs(std::size_t scope) noexcept {
        return (scope < scopeRegisters_.size())
                   ? std::span<RegisterValue>{scopeRegisters_[scope].data(),
                                              scopeRegisters_[scope].size()}
                   : std::span<RegisterValue>{};
    }
    std::span<RegisterValue> externals() noexcept {
        return std::span<RegisterValue>{externals_.data(), externals_.size()};
    }
    std::span<const RegisterValue> externals() const noexcept {
        return std::span<const RegisterValue>{externals_.data(), externals_.size()};
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

    void setEffectAge(f32 age) noexcept {
        effectAge_ = age;
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
        return lifeRatio_ >= 1.0F;
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

private:
    bool runScope(const VMProgramDescriptor& scope, const LayerProgram& layer, IArena& arena,
                  IssueBag& issues);

    std::array<std::vector<RegisterValue>, kScopeRegisterBuckets> scopeRegisters_;
    std::vector<RegisterValue> externals_;
    TFastRandU32 rng_{};
    f32 effectAge_ = 0.0F;
    bool effectIsRunning_ = true;
    f32 timeWindowEnd_ = 0.0F;
    f32 timeWindowStart_ = 0.0F;
    Mat4x3 sceneL2W_ = Mat4x3::identity();
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
    BytecodeTrace* trace_ = nullptr;
    SpawnEventQueue* spawnQueue_ = nullptr;
    std::span<ProximityHash* const> spatialHashes_{};
    u64 selfId_ = 0U;
    u64 parentSelfId_ = 0U;
    u32 parentRngState_ = 0U;
    bool inInitScope_ = false;
    bool wasDeadAtFrameStart_ = false;
    f32 lifeRatio_ = 0.0F;
    std::size_t lastExecuted_ = 0;
    std::size_t lastInstructions_ = 0;
};

} // namespace whiteout::cornflakes
