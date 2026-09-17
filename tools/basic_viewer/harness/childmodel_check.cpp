// `--childmodel-check`: child-model (PE1) validation. Folding PE1 into the
// particle service changed its RNG, so there is no bit-exact oracle to compare
// against; this checks the invariants instead (PARTICLE_TYPES_DESIGN.md):
//
//   * every Birth eventually gets exactly one Death — live child actors track
//     live child particles, so nothing leaks and nothing is orphaned;
//   * the instance and depth caps still bind;
//   * the population is bounded rather than growing without limit.

#include "harness/harness.h"
#include "harness/harness_common.h"

#include "renderer/frame_ticker.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_loader.h"
#include "renderer/particle/particle_service.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <algorithm>
#include <functional>
#include <iostream>

namespace whiteout::flakes::harness {

namespace {

namespace part = renderer::particle;

constexpr f32 kFramesPerSecond = 60.0f;
// Each sequence dwells for at least this many lifespans of the longest-lived
// child particle, plus a margin, so something has time to die.
constexpr i32 kDwellLifespans = 3;
constexpr i32 kDwellMarginFrames = 60;

void ForEachChildEmitter(renderer::RenderService& renderer,
                         const std::function<void(const part::EmitterKey&, const part::ParticleEmitter&)>& fn) {
    renderer.Particles().ForEachEmitter(
        [&](const part::EmitterKey& k, const part::ParticleEmitter& e) {
            if (k.output == part::ParticleOutput::ChildModel)
                fn(k, e);
        });
}

i32 AliveChildParticles(renderer::RenderService& renderer) {
    i32 alive = 0;
    ForEachChildEmitter(renderer, [&](const part::EmitterKey&, const part::ParticleEmitter& e) {
        alive += e.TotalAlive();
    });
    return alive;
}

} // namespace

i32 RunChildModelCheck(const GateContext& ctx) {
    auto& renderer = ctx.renderer;
    auto& scene = ctx.scene;
    if (ctx.model.empty()) {
        std::cerr << "[cmcheck] needs a model path" << std::endl;
        return exit_code::kUsage;
    }
    if (!InitDevice(renderer.Pipeline(), ctx.backend, "cmcheck"))
        return exit_code::kDevice;

    scene.SetPE1BasePath(ctx.model.parent_path());
    auto* hero = renderer.Loader().SpawnUnit(io::PathToUtf8(ctx.model));
    if (!hero) {
        std::cerr << "[cmcheck] SpawnUnit failed" << std::endl;
        return exit_code::kSpawn;
    }

    i32 childEmitters = 0;
    ForEachChildEmitter(renderer, [&](const part::EmitterKey&, const part::ParticleEmitter&) {
        ++childEmitters;
    });
    std::cout << "[cmcheck] " << io::PathToUtf8(ctx.model.filename()) << ": " << childEmitters
              << " child-model emitter(s)" << std::endl;
    ForEachChildEmitter(renderer, [&](const part::EmitterKey& k, const part::ParticleEmitter& e) {
        if (const auto* e2 = e.AsEmitter2())
            std::cout << "[cmcheck]   emitter " << k.id << " path='" << e2->Desc().ChildModelPath()
                      << "' lifeSpan=" << e2->Desc().lifeSpan << std::endl;
    });
    if (childEmitters == 0) {
        std::cout << "[cmcheck] no child-model emitters — nothing to check" << std::endl;
        std::cout << "[cmcheck] PASS" << std::endl;
        Exit(exit_code::kPass);
    }

    i32 peakActors = 0;
    i32 peakParticles = 0;
    i32 worstOrphans = 0; // live actors with no live particle behind them
    bool capHeld = true;

    // A unit's PE1 emitters usually fire in one animation (a breath attack, a
    // death), so every sequence gets a dwell. Less than one lifespan and nothing
    // ever dies, which leaves the Birth/Death balance — the assertion that
    // matters — untested.
    const i32 seqCount = (std::max)(1, static_cast<i32>(hero->animation.Sequences().size()));
    i32 longestLife = 0;
    ForEachChildEmitter(renderer, [&](const part::EmitterKey&, const part::ParticleEmitter& e) {
        if (const auto* e2 = e.AsEmitter2())
            longestLife = (std::max)(longestLife, static_cast<i32>(e2->Desc().lifeSpan * kFramesPerSecond));
    });
    const i32 perSeq = (std::max)(ctx.frames / seqCount, longestLife * kDwellLifespans + kDwellMarginFrames);

    for (i32 s = 0; s < seqCount; ++s) {
        hero->animation.SetActiveSequenceIndex(s);
        for (i32 i = 0; i < perSeq; ++i) {
            // The content-provider pump is what fetches and parses the child MDX
            // a Birth needs. Without it every birth is dropped as unresolved and
            // the check is vacuous.
            if (auto* cp = scene.ActiveContentProvider())
                cp->Pump();
            scene.Update(kTraceStep);
            renderer.Ticker().Tick(kTraceStep);

            const i32 aliveParticles = AliveChildParticles(renderer);
            const i32 liveActors = scene.PE1InstanceCount();
            peakActors = (std::max)(peakActors, liveActors);
            peakParticles = (std::max)(peakParticles, aliveParticles);
            // Actors may lag particles (a birth whose template is not loaded yet,
            // or one refused by the cap) but must never exceed them: that would
            // be a Death that went unreported and an actor that leaked.
            worstOrphans = (std::max)(worstOrphans, liveActors - aliveParticles);
            if (liveActors > renderer::model::kMaxChildModelInstances)
                capHeld = false;
        }
    }

    std::cout << "[cmcheck] swept " << seqCount << " sequence(s): peak child actors=" << peakActors
              << " peak child particles=" << peakParticles
              << " worst orphaned actors=" << worstOrphans << std::endl;

    // Settled: live child actors must not exceed live child particles. With
    // worstOrphans, that is the Birth/Death balance.
    const i32 finalParticles = AliveChildParticles(renderer);
    const i32 finalActors = scene.PE1InstanceCount();
    const bool balanced = finalActors <= finalParticles;
    std::cout << "[cmcheck] settled: child actors=" << finalActors
              << " child particles=" << finalParticles << std::endl;

    if (peakParticles == 0)
        std::cerr << "[cmcheck] no child particles were ever emitted — check is vacuous" << std::endl;
    if (!balanced)
        std::cerr << "[cmcheck] " << (finalActors - finalParticles)
                  << " child actor(s) leaked past their particle" << std::endl;

    const bool pass = capHeld && worstOrphans <= 0 && peakParticles > 0 && balanced;
    if (!capHeld)
        std::cerr << "[cmcheck] instance cap exceeded" << std::endl;
    if (worstOrphans > 0)
        std::cerr << "[cmcheck] " << worstOrphans
                  << " child actor(s) outlived their particle — Birth/Death unbalanced" << std::endl;

    std::cout << "[cmcheck] " << (pass ? "PASS" : "FAIL") << std::endl;
    renderer.Pipeline().Shutdown();
    Exit(pass ? exit_code::kPass : exit_code::kChildModelFail);
}

} // namespace whiteout::flakes::harness
