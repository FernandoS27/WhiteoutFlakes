#include "cubeb_sound_emitter.h"
#include "gfx/gfx.h"
#include "renderer/animation/clip_playlist.h"
#include "renderer/debug/draw_trace.h"
#include "renderer/dnc/dnc_service.h"
#include "renderer/frame_ticker.h"
#include "renderer/model/corn_effect_source.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_loader.h"
#include "renderer/particle/d3_emitter.h"
#include "renderer/particle/particle_service.h"
#include "renderer/particle/particle_trace.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#if WDX_ENABLE_M3
#include "io/m3/m3_model_adapter.h"
#endif
#if WDX_ENABLE_D3
#include "io/d3/d3_model_adapter.h"
#include "renderer/profiles/diablo3/d3_surface_table.h"
#endif
#include "export_ini.h"
#include "io/wem/wem_profiles.h"
#include "localization.h"
#include "log_console.h"
#include "settings_ini.h"
#include "viewer_app.h"
#include "whiteout/flakes/gfx_types.h"
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <nfd.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
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
                           bool useDevice, const std::string& contentRoot, bool dump) {
    namespace wf = whiteout::flakes;
    namespace part = wf::renderer::particle;

    if (mdxPath.empty()) {
        std::cerr << "[ptrace] --particle-diff needs a model path" << std::endl;
        return 2;
    }
    if (recordPath.empty() && checkPath.empty() && !dump) {
        std::cerr << "[ptrace] pass --trace-record <file>, --trace-check <file> or --trace-dump"
                  << std::endl;
        return 2;
    }
    if (useDevice && !renderer.Pipeline().InitDevice(backend)) {
        std::cerr << "[ptrace] InitDevice failed" << std::endl;
        return 3;
    }

    // Same reason --draw-trace takes one: a Diablo III `.acr` names its content
    // by SNO, and the root that resolves those is above the file's own folder.
    scene.SetPE1BasePath(contentRoot.empty() ? mdxPath.parent_path()
                                             : wf::io::FsPathFromUtf8(contentRoot));
    auto* hero = renderer.Loader().SpawnUnit(wf::io::PathToUtf8(mdxPath));
    if (!hero) {
        std::cerr << "[ptrace] SpawnUnit failed: " << wf::io::PathToUtf8(mdxPath) << std::endl;
        return 4;
    }

    if (dump) {
        const auto& b = hero->bounds;
        std::printf("[ptrace] model bounds valid=%d min=(%.2f %.2f %.2f) max=(%.2f %.2f %.2f)"
                    " scale=%.4f\n",
                    b.valid ? 1 : 0, b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z,
                    hero->worldScale);
    }
    const i32 emitters = renderer.Particles().EmitterCount();
    std::cout << "[ptrace] " << mdxPath.filename().string() << ": " << emitters << " emitter(s), "
              << traceFrames << " frames" << std::endl;
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

    // What each emitter actually put in the vertex stream, on the last frame.
    // Enough to tell "nothing emitted" from "emitted at the origin" from
    // "emitted the right shape at the wrong scale" without a GPU or a baseline.
    if (dump && !trace.frames.empty()) {
        // Where the host put each emitter, straight off the live service —
        // the one number that separates "the sim is wrong" from "the emitter
        // is in the wrong place", and the trace schema carries no equivalent.
        renderer.Particles().ForEachEmitter([](const part::EmitterKey& k, const part::Emitter2& e) {
            const wf::Vector3f& w = e.WorldPosition();
            std::printf("  em %2d out=%u at=(%8.2f %8.2f %8.2f) visible=%d\n", k.id,
                        unsigned(k.output), w.x, w.y, w.z, e.Visible() ? 1 : 0);
#if WDX_ENABLE_D3
            const auto* d3 = dynamic_cast<const part::d3::Emitter*>(&e);
            if (!d3)
                return;
            const auto& m = d3->D3Desc().d3mat;
            // Where the emitter was PUT, which the world position above does
            // not separate: a hardpoint that resolved to the wrong bone and
            // one whose frame is wrong read the same from outside.
            const wf::Matrix44f& hp = d3->AttachOffset();
            const wf::Matrix44f& m2w = d3->ModelToWorld();
            std::printf("       place: bone=%d hp=(%.2f %.2f %.2f) m2w=(%.2f %.2f %.2f)"
                        " unit=%.4f\n",
                        d3->AttachBone(), hp.data[3][0], hp.data[3][1], hp.data[3][2],
                        m2w.data[3][0], m2w.data[3][1], m2w.data[3][2], d3->UnitScale());
            // The erosion tail's exponent is `10 * ch6` and ch6 is per
            // particle, so both halves belong in the same line.
            f32 a6lo = 1.0f, a6hi = 1.0f;
            d3->D3Desc().Channel(part::d3::kChAlpha).ScalarRange(a6lo, a6hi);
            std::printf("       sno=%d caps=0x%04X type=%d shape=%d mat: pass=%d %s blend=(%u,%u) "
                        "vcol=(%d%d %d%d) aTest=%.3f erosion=%d ch6=%.2f..%.2f layers=%u\n",
                        d3->D3Desc().snoId, d3->D3Desc().caps, d3->D3Desc().systemType,
                        int(d3->D3Desc().shape), m.passResolved ? 1 : 0, m.effectFile.c_str(),
                        m.blendSrc, m.blendDst, m.colorVcolFirst ? 1 : 0, m.colorVcolLast ? 1 : 0,
                        m.alphaVcolFirst ? 1 : 0, m.alphaVcolLast ? 1 : 0, m.alphaTest,
                        m.erosion ? 1 : 0, a6lo, a6hi, m.layerCount);
            for (unsigned L = 0; L < m.layerCount; ++L) {
                const auto& lay = m.layers[L];
                std::printf("         L%u type=%2d sno=%d texId=%d wrap=%u op=%d/%d"
                            " gain=%.0f/%.0f clamp=%d%d uv=%d",
                            L, lay.rawType, lay.textureSno, lay.textureId, lay.wrapFlags,
                            int(lay.colorOp), int(lay.alphaOp), lay.colorGain, lay.alphaGain,
                            lay.colorClamp ? 1 : 0, lay.alphaClamp ? 1 : 0, int(lay.uv.mode));
                // The sheet, for EVERY layer and not only the mode-3 ones:
                // the quad's base rectangle and its aspect come off stage
                // 0's frame table whatever uv mode stage 0 carries, so a
                // type-1 layer's sheet is worth seeing even at mode 0.
                if (lay.atlas) {
                    const unsigned n = lay.atlas ? unsigned(lay.atlas->frames.size()) : 0u;
                    std::printf(" atlas: frames=%u tile=%.4f,%.4f px=%ux%u rate=%.1f(+%.1f)"
                                " start=%d..%d %s",
                                n, n ? lay.atlas->TileSize().x : 0.0f,
                                n ? lay.atlas->TileSize().y : 0.0f, n ? lay.atlas->width : 0u,
                                n ? lay.atlas->height : 0u, lay.atlasRate, lay.atlasRateJitter,
                                lay.atlasFrameBase, lay.atlasFrameBase + lay.atlasFrameRange,
                                lay.uv.mode == wf::io::D3UvMode::Anim2D
                                    ? "flip"
                                    : (lay.rawType == 1 ? "baseRect" : "-"));
                    for (unsigned k = 0; k < n && k < 3; ++k)
                        std::printf(" [%.3f,%.3f..%.3f,%.3f]", lay.atlas->frames[k].x,
                                    lay.atlas->frames[k].y, lay.atlas->frames[k].z,
                                    lay.atlas->frames[k].w);
                }
                std::printf("\n");
            }
#endif
        });
        const auto& f = trace.frames.back();
        std::printf("[ptrace] frame %d: %zu emitter(s)\n", f.frame, f.emitters.size());
        for (const auto& e : f.emitters) {
            const wf::Vector3f c{(e.boundsMin.x + e.boundsMax.x) * 0.5f,
                                 (e.boundsMin.y + e.boundsMax.y) * 0.5f,
                                 (e.boundsMin.z + e.boundsMax.z) * 0.5f};
            std::printf("  em %2d out=%u alive=%3zu verts=%5d centre=(%8.2f %8.2f %8.2f) "
                        "extent=(%7.2f %7.2f %7.2f) rgba=(%.3f %.3f %.3f %.3f)\n",
                        e.emitterId, unsigned(e.output), e.particles.size(), e.vertexCount, c.x,
                        c.y, c.z, e.boundsMax.x - e.boundsMin.x, e.boundsMax.y - e.boundsMin.y,
                        e.boundsMax.z - e.boundsMin.z, e.meanColor.x, e.meanColor.y, e.meanColor.z,
                        e.meanColor.w);
            for (std::size_t k = 0; k < e.particles.size() && k < 4; ++k) {
                const auto& pt = e.particles[k];
                std::printf("         p%zu pos=(%8.2f %8.2f %8.2f) vel=(%7.2f %7.2f %7.2f) "
                            "age=%.3f\n",
                            k, pt.position.x, pt.position.y, pt.position.z, pt.velocity.x,
                            pt.velocity.y, pt.velocity.z, pt.age);
            }
        }
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
    // One sub-track container of the layered sequence, or -1 for all of them.
    // The Marine's shield is `Cover` sub-track 0; playing the whole of `Cover`
    // also runs `Cover_full`, so only this can isolate a prop.
    i32 layerSubtrack = -1;
    // Silence the model's global loops. On by default because the engine plays
    // them, and off is how a baseline pins one sequence on its own.
    bool noGlobals = false;
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
    // Collapse a Diablo III rigid rig, after the settle.
    //
    // D3 builds this on a gameplay event, not at load (d3_physics.h), so there
    // is nothing to see until something arms it — and "nothing to see" is
    // exactly what a rig that failed to build looks like too. This is the host
    // half of that switch, and the only way a capture can tell the two apart.
    bool ragdoll = false;

    // Dress a Diablo III player character by item name before the settle:
    // (EVisualSlot ordinal, GameBalance item name) pairs, resolved through
    // the item registry against the content root's GameBalance snapshot.
    // What makes the dressed-character scenarios recordable at all.
    std::vector<std::pair<i32, std::string>> d3Equip;
    // (EVisualSlot ordinal, dye row) pairs applied after the equips.
    std::vector<std::pair<i32, i32>> d3Dyes;
    bool d3Sheathed = false;

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
        return !sequence.empty() || switchFrame >= 0 || layerFrame >= 0 || noGlobals || list ||
               probe || solvers || ragdoll || !d3Equip.empty();
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
static int RunDrawTrace(
    whiteout::flakes::renderer::RenderService& renderer,
    whiteout::flakes::renderer::SceneManager& scene, whiteout::flakes::gfx::GfxApi backend,
    const std::filesystem::path& mdxPath, const std::string& recordPath,
    const std::string& checkPath, const std::string& goldenPath, i32 frames, bool hdMode,
    f32 distanceTol, i32 cameraDistance, i32 perturbSeed, i32 instances, bool unlitOddGeosets,
    bool lazyAnim, bool allowLateAssets, const std::string& contentRoot, const AnimScenario& anim,
    const std::vector<std::filesystem::path>& attachAnims, bool debugLight = false,
    bool noRefraction = false, bool refractionMask = false, bool noMultiTex = false,
    whiteout::flakes::ProductId traceGame = whiteout::flakes::ProductId::Neutral,
    bool noDistortion = false, bool distortionBuffer = false,
    whiteout::models::wem::ProfileId wemProfile = whiteout::models::wem::ProfileId::Count) {
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

    // After InitDevice, never before: the product selection repoints the
    // content root, and the engine's own `.bls` shaders are read through the
    // same provider — selecting a game first sends every shader lookup into a
    // game install and leaves the device with nothing to draw with.
    if (traceGame != wf::ProductId::Neutral)
        scene.GetContentProvider().SetGame(traceGame);

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

    // The A/B arm for WoW's refraction particles. With the pass off, a
    // refraction emitter draws nothing at all — so a golden recorded both ways
    // is the only thing that can say the pass reached pixels, the trace being
    // identical either way (the emitters are recorded before the service runs).
    settings.SetRefractionEnabled(!noRefraction);
    settings.SetRefractionDebugMask(refractionMask);
    // Diablo III's distortion. Off is not "the same picture, cheaper": the
    // surfaces whose only pass is phase 3 have no scene draw either, so this
    // arm is the A/B that shows what the buffer is contributing.
    settings.SetD3DistortionEnabled(!noDistortion);
    settings.SetD3DistortionDebugBuffer(distortionBuffer);

    // The A/B arm for WoW's multi-texture particles. With the combiner off the
    // emitter still draws — its first layer only, at half the brightness — so
    // unlike refraction BOTH arms produce an image, and it is the difference
    // between them that says the other two layers reached pixels.
    settings.SetMultiTexParticlesEnabled(!noMultiTex);

    // The -Sc2Mat -DebugLight sub-arm: one scripted point light for the M3
    // DeferredLights pass, identical for every model. The values live HERE,
    // not in the script, so a baseline never depends on shell quoting.
    // Renderer units (post-WorldScale). Placed well OFF the body: an SC2
    // infantry model is ~400 units tall at WorldScale 100, and a light inside
    // the silhouette faces away from every visible pixel — atten and N·L
    // never coincide and the pass proves nothing.
    if (debugLight) {
        settings.SetDebugPointLight(true, {250.0f, -250.0f, 320.0f}, {2.0f, 1.8f, 1.4f}, 900.0f);
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
        // A `.wem` is spawned through the WEM entry point so `--wem-profile`
        // reaches it: SpawnUnit's own sniff would open it at the document's
        // default, which is the right answer only when nobody said otherwise.
        auto* a = wf::io::LooksLikeWemPath(mdxPath)
                      ? renderer.Loader().SpawnWem(
                            wf::ContentRef::FromPath(wf::io::PathToUtf8(mdxPath)), wemProfile)
                      : renderer.Loader().SpawnUnit(wf::io::PathToUtf8(mdxPath));
        if (!a)
            break;
        if (!hero)
            hero = a;
        spawned.push_back(a);
        a->worldTransform = wf::Matrix44f::translation({static_cast<f32>(n) * 120.0f, 0.0f, 0.0f});
    }
    if (!hero) {
        std::cerr << "[dtrace] SpawnUnit failed: " << wf::io::PathToUtf8(mdxPath) << std::endl;
        return 4;
    }
#if WDX_ENABLE_D3
    // ---- D3 outfit scenario: equip by item name, before the settle so the
    // capture never sees the undressed frame. Registry and actors come from
    // the content root's corpus tree (GameBalance/ + Actor/), or from the
    // provider when one can answer.
    if (!anim.d3Equip.empty()) {
        auto adapter = std::dynamic_pointer_cast<wf::io::D3ModelAdapter>(hero->animation.Source());
        if (!adapter) {
            std::cerr << "[dtrace] --d3-equip needs a D3 model" << std::endl;
        } else {
            auto& items = renderer.Loader().D3Items();
            if (!contentRoot.empty()) {
                const auto root = wf::io::FsPathFromUtf8(contentRoot);
                items.SetFallbackDirectory(root / "GameBalance");
                items.SetFallbackActorDirectory(root / "Actor");
                items.SetFallbackStringListDirectory(root / "StringList");
            }
            items.EnsureBuilt(scene.ActiveContentProvider());
            auto& chars = renderer.Loader().D3Characters();
            if (const auto body = wf::io::d3n::playerFromAppearanceStem(mdxPath.stem().string()))
                chars.SetOutfitBody(*adapter, body->first, body->second);
            chars.SetOutfitSheathed(*adapter, anim.d3Sheathed);
            for (const auto& [slot, name] : anim.d3Equip) {
                const wf::io::D3ItemRecord* rec = items.FindByName(name);
                std::shared_ptr<const wf::io::d3n::Actor> itemActor;
                if (rec && rec->snoActor > 0)
                    itemActor = renderer.Loader().D3Cache().Actor(rec->snoActor);
                const bool ok = rec && chars.SetOutfitItem(
                                           *adapter, static_cast<wf::io::d3n::EVisualSlot>(slot),
                                           rec, itemActor);
                std::cout << "[dtrace] scenario: equip slot " << slot << " '" << name << "' "
                          << (ok ? (itemActor ? "ok" : "ok (no actor)") : "UNKNOWN ITEM");
                if (slot == 0 && itemActor) {
                    // The hair cutaway a helm asks for (tag 0x10404) — the
                    // other half of "why is his beard poking through".
                    std::cout << " hair="
                              << wf::io::d3n::tagMapValue(itemActor->arTagMap,
                                                          wf::io::d3n::kTagItemHairStyle)
                                     .value_or(0);
                }
                std::cout << std::endl;
            }
            // What the outfit resolved to — the child actor and hardpoint per
            // attachment slot. This is where "equipped ok but nothing on the
            // model" becomes diagnosable from a log.
            for (const auto& att : chars.OutfitAttachments(*adapter)) {
                std::cout << "[dtrace] scenario: attach slot " << att.visualSlot << " actor "
                          << att.actorSno << " at " << att.hardpoint;
                if (att.visualSlot == 0) {
                    // The per-class art actor's own hair tag, beside the item
                    // actor's above — which of the two the engine honours is
                    // exactly what a wrong beard hinges on.
                    if (auto a2 = renderer.Loader().D3Cache().Actor(att.actorSno))
                        std::cout << " hair="
                                  << wf::io::d3n::tagMapValue(a2->arTagMap,
                                                              wf::io::d3n::kTagItemHairStyle)
                                         .value_or(0);
                }
                std::cout << std::endl;
            }
            for (const auto& [slot, dye] : anim.d3Dyes) {
                chars.SetOutfitDye(*adapter, static_cast<wf::io::d3n::EVisualSlot>(slot), dye);
                std::cout << "[dtrace] scenario: dye slot " << slot << " = " << dye << std::endl;
            }
            for (auto* a2 : spawned)
                renderer.Loader().RestyleD3Model(a2->handle);

            // Per-surface material state for the VISIBLE geosets, so "dressed
            // is too bright" becomes a table of pass flags rather than an
            // impression. Only when asked.
            if (anim.probe) {
                if (const auto* st =
                        dynamic_cast<const wf::renderer::profiles::diablo3::D3SurfaceTable*>(
                            hero->render.surfaceTable.get())) {
                    const auto hidden = adapter->GeosetHidden();
                    const auto& surfs = st->Surfaces();
                    for (wf::u32 g = 0; g < surfs.size(); ++g) {
                        if (g < hidden.size() && hidden[g])
                            continue;
                        const auto& s = surfs[g];
                        if (!s.valid)
                            continue;
                        std::cout << "[dtrace] surf " << g << " fx='" << s.pass.effectFile
                                  << "' resolved=" << s.pass.resolved << " lit=" << s.pass.lit
                                  << " unlit=" << s.unlit
                                  << " vcLights=" << s.pass.vertexColorLights
                                  << " gain=" << s.pass.colorGain << " emis=" << s.emissive.x
                                  << " chain=" << s.chainCount << std::endl;
                    }
                }
            }
        }
    }
#endif

    // Merge any `.m3a` before the scenario resolves a sequence name, because
    // an `.m3a`-driven model has no sequences of its own to resolve against.
    // Same two calls ViewerApp::AttachAnimationFile makes — attach, then re-Bind
    // so the playlist re-reads GetSequences.
    for (const std::filesystem::path& ap : attachAnims) {
#if WDX_ENABLE_M3
        std::ifstream in(ap, std::ios::binary);
        std::vector<wf::u8> bytes;
        if (in)
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (bytes.empty()) {
            std::cerr << "[dtrace] --attach-anim unreadable: " << wf::io::PathToUtf8(ap)
                      << std::endl;
            return 4;
        }
        // Per adapter, not per actor: the loader may hand every copy the same
        // one, and a second attach of the same label is refused by design.
        std::vector<wf::io::M3ModelAdapter*> done;
        for (auto* a : spawned) {
            if (!a->animation.HasSource())
                continue;
            auto* m3 = dynamic_cast<wf::io::M3ModelAdapter*>(a->animation.Source().get());
            if (!m3)
                continue;
            if (std::find(done.begin(), done.end(), m3) == done.end()) {
                if (!m3->AttachAnimationFile(wf::io::PathToUtf8(ap.stem()), bytes)) {
                    std::cerr << "[dtrace] --attach-anim rejected: " << wf::io::PathToUtf8(ap)
                              << std::endl;
                    return 4;
                }
                done.push_back(m3);
            }
            // Bind is what re-reads GetSequences; the merged sequences are
            // invisible to playback until it runs.
            a->animation.Bind(a->animation.Source());
        }
        if (done.empty()) {
            std::cerr << "[dtrace] --attach-anim needs an `.m3` model: " << wf::io::PathToUtf8(ap)
                      << std::endl;
            return 4;
        }
#else
        std::cerr << "[dtrace] --attach-anim needs -DWDX_ENABLE_M3=ON" << std::endl;
        return 4;
#endif
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
        if (auto* ms =
                dynamic_cast<wf::renderer::model::IModelSource*>(hero->animation.Source().get()))
            std::cout << "[dtrace] " << ms->GetSkeleton().nodeCount << " bone(s)" << std::endl;
        std::cout << "[dtrace] " << seqs.size() << " sequence(s):" << std::endl;
#if WDX_ENABLE_M3
        auto* m3 = dynamic_cast<wf::io::M3ModelAdapter*>(hero->animation.Source().get());
#endif
        for (std::size_t s = 0; s < seqs.size(); ++s) {
            std::cout << "[dtrace]   [" << s << "] " << seqs[s].name << "  " << seqs[s].startMs
                      << ".." << seqs[s].endMs << "ms"
                      << (seqs[s].nonLooping ? " (non-looping)" : "")
                      << (seqs[s].alwaysPlays ? " (global loop)" : "");
#if WDX_ENABLE_M3
            // The sub-track containers, because `subtrack=` in the corpus file
            // names one by index and there is no other way to see the list.
            // Only interesting when there is more than one — a lone `_full`
            // container is what every sequence has.
            if (m3) {
                const auto subs = m3->SubtracksOf(static_cast<wf::i32>(s));
                if (subs.size() > 1) {
                    std::cout << "  subtracks:";
                    for (std::size_t k = 0; k < subs.size(); ++k)
                        std::cout << " [" << k << "]" << subs[k].name << "(p" << subs[k].priority
                                  << (subs[k].concurrent ? ",conc" : ",excl") << ","
                                  << subs[k].trackCount << ")";
                }
            }
#endif
            std::cout << std::endl;
        }
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

    // Global loops run unless a scenario says otherwise, because the engine
    // runs them: `M3AnimState::Init` starts every `AlwaysGlobal` sequence
    // before the actor asks for anything. Silencing them is how a baseline
    // isolates one sequence.
    if (anim.noGlobals) {
        for (auto* a : spawned)
            a->animation.Playlist().SetGlobalSequences({});
        std::cout << "[dtrace] scenario: global loops silenced" << std::endl;
    } else {
        std::size_t globals = 0;
        for (const auto& s : seqs)
            if (s.alwaysPlays)
                ++globals;
        if (globals)
            std::cout << "[dtrace] scenario: " << globals << " global loop(s) playing" << std::endl;
    }

    // The solver arm. The renderer's default ground is the grid plane at z = 0
    // (physics/ground_plane.h); the harness overrides it with a plane at
    // `groundZ` because a non-zero height is the case that makes IK visibly
    // move, and supplies the aim target the renderer has no notion of.
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

#if WDX_ENABLE_D3
    if (anim.ragdoll) {
        std::size_t armed = 0;
        for (auto* a : spawned) {
            if (auto* d3 = dynamic_cast<wf::io::D3ModelAdapter*>(a->animation.Source().get())) {
                if (d3->HasPhysicsRig()) {
                    d3->SetRagdoll(true);
                    ++armed;
                }
            }
        }
        // The rig count is the difference between "this model has no rig" and
        // "the rig was armed and did not move" — two very different reasons for
        // a capture to look like its animation.
        std::cout << "[dtrace] scenario: ragdoll armed on " << armed << " actor(s), "
                  << hero->animation.PoseStages().size() << " stage(s)" << std::endl;
    }
#endif

    // Everything between the sampler and the bound palette, in the order it has
    // to hold. Any `no` here explains a frozen model on its own.
    if (anim.probe) {
        const auto& sk = hero->render.skinning;
        std::cout << "[dtrace] probe: skeleton=" << (sk.HasSkeleton() ? "yes" : "no")
                  << " nodes=" << sk.NodeCount() << " ready=" << (sk.IsReady() ? "yes" : "no")
                  << " perActorPalette=" << (sk.UsesPerActorPalette() ? "yes" : "no") << std::endl;
        for (const auto& geo : hero->render.gpuGeosets)
            std::cout << "[dtrace] probe: geoset " << geo.geosetId
                      << " hasSkinning=" << (geo.hasSkinning ? "yes" : "no") << " paletteCb="
                      << (geo.bonePaletteCb != wf::gfx::BufferHandle::Invalid ? "yes" : "no")
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
            d.subtrack = anim.layerSubtrack;
            d.persistent = true; // survives the covered-play cull for the capture
            for (auto* a : spawned)
                a->animation.Playlist().Play(d, a->cursor.actorTimeMs);
            std::cout << "[dtrace] scenario: frame " << i << " layer + [" << layerSeq << "] "
                      << seqs[layerSeq].name << " @w" << anim.layerWeight;
            if (anim.layerSubtrack >= 0)
                std::cout << " subtrack " << anim.layerSubtrack;
            std::cout << std::endl;
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
        // A Diablo III type-1 system spawns a whole `.acr` per emission, and
        // its assets legitimately arrive mid-capture — there is no settle pass
        // that can pre-resolve a model nothing has asked for yet. So this is
        // still fatal for a trace, and a switch for someone who only wants to
        // look at the picture.
        std::cerr << "[dtrace] a new asset need appeared mid-capture — the trace would be "
                     "timing-dependent; "
                  << (allowLateAssets ? "continuing anyway (--draw-trace-allow-late-assets)"
                                      : "aborting rather than recording it")
                  << std::endl;
        if (!allowLateAssets)
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

    std::cout << "[multiscene] sceneA actors=" << actorsA << " geosets=" << geoA << " mean=(" << mrA
              << "," << mgA << "," << mbA << ")  sceneB actors=" << actorsB << " geosets=" << geoB
              << " mean=(" << mrB << "," << mgB << "," << mbB << ")" << std::endl;

    const bool isolated = (actorsA == 0 && geoA == 0) && (actorsB == 1 && geoB > 0);
    // Pixel corroboration where readback actually produced an image. The D3D12
    // capture ring can return all-zero for stacked offscreen targets — treat
    // that as "readback unavailable" (no evidence either way), not cross-bleed.
    const bool readbackAvailable =
        okA && okB && !(mrA == 0 && mgA == 0 && mbA == 0) && !(mrB == 0 && mgB == 0 && mbB == 0);
    const bool pixelsOk = !readbackAvailable ||
                          ((std::abs(mrB - mrA) + std::abs(mgB - mgA) + std::abs(mbB - mbA)) > 0);
    const bool pass = isolated && pixelsOk;
    std::cout << "[multiscene] " << (pass ? "PASS" : "FAIL") << " (isolated=" << isolated
              << " pixelsOk=" << pixelsOk << ")" << std::endl;

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
    bool particleTraceDump = false;
    i32 particleDiffFrames = 120;
    std::string particleTraceRecord;
    std::string particleTraceCheck;
    bool drawTrace = false;
    bool drawTraceHd = false;
    bool drawTraceSdHdr = false;
    bool drawTraceUnlit = false;
    bool noClothDeform = false;
    bool drawTraceNoRefraction = false;
    bool drawTraceNoDistortion = false;
    bool drawTraceDistortionBuffer = false;
    bool drawTraceNoMultiTex = false;
    bool drawTraceRefractionMask = false;
    bool drawTraceDebugLight = false;
    bool drawTraceLazyAnim = false;
    bool drawTraceAllowLate = false;
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
    // Which install the run may reach for assets a model references by SNO id.
    // Empty is the corpus arm's state: the trace resolves nothing beyond the
    // file it was handed, which is what lets it run on a box with no game.
    std::string traceGame;
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
    //
    // The three positional arguments and every flag above keep their exact
    // meaning and their exact output filenames. Everything the redesign added
    // is additive and listed under "--export-clip" below; when any of those is
    // given, the positional sequence index is ignored.
    bool doExport = false;
    // Headless WEM export: load the model, write it as a `.wem`, exit. The
    // batch half of File ▸ Export to WEM, and the only way a script can convert
    // a directory of models without a window.
    std::filesystem::path exportWemPath;
    // Headless Warcraft III export: the batch half of File ▸ Export to MDX.
    // The profile is a flag rather than the dialog's disabled radio because the
    // classic path writes a different container AND a different texture format,
    // and neither is testable through a control nobody can click.
    std::filesystem::path exportMdxPath;
    auto exportMdxProfile = whiteout::models::wem::ProfileId::Wc3Reforged;
    bool exportMdxTextures = true;
    std::filesystem::path exportM3Path;
    auto exportM3Profile = whiteout::models::wem::ProfileId::Sc2;
    bool exportM3Textures = true;
    // Which profile a `.wem` on the command line opens as. `Count` leaves it to
    // the document — there is no dialog out here.
    auto wemProfile = whiteout::models::wem::ProfileId::Count;
    i32 exportSeq = 0;
    // Additive surface. Each --export-clip is `name|#index[:repeats][@speed]`.
    struct CliClip {
        std::string spec;
    };
    std::vector<CliClip> exportClips;
    i32 exportDurationMs = 0; // 0 = the queue's own length
    whiteout::flakes::ExportFill exportFill = whiteout::flakes::ExportFill::LoopLast;
    i32 exportPreRollMs = 0;
    // Applied to every clip start, so a queue can be cross-faded from the CLI
    // without a recipe file.
    i32 exportBlendMs = 0;
    i32 exportFrameStep = 1;
    bool exportOrbit = false;
    f32 exportOrbitDegPerSec = 0.0f;
    f32 exportOrbitRevs = 0.0f;
    bool exportOrbitRevsSet = false;
    f32 exportOrbitPitch = 0.0f;
    bool exportOrbitPitchSet = false;
    f32 exportOrbitDist = 0.0f;
    f32 exportOrbitStart = 0.0f;
    bool exportOrbitFit = false;
    whiteout::flakes::OrbitSubject exportSubject = whiteout::flakes::OrbitSubject::Camera;
    i32 exportAngles = 1;
    i32 exportSheetCols = -1; // -1 = not a sheet
    bool exportCrop = false;
    std::string exportNameTemplate;
    std::filesystem::path exportRecipeFile;
    // A recipe file is the base and flags override it, so the overrides have
    // to be the ones the user actually typed — otherwise a flag's *default*
    // silently overwrites what the recipe stored.
    bool exportFmtSet = false;
    bool exportResSet = false;
    bool exportTransparentSet = false;
    bool exportUiSet = false;
    bool exportFpsSet = false;
    bool exportFillSet = false;
    // `.m3a` files to merge into the loaded `.m3` before anything else runs.
    // The UI route is the toolbar's Anims button; this is the same call, so a
    // scripted render can exercise an attached sequence.
    std::vector<std::filesystem::path> attachAnims;
    i32 exportFps = 30;
    whiteout::flakes::ExportFormat exportFmt = whiteout::flakes::ExportFormat::PngFrames;
    bool exportTransparent = false;
    bool exportCaptureUi = false;
    /// Force the collision-shape overlay on for this run. The menu item is the
    /// real control; this exists so a headless capture can show the colliders,
    /// which is the only way to check a shape decode — a sphere read as a box
    /// is obvious on screen and invisible to every numeric gate.
    bool showCollisions = false;
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
            exportFpsSet = true;
            exportFolder = whiteout::flakes::io::FsPathFromUtf8(argv[++i]);
        } else if (std::strcmp(a, "--wem-profile") == 0 && i + 1 < argc) {
            wemProfile = whiteout::flakes::io::WemProfileFromName(argv[++i]);
            if (wemProfile == whiteout::models::wem::ProfileId::Count) {
                std::cerr << "Unknown WEM profile: " << argv[i]
                          << " (wc3_classic | wc3_reforged | wow | sc2 | heroes | diablo3)\n";
                return 1;
            }
        } else if (std::strcmp(a, "--export-wem") == 0 && i + 1 < argc) {
            exportWemPath = whiteout::flakes::io::FsPathFromUtf8(argv[++i]);
        } else if (std::strcmp(a, "--export-mdx") == 0 && i + 1 < argc) {
            exportMdxPath = whiteout::flakes::io::FsPathFromUtf8(argv[++i]);
        } else if (std::strcmp(a, "--export-mdx-profile") == 0 && i + 1 < argc) {
            const std::string name = argv[++i];
            exportMdxProfile = whiteout::flakes::io::WemProfileFromName(name);
            if (exportMdxProfile != whiteout::models::wem::ProfileId::Wc3Classic &&
                exportMdxProfile != whiteout::models::wem::ProfileId::Wc3Reforged) {
                std::cerr << "Unknown Warcraft III profile: " << name
                          << " (wc3_classic | wc3_reforged)\n";
                return 1;
            }
        } else if (std::strcmp(a, "--export-mdx-no-textures") == 0) {
            exportMdxTextures = false;
        } else if (std::strcmp(a, "--export-m3") == 0 && i + 1 < argc) {
            exportM3Path = whiteout::flakes::io::FsPathFromUtf8(argv[++i]);
        } else if (std::strcmp(a, "--export-m3-profile") == 0 && i + 1 < argc) {
            const std::string name = argv[++i];
            exportM3Profile = whiteout::flakes::io::WemProfileFromName(name);
            if (exportM3Profile != whiteout::models::wem::ProfileId::Sc2 &&
                exportM3Profile != whiteout::models::wem::ProfileId::Heroes) {
                std::cerr << "Unknown StarCraft II profile: " << name << " (sc2 | heroes)\n";
                return 1;
            }
        } else if (std::strcmp(a, "--export-m3-no-textures") == 0) {
            exportM3Textures = false;
        } else if (std::strcmp(a, "--attach-anim") == 0 && i + 1 < argc) {
            attachAnims.push_back(whiteout::flakes::io::FsPathFromUtf8(argv[++i]));
        } else if (std::strcmp(a, "--gif") == 0) {
            exportFmt = whiteout::flakes::ExportFormat::Gif;
            exportFmtSet = true;
        } else if (std::strcmp(a, "--apng") == 0) {
            exportFmt = whiteout::flakes::ExportFormat::Apng;
            exportFmtSet = true;
        } else if (std::strcmp(a, "--webp") == 0) {
            exportFmt = whiteout::flakes::ExportFormat::Webp;
            exportFmtSet = true;
        } else if (std::strcmp(a, "--transparent") == 0) {
            exportTransparent = true;
            exportTransparentSet = true;
        } else if (std::strcmp(a, "--ui") == 0) {
            exportCaptureUi = true;
            exportUiSet = true;
        } else if (std::strcmp(a, "--show-collisions") == 0) {
            showCollisions = true;
        } else if (std::strcmp(a, "--res") == 0 && i + 2 < argc) {
            exportResW = std::atoi(argv[++i]);
            exportResH = std::atoi(argv[++i]);
            exportResSet = true;
        } else if (std::strcmp(a, "--camera") == 0 && i + 1 < argc) {
            exportCamera = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--export-clip") == 0 && i + 1 < argc) {
            exportClips.push_back({argv[++i]});
        } else if (std::strcmp(a, "--export-duration") == 0 && i + 1 < argc) {
            exportDurationMs = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--export-fill") == 0 && i + 1 < argc) {
            const char* v = argv[++i];
            if (std::strcmp(v, "loop-queue") == 0)
                exportFill = whiteout::flakes::ExportFill::LoopQueue;
            else if (std::strcmp(v, "hold-last") == 0)
                exportFill = whiteout::flakes::ExportFill::HoldLast;
            else
                exportFill = whiteout::flakes::ExportFill::LoopLast;
            exportFillSet = true;
        } else if (std::strcmp(a, "--export-preroll") == 0 && i + 1 < argc) {
            exportPreRollMs = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--export-blend") == 0 && i + 1 < argc) {
            exportBlendMs = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--export-step") == 0 && i + 1 < argc) {
            exportFrameStep = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--export-orbit") == 0 && i + 1 < argc) {
            exportOrbit = true;
            exportOrbitDegPerSec = static_cast<f32>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--export-orbit-revs") == 0 && i + 1 < argc) {
            exportOrbit = true;
            exportOrbitRevsSet = true;
            exportOrbitRevs = static_cast<f32>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--export-orbit-pitch") == 0 && i + 1 < argc) {
            exportOrbitPitchSet = true;
            exportOrbitPitch = static_cast<f32>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--export-orbit-dist") == 0 && i + 1 < argc) {
            exportOrbitDist = static_cast<f32>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--export-orbit-start") == 0 && i + 1 < argc) {
            exportOrbitStart = static_cast<f32>(std::atof(argv[++i]));
        } else if (std::strcmp(a, "--export-fit") == 0) {
            exportOrbitFit = true;
        } else if (std::strcmp(a, "--export-subject") == 0 && i + 1 < argc) {
            exportSubject = (std::strcmp(argv[++i], "model") == 0)
                                ? whiteout::flakes::OrbitSubject::Model
                                : whiteout::flakes::OrbitSubject::Camera;
        } else if (std::strcmp(a, "--export-angles") == 0 && i + 1 < argc) {
            exportOrbit = true;
            exportAngles = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--export-sheet") == 0 && i + 1 < argc) {
            const char* v = argv[++i];
            exportSheetCols = (std::strcmp(v, "auto") == 0) ? 0 : std::atoi(v);
        } else if (std::strcmp(a, "--export-crop") == 0) {
            exportCrop = true;
        } else if (std::strcmp(a, "--export-name") == 0 && i + 1 < argc) {
            exportNameTemplate = argv[++i];
        } else if (std::strcmp(a, "--export-recipe") == 0 && i + 1 < argc) {
            exportRecipeFile = whiteout::flakes::io::FsPathFromUtf8(argv[++i]);
            doExport = true;
        } else if (std::strcmp(a, "--mp4") == 0) {
            exportFmt = whiteout::flakes::ExportFormat::Mp4;
            exportFmtSet = true;
        } else if (std::strcmp(a, "--webm") == 0) {
            exportFmt = whiteout::flakes::ExportFormat::WebmVp9;
            exportFmtSet = true;
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
        } else if (std::strcmp(a, "--trace-dump") == 0) {
            particleTraceDump = true;
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
        } else if (std::strcmp(a, "--draw-trace-sd-hdr") == 0) {
            // Route SD through the HDR scene target + tonemap (SceneHdrInSd)
            // — what the interactive viewer does for D3, where additive item
            // glows otherwise clip to white. Off by default so the recorded
            // baselines keep their meaning.
            drawTraceSdHdr = true;
        } else if (std::strcmp(a, "--draw-trace-unlit") == 0) {
            drawTraceUnlit = true;
        } else if (std::strcmp(a, "--no-cloth-deform") == 0) {
            noClothDeform = true;
        } else if (std::strcmp(a, "--draw-trace-no-refraction") == 0) {
            drawTraceNoRefraction = true;
        } else if (std::strcmp(a, "--draw-trace-refraction-mask") == 0) {
            drawTraceRefractionMask = true;
        } else if (std::strcmp(a, "--draw-trace-no-distortion") == 0) {
            drawTraceNoDistortion = true;
        } else if (std::strcmp(a, "--draw-trace-distortion-buffer") == 0) {
            drawTraceDistortionBuffer = true;
        } else if (std::strcmp(a, "--draw-trace-no-multitex") == 0) {
            drawTraceNoMultiTex = true;
        } else if (std::strcmp(a, "--draw-trace-debug-light") == 0) {
            drawTraceDebugLight = true;
        } else if (std::strcmp(a, "--draw-trace-allow-late-assets") == 0) {
            drawTraceAllowLate = true;
        } else if (std::strcmp(a, "--draw-trace-lazy-anim") == 0) {
            drawTraceLazyAnim = true;
        } else if (std::strcmp(a, "--listfile") == 0 && i + 1 < argc) {
            listfilePath = argv[++i];
        } else if (std::strcmp(a, "--draw-trace-game") == 0 && i + 1 < argc) {
            traceGame = argv[++i];
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
        } else if (std::strcmp(a, "--draw-trace-anim-subtrack") == 0 && i + 1 < argc) {
            drawTraceAnim.layerSubtrack = std::atoi(argv[++i]);
        } else if (std::strcmp(a, "--draw-trace-anim-no-globals") == 0) {
            drawTraceAnim.noGlobals = true;
        } else if (std::strcmp(a, "--draw-trace-anim-list") == 0) {
            drawTraceAnim.list = true;
        } else if (std::strcmp(a, "--draw-trace-anim-probe") == 0) {
            drawTraceAnim.probe = true;
        } else if (std::strcmp(a, "--draw-trace-solvers") == 0) {
            drawTraceAnim.solvers = true;
        } else if (std::strcmp(a, "--draw-trace-ragdoll") == 0) {
            drawTraceAnim.ragdoll = true;
        } else if (std::strcmp(a, "--d3-equip") == 0 && i + 1 < argc) {
            // <slot>=<ItemName>, slot by EVisualSlot name or ordinal.
            const std::string spec = argv[++i];
            const auto eq = spec.find('=');
            if (eq != std::string::npos) {
                const std::string slotName = spec.substr(0, eq);
                i32 slot = -1;
                const char* names[8] = {"head",      "torso",    "feet",      "hands",
                                        "righthand", "lefthand", "shoulders", "legs"};
                for (i32 sIdx = 0; sIdx < 8; ++sIdx)
                    if (slotName == names[sIdx])
                        slot = sIdx;
                if (slot < 0 && !slotName.empty() &&
                    slotName.find_first_not_of("0123456789") == std::string::npos)
                    slot = std::atoi(slotName.c_str());
                if (slot >= 0 && slot < 8)
                    drawTraceAnim.d3Equip.emplace_back(slot, spec.substr(eq + 1));
                else
                    std::cerr << "[dtrace] --d3-equip: bad slot '" << slotName << "'" << std::endl;
            }
        } else if (std::strcmp(a, "--d3-dye") == 0 && i + 1 < argc) {
            const std::string spec = argv[++i];
            const auto eq = spec.find('=');
            if (eq != std::string::npos) {
                const std::string slotName = spec.substr(0, eq);
                i32 slot = -1;
                const char* names[8] = {"head",      "torso",    "feet",      "hands",
                                        "righthand", "lefthand", "shoulders", "legs"};
                for (i32 sIdx = 0; sIdx < 8; ++sIdx)
                    if (slotName == names[sIdx])
                        slot = sIdx;
                if (slot < 0 && !slotName.empty() &&
                    slotName.find_first_not_of("0123456789") == std::string::npos)
                    slot = std::atoi(slotName.c_str());
                if (slot >= 0 && slot < 8)
                    drawTraceAnim.d3Dyes.emplace_back(slot, std::atoi(spec.c_str() + eq + 1));
            }
        } else if (std::strcmp(a, "--d3-sheathed") == 0) {
            drawTraceAnim.d3Sheathed = true;
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
                      << "] [--wgpu-backend d3d11|d3d12|vulkan|gl] [<model-path>]\n"
                      << "       --export-wem <out.wem>   write the model as WEM and exit\n"
                      << "       --export-mdx <out.mdx>   write it as Warcraft III and exit\n"
                      << "       --export-mdx-profile <n> wc3_reforged (default) | wc3_classic\n"
                      << "       --export-mdx-no-textures do not write the textures beside it\n"
                      << "       --export-m3 <out.m3>     write it as StarCraft II and exit\n"
                      << "       --export-m3-profile <n>  sc2 (default) | heroes\n"
                      << "       --export-m3-no-textures  do not write the textures beside it\n"
                      << "       --wem-profile <name>     open a .wem as that profile\n";
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
        if (a == Api::Vulkan)
            return true;
#if WDX_HAS_WEBGPU
        if (a == Api::WebGPU)
            return true;
#endif
#if WDX_HAS_METAL
        if (a == Api::Metal)
            return true;
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

    // An explicit opt-in, never inferred from the model's extension. Diablo III
    // reaches its ShaderMap, Shaders and shared Material assets by SNO id, and
    // an id resolves only through an opened storage — so without this a `.app`
    // renders with whatever its embedded material alone can say. The corpus arm
    // deliberately runs without it. Applied inside RunDrawTrace, after the
    // device is up.
    auto traceGameId = whiteout::flakes::ProductId::Neutral;
    if (!traceGame.empty()) {
        using ::whiteout::flakes::ProductId;
        if (traceGame == "wc3")
            traceGameId = ProductId::Wc3;
        else if (traceGame == "wow")
            traceGameId = ProductId::Wow;
        else if (traceGame == "sc2")
            traceGameId = ProductId::Sc2;
        else if (traceGame == "d3")
            traceGameId = ProductId::D3;
        else {
            std::cerr << "[dtrace] --draw-trace-game: expected wc3|wow|sc2|d3, got '" << traceGame
                      << "'" << std::endl;
            return 2;
        }
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
                               particleDiffDevice, contentRoot, particleTraceDump);

    // Before the trace, not with the interactive switches below: RunDrawTrace
    // returns without ever reaching them, and nothing between here and it
    // reloads the settings block.
    if (noClothDeform)
        renderer.Settings().SetClothDeform(false);

    if (drawTraceSdHdr)
        renderer.Settings().SetSceneHdrInSd(true);
    if (drawTrace)
        return RunDrawTrace(renderer, scene, backend, mdxPath, drawTraceRecord, drawTraceCheck,
                            drawTraceGolden, particleDiffFrames, drawTraceHd, drawTraceDistanceTol,
                            drawTraceCameraDistance, drawTracePerturb, drawTraceInstances,
                            drawTraceUnlit, drawTraceLazyAnim, drawTraceAllowLate, contentRoot,
                            drawTraceAnim, attachAnims, drawTraceDebugLight, drawTraceNoRefraction,
                            drawTraceRefractionMask, drawTraceNoMultiTex, traceGameId,
                            drawTraceNoDistortion, drawTraceDistortionBuffer, wemProfile);

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
    // or MPQ off entirely, and reorder the MPQ load list. Per game, and the
    // profile the settings panel was left on is the one loaded now. Through the
    // app so it records the product as configured: after this, switching onto
    // it must not re-apply (see ViewerApp::ApplyProfile).
    app.ApplyProfile(app.SettingsProfile(), /*force=*/true);

    // After the profile, because loading settings overwrites the flag block.
    if (showCollisions) {
        auto df = renderer.Settings().GetDisplayFlags();
        df.showCollisions = true;
        renderer.Settings().SetDisplayFlags(df);
    }

    // NFD is also used by Settings > IO (folder picker) and File > Open
    // (re-opened from the menu bar), so initialise it unconditionally rather
    // than only when we pop the startup model picker.
    NFD::Init();

    // CLI path wins; otherwise pop NFD once at startup. Re-openable via
    // File > Open in the menu bar.
    if (mdxPath.empty()) {
        NFD::UniquePathU8 outPath;
        nfdu8filteritem_t filter[4] = {
            {"All supported", whiteout::flakes::kOpenAllExtensions},
            {"Warcraft III Model", "mdx,mdl"},
            {"PKB Effect", "pkb,pkfx"},
            {"Other Blizzard model", whiteout::flakes::kForeignModelExtensions}};
        const nfdfiltersize_t nFilters = whiteout::flakes::kHasForeignModelFilter ? 4 : 3;
        if (NFD::OpenDialog(outPath, filter, nFilters) == NFD_OKAY)
            mdxPath = whiteout::flakes::io::FsPathFromUtf8(outPath.get());
    }
    // Loading HERE, before the frame loop, is what made "open the app and pick
    // a World of Warcraft model" freeze with nothing on screen: the install
    // open and the client-database reads both happen on this thread, and there
    // is no frame yet in which a progress bar could be drawn.
    //
    // So an interactive run hands the paths to the app and lets the first ticks
    // open them through OpenModelAsync, behind the modal. The headless flags
    // cannot: --attach-anim runs a few lines below and --export-anim counts a
    // fixed number of ticks, so both need the model in hand right now.
    // Before any open: the profile decides which game's storage the document's
    // textures resolve against, which FollowModelGame settles on the way in.
    app.SetPreferredWemProfile(wemProfile);

    const bool headlessWork =
        doExport || !exportWemPath.empty() || !exportMdxPath.empty() || !exportM3Path.empty() ||
        !attachAnims.empty();

    if (!mdxPath.empty()) {
        // No exists() pre-check for headless work: LoadModel accepts a
        // storage-internal path (the shared provider resolves it) and says so
        // itself when nothing does.
        if (headlessWork) {
            if (!app.LoadModel(mdxPath))
                std::cerr << "Failed to load model.\n";
        } else if (!std::filesystem::exists(mdxPath)) {
            std::cerr << "File not found: " << whiteout::flakes::io::PathToUtf8(mdxPath) << "\n";
        } else {
            app.QueueInitialOpen(mdxPath);
        }
    }
    // Open any additional positional paths in their own tabs. The last one
    // loaded ends up active, matching File > Open opening into a new tab.
    for (const auto& extra : extraPaths) {
        if (!std::filesystem::exists(extra)) {
            std::cerr << "File not found: " << whiteout::flakes::io::PathToUtf8(extra) << "\n";
        } else if (headlessWork) {
            if (!app.LoadModel(extra))
                std::cerr << "Failed to load model: " << whiteout::flakes::io::PathToUtf8(extra)
                          << "\n";
        } else {
            app.QueueInitialOpen(extra);
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

    // Headless WEM export. No warm-up ticks: the conversion reads the parsed
    // model the load already produced and touches nothing on the GPU.
    if (!exportWemPath.empty()) {
        const bool ok = app.ExportWem(exportWemPath);
        app.Close();
        return ok ? 0 : 1;
    }

    // Headless Warcraft III export, for the same reason and with the same
    // absence of warm-up ticks: the conversion reads the parsed model and the
    // texture pass reads the content provider, neither of which is the GPU.
    if (!exportMdxPath.empty()) {
        const bool ok = app.ExportMdx(exportMdxPath, exportMdxProfile, exportMdxTextures);
        app.Close();
        return ok ? 0 : 1;
    }

    // Headless StarCraft II export -- ExportMdx's twin, same reasoning.
    if (!exportM3Path.empty()) {
        const bool ok = app.ExportM3(exportM3Path, exportM3Profile, exportM3Textures);
        app.Close();
        return ok ? 0 : 1;
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
        whiteout::flakes::ExportRecipe recipe;
        // A recipe file is the base; individual flags override it.
        if (!exportRecipeFile.empty()) {
            whiteout::flakes::ExportRecipeLoadReport rep;
            if (!whiteout::flakes::ReadExportRecipeFile(exportRecipeFile, recipe,
                                                        app.SequenceNames(), &rep)) {
                std::cerr << "Failed to read export recipe: "
                          << whiteout::flakes::io::PathToUtf8(exportRecipeFile) << "\n";
            }
            for (const std::string& missing : rep.unresolvedClips)
                std::cerr << "[viewer] recipe clip not in this model: " << missing << "\n";
        }

        if (!exportClips.empty()) {
            // `name|#index[:repeats][@speed]`. When any of these is given the
            // positional sequence index is ignored, and we say so.
            if (exportSeq != 0)
                std::printf("[viewer] --export-clip given; ignoring the positional index %d\n",
                            exportSeq);
            recipe.clips.clear();
            for (const CliClip& c : exportClips) {
                std::string spec = c.spec;
                whiteout::flakes::ExportClip clip;
                const std::size_t at = spec.rfind('@');
                if (at != std::string::npos) {
                    clip.speed = static_cast<f32>(std::atof(spec.c_str() + at + 1));
                    spec.resize(at);
                }
                const std::size_t colon = spec.rfind(':');
                if (colon != std::string::npos) {
                    clip.repeats = std::atoi(spec.c_str() + colon + 1);
                    spec.resize(colon);
                }
                if (!spec.empty() && spec[0] == '#') {
                    clip.sequence = std::atoi(spec.c_str() + 1);
                    clip.savedName =
                        whiteout::flakes::SequenceKey(app.SequenceNames(), clip.sequence);
                } else {
                    clip.savedName = spec;
                    clip.sequence = whiteout::flakes::ResolveSequenceKey(app.SequenceNames(), spec);
                    if (clip.sequence < 0)
                        std::cerr << "[viewer] no animation named '" << spec << "'\n";
                }
                recipe.clips.push_back(std::move(clip));
            }
        } else if (recipe.clips.empty()) {
            whiteout::flakes::ExportClip clip;
            clip.sequence = exportSeq;
            clip.savedName = whiteout::flakes::SequenceKey(app.SequenceNames(), exportSeq);
            recipe.clips.push_back(std::move(clip));
        }

        const bool fromRecipe = !exportRecipeFile.empty();
        if (exportFpsSet || !fromRecipe)
            recipe.timing.fps = exportFps;
        if (exportDurationMs > 0) {
            recipe.timing.duration = whiteout::flakes::ExportDuration::Fixed;
            recipe.timing.durationMs = exportDurationMs;
        }
        if (exportFillSet || !fromRecipe)
            recipe.timing.fill = exportFill;
        if (exportPreRollMs > 0)
            recipe.timing.preRollMs = exportPreRollMs;
        if (exportBlendMs > 0)
            for (whiteout::flakes::ExportClip& c : recipe.clips)
                c.blendMs = exportBlendMs;
        if (exportFrameStep > 1)
            recipe.timing.frameStep = exportFrameStep;

        if (exportOrbit) {
            recipe.camera.mode = whiteout::flakes::ExportCameraMode::Orbit;
            recipe.camera.subject = exportSubject;
            recipe.camera.angleCount = exportAngles;
            recipe.camera.startYawDeg = exportOrbitStart;
            recipe.camera.fitToBounds = exportOrbitFit;
            if (exportOrbitRevsSet) {
                recipe.camera.timing = whiteout::flakes::OrbitTiming::Revolutions;
                recipe.camera.revolutions = exportOrbitRevs;
            } else {
                recipe.camera.timing = whiteout::flakes::OrbitTiming::Velocity;
                recipe.camera.degPerSec = exportOrbitDegPerSec;
            }
            if (exportOrbitPitchSet) {
                recipe.camera.overridePitch = true;
                recipe.camera.pitchDeg = exportOrbitPitch;
            }
            if (exportOrbitDist > 0.0f) {
                recipe.camera.overrideDistance = true;
                recipe.camera.distance = exportOrbitDist;
            }
        } else if (exportCamera >= 0) {
            recipe.camera.mode = whiteout::flakes::ExportCameraMode::Preset;
            recipe.camera.preset = exportCamera;
        }

        if (exportSheetCols >= 0)
            recipe.output.format = whiteout::flakes::ExportFormat::PngSheet;
        else if (exportFmtSet || !fromRecipe)
            recipe.output.format = exportFmt;
        if (exportSheetCols > 0)
            recipe.output.sheetColumns = exportSheetCols;
        if (exportTransparentSet || !fromRecipe)
            recipe.output.transparent = exportTransparent;
        if (exportUiSet || !fromRecipe)
            recipe.output.captureUi = exportCaptureUi;
        if (exportResSet || !fromRecipe) {
            recipe.output.width = exportResW;
            recipe.output.height = exportResH;
        }
        if (exportCrop)
            recipe.output.autoCrop = true;
        if (!fromRecipe) {
            // A bare headless run is a gate: leave the overlays exactly where
            // the other flags put them, or --show-collisions would export a
            // picture without the colliders it was asked for, and write no
            // sidecar beside a golden. A recipe says what it wants.
            recipe.output.hideOverlays = false;
            recipe.output.writeSidecar = false;
        }
        if (!exportNameTemplate.empty())
            recipe.output.nameTemplate = exportNameTemplate;
        if (!exportFolder.empty())
            recipe.output.folder = exportFolder;

        app.RequestAnimationExport(std::move(recipe));
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
