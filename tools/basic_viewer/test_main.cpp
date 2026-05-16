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

#include <cstdlib>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#include <climits>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <climits>
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

int main(int argc, char* argv[]) {
    whiteout::flakes::gfx::GfxApi backend = whiteout::flakes::gfx::GfxApi::Vulkan;
#if defined(_WIN32)
    // Windows historically defaults to D3D12; Linux only has Vulkan so the
    // default is fixed above.
    backend = whiteout::flakes::gfx::GfxApi::D3D12;
#endif
    bool backendFromCli = false;
    std::filesystem::path mdxPath;

#if defined(_WIN32)
    constexpr const char* kBackendsHelp = "d3d11, d3d12, vulkan";
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
        } else if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
            std::cout << "Usage: WhiteoutFlakes [--backend " << kBackendsHelp
                      << "] [<mdx-path>]\n";
            return 0;
        } else if (mdxPath.empty()) {
            mdxPath = whiteout::flakes::io::FsPathFromUtf8(a);
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
            std::filesystem::path icd =
                exe.parent_path().parent_path() /
                "Resources" / "vulkan" / "icd.d" / "MoltenVK_icd.json";
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

#if !defined(_WIN32)
    // Defence-in-depth: if a stale INI carries DefaultBackend=d3d11 from a
    // shared-config user, normalise the in-memory Settings value too so
    // SaveSettingsIni doesn't propagate the bad choice forward and so the
    // ImGui Settings combo (which reads Settings().DefaultBackend()) shows
    // Vulkan correctly. The Linux-only ImGui Backend combo is restricted
    // to Vulkan anyway, see viewer_ui.cpp.
    if (backend != whiteout::flakes::gfx::GfxApi::Vulkan) {
        std::cerr << "[viewer] Forcing Vulkan backend (only one available on this platform)\n";
        backend = whiteout::flakes::gfx::GfxApi::Vulkan;
    }
    if (renderer.Settings().DefaultBackend() != whiteout::flakes::gfx::GfxApi::Vulkan)
        renderer.Settings().SetDefaultBackend(whiteout::flakes::gfx::GfxApi::Vulkan);
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
                        std::filesystem::copy_file(shipped, tracePath,
                                                   std::filesystem::copy_options::skip_existing,
                                                   ec);
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
                                                                                 : "?";
    std::cout << "Backend: " << backendName << "\n";

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
    renderer.SwapSoundEmitter(std::make_unique<whiteout::flakes::CubebSoundEmitter>(
        scene.ActiveContentProvider()));

    // Persistent settings (display flags, exposure, tileset, etc.) applied
    // after the device + asset managers are up so they can validate
    // dependent state (e.g. ShadowService cascade count).
    bool loopPolicy = app.LoopNonLoopingPolicy();
    whiteout::flakes::LoadSettingsIni(renderer, loopPolicy);
    app.SetLoopNonLoopingPolicy(loopPolicy);

    // CLI path wins; otherwise pop NFD once at startup. Re-openable via
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
