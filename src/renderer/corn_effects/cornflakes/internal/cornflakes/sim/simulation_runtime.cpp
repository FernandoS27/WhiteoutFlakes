#include <cornflakes/core/determinism.hpp>
#include <cornflakes/sim/evolve_page_task.hpp>
#include <cornflakes/sim/simulation_runtime.hpp>

namespace whiteout::cornflakes {

bool SimulationRuntime::tickEmitter(IWorkerPool& pool, MediumState& medium,
                                    const EffectExecutionPlan& plan, const SceneTimeWindow& window,
                                    IssueBag& issues) const {
    EvolveContext ctx;
    ctx.window = window;

    if (!plan.layers.empty()) {
        ctx.program = &plan.layers.front().program;
    }

    EvolvePageTask task;
    for (auto& page : medium.pages) {
        WorkerTask worker;
        worker.fn = [&task, &page, &ctx, &issues] { (void)task.evolve(page, ctx, issues); };
        pool.submit(worker);
    }

    return !issues.hasFatal();
}

} // namespace whiteout::cornflakes
