#pragma once

/// @file
/// @brief Per-execution VM state — register banks, externals, samplers, payload scratch.
///
/// `BytecodeExecContext` is the union of "everything the VM might touch while
/// executing one scope of one particle". `LayerTickHarness` builds one of these
/// each tick by aliasing into its owned register/external buffers.

#include <cornflakes/interface/binding/event_payload_decl.hpp>
#include <cornflakes/interface/binding/external_binding.hpp>
#include <cornflakes/interface/binding/sampler_resource.hpp>
#include <cornflakes/interface/binding/spatial_layer_resource.hpp>
#include <cornflakes/interface/core/fast_rand.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/sim/proximity_hash.hpp>
#include <cornflakes/interface/sim/spawn_event.hpp>
#include <cornflakes/interface/vm/bytecode_trace.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace whiteout::cornflakes {

/// @brief Affine 3x4 transform (row-major) used for scene-local L2W transforms.
struct Mat4x3 {
    f32 m[3][4]{
        {1.0F, 0.0F, 0.0F, 0.0F},
        {0.0F, 1.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 1.0F, 0.0F},
    };

    static constexpr Mat4x3 identity() noexcept {
        return Mat4x3{};
    }

    /// @brief Apply as a point transform (includes translation).
    constexpr void apply(const f32 in[3], f32 out[3]) const noexcept {
        for (int i = 0; i < 3; ++i) {
            out[i] = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2] + m[i][3];
        }
    }
    /// @brief Apply as a direction transform (translation skipped).
    constexpr void applyDirection(const f32 in[3], f32 out[3]) const noexcept {
        for (int i = 0; i < 3; ++i) {
            out[i] = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2];
        }
    }
};

/// @brief Number of scope register banks (init/physics/timeFixed/timeVarying).
inline constexpr std::size_t kScopeRegisterBuckets = 4;

/// @brief Aliased view of all per-execution state the VM may read/write.
struct BytecodeExecContext {
    std::array<std::span<RegisterValue>, kScopeRegisterBuckets> scopeRegisters;
    std::span<RegisterValue> externals;
    std::span<const std::byte> constantsPool;
    std::span<const FunctionBinding> functions;
    std::span<const ExternalBinding> externalBindings;
    std::span<const SamplerResource> samplers;
    std::span<const SpatialLayerResource> spatialLayers;

    /// Payload decls for name-matched payload elements. `kickedEventDecls` names the slots of
    /// events this layer emits (appendPayload); `rootEventDecl` names the elements of the event
    /// that spawned this layer (extractPayloadElement). See `event_payload_decl.hpp`.
    std::span<const KickedEventPayloadDecl> kickedEventDecls;
    std::span<const EventPayloadElement> rootEventDecl;

    std::span<ProximityHash* const> spatialHashes;
    TFastRandU32* rng = nullptr;
    f32 effectAge = 0.0F;

    bool effectIsRunning = true;

    f32 timeWindowEnd = 0.0F;

    f32 timeWindowStart = 0.0F;
    Mat4x3 sceneL2W = Mat4x3::identity();

    std::array<f32, 3> spawnTranslate{0.0F, 0.0F, 0.0F};
    std::array<f32, 4> spawnQuat{0.0F, 0.0F, 0.0F, 1.0F};
    std::array<f32, 3> spawnScale{1.0F, 1.0F, 1.0F};

    bool hasSpawnIntPayload = false;
    u8 spawnIntPayloadWidth = 0;
    std::array<i32, 4> spawnIntPayload{};
    u32 spawnIntPayloadId = 0;
    bool hasSpawnBoolPayload = false;
    u8 spawnBoolPayloadWidth = 0;
    std::array<i32, 4> spawnBoolPayload{};
    u32 spawnBoolPayloadId = 0;

    u32 spawnPositionPayloadId = 0;
    u32 spawnOrientationPayloadId = 0;

    /// Slot-indexed float payload elements delivered to THIS spawned particle (Color,
    /// Size, …); read by extractPayloadElementF*. Populated from `SpawnEvent::floatSlots`.
    std::array<PayloadFloatSlot, kMaxPayloadFloatSlots> spawnFloatSlots{};

    /// Transient build→append handoff: `buildPayloadElement` stashes the computed value
    /// keyed by the elementId it returns; `appendPayload(slotId, elementId)` retrieves it
    /// and places it into the pending event's `floatSlots[slotId]`.
    struct BuiltPayloadFloat {
        bool valid = false;
        u32 elementId = 0;
        u8 width = 0;
        std::array<f32, 4> value{};
    };
    std::array<BuiltPayloadFloat, 8> builtPayloadFloats{};

    /// The no-value `buildPayloadElement(generate())` form — the engine builds a per-child
    /// sequential INDEX int payload (`generate.newTotal + localChildIndex`), read by
    /// `extractPayloadElementI1` and gated on `index % N == 0` to spawn 1/N of children. Stashed
    /// by elementId; `appendPayload(slotId, elementId)` marks the pending event so `dispatchKick`
    /// stamps `base + childIndex` per spawned child.
    struct BuiltPayloadIndex {
        bool valid = false;
        u32 elementId = 0;
        i32 base = 0;
    };
    BuiltPayloadIndex builtPayloadIndex{};

