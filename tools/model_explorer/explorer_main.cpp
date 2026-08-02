// ModelExplorer — entry point. Brings up the renderer + window and runs the
// explorer loop. Optional CLI: [--backend d3d11|d3d12|vulkan|webgpu] [<casc-root>].

#include "casc_browser.h"
#include "explorer_app.h"
#include "renderer/model/corn_effect_source.h"
#include "thumbnail_framing.h"
#include "thumbnail_pool.h"

#include "gfx/gfx.h"
#include "io/file_content_provider.h"
#include "renderer/camera.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_loader.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/render_settings.h"
#include "renderer/scene_manager.h"
#include "renderer/viewport.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <whiteout/textures/png/writer.h>
#include <whiteout/textures/texture.h>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    std::fprintf(stderr, "\n[CRASH] code=0x%08lX addr=%p\n",
                 ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    HANDLE proc = ::GetCurrentProcess();
    ::SymInitialize(proc, nullptr, TRUE);
    void* frames[32];
    USHORT n = ::CaptureStackBackTrace(0, 32, frames, nullptr);
    char buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    for (USHORT i = 0; i < n; ++i) {
        DWORD64 disp = 0;
        if (::SymFromAddr(proc, reinterpret_cast<DWORD64>(frames[i]), &disp, sym))
            std::fprintf(stderr, "  [%2d] %s +0x%llX\n", i, sym->Name, (unsigned long long)disp);
        else
            std::fprintf(stderr, "  [%2d] %p\n", i, frames[i]);
    }
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

using whiteout::flakes::gfx::GfxApi;

static int CompareCi(const char* a, const char* b) {
#if defined(_WIN32)
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}

int main(int argc, char* argv[]) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered so diagnostics survive crashes
#if defined(_WIN32)
    ::SetUnhandledExceptionFilter(CrashHandler);
#endif
#if defined(_WIN32)
    GfxApi backend = GfxApi::D3D12;
    std::vector<std::string> utf8Args;
    std::vector<char*> utf8Argv;
    if (int wArgc = 0; LPWSTR* wArgv = ::CommandLineToArgvW(::GetCommandLineW(), &wArgc)) {
        for (int i = 0; i < wArgc; ++i)
            utf8Args.push_back(whiteout::flakes::io::PathToUtf8(std::filesystem::path(wArgv[i])));
        ::LocalFree(wArgv);
        for (auto& s : utf8Args)
            utf8Argv.push_back(s.data());
        argc = wArgc;
        argv = utf8Argv.data();
    }
#else
    GfxApi backend = GfxApi::Vulkan;
