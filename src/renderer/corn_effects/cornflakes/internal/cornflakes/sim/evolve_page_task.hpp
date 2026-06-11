#pragma once

#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/sim/particle_page.hpp>
#include <cornflakes/sim/scene_time_window.hpp>

namespace whiteout::cornflakes {

struct EvolveContext {
    SceneTimeWindow window;
    const VMProgramDescriptor* program = nullptr;
};

class EvolvePageTask {
public:
    bool evolve(ParticlePage& page, const EvolveContext& ctx, IssueBag& issues) const;
};

}