    /// Transient `__spatialLayer_N.appendPayload(key, value)` → `insert(key, location)` handoff,
    /// analogous to `builtPayloadFloats` for events. Spatial appendPayload stashes the value keyed
    /// by the allocatePayload key; insert retrieves it as the entry's payload (what `closest*`
    /// returns — the appended world position, distinct from the synthetic hash key location).
    struct SpatialAppendSlot {
        bool valid = false;
        i32 key = 0;
        std::array<f32, 3> value{0.0F, 0.0F, 0.0F};
    };
    std::array<SpatialAppendSlot, 8> spatialAppendStaged{};

    BytecodeTrace* trace = nullptr;
    SpawnEventQueue* spawnQueue = nullptr;
    u32 functionDepth = 0;

    bool inInitScope = false;

    struct PendingKickPayload {
        u32 eventId = 0;
        u32 count = 0;
        bool valid = false;
    };
    std::array<PendingKickPayload, 8> pendingKickPayloads{};

    static constexpr std::size_t kMaxPendingPositions = 64U;
    struct PendingPayloadElement {
        u32 eventId = 0;

        u32 positionPayloadId = 0;
        u32 orientationPayloadId = 0;
        u32 intPayloadId = 0;
        u32 boolPayloadId = 0;
        u32 positionCount = 0;
        std::array<std::array<f32, 3>, kMaxPendingPositions> positions{};
        bool hasOrientation = false;
        std::array<f32, 4> orientation{0.0F, 0.0F, 0.0F, 1.0F};

        bool hasIntPayload = false;
        u8 intPayloadWidth = 0;
        std::array<i32, 4> intPayload{};
        // Per-spawn INDEX int payload from the no-value `buildPayloadElement(generate())` form:
        // the engine writes a sequential index `generate.newTotal + localChildIndex` per spawned
        // child (read back by `extractPayloadElementI1`; effects gate spawns on `index % N == 0`).
        // Distinct from `intPayload` (a fixed value) because it varies per child.
        bool hasSpawnIndexPayload = false;
        u32 spawnIndexPayloadId = 0;
        i32 spawnIndexBase = 0;
        bool hasBoolPayload = false;
        u8 boolPayloadWidth = 0;
        std::array<i32, 4> boolPayload{};

        /// Slot-indexed float payload elements staged by appendPayload (Color, Size, …).
        std::array<PayloadFloatSlot, kMaxPayloadFloatSlots> floatSlots{};

        bool valid = false;
    };
    std::array<PendingPayloadElement, 8> pendingPayloadElements{};

    u32 nextPayloadElementId = 1;

    u32 lastGenerateCount = 0;
    bool lastGenerateValid = false;
    std::array<f32, kMaxPendingPositions> lastGenerateTs{};

    std::array<f32, kMaxPendingPositions> lastGenerateLerpedTimes{};

    bool selfKillRequested = false;

    u64 currentSelfId = 0U;

    struct EventCacheEntry {
        u32 key = 0;
        u32 count = 0;
        u32 currentElementIdx = 0;
        u32 countDup = 0;

        u32 forwardFlag = 0;
        std::array<u32, kMaxPendingPositions> particleIndices{};
        std::array<f32, kMaxPendingPositions> tFractions{};
        std::array<f32, kMaxPendingPositions> lerpedTimes{};
        bool valid = false;
    };
    static constexpr std::size_t kMaxEventCacheEntries = 16U;
    std::array<EventCacheEntry, kMaxEventCacheEntries> eventCaches{};

    u32 simUnitScratchCounter = 0;
};

/// @brief Look up or claim an event-cache slot for `key`. Returns null when full.
inline BytecodeExecContext::EventCacheEntry* allocEventCacheEntry(BytecodeExecContext& ctx,
                                                                  u32 key) noexcept {
    for (auto& e : ctx.eventCaches) {
        if (e.valid && e.key == key) {

            return &e;
        }
    }
    for (auto& e : ctx.eventCaches) {
        if (!e.valid) {
            e = BytecodeExecContext::EventCacheEntry{};
            e.key = key;
            e.valid = true;
            return &e;
        }
    }
    return nullptr;
}

/// @brief Record an in-progress kick count for `eventId`, replacing any prior value.
inline void setPendingKickCount(BytecodeExecContext& ctx, u32 eventId, u32 count) noexcept {
    for (auto& p : ctx.pendingKickPayloads) {
        if (p.valid && p.eventId == eventId) {
            p.count = count;
            return;
        }
    }
    for (auto& p : ctx.pendingKickPayloads) {
        if (!p.valid) {
            p.eventId = eventId;
            p.count = count;
            p.valid = true;
            return;
        }
    }
}

/// @brief Consume the pending kick count for `eventId`. Returns 0 if none pending.
inline u32 takePendingKickCount(BytecodeExecContext& ctx, u32 eventId) noexcept {
    for (auto& p : ctx.pendingKickPayloads) {
        if (p.valid && p.eventId == eventId) {
            const u32 c = p.count;
            p.valid = false;
            return c;
        }
    }
    return 0U;
}

} // namespace whiteout::cornflakes
