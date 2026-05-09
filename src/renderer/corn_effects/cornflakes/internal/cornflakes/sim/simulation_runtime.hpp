#pragma once

/// @file
/// @brief Worker-pool wrapper that ticks one emitter's medium across a time window.

#include <cornflakes/interface/binding/effect_execution_plan.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/scheduler/worker_pool.hpp>
#include <cornflakes/sim/medium.hpp>
#include <cornflakes/sim/scene_time_window.hpp>

namespace whiteout::cornflakes {

/// @brief Schedules per-page evolve tasks on a worker pool for a single emitter.
class SimulationRuntime {
public:
    SimulationRuntime() = default;

    /// @brief Tick `medium` against `plan` over `window` using `pool` for parallelism.
    bool tickEmitter(IWorkerPool& pool, MediumState& medium, const EffectExecutionPlan& plan,
                     const SceneTimeWindow& window, IssueBag& issues) const;
};

} // namespace whiteout::cornflakes
