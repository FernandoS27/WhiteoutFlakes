#pragma once

/// @file
/// @brief Public service-level facade — load effects, manage emitters, tick, collect packets.

#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/diagnostics/diagnostics_facade.hpp>
#include <cornflakes/interface/render/render_packet.hpp>
#include <cornflakes/interface/schema/handles.hpp>
#include <cornflakes/service/service_outcome.hpp>
#include <cornflakes/interface/service/service_types.hpp>

#include <memory>
#include <span>
#include <string_view>

namespace whiteout::cornflakes {

class IWorkerPool;

/// @brief Construction parameters for `CornFlakesService::create`.
struct ServiceConfig {
    IWorkerPool* workerPool = nullptr;
    RandomSeedMode seedMode = RandomSeedMode::HostProvided;
    TraceLevel traceLevel = TraceLevel::Summary;
    bool strictWar3 = true;
    AssetFormats allowedFormats = AssetFormats::all();
};

/// @brief Public, ABI-stable service facade. All methods return `ServiceOutcome` with diagnostics.
class CornFlakesService {
public:
    /// @brief Build a concrete service implementation. Returns null on misconfiguration.
    static std::unique_ptr<CornFlakesService> create(const ServiceConfig& cfg);

    CornFlakesService() = default;
    virtual ~CornFlakesService() = default;

    CornFlakesService(const CornFlakesService&) = delete;
    CornFlakesService& operator=(const CornFlakesService&) = delete;
    CornFlakesService(CornFlakesService&&) = delete;
    CornFlakesService& operator=(CornFlakesService&&) = delete;

    virtual ServiceOutcome<EffectHandle> loadEffect(EffectId id, const BakedSource& src) = 0;
    virtual ServiceOutcome<EmitterId> createEmitter(EffectHandle effect,
                                                    const EmitterInitState& state) = 0;
    virtual ServiceOutcome<void> destroyEmitter(EmitterId id) = 0;

    virtual ServiceOutcome<void> setEmitterEnabled(EmitterId id, bool enabled) = 0;
    virtual ServiceOutcome<void> setEmitterTransform(EmitterId id, const Transform& transform) = 0;
    virtual ServiceOutcome<void> setEmitterAttribute(EmitterId id, std::string_view name,
                                                     const AttributeValue& value) = 0;
    virtual ServiceOutcome<void> pushEmitterEvent(EmitterId id, std::string_view name,
                                                  const EventPayload& payload) = 0;

    /// @brief Advance simulation by `dt` seconds across every live emitter.
    virtual ServiceOutcome<void> tick(f32 dt) = 0;
    /// @brief Collect render packets produced by the most recent tick.
    virtual ServiceOutcome<std::span<const RenderPacket>> collectRenderPackets(FrameId frame) = 0;

    virtual DiagnosticsFacade& diagnostics() noexcept = 0;
};

} // namespace whiteout::cornflakes
