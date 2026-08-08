#pragma once

#include <cornflakes/interface/core/types.hpp>

#include <cstddef>
#include <string>

namespace whiteout::cornflakes {

class EffectRuntime;

enum class DivergenceKind : u8 {
    None = 0,
    LayerCount,
    PoolSize,
    SlotValidity,
    RngState,
    LifeRatio,
    SelfId,
    ParentSelfId,
    DeadFlag,
    FrameStartDeadFlag,
    FrameUniform,
    ExternalCount,
    ExternalValue,
    StreamRegisterCount,
    StreamRegisterValue,
    LocalRegisterCount,
    LocalRegisterValue,
    SpawnQueueLength,
    SpawnEventValue,
    SpatialHashCount,
    SpatialHashName,
    SpatialEntryCount,
    SpatialEntryValue,
};

const char* divergenceKindName(DivergenceKind kind) noexcept;

struct RuntimeDivergence {
    bool diverged = false;
    DivergenceKind kind = DivergenceKind::None;

    std::size_t tick = 0;

    std::size_t layerIndex = 0;
    std::size_t slotIndex = 0;

    static constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);
    std::size_t elementIndex = kNoIndex;

    std::size_t lane = kNoIndex;

    std::string valueA;
    std::string valueB;

    std::string describe() const;
};

RuntimeDivergence compareRuntimes(const EffectRuntime& a, const EffectRuntime& b);

}
