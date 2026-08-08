#pragma once

#include <cornflakes/interface/core/types.hpp>

#include <array>
#include <cstddef>
#include <vector>

namespace whiteout::cornflakes {

inline constexpr std::size_t kMaxPayloadFloatSlots = 16;

struct PayloadFloatSlot {
    u32 nameId = 0;
    bool valid = false;
    u8 width = 0;
    std::array<f32, 4> value{};
};

struct SpawnEvent {
    u32 eventId = 0;
    std::array<i32, 3> payload{};
    u32 sequenceIndex = 0;

    u64 parentSelfId = 0U;
    u32 parentRngState = 0U;

    bool hasSpawnPosition = false;
    std::array<f32, 3> spawnPosition{0.0F, 0.0F, 0.0F};
    u32 spawnPositionPayloadId = 0;
    bool hasSpawnOrientation = false;
    std::array<f32, 4> spawnOrientation{0.0F, 0.0F, 0.0F, 1.0F};
    u32 spawnOrientationPayloadId = 0;

    bool hasIntPayload = false;
    u8 intPayloadWidth = 0;
    std::array<i32, 4> intPayload{};
    u32 intPayloadId = 0;
    bool hasBoolPayload = false;
    u8 boolPayloadWidth = 0;
    std::array<i32, 4> boolPayload{};
    u32 boolPayloadId = 0;

    std::array<PayloadFloatSlot, kMaxPayloadFloatSlots> floatSlots{};

    f32 subFrameFraction = 1.0F;
    f32 lerpedTime = 0.0F;
};

struct SpawnEventQueue {
    std::vector<SpawnEvent> events;
    std::size_t capacity = 0;
    std::size_t dropped = 0;

    void clear() noexcept {
        events.clear();
        dropped = 0;
    }
};

}
