#include <cornflakes/core/determinism.hpp>
#include <cornflakes/diagnostics/issue_codes.hpp>
#include <cornflakes/sim/spawn_processor.hpp>

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

bool hasFullBuiltinStorage(const ParticlePage& page) noexcept {
    return page.selfIds.size() == page.capacity && page.effectIds.size() == page.capacity &&
           page.parentIds.size() == page.capacity && page.randStates.size() == page.capacity &&
           page.lifeRatios.size() == page.capacity && page.metaData.size() == page.capacity;
}

} // namespace

bool SpawnProcessor::setupStream(ParticlePage& page, MediumState& medium, const SpawnContext& ctx,
                                 IssueBag& issues) const {
    if (!hasFullBuiltinStorage(page)) {
        issues.push(simFatal(issues::sim::kSpawnPageMissing,
                             "SpawnProcessor: page is missing built-in stream storage"));
        return false;
    }

    const u64 start = page.particleCount;
    const u64 end = start + ctx.count;
    if (end > page.capacity) {
        issues.push(simFatal(issues::sim::kSpawnOverflow,
                             "SpawnProcessor: requested count would exceed page capacity"));
        return false;
    }

    const u32 randStateInit = ctx.parentSeed + medium.randomSeedModifier + kRandStateSpawnAddend;

    for (u64 i = start; i < end; ++i) {

        page.selfIds[i] = medium.nextSelfId++;

        page.effectIds[i] = medium.effectIdValue;

        page.parentIds[i] = 0U;

        page.randStates[i] = randStateInit;

        page.lifeRatios[i] = 0.0F;

        page.metaData[i] = 0U;
    }

    page.particleCount = static_cast<u32>(end);
    return true;
}

} // namespace whiteout::cornflakes