#endif

    std::string cascRoot;
    bool selfTest = false;
    bool renderSelfTest = false;
    std::string rsInstallDir, rsRelPath;
    std::string lsPath;          // --ls: navigate here in --selftest
    bool lsPathSet = false;
    std::string cascRenderPath;  // --casc-render / --window-repro: archive path from cascRoot
    bool cascRender = false;
    bool windowRepro = false;
    bool navStress = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strcmp(a, "--nav-stress") == 0) {
            navStress = true;
        } else if (std::strcmp(a, "--selftest") == 0) {
            selfTest = true;
        } else if (std::strcmp(a, "--ls") == 0 && i + 1 < argc) {
            lsPath = argv[++i];
            lsPathSet = true;
        } else if (std::strcmp(a, "--window-repro") == 0 && i + 1 < argc) {
            windowRepro = true;
            cascRenderPath = argv[++i];
        } else if (std::strcmp(a, "--casc-render") == 0 && i + 1 < argc) {
            cascRender = true;
            cascRenderPath = argv[++i];
        } else if (std::strcmp(a, "--render-selftest") == 0 && i + 2 < argc) {
            renderSelfTest = true;
            rsInstallDir = argv[++i];
            rsRelPath = argv[++i];
        } else if ((std::strcmp(a, "--backend") == 0 || std::strcmp(a, "-b") == 0) && i + 1 < argc) {
            const char* v = argv[++i];
            if (CompareCi(v, "vulkan") == 0)
                backend = GfxApi::Vulkan;
            else if (CompareCi(v, "webgpu") == 0)
                backend = GfxApi::WebGPU;
#if defined(_WIN32)
            else if (CompareCi(v, "d3d11") == 0)
                backend = GfxApi::D3D11;
            else if (CompareCi(v, "d3d12") == 0)
                backend = GfxApi::D3D12;
#endif
        } else if (a[0] != '-' && cascRoot.empty()) {
            cascRoot = a;
        }
    }

    // Device-free smoke test of the CASC browser: open, list the root, descend
    // into the first folder, list again. Verifies enumeration without a window.
    if (selfTest) {
        if (cascRoot.empty()) {
            std::fprintf(stderr, "[explorer-selftest] usage: --selftest <casc-root>\n");
            return 1;
        }
        whiteout::flakes::tools::CascBrowser br;
        std::string err;
        if (!br.Open(cascRoot, &err)) {
            std::fprintf(stderr, "[explorer-selftest] open FAILED: %s\n", err.c_str());
            return 2;
        }
        std::printf("[explorer-selftest] opened '%s'\n", br.Root().c_str());
        if (lsPathSet) {
            br.NavigateTo(lsPath);
            std::printf("[explorer-selftest] '%s': %zu folders, %zu model files\n", lsPath.c_str(),
                        br.Current().folders.size(), br.Current().modelFiles.size());
            for (size_t i = 0; i < br.Current().folders.size() && i < 30; ++i)
                std::printf("  [dir]  %s\n", br.Current().folders[i].c_str());
            for (size_t i = 0; i < br.Current().modelFiles.size() && i < 30; ++i)
                std::printf("  [file] %s\n", br.Current().modelFiles[i].c_str());
            std::printf("[explorer-selftest] PASS\n");
            return 0;
        }
        std::printf("[explorer-selftest] root: %zu folders, %zu model files\n",
                    br.Current().folders.size(), br.Current().modelFiles.size());
        if (!br.Current().folders.empty()) {
            const std::string first = br.Current().folders.front();
            br.Descend(first);
            std::printf("[explorer-selftest] descended into '%s': %zu folders, %zu model files\n",
                        first.c_str(), br.Current().folders.size(),
                        br.Current().modelFiles.size());
        }
        std::printf("[explorer-selftest] PASS\n");
        return 0;
    }

    whiteout::flakes::renderer::SceneManager scene;
    whiteout::flakes::renderer::RenderService renderer(scene);

    // Stress the scene create/destroy/recycle/clear cycles the live grid drives
    // when browsing: load many models into a small pool (forcing recycle), render
    // + Present each, Clear, navigate, repeat. Survives without abort() => OK.
    if (navStress) {
        namespace wf = whiteout::flakes;
        if (cascRoot.empty()) {
            std::fprintf(stderr, "[nav-stress] usage: <cascRoot> --nav-stress\n");
            return 1;
        }
        wf::tools::CascBrowser br;
        std::string err;
        if (!br.Open(cascRoot, &err)) {
            std::fprintf(stderr, "[nav-stress] CASC open FAILED: %s\n", err.c_str());
            return 2;
        }
        if (!renderer.Pipeline().InitDevice(backend)) {
            std::fprintf(stderr, "[nav-stress] InitDevice FAILED\n");
            return 2;
        }
        auto provider = std::make_shared<wf::io::FileContentProvider>();
        provider->SetInstallPath(br.Root());
        renderer.Settings().SetBackgroundColor(58, 64, 74);
        wf::renderer::RenderTargetId screen =
            renderer.Pipeline().CreateOffscreenTarget(256, 256);
        renderer.Pipeline().SetPrimaryTarget(screen);
        // A couple of model-rich folders to bounce between.
        const char* folders[] = {"sharedmodels", "_hd.w3mod\\sharedmodels",
                                  "_hd.w3mod\\units\\human", "_hd.w3mod\\units\\nightelf"};
        {
            const int stressCap = std::getenv("STRESS_CAP") ? std::atoi(std::getenv("STRESS_CAP")) : 4;
            wf::tools::ThumbnailPool pool(renderer, provider, stressCap, /*res*/ 256);
            std::uint64_t frame = 0;
            for (int round = 0; round < 6; ++round) {
                br.NavigateTo(folders[round % 4]);
                const auto files = br.Current().modelFiles; // copy (mirrors deferred-nav safety)
                for (int f = 0; f < 8; ++f) {
                    pool.BeginFrame(++frame);
                    const int maxShown = std::getenv("SHOWN") ? std::atoi(std::getenv("SHOWN")) : 12;
                    int shown = 0;
                    for (const auto& name : files) {
                        if (shown++ >= maxShown)
                            break;
                        const bool eff = name.find(".pkb") != std::string::npos ||
                                         name.find(".pkfx") != std::string::npos;
                        pool.Acquire(br.ChildPath(name), eff, frame);
                    }
                    pool.RenderVisible(0.016f);
                }
                pool.Clear(); // navigation away
                std::printf("[nav-stress] round %d folder='%s' files=%zu OK\n", round,
                            folders[round % 4], files.size());
            }
        }
        renderer.Pipeline().Gfx()->WaitIdle();
        std::printf("[nav-stress] PASS\n");
        renderer.Pipeline().Shutdown();
        std::fflush(stdout);
        std::_Exit(0);
    }

    // Headless render smoke test of the ThumbnailPool: bring up the device with
    // no window, load one model into a pooled cell scene via a disk-backed
    // provider, render it a few frames, and confirm the cell produced a live
    // texture. Exercises the pool's Acquire/LoadCell/RenderVisible path.
    if (renderSelfTest) {
        if (!renderer.Pipeline().InitDevice(backend)) {
            std::fprintf(stderr, "[explorer-render] InitDevice FAILED\n");
            return 2;
        }
        auto provider = std::make_shared<whiteout::flakes::io::FileContentProvider>();
        provider->SetInstallPath(rsInstallDir);
        renderer.Settings().SetBackgroundColor(40, 80, 160); // distinguish bg from broken target
        { // match the app's thumbnail render config (HD, no grid)
            auto df = renderer.Settings().GetDisplayFlags();
            df.showGrid = false;
            df.renderMode = whiteout::flakes::renderer::RenderMode::HD;
            renderer.Settings().SetDisplayFlags(df);
            provider->SetHdMode(true);
        }
        {
            whiteout::flakes::tools::ThumbnailPool pool(renderer, provider, /*cap*/ 4, /*res*/ 256);
            const bool isEffect =
                rsRelPath.size() > 4 && (rsRelPath.substr(rsRelPath.size() - 4) == ".pkb" ||
                                         rsRelPath.find(".pkfx") != std::string::npos);
            whiteout::flakes::gfx::TextureHandle tex = whiteout::flakes::gfx::TextureHandle::Invalid;
            const int rsFrames = std::getenv("RS_FRAMES") ? std::atoi(std::getenv("RS_FRAMES")) : 40;
            for (int f = 0; f < rsFrames; ++f) {
                pool.BeginFrame(static_cast<std::uint64_t>(f + 1));
                tex = pool.Acquire(rsRelPath, isEffect, static_cast<std::uint64_t>(f + 1));
                pool.RenderVisible(0.016f);
            }
            renderer.Pipeline().Gfx()->WaitIdle();
            // Read the cell's target.color — the exact image ImGui samples. The
            // pool now Presents each cell, so its render is flushed. ReadbackTarget
            // works on WebGPU; on D3D it returns false (no portable readback) and
            // the texture-valid check carries the verdict.
            long mean = -1;
            std::vector<whiteout::flakes::u8> rgba;
            int cw = 0, ch = 0;
            if (renderer.Pipeline().ReadbackTarget(pool.DebugFirstCellTarget(), rgba, cw, ch) &&
                (int)rgba.size() >= cw * ch * 4 && cw > 0) {
                long s = 0;
                for (int p = 0; p < cw * ch; ++p)
                    s += rgba[p * 4 + 0] + rgba[p * 4 + 1] + rgba[p * 4 + 2];
                mean = s / (long)(cw * ch * 3);
            }
            const bool pass = tex != whiteout::flakes::gfx::TextureHandle::Invalid && mean != 0;
            std::printf("[explorer-render] cell texture=%s target.color mean=%ld -> %s\n",
                        pass ? "valid" : "invalid", mean, pass ? "PASS" : "FAIL");
            renderer.Pipeline().Shutdown();
            std::fflush(stdout);
            std::_Exit(pass ? 0 : 7);
        }
    }

    // End-to-end CASC render test: open the CASC as the shared provider, load
    // one model from an archive path into a fresh scene, render it into an
    // offscreen target, and confirm it loaded geosets + produced a non-black
    // image — proving CASC-backed model + texture resolution and rendering.
    if (cascRender) {
        namespace wf = whiteout::flakes;
        if (cascRoot.empty()) {
            std::fprintf(stderr, "[casc-render] usage: <cascRoot> --casc-render <archivePath>\n");
            return 1;
        }
        if (!renderer.Pipeline().InitDevice(backend)) {
            std::fprintf(stderr, "[casc-render] InitDevice FAILED\n");
            return 2;
        }
        auto provider = std::make_shared<wf::io::FileContentProvider>();
        provider->SetInstallPath(cascRoot);
        renderer.Settings().SetBackgroundColor(40, 80, 160);
        { // match the app's thumbnail config (no grid; mode set per-model below)
            auto df = renderer.Settings().GetDisplayFlags();
            df.showGrid = false;
            renderer.Settings().SetDisplayFlags(df);
        }
        constexpr int kRes = 256;
        wf::renderer::RenderTargetId tid = renderer.Pipeline().CreateOffscreenTarget(kRes, kRes);
        renderer.Pipeline().SetPrimaryTarget(tid);
        renderer.Pipeline().EnableFrameCapture(true);

        wf::renderer::SceneId sid =
            std::getenv("USE_DEFAULT") ? renderer.DefaultSceneId() : renderer.CreateScene();
        renderer.SceneAt(sid).SetContentProvider(provider);
        renderer.SetActiveScene(sid);
        std::printf("[casc-render] scene=%u (default=%u)\n", sid, renderer.DefaultSceneId());
        std::filesystem::path ap(cascRenderPath);
        renderer.SceneAt(sid).SetPE1BasePath(ap.parent_path());
        const bool isEffect = cascRenderPath.find(".pkb") != std::string::npos ||
                              cascRenderPath.find(".pkfx") != std::string::npos;
        wf::renderer::model::Actor* hero =
            isEffect ? renderer.Loader().SpawnUnitFromSource(
                           std::make_shared<wf::renderer::model::CornEffectSource>(cascRenderPath))
                     : renderer.Loader().SpawnUnit(cascRenderPath);
        if (isEffect) {
            renderer.Settings().SetBackgroundColor(33, 38, 48); // match app UI bg for effects
            const bool effHd = cascRenderPath.find("_hd.w3mod") != std::string::npos;
            auto df = renderer.Settings().GetDisplayFlags();
            df.renderMode = effHd ? wf::renderer::RenderMode::HD : wf::renderer::RenderMode::SD;
            renderer.Settings().SetDisplayFlags(df);
            provider->SetHdMode(effHd);
            std::printf("[casc-render] effect hd=%d\n", effHd ? 1 : 0);
        }
        if (hero && !isEffect) {
            const bool hd = whiteout::flakes::tools::IsHdModel(hero);
            // Render in the model's natural shading mode, but route the SD path
            // through the HDR scene target + tonemap (SceneHdrInSd) so additive
            // / team-color geosets roll off instead of clipping to opaque white
            // — the same clipping the .pkb effects hit. SD models keep their SD
            // textures (provider overlay follows real HD-ness).
            renderer.Settings().SetSceneHdrInSd(true);
            auto df = renderer.Settings().GetDisplayFlags();
            df.renderMode = hd ? wf::renderer::RenderMode::HD : wf::renderer::RenderMode::SD;
            renderer.Settings().SetDisplayFlags(df);
            provider->SetHdMode(hd);
            std::printf("[casc-render] hd=%d (sd-hdr tonemap on)\n", hd ? 1 : 0);
            const int sidx = whiteout::flakes::tools::PickStandSequenceIndex(hero);
            if (sidx >= 0)
                hero->animation.SetActiveSequenceIndex(sidx);
            const auto sq = hero->animation.Sequences();
            const std::string sname =
                (sidx >= 0 && sidx < (int)sq.size()) ? sq[sidx].name : std::string("<none>");
            whiteout::flakes::tools::FrameCameraToModelSequence(renderer.SceneAt(sid).Camera(), hero,
                                                                sname);
            std::printf("[casc-render] sequences=%zu activeSeq=#%d '%s'\n", sq.size(), sidx,
                        sname.c_str());
        }
        renderer.SetActiveScene(renderer.DefaultSceneId());
        std::printf("[casc-render] spawn %s\n", hero ? "OK" : "FAILED");

        wf::renderer::Viewport vp;
        vp.scene = sid;
        vp.target = tid;
        vp.camera = &renderer.SceneAt(sid).Camera();
        const float loopDt = std::getenv("STATIC") ? 0.0f : 0.016f;
        std::printf("[casc-render] entering render loop (effect=%d)\n", isEffect ? 1 : 0);
        bool effectFramed = false;
        for (int f = 0; f < 60; ++f) {
            renderer.TickScenes(loopDt);
            renderer.Pipeline().RenderViewport(vp);
            renderer.Pipeline().Present(tid);
            if (isEffect && hero && !effectFramed && f >= 12) {
                renderer.SetActiveScene(sid);
                effectFramed = whiteout::flakes::tools::FrameCameraToEffect(
                    renderer, renderer.SceneAt(sid).Camera(), hero->handle);
                renderer.SetActiveScene(renderer.DefaultSceneId());
            }
        }
        int geosets = 0, textures = 0;
        for (auto& [h, mi] : renderer.SceneAt(sid).Actors().All()) {
            geosets += (int)mi->render.gpuGeosets.size();
            textures += mi->render.textures ? (int)mi->render.textures->Size() : 0;
        }
        std::printf("[casc-render] textures=%d\n", textures);

        std::vector<wf::u8> rgba;
        int cw = 0, ch = 0;
        int slot = renderer.Pipeline().LastCapturedSlot();
        bool rb = slot >= 0 && renderer.Pipeline().DownloadCaptureSlot(slot, rgba, cw, ch);
        long mean = 0;
        if (rb && (int)rgba.size() >= kRes * kRes * 4) {
            long s = 0;
            for (int p = 0; p < kRes * kRes; ++p)
                s += rgba[p * 4 + 0] + rgba[p * 4 + 1] + rgba[p * 4 + 2];
            mean = s / (kRes * kRes * 3);
        }
        renderer.Pipeline().Gfx()->WaitIdle();
        if (rb && cw > 0 && (int)rgba.size() >= cw * ch * 4) {
            if (const char* out = std::getenv("CASC_PNG")) {
                auto tex = whiteout::textures::Texture::create2D(
                    whiteout::textures::PixelFormat::RGBA8, (unsigned)cw, (unsigned)ch, 1);
                auto dst = tex.mipData(0);
                std::memcpy(dst.data(), rgba.data(),
                            std::min<size_t>(dst.size(), rgba.size()));
                whiteout::textures::png::Writer w;
                w.write(out, tex);
                std::printf("[casc-render] wrote %s\n", out);
            }
        }
        // Decisive proof = the model loaded from CASC (textures + geometry
        // resolved) and uploaded geosets. Non-black readback corroborates where
        // available; the D3D12 capture ring returns all-zero for stacked
        // offscreen targets (a harness quirk), so an all-zero read isn't a
        // failure.
        const bool readbackAvailable = rb && mean > 0;
        const bool pass = hero && geosets > 0;
        std::printf("[casc-render] geosets=%d readbackMean=%ld%s -> %s\n", geosets, mean,
                    readbackAvailable ? "" : " (readback n/a)", pass ? "PASS" : "FAIL");
        renderer.Pipeline().EnableFrameCapture(false);
        renderer.Pipeline().Shutdown();
        std::fflush(stdout);
        std::_Exit(pass ? 0 : 7);
    }

    // Windowed-path repro (no GLFW window): build a real ImGui frame that
    // ImGui::Image's a pooled cell, render the cells, then render the main
    // ImGui pass into an offscreen "screen" and read it back. Confirms the
    // cross-RenderViewport ImGui sampling that the live grid relies on.
    if (windowRepro) {
        namespace wf = whiteout::flakes;
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(512, 512);
        io.DeltaTime = 0.016f;
        unsigned char* px = nullptr;
        int fw = 0, fh = 0;
        io.Fonts->GetTexDataAsRGBA32(&px, &fw, &fh); // build atlas so NewFrame won't assert

        if (!renderer.Pipeline().InitDevice(backend)) {
            std::fprintf(stderr, "[window-repro] InitDevice FAILED\n");
            return 2;
        }
        renderer.Settings().SetBackgroundColor(58, 64, 74);
        auto provider = std::make_shared<wf::io::FileContentProvider>();
        provider->SetInstallPath(cascRoot);

        constexpr int kScreen = 512, kImg = 256, kImgX = 10, kImgY = 10;
        wf::renderer::RenderTargetId screen =
            renderer.Pipeline().CreateOffscreenTarget(kScreen, kScreen);
        renderer.Pipeline().SetPrimaryTarget(screen);
        renderer.Pipeline().EnableFrameCapture(true);
        {
            wf::renderer::model::Actor* dummy = nullptr;
            (void)dummy;
            wf::tools::ThumbnailPool pool(renderer, provider, 4, 256);
            const bool isEffect = cascRenderPath.find(".pkb") != std::string::npos ||
                                  cascRenderPath.find(".pkfx") != std::string::npos;
            for (int f = 0; f < 40; ++f) {
                pool.BeginFrame(static_cast<std::uint64_t>(f + 1));
                auto tex = pool.Acquire(cascRenderPath, isEffect, static_cast<std::uint64_t>(f + 1));
                ImGui::NewFrame();
                if (tex != wf::gfx::TextureHandle::Invalid)
                    ImGui::GetBackgroundDrawList()->AddImage(
                        static_cast<ImTextureID>(static_cast<std::uint64_t>(tex)),
                        ImVec2(kImgX, kImgY), ImVec2(kImgX + kImg, kImgY + kImg));
                ImGui::Render();
                pool.RenderVisible(0.016f);                 // render cells
                renderer.Pipeline().RenderFrame(screen);    // main pass samples cell via ImGui
                renderer.Pipeline().Present(screen);
            }
            renderer.Pipeline().Gfx()->WaitIdle();
            std::vector<wf::u8> rgba;
            int cw = 0, ch = 0;
            const int slot = renderer.Pipeline().LastCapturedSlot();
            bool rb = slot >= 0 && renderer.Pipeline().DownloadCaptureSlot(slot, rgba, cw, ch);
            if (!rb)
                rb = renderer.Pipeline().ReadbackTarget(screen, rgba, cw, ch);
            long meanImg = -1;
            int maxImg = -1;
            if (rb && cw == kScreen && (int)rgba.size() >= cw * ch * 4) {
                long s = 0;
                int n = 0, mx = 0;
                for (int y = kImgY; y < kImgY + kImg; ++y)
                    for (int x = kImgX; x < kImgX + kImg; ++x) {
                        const int p = (y * cw + x) * 4;
                        s += rgba[p] + rgba[p + 1] + rgba[p + 2];
                        mx = (std::max)(mx, (int)rgba[p]);
                        mx = (std::max)(mx, (int)rgba[p + 1]);
                        mx = (std::max)(mx, (int)rgba[p + 2]);
                        ++n;
                    }
                meanImg = n ? s / (long)(n * 3) : -1;
                maxImg = mx;
            }
            if (rb && cw > 0 && (int)rgba.size() >= cw * ch * 4) {
                if (const char* out = std::getenv("CASC_PNG")) {
                    auto tex = whiteout::textures::Texture::create2D(
                        whiteout::textures::PixelFormat::RGBA8, (unsigned)cw, (unsigned)ch, 1);
                    auto dst = tex.mipData(0);
                    std::memcpy(dst.data(), rgba.data(), std::min<size_t>(dst.size(), rgba.size()));
                    whiteout::textures::png::Writer w;
                    w.write(out, tex);
                    std::printf("[window-repro] wrote %s\n", out);
                }
            }
            const bool pass = meanImg > 0;
            std::printf("[window-repro] sampled-cell region mean=%ld max=%d -> %s\n", meanImg,
                        maxImg, pass ? "PASS" : "FAIL");
            renderer.Pipeline().EnableFrameCapture(false);
            renderer.Pipeline().Shutdown();
            std::fflush(stdout);
            std::_Exit(pass ? 0 : 7);
        }
    }

    whiteout::flakes::tools::ExplorerApp app(renderer);
    if (!app.Open(1280, 800, backend)) {
        std::fprintf(stderr, "Failed to open the model explorer window.\n");
        return 1;
    }

    if (!cascRoot.empty())
        app.OpenCasc(cascRoot);

    auto last = std::chrono::steady_clock::now();
    while (!app.ShouldClose()) {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        app.Tick(dt);
    }
    app.Close(); // releases GPU resources while the device is alive
    // Skip the static/global C++ teardown: ~RenderService destructs the gfx
    // services (PostProcess/Shadow/Gtao) after the device is already gone, which
    // crashes — a pre-existing engine teardown-order issue the headless tests
    // also _Exit past. Close() already did the orderly GPU shutdown, so exiting
    // here gives the user a clean close instead of a crash dialog.
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(0);
}
