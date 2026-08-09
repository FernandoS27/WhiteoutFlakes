#pragma once

#include <cornflakes/interface/binding/event_payload_decl.hpp>
#include <cornflakes/interface/binding/external_binding.hpp>
#include <cornflakes/interface/binding/sampler_resource.hpp>
#include <cornflakes/interface/binding/spatial_layer_resource.hpp>
#include <cornflakes/interface/core/fast_rand.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/sim/proximity_hash.hpp>
#include <cornflakes/interface/sim/spawn_event.hpp>
#include <cornflakes/interface/vm/bytecode_trace.hpp>
#include <cornflakes/interface/sim/external_store.hpp>
#include <cornflakes/interface/vm/register_value.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace whiteout::cornflakes {

struct Mat4x3 {
    f32 m[3][4]{
        {1.0F, 0.0F, 0.0F, 0.0F},
        {0.0F, 1.0F, 0.0F, 0.0F},
        {0.0F, 0.0F, 1.0F, 0.0F},
    };

    static constexpr Mat4x3 identity() noexcept {
        return Mat4x3{};
    }

    constexpr void apply(const f32 in[3], f32 out[3]) const noexcept {
        for (int i = 0; i < 3; ++i) {
            out[i] = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2] + m[i][3];
        }
    }
    constexpr void applyDirection(const f32 in[3], f32 out[3]) const noexcept {
        for (int i = 0; i < 3; ++i) {
            out[i] = m[i][0] * in[0] + m[i][1] * in[1] + m[i][2] * in[2];
        }
    }
};

inline constexpr std::size_t kScopeRegisterBuckets = 4;

struct SceneCamera {
    std::array<f32, 3> position{0.0F, 0.0F, 0.0F};
    std::array<i32, 2> resolution{1, 1};
    std::array<std::array<f32, 3>, 3> basis{
        std::array<f32, 3>{1.0F, 0.0F, 0.0F},
        std::array<f32, 3>{0.0F, 1.0F, 0.0F},
        std::array<f32, 3>{0.0F, 0.0F, 1.0F},
    };

    std::array<std::array<f32, 4>, 6> frustum{};
    bool hasFrustum = false;

    f32 lodBias = 0.0F;
};

struct BytecodeExecContext {
    std::array<std::span<RegisterValue>, kScopeRegisterBuckets> scopeRegisters;
    ExternalView externals;
    std::span<const std::byte> constantsPool;
    std::span<const FunctionBinding> functions;
    std::span<const ExternalBinding> externalBindings;
    std::span<const SamplerResource> samplers;
    std::span<const SpatialLayerResource> spatialLayers;

    std::span<const KickedEventPayloadDecl> kickedEventDecls;
    std::span<const EventPayloadElement> rootEventDecl;

    std::span<ProximityHash* const> spatialHashes;
    std::span<ProximityHash* const> spatialHashesWrite;

    std::span<const SceneCamera> cameras;

    f32 simLod = 0.0F;
    f32 simLodBias = 0.0F;
    f32 simLodDistanceMin = 5.0F;
    f32 simLodDistanceMax = 200.0F;

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

    std::array<PayloadFloatSlot, kMaxPayloadFloatSlots> spawnFloatSlots{};

    struct BuiltPayloadFloat {
        bool valid = false;
        u32 elementId = 0;
        u8 width = 0;
        std::array<f32, 4> value{};
    };
    std::array<BuiltPayloadFloat, 8> builtPayloadFloats{};

    struct BuiltPayloadIndex {
        bool valid = false;
        u32 elementId = 0;
        i32 base = 0;
    };
    BuiltPayloadIndex builtPayloadIndex{};

    struct SpatialAppendSlot {
        bool valid = false;
        i32 key = 0;
        u32 nameHash = 0;
        u8 components = 0;
        std::array<f32, 4> value{0.0F, 0.0F, 0.0F, 0.0F};
    };
    std::array<SpatialAppendSlot, 16> spatialAppendStaged{};

    struct HandleRegisterBinding {
        u32 reg = 0;
        u16 slot = 0;
    };
    std::array<HandleRegisterBinding, 16> handleRegisterSlots{};
    u8 handleRegisterCount = 0;

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
        bool hasSpawnIndexPayload = false;
        u32 spawnIndexPayloadId = 0;
        i32 spawnIndexBase = 0;
        bool hasBoolPayload = false;
        u8 boolPayloadWidth = 0;
        std::array<i32, 4> boolPayload{};

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

    void resetPerScopeRun() noexcept {
        simUnitScratchCounter = 0U;
        nextPayloadElementId = 1U;
        selfKillRequested = false;
        functionDepth = 0U;
        handleRegisterCount = 0U;

        lastGenerateValid = false;
        lastGenerateCount = 0U;

        builtPayloadIndex.valid = false;

        for (auto& e : eventCaches) {
            e.valid = false;
        }
        for (auto& p : pendingPayloadElements) {
            p.valid = false;
        }
        for (auto& p : pendingKickPayloads) {
            p.valid = false;
        }
        for (auto& b : builtPayloadFloats) {
            b.valid = false;
        }
        for (auto& s : spatialAppendStaged) {
            s.valid = false;
        }
    }
};

// 64-bit only: the census counts 16-byte spans and 8-byte pointers. wasm32
// packs the same fields into 24912, which is not a layout change.
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFULL
static_assert(sizeof(BytecodeExecContext) == 25048,
              "BytecodeExecContext size changed -- update the M-3 field census above, "
              "resetPerScopeRun(), and the arithmetic in SIMT_MIGRATION_PLAN.md §0 and §5/B4");
#endif

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

}
