#include <cornflakes/interface/asset/asset_reader.hpp>
#include <cornflakes/asset/war3_compatibility_validator.hpp>
#include <cornflakes/interface/binding/effect_binder.hpp>
#include <cornflakes/interface/binding/effect_execution_plan.hpp>
#include <cornflakes/interface/core/arena.hpp>
#include <cornflakes/core/determinism.hpp>
#include <cornflakes/diagnostics/frame_trace.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/diagnostics/issue_codes.hpp>
#include <cornflakes/render/render_extractor.hpp>
#include <cornflakes/scheduler/serial_worker_pool.hpp>
#include <cornflakes/service/cornflakes_service.hpp>
#include <cornflakes/sim/medium.hpp>
#include <cornflakes/sim/scene_time_window.hpp>
#include <cornflakes/sim/simulation_runtime.hpp>

#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace whiteout::cornflakes {

namespace {

inline constexpr f32 kMaxDt = 0.25F;

Issue serviceWarning(u32 code, std::string_view message) noexcept {
    Issue issue;
    issue.severity = Severity::Warning;
    issue.category = Category::Service;
    issue.code = code;
    issue.message = message;
    return issue;
}

Issue serviceFatal(u32 code, std::string_view message) noexcept {
    Issue issue;
    issue.severity = Severity::Fatal;
    issue.category = Category::Service;
    issue.code = code;
    issue.message = message;
    return issue;
}

class CornFlakesServiceImpl final : public CornFlakesService, public DiagnosticsFacade {
public:
    explicit CornFlakesServiceImpl(const ServiceConfig& cfg) : m_config(cfg) {
        if (m_config.workerPool == nullptr) {
            m_ownedPool = std::make_unique<SerialWorkerPool>(m_schedulerIssues);
        }
    }

    ServiceOutcome<EffectHandle> loadEffect(EffectId id, const BakedSource& src) override {
        ServiceOutcome<EffectHandle> out;

        auto arena = std::make_unique<ExpandingArena>();
        auto model = m_dispatcher.read(src, *arena, out.issues);
        if (!model) {
            return out;
        }

        if (!m_validator.validate(*model, out.issues)) {
            return out;
        }

        auto plan = m_binder.bind(*model, id, *arena, out.issues);
        if (!plan) {
            return out;
        }

        PlanEntry entry;
        entry.arena = std::move(arena);
        entry.plan = *plan;
        entry.generation = 1U;
        const u32 gen = entry.generation;
        m_plans.insert_or_assign(id.value, std::move(entry));

        out.value = EffectHandle{id, gen};
        return out;
    }

    ServiceOutcome<EmitterId> createEmitter(EffectHandle effect, const EmitterInitState&) override {
        ServiceOutcome<EmitterId> out;
        if (m_plans.find(effect.id.value) == m_plans.end()) {
            out.issues.push(serviceFatal(issues::service::kEmitterEffectMissing,
                                         "createEmitter: EffectHandle references unloaded effect"));
            return out;
        }
        const EmitterId newId{++m_nextEmitterId};

        EmitterEntry ee;
        ee.medium.emitter = newId;
        ee.medium.effectIdValue = static_cast<u32>(effect.id.value & 0xFFFFFFFFU);
        ee.effect = effect;
        m_emitters.insert_or_assign(newId.value, std::move(ee));

        out.value = newId;
        return out;
    }

    ServiceOutcome<void> destroyEmitter(EmitterId id) override {
        m_emitters.erase(id.value);
        return {};
    }

    ServiceOutcome<void> setEmitterEnabled(EmitterId, bool) override {
        return {};
    }

    ServiceOutcome<void> setEmitterTransform(EmitterId, const Transform&) override {
        return {};
    }

    ServiceOutcome<void> setEmitterAttribute(EmitterId, std::string_view,
                                             const AttributeValue&) override {
        return {};
    }

    ServiceOutcome<void> pushEmitterEvent(EmitterId, std::string_view,
                                          const EventPayload&) override {
        return {};
    }

