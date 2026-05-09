#pragma once

/// @file
/// @brief Evolves one `ParticlePage` over a time window — the per-page tick body.

#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/sim/particle_page.hpp>
#include <cornflakes/sim/scene_time_window.hpp>

namespace whiteout::cornflakes {

/// @brief Inputs to one evolve task: time window + the scope program to execute.
struct EvolveContext {
    SceneTimeWindow window;
    const VMProgramDescriptor* program = nullptr;
};

/// @brief Per-page evolve worker, scheduled by `SimulationRuntime`.
class EvolvePageTask {
public:
    bool evolve(ParticlePage& page, const EvolveContext& ctx, IssueBag& issues) const;
};

} // namespace whiteout::cornflakes
