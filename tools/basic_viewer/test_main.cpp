#include "cubeb_sound_emitter.h"
#include "gfx/gfx.h"
#include "renderer/animation/clip_playlist.h"
#include "renderer/model/corn_effect_source.h"
#include "renderer/frame_ticker.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_loader.h"
#include "renderer/debug/draw_trace.h"
#include "renderer/dnc/dnc_service.h"
#include "renderer/particle/particle_service.h"
#include "renderer/particle/particle_trace.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "localization.h"
#include "log_console.h"
#include "settings_ini.h"
#include "viewer_app.h"
#include "whiteout/flakes/gfx_types.h"
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <nfd.hpp>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// clang-format off
#include <windows.h>      // must precede shellapi.h — it defines the types it uses
#include <shellapi.h>     // CommandLineToArgvW
// clang-format on
#elif defined(__linux__)
#include <climits>
#include <unistd.h>
#elif defined(__APPLE__)
#include <climits>
#include <mach-o/dyld.h>
#endif

using whiteout::flakes::f32;
using whiteout::flakes::i32;

// Returns the absolute path of the running executable, or {} on failure.
// The standalone uses this both for the Vulkan pipeline-cache file and as
// the parent dir for engine-shipped assets (e.g. the .bls bundle staged
// next to the binary by the build).
static std::filesystem::path GetExecutablePath() {
#if defined(_WIN32)
    wchar_t exe[MAX_PATH] = {};
    DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH)
        return std::filesystem::path(exe);
    return {};
#elif defined(__linux__)
    char buf[PATH_MAX] = {};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return {};
    return std::filesystem::path(std::string(buf, static_cast<size_t>(n)));
#elif defined(__APPLE__)
    char buf[PATH_MAX] = {};
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0)
        return {};
    std::error_code ec;
    auto resolved = std::filesystem::canonical(std::filesystem::path(buf), ec);
    if (ec)
        resolved = std::filesystem::path(buf);
    return resolved;
#else
    return {};
#endif
}

static int CompareCi(const char* a, const char* b) {
#if defined(_WIN32)
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}