    ServiceOutcome<void> tick(f32 dt) override {
        m_lastFrameIssues.clear();

        f32 applied = dt;
        if (dt < 0.0F) {
            m_lastFrameIssues.push(
                serviceWarning(issues::service::kNegativeDt, "Tick dt < 0; treated as 0"));
            applied = 0.0F;
        } else if (dt > kMaxDt) {
            applied = kMaxDt;
        }
        m_lastTickDt = applied;

        ++m_frameCounter;

        m_trace.beginFrame(FrameId{m_frameCounter}, applied);

        const SceneTimeWindow window{m_sceneTimeEnd, m_sceneTimeEnd + applied};
        m_sceneTimeEnd = window.end;

        m_renderPackets.clear();
        m_frameArena.reset();

        for (auto& [emitterKey, ee] : m_emitters) {
            const auto planIt = m_plans.find(ee.effect.id.value);
            if (planIt == m_plans.end()) {
                continue;
            }
            const EffectExecutionPlan& plan = planIt->second.plan;

            m_trace.addEmitter();

            (void)m_sim.tickEmitter(m_ownedPool != nullptr ? *m_ownedPool : *m_config.workerPool,
                                    ee.medium, plan, window, m_lastFrameIssues);

            for (const auto& page : ee.medium.pages) {
                m_trace.addPage(page.particleCount);
            }

            for (const auto& layer : plan.layers) {
                auto perLayer =
                    m_extractor.extract(ee.medium, layer, m_frameArena, m_lastFrameIssues);
                for (auto& pkt : perLayer) {
                    m_trace.addPacket();
                    m_renderPackets.push_back(std::move(pkt));
                }
            }
        }

        m_trace.endFrame();

        ServiceOutcome<void> out;
        for (const auto& issue : m_lastFrameIssues.view()) {
            out.issues.push(issue);
        }
        return out;
    }

    ServiceOutcome<std::span<const RenderPacket>> collectRenderPackets(FrameId) override {
        ServiceOutcome<std::span<const RenderPacket>> out;
        out.value = std::span<const RenderPacket>{m_renderPackets.data(), m_renderPackets.size()};
        return out;
    }

    DiagnosticsFacade& diagnostics() noexcept override {
        return *this;
    }

    std::span<const Issue> lastFrameIssues() const noexcept override {
        return m_lastFrameIssues.view();
    }

    f32 lastTickDt() const noexcept override {
        return m_lastTickDt;
    }

    const FrameTrace& lastFrameTrace() const noexcept override {
        return m_trace.lastFrame();
    }

private:
    struct PlanEntry {
        std::unique_ptr<ExpandingArena> arena;
        EffectExecutionPlan plan;
        u32 generation = 1U;
    };

    struct EmitterEntry {
        EffectHandle effect{};
        MediumState medium{};
    };

    ServiceConfig m_config;
    IssueBag m_schedulerIssues;
    std::unique_ptr<IWorkerPool> m_ownedPool;

    SerializerPriorityDispatcher m_dispatcher;
    War3CompatibilityValidator m_validator;
    EffectBinder m_binder;
    SimulationRuntime m_sim;
    RenderExtractor m_extractor;
    FrameTraceRecorder m_trace;

    std::unordered_map<u64, PlanEntry> m_plans;
    std::unordered_map<u64, EmitterEntry> m_emitters;

    IssueBag m_lastFrameIssues;
    f32 m_lastTickDt = 0.0F;
    u64 m_frameCounter = 0;
    f32 m_sceneTimeEnd = 0.0F;

    ExpandingArena m_frameArena{};
    std::vector<RenderPacket> m_renderPackets;
    u64 m_nextEmitterId = 0;
};

} // namespace

std::unique_ptr<CornFlakesService> CornFlakesService::create(const ServiceConfig& cfg) {
    return std::make_unique<CornFlakesServiceImpl>(cfg);
}

} // namespace whiteout::cornflakes
