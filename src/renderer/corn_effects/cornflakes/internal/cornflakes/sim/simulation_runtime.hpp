#pragma once

#include <cornflakes/interface/binding/effect_execution_plan.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/scheduler/worker_pool.hpp>
#include <cornflakes/sim/medium.hpp>
#include <cornflakes/sim/scene_time_window.hpp>

namespace whiteout::cornflakes {

class SimulationRuntime {
public:
    SimulationRuntime() = default;

    bool tickEmitter(IWorkerPool& pool, MediumState& medium, const EffectExecutionPlan& plan,
                     const SceneTimeWindow& window, IssueBag& issues) const;
};

}
