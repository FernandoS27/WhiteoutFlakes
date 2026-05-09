#include <cornflakes/core/determinism.hpp>
#include <cornflakes/sim/spawn_evaluator_driver.hpp>

namespace whiteout::cornflakes {

bool SpawnEvaluatorDriver::run(const VMProgramDescriptor& program, ParticlePage&, u32,
                               IssueBag&) const {
    if (program.cbemBytecode.empty()) {

        return true;
    }

    return true;
}

} // namespace whiteout::cornflakes
