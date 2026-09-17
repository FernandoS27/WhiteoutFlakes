// `--headless-test`: brings the device up, renders an empty scene (or one model)
// through a camera into an OFF-SCREEN target, reads the result back through the
// capture ring and checks a non-black image came out. Exercises InitDevice →
// CreateOffscreenTarget → RenderViewport → DownloadCaptureSlot end to end per
// backend.

#include "documents/model_formats.h"
#include "harness/harness.h"
#include "harness/harness_common.h"

#include "renderer/frame_ticker.h"
#include "renderer/model/corn_effect_source.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_loader.h"
#include "renderer/particle/particle_service.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "renderer/viewport.h"
#include "whiteout/flakes/util/path_utf8.h"

#include "gfx/gfx.h"

#include <iostream>
#include <memory>

namespace whiteout::flakes::harness {

namespace {

// Frames pumped so the synchronous desktop asset pump (textures, child models,
// corn/event data) drains and uploads.
constexpr i32 kEmptySceneFrames = 3;
constexpr i32 kModelFrames = 40;

} // namespace

i32 RunHeadlessTest(const GateContext& ctx) {
    auto& renderer = ctx.renderer;
    auto& scene = ctx.scene;
    const auto step = [](const char* s) { std::cout << "[headless] " << s << std::endl; };
    auto& pipe = renderer.Pipeline();

    step("InitDevice…");
    if (!InitDevice(pipe, ctx.backend, "headless"))
        return exit_code::kSmokeDevice;
    step("InitDevice OK");

    const renderer::RenderTargetId tid = pipe.CreateOffscreenTarget(kSmokeTargetSize, kSmokeTargetSize);
    if (!tid) {
        std::cerr << "[headless] CreateOffscreenTarget failed" << std::endl;
        return exit_code::kSmokeTarget;
    }
    pipe.SetPrimaryTarget(tid);
    step("offscreen target created");

    renderer.Settings().SetBackgroundColor(40, 80, 160);
    pipe.EnableFrameCapture(true);

    // Optional model: exercises the texture / child-model / corn-effects load
    // path. No path is the bare background smoke test.
    renderer::model::Actor* hero = nullptr;
    if (!ctx.model.empty()) {
        scene.SetPE1BasePath(ctx.model.parent_path());
        if (IsModelKind(ctx.model, ModelKind::Effect)) {
            // Standalone PopcornFX effect — the same path the viewer's LoadEffect uses.
            auto src = std::make_shared<renderer::model::CornEffectSource>(io::PathToUtf8(ctx.model));
            hero = renderer.Loader().SpawnUnitFromSource(src);
            step(hero ? "SpawnEffect OK" : "SpawnEffect FAILED");
        } else {
            hero = renderer.Loader().SpawnUnit(io::PathToUtf8(ctx.model));
            step(hero ? "SpawnUnit OK" : "SpawnUnit FAILED");
        }
    }

    renderer::Viewport vp;
    vp.target = tid;
    vp.camera = &scene.Camera();

    const i32 frames = ctx.model.empty() ? kEmptySceneFrames : kModelFrames;
    for (i32 i = 0; i < frames; ++i) {
        scene.Update(kSmokeStep);
        renderer.Ticker().Tick(kSmokeStep); // evaluate actors + advance particle/corn sim
        pipe.RenderViewport(vp);
        pipe.Present(tid);
    }
    pipe.Gfx()->WaitIdle();
    step("frames rendered");

    std::cout << "[headless] childModels(PE1)=" << scene.PE1InstanceCount()
              << " | particle emitters=" << renderer.Particles().EmitterCount() << std::endl;
    if (hero) {
        const i32 texCount = hero->render.textures ? static_cast<i32>(hero->render.textures->Size()) : 0;
        std::cout << "[headless] actor: geosets=" << hero->render.gpuGeosets.size()
                  << " textures=" << texCount << std::endl;
    }

    const std::optional<Readback> frame = ReadFrame(pipe, tid, kSmokeTargetSize, kSmokeTargetSize);
    bool pass = false;
    if (frame) {
        if (frame->viaTargetFallback)
            step("used ReadbackTarget fallback");
        const MeanRgb mean = Mean(frame->rgba, kSmokeTargetSize * kSmokeTargetSize);
        std::cout << "[headless] readback OK " << kSmokeTargetSize << "x" << kSmokeTargetSize
                  << " mean RGB=(" << mean.r << "," << mean.g << "," << mean.b << ")" << std::endl;
        pass = !mean.Black();
    } else {
        std::cerr << "[headless] readback FAILED (slot=" << pipe.LastCapturedSlot() << ")"
                  << std::endl;
    }

    // The verdict before teardown, so a teardown issue cannot hide it.
    std::cout << "[headless] " << (pass ? "PASS" : "FAIL") << std::endl;

    step("teardown…");
    pipe.EnableFrameCapture(false);
    pipe.Shutdown(); // frees all targets (CleanupGFX iterates targets_)
    step("teardown OK");
    Exit(pass ? exit_code::kPass : exit_code::kHeadlessFail);
}

} // namespace whiteout::flakes::harness
