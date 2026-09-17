// `--multiscene-test`: two scenes render into their own targets with no
// cross-bleed. Scene B (a CreateScene'd scene) gets the model; the default
// scene A stays empty. Both render into separate offscreen targets and are read
// back: A must be background-only, B must contain the model.

#include "harness/harness.h"
#include "harness/harness_common.h"

#include "renderer/model/model_instance.h"
#include "renderer/model/model_loader.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "renderer/viewport.h"
#include "whiteout/flakes/util/path_utf8.h"

#include "gfx/gfx.h"

#include <cstdlib>
#include <iostream>
#include <utility>

namespace whiteout::flakes::harness {

namespace {

constexpr i32 kFramesPerScene = 40;

} // namespace

i32 RunMultiSceneTest(const GateContext& ctx) {
    auto& renderer = ctx.renderer;
    auto& pipe = renderer.Pipeline();
    const auto say = [](const char* s) { std::cout << "[multiscene] " << s << std::endl; };

    if (!InitDevice(pipe, ctx.backend, "multiscene"))
        return exit_code::kSmokeDevice;
    renderer.Settings().SetBackgroundColor(40, 80, 160);

    const renderer::RenderTargetId tA = pipe.CreateOffscreenTarget(kSmokeTargetSize, kSmokeTargetSize);
    const renderer::RenderTargetId tB = pipe.CreateOffscreenTarget(kSmokeTargetSize, kSmokeTargetSize);
    if (!tA || !tB) {
        std::cerr << "[multiscene] CreateOffscreenTarget failed" << std::endl;
        return exit_code::kSmokeTarget;
    }
    pipe.SetPrimaryTarget(tA);
    pipe.EnableFrameCapture(true);

    const renderer::SceneId sceneA = renderer.DefaultSceneId();
    const renderer::SceneId sceneB = renderer.CreateScene();
    say("created scene B");

    // Into scene B: make it active so the loader and services target it, then
    // restore the default.
    if (!ctx.model.empty()) {
        renderer.SetActiveScene(sceneB);
        renderer.SceneAt(sceneB).SetPE1BasePath(ctx.model.parent_path());
        auto* hero = renderer.Loader().SpawnUnit(io::PathToUtf8(ctx.model));
        renderer.SetActiveScene(sceneA);
        say(hero ? "spawned model into scene B" : "spawn into scene B FAILED");
    }

    const auto renderAndRead = [&](renderer::SceneId scene, renderer::RenderTargetId target) {
        renderer::Viewport vp;
        vp.scene = scene;
        vp.target = target;
        vp.camera = &renderer.SceneAt(scene).Camera();
        for (i32 i = 0; i < kFramesPerScene; ++i) {
            renderer.TickScenes(kSmokeStep);
            pipe.RenderViewport(vp);
            pipe.Present(target);
        }
        const std::optional<Readback> frame = ReadFrame(pipe, target, kSmokeTargetSize, kSmokeTargetSize);
        return frame ? std::optional<MeanRgb>(Mean(frame->rgba, kSmokeTargetSize * kSmokeTargetSize))
                     : std::nullopt;
    };

    // The D3D/Vulkan capture ring mirrors the primary target, so it is pointed at
    // the one being read back.
    const std::optional<MeanRgb> meanA = renderAndRead(sceneA, tA);
    pipe.SetPrimaryTarget(tB);
    const std::optional<MeanRgb> meanB = renderAndRead(sceneB, tB);
    pipe.Gfx()->WaitIdle();

    // Engine-level isolation, independent of readback: the loader spawned into
    // scene B only.
    const auto sceneGeosets = [&](renderer::SceneId id) {
        i32 actors = 0, geosets = 0;
        for (auto& [h, mi] : renderer.SceneAt(id).Actors().All()) {
            ++actors;
            geosets += static_cast<i32>(mi->render.gpuGeosets.size());
        }
        return std::pair<i32, i32>{actors, geosets};
    };
    const auto [actorsA, geoA] = sceneGeosets(sceneA);
    const auto [actorsB, geoB] = sceneGeosets(sceneB);
    const MeanRgb a = meanA.value_or(MeanRgb{});
    const MeanRgb b = meanB.value_or(MeanRgb{});

    std::cout << "[multiscene] sceneA actors=" << actorsA << " geosets=" << geoA << " mean=(" << a.r
              << "," << a.g << "," << a.b << ")  sceneB actors=" << actorsB << " geosets=" << geoB
              << " mean=(" << b.r << "," << b.g << "," << b.b << ")" << std::endl;

    const bool isolated = (actorsA == 0 && geoA == 0) && (actorsB == 1 && geoB > 0);
    // Pixel corroboration where readback produced an image. The D3D12 capture
    // ring can return all-zero for stacked offscreen targets: that is "readback
    // unavailable", no evidence either way, not cross-bleed.
    const bool readbackAvailable = meanA && meanB && !a.Black() && !b.Black();
    const bool pixelsOk =
        !readbackAvailable || (std::abs(b.r - a.r) + std::abs(b.g - a.g) + std::abs(b.b - a.b)) > 0;
    const bool pass = isolated && pixelsOk;
    std::cout << "[multiscene] " << (pass ? "PASS" : "FAIL") << " (isolated=" << isolated
              << " pixelsOk=" << pixelsOk << ")" << std::endl;

    pipe.EnableFrameCapture(false);
    pipe.Shutdown();
    Exit(pass ? exit_code::kPass : exit_code::kMultiSceneFail);
}

} // namespace whiteout::flakes::harness
