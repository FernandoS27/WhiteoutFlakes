#include <cornflakes/core/determinism.hpp>
#include <cornflakes/diagnostics/issue_codes.hpp>
#include <cornflakes/sim/evolve_page_task.hpp>

namespace whiteout::cornflakes {

namespace {

Issue simFatal(u32 code, std::string_view message) noexcept {
    Issue issue;
    issue.severity = Severity::Fatal;
    issue.category = Category::Sim;
    issue.code = code;
    issue.message = message;
    return issue;
}

} // namespace

bool EvolvePageTask::evolve(ParticlePage& page, const EvolveContext& ctx, IssueBag& issues) const {
    if (page.particleCount == 0U) {
        return true;
    }
    if (page.lifeRatios.size() < page.particleCount) {
        issues.push(simFatal(issues::sim::kEvolveNoLifeStream,
                             "EvolvePageTask: page is missing LifeRatio stream"));
        return false;
    }

    const f32 dt = ctx.window.dt();
    for (u32 i = 0; i < page.particleCount; ++i) {
        page.lifeRatios[i] += dt;
    }

    return true;
}

} // namespace whiteout::cornflakes
