// `--draw-trace`: gate G1 — a deterministic record of every decision the draw
// path makes (REFACTOR_PLAN.md §2) — and G2, the raw readback of the last frame.
// Unlike --particle-diff this needs a device: the hooks sit inside the
// submission paths, after PSO resolve, so only draws really submitted count.
//
// The scene is pinned along every axis that reaches submit order or pixels:
// fixed step, camera pose, LOD, time of day and lighting mode. What pinning
// cannot cover is asset arrival — which frame a texture or child template lands
// on varies with core count and disk cache — so capture is preceded by a settle
// phase, and the capture fails rather than emitting a divergent trace if a new
// need appears (§1.1 #13).

#include "harness/draw_trace_scenario.h"
#include "harness/harness.h"
#include "harness/harness_common.h"

#include "io/wem/wem_profiles.h"
#include "renderer/animation/clip_playlist.h"
#include "renderer/debug/draw_trace.h"
#include "renderer/dnc/dnc_service.h"
#include "renderer/frame_ticker.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_loader.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "renderer/shadow/shadow_service.h"
#include "renderer/viewport.h"
#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/util/path_utf8.h"

#include "gfx/gfx.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace whiteout::flakes::harness {

namespace wem = ::whiteout::models::wem;

namespace {

namespace dbg = renderer::debug;
namespace model = renderer::model;

// The gate's fixed camera: a deterministic pose rather than a framed one.
// FrameCameraToModel reads bounds the engine does not expose format-neutrally,
// and a framing rule that changed would silently re-baseline every golden.
constexpr f32 kCameraTargetZ = 50.0f;
constexpr f32 kCameraPitch = 0.35f;
constexpr f32 kTimeOfDay = 12.0f;
// Instances are spread along +X so the transparent back-to-front sort has real
// work and equal-depth ties are reachable.
constexpr f32 kInstanceSpacing = 120.0f;

void ApplyRenderArms(renderer::RenderService& renderer, const cli::DrawTraceOptions& o) {
    auto& settings = renderer.Settings();
    settings.SetRenderMode(o.hd ? renderer::RenderMode::HD : renderer::RenderMode::SD);
    settings.SetBackgroundColor(0, 0, 0);
    settings.SetLodOverride(0);
    settings.SetLightingMode(renderer::LightingMode::InGame);
    // The multi-model arm. On, every odd-indexed geoset is drawn by UnlitShading
    // instead of the WC3 model, so one frame exercises SurfacePass's open/close
    // transition and the key.model sort term. Off is what every byte-identical
    // baseline is recorded and checked with.
    settings.SetDebugUnlitOddGeosets(o.unlitOddGeosets);

    // The `.m2` streaming arm, and the reason it shares its baselines: deferring
    // the `.anim` reads has to leave the frame byte-identical, so the claim only
    // means something checked against the file the eager run recorded.
    settings.SetM2LazyAnimations(o.lazyAnim);

    // WoW's refraction particles. With the pass off a refraction emitter draws
    // nothing — a golden recorded both ways is the only thing that can say the
    // pass reached pixels, the trace being identical either way.
    settings.SetRefractionEnabled(!o.noRefraction);
    settings.SetRefractionDebugMask(o.refractionMask);
    // Diablo III's distortion. Off is not "the same picture, cheaper": surfaces
    // whose only pass is phase 3 have no scene draw either.
    settings.SetD3DistortionEnabled(!o.noDistortion);
    settings.SetD3DistortionDebugBuffer(o.distortionBuffer);

    // WoW's multi-texture particles. With the combiner off the emitter still
    // draws its first layer at half brightness, so both arms produce an image
    // and it is the difference that says the other two layers reached pixels.
    settings.SetMultiTexParticlesEnabled(!o.noMultiTex);

    // The -Sc2Mat -DebugLight sub-arm: one scripted point light for the M3
    // DeferredLights pass, identical for every model. The values live HERE, not
    // in the script, so a baseline never depends on shell quoting. Renderer units
    // (post-WorldScale), well OFF the body: an SC2 infantry model is ~400 units
    // tall, and a light inside the silhouette faces away from every visible pixel.
    if (o.debugLight)
        settings.SetDebugPointLight(true, {250.0f, -250.0f, 320.0f}, {2.0f, 1.8f, 1.4f}, 900.0f);

    // Cascade shadows, off by default. Only the golden tells the arms apart.
    if (o.shadows) {
        if (auto* svc = renderer.GetShadowService())
            svc->SetEnabled(true);
    }

    // One fixed fog per shader mode, sized to the gate's 350-unit camera so every
    // mode visibly moves the frame.
    if (o.fogMode > 0) {
        renderer::RenderSettings::WorldFog fog;
        fog.mode = o.fogMode;
        fog.color[0] = 90;
        fog.color[1] = 120;
        fog.color[2] = 160;
        fog.start = 200.0f;
        fog.end = 700.0f;
        fog.density = 0.002f;
        fog.heightTop = 150.0f;
        fog.heightBottom = 0.0f;
        fog.radialInner = 50.0f;
        fog.radialOuter = 400.0f;
        fog.radialStrength = 0.6f;
        settings.SetWorldFog(fog);
    }
}

// More than one top-level actor on purpose: with a single actor BuildDrawLists
// iterates a one-entry map, and the hash-order dependence the perturbation arm
// is about cannot show up at all.
std::vector<model::Actor*> SpawnInstances(renderer::RenderService& renderer,
                                          renderer::SceneManager& scene,
                                          const std::filesystem::path& path, i32 instances,
                                          i32 perturbSeed, wem::ProfileId wemProfile) {
    std::vector<model::Actor*> spawned;
    const i32 copies = (instances < 1) ? 1 : instances;
    for (i32 n = 0; n < copies; ++n) {
        // Uneven gaps in the handle sequence, not a shifted start: with an
        // identity hash and consecutive ids a uniform shift keeps the bucket
        // *order* for small maps, and the arm would pass with the bug live.
        for (i32 burn = 0; perturbSeed > 0 && burn < ((perturbSeed >> (n & 7)) & 3) + 1; ++burn)
            (void)scene.AllocActorId();
        // A `.wem` through the WEM entry point, so `--wem-profile` reaches it:
        // SpawnUnit's own sniff opens it at the document's default.
        auto* a = io::LooksLikeWemPath(path)
                      ? renderer.Loader().SpawnWem(ContentRef::FromPath(io::PathToUtf8(path)), wemProfile)
                      : renderer.Loader().SpawnUnit(io::PathToUtf8(path));
        if (!a)
            break;
        spawned.push_back(a);
        a->worldTransform = Matrix44f::translation({static_cast<f32>(n) * kInstanceSpacing, 0.0f, 0.0f});
    }
    return spawned;
}

void PrintDrawCounts(const dbg::DrawTrace& trace) {
    usize totalDraws = 0;
    i32 kinds[4] = {0, 0, 0, 0};
    // SC2_PARTICLE_PLAN X0: particle draws whose shading model is M3Standard —
    // the SC2 particle dialect. A carrier with a firing emitter moves it off zero.
    i32 sc2Particles = 0;
    for (const auto& fr : trace.frames) {
        totalDraws += fr.draws.size();
        for (const auto& d : fr.draws) {
            if (d.producer < 4)
                ++kinds[d.producer];
            if (d.producer == static_cast<decltype(d.producer)>(dbg::TraceProducer::Particle) &&
                d.shadingModel == static_cast<decltype(d.shadingModel)>(dbg::TraceShadingModel::M3Standard))
                ++sc2Particles;
        }
    }
    std::cout << "[dtrace] " << totalDraws << " draw(s) over " << trace.frames.size()
              << " frame(s); kinds geoset=" << kinds[0] << " particle=" << kinds[1]
              << " ribbon=" << kinds[2] << " corn=" << kinds[3] << " sc2par=" << sc2Particles
              << std::endl;
}

// G2: the raw readback, written or compared byte for byte.
bool RecordOrCheckGolden(renderer::RenderPipeline& pipe, renderer::RenderTargetId tid,
                         const cli::DrawTraceOptions& o) {
    std::vector<u8> rgba;
    i32 cw = 0, ch = 0;
    if (!pipe.ReadbackTarget(tid, rgba, cw, ch) || cw != kTraceTargetSize || ch != kTraceTargetSize) {
        std::cerr << "[dtrace] golden: ReadbackTarget unavailable on this backend" << std::endl;
        return false;
    }
    if (!o.recordPath.empty()) {
        std::ofstream out(o.goldenPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(rgba.data()), static_cast<std::streamsize>(rgba.size()));
        std::cout << "[dtrace] golden recorded -> " << o.goldenPath << std::endl;
        return true;
    }
    std::ifstream in(o.goldenPath, std::ios::binary);
    const std::vector<u8> ref((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // Byte-exact on the raw readback. Do NOT sRGB-encode first: that many-to-one
    // map collapses distinguishable bright values onto one byte, hiding
    // regressions exactly where additive and emissive bugs live.
    if (ref.size() != rgba.size()) {
        std::cerr << "[dtrace] golden size differs: " << ref.size() << " vs " << rgba.size() << std::endl;
        return false;
    }
    usize diff = 0;
    for (usize p = 0; p < ref.size(); ++p)
        diff += (ref[p] != rgba[p]) ? 1 : 0;
    if (diff) {
        std::cerr << "[dtrace] golden differs in " << diff << " of " << ref.size() << " bytes" << std::endl;
        return false;
    }
    std::cout << "[dtrace] golden MATCH" << std::endl;
    return true;
}

} // namespace

i32 RunDrawTrace(const GateContext& ctx, const cli::DrawTraceOptions& o,
                 std::span<const std::filesystem::path> attachAnims, wem::ProfileId wemProfile,
                 ProductId game) {
    auto& renderer = ctx.renderer;
    auto& scene = ctx.scene;
    const cli::AnimScenario& anim = o.anim;

    if (ctx.model.empty()) {
        std::cerr << "[dtrace] --draw-trace needs a model path" << std::endl;
        return exit_code::kUsage;
    }
    if (o.recordPath.empty() && o.checkPath.empty() && !anim.list && !anim.particleList &&
        !anim.ribbonList) {
        std::cerr << "[dtrace] pass --draw-trace-record <file> or --draw-trace-check <file>" << std::endl;
        return exit_code::kUsage;
    }
    auto& pipe = renderer.Pipeline();
    if (!InitDevice(pipe, ctx.backend, "dtrace"))
        return exit_code::kDevice;

    // After InitDevice, never before: the product selection repoints the content
    // root, and the engine's own `.bls` shaders read through the same provider.
    if (game != ProductId::Neutral)
        scene.GetContentProvider().SetGame(game);

    const renderer::RenderTargetId tid = pipe.CreateOffscreenTarget(kTraceTargetSize, kTraceTargetSize);
    if (!tid) {
        std::cerr << "[dtrace] CreateOffscreenTarget failed" << std::endl;
        return exit_code::kDevice;
    }
    pipe.SetPrimaryTarget(tid);
    // G2 reads back through ReadbackTarget, which applies no conversion. The
    // capture ring has a GPU-side sRGB flag, so leaving it on would let an
    // encoding change silently re-baseline the golden.
    pipe.EnableFrameCapture(false);
    ApplyRenderArms(renderer, o);

    // The perturbation arm: a different first handle puts every actor in a
    // different hash bucket. Before the first spawn.
    if (o.perturbSeed > 0)
        scene.SeedActorIds(static_cast<model::ActorId>(o.perturbSeed));

    // Also the provider's disk search root (SetPE1BasePath sets both), which is
    // why --content-root lands here rather than at startup: the model's own
    // folder would otherwise overwrite it.
    scene.SetPE1BasePath(ctx.contentRoot.empty() ? ctx.model.parent_path()
                                                 : io::FsPathFromUtf8(ctx.contentRoot));
    const std::vector<model::Actor*> spawned =
        SpawnInstances(renderer, scene, ctx.model, o.instances, o.perturbSeed, wemProfile);
    if (spawned.empty()) {
        std::cerr << "[dtrace] SpawnUnit failed: " << io::PathToUtf8(ctx.model) << std::endl;
        return exit_code::kSpawn;
    }
    model::Actor& hero = *spawned.front();
    draw_trace::ApplyD3Outfit(renderer, scene, spawned, anim, ctx.model, ctx.contentRoot);
    if (const auto failed = draw_trace::AttachAnimations(spawned, attachAnims))
        return *failed;

    if (auto* dnc = renderer.GetDncService())
        dnc->SetTimeOfDay(kTimeOfDay);

    auto& cam = scene.Camera();
    cam.SetOrbitalMode();
    cam.SetTarget(0.0f, 0.0f, kCameraTargetZ);
    cam.SetPitch(kCameraPitch);
    // 0.7 unless a caller walks the camera round: a billboard is only a
    // billboard when the view moves, so the war3-diff arm renders several.
    cam.SetYaw(o.cameraYaw);
    cam.SetDistance(static_cast<f32>(o.cameraDistance));

    const Settle settle = SettleAssets(renderer, scene);
    if (!settle.settled) {
        std::cerr << "[dtrace] assets never settled after " << settle.iterations << " iterations"
                  << std::endl;
        return exit_code::kTraceIo;
    }
    std::cout << "[dtrace] " << io::PathToUtf8(ctx.model.filename()) << ": settled in "
              << settle.iterations << " iteration(s), " << (o.hd ? "HD" : "SD") << ", " << ctx.frames
              << " frames" << std::endl;

    // The mesh overlay's selection arm, after the settle when the geosets exist.
    if (o.selectStride > 0)
        draw_trace::SelectMeshElements(renderer, spawned, o.selectStride);

    // G5's scenario. After the settle, not at spawn: the animation source arrives
    // with the template, so a sequence table asked for earlier is empty.
    const auto seqs = hero.animation.Sequences();
    if (draw_trace::PrintListing(renderer, hero, seqs, anim)) {
        pipe.Shutdown();
        return exit_code::kPass;
    }
    draw_trace::ApplyScenario(renderer, spawned, seqs, anim);
    const i32 switchSeq = draw_trace::ResolveSequenceSpec(seqs, anim.switchSequence);
    const i32 layerSeq = draw_trace::ResolveSequenceSpec(seqs, anim.layerSequence);
    if (anim.probe)
        draw_trace::PrintProbeSetup(hero);

    // ---- Capture ----
    renderer::Viewport vp;
    vp.target = tid;
    vp.camera = &cam;

    auto& rec = dbg::DrawTraceRecorder::Instance();
    rec.Clear();
    rec.Begin();
    bool needAppeared = false;
    const u64 arrivalAtStart = renderer.AssetArrivalCounter();
    for (i32 i = 0; i < ctx.frames; ++i) {
        // Before the update, so the frame this fires on is the first to render
        // with it; applied afterwards it would land a frame late and put the
        // baseline's blend curve out of step with the plan.
        if (i == anim.switchFrame && switchSeq >= 0) {
            for (auto* a : spawned)
                a->animation.SetActiveSequenceIndex(switchSeq);
            std::cout << "[dtrace] scenario: frame " << i << " switch -> [" << switchSeq << "] "
                      << seqs[static_cast<usize>(switchSeq)].name << std::endl;
        }
        if (i == anim.layerFrame && layerSeq >= 0) {
            renderer::animation::PlayDesc d;
            d.sequence = layerSeq;
            d.weight = anim.layerWeight;
            d.blendInMs = anim.layerBlendInMs;
            d.subtrack = anim.layerSubtrack;
            d.persistent = true; // survives the covered-play cull for the capture
            for (auto* a : spawned)
                a->animation.Playlist().Play(d, a->cursor.actorTimeMs);
            std::cout << "[dtrace] scenario: frame " << i << " layer + [" << layerSeq << "] "
                      << seqs[static_cast<usize>(layerSeq)].name << " @w" << anim.layerWeight;
            if (anim.layerSubtrack >= 0)
                std::cout << " subtrack " << anim.layerSubtrack;
            std::cout << std::endl;
        }
        scene.Update(kTraceStep);
        renderer.Ticker().Tick(kTraceStep);
        if (anim.attachList && i == 0)
            draw_trace::PrintAttachments(hero);
        if (anim.probe && (i % 20) == 0)
            draw_trace::PrintProbeFrame(hero, i);
        rec.BeginFrame(i);
        pipe.RenderViewport(vp);
        pipe.Present(tid);
        // Deliberately no pump: a need raised mid-capture is bytes the frame
        // wanted and did not have, and which frame they land on is a function of
        // the disk. Re-acquiring a resident path is fine — every PE1 birth does —
        // so this watches arrivals, not acquires.
        if (renderer.AssetArrivalCounter() != arrivalAtStart)
            needAppeared = true;
    }
    rec.End();
    pipe.Gfx()->WaitIdle();

    if (needAppeared) {
        // A Diablo III type-1 system spawns a whole `.acr` per emission, and its
        // assets legitimately arrive mid-capture. Still fatal for a trace; a
        // switch for someone who only wants to look at the picture.
        std::cerr << "[dtrace] a new asset need appeared mid-capture — the trace would be "
                     "timing-dependent; "
                  << (o.allowLateAssets ? "continuing anyway (--draw-trace-allow-late-assets)"
                                        : "aborting rather than recording it")
                  << std::endl;
        if (!o.allowLateAssets)
            return exit_code::kLateAsset;
    }

    const dbg::DrawTrace& trace = rec.Trace();
    PrintDrawCounts(trace);

    bool pass = true;
    std::string err;
    if (!o.recordPath.empty()) {
        if (!dbg::WriteTrace(trace, o.recordPath, err)) {
            std::cerr << "[dtrace] " << err << std::endl;
            return exit_code::kTraceIo;
        }
        std::cout << "[dtrace] recorded -> " << o.recordPath << std::endl;
    }
    if (!o.checkPath.empty()) {
        dbg::DrawTrace baseline;
        if (!dbg::ReadTrace(baseline, o.checkPath, err)) {
            std::cerr << "[dtrace] " << err << std::endl;
            return exit_code::kTraceIo;
        }
        dbg::CompareTolerance tol;
        if (o.distanceTol > 0.0f) {
            tol.distance = o.distanceTol;
            tol.requireCbHash = false;
        }
        std::string report;
        pass = dbg::CompareTraces(baseline, trace, tol, report);
        std::cout << "[dtrace] " << (pass ? "MATCH" : "DIFF") << ": " << report << std::endl;
    }
    if (!o.goldenPath.empty() && !RecordOrCheckGolden(pipe, tid, o))
        pass = false;

    std::cout << "[dtrace] " << (pass ? "PASS" : "FAIL") << std::endl;
    pipe.Shutdown();
    Exit(pass ? exit_code::kPass : exit_code::kDrawTraceFail);
}

} // namespace whiteout::flakes::harness
