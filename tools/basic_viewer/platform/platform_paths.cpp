#include "platform/platform_paths.h"

#include "gfx/gfx.h"
#include "renderer/render_settings.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
// clang-format off
#include <windows.h>      // must precede shellapi.h — it defines the types it uses
#include <shellapi.h>     // CommandLineToArgvW
// clang-format on
#endif

namespace whiteout::flakes::platform {

namespace fs = std::filesystem;

std::vector<std::string> Utf8Arguments(int argc, char* argv[]) {
    std::vector<std::string> args;
#if defined(_WIN32)
    int wArgc = 0;
    if (LPWSTR* wArgv = ::CommandLineToArgvW(::GetCommandLineW(), &wArgc)) {
        for (int i = 1; i < wArgc; ++i)
            args.push_back(io::PathToUtf8(fs::path(wArgv[i])));
        ::LocalFree(wArgv);
        return args;
    }
#endif
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);
    return args;
}

void ConfigureVulkanEnvironment() {
#if defined(__APPLE__)
    // .app-bundled MoltenVK: the loader's ICD discovery does not walk into
    // Contents/Resources by default. The bundle the macOS CI produces:
    //   WhiteoutFlakes.app/Contents/MacOS/WhiteoutFlakes
    //   WhiteoutFlakes.app/Contents/Resources/vulkan/icd.d/MoltenVK_icd.json
    //   WhiteoutFlakes.app/Contents/Frameworks/libMoltenVK.dylib
    // The JSON's library_path is relative to its own directory, so the dylib
    // resolves from inside the bundle without a system MoltenVK.
    {
        // ExecutableDirectory already answers Contents/Resources inside a bundle.
        fs::path dir = io::ExecutableDirectory();
        if (!dir.empty()) {
            if (dir.filename() != "Resources")
                dir = dir.parent_path() / "Resources";
            fs::path icd = dir / "vulkan" / "icd.d" / "MoltenVK_icd.json";
            if (fs::exists(icd))
                ::setenv("VK_ICD_FILENAMES", icd.c_str(), 1);
        }
    }
    // Metal argument buffers: off by default in MoltenVK because old Metal
    // drivers had bugs, safe and much faster on Apple Silicon, the only macOS
    // target. Read during ICD initialisation, hence before any Vulkan call.
    ::setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "1", 1);
#endif
}

gfx::GfxApi CoerceBackendToPlatform(gfx::GfxApi backend, renderer::RenderSettings& settings) {
#if defined(__linux__)
    // D3D11/D3D12 are WIN32-only and Dawn is not built into Linux binaries.
    if (backend != gfx::GfxApi::Vulkan) {
        std::cerr << "[viewer] Forcing Vulkan backend (only one available on this platform)\n";
        backend = gfx::GfxApi::Vulkan;
    }
    if (settings.DefaultBackend() != gfx::GfxApi::Vulkan)
        settings.SetDefaultBackend(gfx::GfxApi::Vulkan);
#elif defined(__APPLE__)
    // Vulkan through MoltenVK, Metal with WDX_HAS_METAL, WebGPU through Dawn
    // with WDX_HAS_WEBGPU. D3D11/D3D12 only build on Windows.
    auto isMacOk = [](gfx::GfxApi a) {
        if (a == gfx::GfxApi::Vulkan)
            return true;
#if WDX_HAS_WEBGPU
        if (a == gfx::GfxApi::WebGPU)
            return true;
#endif
#if WDX_HAS_METAL
        if (a == gfx::GfxApi::Metal)
            return true;
#endif
        return false;
    };
    if (!isMacOk(backend)) {
        std::cerr << "[viewer] Backend not supported on macOS; falling back to Vulkan\n";
        backend = gfx::GfxApi::Vulkan;
    }
    if (!isMacOk(settings.DefaultBackend()))
        settings.SetDefaultBackend(gfx::GfxApi::Vulkan);
#else
    (void)settings;
#endif
    return backend;
}

void ConfigurePipelineCache() {
    fs::path cachePath;
#if defined(_WIN32)
    cachePath = io::ExecutableDirectory() / "vk_pipeline_cache.bin";
#else
    fs::path base;
#if defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home)
        base = fs::path(home) / "Library" / "Caches";
#else
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        base = xdg;
    else if (const char* home = std::getenv("HOME"); home && *home)
        base = fs::path(home) / ".cache";
#endif
    else
        base = ".";
    std::error_code ec;
    fs::create_directories(base / "WhiteoutFlakes", ec);
    cachePath = base / "WhiteoutFlakes" / "vk_pipeline_cache.bin";
#endif

    // The engine reads the PSO trace from beside the pipeline cache. Shipping a
    // pre-warmed one cuts the cold-launch PSO build hitch on Vulkan; later runs
    // append to the user copy (BlsPsoTrace::Save). Inside a macOS bundle the
    // shipped copy is in Contents/Resources, which ExecutableDirectory answers.
    {
        fs::path tracePath = cachePath;
        tracePath.replace_filename("pso_trace.bin");
        std::error_code ec;
        if (!fs::exists(tracePath, ec)) {
            const fs::path shippedDir = io::ExecutableDirectory();
            if (!shippedDir.empty()) {
                const fs::path shipped = shippedDir / "pso_trace.bin";
                if (fs::exists(shipped, ec))
                    fs::copy_file(shipped, tracePath, fs::copy_options::skip_existing, ec);
            }
        }
    }

    const std::string u8 = io::PathToUtf8(cachePath);
    gfx::SetPipelineCachePath(u8.c_str());
}

} // namespace whiteout::flakes::platform