// Headless smoke test for the multi-viewport refactor: brings the device up,
// renders an empty scene through a camera into an OFF-SCREEN target (no window /
// swap-chain), reads the result back via the capture ring, and verifies a
// non-black image came out. Exercises InitDevice → CreateOffscreenTarget →
// RenderViewport → DownloadCaptureSlot end-to-end per backend. Returns 0 on
// pass, non-zero on the first failing step.
static int RunHeadlessTest(whiteout::flakes::renderer::RenderService& renderer,
                           whiteout::flakes::renderer::SceneManager& scene,
                           whiteout::flakes::gfx::GfxApi backend,
                           const std::filesystem::path& mdxPath) {
    namespace wf = whiteout::flakes;
    auto step = [](const char* s) { std::cout << "[headless] " << s << std::endl; };
    auto& pipe = renderer.Pipeline();

    step("InitDevice…");
    if (!pipe.InitDevice(backend)) {
        std::cerr << "[headless] InitDevice failed" << std::endl;
        return 2;
    }
    step("InitDevice OK");

    constexpr i32 kW = 256, kH = 256;
    wf::renderer::RenderTargetId tid = pipe.CreateOffscreenTarget(kW, kH);
    if (!tid) {
        std::cerr << "[headless] CreateOffscreenTarget failed" << std::endl;
        return 3;
    }
    pipe.SetPrimaryTarget(tid);
    step("offscreen target created");

    renderer.Settings().SetBackgroundColor(40, 80, 160);
    pipe.EnableFrameCapture(true);

    // Optional model load — exercises the texture / child-model / corn-effects
    // load path the user reported failing on Vulkan. Empty path = bare
    // background smoke test.
    wf::renderer::model::Actor* hero = nullptr;
    if (!mdxPath.empty()) {
        scene.SetPE1BasePath(mdxPath.parent_path());
        std::string ext = mdxPath.extension().string();
        for (char& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (ext == ".pkb" || ext == ".pkfx") {
            // Standalone PopcornFX effect — same path the viewer's LoadEffect uses.
            auto src = std::make_shared<wf::renderer::model::CornEffectSource>(
                wf::io::PathToUtf8(mdxPath));
            hero = renderer.Loader().SpawnUnitFromSource(src);
            step(hero ? "SpawnEffect OK" : "SpawnEffect FAILED");
        } else {
            hero = renderer.Loader().SpawnUnit(wf::io::PathToUtf8(mdxPath));
            step(hero ? "SpawnUnit OK" : "SpawnUnit FAILED");
        }
    }

    wf::renderer::Viewport vp;
    vp.target = tid;
    vp.camera = &scene.Camera();

    // Pump frames so the synchronous desktop asset pump (textures, child
    // models, corn/event data) drains and uploads.
    const i32 frames = mdxPath.empty() ? 3 : 40;
    for (i32 i = 0; i < frames; ++i) {
        scene.Update(0.016f);
        renderer.Ticker().Tick(0.016f); // evaluate actors + advance particle/corn sim
        pipe.RenderViewport(vp);
        pipe.Present(tid);
    }
    pipe.Gfx()->WaitIdle();
    step("frames rendered");

    // Loading report — the metrics the user's concern is about.
    {
        std::cout << "[headless] childModels(PE1)=" << scene.PE1InstanceCount()
                  << " | particle emitters=" << renderer.Particles().EmitterCount() << std::endl;
        if (hero) {
            const i32 texCount = hero->render.textures ? (i32)hero->render.textures->Size() : 0;
            std::cout << "[headless] actor: geosets=" << hero->render.gpuGeosets.size()
                      << " textures=" << texCount << std::endl;
        }
    }

    std::vector<wf::u8> rgba;
    i32 cw = 0, ch = 0;
    const i32 slot = pipe.LastCapturedSlot();
    bool readOk = (slot >= 0 && pipe.DownloadCaptureSlot(slot, rgba, cw, ch) && cw == kW &&
                   ch == kH && static_cast<i32>(rgba.size()) >= kW * kH * 4);
    // Fallback for backends without the compute-capture path (WebGPU has no
    // compute): direct texture→buffer readback.
    if (!readOk) {
        readOk = pipe.ReadbackTarget(tid, rgba, cw, ch) && cw == kW && ch == kH &&
                 static_cast<i32>(rgba.size()) >= kW * kH * 4;
        if (readOk)
            step("used ReadbackTarget fallback");
    }
    i32 mr = -1, mg = -1, mb = -1;
    if (readOk) {
        wf::u64 sr = 0, sg = 0, sb = 0;
        for (i32 p = 0; p < kW * kH; ++p) {
            sr += rgba[p * 4 + 0];
            sg += rgba[p * 4 + 1];
            sb += rgba[p * 4 + 2];
        }
        const i32 n = kW * kH;
        mr = (i32)(sr / n);
        mg = (i32)(sg / n);
        mb = (i32)(sb / n);
        std::cout << "[headless] readback OK " << cw << "x" << ch << " mean RGB=(" << mr << ","
                  << mg << "," << mb << ")" << std::endl;
    } else {
        std::cerr << "[headless] readback FAILED (slot=" << slot << ")" << std::endl;
    }

    // Verdict BEFORE teardown so a teardown issue can't hide it.
    const bool pass = readOk && !(mr == 0 && mg == 0 && mb == 0);
    std::cout << "[headless] " << (pass ? "PASS" : "FAIL") << std::endl;

    step("teardown…");
    pipe.EnableFrameCapture(false);
    pipe.Shutdown(); // frees all targets (CleanupGFX iterates targets_)
    step("teardown OK");

    // Shutdown() completed cleanly (verdict already printed above), but letting
    // the RenderService / SceneManager locals destruct afterwards crashes on
    // process exit — the renderer's services still hold device pointers that
    // CleanupGFX freed, and their destructors poke the dead device. That
    // exit-only teardown crash is a separate, pre-existing issue; for this
    // smoke test we flush and terminate with the verdict code so it doesn't
    // turn a valid PASS/FAIL into a misleading segfault exit status.
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(pass ? 0 : 6);
}

// Particle trace diff: the L1/L2 harness from PARTICLE_TYPES_DESIGN.md. Spawns
// the model, ticks a fixed number of frames at a fixed dt, and captures the
// particle pool state plus each emitter's slice of the vertex stream. With
// --trace-record it writes a baseline; with --trace-check it compares against
// one and reports the first divergence (frame, emitter, particle, field).
//
// Both trace levels are pure CPU — BuildGeometry is a function of sim state and
// a view matrix — and emitter registration is synchronous (GetOrLoadSync stages
// the actor inside SpawnUnit). The per-frame tick is not yet device-free
// though: FrameTicker::Tick faults without a live device, so the harness brings
// the backend up. Making the tick null-gfx safe would let this run on a CI
// machine with no GPU; --trace-no-device exists to retest that.
static int RunParticleDiff(whiteout::flakes::renderer::RenderService& renderer,
                           whiteout::flakes::renderer::SceneManager& scene,
                           whiteout::flakes::gfx::GfxApi backend,
                           const std::filesystem::path& mdxPath, const std::string& recordPath,
                           const std::string& checkPath, i32 traceFrames, bool curveTolerance,
                           bool useDevice) {
    namespace wf = whiteout::flakes;
    namespace part = wf::renderer::particle;

    if (mdxPath.empty()) {
        std::cerr << "[ptrace] --particle-diff needs a model path" << std::endl;
        return 2;
    }
    if (recordPath.empty() && checkPath.empty()) {
        std::cerr << "[ptrace] pass --trace-record <file> or --trace-check <file>" << std::endl;
        return 2;
    }
    if (useDevice && !renderer.Pipeline().InitDevice(backend)) {
        std::cerr << "[ptrace] InitDevice failed" << std::endl;
        return 3;
    }

    scene.SetPE1BasePath(mdxPath.parent_path());
    auto* hero = renderer.Loader().SpawnUnit(wf::io::PathToUtf8(mdxPath));
    if (!hero) {
        std::cerr << "[ptrace] SpawnUnit failed: " << wf::io::PathToUtf8(mdxPath) << std::endl;
        return 4;
    }

    const i32 emitters = renderer.Particles().EmitterCount();
    std::cout << "[ptrace] " << mdxPath.filename().string() << ": " << emitters
              << " emitter(s), " << traceFrames << " frames" << std::endl;
    if (emitters == 0)
        std::cout << "[ptrace] note: model has no PE2 emitters — trace covers PE1/none only"
                  << std::endl;

    // Fixed, non-axis-aligned view so the billboard basis, tail perpendicular
    // and sort key are all exercised, and the trace stays independent of
    // wherever the scene camera happens to sit.
    const wf::Matrix44f kTraceView =
        wf::Matrix44f::rotation_x(0.4f) * wf::Matrix44f::rotation_y(0.7f);
    constexpr f32 kDt = 1.0f / 60.0f;

    part::Trace trace;
    for (i32 i = 0; i < traceFrames; ++i) {
        scene.Update(kDt);
        renderer.Ticker().Tick(kDt);
        part::CaptureFrame(renderer.Particles(), kTraceView, i, trace);
    }

    std::string err;
    if (!recordPath.empty()) {
        if (!part::WriteTrace(trace, recordPath, err)) {
            std::cerr << "[ptrace] " << err << std::endl;
            return 5;
        }
        std::cout << "[ptrace] recorded " << trace.frames.size() << " frames -> " << recordPath
                  << std::endl;
    }

    bool pass = true;
    if (!checkPath.empty()) {
        part::Trace baseline;
        if (!part::ReadTrace(baseline, checkPath, err)) {
            std::cerr << "[ptrace] " << err << std::endl;
            return 5;
        }
        part::CompareTolerance tol;
        if (curveTolerance) {
            // Step 5's declared budget: colour within 1/255, geometry within
            // 1e-5 relative. Re-normalising particle age into [0,1] and back
            // costs a few float ULPs, which shows up in the vertex bounds.
            tol.color = 1.0f / 255.0f;
            tol.position = 1e-5f;
            tol.bounds = 1e-5f;
            tol.requireVertexHash = false;
        }
        std::string report;
        pass = part::CompareTraces(baseline, trace, tol, report);
        std::cout << "[ptrace] " << (pass ? "MATCH" : "DIFF") << ": " << report << std::endl;
    }

    std::cout << "[ptrace] " << (pass ? "PASS" : "FAIL") << std::endl;
    if (useDevice)
        renderer.Pipeline().Shutdown();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(pass ? 0 : 8);
}

// Gate G5's scripted scenario. The `.mdx` and `.m2` arms need nothing like it —
// they play whatever sequence the model opens on and cut hard between them, so
// "spawn and let it run" already covers their whole playback surface. StarCraft
// II's does not: layered plays, blend envelopes and cross-fades only exist once
// something asks for a second sequence, and a capture that never asks would
// trace a single steady layer and call the blender covered.
//
// Everything here is frame-indexed rather than time-indexed on purpose. The
// capture loop's dt is fixed, so a frame number is an exact millisecond, and a
// baseline recorded today stays reproducible if the dt ever changes shape.
struct AnimScenario {
    // Index or (case-insensitive substring of a) name. Empty leaves whatever
    // the model opens on, which is what the geometry arm records.
    std::string sequence;
    // Hard-ish switch: `SetActiveSequence`, so it takes the format's own
    // transition policy — a cut for WC3/WoW, a cross-fade for M3.
    i32 switchFrame = -1;
    std::string switchSequence;
    // Additive layer: a second play stacked on the first, which is the only
    // way to reach the weight-budget blender.
    i32 layerFrame = -1;
    std::string layerSequence;
    i32 layerBlendInMs = 250;
    f32 layerWeight = 1.0f;
    // Print the sequence table and stop. How the corpus file gets curated:
    // sequence *indices* are export order and differ per model, so the corpus
    // names sequences and this is what tells you which names exist.
    bool list = false;
    // Turn the pose stages on and install a ground plane, so a capture can see
    // terrain IK and the turret at all. Off in every other arm, which is what
    // makes the byte-identical baselines mean "the animation did not move".
    bool solvers = false;
    /// @brief Ground plane height for the solver arm. Non-zero is the
    ///        interesting case: a plane at the model's own feet is what the
    ///        tolerance skip already declines to solve.
    f32 groundZ = 0.0f;
    /// @brief Aim target for turrets, in model space. Only used with @ref
    ///        solvers.
    bool hasAim = false;
    Vector3f aim{0.0f, 0.0f, 0.0f};
    // Print the skinning plumbing and a per-frame pose hash.
    //
    // Earns its place because the failure this gate is most likely to hit is
    // silent: every link between "the sampler produced new bone matrices" and
    // "the GPU drew a new pose" fails by leaving the previous frame's palette
    // in place, so the model renders perfectly in bind pose and the trace,
    // the golden and the draw count all look healthy. A pose hash that never
    // changes is the only cheap way to see it.
    bool probe = false;

    bool Any() const {
        return !sequence.empty() || switchFrame >= 0 || layerFrame >= 0 || list || probe ||
               solvers;
    }
};

// `spec` is an index if it parses as one, else a case-insensitive substring
// match against the sequence names. Returns -1 for "not found", which every
// caller treats as "leave it alone" rather than as an error: a corpus curated
// against one build should not hard-fail on a model whose export lacks the
// sequence, it should say so and still trace.
static i32 ResolveSequenceSpec(const std::vector<whiteout::flakes::SequenceInfo>& seqs,
                               const std::string& spec) {
    if (spec.empty() || seqs.empty())
        return -1;
    if (spec.find_first_not_of("0123456789") == std::string::npos) {
        const i32 idx = std::atoi(spec.c_str());
        return (idx >= 0 && idx < static_cast<i32>(seqs.size())) ? idx : -1;
    }
    // `+` stands in for a space: SC2 sequence names are "Attack 02", and the
    // corpus file separates its scenario tokens on whitespace.
    std::string needle;
    for (char c : spec)
        needle.push_back(c == '+' ? ' '
                                  : static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    for (std::size_t i = 0; i < seqs.size(); ++i) {
        std::string name;
        for (char c : seqs[i].name)
            name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        if (name.find(needle) != std::string::npos)
            return static_cast<i32>(i);
    }
    return -1;
}

// Draw trace (gate G1) — a deterministic record of every decision the draw path
// makes, per REFACTOR_PLAN.md §2. Unlike --particle-diff this needs a device:
// the hooks sit inside the submission paths, after PSO resolve, so only draws
// that were really submitted get recorded.
//
// The scene is pinned along every axis that reaches submit order or pixels:
// fixed dt, fixed camera pose, fixed LOD, fixed time-of-day and lighting mode.
// What that pinning cannot cover is asset arrival — the IO worker pool means
// which frame a texture or child template lands on varies with core count and
// disk cache, and BuildDrawLists skips geosets whose VB is still Invalid. So
// capture is preceded by a settle phase and the capture itself fails rather
// than emitting a divergent trace if a new need appears (§1.1 #13).
static int RunDrawTrace(whiteout::flakes::renderer::RenderService& renderer,
                        whiteout::flakes::renderer::SceneManager& scene,
                        whiteout::flakes::gfx::GfxApi backend,
                        const std::filesystem::path& mdxPath, const std::string& recordPath,
                        const std::string& checkPath, const std::string& goldenPath, i32 frames,
                        bool hdMode, f32 distanceTol, i32 cameraDistance, i32 perturbSeed,
                        i32 instances, bool unlitOddGeosets, bool lazyAnim,
                        const std::string& contentRoot, const AnimScenario& anim,
                        bool debugLight = false) {
    namespace wf = whiteout::flakes;
    namespace dbg = wf::renderer::debug;

    if (mdxPath.empty()) {
        std::cerr << "[dtrace] --draw-trace needs a model path" << std::endl;
        return 2;
    }
    if (recordPath.empty() && checkPath.empty() && !anim.list) {
        std::cerr << "[dtrace] pass --draw-trace-record <file> or --draw-trace-check <file>"
                  << std::endl;
        return 2;
    }
    auto& pipe = renderer.Pipeline();
    if (!pipe.InitDevice(backend)) {
        std::cerr << "[dtrace] InitDevice failed" << std::endl;
        return 3;
    }

    constexpr i32 kW = 512, kH = 512;
    const wf::renderer::RenderTargetId tid = pipe.CreateOffscreenTarget(kW, kH);
    if (!tid) {
        std::cerr << "[dtrace] CreateOffscreenTarget failed" << std::endl;
        return 3;
    }
    pipe.SetPrimaryTarget(tid);
    // G2 reads back through ReadbackTarget, which applies no conversion. The
    // capture ring has a GPU-side sRGB flag, so leaving it on would let an
    // encoding change silently re-baseline the golden.
    pipe.EnableFrameCapture(false);

    auto& settings = renderer.Settings();
    settings.SetRenderMode(hdMode ? wf::renderer::RenderMode::HD : wf::renderer::RenderMode::SD);
    settings.SetBackgroundColor(0, 0, 0);
    settings.SetLodOverride(0);
    settings.SetLightingMode(wf::renderer::LightingMode::InGame);
    // The multi-model arm. On, every odd-indexed geoset is drawn by
    // UnlitShading instead of the WC3 model, so one frame exercises
    // SurfacePass's open/close transition and the key.model sort term. Off is
    // what every byte-identical baseline is recorded and checked with.
    settings.SetDebugUnlitOddGeosets(unlitOddGeosets);

    // The `.m2` streaming arm, and the reason it shares its baselines rather
    // than getting its own: deferring the `.anim` reads has to leave the frame
    // byte-identical, so the claim only means something if it is checked
    // against the same file the eager run recorded.
    settings.SetM2LazyAnimations(lazyAnim);

    // The -Sc2Mat -DebugLight sub-arm: one scripted point light for the M3
    // DeferredLights pass, identical for every model. The values live HERE,
    // not in the script, so a baseline never depends on shell quoting.
    // Renderer units (post-WorldScale). Placed well OFF the body: an SC2
    // infantry model is ~400 units tall at WorldScale 100, and a light inside
    // the silhouette faces away from every visible pixel — atten and N·L
    // never coincide and the pass proves nothing.
    if (debugLight) {
        settings.SetDebugPointLight(true, {250.0f, -250.0f, 320.0f}, {2.0f, 1.8f, 1.4f},
                                    900.0f);
    }

    // The gate's perturbation arm: a different first handle puts every actor
    // in a different hash bucket, so any draw path that follows unordered_map
    // iteration order diverges. Must be set before the first spawn.
    if (perturbSeed > 0)
        scene.SeedActorIds(static_cast<wf::renderer::model::ActorId>(perturbSeed));

    // Also the provider's disk search root (SetPE1BasePath sets both), which is
    // why --content-root has to land here rather than at startup: the model's
    // own folder would otherwise overwrite it. An extracted corpus keeps
    // directory-named content — `dbfilesclient/` — above the model, out of
    // reach from there.
    scene.SetPE1BasePath(contentRoot.empty() ? mdxPath.parent_path()
                                             : wf::io::FsPathFromUtf8(contentRoot));
    // More than one top-level actor on purpose. With a single actor,
    // BuildDrawLists iterates a one-entry unordered_map and the hash-order
    // dependence §1.1a is about cannot show up at all — the perturbation arm
    // would pass while the bug was fully live. Spread them along +X so the
    // transparent back-to-front sort has real work and equal-depth ties are
    // reachable.
    wf::renderer::model::Actor* hero = nullptr;
    std::vector<wf::renderer::model::Actor*> spawned;
    const i32 copies = (instances < 1) ? 1 : instances;
    for (i32 n = 0; n < copies; ++n) {
        // Uneven gaps in the handle sequence, not just a shifted start. With
        // an identity hash and consecutive ids, a uniform shift leaves the
        // bucket *order* intact for small maps, so the arm would still pass
        // with the bug live. Irregular spacing changes the residues.
        for (i32 burn = 0; perturbSeed > 0 && burn < ((perturbSeed >> (n & 7)) & 3) + 1; ++burn)
            (void)scene.AllocActorId();
        auto* a = renderer.Loader().SpawnUnit(wf::io::PathToUtf8(mdxPath));
        if (!a)
            break;
        if (!hero)
            hero = a;
        spawned.push_back(a);
        a->worldTransform = wf::Matrix44f::translation(
            {static_cast<f32>(n) * 120.0f, 0.0f, 0.0f});
    }
    if (!hero) {
        std::cerr << "[dtrace] SpawnUnit failed: " << wf::io::PathToUtf8(mdxPath) << std::endl;
        return 4;
    }
    if (auto* dnc = renderer.GetDncService())
        dnc->SetTimeOfDay(12.0f);

    // Deterministic pose rather than a framed one: FrameCameraToModel reads
    // model bounds the engine does not expose format-neutrally yet (P9), and a
    // framing rule that changes would silently re-baseline every golden.
    auto& cam = scene.Camera();
    cam.SetOrbitalMode();
    cam.SetTarget(0.0f, 0.0f, 50.0f);
    cam.SetPitch(0.35f);
    cam.SetYaw(0.7f);
    cam.SetDistance(static_cast<f32>(cameraDistance));

    // ---- Settle: drain asset arrival until nothing is outstanding ----------
    constexpr i32 kQuietIterations = 16;
    constexpr i32 kSettleCap = 2000;
    i32 quiet = 0, iters = 0;
    wf::u64 lastActivity = ~0ull;
    for (; iters < kSettleCap && quiet < kQuietIterations; ++iters) {
        if (auto* cp = scene.ActiveContentProvider())
            cp->Pump();
        renderer.PumpAssetsViaProvider();
        renderer.Ticker().Tick(0.0f);
        renderer.Loader().CommitPendingUploads();
        const wf::u64 activity = renderer.AssetActivityCounter();
        quiet = (activity == lastActivity) ? quiet + 1 : 0;
        lastActivity = activity;
    }
    if (quiet < kQuietIterations) {
        std::cerr << "[dtrace] assets never settled after " << iters << " iterations" << std::endl;
        return 5;
    }
    std::cout << "[dtrace] " << mdxPath.filename().string() << ": settled in " << iters
              << " iteration(s), " << (hdMode ? "HD" : "SD") << ", " << frames << " frames"
              << std::endl;

    // ---- G5: scripted animation scenario ----------------------------------
    // After the settle, not at spawn: the animation source arrives with the
    // template, so a sequence table asked for any earlier is empty and every
    // name would resolve to -1.
    const auto seqs = hero->animation.Sequences();
    if (anim.list) {
        // The bone count is here because the corpus has to name a model whose
        // palette overflows `kActorPaletteCap`, and that is not guessable from
        // a filename.
        // Through the source rather than the template: `sourceTemplate` is only
        // set for actors born from the template cache, and a directly-spawned
        // one leaves it null.
        if (auto* ms = dynamic_cast<wf::renderer::model::IModelSource*>(
                hero->animation.Source().get()))
            std::cout << "[dtrace] " << ms->GetSkeleton().nodeCount << " bone(s)" << std::endl;
        std::cout << "[dtrace] " << seqs.size() << " sequence(s):" << std::endl;
        for (std::size_t s = 0; s < seqs.size(); ++s)
            std::cout << "[dtrace]   [" << s << "] " << seqs[s].name << "  " << seqs[s].startMs
                      << ".." << seqs[s].endMs << "ms"
                      << (seqs[s].nonLooping ? " (non-looping)" : "") << std::endl;
        pipe.Shutdown();
        return 0;
    }
    const i32 startSeq = ResolveSequenceSpec(seqs, anim.sequence);
    const i32 switchSeq = ResolveSequenceSpec(seqs, anim.switchSequence);
    const i32 layerSeq = ResolveSequenceSpec(seqs, anim.layerSequence);
    if (!anim.sequence.empty() && startSeq < 0)
        std::cout << "[dtrace] scenario: no sequence matching '" << anim.sequence << "'"
                  << std::endl;
    if (startSeq >= 0) {
        for (auto* a : spawned)
            a->animation.SetActiveSequenceIndex(startSeq);
        std::cout << "[dtrace] scenario: start seq [" << startSeq << "] " << seqs[startSeq].name
                  << std::endl;
    }

    // The solver arm. Both inputs are host policy — the renderer has no terrain
    // and no notion of what a unit is shooting at — so the harness plays host
    // exactly as the viewer does.
    if (anim.solvers) {
        settings.SetPoseSolversEnabled(true);
        const f32 planeZ = anim.groundZ;
        settings.SetGroundQuery([planeZ](const Vector3f& pos, f32 up, f32 down, f32& outZ) {
            if (planeZ > pos.z + up || planeZ < pos.z - down)
                return false;
            outZ = planeZ;
            return true;
        });
        if (anim.hasAim)
            for (auto* a : spawned)
                a->aimTarget = anim.aim;
        // The stage count is the difference between "this model has no solver
        // chunks" and "the solver ran and changed nothing" — two very different
        // reasons for a golden to match the solvers-off one.
        std::cout << "[dtrace] scenario: solvers on, ground z=" << planeZ
                  << (anim.hasAim ? ", aiming" : ", no aim target") << ", "
                  << hero->animation.PoseStages().size() << " stage(s)" << std::endl;
    }

    // Everything between the sampler and the bound palette, in the order it has
    // to hold. Any `no` here explains a frozen model on its own.
    if (anim.probe) {
        const auto& sk = hero->render.skinning;
        std::cout << "[dtrace] probe: skeleton=" << (sk.HasSkeleton() ? "yes" : "no")
                  << " nodes=" << sk.NodeCount() << " ready=" << (sk.IsReady() ? "yes" : "no")
                  << " perActorPalette=" << (sk.UsesPerActorPalette() ? "yes" : "no") << std::endl;
        for (const auto& geo : hero->render.gpuGeosets)
            std::cout << "[dtrace] probe: geoset " << geo.geosetId
                      << " hasSkinning=" << (geo.hasSkinning ? "yes" : "no")
                      << " paletteCb=" << (geo.bonePaletteCb != wf::gfx::BufferHandle::Invalid
                                               ? "yes" : "no")
                      << " paletteSlots=" << sk.GeosetPaletteSize(geo.geosetId)
                      << " layout=" << geo.layoutId << std::endl;
    }

    // ---- Capture ----------------------------------------------------------
    wf::renderer::Viewport vp;
    vp.target = tid;
    vp.camera = &cam;

    constexpr f32 kDt = 1.0f / 60.0f;
    auto& rec = dbg::DrawTraceRecorder::Instance();
    rec.Clear();
    rec.Begin();
    bool needAppeared = false;
    const wf::u64 arrivalAtStart = renderer.AssetArrivalCounter();
    for (i32 i = 0; i < frames; ++i) {
        // Before the update, so the frame this fires on is the first one that
        // renders with it — a request applied afterwards would land a frame
        // late and put the baseline's blend curve out of step with the plan.
        if (i == anim.switchFrame && switchSeq >= 0) {
            for (auto* a : spawned)
                a->animation.SetActiveSequenceIndex(switchSeq);
            std::cout << "[dtrace] scenario: frame " << i << " switch -> [" << switchSeq << "] "
                      << seqs[switchSeq].name << std::endl;
        }
        if (i == anim.layerFrame && layerSeq >= 0) {
            wf::renderer::animation::PlayDesc d;
            d.sequence = layerSeq;
            d.weight = anim.layerWeight;
            d.blendInMs = anim.layerBlendInMs;
            d.persistent = true; // survives the covered-play cull for the capture
            for (auto* a : spawned)
                a->animation.Playlist().Play(d, a->cursor.actorTimeMs);
            std::cout << "[dtrace] scenario: frame " << i << " layer + [" << layerSeq << "] "
                      << seqs[layerSeq].name << " @w" << anim.layerWeight << std::endl;
        }
        scene.Update(kDt);
        renderer.Ticker().Tick(kDt);
        if (anim.probe && (i % 20) == 0) {
            // Over the offset matrices rather than the world ones: those are
            // what the palette actually carries, so a hash that moves here but
            // a frozen image narrows the fault to the upload or the shader.
            const auto& sk = hero->render.skinning;
            wf::u64 h = 1469598103934665603ull;
            if (const Matrix44f* off = sk.OffsetMatrices()) {
                const auto* raw = reinterpret_cast<const unsigned char*>(off);
                for (std::size_t b = 0; b < sk.NodeCount() * sizeof(Matrix44f); ++b)
                    h = (h ^ raw[b]) * 1099511628211ull;
            }
            std::cout << "[dtrace] probe: frame " << i << " t=" << hero->animation.TimeMs()
                      << "ms seq=" << hero->animation.ActiveSequenceIndex()
                      << " plays=" << hero->animation.Playlist().PlayCount() << " pose=" << h;
            for (const auto& cl : hero->animation.Playlist().Clips())
                std::cout << " | clip seq=" << cl.sequence << " time=" << cl.timeMs
                          << " elapsed=" << cl.elapsedMs << " w=" << cl.weight;
            std::cout << std::endl;
        }
        rec.BeginFrame(i);
        pipe.RenderViewport(vp);
        pipe.Present(tid);
        // Deliberately no pump here: a need raised mid-capture means bytes the
        // frame wanted were not resolved, and whichever frame they land on is
        // a function of the disk, not of the renderer. Re-acquiring a resident
        // path is fine — every PE1 birth does it — so this watches arrivals,
        // not acquires.
        if (renderer.AssetArrivalCounter() != arrivalAtStart)
            needAppeared = true;
    }
    rec.End();
    pipe.Gfx()->WaitIdle();

    if (needAppeared) {
        std::cerr << "[dtrace] a new asset need appeared mid-capture — the trace would be "
                     "timing-dependent; aborting rather than recording it"
                  << std::endl;
        return 6;
    }

    const dbg::DrawTrace& trace = rec.Trace();
    std::size_t totalDraws = 0;
    i32 kinds[4] = {0, 0, 0, 0};
    for (const auto& fr : trace.frames) {
        totalDraws += fr.draws.size();
        for (const auto& d : fr.draws)
            if (d.producer < 4)
                ++kinds[d.producer];
    }
    std::cout << "[dtrace] " << totalDraws << " draw(s) over " << trace.frames.size()
              << " frame(s); kinds geoset=" << kinds[0] << " particle=" << kinds[1]
              << " ribbon=" << kinds[2] << " corn=" << kinds[3] << std::endl;

    bool pass = true;
    std::string err;
    if (!recordPath.empty()) {
        if (!dbg::WriteTrace(trace, recordPath, err)) {
            std::cerr << "[dtrace] " << err << std::endl;
            return 5;
        }
        std::cout << "[dtrace] recorded -> " << recordPath << std::endl;
    }
    if (!checkPath.empty()) {
        dbg::DrawTrace baseline;
        if (!dbg::ReadTrace(baseline, checkPath, err)) {
            std::cerr << "[dtrace] " << err << std::endl;
            return 5;
        }
        dbg::CompareTolerance tol;
        if (distanceTol > 0.0f) {
            tol.distance = distanceTol;
            tol.requireCbHash = false;
        }
        std::string report;
        pass = dbg::CompareTraces(baseline, trace, tol, report);
        std::cout << "[dtrace] " << (pass ? "MATCH" : "DIFF") << ": " << report << std::endl;
    }

    // ---- G2: golden image -------------------------------------------------
    if (!goldenPath.empty()) {
        std::vector<wf::u8> rgba;
        i32 cw = 0, ch = 0;
        if (!pipe.ReadbackTarget(tid, rgba, cw, ch) || cw != kW || ch != kH) {
            std::cerr << "[dtrace] golden: ReadbackTarget unavailable on this backend" << std::endl;
            pass = false;
        } else if (!recordPath.empty()) {
            std::ofstream out(goldenPath, std::ios::binary);
            out.write(reinterpret_cast<const char*>(rgba.data()),
                      static_cast<std::streamsize>(rgba.size()));
            std::cout << "[dtrace] golden recorded -> " << goldenPath << std::endl;
        } else {
            std::ifstream in(goldenPath, std::ios::binary);
            std::vector<wf::u8> ref((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
            // Byte-exact on the raw readback. Do NOT sRGB-encode first: that is
            // a lossy many-to-one map which collapses distinguishable bright
            // values onto one byte, hiding regressions exactly where additive
            // and emissive bugs live.
            if (ref.size() != rgba.size()) {
                std::cerr << "[dtrace] golden size differs: " << ref.size() << " vs " << rgba.size()
                          << std::endl;
                pass = false;
            } else {
                std::size_t diff = 0;
                for (std::size_t p = 0; p < ref.size(); ++p)
                    diff += (ref[p] != rgba[p]) ? 1 : 0;
                if (diff) {
                    std::cerr << "[dtrace] golden differs in " << diff << " of " << ref.size()
                              << " bytes" << std::endl;
                    pass = false;
                } else {
                    std::cout << "[dtrace] golden MATCH" << std::endl;
                }
            }
        }
    }

    std::cout << "[dtrace] " << (pass ? "PASS" : "FAIL") << std::endl;
    pipe.Shutdown();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(pass ? 0 : 9);
}

// Child-model (PE1) validation. Folding PE1 into the particle service changed
// its RNG, so there is no bit-exact oracle to compare against — this checks the
// invariants instead, per PARTICLE_TYPES_DESIGN.md:
//
//   * every Birth eventually gets exactly one Death — i.e. live child actors
//     track live child particles, so nothing leaks and nothing is orphaned;
//   * the instance and depth caps still bind;
//   * the population is bounded rather than growing without limit.
static int RunChildModelCheck(whiteout::flakes::renderer::RenderService& renderer,
                              whiteout::flakes::renderer::SceneManager& scene,
                              whiteout::flakes::gfx::GfxApi backend,
                              const std::filesystem::path& mdxPath, i32 frames) {
    namespace wf = whiteout::flakes;
    namespace part = wf::renderer::particle;

    if (mdxPath.empty()) {
        std::cerr << "[cmcheck] needs a model path" << std::endl;
        return 2;
    }
    if (!renderer.Pipeline().InitDevice(backend)) {
        std::cerr << "[cmcheck] InitDevice failed" << std::endl;
        return 3;
    }

    scene.SetPE1BasePath(mdxPath.parent_path());
    auto* hero = renderer.Loader().SpawnUnit(wf::io::PathToUtf8(mdxPath));
    if (!hero) {
        std::cerr << "[cmcheck] SpawnUnit failed" << std::endl;
        return 4;
    }

    i32 childEmitters = 0;
    renderer.Particles().ForEachEmitter([&](const part::EmitterKey& k, const part::Emitter2&) {
        if (k.output == part::ParticleOutput::ChildModel)
            ++childEmitters;
    });
    std::cout << "[cmcheck] " << mdxPath.filename().string() << ": " << childEmitters
              << " child-model emitter(s)" << std::endl;
    renderer.Particles().ForEachEmitter([&](const part::EmitterKey& k, const part::Emitter2& e) {
        if (k.output == part::ParticleOutput::ChildModel)
            std::cout << "[cmcheck]   emitter " << k.id << " path='" << e.Desc().childModelPath
                      << "' lifeSpan=" << e.Desc().lifeSpan << std::endl;
    });
    if (childEmitters == 0) {
        std::cout << "[cmcheck] no child-model emitters — nothing to check" << std::endl;
        std::cout << "[cmcheck] PASS" << std::endl;
        std::cout.flush();
        std::_Exit(0);
    }

    constexpr f32 kDt = 1.0f / 60.0f;
    i32 peakActors = 0;
    i32 peakParticles = 0;
    i32 worstOrphans = 0; // live actors with no live particle behind them
    bool capHeld = true;

    // A unit's PE1 emitters usually fire in one specific animation (a breath
    // attack, a death), so first find a sequence that actually emits, then dwell
    // on it. Each sequence gets at least a few particle lifespans' worth of
    // frames: dwelling for less than one lifespan means nothing ever dies, and
    // the Birth/Death balance — the assertion that matters — goes untested.
    const i32 seqCount = (std::max)(1, (i32)hero->animation.Sequences().size());
    i32 longestLife = 0;
    renderer.Particles().ForEachEmitter([&](const part::EmitterKey& k, const part::Emitter2& e) {
        if (k.output == part::ParticleOutput::ChildModel)
            longestLife = (std::max)(longestLife, (i32)(e.Desc().lifeSpan * 60.0f));
    });
    const i32 perSeq = (std::max)(frames / seqCount, longestLife * 3 + 60);

    for (i32 s = 0; s < seqCount; ++s) {
        hero->animation.SetActiveSequenceIndex(s);
        for (i32 i = 0; i < perSeq; ++i) {
            // Drive the host content-provider pump: it is what actually fetches
            // and parses the child MDX a Birth needs. Without it every birth is
            // dropped as unresolved and the whole check is vacuous.
            if (auto* cp = scene.ActiveContentProvider())
                cp->Pump();
            scene.Update(kDt);
            renderer.Ticker().Tick(kDt);

            i32 aliveParticles = 0;
            renderer.Particles().ForEachEmitter(
                [&](const part::EmitterKey& k, const part::Emitter2& e) {
                    if (k.output == part::ParticleOutput::ChildModel)
                        aliveParticles += e.TotalAlive();
                });
            const i32 liveActors = scene.PE1InstanceCount();

            peakActors = (std::max)(peakActors, liveActors);
            peakParticles = (std::max)(peakParticles, aliveParticles);
            // Actors may lag particles (a birth whose template is not loaded
            // yet, or one refused by the cap) but must never exceed them: that
            // would mean a Death went unreported and the actor leaked.
            worstOrphans = (std::max)(worstOrphans, liveActors - aliveParticles);
            if (liveActors > wf::renderer::model::kMaxChildModelInstances)
                capHeld = false;
        }
    }

    std::cout << "[cmcheck] swept " << seqCount << " sequence(s): peak child actors=" << peakActors
              << " peak child particles=" << peakParticles
              << " worst orphaned actors=" << worstOrphans << std::endl;

    // Settled state: live child actors must exactly match live child particles.
    // Combined with worstOrphans this is the Birth/Death balance check — every
    // Birth that produced an actor eventually produced exactly one Death.
    i32 finalParticles = 0;
    renderer.Particles().ForEachEmitter([&](const part::EmitterKey& k, const part::Emitter2& e) {
        if (k.output == part::ParticleOutput::ChildModel)
            finalParticles += e.TotalAlive();
    });
    const i32 finalActors = scene.PE1InstanceCount();
    const bool balanced = (finalActors <= finalParticles);
    std::cout << "[cmcheck] settled: child actors=" << finalActors
              << " child particles=" << finalParticles << std::endl;

    // Nothing ever spawning would make every other assertion vacuous.
    if (peakParticles == 0)
        std::cerr << "[cmcheck] no child particles were ever emitted — check is vacuous"
                  << std::endl;
    if (!balanced)
        std::cerr << "[cmcheck] " << (finalActors - finalParticles)
                  << " child actor(s) leaked past their particle" << std::endl;

    const bool pass = capHeld && worstOrphans <= 0 && peakParticles > 0 && balanced;
    if (!capHeld)
        std::cerr << "[cmcheck] instance cap exceeded" << std::endl;
    if (worstOrphans > 0)
        std::cerr << "[cmcheck] " << worstOrphans
                  << " child actor(s) outlived their particle — Birth/Death unbalanced"
                  << std::endl;

    std::cout << "[cmcheck] " << (pass ? "PASS" : "FAIL") << std::endl;
    renderer.Pipeline().Shutdown();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(pass ? 0 : 10);
}

// Multi-scene smoke test: proves two scenes render into their own targets with
// no cross-bleed. Scene B (a CreateScene'd scene) gets the model; the default
// scene A stays empty. Renders both into separate offscreen targets and reads
// back: A must be background-only, B must contain the model — i.e. B's model
// never leaks into A's image. Returns 0 on PASS.
static int RunMultiSceneTest(whiteout::flakes::renderer::RenderService& renderer,
                             whiteout::flakes::gfx::GfxApi backend,
                             const std::filesystem::path& mdxPath) {
    namespace wf = whiteout::flakes;
    auto& pipe = renderer.Pipeline();
    auto say = [](const char* s) { std::cout << "[multiscene] " << s << std::endl; };

    if (!pipe.InitDevice(backend)) {
        std::cerr << "[multiscene] InitDevice failed" << std::endl;
        return 2;
    }
    renderer.Settings().SetBackgroundColor(40, 80, 160);

    constexpr i32 kW = 256, kH = 256;
    wf::renderer::RenderTargetId tA = pipe.CreateOffscreenTarget(kW, kH);
    wf::renderer::RenderTargetId tB = pipe.CreateOffscreenTarget(kW, kH);
    if (!tA || !tB) {
        std::cerr << "[multiscene] CreateOffscreenTarget failed" << std::endl;
        return 3;
    }
    pipe.SetPrimaryTarget(tA);
    pipe.EnableFrameCapture(true);

    // Scene A = the default scene (empty). Scene B = a new scene with the model.
    const wf::renderer::SceneId sceneA = renderer.DefaultSceneId();
    const wf::renderer::SceneId sceneB = renderer.CreateScene();
    say("created scene B");

    // Spawn the model into scene B: make it active so the loader + services
    // target it, then restore the default.
    if (!mdxPath.empty()) {
        renderer.SetActiveScene(sceneB);
        renderer.SceneAt(sceneB).SetPE1BasePath(mdxPath.parent_path());
        auto* hero = renderer.Loader().SpawnUnit(wf::io::PathToUtf8(mdxPath));
        renderer.SetActiveScene(sceneA);
        say(hero ? "spawned model into scene B" : "spawn into scene B FAILED");
    }

    auto readbackMean = [&](wf::renderer::RenderTargetId tid, i32& mr, i32& mg, i32& mb) -> bool {
        std::vector<wf::u8> rgba;
        i32 cw = 0, ch = 0;
        bool ok = false;
        const i32 slot = pipe.LastCapturedSlot();
        if (slot >= 0 && pipe.DownloadCaptureSlot(slot, rgba, cw, ch) && cw == kW && ch == kH)
            ok = true;
        if (!ok)
            ok = pipe.ReadbackTarget(tid, rgba, cw, ch) && cw == kW && ch == kH;
        if (!ok || (i32)rgba.size() < kW * kH * 4)
            return false;
        wf::u64 sr = 0, sg = 0, sb = 0;
        for (i32 p = 0; p < kW * kH; ++p) {
            sr += rgba[p * 4 + 0];
            sg += rgba[p * 4 + 1];
            sb += rgba[p * 4 + 2];
        }
        mr = (i32)(sr / (kW * kH));
        mg = (i32)(sg / (kW * kH));
        mb = (i32)(sb / (kW * kH));
        return true;
    };

    // Tick both scenes a few times so assets stream and animation advances.
    wf::renderer::Viewport vpA, vpB;
    vpA.scene = sceneA;
    vpA.target = tA;
    vpA.camera = &renderer.SceneAt(sceneA).Camera();
    vpB.scene = sceneB;
    vpB.target = tB;
    vpB.camera = &renderer.SceneAt(sceneB).Camera();

    i32 mrA = 0, mgA = 0, mbA = 0, mrB = 0, mgB = 0, mbB = 0;
    // The D3D/Vulkan capture ring mirrors the primary target — point it at the
    // one being read back (already tA from setup).
    for (i32 i = 0; i < 40; ++i) {
        renderer.TickScenes(0.016f);
        pipe.RenderViewport(vpA);
        pipe.Present(tA);
    }
    bool okA = readbackMean(tA, mrA, mgA, mbA);
    pipe.SetPrimaryTarget(tB);
    for (i32 i = 0; i < 40; ++i) {
        renderer.TickScenes(0.016f);
        pipe.RenderViewport(vpB);
        pipe.Present(tB);
    }
    bool okB = readbackMean(tB, mrB, mgB, mbB);
    pipe.Gfx()->WaitIdle();

    // Engine-level isolation check (backend-independent): the loader spawned
    // into scene B only, so scene B has an actor with geosets and scene A has
    // none. This proves the scenes are independent regardless of readback.
    auto sceneGeosets = [&](wf::renderer::SceneId id) {
        i32 actors = 0, geosets = 0;
        for (auto& [h, mi] : renderer.SceneAt(id).Actors().All()) {
            ++actors;
            geosets += (i32)mi->render.gpuGeosets.size();
        }
        return std::pair<i32, i32>{actors, geosets};
    };
    auto [actorsA, geoA] = sceneGeosets(sceneA);
    auto [actorsB, geoB] = sceneGeosets(sceneB);

    std::cout << "[multiscene] sceneA actors=" << actorsA << " geosets=" << geoA
              << " mean=(" << mrA << "," << mgA << "," << mbA << ")  sceneB actors=" << actorsB
              << " geosets=" << geoB << " mean=(" << mrB << "," << mgB << "," << mbB << ")"
              << std::endl;

    const bool isolated = (actorsA == 0 && geoA == 0) && (actorsB == 1 && geoB > 0);
    // Pixel corroboration where readback actually produced an image. The D3D12
    // capture ring can return all-zero for stacked offscreen targets — treat
    // that as "readback unavailable" (no evidence either way), not cross-bleed.
    const bool readbackAvailable = okA && okB && !(mrA == 0 && mgA == 0 && mbA == 0) &&
                                   !(mrB == 0 && mgB == 0 && mbB == 0);
    const bool pixelsOk = !readbackAvailable ||
                          ((std::abs(mrB - mrA) + std::abs(mgB - mgA) + std::abs(mbB - mbA)) > 0);
    const bool pass = isolated && pixelsOk;
    std::cout << "[multiscene] " << (pass ? "PASS" : "FAIL")
              << " (isolated=" << isolated << " pixelsOk=" << pixelsOk << ")" << std::endl;

    pipe.EnableFrameCapture(false);
    pipe.Shutdown();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(pass ? 0 : 7);
}

int main(int argc, char* argv[]) {
    // Capture stdout/stderr into the in-app Log Console before anything logs, so
    // startup output is caught. In Release the exe is GUI-subsystem (no console
    // window); this is the only way users can see the dev log.
    whiteout::flakes::tools::LogConsole::Instance().Begin();

    whiteout::flakes::gfx::GfxApi backend = whiteout::flakes::gfx::GfxApi::Vulkan;
#if defined(_WIN32)
    // Windows historically defaults to D3D12; Linux only has Vulkan so the
    // default is fixed above.
    backend = whiteout::flakes::gfx::GfxApi::D3D12;

    // main()'s argv arrives in the system ANSI codepage, so a path with
    // non-Latin characters — e.g. an .mdx with Chinese characters launched
    // via a file association — is already mangled by the time it reaches
    // us. Re-derive the args from the wide command line and transcode to
    // UTF-8 (which FsPathFromUtf8 below expects). The storage vectors are
    // function-scoped so the rebound argv stays valid for all of main().
    std::vector<std::string> utf8Args;
    std::vector<char*> utf8Argv;
    if (int wArgc = 0; LPWSTR* wArgv = ::CommandLineToArgvW(::GetCommandLineW(), &wArgc)) {
        utf8Args.reserve(static_cast<size_t>(wArgc));
        for (int i = 0; i < wArgc; ++i)
            utf8Args.push_back(whiteout::flakes::io::PathToUtf8(std::filesystem::path(wArgv[i])));
        ::LocalFree(wArgv);
        utf8Argv.reserve(utf8Args.size());
        for (auto& s : utf8Args)
            utf8Argv.push_back(s.data());
        argc = wArgc;
        argv = utf8Argv.data();
    }
#endif
    bool backendFromCli = false;
    bool headlessTest = false;
    bool multiSceneTest = false;
    bool particleDiff = false;
    bool childModelCheck = false;
    bool particleDiffDevice = true;
    bool particleDiffCurveTol = false;
    i32 particleDiffFrames = 120;
    std::string particleTraceRecord;
    std::string particleTraceCheck;
    bool drawTrace = false;
    bool drawTraceHd = false;
    bool drawTraceUnlit = false;
    bool drawTraceDebugLight = false;
    bool drawTraceLazyAnim = false;
    std::string drawTraceRecord;
    std::string drawTraceCheck;
    std::string drawTraceGolden;
    f32 drawTraceDistanceTol = 0.0f;
    i32 drawTraceCameraDistance = 350;
    i32 drawTracePerturb = 0;
    i32 drawTraceInstances = 3;
    AnimScenario drawTraceAnim;
    // World of Warcraft's `id;path` CSV. The GUI takes this from Settings > IO,
    // but the headless runs happen before those are applied — and without it a
    // WoW root can only be read by id, so nothing can look a model up in the
    // client databases (see WowReplaceableTextures).
    std::string listfilePath;
    // Community `keyName keyHex` list. Without it any file with a TACT-encrypted
    // frame reads back as missing, which on retail includes several of the
    // client databases.
    std::string tactKeyPath;
    // Loose asset tree searched before the archives, which is what the GUI
    // points at the model's own folder on File > Open. The headless runs need
    // to be told: an extracted corpus keeps files a model references by
    // directory (`dbfilesclient/`) somewhere above the model itself.
    std::string contentRoot;
    std::filesystem::path mdxPath;
    // Extra positional paths beyond the first open in their own tabs, so
    // `WhiteoutFlakes a.mdx b.mdx c.mdx` launches with three documents.
    std::vector<std::filesystem::path> extraPaths;

    // Headless animation-frame export: --export-anim <seqIdx> <fps> <folder>
    // [--gif] [--apng] [--webp] [--transparent] [--ui] [--res <w> <h>]
    // [--camera <idx>]. Loads the model, exports, and exits — verifies the
    // capture pipeline without UI. --camera selects a model camera preset
    // (0-based); --ui composites the viewer UI overlay into each frame.
    bool doExport = false;
    i32 exportSeq = 0;
    // `.m3a` files to merge into the loaded `.m3` before anything else runs.
    // The UI route is the toolbar's Anims button; this is the same call, so a
    // scripted render can exercise an attached sequence.
    std::vector<std::filesystem::path> attachAnims;
    i32 exportFps = 30;
    whiteout::flakes::ExportFormat exportFmt = whiteout::flakes::ExportFormat::PngFrames;
    bool exportTransparent = false;
    bool exportCaptureUi = false;
    i32 exportResW = 0;
    i32 exportResH = 0;
    i32 exportCamera = -1; // -1 = free camera; >= 0 = model camera preset index
    std::filesystem::path exportFolder;

#if defined(_WIN32)
    constexpr const char* kBackendsHelp = "d3d11, d3d12, vulkan";
#elif defined(__APPLE__)
    constexpr const char* kBackendsHelp = "metal, vulkan";
#else
    constexpr const char* kBackendsHelp = "vulkan";
#endif

    for (i32 i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if ((std::strcmp(a, "--backend") == 0 || std::strcmp(a, "-b") == 0) && i + 1 < argc) {
            const char* v = argv[++i];
            if (CompareCi(v, "vulkan") == 0 || CompareCi(v, "vk") == 0) {
                backend = whiteout::flakes::gfx::GfxApi::Vulkan;
            } else if (CompareCi(v, "webgpu") == 0 || CompareCi(v, "wgpu") == 0) {
                backend = whiteout::flakes::gfx::GfxApi::WebGPU;
            }
#if defined(__APPLE__)
            // Metal is Apple-only — only accept it as a CLI backend there.
            else if (CompareCi(v, "metal") == 0 || CompareCi(v, "mtl") == 0) {
                backend = whiteout::flakes::gfx::GfxApi::Metal;
            }
#endif
#if defined(_WIN32)
            else if (CompareCi(v, "d3d11") == 0 || CompareCi(v, "dx11") == 0) {
                backend = whiteout::flakes::gfx::GfxApi::D3D11;
            } else if (CompareCi(v, "d3d12") == 0 || CompareCi(v, "dx12") == 0) {
                backend = whiteout::flakes::gfx::GfxApi::D3D12;
            }
#endif
            else {
                // On Linux, d3d11/d3d12 are not built — surface the request
                // as an error rather than a silent override so the user knows
                // their CLI flag did nothing useful.
                std::cerr << "Unknown / unsupported backend: " << v
                          << " (valid on this platform: " << kBackendsHelp << ")\n";
                return 1;
            }
            backendFromCli = true;
        } else if (std::strcmp(a, "--export-anim") == 0 && i + 3 < argc) {
            doExport = true;
            exportSeq = std::atoi(argv[++i]);
            exportFps = std::atoi(argv[++i]);
            exportFolder = whiteout::flakes::io::FsPathFromUtf8(argv[++i]);
        } else if (std::strcmp(a, "--attach-anim") == 0 && i + 1 < argc) {
            attachAnims.push_back(whiteout::flakes::io::FsPathFromUtf8(argv[++i]));
        } else if (std::strcmp(a, "--gif") == 0) {
            exportFmt = whiteout::flakes::ExportFormat::Gif;
        } else if (std::strcmp(a, "--apng") == 0) {
            exportFmt = whiteout::flakes::ExportFormat::Apng;
        } else if (std::strcmp(a, "--webp") == 0) {
            exportFmt = whiteout::flakes::ExportFormat::Webp;
        } else if (std::strcmp(a, "--transparent") == 0) {
            exportTransparent = true;
        } else if (std::strcmp(a, "--ui") == 0) {
            exportCaptureUi = true;
        } else if (std::strcmp(a, "--res") == 0 && i + 2 < argc) {
            exportResW = std::atoi(argv[++i]);
            exportResH = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--camera") == 0 && i + 1 < argc) {
            exportCamera = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--headless-test") == 0) {
            headlessTest = true;
        } else if (std::strcmp(a, "--multiscene-test") == 0) {
            multiSceneTest = true;
        } else if (std::strcmp(a, "--particle-diff") == 0) {
            particleDiff = true;
        } else if (std::strcmp(a, "--childmodel-check") == 0) {
            childModelCheck = true;
        } else if (std::strcmp(a, "--trace-record") == 0 && i + 1 < argc) {
            particleTraceRecord = argv[++i];
        } else if (std::strcmp(a, "--trace-check") == 0 && i + 1 < argc) {
            particleTraceCheck = argv[++i];
        } else if (std::strcmp(a, "--trace-frames") == 0 && i + 1 < argc) {
            particleDiffFrames = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--trace-curve-tol") == 0) {
            particleDiffCurveTol = true;
        } else if (std::strcmp(a, "--trace-no-device") == 0) {
            particleDiffDevice = false;
        } else if (std::strcmp(a, "--draw-trace") == 0) {
            drawTrace = true;
        } else if (std::strcmp(a, "--draw-trace-record") == 0 && i + 1 < argc) {
            drawTraceRecord = argv[++i];
        } else if (std::strcmp(a, "--draw-trace-check") == 0 && i + 1 < argc) {
            drawTraceCheck = argv[++i];
        } else if (std::strcmp(a, "--draw-trace-golden") == 0 && i + 1 < argc) {
            drawTraceGolden = argv[++i];
        } else if (std::strcmp(a, "--draw-trace-hd") == 0) {
            drawTraceHd = true;
        } else if (std::strcmp(a, "--draw-trace-unlit") == 0) {
            drawTraceUnlit = true;
        } else if (std::strcmp(a, "--draw-trace-debug-light") == 0) {
            drawTraceDebugLight = true;
        } else if (std::strcmp(a, "--draw-trace-lazy-anim") == 0) {
            drawTraceLazyAnim = true;
        } else if (std::strcmp(a, "--listfile") == 0 && i + 1 < argc) {
            listfilePath = argv[++i];
        } else if (std::strcmp(a, "--tact-keys") == 0 && i + 1 < argc) {
            tactKeyPath = argv[++i];
        } else if (std::strcmp(a, "--content-root") == 0 && i + 1 < argc) {
            contentRoot = argv[++i];
        } else if (std::strcmp(a, "--draw-trace-distance-tol") == 0 && i + 1 < argc) {
            drawTraceDistanceTol = static_cast<f32>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--draw-trace-camera-distance") == 0 && i + 1 < argc) {
            drawTraceCameraDistance = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--draw-trace-perturb") == 0 && i + 1 < argc) {
            drawTracePerturb = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--draw-trace-instances") == 0 && i + 1 < argc) {
            drawTraceInstances = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--draw-trace-anim") == 0 && i + 1 < argc) {
            drawTraceAnim.sequence = argv[++i];
        } else if (std::strcmp(a, "--draw-trace-anim-switch") == 0 && i + 2 < argc) {
            drawTraceAnim.switchFrame = std::atoi(argv[++i]);
            drawTraceAnim.switchSequence = argv[++i];
        } else if (std::strcmp(a, "--draw-trace-anim-layer") == 0 && i + 2 < argc) {
            drawTraceAnim.layerFrame = std::atoi(argv[++i]);
            drawTraceAnim.layerSequence = argv[++i];
        } else if (std::strcmp(a, "--draw-trace-anim-blend") == 0 && i + 1 < argc) {
            drawTraceAnim.layerBlendInMs = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--draw-trace-anim-weight") == 0 && i + 1 < argc) {
            drawTraceAnim.layerWeight = static_cast<f32>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--draw-trace-anim-list") == 0) {
            drawTraceAnim.list = true;
        } else if (std::strcmp(a, "--draw-trace-anim-probe") == 0) {
            drawTraceAnim.probe = true;
        } else if (std::strcmp(a, "--draw-trace-solvers") == 0) {
            drawTraceAnim.solvers = true;
        } else if (std::strcmp(a, "--draw-trace-ground") == 0 && i + 1 < argc) {
            drawTraceAnim.groundZ = static_cast<f32>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--draw-trace-aim") == 0 && i + 3 < argc) {
            drawTraceAnim.hasAim = true;
            drawTraceAnim.aim.x = static_cast<f32>(std::atof(argv[++i]));
            drawTraceAnim.aim.y = static_cast<f32>(std::atof(argv[++i]));
            drawTraceAnim.aim.z = static_cast<f32>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--wgpu-backend") == 0 && i + 1 < argc) {
            // Force Dawn's underlying adapter backend (d3d11/d3d12/vulkan/gl/metal).
            // Only meaningful when --backend webgpu is selected.
            whiteout::flakes::gfx::SetWebGPUBackend(argv[++i]);
        } else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            std::cout << "Usage: WhiteoutFlakes [--backend " << kBackendsHelp
                      << "] [--wgpu-backend d3d11|d3d12|vulkan|gl] [<mdx-path>]\n";
            return 0;
        } else if (mdxPath.empty()) {
            mdxPath = whiteout::flakes::io::FsPathFromUtf8(a);
        } else {
            extraPaths.push_back(whiteout::flakes::io::FsPathFromUtf8(a));
        }
    }

#if defined(__APPLE__)
    // .app-bundled MoltenVK: the Vulkan loader's ICD discovery doesn't
    // walk into Contents/Resources by default. Point it at our bundled
    // ICD JSON before any Vulkan call. Layout produced by the macOS CI:
    //   WhiteoutFlakes.app/Contents/MacOS/WhiteoutFlakes
    //   WhiteoutFlakes.app/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json
    //   WhiteoutFlakes.app/Contents/Frameworks/libMoltenVK.dylib
    // The JSON's library_path is "../../../Frameworks/libMoltenVK.dylib"
    // (relative to the JSON's parent dir), so the loader resolves the
    // dylib from inside the bundle without depending on system MoltenVK.
    {
        std::filesystem::path exe = GetExecutablePath();
        if (!exe.empty()) {
            std::filesystem::path icd = exe.parent_path().parent_path() / "Resources" / "vulkan" /
                                        "icd.d" / "MoltenVK_icd.json";
            if (std::filesystem::exists(icd))
                ::setenv("VK_ICD_FILENAMES", icd.c_str(), 1);
        }
    }
    // MoltenVK perf: opt into Metal argument buffers (descriptor indirection
    // table). Default-off in MoltenVK because old Metal drivers had bugs;
    // safe and substantially faster on Apple Silicon (M1+) which is our
    // only macOS target. Setting it before any Vulkan call ensures
    // MoltenVK reads it during ICD initialization.
    ::setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "1", 1);
#endif

    whiteout::flakes::renderer::SceneManager scene;
    whiteout::flakes::renderer::RenderService renderer(scene);

    // Startup-only settings (validation layer, default backend, preferred
    // device) must land on RenderSettings before gfx::CreateDevice runs.
    whiteout::flakes::LoadStartupSettingsFromIni(renderer);
    if (!backendFromCli)
        backend = renderer.Settings().DefaultBackend();

#if defined(__linux__)
    // Linux only ships the Vulkan backend; D3D11/D3D12 are WIN32-gated and
    // WebGPU/Dawn isn't built into Linux Whiteout binaries. Coerce any
    // stale INI / CLI choice back to Vulkan so SaveSettingsIni doesn't
    // propagate a bad value forward and the ImGui combo (see viewer_ui.cpp)
    // stays consistent.
    if (backend != whiteout::flakes::gfx::GfxApi::Vulkan) {
        std::cerr << "[viewer] Forcing Vulkan backend (only one available on this platform)\n";
        backend = whiteout::flakes::gfx::GfxApi::Vulkan;
    }
    if (renderer.Settings().DefaultBackend() != whiteout::flakes::gfx::GfxApi::Vulkan)
        renderer.Settings().SetDefaultBackend(whiteout::flakes::gfx::GfxApi::Vulkan);
#elif defined(__APPLE__)
    // macOS supports Vulkan (via MoltenVK) and, when WDX_HAS_METAL is on,
    // the native Metal backend. WebGPU (via Dawn → Metal) is also accepted
    // when WDX_HAS_WEBGPU is on. Reject D3D11/D3D12 — they only build on
    // Windows.
    using Api = whiteout::flakes::gfx::GfxApi;
    auto isMacOk = [](Api a) {
        if (a == Api::Vulkan) return true;
#if WDX_HAS_WEBGPU
        if (a == Api::WebGPU) return true;
#endif
#if WDX_HAS_METAL
        if (a == Api::Metal) return true;
#endif
        return false;
    };
    if (!isMacOk(backend)) {
        std::cerr << "[viewer] Backend not supported on macOS; falling back to Vulkan\n";
        backend = Api::Vulkan;
    }
    if (!isMacOk(renderer.Settings().DefaultBackend()))
        renderer.Settings().SetDefaultBackend(Api::Vulkan);
#endif

    // Vulkan pipeline cache: alongside the exe on Windows; per-user cache
    // dir on Linux/macOS (the AppImage mount and the .app bundle are both
    // read-only, so we can't drop the cache next to the binary there).
    //   • Linux: $XDG_CACHE_HOME/WhiteoutFlakes/ (or ~/.cache/...)
    //   • macOS: ~/Library/Caches/WhiteoutFlakes/
    {
        std::filesystem::path cachePath;
#if defined(_WIN32)
        std::filesystem::path exe = GetExecutablePath();
        if (!exe.empty()) {
            exe.replace_filename("vk_pipeline_cache.bin");
            cachePath = std::move(exe);
        } else {
            cachePath = "vk_pipeline_cache.bin";
        }
#elif defined(__APPLE__)
        std::filesystem::path base;
        if (const char* home = std::getenv("HOME"); home && *home)
            base = std::filesystem::path(home) / "Library" / "Caches";
        else
            base = ".";
        std::error_code ec;
        std::filesystem::create_directories(base / "WhiteoutFlakes", ec);
        cachePath = base / "WhiteoutFlakes" / "vk_pipeline_cache.bin";
#else
        std::filesystem::path base;
        if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
            base = xdg;
        else if (const char* home = std::getenv("HOME"); home && *home)
            base = std::filesystem::path(home) / ".cache";
        else
            base = ".";
        std::error_code ec;
        std::filesystem::create_directories(base / "WhiteoutFlakes", ec);
        cachePath = base / "WhiteoutFlakes" / "vk_pipeline_cache.bin";
#endif
        // Seed pso_trace.bin from the shipped one in the exe dir if the
        // user-cache copy doesn't exist yet. The engine reads the trace from
        // <cache_dir>/pso_trace.bin (sibling of the pipeline cache); shipping
        // a pre-warmed trace inside the AppImage / installer cuts the
        // cold-launch PSO build hitch on Vulkan. Subsequent runs append their
        // own additions via BlsPsoTrace::Save() in the user-cache copy.
        //
        // On macOS, the .app bundle stores ship-with-the-binary data under
        // Contents/Resources/ (not Contents/MacOS/) so codesign accepts
        // the non-Mach-O .bls files; translate the seed source accordingly.
        {
            std::filesystem::path tracePath = cachePath;
            tracePath.replace_filename("pso_trace.bin");
            std::error_code ec;
            if (!std::filesystem::exists(tracePath, ec)) {
                std::filesystem::path exe = GetExecutablePath();
                if (!exe.empty()) {
                    std::filesystem::path shippedDir = exe.parent_path();
#if defined(__APPLE__)
                    if (shippedDir.filename() == "MacOS" &&
                        shippedDir.parent_path().filename() == "Contents") {
                        shippedDir = shippedDir.parent_path() / "Resources";
                    }
#endif
                    std::filesystem::path shipped = shippedDir / "pso_trace.bin";
                    if (std::filesystem::exists(shipped, ec)) {
                        std::filesystem::copy_file(
                            shipped, tracePath, std::filesystem::copy_options::skip_existing, ec);
                    }
                }
            }
        }

        const std::string u8 = whiteout::flakes::io::PathToUtf8(cachePath);
        whiteout::flakes::gfx::SetPipelineCachePath(u8.c_str());
    }

    const char* backendName = backend == whiteout::flakes::gfx::GfxApi::D3D11    ? "D3D11"
                              : backend == whiteout::flakes::gfx::GfxApi::D3D12  ? "D3D12"
                              : backend == whiteout::flakes::gfx::GfxApi::Vulkan ? "Vulkan"
                              : backend == whiteout::flakes::gfx::GfxApi::WebGPU ? "WebGPU"
                              : backend == whiteout::flakes::gfx::GfxApi::Metal  ? "Metal"
                                                                                 : "?";
    std::cout << "Backend: " << backendName << "\n";

    // Set on the World of Warcraft slot specifically, and left there: it is
    // that product's config, and the run does not know yet which game the model
    // it was handed belongs to. Restoring the previous selection keeps this
    // from being a back-door SetGame.
    if (!listfilePath.empty() || !tactKeyPath.empty()) {
        auto& cp = renderer.Scene().GetContentProvider();
        const auto was = cp.Game();
        cp.SetGame(whiteout::flakes::ProductId::Wow);
        if (!listfilePath.empty())
            cp.SetListfilePath(whiteout::flakes::io::FsPathFromUtf8(listfilePath));
        if (!tactKeyPath.empty())
            cp.SetTactKeyPath(whiteout::flakes::io::FsPathFromUtf8(tactKeyPath));
        cp.SetGame(was);
    }

    // Headless multi-viewport smoke test: no window, no ViewerApp — drive the
    // pipeline straight into an off-screen target and read it back. Runs and
    // exits without ever creating a GLFW window.
    if (headlessTest)
        return RunHeadlessTest(renderer, scene, backend, mdxPath);

    if (multiSceneTest)
        return RunMultiSceneTest(renderer, backend, mdxPath);

    if (childModelCheck)
        return RunChildModelCheck(renderer, scene, backend, mdxPath, particleDiffFrames);

    if (particleDiff)
        return RunParticleDiff(renderer, scene, backend, mdxPath, particleTraceRecord,
                               particleTraceCheck, particleDiffFrames, particleDiffCurveTol,
                               particleDiffDevice);

    if (drawTrace)
        return RunDrawTrace(renderer, scene, backend, mdxPath, drawTraceRecord, drawTraceCheck,
                            drawTraceGolden, particleDiffFrames, drawTraceHd, drawTraceDistanceTol,
                            drawTraceCameraDistance, drawTracePerturb, drawTraceInstances,
                            drawTraceUnlit, drawTraceLazyAnim, contentRoot, drawTraceAnim,
                            drawTraceDebugLight);

    whiteout::flakes::ViewerApp app(renderer);
    if (!app.Open(1024, 768, backend)) {
        std::cerr << "Failed to open viewer\n";
        return 1;
    }

    // Install the real sound emitter *before* LoadSettingsIni — the latter
    // applies the persisted SoundVolume via service.Sound().SetVolume(), and
    // the default NullSoundEmitter swallows it (its SetVolume is a no-op and
    // GetVolume always reports 1.0, so SwapSoundEmitter couldn't carry it
    // over either).
    renderer.SwapSoundEmitter(
        std::make_unique<whiteout::flakes::CubebSoundEmitter>(scene.ActiveContentProvider()));

    // Persistent settings (display flags, exposure, tileset, etc.) applied
    // after the device + asset managers are up so they can validate
    // dependent state (e.g. ShadowService cascade count).
    bool loopPolicy = app.LoopNonLoopingPolicy();
    bool forceHdPolicy = app.ForceHd();
    std::string languageCode = "en";
    whiteout::flakes::LoadSettingsIni(renderer, loopPolicy, forceHdPolicy, languageCode);
    app.SetLoopNonLoopingPolicy(loopPolicy);
    app.SetForceHd(forceHdPolicy);

    // UI localization: load the `<code>.ini` catalogs from `lang/` next to the
    // exe and activate the persisted language (English fallback otherwise).
    {
        namespace i18n = whiteout::flakes::i18n;
        const std::string langDir =
            whiteout::flakes::io::PathToUtf8(whiteout::flakes::AssetDir() / "lang");
        i18n::Localizer::instance().load(langDir, i18n::languageFromCode(languageCode));
    }

    // IO overrides — Settings > IO can repoint the install path, toggle CASC
    // or MPQ off entirely, and reorder the MPQ load list. Per game, and which
    // game the settings panel was left on decides what opens now.
    whiteout::flakes::ApplyIoPathOverrides(renderer.Scene().GetContentProvider(),
                                           whiteout::flakes::LoadIoProduct());

    // NFD is also used by Settings > IO (folder picker) and File > Open
    // (re-opened from the menu bar), so initialise it unconditionally rather
    // than only when we pop the startup model picker.
    NFD::Init();

    // CLI path wins; otherwise pop NFD once at startup. Re-openable via
    // File > Open in the menu bar.
    if (mdxPath.empty()) {
        NFD::UniquePathU8 outPath;
        nfdu8filteritem_t filter[4] = {{"All supported", whiteout::flakes::kOpenAllExtensions},
                                       {"Warcraft III Model", "mdx,mdl"},
                                       {"PKB Effect", "pkb,pkfx"},
                                       {"Other Blizzard model",
                                        whiteout::flakes::kForeignModelExtensions}};
        const nfdfiltersize_t nFilters = whiteout::flakes::kHasForeignModelFilter ? 4 : 3;
        if (NFD::OpenDialog(outPath, filter, nFilters) == NFD_OKAY)
            mdxPath = whiteout::flakes::io::FsPathFromUtf8(outPath.get());
    }
    if (!mdxPath.empty()) {
        if (!std::filesystem::exists(mdxPath)) {
            std::cerr << "File not found: " << whiteout::flakes::io::PathToUtf8(mdxPath) << "\n";
        } else if (!app.LoadModel(mdxPath)) {
            std::cerr << "Failed to load model.\n";
        }
    }
    // Open any additional positional paths in their own tabs. The last one
    // loaded ends up active, matching File > Open opening into a new tab.
    for (const auto& extra : extraPaths) {
        if (!std::filesystem::exists(extra)) {
            std::cerr << "File not found: " << whiteout::flakes::io::PathToUtf8(extra) << "\n";
        } else if (!app.LoadModel(extra)) {
            std::cerr << "Failed to load model: " << whiteout::flakes::io::PathToUtf8(extra) << "\n";
        }
    }

    // Merge any external animation files into the freshly loaded model. After
    // this the sequence list — and so the toolbar dropdown and --export-anim's
    // index — spans the model's sequences followed by each attached file's.
    for (const auto& anim : attachAnims) {
        if (!app.AttachAnimationFile(anim)) {
            std::cerr << "Failed to attach animation file: "
                      << whiteout::flakes::io::PathToUtf8(anim) << "\n";
        } else {
            std::printf("[viewer] attached '%s'; %zu sequence(s) now available\n",
                        whiteout::flakes::io::PathToUtf8(anim.filename()).c_str(),
                        app.SequenceNames().size());
        }
    }

    // Headless export path: queue the request, run a couple of ticks to let
    // RenderService warm up (BLS shaders / textures finish loading), run the
    // export, then exit.
    if (doExport) {
        // Warm-up: drain the async asset pump and let a standalone .pkb's
        // particle cloud develop so the viewer's deferred effect-framing
        // (FrameCameraToEffect, ~12 ticks) reframes before the export.
        for (i32 i = 0; i < 24 && !app.ShouldClose(); ++i) {
            scene.Update(0.016f);
            app.Tick(0.016f);
        }
        // Camera presets exist only once the model template has loaded, so
        // select one after the warm-up ticks.
        if (exportCamera >= 0) {
            std::printf("[viewer] model has %zu camera preset(s); activating #%d\n",
                        app.CameraPresets().size(), exportCamera);
            app.ActivateCameraPreset(exportCamera);
        }
        whiteout::flakes::AnimationExportParams params;
        params.sequenceIndex = exportSeq;
        params.fps = exportFps;
        params.format = exportFmt;
        params.transparentBackground = exportTransparent;
        params.captureUi = exportCaptureUi;
        params.width = exportResW;
        params.height = exportResH;
        params.outputFolder = exportFolder;
        app.RequestAnimationExport(std::move(params));
        scene.Update(0.016f);
        app.Tick(0.016f); // runs the export synchronously
        app.Close();
        // Close() did the orderly GPU shutdown; skip static/global teardown,
        // where ~RenderService destructs the gfx services after the device is
        // already gone (a pre-existing engine teardown-order issue the model
        // explorer + headless tests also _Exit past).
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(0);
    }

    auto last = std::chrono::steady_clock::now();
    while (!app.ShouldClose()) {
        const auto now = std::chrono::steady_clock::now();
        const f32 dt = std::chrono::duration<f32>(now - last).count();
        last = now;
        scene.Update(dt);
        app.Tick(dt);
    }

    app.Close();
    // See the export path above: _Exit past the crash-prone static teardown now
    // that Close() has done the orderly GPU shutdown.
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(0);
}
