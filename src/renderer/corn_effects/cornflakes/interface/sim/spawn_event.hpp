#pragma once

/// @file
/// @brief One queued spawn-on-event request and the queue type that buffers them between layers.

#include <cornflakes/interface/core/types.hpp>

#include <array>
#include <cstddef>
#include <vector>

namespace whiteout::cornflakes {

/// @brief Max distinct float payload elements carried across a kick. WC3 event decls hold
/// up to ~7 elements (Position/Velocity/Color/Size/…); 16 is headroom.
inline constexpr std::size_t kMaxPayloadFloatSlots = 16;

/// @brief One named float payload element (Color, Size, custom …) carried across a kick.
///
/// Matched by `nameId` (a hash of the element's `PayloadName`), mirroring the engine's
/// name-GUID lookup (`SEvent::FindPayload` on `m_NameGUID`): the parent's append slot and the
/// child's extract index do NOT align numerically, so they are bridged by name. The Position
/// element additionally feeds `spawnPosition`/`spawnTranslate`.
struct PayloadFloatSlot {
    u32 nameId = 0;
    bool valid = false;
    u8 width = 0; ///< 1..4
    std::array<f32, 4> value{};
};

/// @brief One queued cross-layer spawn — payload, parent identity, and optional pose.
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

    /// Slot-indexed float payload elements (Color, Size, …); read by extractPayloadElementF*.
    std::array<PayloadFloatSlot, kMaxPayloadFloatSlots> floatSlots{};

    f32 subFrameFraction = 1.0F;
    f32 lerpedTime = 0.0F;
};

/// @brief Bounded FIFO of `SpawnEvent`s; drained by the target layer at the next tick boundary.
struct SpawnEventQueue {
    std::vector<SpawnEvent> events;
    std::size_t capacity = 0;
    std::size_t dropped = 0;

    void clear() noexcept {
        events.clear();
        dropped = 0;
    }
};

} // namespace whiteout::cornflakes
