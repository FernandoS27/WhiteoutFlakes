#include "gfx/gfx.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "settings_ini.h"
#include "viewer_app.h"
#include "whiteout/flakes/gfx_types.h"
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/path_utf8.h"
#include "cubeb_sound_emitter.h"

#include <nfd.hpp>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using whiteout::flakes::f32;
using whiteout::flakes::i32;

int wmain(int argc, wchar_t* argv[]) {
    whiteout::flakes::gfx::GfxApi backend = whiteout::flakes::gfx::GfxApi::D3D12;
    bool backendFromCli = false;
    std::filesystem::path mdxPath;

    for (i32 i = 1; i < argc; ++i) {
        const wchar_t* a = argv[i];
        if ((std::wcscmp(a, L"--backend") == 0 || std::wcscmp(a, L"-b") == 0) && i + 1 < argc) {
            const wchar_t* v = argv[++i];
            if (_wcsicmp(v, L"d3d11") == 0 || _wcsicmp(v, L"dx11") == 0)
                backend = whiteout::flakes::gfx::GfxApi::D3D11;
            else if (_wcsicmp(v, L"d3d12") == 0 || _wcsicmp(v, L"dx12") == 0)
                backend = whiteout::flakes::gfx::GfxApi::D3D12;
            else if (_wcsicmp(v, L"vulkan") == 0 || _wcsicmp(v, L"vk") == 0)
                backend = whiteout::flakes::gfx::GfxApi::Vulkan;
            else {
                std::wcerr << L"Unknown backend: " << v << L" (valid: d3d11, d3d12, vulkan)\n";
                return 1;
            }
            backendFromCli = true;
        } else if (std::wcscmp(a, L"--help") == 0 || std::wcscmp(a, L"-h") == 0) {
            std::cout << "Usage: WhiteoutFlakes.exe [--backend d3d11|d3d12|vulkan] [<mdx-path>]\n";
            return 0;
        } else if (mdxPath.empty()) {
            mdxPath = a;
        }
    }

    whiteout::flakes::renderer::SceneManager scene;
    whiteout::flakes::renderer::RenderService renderer(scene);

    // Startup-only settings (validation layer, default backend, preferred
    // device) must land on RenderSettings before gfx::CreateDevice runs.
    whiteout::flakes::LoadStartupSettingsFromIni(renderer);
    if (!backendFromCli)
        backend = renderer.Settings().DefaultBackend();

    // Tell the gfx layer where to persist the Vulkan pipeline cache.
#if defined(_WIN32)
    {
        wchar_t exe[MAX_PATH] = {};
        DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::filesystem::path p(exe);
            p.replace_filename(L"vk_pipeline_cache.bin");
            const std::string u8 = whiteout::flakes::io::PathToUtf8(p);
            whiteout::flakes::gfx::SetPipelineCachePath(u8.c_str());
        }
    }
#else
    whiteout::flakes::gfx::SetPipelineCachePath("vk_pipeline_cache.bin");
#endif

    const char* backendName = backend == whiteout::flakes::gfx::GfxApi::D3D11    ? "D3D11"
                              : backend == whiteout::flakes::gfx::GfxApi::D3D12  ? "D3D12"
                              : backend == whiteout::flakes::gfx::GfxApi::Vulkan ? "Vulkan"
                                                                                 : "?";
    std::cout << "Backend: " << backendName << "\n";

    whiteout::flakes::ViewerApp app(renderer);
    if (!app.Open(1024, 768, backend)) {
        std::cerr << "Failed to open viewer\n";
        return 1;
    }

    // Persistent settings (display flags, exposure, tileset, etc.) are
    // applied after the device + asset managers are up so they can validate
    // dependent state (e.g. ShadowService cascade count).
    bool loopPolicy = app.LoopNonLoopingPolicy();
    whiteout::flakes::LoadSettingsIni(renderer, loopPolicy);
    app.SetLoopNonLoopingPolicy(loopPolicy);

    renderer.SwapSoundEmitter(std::make_unique<whiteout::flakes::CubebSoundEmitter>(
        scene.ActiveContentProvider()));

    // If a path came in on the CLI, load it; otherwise pop the NFD picker
    // once at startup (matches the old Win32 viewer's GetOpenFileNameW
    // behaviour). The user can also reopen the picker any time via
    // File > Open in the menu bar.
    if (mdxPath.empty()) {
        NFD::Init();
        NFD::UniquePathU8 outPath;
        nfdu8filteritem_t filter[1] = {{"MDX Model", "mdx"}};
        if (NFD::OpenDialog(outPath, filter, 1) == NFD_OKAY)
            mdxPath = whiteout::flakes::io::FsPathFromUtf8(outPath.get());
    }
    if (!mdxPath.empty()) {
        if (!std::filesystem::exists(mdxPath)) {
            std::cerr << "File not found: " << whiteout::flakes::io::PathToUtf8(mdxPath) << "\n";
        } else if (!app.LoadModel(mdxPath)) {
            std::cerr << "Failed to load MDX.\n";
        }
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
    return 0;
}
