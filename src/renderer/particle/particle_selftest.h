#pragma once

#include <string>

namespace whiteout::flakes::renderer::particle {

// Runs the curve / cell-track / shape invariant checks. Returns true when all
// pass; `report` always receives a human-readable summary.
bool RunParticleSelfTest(std::string& report);

} // namespace whiteout::flakes::renderer::particle
