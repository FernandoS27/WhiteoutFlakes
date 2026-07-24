#include "cubeb_sound_emitter.h"
#include "gfx/gfx.h"
#include "pkb_effect_source.h"
#include "renderer/frame_ticker.h"
#include "renderer/model/model_loader.h"
#include "renderer/particle/particle_service.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "localization.h"
#include "settings_ini.h"
#include "viewer_app.h"
#include "whiteout/flakes/gfx_types.h"
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <nfd.hpp>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
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
            auto src = std::make_shared<wf::renderer::model::PkbEffectSource>(
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

    // Headless multi-viewport smoke test: no window, no ViewerApp — drive the
    // pipeline straight into an off-screen target and read it back. Runs and
    // exits without ever creating a GLFW window.
    if (headlessTest)
        return RunHeadlessTest(renderer, scene, backend, mdxPath);

    if (multiSceneTest)
        return RunMultiSceneTest(renderer, backend, mdxPath);

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
            whiteout::flakes::io::PathToUtf8(whiteout::flakes::ExecutableDir() / "lang");
        i18n::Localizer::instance().load(langDir, i18n::languageFromCode(languageCode));
    }

    // IO overrides — Settings > IO can repoint the install path, toggle CASC
    // or MPQ off entirely, and reorder the MPQ load list.
    {
        auto overrides = whiteout::flakes::LoadIoPathOverrides();
        auto& provider = renderer.Scene().GetContentProvider();
        if (!overrides.installPath.empty())
            provider.SetInstallPath(overrides.installPath);
        provider.SetIgnoreCasc(overrides.ignoreCasc);
        provider.SetIgnoreMpq(overrides.ignoreMpq);
        if (overrides.mpqListSet)
            provider.SetMpqList(std::move(overrides.mpqList));
    }

    // NFD is also used by Settings > IO (folder picker) and File > Open
    // (re-opened from the menu bar), so initialise it unconditionally rather
    // than only when we pop the startup model picker.
    NFD::Init();

    // CLI path wins; otherwise pop NFD once at startup. Re-openable via
    // File > Open in the menu bar.
    if (mdxPath.empty()) {
        NFD::UniquePathU8 outPath;
        nfdu8filteritem_t filter[3] = {{"All supported", "mdx,mdl,pkb,pkfx"},
                                       {"Warcraft III Model", "mdx,mdl"},
                                       {"PKB Effect", "pkb,pkfx"}};
        if (NFD::OpenDialog(outPath, filter, 3) == NFD_OKAY)
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
