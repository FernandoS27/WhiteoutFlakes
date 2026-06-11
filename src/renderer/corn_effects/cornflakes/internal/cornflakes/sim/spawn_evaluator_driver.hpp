#pragma once

#include <cornflakes/interface/binding/layer_program.hpp>
#include <cornflakes/interface/core/types.hpp>
#include <cornflakes/interface/diagnostics/issue.hpp>
#include <cornflakes/sim/particle_page.hpp>

namespace whiteout::cornflakes {

class SpawnEvaluatorDriver {
public:
    bool run(const VMProgramDescriptor& program, ParticlePage& page, u32 firstNewParticle,
             IssueBag& issues) const;
};

}
