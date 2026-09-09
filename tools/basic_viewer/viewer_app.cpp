#include "viewer_app.h"

#include "io/mdx_model_adapter.h"
#include "io/wem/wem_export.h"
#include "io/wem/wem_import.h"
#include "io/wem/wem_profiles.h"
#include "gltf_export.h"
#include "m3_export.h"

#include "whiteout/flakes/util/replaceable_paths.h"
#if WDX_ENABLE_M3
#include "m3_save.h"
#endif
#include "mdx_export.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/camera.h"
#include "renderer/debug/debug_renderer.h"
#include "renderer/frame_ticker.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_loader.h"
#if WDX_ENABLE_M3
#include "renderer/profiles/sc2_heroes/sc2_model_catalog.h"
#endif
#include "renderer/model/model_template.h"
#if WDX_ENABLE_M3
#include "io/m3/m3_model_adapter.h"
#endif
#if WDX_ENABLE_D3
#include "io/d3/d3_model_adapter.h"
#include "renderer/profiles/diablo3/d3_character_appearance.h"
#endif
#include "renderer/model/corn_effect_source.h"
#include "renderer/particle/splat_service.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#if WDX_ENABLE_M2
#include "renderer/profiles/wow/wow_character_appearance.h"
#include "renderer/profiles/wow/wow_replaceable_textures.h"
#endif
#include "renderer/viewport.h"
#include "storage_explorer.h"
#if defined(_WIN32)
#include "resource.h" // IDI_WHITEOUT_ICON
#endif
#include "imgui_theme.h"
#include "ini_file.h"
#include "io/file_content_provider.h"
#include "localization.h"
#include "settings_ini.h"
#include "storage_explorer_ini.h"
#include "thumbnail_framing.h"
#include "viewer_ui.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/event_data.h"
#include "whiteout/flakes/util/path_utf8.h"

#include "gfx/gfx.h"

#include <whiteout/models/mdx/mdx.h>
#include <whiteout/textures/png/writer.h>
#include <whiteout/textures/texture.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>

// We use glfwCreateWindowSurface (cross-platform) for the Vulkan backend.
// On Windows, the D3D backends still want a raw HWND, so we pull in
// glfw3native there. Including <vulkan/vulkan.h> *before* <GLFW/glfw3.h>
// makes glfwCreateWindowSurface visible without GLFW pulling in its own
// vulkan header copy.
#include <vulkan/vulkan.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#if defined(__APPLE__)
// MoltenVK 1.4 + GLFW 3.4: glfwCreateWindowSurface sets the contentView's
// layer before flipping wantsLayer=YES, which leaves the CAMetalLayer
// un-installed on macOS 13+ and trips vkCreateMetalSurfaceEXT into
// VK_ERROR_INITIALIZATION_FAILED. Bypass it with our own shim — pulls in
// glfw3native here so we can hand the NSWindow* over to the .mm file.
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
namespace whiteout::flakes {
VkResult CreateVulkanSurfaceMacOS(VkInstance instance, void* nsWindow, VkSurfaceKHR* outSurface);
void SetCocoaWindowChrome(void* nsWindow, float r, float g, float b);
} // namespace whiteout::flakes
#endif
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
// Newer DWM attributes — define locally so we don't depend on the SDK version.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
// Per-monitor V2 DPI context — declared in Win10 1607+ SDKs. Older SDKs need
// the cast-from-int fallback (same trick the SDK header itself uses).
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT) - 4)
#endif
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace whiteout::flakes {

namespace {

const char* kWindowTitle = "WhiteoutFlakes";

// For the progress modal's title. The same names the Settings profile combo
// uses, kept here rather than reached for across viewer_ui.cpp because a
// window title is not a settings concern.
const char* GameDisplayName(ProductId game) {
    switch (game) {
    case ProductId::Wow:
        return "World of Warcraft";
    case ProductId::Sc2:
        return "StarCraft II / Storm";
    case ProductId::D3:
        return "Diablo III";
    default:
        return "Warcraft III";
    }
}

bool ContainsCi(const std::string& hay, const char* needle) {
    const usize hn = hay.size();
    const usize nn = std::strlen(needle);
    if (nn == 0 || hn < nn)
        return false;
    for (usize i = 0; i + nn <= hn; ++i) {
        bool ok = true;
        for (usize j = 0; j < nn; ++j) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(hay[i + j])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(needle[j])));
            if (a != b) {
                ok = false;
                break;
            }
        }
        if (ok)
            return true;
    }
    return false;
}

} // namespace

ViewerApp::ViewerApp(RenderService& service) : service_(service) {
    // The game the user was last working with. Reading it is an ini read and
    // nothing more — which product the provider ends up serving is decided by
    // test_main applying the same value, and by content loaded later.
    settingsProfile_ = LoadIoProduct();
    ui_ = std::make_unique<ViewerUI>(*this);
}

void ViewerApp::ApplyProfile(ProductId game, bool force) {
    // D3's HDR frame buffer + tonemap is intrinsic to the Diablo3Profile now,
    // not a host setting we flip here — flipping it on a profile change was
    // unreliable (a change is not the only time a D3 model is on screen) and
    // forcing the shared SceneHdrInSd flag off for other games stomped a WC3
    // user's own opt-in.
    auto& provider = service_.DefaultScene().GetContentProvider();
    const auto idx = static_cast<usize>(game);
    const bool first = idx >= ioProfileApplied_.size() || !ioProfileApplied_[idx];
    if (!force && !first) {
        // The slot already holds this profile's settings — and whatever the
        // session added to them, like the keys AdoptNearbyWowKeys found beside
        // a model. Re-applying the ini would write its empty listfile back over
        // that, and an ini write that changes a value invalidates the slot: the
        // provider drops the last reference to the install and the registry
        // holds it only by weak_ptr, so it is DESTROYED and the next read
        // re-parses indices, manifest and a 144 MB listfile. Switching between
        // two open models is supposed to be a pointer move, which is the whole
        // point of the per-product slots.
        provider.SetGame(game);
        return;
    }
    if (game == ProductId::Wow)
        wowTablesPrewarmed_ = false; // different install, different tables
    if (game == ProductId::Sc2)
        sc2CatalogPrewarmed_ = false; // different install, different catalog
    if (game == ProductId::D3 && d3ItemsBuild_ != D3ItemsBuild::Building) {
        // Same idea for the item registry — but never mid-build: during
        // Building the registry belongs to the task thread. Skipping the
        // clear then can leave items from the previous install standing;
        // that beats a data race, and the next profile apply clears them.
#if WDX_ENABLE_D3
        service_.Loader().D3Items().Clear();
#endif
        d3ItemsBuild_ = D3ItemsBuild::NotStarted;
    }
    ApplyIoPathOverrides(provider, game);
    if (idx < ioProfileApplied_.size())
        ioProfileApplied_[idx] = true;
}

void ViewerApp::RunStorageOpenTask(io::FileContentProvider& provider,
                                   std::function<void(bool ok)> onDone) {
    // Already open, or already failed and not worth retrying: answer now rather
    // than flashing a modal for a task with nothing to do.
    const io::StorageState state = provider.StoragesState();
    if (state == io::StorageState::Open || state == io::StorageState::Failed) {
        if (onDone)
            onDone(state == io::StorageState::Open);
        return;
    }
    io::FileContentProvider* p = &provider;

    if (state == io::StorageState::Opening) {
        // A read on a provider worker already started one. We cannot report its
        // progress — the monitor driving it belongs to that call — but waiting
        // behind an indeterminate bar beats returning "not open" and letting
        // the caller block the host thread on a read that is about to do this
        // same wait invisibly.
        tasks_.Run(
            std::string("Opening ") + GameDisplayName(provider.Game()),
            [p](io::ProgressMonitor& m) {
                m.Begin("Waiting for storage"); // no total: not our open to count
                while (p->StoragesState() == io::StorageState::Opening) {
                    if (m.Cancelled())
                        return io::TaskResult::Fail("Cancelled");
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
                return io::TaskResult::Ok();
            },
            [p, onDone = std::move(onDone)](const io::TaskOutcome&) {
                if (onDone)
                    onDone(p->StoragesState() == io::StorageState::Open);
            },
            // Not cancellable, and saying so rather than offering a button that
            // does nothing: the open belongs to another caller, and stopping
            // this wait would not stop it.
            /*cancellable=*/false);
        return;
    }

    tasks_.Run(
        std::string("Opening ") + GameDisplayName(provider.Game()),
        [p](io::ProgressMonitor& m) {
            // OpenStorages is the demanded path, so it retries a previously
            // cancelled open - which is the whole point of the user asking
            // again after pressing Cancel.
            return p->OpenStorages(&m) ? io::TaskResult::Ok()
                                       : io::TaskResult::Fail("No storage opened");
        },
        [onDone = std::move(onDone)](const io::TaskOutcome& out) {
            if (onDone)
                onDone(out.ok);
        });
}

void ViewerApp::OpenStoragesAsync(std::function<void(bool ok)> onDone) {
    RunStorageOpenTask(service_.DefaultScene().GetContentProvider(),
                       [this, onDone = std::move(onDone)](bool ok) {
                           // Assets that missed while nothing was open get another chance now
                           // that something is. Cheap when nothing missed.
                           if (ok) {
                               service_.RetryUnloadedAssets();
                               // Chained rather than kicked in parallel: both want the task
                               // thread, and the tables cannot be read before the storage
                               // that holds them is up.
                               PrewarmWowTablesAsync();
                               // At most one of the two does anything — they are
                               // different products.
                               PrewarmSc2CatalogAsync();
                           }
                           if (onDone)
                               onDone(ok);
                       });
}

void ViewerApp::PrewarmWowTablesAsync() {
    io::FileContentProvider& provider = service_.DefaultScene().GetContentProvider();
    if (provider.Game() != ProductId::Wow)
        return;
    // Only once the storage is actually up. Kicking this against a pending
    // storage would have the task thread trigger the open itself, silently,
    // behind a bar that claims to be reading databases.
    if (provider.StoragesState() != io::StorageState::Open)
        return;
    auto& replaceables = service_.Loader().WowReplaceables();
    auto& characters = service_.Loader().WowCharacters();
    if (replaceables.Table().Loaded() && characters.Tables().Loaded())
        return;

    replaceables.SetContentProvider(&provider);
    characters.SetContentProvider(&provider);
    tasks_.Run(
        "Reading client databases",
        [&replaceables, &characters](io::ProgressMonitor& m) {
            // Character customisation is fourteen tables against the skin
            // tables' four, and is the one that actually hurts.
            m.Begin("Client databases", 4);
            {
                io::ProgressMonitor step = m.Split(3);
                characters.Prewarm(&step);
            }
            if (m.Cancelled())
                return io::TaskResult::Fail("Cancelled");
            io::ProgressMonitor step = m.Split(1);
            replaceables.Prewarm(&step);
            // Deliberately always Ok: an install that cannot serve these tables
            // is a normal state (no listfile, no keys, a classic client), and
            // reporting it as a failed operation would put an error box in
            // front of a user who asked for nothing.
            return io::TaskResult::Ok();
        },
        [this](const io::TaskOutcome& out) {
            if (!out.cancelled)
                wowTablesPrewarmed_ = true;
            if (!out.ok)
                return;
            // Re-apply to what is already loaded. RestyleWowModel runs exactly
            // the pair the tables feed and re-stages the textures, which is why
            // publishing on completion costs nothing new here.
            RestyleLoadedWowModels();
        },
        /*cancellable=*/true,
        // NOT modal. Nobody asked for these tables; a model spawned before they
        // arrive simply shows its default look and is restyled above. Taking
        // the screen for that would be a worse trade than the wait it replaces.
        /*modal=*/false);
}

void ViewerApp::PrewarmSc2CatalogAsync() {
#if WDX_ENABLE_M3
    io::FileContentProvider& provider = service_.DefaultScene().GetContentProvider();
    if (provider.Game() != ProductId::Sc2 || sc2CatalogPrewarmed_)
        return;
    // Same rule as the World of Warcraft tables: only once the storage is
    // actually up, or the task thread triggers the open itself behind a bar
    // that claims to be reading a catalog.
    if (provider.StoragesState() != io::StorageState::Open)
        return;
    auto& catalog = service_.Loader().Sc2Catalog();
    if (catalog.Loaded())
        return;

    catalog.SetContentProvider(&provider);
    tasks_.Run(
        "Reading the model catalog",
        [&catalog](io::ProgressMonitor& m) {
            catalog.Prewarm(&m);
            // Always Ok, for the reason the World of Warcraft one is: an
            // install with no GameData in it is a normal state, not a failed
            // operation to put a box in front of.
            return io::TaskResult::Ok();
        },
        [this](const io::TaskOutcome& out) {
            if (!out.cancelled)
                sc2CatalogPrewarmed_ = true;
        },
        /*cancellable=*/true,
        // NOT modal, and nothing to re-apply on completion. A model that loaded
        // first built the index inside its own load and already has its
        // animations; this only spares the ones after it.
        /*modal=*/false);
#endif
}

void ViewerApp::SetSettingsProfile(ProductId game) {
    if (settingsProfile_ == game)
        return;
    settingsProfile_ = game;
    // Persisted so the next launch comes back here, and applied to nothing:
    // a profile's storage opens when that profile is needed, not when its
    // settings page is opened.
    SaveIoProduct(game);
}

ViewerApp::~ViewerApp() {
    Close();
}

bool ViewerApp::Open(i32 width, i32 height, gfx::GfxApi api, bool visible) {
    backend_ = api;

#if defined(_WIN32)
    // Opt into per-monitor V2 awareness before any HWND is created. Without
    // this, Windows bitmap-stretches the whole window on non-100% displays
    // (4K / scaled laptop panels), which is what makes ImGui glyphs look
    // soft and wavy. SetProcessDpiAwarenessContext exists on Windows 10
    // 1703+; pre-Win10 falls back through the older SetProcessDpiAware.
    {
        using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
        auto setCtx =
            u32 ? reinterpret_cast<SetCtxFn>(::GetProcAddress(u32, "SetProcessDpiAwarenessContext"))
                : nullptr;
        if (!setCtx || !setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            ::SetProcessDPIAware();
    }
#endif

    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit FAILED\n");
        return false;
    }

    // No OpenGL context — we drive the swap chain through the engine's gfx
    // layer, which talks directly to d3d11 / d3d12 / vulkan.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);

    // The caller's width/height are logical (96-DPI) pixels — pre-scale them
    // for the primary monitor so a 1280x720 viewer doesn't shrink to a quarter
    // of the screen on a 4K 200% display now that we're DPI-aware.
    float initialDpiScale = 1.0f;
    if (GLFWmonitor* primary = glfwGetPrimaryMonitor()) {
        float xs = 1.0f;
        float ys = 1.0f;
        glfwGetMonitorContentScale(primary, &xs, &ys);
        if (xs > 0.0f)
            initialDpiScale = xs;
    }
    const i32 scaledW = static_cast<i32>(static_cast<f32>(width) * initialDpiScale);
    const i32 scaledH = static_cast<i32>(static_cast<f32>(height) * initialDpiScale);

    window_ = glfwCreateWindow(scaledW, scaledH, kWindowTitle, nullptr, nullptr);
    if (!window_) {
        std::fprintf(stderr, "glfwCreateWindow FAILED\n");
        glfwTerminate();
        return false;
    }
    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, &ViewerApp::FramebufferSizeCallback);
    glfwSetWindowRefreshCallback(window_, &ViewerApp::WindowRefreshCallback);
    glfwSetMouseButtonCallback(window_, &ViewerApp::MouseButtonCallback);
    glfwSetCursorPosCallback(window_, &ViewerApp::CursorPosCallback);
    glfwSetScrollCallback(window_, &ViewerApp::ScrollCallback);

#if defined(_WIN32)
    // GLFW's cross-platform glfwSetWindowIcon takes RGBA pixels — we'd need
    // a decoder to feed it an .ico. On Windows the embedded Win32 resource
    // is already in the right format for WM_SETICON, so we use it directly.
    // On Linux the window manager picks a default icon for now.
    {
        HWND hwnd = glfwGetWin32Window(window_);
        HMODULE hMod = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                             reinterpret_cast<LPCWSTR>(&ViewerApp::FramebufferSizeCallback), &hMod);
        HINSTANCE hInst = hMod ? reinterpret_cast<HINSTANCE>(hMod) : ::GetModuleHandle(nullptr);
        HICON hIcon = ::LoadIconW(hInst, MAKEINTRESOURCEW(IDI_WHITEOUT_ICON));
        if (hIcon) {
            ::SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
            ::SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
        }
        // Match the title bar + thin window border to the ImGui MenuBarBg so
        // the OS chrome blends with the menu strip below it. Silently ignored
        // on older Windows.
        const BOOL useDark = TRUE;
        const COLORREF chrome = RGB(38, 45, 56);
        ::DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
        ::DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &chrome, sizeof(chrome));
        ::DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &chrome, sizeof(chrome));
    }
#elif defined(__APPLE__)
    // Tint the NSWindow's title bar to the same ImGui MenuBarBg chrome the
    // Windows DWM path uses (RGB(38,45,56) = ImVec4(38,45,56)/255 — see
    // tools/common/imgui_theme.cpp). Implementation lives in
    // cocoa_window_macos.mm; we hand it the GLFW NSWindow via glfw3native.
    SetCocoaWindowChrome(glfwGetCocoaWindow(window_), 38.0f / 255.0f, 45.0f / 255.0f,
                         56.0f / 255.0f);
#endif

    // ImGui context must exist *before* Pipeline.InitDevice() because
    // InitBlsShaders calls RenderService::EnsureImGui, which constructs
    // ImGuiRenderer and (on the first Render() call) queries io.Fonts.
    InitImGui();

    if (!service_.Pipeline().InitDevice(api)) {
        std::fprintf(stderr, "Pipeline().InitDevice FAILED\n");
        Close();
        return false;
    }

    i32 fbW = width;
    i32 fbH = height;
    glfwGetFramebufferSize(window_, &fbW, &fbH);
    if (fbW <= 0)
        fbW = width;
    if (fbH <= 0)
        fbH = height;

    // Build the swap-chain handle the gfx layer expects per backend.
    //   • d3d11/d3d12: HWND
    //   • vulkan on Windows: HWND (gfx creates the Win32 surface internally)
    //   • vulkan on Linux: a pre-built VkSurfaceKHR from glfwCreateWindowSurface
    //     (so gfx doesn't have to branch on xcb / xlib / wayland itself)
    //   • webgpu on non-Windows: the GLFWwindow* itself — the WebGPU
    //     backend pulls the platform-specific handles (Display+Window /
    //     wl_display+wl_surface / NSWindow) via glfw3native.h, so the
    //     viewer doesn't have to duplicate the X11/Wayland/Cocoa branches.
    void* swapHandle = nullptr;
#if !defined(_WIN32)
    if (api == gfx::GfxApi::Vulkan) {
        gfx::IGFXDevice* dev = service_.Pipeline().Gfx();
        VkInstance instance = dev ? static_cast<VkInstance>(dev->GetNativeInstance()) : nullptr;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
#if defined(__APPLE__)
        VkResult sr =
            instance ? CreateVulkanSurfaceMacOS(instance, glfwGetCocoaWindow(window_), &surface)
                     : VK_ERROR_INITIALIZATION_FAILED;
#else
        VkResult sr = instance ? glfwCreateWindowSurface(instance, window_, nullptr, &surface)
                               : VK_ERROR_INITIALIZATION_FAILED;
#endif
        if (!instance || sr != VK_SUCCESS) {
            std::fprintf(stderr, "Vulkan surface creation FAILED (instance=%p, VkResult=%d)\n",
                         (void*)instance, (int)sr);
            Close();
            return false;
        }
        // VkSurfaceKHR is a 64-bit non-dispatchable handle on x86_64; pack it
        // into the void* the gfx interface expects.
        std::memcpy(&swapHandle, &surface, sizeof(swapHandle));
    } else if (api == gfx::GfxApi::WebGPU) {
        swapHandle = static_cast<void*>(window_);
    } else if (api == gfx::GfxApi::Metal) {
        // Same convention as WebGPU on non-Windows: hand the gfx layer
        // a GLFWwindow* and let it pull glfwGetCocoaWindow internally.
        swapHandle = static_cast<void*>(window_);
    } else {
        std::fprintf(stderr, "Backend not supported on this platform\n");
        Close();
        return false;
    }
#else
    swapHandle = static_cast<void*>(glfwGetWin32Window(window_));
#endif

    targetId_ = service_.Pipeline().CreateSwapChainTarget(swapHandle, fbW, fbH);
    if (targetId_ == 0) {
        std::fprintf(stderr, "CreateSwapChainTarget FAILED\n");
        Close();
        return false;
    }
    service_.Pipeline().SetPrimaryTarget(targetId_);
    lastFbW_ = fbW;
    lastFbH_ = fbH;

    return true;
}

void ViewerApp::Close() {
    // Destroy the Storage Explorer (its thumbnail scenes + offscreen targets)
    // while the gfx device is still alive, before Pipeline().Shutdown().
    storageExplorer_.reset();
    if (imguiInitialised_) {
        ShutdownImGui();
    }
    if (service_.Pipeline().IsDeviceReady()) {
        service_.Pipeline().Shutdown();
    }
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
        glfwTerminate();
    }
    targetId_ = 0;
}

bool ViewerApp::ShouldClose() const {
    return !window_ || glfwWindowShouldClose(window_);
}

void ViewerApp::InitImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // imgui.ini lives alongside the exe (next to settings ini) — letting
    // ImGui pick up its own default `imgui.ini` in CWD is fine for now.

    ApplyImGuiTheme();

    // Scale fonts + style sizes to the current monitor's content scale.
    // Glyphs rasterise at scaled pixel size (via style.FontScaleDpi) instead
    // of being bilinear-upsampled, which is what fixes "weird" text on 4K
    // and 1080p-scaled displays.
    float xs = 1.0f;
    float ys = 1.0f;
    glfwGetWindowContentScale(window_, &xs, &ys);
    // Bundled Noto fonts live in `fonts/` next to the exe — hand the dir to the
    // font loader so the atlas covers every UI language (CJK included). Also pass
    // the language-picker endonyms: they're hardcoded (not in any catalog) and
    // can use glyphs no translation does (e.g. 體 in 繁體中文).
    std::string endonyms;
    for (const auto& e : i18n::languages()) {
        endonyms += e.endonym;
        endonyms += ' ';
    }
    ApplyImGuiDpiScale(xs, io::PathToUtf8(AssetDir() / "fonts"), endonyms);

    // GLFW backend handles input only; the engine adapter draws.
    ImGui_ImplGlfw_InitForOther(window_, true);
    imguiInitialised_ = true;
}

void ViewerApp::ShutdownImGui() {
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    imguiInitialised_ = false;
}

void ViewerApp::SetLoopNonLoopingPolicy(bool on) {
    loopNonLoopingPolicy_ = on;
    for (auto& [h, mi] : service_.Scene().Actors().All()) {
        if (mi->IsChild())
            continue;
        mi->ignoreNonLooping = on;
    }
}

model::Actor* ViewerApp::FocusActorPtr() const {
    return service_.Scene().Actors().Find(focusActor_);
}

void ViewerApp::FramebufferSizeCallback(GLFWwindow* w, int width, int height) {
    auto* self = static_cast<ViewerApp*>(glfwGetWindowUserPointer(w));
    if (self)
        self->OnFramebufferResize(width, height);
}
void ViewerApp::WindowRefreshCallback(GLFWwindow* w) {
    auto* self = static_cast<ViewerApp*>(glfwGetWindowUserPointer(w));
    if (self)
        self->RedrawFromCallback();
}
void ViewerApp::MouseButtonCallback(GLFWwindow* w, int button, int action, int /*mods*/) {
    auto* self = static_cast<ViewerApp*>(glfwGetWindowUserPointer(w));
    if (self)
        self->OnMouseButton(button, action);
}
void ViewerApp::CursorPosCallback(GLFWwindow* w, double x, double y) {
    auto* self = static_cast<ViewerApp*>(glfwGetWindowUserPointer(w));
    if (self)
        self->OnCursorPos(x, y);
}
void ViewerApp::ScrollCallback(GLFWwindow* w, double /*xoff*/, double yoff) {
    auto* self = static_cast<ViewerApp*>(glfwGetWindowUserPointer(w));
    if (self)
        self->OnScroll(yoff);
}

void ViewerApp::OnFramebufferResize(i32 w, i32 h) {
    if (w <= 0 || h <= 0)
        return;
    if (!service_.Pipeline().IsDeviceReady())
        return;
    if (w == lastFbW_ && h == lastFbH_)
        return;
    service_.Pipeline().ResizePrimaryTarget(w, h);
    lastFbW_ = w;
    lastFbH_ = h;

    // Paint the new size right away: on Windows this callback fires from
    // inside the modal sizing loop, so without it the freshly-resized swap
    // chain stays unpresented for the whole drag and the window shows the
    // old frame with black margins.
    RedrawFromCallback();
}

void ViewerApp::RedrawFromCallback() {
    if (inCallbackRedraw_ || !window_ || !ui_ || targetId_ == 0)
        return;
    if (!service_.Pipeline().IsDeviceReady())
        return;
    inCallbackRedraw_ = true;
    // dt 0 — the modal loop owns wall-clock time here; advancing animation
    // per repaint would fast-forward the scene while the user drags.
    Tick(0.0f);
    inCallbackRedraw_ = false;
}

void ViewerApp::OnMouseButton(i32 button, i32 action) {
    // ImGui's GLFW backend already routes events into ImGui IO. We gate
    // camera input on WantCaptureMouse so clicks inside an ImGui window
    // don't double up.
    if (ImGui::GetCurrentContext()) {
        if (ImGui::GetIO().WantCaptureMouse) {
            // Forget any in-flight drag so releasing the button outside an
            // ImGui window doesn't snap the camera.
            lmbDown_ = rmbDown_ = mmbDown_ = false;
            return;
        }
    }

    f64 mx = 0.0, my = 0.0;
    glfwGetCursorPos(window_, &mx, &my);
    const bool pressed = (action == GLFW_PRESS);

    // ViewCube clicks no longer get a dedicated branch here — the host
    // overlays an invisible ImGui button on the cube region (see
    // ViewerUI::BuildViewCubeWidget). When that button is hovered ImGui's
    // WantCaptureMouse short-circuits the camera handler above, and the
    // widget itself performs the hit-test + camera snap.
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        lmbDown_ = pressed;
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        rmbDown_ = pressed;
    } else if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        mmbDown_ = pressed;
    }
    lastMouseX_ = mx;
    lastMouseY_ = my;
}

void ViewerApp::OnCursorPos(f64 x, f64 y) {
    const f64 dx = x - lastMouseX_;
    const f64 dy = y - lastMouseY_;
    lastMouseX_ = x;
    lastMouseY_ = y;

    if (cameraLocked_)
        return;
    if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse)
        return;

    auto& cam = service_.Scene().Camera();
    if (lmbDown_)
        cam.Rotate(static_cast<i32>(dx), static_cast<i32>(dy));
    if (rmbDown_)
        cam.Pan(static_cast<i32>(-dx), static_cast<i32>(dy));
    if (mmbDown_)
        cam.ZoomSmooth(static_cast<f32>(dy) * cam.GetDistance() / Camera::kFactorRelDist);
}

void ViewerApp::OnScroll(f64 yoffset) {
    if (cameraLocked_)
        return;
    if (ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse)
        return;
    service_.Scene().Camera().Zoom(static_cast<i32>(yoffset * 30.0));
}

void ViewerApp::FrameCameraToModel(model::Actor* hero) {
    tools::FrameCameraToModel(service_.Scene().Camera(), hero);
}

bool ViewerApp::FrameCameraToEffect() {
    return tools::FrameCameraToEffect(service_, service_.Scene().Camera(), focusActor_);
}

namespace {
// Whether the renderer this viewer links was built with each foreign format.
// Constants rather than #ifdef at every use site so the surrounding code reads
// the same in either configuration.
#if WDX_ENABLE_M2
constexpr bool kM2Compiled = true;
#else
constexpr bool kM2Compiled = false;
#endif
#if WDX_ENABLE_M3
constexpr bool kM3Compiled = true;
#else
constexpr bool kM3Compiled = false;
#endif

std::string LowerAscii(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string LowerExt(const std::filesystem::path& path) {
    return LowerAscii(path.extension().string());
}

// .pkb / .pkfx are standalone PopcornFX effects, not models.
bool IsEffectPath(const std::filesystem::path& path) {
    const std::string ext = LowerExt(path);
    return ext == ".pkb" || ext == ".pkfx";
}

// A model from another Blizzard game. Loads through the same SpawnUnit — the
// renderer sniffs the chunk magic — but none of the Warcraft III preamble
// applies to it: no BLS layers to probe for HD-ness, no MDX writer to save
// through, no Reforged art overlay to re-resolve.
bool IsForeignModelPath(const std::filesystem::path& path) {
    const std::string ext = LowerExt(path);
    return ext == ".m2" || ext == ".m3";
}

// A `.wem` is none of the above and all of them: an interchange container that
// becomes a native model of whichever profile the user picked. Which is why it
// is its own predicate rather than a third arm of IsForeignModelPath — the
// Warcraft III preamble does not apply, and neither does "there is no writer
// for it", since writing WEM is the one thing every format can do.
bool IsWemPath(const std::filesystem::path& path) {
    return LowerExt(path) == ".wem";
}

// glTF opens through the same machinery a `.wem` does: the import is a
// Generic-profile document, the popup lists what it derives into, and
// `BuildWemSource` draws it (GLTF_DESIGN §2). One predicate beside IsWemPath
// rather than folded into it, because Save As and the profile texts still
// treat the two containers differently.
bool IsGltfPath(const std::filesystem::path& path) {
    const std::string ext = LowerExt(path);
    return ext == ".gltf" || ext == ".glb";
}

// A path the WEM open path owns — the interchange containers.
bool IsInterchangePath(const std::filesystem::path& path) {
    return IsWemPath(path) || IsGltfPath(path);
}
} // namespace

bool ViewerApp::CurrentModelIsForeign() const {
    return IsForeignModelPath(currentModelPath_);
}

// ---- WEM interchange --------------------------------------------------------

std::shared_ptr<io::WemDocument> ViewerApp::PeekWemDocument(const std::filesystem::path& path) {
    if (IsGltfPath(path))
        return io::ParseGltfFile(path);
    if (!IsWemPath(path))
        return nullptr;
    return io::ParseWemFile(path);
}

bool ViewerApp::OpenWemAs(const std::filesystem::path& path,
                          std::shared_ptr<io::WemDocument> document,
                          ::whiteout::models::wem::ProfileId profile) {
    if (!document)
        document = PeekWemDocument(path);
    if (!document) {
        std::fprintf(stderr, "[viewer] '%s' is not a WEM file this build can read\n",
                     io::PathToUtf8(path).c_str());
        return false;
    }
    if (profile == ::whiteout::models::wem::ProfileId::Count)
        profile = preferredWemProfile_;
    if (profile == ::whiteout::models::wem::ProfileId::Count)
        profile = io::DefaultWemProfile(document->document);

    // Both before OpenDocument, because FollowModelGame runs inside it and the
    // profile is what tells it which game's storage to point the shared
    // provider at. A `.wem` whose textures are fileDataIDs and a provider left
    // on Warcraft III is the same silent all-white load an `.m2` used to give.
    pendingWemDocument_ = std::move(document);
    pendingWemProfile_ = profile;
    const bool ok = OpenDocument(path, /*effect=*/false);
    pendingWemDocument_.reset();
    return ok;
}

bool ViewerApp::CanSaveAsMdx() const {
    ViewerApp* self = const_cast<ViewerApp*>(this);
    // A PopcornFX effect is copied verbatim rather than written, so it answers
    // yes on its own terms — the dialog routes it before it ever reaches the
    // MDX writer.
    if (IsEffectPath(currentModelPath_))
        return true;
    model::Actor* actor = self->FocusActorPtr();
    if (!actor)
        return false;
    if (actor->sourceTemplate &&
        dynamic_cast<const io::MdxModelAdapter*>(actor->sourceTemplate->adapter.get())) {
        return true;
    }
    return dynamic_cast<const io::MdxModelAdapter*>(actor->animation.Source().get()) != nullptr;
}

bool ViewerApp::CanExportWem() const {
    const model::Actor* actor = const_cast<ViewerApp*>(this)->FocusActorPtr();
    if (!actor)
        return false;
    const auto& source = actor->animation.Source();
    if (!source)
        return false;
    const auto* modelSource = dynamic_cast<const IModelSource*>(source.get());
    return modelSource && io::CanExportModelToWem(*modelSource);
}

bool ViewerApp::ExportWem(const std::filesystem::path& outPath) {
    model::Actor* actor = FocusActorPtr();
    if (!actor || !actor->animation.Source()) {
        std::fprintf(stderr, "[viewer] Export WEM: no model on screen\n");
        return false;
    }
    auto* source = dynamic_cast<IModelSource*>(actor->animation.Source().get());
    if (!source) {
        std::fprintf(stderr, "[viewer] Export WEM: this actor has no model source\n");
        return false;
    }

    io::WemExportOptions options;
    options.documentName = io::PathToUtf8(currentModelPath_.stem());
    const io::WemExportResult exported =
        io::ExportModelToWem(*source, service_.Scene().ActiveContentProvider(), options);
    if (!exported.ok()) {
        std::fprintf(stderr, "[viewer] Export WEM FAILED: %s\n", exported.error.c_str());
        return false;
    }
    if (!exported.diagnostics.empty()) {
        // A lossy conversion that succeeded and one that failed are different
        // states; both have something to say, and this is the one the user can
        // act on — a particle emitter the format does not carry, a look that
        // did not resolve.
        std::fprintf(stderr, "[viewer] Export WEM: %zu diagnostic(s)\n%s",
                     exported.diagnostics.size(),
                     io::DescribeWemDiagnostics(exported.diagnostics).c_str());
    }

    std::string error;
    ::whiteout::models::wem::Diagnostics writeReport;
    if (!io::WriteWemDocument(*exported.document, outPath, &writeReport, &error)) {
        std::fprintf(stderr, "[viewer] Export WEM FAILED: %s\n", error.c_str());
        return false;
    }
    std::printf("[viewer] Saved WEM (%s): %s\n", exported.formatId.c_str(),
                io::PathToUtf8(outPath).c_str());
    return true;
}

// ---- Warcraft III export -----------------------------------------------------
//
// The other half of `Save As`. That one re-serialises a Warcraft III model in
// its own format; this converts a foreign one *into* Warcraft III, which is the
// whole reason the interchange format exists. See tools/basic_viewer/mdx_export.h.

bool ViewerApp::CanExportMdx() const {
    if (!CanExportWem())
        return false;
    // A Warcraft III model is offered Save As instead: writing it back through
    // WEM would derive a material set it already carries, which is lossy for no
    // reason at all.
    const model::Actor* actor = const_cast<ViewerApp*>(this)->FocusActorPtr();
    return actor &&
           dynamic_cast<const io::MdxModelAdapter*>(actor->animation.Source().get()) == nullptr;
}

bool ViewerApp::ExportMdx(const std::filesystem::path& outPath,
                          ::whiteout::models::wem::ProfileId profile, bool exportTextures) {
    model::Actor* actor = FocusActorPtr();
    auto* source = actor ? dynamic_cast<IModelSource*>(actor->animation.Source().get()) : nullptr;
    if (!source) {
        std::fprintf(stderr, "[viewer] Export MDX: no model on screen\n");
        return false;
    }

    MdxExportRequest request;
    request.source = source;
    request.provider = service_.Scene().ActiveContentProvider();
    request.outPath = outPath;
    request.modelName = io::PathToUtf8(currentModelPath_.stem());
    request.profile = profile;
    request.exportTextures = exportTextures;

    const MdxExportReport report = ExportModelAsMdx(request);
    if (!report.diagnostics.empty()) {
        // A cross-format write is lossy by construction and this is the list of
        // what it cost — the particle emitters Warcraft III has no vocabulary
        // for, the stages the layer stack could not fold. Printed on success as
        // well, because that is when it is worth reading.
        std::fprintf(stderr, "[viewer] Export MDX: %zu diagnostic(s)\n%s",
                     report.diagnostics.size(),
                     io::DescribeWemDiagnostics(report.diagnostics, 400).c_str());
    }
    if (!report.ok) {
        std::fprintf(stderr, "[viewer] Export MDX FAILED: %s\n", report.error.c_str());
        return false;
    }
    std::printf("[viewer] Saved Warcraft III model (%s, %gx scale): %s\n", report.formatId.c_str(),
                static_cast<double>(report.scale), io::PathToUtf8(outPath).c_str());
    if (exportTextures) {
        std::printf("[viewer] Textures: %d exported, %d skipped, %d failed\n",
                    report.texturesExported, report.texturesSkipped, report.texturesFailed);
    }
    return true;
}

// ---- StarCraft II export ----------------------------------------------------
//
// ExportMdx's twin: a `.mdx`, a `.m2` or a Diablo III `.app` converted through
// WEM and written as `.m3`. See tools/basic_viewer/m3_export.h.

bool ViewerApp::CanExportM3() const {
    if (!CanExportWem())
        return false;
#if WDX_ENABLE_M3
    // A StarCraft II model is offered Save As instead, for ExportMdx's reason:
    // writing it back through WEM would derive a material set it already
    // carries.
    const model::Actor* actor = const_cast<ViewerApp*>(this)->FocusActorPtr();
    return actor &&
           dynamic_cast<const io::M3ModelAdapter*>(actor->animation.Source().get()) == nullptr;
#else
    return true;
#endif
}

bool ViewerApp::ExportM3(const std::filesystem::path& outPath,
                         ::whiteout::models::wem::ProfileId profile, bool exportTextures,
                         bool exactPasses, bool sharpenTeamKey) {
    model::Actor* actor = FocusActorPtr();
    auto* source = actor ? dynamic_cast<IModelSource*>(actor->animation.Source().get()) : nullptr;
    if (!source) {
        std::fprintf(stderr, "[viewer] Export M3: no model on screen\n");
        return false;
    }

    M3ExportRequest request;
    request.source = source;
    request.provider = service_.Scene().ActiveContentProvider();
    request.outPath = outPath;
    request.modelName = io::PathToUtf8(currentModelPath_.stem());
    request.profile = profile;
    request.exportTextures = exportTextures;
    // The tileset the viewer currently resolves replaceables with is the one
    // the export resolves them with.
    request.wc3.tileset = GetCurrentTileset();
    request.wc3.exactPasses = exactPasses;
    request.wc3.sharpenTeamKey = sharpenTeamKey;

    const M3ExportReport report = ExportModelAsM3(request);
    if (!report.diagnostics.empty()) {
        std::fprintf(stderr, "[viewer] Export M3: %zu diagnostic(s)\n%s",
                     report.diagnostics.size(),
                     io::DescribeWemDiagnostics(report.diagnostics, 400).c_str());
    }
    if (!report.ok) {
        std::fprintf(stderr, "[viewer] Export M3 FAILED: %s\n", report.error.c_str());
        return false;
    }
    std::printf("[viewer] Saved StarCraft II model (%s, %gx scale): %s\n", report.formatId.c_str(),
                static_cast<double>(report.scale), io::PathToUtf8(outPath).c_str());
    if (exportTextures) {
        std::printf("[viewer] Textures: %d exported, %d skipped, %d failed\n",
                    report.texturesExported, report.texturesSkipped, report.texturesFailed);
    }
    return true;
}

// ---- glTF export -------------------------------------------------------------

bool ViewerApp::CanExportGltf() const {
    // Exactly CanExportWem: glTF export takes any carried profile, so every
    // model WEM reads — a Warcraft III one included — exports (GLTF_DESIGN §2).
    return CanExportWem();
}

bool ViewerApp::ExportGltf(const std::filesystem::path& outPath, bool binary,
                           bool exportTextures) {
    model::Actor* actor = FocusActorPtr();
    auto* source = actor ? dynamic_cast<IModelSource*>(actor->animation.Source().get()) : nullptr;
    if (!source) {
        std::fprintf(stderr, "[viewer] Export glTF: no model on screen\n");
        return false;
    }

    GltfExportRequest request;
    request.source = source;
    request.provider = service_.Scene().ActiveContentProvider();
    request.outPath = outPath;
    request.modelName = io::PathToUtf8(currentModelPath_.stem());
    request.binary = binary;
    request.exportTextures = exportTextures;

    const GltfExportReport report = ExportModelAsGltf(request);
    if (!report.diagnostics.empty()) {
        std::fprintf(stderr, "[viewer] Export glTF: %zu diagnostic(s)\n%s",
                     report.diagnostics.size(),
                     io::DescribeWemDiagnostics(report.diagnostics, 400).c_str());
    }
    if (!report.ok) {
        std::fprintf(stderr, "[viewer] Export glTF FAILED: %s\n", report.error.c_str());
        return false;
    }
    std::printf("[viewer] Saved glTF (%s, %s profile): %s\n", report.formatId.c_str(),
                ::whiteout::models::wem::Profile(report.profile).displayName,
                io::PathToUtf8(outPath).c_str());
    if (exportTextures) {
        std::printf("[viewer] Textures: %d %s, %d failed\n", report.texturesExported,
                    binary ? "embedded" : "exported", report.texturesFailed);
    }
    return true;
}

// ---- StarCraft II save -------------------------------------------------------
//
// ExportM3's mirror. That one derives a StarCraft II material set for a model
// that never had one; this writes back a model that already IS `.m3`, which is
// why it goes straight through `m3::Writer` and not through WEM. See
// tools/basic_viewer/m3_save.h.

bool ViewerApp::CanSaveM3() const {
#if WDX_ENABLE_M3
    const model::Actor* actor = const_cast<ViewerApp*>(this)->FocusActorPtr();
    return actor && actor->animation.HasSource() &&
           dynamic_cast<const io::M3ModelAdapter*>(actor->animation.Source().get()) != nullptr;
#else
    return false;
#endif
}

bool ViewerApp::SaveM3(const std::filesystem::path& outPath, bool mergeAnimations,
                       bool convertToSc2, bool exportTextures, std::string* error) {
    if (error)
        error->clear();
#if WDX_ENABLE_M3
    model::Actor* actor = FocusActorPtr();
    const auto* source =
        actor && actor->animation.HasSource()
            ? dynamic_cast<const io::M3ModelAdapter*>(actor->animation.Source().get())
            : nullptr;
    if (!source) {
        if (error)
            *error = "no StarCraft II model on screen";
        std::fprintf(stderr, "[viewer] Save M3: no StarCraft II model on screen\n");
        return false;
    }

    M3SaveRequest request;
    request.source = source;
    request.provider = service_.Scene().ActiveContentProvider();
    request.outPath = outPath;
    request.mergeAnimations = mergeAnimations;
    request.convertToSc2 = convertToSc2;
    request.exportTextures = exportTextures;

    const M3SaveReport report = SaveModelAsM3(request);
    for (const std::string& reason : report.lossy)
        std::fprintf(stderr, "[viewer] Save M3: %s\n", reason.c_str());
    if (!report.ok) {
        if (error)
            *error = report.error;
        std::fprintf(stderr, "[viewer] Save M3 FAILED: %s\n", report.error.c_str());
        return false;
    }
    std::printf("[viewer] Saved M3 (MODL v%d%s%s): %s\n", report.version,
                report.retargeted ? ", retargeted for StarCraft II" : "",
                report.mergedFiles ? ", animations merged" : "", io::PathToUtf8(outPath).c_str());
    if (report.mergedFiles) {
        std::printf("[viewer] Merged %zu animation file(s), %zu sequence(s)\n", report.mergedFiles,
                    report.mergedSequences);
    }
    if (exportTextures) {
        std::printf("[viewer] Textures: %d exported, %d skipped, %d failed\n",
                    report.texturesExported, report.texturesSkipped, report.texturesFailed);
    }
    return true;
#else
    (void)outPath;
    (void)mergeAnimations;
    (void)convertToSc2;
    (void)exportTextures;
    return false;
#endif
}

// ---- World of Warcraft creature skins ---------------------------------------
//
// A skin is a property of the display record a creature was spawned with, not
// of the model, so picking one is a host decision. Both pickers below change
// the actor where it stands via ModelLoader::RestyleWowModel and only reload
// when that says it cannot — the document's pose and the camera's framing are
// what the reload used to throw away, and neither is a function of the skin.

std::vector<std::string> ViewerApp::WowSkinNames() const {
#if WDX_ENABLE_M2
    if (currentModelPath_.empty())
        return {};
    std::vector<std::string> names;
    for (const auto& v :
         const_cast<ViewerApp*>(this)->service_.Loader().WowReplaceables().Variations(
             ContentRef::FromPath(io::PathToUtf8(currentModelPath_))))
        names.push_back(v.label);
    return names;
#else
    return {};
#endif
}

u32 ViewerApp::WowSkin() const {
#if WDX_ENABLE_M2
    return const_cast<ViewerApp*>(this)->service_.Loader().WowReplaceables().Variation();
#else
    return 0;
#endif
}

void ViewerApp::RestyleLoadedWowModels() {
#if WDX_ENABLE_M2
    if (currentModelPath_.empty() || !IsForeignModelPath(currentModelPath_))
        return;
    // Same shape as SetWowSkin: restyle in place, and fall back to a reload
    // only when the actor cannot be restyled where it stands.
    if (!service_.Loader().RestyleWowModel(focusActor_,
                                           ContentRef::FromPath(io::PathToUtf8(currentModelPath_))))
        LoadModelIntoActiveScene(currentModelPath_);
#endif
}

void ViewerApp::SetWowSkin(u32 skin) {
#if WDX_ENABLE_M2
    auto& replaceables = service_.Loader().WowReplaceables();
    if (replaceables.Variation() == skin)
        return;
    replaceables.SetVariation(skin);
    if (currentModelPath_.empty() || !IsForeignModelPath(currentModelPath_))
        return;
    if (!service_.Loader().RestyleWowModel(focusActor_,
                                           ContentRef::FromPath(io::PathToUtf8(currentModelPath_))))
        LoadModelIntoActiveScene(currentModelPath_);
#else
    (void)skin;
#endif
}

// ---- World of Warcraft character customisation ------------------------------
//
// The other half of the same idea: a character model leaves its geosets and its
// body texture for the game to choose, and the choice is the player's rather
// than the model's. See renderer::profiles::wow::WowCharacterAppearance.

std::vector<ViewerApp::WowCharacterOption> ViewerApp::WowCharacterOptions() const {
#if WDX_ENABLE_M2
    if (currentModelPath_.empty())
        return {};
    std::vector<WowCharacterOption> out;
    for (const auto& o : const_cast<ViewerApp*>(this)->service_.Loader().WowCharacters().Options(
             ContentRef::FromPath(io::PathToUtf8(currentModelPath_)))) {
        out.push_back({o.name, o.optionId, o.choiceCount, o.selected});
    }
    return out;
#else
    return {};
#endif
}

void ViewerApp::SetWowCharacterChoice(u32 optionId, u32 choiceIndex) {
#if WDX_ENABLE_M2
    if (currentModelPath_.empty())
        return;
    const ContentRef ref = ContentRef::FromPath(io::PathToUtf8(currentModelPath_));
    service_.Loader().WowCharacters().SetChoice(ref, optionId, choiceIndex);
    if (!service_.Loader().RestyleWowModel(focusActor_, ref))
        LoadModelIntoActiveScene(currentModelPath_);
#else
    (void)optionId;
    (void)choiceIndex;
#endif
}

// ---- Diablo III character dressing -----------------------------------------
//
// The third of the three: WowSkin picks a creature's fill, WowCharacterOptions
// picks a character's, and this picks which of the armour variants a `.app`
// already carries is the one being worn. All three restyle in place.
//
// Addressed through the focus actor rather than through a path, unlike the two
// above, because a D3 outfit is keyed on the *appearance* — 594 actors name one
// `.app` and ModelLoader hands them one shared drawable, so the file that was
// asked for is not what is being dressed.

#if WDX_ENABLE_D3
namespace {

renderer::profiles::diablo3::D3CharacterAppearance* D3Characters(renderer::RenderService& svc,
                                                                 u32 actorHandle,
                                                                 io::D3ModelAdapter** outAdapter) {
    auto adapter = svc.Loader().D3AdapterOf(actorHandle);
    if (!adapter)
        return nullptr;
    auto& chars = svc.Loader().D3Characters();
    if (!chars.IsCharacter(*adapter))
        return nullptr;
    *outAdapter = adapter.get();
    return &chars;
}

} // namespace
#endif

std::vector<ViewerApp::D3CharacterSlot> ViewerApp::D3CharacterSlots() const {
#if WDX_ENABLE_D3
    auto& svc = const_cast<ViewerApp*>(this)->service_;
    io::D3ModelAdapter* adapter = nullptr;
    auto* chars = D3Characters(svc, focusActor_, &adapter);
    if (!chars)
        return {};
    std::vector<D3CharacterSlot> out;
    for (const auto& s : chars->Slots(*adapter)) {
        D3CharacterSlot row;
        row.name = s.name;
        row.slot = static_cast<i32>(s.slot);
        for (const auto& item : s.items)
            row.items.push_back(item.label);
        row.selectedItem = s.selectedItem;
        row.lookIndex = s.lookIndex;
        row.registryDriven = s.slot != ::whiteout::sno::d3::native::LookSlot::Hair;
        out.push_back(std::move(row));
    }
    return out;
#else
    return {};
#endif
}

void ViewerApp::SetD3CharacterItem(i32 slot, u32 itemIndex) {
#if WDX_ENABLE_D3
    io::D3ModelAdapter* adapter = nullptr;
    auto* chars = D3Characters(service_, focusActor_, &adapter);
    if (!chars)
        return;
    chars->SetItem(*adapter, static_cast<::whiteout::sno::d3::native::LookSlot>(slot), itemIndex);
    RestyleD3();
#else
    (void)slot;
    (void)itemIndex;
#endif
}

void ViewerApp::SetD3CharacterSlotLook(i32 slot, u32 lookIndex) {
#if WDX_ENABLE_D3
    io::D3ModelAdapter* adapter = nullptr;
    auto* chars = D3Characters(service_, focusActor_, &adapter);
    if (!chars)
        return;
    chars->SetSlotLook(*adapter, static_cast<::whiteout::sno::d3::native::LookSlot>(slot),
                       lookIndex);
    RestyleD3();
#else
    (void)slot;
    (void)lookIndex;
#endif
}

std::vector<std::string> ViewerApp::D3LookNames() const {
#if WDX_ENABLE_D3
    auto& svc = const_cast<ViewerApp*>(this)->service_;
    io::D3ModelAdapter* adapter = nullptr;
    if (!D3Characters(svc, focusActor_, &adapter))
        return {};
    std::vector<std::string> out;
    for (const auto& n : adapter->Looks())
        out.push_back(n);
    return out;
#else
    return {};
#endif
}

void ViewerApp::SetD3CharacterLookForAll(u32 lookIndex) {
#if WDX_ENABLE_D3
    io::D3ModelAdapter* adapter = nullptr;
    auto* chars = D3Characters(service_, focusActor_, &adapter);
    if (!chars)
        return;
    chars->SetLookForAll(*adapter, lookIndex);
    RestyleD3();
#else
    (void)lookIndex;
#endif
}

std::vector<ViewerApp::D3CharacterExtra> ViewerApp::D3CharacterExtras() const {
#if WDX_ENABLE_D3
    auto& svc = const_cast<ViewerApp*>(this)->service_;
    io::D3ModelAdapter* adapter = nullptr;
    auto* chars = D3Characters(svc, focusActor_, &adapter);
    if (!chars)
        return {};
    std::vector<D3CharacterExtra> out;
    for (const auto& e : chars->Extras(*adapter))
        out.push_back({e.name, e.geoset, e.shown});
    return out;
#else
    return {};
#endif
}

void ViewerApp::SetD3CharacterExtra(u32 geoset, bool shown) {
#if WDX_ENABLE_D3
    io::D3ModelAdapter* adapter = nullptr;
    auto* chars = D3Characters(service_, focusActor_, &adapter);
    if (!chars)
        return;
    chars->SetExtra(*adapter, geoset, shown);
    RestyleD3();
#else
    (void)geoset;
    (void)shown;
#endif
}

#if WDX_ENABLE_D3
namespace d3n = ::whiteout::sno::d3::native;
#endif

namespace {

#if WDX_ENABLE_D3
constexpr const char* kD3VisualSlotNames[8] = {
    "Head", "Torso", "Feet", "Hands", "Right hand", "Left hand", "Shoulders", "Legs",
};

bool ContainsCaseless(std::string_view hay, std::string_view needle) {
    if (needle.empty())
        return true;
    if (needle.size() > hay.size())
        return false;
    auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
    };
    for (usize i = 0; i + needle.size() <= hay.size(); ++i) {
        usize j = 0;
        while (j < needle.size() && lower(hay[i + j]) == lower(needle[j]))
            ++j;
        if (j == needle.size())
            return true;
    }
    return false;
}
#endif

} // namespace

std::vector<ViewerApp::D3OutfitRow> ViewerApp::D3OutfitSlots() const {
#if WDX_ENABLE_D3
    auto& svc = const_cast<ViewerApp*>(this)->service_;
    io::D3ModelAdapter* adapter = nullptr;
    auto* chars = D3Characters(svc, focusActor_, &adapter);
    if (!chars)
        return {};
    if (!D3ItemRegistryReady())
        return {};
    auto& items = svc.Loader().D3Items();

    const auto outfit = chars->OutfitOf(*adapter);
    std::vector<D3OutfitRow> out;
    for (i32 s = 0; s < 8; ++s) {
        D3OutfitRow row;
        row.name = kD3VisualSlotNames[s];
        row.visualSlot = s;
        row.dye = outfit.slots[s].dyeType;
        const auto v = static_cast<d3n::EVisualSlot>(s);
        row.armour = v == d3n::EVisualSlot::Torso || v == d3n::EVisualSlot::Feet ||
                     v == d3n::EVisualSlot::Hands || v == d3n::EVisualSlot::Legs;
        if (outfit.slots[s].itemGbid != -1) {
            if (const auto* rec = items.FindByGbid(static_cast<u32>(outfit.slots[s].itemGbid))) {
                row.equipped = rec->name;
                row.equippedLabel = rec->displayName.empty() ? rec->name : rec->displayName;
            }
        }
        out.push_back(std::move(row));
    }
    return out;
#else
    return {};
#endif
}

bool ViewerApp::D3ItemRegistryReady() const {
#if WDX_ENABLE_D3
    auto* self = const_cast<ViewerApp*>(this);
    auto& items = self->service_.Loader().D3Items();
    switch (d3ItemsBuild_) {
    case D3ItemsBuild::Building:
        // The registry belongs to the task thread right now — not even
        // Built() may be asked.
        return false;
    case D3ItemsBuild::Ready:
        // A profile change can clear the registry under a Ready latch;
        // healing here re-kicks the build instead of answering "no tables"
        // for an install that has them.
        if (items.Built())
            return !items.Items().empty();
        self->d3ItemsBuild_ = D3ItemsBuild::NotStarted;
        [[fallthrough]];
    case D3ItemsBuild::NotStarted:
        // A CLI path (--d3-equip) may have built it synchronously already.
        if (items.Built()) {
            self->d3ItemsBuild_ = D3ItemsBuild::Ready;
            return !items.Items().empty();
        }
        self->BuildD3ItemRegistryAsync();
        return false;
    }
    return false;
#else
    return false;
#endif
}

void ViewerApp::BuildD3ItemRegistryAsync() {
#if WDX_ENABLE_D3
    if (d3ItemsBuild_ != D3ItemsBuild::NotStarted)
        return;
    auto& items = service_.Loader().D3Items();
    auto* provider = service_.Scene().ActiveContentProvider();
    d3ItemsBuild_ = D3ItemsBuild::Building;
    tasks_.Run(
        "Building item registry",
        [&items, provider](io::ProgressMonitor& m) {
            items.EnsureBuilt(provider, &m);
            // Deliberately always Ok: a storage with no GameBalance tables is
            // a normal state the popup reads off the emptiness, not an error
            // box in front of a user who pressed a button labelled Equip.
            return io::TaskResult::Ok();
        },
        [this](const io::TaskOutcome&) { d3ItemsBuild_ = D3ItemsBuild::Ready; },
        /*cancellable=*/false,
        // NOT modal: the popup draws the progress where the user is looking;
        // taking the whole screen for a wardrobe would be worse.
        /*modal=*/false);
#endif
}

#if WDX_ENABLE_D3
namespace {

// The focused model's class, off the loaded Appearance stem — the same read
// SetD3OutfitItem makes for the per-class attachment art. Nullopt for a
// non-player, which the pickers treat as "show everything".
std::optional<d3n::PlayerClass> D3FocusClass(const std::filesystem::path& modelPath) {
    if (const auto body = d3n::playerFromAppearanceStem(io::PathToUtf8(modelPath.stem())))
        return body->first;
    return std::nullopt;
}

const std::string& D3ItemLabel(const io::D3ItemRecord& rec) {
    return rec.displayName.empty() ? rec.name : rec.displayName;
}

} // namespace
#endif

std::vector<ViewerApp::D3OutfitItemEntry> ViewerApp::D3OutfitItemEntries(i32 visualSlot,
                                                                         std::string_view filter,
                                                                         usize max,
                                                                         bool allClasses) const {
#if WDX_ENABLE_D3
    auto& svc = const_cast<ViewerApp*>(this)->service_;
    if (!D3ItemRegistryReady())
        return {};
    auto& items = svc.Loader().D3Items();
    // The equipped stem, so the survivor of a display-name collision below is
    // the row the picker must show as selected.
    std::string equipped;
    {
        io::D3ModelAdapter* adapter = nullptr;
        if (auto* chars = D3Characters(svc, focusActor_, &adapter)) {
            const i32 gbid = chars->OutfitOf(*adapter).slots[visualSlot].itemGbid;
            if (gbid != -1)
                if (const auto* rec = items.FindByGbid(static_cast<u32>(gbid)))
                    equipped = rec->name;
        }
    }
    const auto cls =
        allClasses ? std::optional<d3n::PlayerClass>{} : D3FocusClass(currentModelPath_);
    std::vector<D3OutfitItemEntry> out;
    for (const u32 i : items.ItemsForSlot(static_cast<d3n::EVisualSlot>(visualSlot))) {
        const auto& rec = items.Items()[i];
        if (cls && !rec.CanWear(*cls))
            continue;
        const std::string& label = D3ItemLabel(rec);
        if (!ContainsCaseless(label, filter) && !ContainsCaseless(rec.name, filter))
            continue;
        out.push_back({label, rec.name});
    }
    // ItemsForSlot is stem-sorted; the picker reads display names, so order
    // by those and only then cap — a filter must reach the whole offer. The
    // equipped stem sorts to the front of its name group so the dedup below
    // keeps it.
    std::sort(out.begin(), out.end(), [&](const D3OutfitItemEntry& a, const D3OutfitItemEntry& b) {
        if (a.label != b.label)
            return a.label < b.label;
        if ((a.stem == equipped) != (b.stem == equipped))
            return a.stem == equipped;
        return a.stem < b.stem;
    });
    // One display name is one row: the art-test dupes ship one name on six
    // records, and six identical rows offer nothing five of them.
    out.erase(std::unique(out.begin(), out.end(),
                          [](const D3OutfitItemEntry& a, const D3OutfitItemEntry& b) {
                              return a.label == b.label;
                          }),
              out.end());
    if (out.size() > max)
        out.resize(max);
    return out;
#else
    (void)visualSlot;
    (void)filter;
    (void)max;
    (void)allClasses;
    return {};
#endif
}

std::vector<ViewerApp::D3OutfitSetEntry> ViewerApp::D3OutfitSetEntries(std::string_view filter,
                                                                       bool allClasses) const {
#if WDX_ENABLE_D3
    auto& svc = const_cast<ViewerApp*>(this)->service_;
    if (!D3ItemRegistryReady())
        return {};
    auto& items = svc.Loader().D3Items();
    const auto cls =
        allClasses ? std::optional<d3n::PlayerClass>{} : D3FocusClass(currentModelPath_);
    std::vector<D3OutfitSetEntry> out;
    for (const auto& set : items.Sets()) {
        if (set.key.empty())
            continue;
        if (cls && !((set.classMask >> static_cast<u32>(*cls)) & 1u))
            continue;
        usize pieces = 0;
        for (const u32 i : set.members)
            pieces += items.Items()[i].slotMask != 0;
        if (pieces == 0)
            continue;
        const std::string& label = set.displayName.empty() ? set.key : set.displayName;
        if (!ContainsCaseless(label, filter) && !ContainsCaseless(set.key, filter))
            continue;
        out.push_back({label, set.key, pieces});
    }
    // One display name is one row: the crafted tiers ship one name on two
    // keys ("Crafted Hell Set 002_104"/"_1xx", "_x1"/"P74_..."), identical to
    // a player. Sets() is display-name sorted, so duplicates are adjacent;
    // keep the fuller offer.
    std::vector<D3OutfitSetEntry> dedup;
    dedup.reserve(out.size());
    for (auto& e : out) {
        if (!dedup.empty() && dedup.back().label == e.label) {
            if (e.pieces > dedup.back().pieces)
                dedup.back() = std::move(e);
        } else {
            dedup.push_back(std::move(e));
        }
    }
    return dedup;
#else
    (void)filter;
    (void)allClasses;
    return {};
#endif
}

bool ViewerApp::EquipD3OutfitSet(std::string_view key) {
#if WDX_ENABLE_D3
    if (d3ItemsBuild_ == D3ItemsBuild::Building)
        return false; // the registry belongs to the task thread right now
    auto& items = service_.Loader().D3Items();
    items.EnsureBuilt(service_.Scene().ActiveContentProvider());
    const io::D3ItemSet* set = nullptr;
    for (const auto& s : items.Sets()) {
        if (s.key == key) {
            set = &s;
            break;
        }
    }
    if (!set)
        return false;
    // Each piece takes its own slot; weapons fill right hand then left so a
    // paired-blades set dual-wields. Slots the set does not cover keep what
    // they wore — a six-piece armour set must not undress the hands.
    using VS = d3n::EVisualSlot;
    constexpr VS kOrder[] = {VS::Head, VS::Shoulders, VS::Torso,     VS::Hands,
                             VS::Legs, VS::Feet,      VS::RightHand, VS::LeftHand};
    bool any = false;
    u16 taken = 0;
    for (const u32 i : set->members) {
        const auto& rec = items.Items()[i];
        for (const VS slot : kOrder) {
            const auto bit = static_cast<u16>(1u << static_cast<u32>(slot));
            if (!rec.CanGo(slot) || (taken & bit))
                continue;
            if (SetD3OutfitItem(static_cast<i32>(slot), rec.name)) {
                taken |= bit;
                any = true;
            }
            break;
        }
    }
    return any;
#else
    (void)key;
    return false;
#endif
}

bool ViewerApp::SetD3OutfitItem(i32 visualSlot, std::string_view itemName) {
#if WDX_ENABLE_D3
    io::D3ModelAdapter* adapter = nullptr;
    auto* chars = D3Characters(service_, focusActor_, &adapter);
    if (!chars)
        return false;
    if (d3ItemsBuild_ == D3ItemsBuild::Building)
        return false; // the registry belongs to the task thread right now
    auto& items = service_.Loader().D3Items();
    items.EnsureBuilt(service_.Scene().ActiveContentProvider());

    const auto slot = static_cast<d3n::EVisualSlot>(visualSlot);
    if (itemName.empty()) {
        chars->SetOutfitItem(*adapter, slot, nullptr, {});
        RestyleD3();
        return true;
    }
    const io::D3ItemRecord* rec = items.FindByName(itemName);
    if (!rec)
        return false;
    // The per-class attachment art needs to know who is wearing this; the
    // loaded path's stem says (Barbarian_Male.app and friends).
    const std::string stem = io::PathToUtf8(currentModelPath_.stem());
    if (const auto body = d3n::playerFromAppearanceStem(stem))
        chars->SetOutfitBody(*adapter, body->first, body->second);
    std::shared_ptr<const d3n::Actor> actor;
    if (rec->snoActor > 0)
        actor = service_.Loader().D3Cache().Actor(rec->snoActor);
    if (!chars->SetOutfitItem(*adapter, slot, rec, std::move(actor)))
        return false;
    RestyleD3();
    return true;
#else
    (void)visualSlot;
    (void)itemName;
    return false;
#endif
}

bool ViewerApp::D3OutfitSheathed() const {
#if WDX_ENABLE_D3
    auto& svc = const_cast<ViewerApp*>(this)->service_;
    io::D3ModelAdapter* adapter = nullptr;
    auto* chars = D3Characters(svc, focusActor_, &adapter);
    return chars && chars->OutfitOf(*adapter).sheathed;
#else
    return false;
#endif
}

void ViewerApp::SetD3OutfitSheathed(bool sheathed) {
#if WDX_ENABLE_D3
    io::D3ModelAdapter* adapter = nullptr;
    auto* chars = D3Characters(service_, focusActor_, &adapter);
    if (!chars)
        return;
    chars->SetOutfitSheathed(*adapter, sheathed);
    RestyleD3();
#else
    (void)sheathed;
#endif
}

void ViewerApp::SetD3OutfitDye(i32 visualSlot, i32 dye) {
#if WDX_ENABLE_D3
    io::D3ModelAdapter* adapter = nullptr;
    auto* chars = D3Characters(service_, focusActor_, &adapter);
    if (!chars)
        return;
    chars->SetOutfitDye(*adapter, static_cast<d3n::EVisualSlot>(visualSlot), dye);
    RestyleD3();
#else
    (void)visualSlot;
    (void)dye;
#endif
}

std::string ViewerApp::D3OutfitItemTip(std::string_view itemName) const {
#if WDX_ENABLE_D3
    if (d3ItemsBuild_ == D3ItemsBuild::Building)
        return {}; // the registry belongs to the task thread right now
    auto& svc = const_cast<ViewerApp*>(this)->service_;
    auto& items = svc.Loader().D3Items();
    const io::D3ItemRecord* rec = items.FindByName(itemName);
    if (!rec)
        return {};
    // First line: the record stem and its type — what a preset or a corpus
    // scenario token would spell. Below it, the set and the class lock.
    std::string tip = rec->name;
    const std::string_view type = items.TypeNameOf(rec->gbidItemType);
    if (!type.empty()) {
        tip += "   ";
        tip += type;
    }
    if (const auto* set = items.SetOf(*rec); set && !set->displayName.empty()) {
        tip += "\n";
        tip += set->displayName;
    }
    const u32 m = rec->classMask;
    if (m != io::kD3AllClasses && (m & (m - 1)) == 0) {
        u32 bit = m, ordinal = 0;
        while (bit >>= 1)
            ++ordinal;
        tip += "\n";
        tip += d3n::playerClassName(static_cast<d3n::PlayerClass>(ordinal));
        tip += " only";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "\ngbid 0x%08X   actor %d", rec->gbid, rec->snoActor);
    tip += buf;
    return tip;
#else
    (void)itemName;
    return {};
#endif
}

#if WDX_ENABLE_D3
namespace {

std::filesystem::path D3OutfitPresetPath() {
    return SettingsIniPath().parent_path() / "d3_outfits.ini";
}

// A preset name becomes an ini section, and the section separator is '.'.
std::string PresetSection(std::string_view name) {
    std::string s(name);
    for (char& c : s)
        if (c == '.' || c == '[' || c == ']' || c == '=')
            c = '_';
    return s;
}

} // namespace
#endif

std::vector<std::string> ViewerApp::D3OutfitPresetNames() const {
#if WDX_ENABLE_D3
    ini::IniMap map;
    map.Load(D3OutfitPresetPath());
    std::set<std::string> names;
    for (const auto& [k, v] : map.values) {
        const auto dot = k.rfind('.');
        if (dot != std::string::npos)
            names.insert(k.substr(0, dot));
    }
    return {names.begin(), names.end()};
#else
    return {};
#endif
}

bool ViewerApp::SaveD3OutfitPreset(std::string_view name) {
#if WDX_ENABLE_D3
    if (name.empty())
        return false;
    io::D3ModelAdapter* adapter = nullptr;
    auto* chars = D3Characters(service_, focusActor_, &adapter);
    if (!chars)
        return false;
    if (d3ItemsBuild_ == D3ItemsBuild::Building)
        return false; // the registry belongs to the task thread right now
    auto& items = service_.Loader().D3Items();
    const auto outfit = chars->OutfitOf(*adapter);

    ini::IniMap map;
    map.Load(D3OutfitPresetPath());
    const std::string sec = PresetSection(name);
    map.RemovePrefix(sec + ".");
    for (i32 s = 0; s < 8; ++s) {
        const auto& os = outfit.slots[s];
        if (os.itemGbid != -1) {
            if (const auto* rec = items.FindByGbid(static_cast<u32>(os.itemGbid)))
                map.Set(sec + ".Slot" + std::to_string(s), rec->name);
        }
        if (os.dyeType != 0)
            map.Set(sec + ".Dye" + std::to_string(s), std::to_string(os.dyeType));
    }
    map.Set(sec + ".Sheathed", outfit.sheathed ? "1" : "0");
    map.Save(D3OutfitPresetPath());
    return true;
#else
    (void)name;
    return false;
#endif
}

bool ViewerApp::LoadD3OutfitPreset(std::string_view name) {
#if WDX_ENABLE_D3
    ini::IniMap map;
    map.Load(D3OutfitPresetPath());
    const std::string sec = PresetSection(name);
    bool any = false;
    for (i32 s = 0; s < 8; ++s) {
        const std::string* item = map.Get(sec + ".Slot" + std::to_string(s));
        // Unknown names report and skip — a preset from a newer snapshot must
        // not wipe the outfit it cannot fully express.
        if (item && !SetD3OutfitItem(s, *item))
            std::fprintf(stderr, "[d3-outfit] preset '%.*s': unknown item '%s'\n",
                         static_cast<int>(name.size()), name.data(), item->c_str());
        else if (item)
            any = true;
        if (!item)
            SetD3OutfitItem(s, "");
        const std::string* dye = map.Get(sec + ".Dye" + std::to_string(s));
        SetD3OutfitDye(s, dye ? std::atoi(dye->c_str()) : 0);
    }
    if (const std::string* sh = map.Get(sec + ".Sheathed"))
        SetD3OutfitSheathed(*sh == "1");
    return any;
#else
    (void)name;
    return false;
#endif
}

// The ragdoll switch goes straight to the adapter rather than through
// `D3Characters`: the rig is not a wardrobe, and the models that carry one are
// mostly not characters — 2,367 breakables against 570 skeletons.

bool ViewerApp::HasD3Ragdoll() const {
#if WDX_ENABLE_D3
    auto& svc = const_cast<ViewerApp*>(this)->service_;
    auto adapter = svc.Loader().D3AdapterOf(focusActor_);
    if (!adapter)
        return false;
    // A rig exists when some bone would get a dynamic body at the lod the
    // adapter builds at, which is exactly what makes it append a stage. Asked
    // of the adapter rather than recomputed here so the button and the stage
    // cannot disagree.
    //
    // That lod is 1, so this is true for the 570 models carrying a character
    // proxy and false for the 2,367 breakables — which is faithful: a breakable
    // collapses through the *other* builder in the client, and that one is PH3.
    return adapter->HasPhysicsRig();
#else
    return false;
#endif
}

bool ViewerApp::D3Ragdoll() const {
#if WDX_ENABLE_D3
    auto& svc = const_cast<ViewerApp*>(this)->service_;
    auto adapter = svc.Loader().D3AdapterOf(focusActor_);
    return adapter && adapter->IsRagdoll();
#else
    return false;
#endif
}

void ViewerApp::SetD3Ragdoll(bool on) {
#if WDX_ENABLE_D3
    if (auto adapter = service_.Loader().D3AdapterOf(focusActor_))
        adapter->SetRagdoll(on);
#else
    (void)on;
#endif
}

void ViewerApp::RestyleD3() {
#if WDX_ENABLE_D3
    // A reload is the fallback and not the path: what a character wears is not
    // a function of its pose or of where the camera is standing, and reloading
    // throws both away.
    if (!service_.Loader().RestyleD3Model(focusActor_) && !currentModelPath_.empty())
        LoadModelIntoActiveScene(currentModelPath_);
#endif
}

// ---- StarCraft II external animation files (`.m3a`) -------------------------
//
// A `.m3` names none of these — its chunk table has no path of any kind. The
// game reads them off the model's catalog entry and merges each into one global
// sequence space, binding tracks to the model by animId. Here the user does the
// naming; M3ModelAdapter does the merge.

namespace {

#if WDX_ENABLE_M3
// The focus actor's `.m3` source, or null when it is any other format. The
// adapter outlives the call: it is owned by the actor's AnimationDriver.
io::M3ModelAdapter* FocusM3Adapter(model::Actor* hero) {
    if (!hero || !hero->animation.HasSource())
        return nullptr;
    return dynamic_cast<io::M3ModelAdapter*>(hero->animation.Source().get());
}
#endif

} // namespace

bool ViewerApp::CanAttachAnimations() const {
#if WDX_ENABLE_M3
    return FocusM3Adapter(FocusActorPtr()) != nullptr;
#else
    return false;
#endif
}

std::vector<ViewerApp::AttachedAnimationInfo> ViewerApp::AttachedAnimations() const {
#if WDX_ENABLE_M3
    std::vector<AttachedAnimationInfo> out;
    if (io::M3ModelAdapter* m3 = FocusM3Adapter(FocusActorPtr())) {
        for (const auto& a : m3->AttachedAnimations())
            out.push_back({a.label, a.sequenceCount, a.firstSequence});
    }
    return out;
#else
    return {};
#endif
}

bool ViewerApp::AttachAnimationFile(const std::filesystem::path& path) {
#if WDX_ENABLE_M3
    model::Actor* hero = FocusActorPtr();
    io::M3ModelAdapter* m3 = FocusM3Adapter(hero);
    if (!m3)
        return false;

    // Loose file first, because that is what the picker hands over; then the
    // scene's provider, so a storage-relative name resolves against the same
    // CASC the model itself came out of.
    std::vector<u8> bytes;
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
        std::ifstream in(path, std::ios::binary);
        if (in)
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    } else if (auto* provider = service_.Scene().ActiveContentProvider()) {
        if (auto read = provider->ReadFile(ContentRef::FromPath(io::PathToUtf8(path))))
            bytes = std::move(*read);
    }
    if (bytes.empty())
        return false;

    if (!m3->AttachAnimationFile(io::PathToUtf8(path.stem()), bytes))
        return false;

    // Re-bind rather than poke the driver's cache: Bind is what re-reads
    // GetSequences() and rebuilds the pose stages, and the newly merged
    // sequences are invisible to playback until it runs.
    //
    // Not refreshed: the event configs the loader read at spawn time, so an
    // attached file's `Evt_Sound` cues need a reload to fire. Nothing else in
    // the actor depends on the animation file — no geometry, no bones, no
    // palette — because an `.m3a` contributes tracks and nothing more.
    hero->animation.Bind(hero->animation.Source());
    RefreshSequenceCache(hero, /*resetSelection*/ false);
    return true;
#else
    (void)path;
    return false;
#endif
}

// ---- Animation tracks and global loops -------------------------------------
//
// Both sit on the focus actor's ClipPlaylist, which is the renderer's own
// layering surface — the same one `ActorView::Play` exposes to an embedder.
// Nothing here reaches into the sampler.

bool ViewerApp::AddAnimTrack() {
    model::Actor* hero = FocusActorPtr();
    if (!hero || !hero->animation.HasSource())
        return false;
    AnimTrackInfo t;
    t.sequence = hero->animation.ActiveSequenceIndex();
    if (t.sequence < 0 || t.sequence >= static_cast<i32>(sequenceRanges_.size()))
        t.sequence = 0;
    animTracks_.push_back(t);
    animTrackHandles_.push_back(0);
    SetAnimTrack(animTracks_.size() - 1, t);
    return true;
}

void ViewerApp::SetAnimTrack(std::size_t index, const AnimTrackInfo& t) {
    if (index >= animTracks_.size())
        return;
    model::Actor* hero = FocusActorPtr();
    if (!hero || !hero->animation.HasSource())
        return;
    auto& playlist = hero->animation.Playlist();
    const AnimTrackInfo prev = animTracks_[index];
    animTracks_[index] = t;

    const bool restart =
        prev.sequence != t.sequence || prev.subtrack != t.subtrack || animTrackHandles_[index] == 0;
    if (!restart && playlist.Retune(animTrackHandles_[index], t.weight, t.speed, t.loop))
        return;

    // Either the animation itself changed or the play is gone (it ran out, or
    // a Bind rebuilt the stack). Replace it outright.
    if (animTrackHandles_[index] != 0)
        playlist.Stop(animTrackHandles_[index], 0, hero->cursor.actorTimeMs);
    renderer::animation::PlayDesc d;
    d.sequence = t.sequence;
    d.subtrack = t.subtrack;
    d.weight = t.weight;
    d.speed = t.speed;
    d.loop = t.loop;
    // Persistent, like every host-started play: the covered-play cull exists to
    // drop what a sequence switch buried, and a track the user added by hand is
    // not that. It also keeps the sequence dropdown working — the playlist
    // treats a stack of persistent plays as an empty foreground.
    d.persistent = true;
    animTrackHandles_[index] = playlist.Play(d, hero->cursor.actorTimeMs);
}

void ViewerApp::RemoveAnimTrack(std::size_t index) {
    if (index >= animTracks_.size())
        return;
    if (model::Actor* hero = FocusActorPtr(); hero && hero->animation.HasSource())
        hero->animation.Playlist().Stop(animTrackHandles_[index], 0, hero->cursor.actorTimeMs);
    animTracks_.erase(animTracks_.begin() + static_cast<std::ptrdiff_t>(index));
    animTrackHandles_.erase(animTrackHandles_.begin() + static_cast<std::ptrdiff_t>(index));
}

std::vector<ViewerApp::SubtrackInfo> ViewerApp::SubtracksOf(i32 sequence) const {
#if WDX_ENABLE_M3
    std::vector<SubtrackInfo> out;
    if (io::M3ModelAdapter* m3 = FocusM3Adapter(FocusActorPtr())) {
        for (const auto& s : m3->SubtracksOf(sequence))
            out.push_back({s.name, s.priority, s.concurrent, s.trackCount});
    }
    return out;
#else
    (void)sequence;
    return {};
#endif
}

std::vector<ViewerApp::GlobalLoopInfo> ViewerApp::GlobalLoops() const {
    std::vector<GlobalLoopInfo> out;
    for (std::size_t i = 0; i < sequenceRanges_.size(); ++i) {
        if (!sequenceRanges_[i].alwaysPlays)
            continue;
        const i32 seq = static_cast<i32>(i);
        const bool silenced = std::find(silencedGlobals_.begin(), silencedGlobals_.end(), seq) !=
                              silencedGlobals_.end();
        out.push_back({seq, sequenceNames_[i], !silenced});
    }
    return out;
}

void ViewerApp::SetGlobalLoopEnabled(i32 sequence, bool on) {
    const auto it = std::find(silencedGlobals_.begin(), silencedGlobals_.end(), sequence);
    if (on == (it == silencedGlobals_.end()))
        return;
    if (on)
        silencedGlobals_.erase(it);
    else
        silencedGlobals_.push_back(sequence);
    PublishGlobalLoops();
}

void ViewerApp::PublishGlobalLoops() {
    model::Actor* hero = FocusActorPtr();
    if (!hero || !hero->animation.HasSource())
        return;
    // The whole set, every time, rather than a stop on one play: the playlist
    // owns the global plays and reconciles against this list, so handing it a
    // subset is the only way to keep it from starting the silenced one straight
    // back. Reconciling also means the ones that stay keep their clock.
    std::vector<i32> live;
    for (std::size_t i = 0; i < sequenceRanges_.size(); ++i) {
        if (!sequenceRanges_[i].alwaysPlays)
            continue;
        const i32 seq = static_cast<i32>(i);
        if (std::find(silencedGlobals_.begin(), silencedGlobals_.end(), seq) ==
            silencedGlobals_.end())
            live.push_back(seq);
    }
    hero->animation.Playlist().SetGlobalSequences(std::move(live));
}

void ViewerApp::ReassertAnimTracks() {
    for (auto& h : animTrackHandles_)
        h = 0; // forces SetAnimTrack down its restart path
    for (std::size_t i = 0; i < animTracks_.size(); ++i)
        SetAnimTrack(i, AnimTrackInfo(animTracks_[i]));
}

bool ViewerApp::DetachAnimationFile(std::size_t index) {
#if WDX_ENABLE_M3
    model::Actor* hero = FocusActorPtr();
    io::M3ModelAdapter* m3 = FocusM3Adapter(hero);
    if (!m3 || !m3->DetachAnimationFile(index))
        return false;
    hero->animation.Bind(hero->animation.Source());
    // Detaching renumbers everything after the removed file, so the selection
    // is not merely clamped — it is meaningless. Back to the model's first.
    RefreshSequenceCache(hero, /*resetSelection*/ true);
    return true;
#else
    (void)index;
    return false;
#endif
}

bool ViewerApp::LoadModel(const std::filesystem::path& path) {
    // A .pkb / .pkfx isn't a model — it's one particle effect with no
    // animation list. Route it to the effect player so File > Open / CLI /
    // the startup picker all transparently accept effects too.
    if (IsEffectPath(path))
        return LoadEffect(path);

    // A path with no file behind it may still be a storage-internal one — the
    // CLI names `units/human/footman/footman.mdx` and the shared provider's
    // game storage resolves it, exactly as the draw-trace harness does. The
    // open itself is the probe: SpawnUnit fails cleanly when nothing resolves,
    // and only then is "file not found" the truthful message.
    const bool onDisk = std::filesystem::exists(path);

    // A `.wem` with nobody to ask: the CLI, the startup picker and a drop on
    // the window all reach here, and none of them has a dialog in front of it.
    // The document's own default profile is the answer, and it has to be
    // settled *before* the document opens — see OpenWemAs.
    if (onDisk && IsInterchangePath(path) && !pendingWemDocument_)
        return OpenWemAs(path, nullptr, ::whiteout::models::wem::ProfileId::Count);

    if (OpenDocument(path, /*effect=*/false))
        return true;
    if (!onDisk)
        std::fprintf(stderr, "[viewer] file not found: %s\n", io::PathToUtf8(path).c_str());
    return false;
}

void ViewerApp::PreloadForDocumentAsync(std::function<void()> then) {
    io::FileContentProvider& provider = service_.DefaultScene().GetContentProvider();

    // Resolved on the host thread, not in the body: both are lazily
    // constructed, and the task thread must not be what constructs them.
    profiles::wow::WowReplaceableTextures* skins = nullptr;
    profiles::wow::WowCharacterAppearance* chars = nullptr;
#if WDX_ENABLE_M2
    if (provider.Game() == ProductId::Wow && !wowTablesPrewarmed_) {
        skins = &service_.Loader().WowReplaceables();
        chars = &service_.Loader().WowCharacters();
        skins->SetContentProvider(&provider);
        chars->SetContentProvider(&provider);
    }
#endif

    io::FileContentProvider* p = &provider;
    const bool tables = chars != nullptr;
    tasks_.Run(
        std::string("Opening ") + GameDisplayName(provider.Game()),
        [p, skins, chars, tables](io::ProgressMonitor& m) {
            // Weighted by what actually costs: the install open is the long
            // pole, the fourteen database reads behind it are the rest.
            m.Begin("Preparing", tables ? 100u : 70u);
            {
                io::ProgressMonitor step = m.Split(70);
                step.Begin("Waiting for storage");
                // A read on a provider worker may already be opening it. Wait
                // that out rather than queueing a second open behind it — and
                // report while waiting, which is the whole point.
                while (p->StoragesState() == io::StorageState::Opening) {
                    if (m.Cancelled())
                        return io::TaskResult::Fail("Cancelled");
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
                // Demanded, so a previously cancelled open is retried. Its own
                // plan replaces the placeholder stage above.
                p->OpenStorages(&step);
            }
            if (m.Cancelled())
                return io::TaskResult::Fail("Cancelled");

            if (chars) {
                // Character customisation is fourteen tables against the skin
                // tables' four, and is the one that actually hurts.
                {
                    io::ProgressMonitor step = m.Split(22);
                    chars->Prewarm(&step);
                }
                if (!m.Cancelled()) {
                    io::ProgressMonitor step = m.Split(8);
                    skins->Prewarm(&step);
                }
            }
            // Always Ok. A storage that would not open and an install that
            // cannot serve the databases are both normal states; the document
            // opens either way and reports its own miss, which is a better
            // answer than an error box in front of a load the user asked for.
            return io::TaskResult::Ok();
        },
        [this, tables, then = std::move(then)](const io::TaskOutcome& out) {
            // Tried, whether or not it worked — see wowTablesPrewarmed_.
            if (tables && !out.cancelled)
                wowTablesPrewarmed_ = true;
            // Assets that missed while nothing was open get another chance.
            service_.RetryUnloadedAssets();
            if (then)
                then();
        });
}

void ViewerApp::QueueInitialOpen(const std::filesystem::path& path) {
    pendingInitialOpens_.push_back(path);
}

bool ViewerApp::OpenModelAsync(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        std::fprintf(stderr, "[viewer] file not found: %s\n", io::PathToUtf8(path).c_str());
        return false;
    }
    const bool effect = IsEffectPath(path);

    // The game FIRST. Which storage this load needs is decided by the model's
    // extension, so asking about the slot before this would ask about the
    // wrong one — an `.m2` opened while the provider is still on Warcraft III
    // would report Warcraft III's storage as the thing to wait for.
    FollowModelGame(path);

    io::FileContentProvider& provider = service_.DefaultScene().GetContentProvider();
    const io::StorageState state = provider.StoragesState();
    const bool needStorage = state != io::StorageState::Open && state != io::StorageState::Failed;
    // The databases count too. The spawn reads them synchronously through
    // WowReplaceableTextures::Apply, so a document opened before they are in
    // hand freezes the host thread for fourteen CASC reads — which is most of
    // what "opening a World of Warcraft model hangs" actually was.
    const bool needTables = provider.Game() == ProductId::Wow && !wowTablesPrewarmed_;

    if (!needStorage && !needTables)
        return OpenDocument(path, effect); // nothing to wait for; behave as before

    PreloadForDocumentAsync([this, path, effect] {
        // Opened, failed or cancelled — the document opens either way. A model
        // that needs no storage (a loose `.mdx` beside its textures) still
        // loads, and one that does reports its own miss rather than being
        // silently dropped because the install did not come up.
        OpenDocument(path, effect);
    });
    return true;
}

bool ViewerApp::LoadEffect(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        std::fprintf(stderr, "[viewer] file not found: %s\n", io::PathToUtf8(path).c_str());
        return false;
    }
    return OpenDocument(path, /*effect=*/true);
}

void ViewerApp::RestoreActiveAfterFailedOpen(i32 prevDoc) {
    if (prevDoc >= 0) {
        activeDoc_ = prevDoc;
        LoadActiveDocState();
        service_.SetActiveScene(documents_[prevDoc].scene);
    } else {
        ClearWorkingState();
        service_.SetActiveScene(service_.DefaultSceneId());
    }
}

bool ViewerApp::OpenDocumentScene(std::shared_ptr<io::IContentProvider> provider, std::string title,
                                  const std::function<bool()>& loadBody) {
    // Preserve the outgoing tab's live state before the flat working members get
    // reused for the new document.
    const i32 prevDoc = activeDoc_;
    if (prevDoc >= 0)
        SaveActiveDocState();

    // Every document owns its own scene, bound to the caller's provider.
    const SceneId sid = service_.CreateScene();
    service_.SceneAt(sid).SetContentProvider(std::move(provider));
    service_.SetActiveScene(sid);

    ClearWorkingState();
    if (!loadBody()) {
        service_.DestroyScene(sid); // discard the empty scene, re-activate prev tab
        RestoreActiveAfterFailedOpen(prevDoc);
        return false;
    }

    Document doc;
    doc.scene = sid;
    doc.title = std::move(title);
    documents_.push_back(std::move(doc));
    activeDoc_ = static_cast<i32>(documents_.size()) - 1;
    SaveActiveDocState();           // persist the freshly-loaded flat state
    pendingTabSelect_ = activeDoc_; // the tab bar must select this new tab
    return true;
}

// Point the shared provider at the game a model file belongs to.
//
// Which game the provider serves is what decides which storage resolves the
// model's textures, and every document shares one provider whose game comes
// from Settings. So an `.m2` opened while that says Warcraft III loads its
// geometry — the `.skin` sits next to it on disk — and then silently loses
// every texture, because those are fileDataIDs and no WoW storage is open.
//
// Warcraft III is in the map for the same reason and not as a formality: with
// the panel left on StarCraft II or World of Warcraft, an `.mdx` loses its CASC
// textures, the day/night rig, the IBL probes and every event SLK — all of them
// read during the load, through whichever storage is active then. That leaves a
// Reforged model lit by the procedural fallback probe and wearing the white
// placeholder, and switching the panel back does not repair it, because those
// are load-time decisions.
//
// Deliberately not persisted: the user did not pick this, they opened a file.
// The Settings panel reads its selection off the provider, so it still shows
// the truth for the session, and the next launch is back to their choice.
void ViewerApp::FollowModelGame(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    ProductId game = ProductId::Neutral;
    if (ext == ".m2")
        game = ProductId::Wow;
    else if (ext == ".m3")
        game = ProductId::Sc2;
    else if (ext == ".acr" || ext == ".app")
        game = ProductId::D3;
    else if (ext == ".mdx" || ext == ".mdl" || ext == ".pkb" || ext == ".pkfx")
        game = ProductId::Wc3;
    else if (ext == ".wem" || ext == ".gltf" || ext == ".glb")
        // The suffix says nothing here: one `.wem` can be opened as any profile
        // it carries, and the profile is what decides which game's storage its
        // textures resolve against. The pick is already made by the time a
        // document opens — the dialog made it, or DefaultWemProfile did. A
        // glTF rides the same rule: its import derives into Reforged, so the
        // answer is Warcraft III.
        game = io::ProductForWemProfile(pendingWemProfile_);
    if (game == ProductId::Neutral)
        return;

    // Content is what makes a profile *needed*, and this is the only path that
    // moves the provider onto one. Selecting a profile in Settings does not:
    // that is a choice of what to configure, and configuring a game the user is
    // not reading has no business opening its install.
    settingsProfile_ = game;
    auto& provider = service_.DefaultScene().GetContentProvider();
    if (provider.Game() != game) {
        ApplyProfile(game, /*force=*/false);
        // The load that follows is about to demand this storage anyway, so
        // retrying here costs no open that was not already coming — and assets
        // that missed under the old game get their chance under the new one.
        service_.RetryUnloadedAssets();
    }
    if (game == ProductId::Wow)
        AdoptNearbyWowKeys(path);
}

// A loose `.m2` tree was extracted from an id-keyed root by *something*, and
// that something needed a listfile — so one is usually sitting a directory or
// two above the model, next to the TACT key list the same extraction needed.
// Adopting them turns the extraction back into a storage that knows its own
// names and can read the client databases, which is the difference between a
// creature wearing the skin CreatureDisplayInfo names and wearing one picked
// off its folder (see WowReplaceableTextures).
//
// Session-only and never persisted, exactly like the game switch above: the
// user opened a file, they did not pick either of these. An explicit path from
// Settings > IO always wins.
void ViewerApp::AdoptNearbyWowKeys(const std::filesystem::path& modelPath) {
    auto& provider = service_.DefaultScene().GetContentProvider();
    bool wantListfile = provider.ListfilePath().empty();
    bool wantKeys = provider.TactKeyPath().empty();

    std::error_code ec;
    std::filesystem::path dir = modelPath.parent_path();
    for (int up = 0; up < 5 && (wantListfile || wantKeys); ++up) {
        for (const auto& entry : std::filesystem::directory_iterator(
                 dir, std::filesystem::directory_options::skip_permission_denied, ec)) {
            const std::string name = LowerAscii(io::PathToUtf8(entry.path().filename()));
            const bool isListfile = wantListfile && name.ends_with(".csv") &&
                                    name.find("listfile") != std::string::npos;
            const bool isKeys =
                wantKeys && name.ends_with(".txt") && name.find("tactkey") != std::string::npos;
            if (!isListfile && !isKeys)
                continue;
            std::fprintf(stderr, "[viewer] adopting %s beside the content: %s\n",
                         isListfile ? "listfile" : "TACT keys",
                         io::PathToUtf8(entry.path()).c_str());
            if (isListfile) {
                provider.SetListfilePath(entry.path());
                wantListfile = false;
            } else {
                provider.SetTactKeyPath(entry.path());
                wantKeys = false;
            }
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir)
            break;
        dir = parent;
    }
}

bool ViewerApp::OpenDocument(const std::filesystem::path& path, bool effect) {
    FollowModelGame(path);
    // All documents share one configured game provider so the CASC/MPQ/install
    // set is identical across tabs.
    return OpenDocumentScene(SharedProvider(), path.stem().string(), [&] {
        // SetPE1BasePath on a scene with an external provider only updates the
        // template cache's base path, not the (shared) provider's — set the
        // provider's local-file root directly so sibling textures resolve.
        service_.DefaultScene().GetContentProvider().SetBasePath(path.parent_path());
        return effect ? LoadEffectIntoActiveScene(path) : LoadModelIntoActiveScene(path);
    });
}

bool ViewerApp::OpenStorageDocument(const std::string& archivePath, bool effect,
                                    std::shared_ptr<io::IContentProvider> provider) {
    // The Storage Explorer's provider opens its storage on first read — and for
    // a double-clicked model that read is this load, so the host thread would
    // sit in Wait() through an install open with nothing on screen to say so.
    // The panel's own open put a bar in front of the browse; this puts one in
    // front of the spawn.
    if (auto* fp = dynamic_cast<io::FileContentProvider*>(provider.get())) {
        const io::StorageState state = fp->StoragesState();
        if (state != io::StorageState::Open && state != io::StorageState::Failed) {
            RunStorageOpenTask(*fp, [this, archivePath, effect, provider](bool) {
                OpenStorageDocumentNow(archivePath, effect, provider);
            });
            return true;
        }
    }
    return OpenStorageDocumentNow(archivePath, effect, std::move(provider));
}

bool ViewerApp::OpenStorageDocumentNow(const std::string& archivePath, bool effect,
                                       std::shared_ptr<io::IContentProvider> provider) {
    const std::filesystem::path apath(archivePath);
    // The doc scene reads through the EXPLORER's CASC provider, so the model
    // resolves from the same storage the user is browsing.
    return OpenDocumentScene(provider, apath.stem().string(), [&] {
        service_.Scene().SetPE1BasePath(apath.parent_path());

        // HD-ness from the archive location (there's no filesystem MDX to
        // probe). Arm the scene's mode before spawn so the parse resolves
        // through the right overlay — ApplyRenderMode routes to the scene,
        // which forwards the overlay to its provider itself.
        const bool hd = archivePath.find("_hd.w3mod") != std::string::npos;
        ApplyRenderMode(hd ? RenderMode::HD : RenderMode::SD);

        service_.Loader().RequestClearAll();
        currentModelPath_ = apath;
        model::Actor* hero = effect ? service_.Loader().SpawnUnitFromSource(
                                          std::make_shared<model::CornEffectSource>(archivePath))
                                    : service_.Loader().SpawnUnit(archivePath);
        if (!hero) {
            std::fprintf(stderr, "[viewer] storage open FAILED for %s\n", archivePath.c_str());
            return false;
        }
        if (effect)
            FillEffectDocState(hero);
        else
            FillModelDocState(hero, apath);
        // The direct-open path gets this through FollowModelGame /
        // OpenStoragesAsync; this path never did. Assets the browse already
        // queued could drain through the WRONG viewport's provider — with no
        // document open, the main viewport pumps the shared needs queue
        // through the default scene's provider, which on a fresh session has
        // no storage open and no HD overlay — and a failed drain is dropped
        // for good (Acquire dedups, so the document's own Acquire of the same
        // ref just returns the dead slot). Re-queue them now that this scene,
        // with the right provider and render mode, is what pumps next.
        service_.RetryUnloadedAssets();
        return true;
    });
}

bool ViewerApp::LoadModelIntoActiveScene(const std::filesystem::path& path) {
    // Record the loaded path so CurrentModelPath() is accurate regardless of
    // entry point (CLI, startup picker, or File > Open). Save As reads it back.
    currentModelPath_ = path;

    service_.Scene().SetPE1BasePath(path.parent_path());

    // Decide the render mode BEFORE SpawnUnit. service_.Loader().SpawnUnit
    // synchronously triggers SLK loads, splat-texture prefetches, and the
    // BLS shader path selection — each of which consults the current
    // RenderMode and caches its result. If we waited until after SpawnUnit
    // to flip the mode, those caches would already be primed for the wrong
    // mode (e.g. SD splat textures pinned for an HD model). To avoid
    // pulling in the full ModelTemplate machinery just to inspect material
    // shader IDs, parse the MDX directly through WhiteoutLib and walk
    // material layers — any non-`SD` ShaderType means a Reforged HD layer
    // (Reforged shipping models tag their classic-on-HD path as `SDOnHD`,
    // which also counts as HD here per the user-set render mode).
    //
    // Skipped entirely for a non-WC3 model: the probe would throw on the first
    // chunk, and "HD" is a Warcraft III distinction that means nothing to an
    // .m2 or .m3 anyway — their frame is chosen by the scene's ProductId, which
    // SpawnUnit sets from the magic it just sniffed.
    if (IsInterchangePath(path)) {
        // The render mode comes from the PROFILE, not from a probe: the HD
        // question is "is this Reforged", and for a `.wem` the answer is which
        // of the two Warcraft III profiles was picked. Every other profile is
        // SD here for the same reason `.m2` and `.m3` are — HD is a Warcraft
        // III distinction and their frame is chosen by the scene's product.
        ApplyRenderMode((forceHd_ || io::WemProfileIsHd(pendingWemProfile_)) ? RenderMode::HD
                                                                             : RenderMode::SD);
        service_.Loader().RequestClearAll();

        std::shared_ptr<io::WemDocument> document = pendingWemDocument_;
        if (!document)
            document = IsGltfPath(path) ? io::ParseGltfFile(path) : io::ParseWemFile(path);
        if (!document) {
            std::fprintf(stderr, "[viewer] '%s' is not a file this build can read\n",
                         io::PathToUtf8(path).c_str());
            return false;
        }
        model::Actor* wemHero = service_.Loader().SpawnWemDocument(*document, pendingWemProfile_);
        if (!wemHero) {
            std::fprintf(stderr, "[viewer] SpawnWemDocument FAILED for %s\n",
                         io::PathToUtf8(path).c_str());
            return false;
        }
        FillModelDocState(wemHero, path);
        return true;
    }

    if (IsForeignModelPath(path)) {
        ApplyRenderMode(RenderMode::SD);
    } else {
        bool anyHdLayer = false;
        try {
            whiteout::mdx::Parser parser;
            whiteout::mdx::Model probe = parser.parse(io::PathToUtf8(path));
            for (const auto& mat : probe.materials) {
                for (const auto& layer : mat.layers) {
                    if (layer.shader != whiteout::mdx::Layer::ShaderType::SD) {
                        anyHdLayer = true;
                        break;
                    }
                }
                if (anyHdLayer)
                    break;
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[viewer] HD-probe parse FAILED for %s: %s (continuing in SD)\n",
                         io::PathToUtf8(path).c_str(), e.what());
        }
        // "Reforged Graphics" forces HD regardless of the probe result.
        ApplyRenderMode((forceHd_ || anyHdLayer) ? RenderMode::HD : RenderMode::SD);
    }

    service_.Loader().RequestClearAll();
    model::Actor* hero = service_.Loader().SpawnUnit(io::PathToUtf8(path));
    if (!hero) {
        std::fprintf(stderr, "[viewer] SpawnUnit FAILED for %s\n", io::PathToUtf8(path).c_str());
        // The likeliest cause for these two, and one "SpawnUnit FAILED" alone
        // sends the reader looking for a corrupt file instead of a build flag.
        const std::string ext = LowerExt(path);
        if (ext == ".m2" && !kM2Compiled)
            std::fprintf(stderr, "[viewer]   .m2 support is not compiled in — configure with "
                                 "-DWDX_ENABLE_M2=ON\n");
        if (ext == ".m3" && !kM3Compiled)
            std::fprintf(stderr, "[viewer]   .m3 support is not compiled in — configure with "
                                 "-DWDX_ENABLE_M3=ON\n");
        return false;
    }
    FillModelDocState(hero, path);
    return true;
}

void ViewerApp::RefreshSequenceCache(model::Actor* hero, bool resetSelection) {
    if (!hero)
        return;
    auto sequences = hero->animation.Sequences();
    sequenceNames_.clear();
    sequenceRanges_.clear();
    sequenceNames_.reserve(sequences.size());
    sequenceRanges_.reserve(sequences.size());
    for (auto& s : sequences) {
        sequenceNames_.push_back(s.name);
        sequenceRanges_.push_back(s);
    }
    // `resetSelection` means the indices themselves changed meaning — a fresh
    // model, or a detach that renumbered everything after it — so the tracks
    // and the silenced-globals list go with the selection. An attach only
    // appends, so those survive it; what does not survive is the playlist,
    // which `Bind` rebuilt on the way here, taking every handle with it.
    if (resetSelection) {
        animTracks_.clear();
        animTrackHandles_.clear();
        silencedGlobals_.clear();
    } else {
        ReassertAnimTracks();
        PublishGlobalLoops();
    }

    if (sequences.empty())
        return;
    const i32 count = static_cast<i32>(sequences.size());
    const i32 current = hero->animation.ActiveSequenceIndex();
    if (resetSelection || current < 0 || current >= count)
        hero->animation.SetActiveSequenceIndex(0);
}

void ViewerApp::FillModelDocState(model::Actor* hero, const std::filesystem::path& path) {
    if (!hero)
        return;
    focusActor_ = hero->handle;
    hero->ignoreNonLooping = loopNonLoopingPolicy_;

    RefreshSequenceCache(hero, /*resetSelection*/ true);
    FrameCameraToModel(hero);

    cameraPresets_.clear();
    if (hero->sourceTemplate)
        cameraPresets_ = hero->sourceTemplate->cameraPresets;
    cameraPresetNamesUtf8_.clear();
    cameraPresetNamesUtf8_.reserve(cameraPresets_.size());
    for (const auto& p : cameraPresets_)
        cameraPresetNamesUtf8_.push_back(p.name); // CameraPreset::name is already UTF-8.
    activeCameraPresetIdx_ = -1;
    cameraLocked_ = false;
    walkDriftPrevSeqIdx_ = -1;
    walkDriftAccumulated_ = 0.0f;

    currentModelPath_ = path;
}

bool ViewerApp::LoadEffectIntoActiveScene(const std::filesystem::path& path) {
    currentModelPath_ = path;
    // PopcornFX (.pkb/.pkfx) is Reforged-only content — always render it in HD,
    // regardless of the prior document's mode or the Reforged-Graphics toggle.
    ApplyRenderMode(RenderMode::HD);
    // Textures the .pkb references resolve against its own directory.
    service_.Scene().SetPE1BasePath(path.parent_path());

    service_.Loader().RequestClearAll();
    auto source = std::make_shared<model::CornEffectSource>(io::PathToUtf8(path));
    model::Actor* hero = service_.Loader().SpawnUnitFromSource(source);
    if (!hero) {
        std::fprintf(stderr, "[viewer] effect spawn FAILED for %s\n", io::PathToUtf8(path).c_str());
        return false;
    }
    FillEffectDocState(hero);
    return true;
}

void ViewerApp::FillEffectDocState(model::Actor* hero) {
    if (!hero)
        return;
    focusActor_ = hero->handle;
    hero->ignoreNonLooping = loopNonLoopingPolicy_;

    // The source exposes one placeholder "Effect" sequence — mirror it into the
    // UI dropdowns like a model so the sequence picker stays consistent.
    auto sequences = hero->animation.Sequences();
    sequenceNames_.clear();
    sequenceRanges_.clear();
    sequenceNames_.reserve(sequences.size());
    sequenceRanges_.reserve(sequences.size());
    for (auto& s : sequences) {
        sequenceNames_.push_back(s.name);
        sequenceRanges_.push_back(s);
    }
    if (!sequences.empty())
        hero->animation.SetActiveSequenceIndex(0);

    // PKB effects have no mesh bounds, and the PopcornFX runtime extents
    // aren't known until the sim has run a few frames. Seed a provisional
    // pose now (origin, moderate distance) so the first frames aren't framed
    // blind, then let Tick reframe to the real particle cloud via
    // FrameCameraToEffect once it develops (effectFrameTicks_).
    {
        auto& cam = service_.Scene().Camera();
        cam.SetOrbitalMode();
        cam.SetTarget(Vector3f{0.0f, 0.0f, 0.0f});
        cam.SetYaw(Camera::kDefaultYaw - 0.785398f); // 3/4 view, like FrameCameraToModel
        cam.SetPitch(0.6f);
        cam.SetDistance(100.0f);
        cam.SetFovDiagonal(Camera::kDefaultFovDiagonal);
        cam.SetClip(Camera::kDefaultNearZ, Camera::kDefaultFarZ);
    }
    effectFrameTicks_ = 0;

    // Effects carry no camera presets.
    cameraPresets_.clear();
    cameraPresetNamesUtf8_.clear();
    activeCameraPresetIdx_ = -1;
    cameraLocked_ = false;
    walkDriftPrevSeqIdx_ = -1;
    walkDriftAccumulated_ = 0.0f;
}

void ViewerApp::SetForceHd(bool on) {
    if (forceHd_ == on)
        return;
    forceHd_ = on;
    // Forcing a mode IS scripting it: while the force stands, the loader must
    // not true the scene up to the parsed template, or an SD model would snap
    // straight back to SD on load.
    service_.Settings().SetFollowModelRenderMode(!on);
    // Reload the active model so it re-probes (forced HD vs detected) and its
    // deps re-resolve under the new CASC overlay. Empty path ⇒ nothing loaded;
    // the next load picks it up. Effects (.pkb/.pkfx) are mode-agnostic and
    // can't be re-probed as MDX; `.m2`/`.m3` render through a profile the
    // scene's product picks, which "Reforged Graphics" does not touch. Both
    // would be a reload with no possible effect, so leave them be.
    const std::filesystem::path path = currentModelPath_;
    if (path.empty() || IsEffectPath(path) || IsForeignModelPath(path))
        return;
    LoadModelIntoActiveScene(path);
}

void ViewerApp::ApplyRenderMode(RenderMode wanted) {
    const bool modeFlipped = (service_.EffectiveRenderMode() != wanted);
    // The ACTIVE SCENE owns its mode now — the pipeline, the loader's latches
    // and the CASC overlay all read it there (SceneManager::SetRenderMode
    // forwards the overlay itself, which is why the explicit SetHdMode this
    // function used to carry is gone). The global stays in step as the
    // fallback for unsettled scenes and as the ini-persisted preference.
    service_.Scene().SetRenderMode(wanted);
    service_.Settings().SetRenderMode(wanted);
    if (!modeFlipped)
        return;

    // The remaining side effects of a mode flip: splat / SLK caches keyed
    // under the old mode have to move before any subsequent event-data fetch
    // resolves under the new one.
    auto* p = service_.Scene().ActiveContentProvider();
    // Nothing cached under the old mode means nothing to re-resolve, and the
    // tables are loaded on demand by the first Warcraft III model — forcing
    // them in here would open that game's install for a mode flip alone.
    if (!p || !io::IsSplCachePopulated())
        return;
    // Kill any splats currently alive — each one holds a refcount on an
    // AssetManager slot keyed by the old-mode texture; without releasing them
    // the hand-over below only bumps the same stale handle.
    service_.Splats().Clear();
    // Move the event-data tables and the splat prefetch onto the new mode.
    // The tables are kept per mode, so flipping back to one this session has
    // already been in re-reads no SLKs — only the textures, whose slots are
    // keyed by path alone and so cannot stay resident across the flip.
    io::SyncEventDataMode(p, service_.Assets());
}

std::shared_ptr<io::IContentProvider> ViewerApp::SharedProvider() {
    if (!sharedProvider_) {
        // Alias the default scene's configured FileContentProvider without
        // owning it (no-op deleter) — every document scene shares this one
        // provider, so they all see the same CASC/MPQ/install configuration.
        io::IContentProvider* p = &service_.DefaultScene().GetContentProvider();
        sharedProvider_ = std::shared_ptr<io::IContentProvider>(p, [](io::IContentProvider*) {});
    }
    return sharedProvider_;
}

SceneId ViewerApp::ActiveSceneId() const {
    if (activeDoc_ >= 0 && activeDoc_ < static_cast<i32>(documents_.size()))
        return documents_[activeDoc_].scene;
    return service_.DefaultSceneId();
}

void ViewerApp::PublishActiveScene() {
    service_.SetActiveScene(ActiveSceneId());
}

void ViewerApp::SaveActiveDocState() {
    if (activeDoc_ < 0 || activeDoc_ >= static_cast<i32>(documents_.size()))
        return;
    Document& d = documents_[activeDoc_];
    d.modelPath = currentModelPath_;
    d.focusActor = focusActor_;
    d.sequenceNames = sequenceNames_;
    d.sequenceRanges = sequenceRanges_;
    d.animTracks = animTracks_;
    d.animTrackHandles = animTrackHandles_;
    d.silencedGlobals = silencedGlobals_;
    d.cameraPresets = cameraPresets_;
    d.cameraPresetNamesUtf8 = cameraPresetNamesUtf8_;
    d.activeCameraPresetIdx = activeCameraPresetIdx_;
    d.cameraLocked = cameraLocked_;
    d.walkDriftPrevSeqIdx = walkDriftPrevSeqIdx_;
    d.walkDriftAccumulated = walkDriftAccumulated_;
    d.effectFrameTicks = effectFrameTicks_;
    d.lastParentTimeMs = lastParentTimeMs_;
    // The render mode is not mirrored: the document's SCENE carries its own
    // (SceneManager::SetRenderMode), which the tab switch reads back.
}

void ViewerApp::LoadActiveDocState() {
    if (activeDoc_ < 0 || activeDoc_ >= static_cast<i32>(documents_.size()))
        return;
    const Document& d = documents_[activeDoc_];
    currentModelPath_ = d.modelPath;
    focusActor_ = d.focusActor;
    sequenceNames_ = d.sequenceNames;
    sequenceRanges_ = d.sequenceRanges;
    // Handles come back with the tracks because the playlist they name belongs
    // to the document's own actor, which the tab switch left running.
    animTracks_ = d.animTracks;
    animTrackHandles_ = d.animTrackHandles;
    silencedGlobals_ = d.silencedGlobals;
    cameraPresets_ = d.cameraPresets;
    cameraPresetNamesUtf8_ = d.cameraPresetNamesUtf8;
    activeCameraPresetIdx_ = d.activeCameraPresetIdx;
    cameraLocked_ = d.cameraLocked;
    walkDriftPrevSeqIdx_ = d.walkDriftPrevSeqIdx;
    walkDriftAccumulated_ = d.walkDriftAccumulated;
    effectFrameTicks_ = d.effectFrameTicks;
    lastParentTimeMs_ = d.lastParentTimeMs;
    // Render mode (d.renderMode) is re-applied by the caller via ApplyRenderMode
    // so the splat / event-data side effects run only when it actually changes.
}

void ViewerApp::ClearWorkingState() {
    focusActor_ = 0;
    currentModelPath_.clear();
    sequenceNames_.clear();
    sequenceRanges_.clear();
    animTracks_.clear();
    animTrackHandles_.clear();
    silencedGlobals_.clear();
    cameraPresets_.clear();
    cameraPresetNamesUtf8_.clear();
    activeCameraPresetIdx_ = -1;
    cameraLocked_ = false;
    walkDriftPrevSeqIdx_ = -1;
    walkDriftAccumulated_ = 0.0f;
    effectFrameTicks_ = -1;
    lastParentTimeMs_ = 0;
}

const std::string& ViewerApp::DocumentTitle(i32 idx) const {
    static const std::string kEmpty;
    if (idx < 0 || idx >= static_cast<i32>(documents_.size()))
        return kEmpty;
    return documents_[idx].title;
}

i32 ViewerApp::ConsumePendingTabSelect() {
    const i32 v = pendingTabSelect_;
    pendingTabSelect_ = -1;
    return v;
}

void ViewerApp::SetActiveDocument(i32 idx) {
    if (idx < 0 || idx >= static_cast<i32>(documents_.size()) || idx == activeDoc_)
        return;
    if (activeDoc_ >= 0)
        SaveActiveDocState();
    activeDoc_ = idx;
    LoadActiveDocState();
    service_.SetActiveScene(documents_[idx].scene);
    // The scene carries its own mode; re-applying it here only runs the
    // splat / event-data sync when the mode actually changed between docs.
    ApplyRenderMode(service_.EffectiveRenderMode(service_.SceneAt(documents_[idx].scene)));
}

void ViewerApp::CloseDocument(i32 idx) {
    if (idx < 0 || idx >= static_cast<i32>(documents_.size()))
        return;
    const SceneId sid = documents_[idx].scene;
    const bool closingActive = (idx == activeDoc_);

    // Drop the scene (actors + GPU state). DestroyScene falls back to the
    // default scene if this was the active one; we re-point below.
    service_.DestroyScene(sid);
    documents_.erase(documents_.begin() + idx);

    if (documents_.empty()) {
        activeDoc_ = -1;
        ClearWorkingState();
        service_.SetActiveScene(service_.DefaultSceneId());
        return;
    }
    if (closingActive) {
        // The flat members still mirror the now-closed doc; overwrite them with
        // the neighbour that takes focus (no save — the closed state is gone).
        activeDoc_ = std::min<i32>(idx, static_cast<i32>(documents_.size()) - 1);
        LoadActiveDocState();
        service_.SetActiveScene(documents_[activeDoc_].scene);
        ApplyRenderMode(
            service_.EffectiveRenderMode(service_.SceneAt(documents_[activeDoc_].scene)));
        pendingTabSelect_ = activeDoc_; // tell the tab bar which neighbour won
    } else if (idx < activeDoc_) {
        --activeDoc_; // our slot shifted left
    }
}

void ViewerApp::SetStorageExplorerOpen(bool on) {
    storageExplorerOpen_ = on;
    if (!on)
        return;
    if (!storageExplorer_) {
        storageExplorer_ = std::make_unique<tools::StorageExplorer>(service_);
        // Its opens go on the viewer's task thread, behind the viewer's modal.
        // A StarCraft II switch walks three quarters of a million manifest
        // entries, which used to run inside the panel's own game combo.
        storageExplorer_->SetTaskRunner(&tasks_);
        // Double-clicking a model opens it as a new tab, loaded from the
        // explorer's CASC provider (the viewer wants the path, not the bytes —
        // it spawns through the same provider).
        storageExplorer_->SetOnActivate([this](const tools::ActivatedFile& f) {
            OpenStorageDocument(f.path, f.isEffect, f.provider);
        });
        // Where the panel gets a product's install path and (for World of
        // Warcraft, where it is the difference between a browse and an empty
        // grid) its listfile. Answered per product and on demand, because
        // neither is a fact the viewer holds in one place at one time: the live
        // provider is authoritative for the game it is currently serving —
        // Settings ▸ IO edits and the session-only keys AdoptNearbyWowKeys
        // picks up beside a loose model both land there — while the ini is the
        // only record of the games it is not on, which the panel's own game
        // combo can still browse.
        storageExplorer_->SetGameKeys([this](ProductId game) {
            auto& provider = service_.DefaultScene().GetContentProvider();
            tools::GameStorageKeys keys;
            if (provider.Game() == game) {
                keys.installPath = provider.InstallPath();
                keys.listfilePath = provider.ListfilePath();
                keys.tactKeyPath = provider.TactKeyPath();
            } else {
                const IoPathOverrides o = LoadIoPathOverrides(game);
                keys.installPath = o.installPath;
                keys.listfilePath = o.listfilePath;
                keys.tactKeyPath = o.tactKeyPath;
            }
            return keys;
        });
        // Before Sync, which prefers the restored game over settingsProfile_:
        // where the panel points is the user's own setting, and last session's
        // answer is a better one than the host's current profile.
        storageExplorer_->RestoreState(LoadStorageExplorerState());
        explorerStateKey_.clear();
    }
    // Every show, not only the first. The panel outlives any one of these
    // settings: a listfile adopted beside a model opened after the panel was
    // first built used to never reach it. Sync reopens only when what the host
    // knows has actually moved, so the folder the user was browsing survives a
    // close/reopen.
    storageExplorer_->Sync(settingsProfile_);
}

// Persist where the panel was left, once it stops moving. Only while it is
// open AND settled: a panel mid-open describes the storage it is leaving, and
// saving that would hand the next session a folder from the wrong game.
void ViewerApp::PollStorageExplorerState(f32 dt) {
    if (!storageExplorer_->IsOpen() || storageExplorer_->Opening())
        return;
    constexpr f32 kSettle = 1.0f; // seconds
    std::string key = ExplorerStateKey(storageExplorer_->State());
    if (key != explorerStateKey_) {
        explorerStateKey_ = std::move(key);
        explorerSaveDelay_ = kSettle;
        return;
    }
    if (explorerSaveDelay_ <= 0.0f)
        return;
    explorerSaveDelay_ -= dt;
    if (explorerSaveDelay_ <= 0.0f)
        SaveStorageExplorerState(storageExplorer_->State());
}

void ViewerApp::BuildStorageExplorerWindow() {
    if (storageExplorerOpen_ && storageExplorer_)
        storageExplorer_->BuildWindow(&storageExplorerOpen_);
}

void ViewerApp::ActivateCameraPreset(i32 idx) {
    auto& cam = service_.Scene().Camera();

    if (idx < 0 || idx >= static_cast<i32>(cameraPresets_.size())) {
        activeCameraPresetIdx_ = -1;
        cameraLocked_ = false;
        cam.SetOrbitalMode();
        cam.SetFovDiagonal(Camera::kDefaultFovDiagonal);
        cam.SetClip(Camera::kDefaultNearZ, Camera::kDefaultFarZ);
        return;
    }

    activeCameraPresetIdx_ = idx;
    const CameraPreset& preset = cameraPresets_[idx];
    cameraLocked_ = preset.isLive;

    Vector3f pos = preset.position;
    Vector3f tgt = preset.target;
    f32 roll = preset.staticRoll;
    if (preset.animator) {
        i32 seqStart = 0;
        i32 seqEnd = 0;
        model::Actor* focus = FocusActorPtr();
        const i32 seqIdx = focus ? focus->animation.ActiveSequenceIndex() : 0;
        if (seqIdx >= 0 && seqIdx < static_cast<i32>(sequenceRanges_.size())) {
            seqStart = sequenceRanges_[seqIdx].startMs;
            seqEnd = sequenceRanges_[seqIdx].endMs;
        }
        if (seqStart == 0 && seqEnd == 0)
            seqEnd = 1 << 30;
        const i32 sampleMs =
            focus ? focus->animation.TimeMs() : service_.Scene().GetAnimationTime();
        preset.animator(pos, tgt, roll, sampleMs, seqStart, seqEnd);
    }
    cam.SetDirectPose(pos, tgt, roll);
    const f32 fov = (preset.fovDiagonal > 1e-3f) ? preset.fovDiagonal : Camera::kDefaultFovDiagonal;
    cam.SetFovDiagonal(fov);
    cam.SetClip(preset.zNear, preset.zFar);
}

void ViewerApp::UpdateCameraPresetAnimator() {
    if (activeCameraPresetIdx_ < 0 ||
        activeCameraPresetIdx_ >= static_cast<i32>(cameraPresets_.size()))
        return;
    const CameraPreset& preset = cameraPresets_[activeCameraPresetIdx_];
    if (!preset.animator)
        return;

    model::Actor* focus = FocusActorPtr();
    const i32 seqIdx = focus ? focus->animation.ActiveSequenceIndex() : 0;
    i32 seqStart = 0;
    i32 seqEnd = 0;
    if (seqIdx >= 0 && seqIdx < static_cast<i32>(sequenceRanges_.size())) {
        seqStart = sequenceRanges_[seqIdx].startMs;
        seqEnd = sequenceRanges_[seqIdx].endMs;
    }
    if (seqStart == 0 && seqEnd == 0)
        seqEnd = 1 << 30;

    Vector3f pos = preset.position;
    Vector3f tgt = preset.target;
    f32 roll = preset.staticRoll;
    const i32 sampleMs = focus ? focus->animation.TimeMs() : service_.Scene().GetAnimationTime();
    preset.animator(pos, tgt, roll, sampleMs, seqStart, seqEnd);
    service_.Scene().Camera().SetDirectPose(pos, tgt, roll);
}

void ViewerApp::RequestAnimationExport(ExportRecipe recipe) {
    pendingExport_ = std::move(recipe);
    exportPending_ = true;
}

bool ViewerApp::ConsumeExportFinished() {
    const bool was = exportFinished_;
    exportFinished_ = false;
    return was;
}

// The callbacks export_runner.cpp drives the viewer through. Everything it
// needs that is ViewerApp-private goes here rather than the runner reaching
// back in, which keeps the loop readable and the host policy in one place.
ExportHost ViewerApp::MakeExportHost() {
    ExportHost host;
    host.service = &service_;
    host.scene = ActiveSceneId();
    host.target = targetId_;
    host.window = window_;
    host.hero = FocusActorPtr();
    host.sequenceNames = sequenceNames_;
    host.modelPath = currentModelPath_;
    // Keep an animated camera preset (if one is active) tracking the sequence
    // for every captured frame, exactly as normal playback does in Tick().
    host.applyCameraPreset = [this] { UpdateCameraPresetAnimator(); };
    host.activateCameraPreset = [this](i32 idx) { ActivateCameraPreset(idx); };
    host.currentCameraPreset = [this] { return activeCameraPresetIdx_; };
    // buildFrame emits the ImGui draw data RenderFrame composites: the live UI
    // overlay when requested, otherwise an empty frame (no overlay).
    host.buildUiFrame = [this] {
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ui_->BuildFrame();
        ImGui::Render();
    };
    host.buildEmptyFrame = [] {
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGui::Render();
    };
    host.reassertAnimTracks = [this] { ReassertAnimTracks(); };
    host.onProgress = [this](i32 frame, i32 total) {
        // An in-viewport overlay would be composited into the frame being
        // captured, so progress goes where it costs nothing.
        if (!window_)
            return;
        char title[128];
        std::snprintf(title, sizeof(title), "WhiteoutFlakes - exporting %d/%d (%d%%)", frame, total,
                      total > 0 ? (frame * 100) / total : 0);
        glfwSetWindowTitle(window_, title);
    };
    return host;
}

void ViewerApp::RunAnimationExport(const ExportRecipe& recipe) {
    const ExportHost host = MakeExportHost();
    if (!host.hero) {
        lastExportReport_ = {};
        lastExportReport_.error = "no animated model loaded";
        exportFinished_ = true;
        std::fprintf(stderr, "[viewer] Export: no animated model loaded\n");
        return;
    }

    exportRunning_ = true;
    lastExportReport_ = RunExport(recipe, host);
    exportRunning_ = false;
    exportFinished_ = true;
    const ExportReport& r = lastExportReport_;

    if (window_)
        glfwSetWindowTitle(window_, "WhiteoutFlakes");

    if (r.ok) {
        std::fprintf(stderr, "[viewer] Exported %d frame(s) in %.1fs (%s) to %s\n",
                     r.framesCaptured, r.elapsedSec,
                     r.files.empty() ? "" : io::PathToUtf8(r.files.front().filename()).c_str(),
                     io::PathToUtf8(r.folder).c_str());
    } else if (r.cancelled) {
        std::fprintf(stderr, "[viewer] Export cancelled after %d frame(s)\n", r.framesCaptured);
    } else {
        std::fprintf(stderr, "[viewer] Export failed: %s\n",
                     r.error.empty() ? "unknown error" : r.error.c_str());
    }
}

bool ViewerApp::ScrubExportRecipe(const ExportRecipe& recipe, i32 frameIndex, bool applyCamera) {
    return PreviewExportFrame(recipe, MakeExportHost(), frameIndex, applyCamera);
}

bool ViewerApp::IsPaused() const {
    return service_.SceneAt(ActiveSceneId()).IsPaused();
}

void ViewerApp::SetPaused(bool paused) {
    service_.SceneAt(ActiveSceneId())
        .SetPlaybackState(paused ? PlaybackState::Paused : PlaybackState::Playing);
}

void ViewerApp::RestartPlayback() {
    // RewindScene and the effect services it restarts all resolve through the
    // ACTIVE scene, so publish this document's before asking.
    PublishActiveScene();
    service_.RewindScene();
    service_.SceneAt(ActiveSceneId()).SetPlaybackState(PlaybackState::Playing);

    // The parent-clock delta below is measured against this stamp; leaving the
    // pre-rewind (larger) value there would report one negative frame.
    lastParentTimeMs_ = 0;

    // Walk-drift is an offset this app pushed the actor along by, not scene
    // state the rewind touches, so take it back off — the same unwind a
    // sequence change does.
    if (walkDriftAccumulated_ != 0.0f) {
        if (auto* hero = FocusActorPtr())
            hero->worldTransform.data[3][0] -= walkDriftAccumulated_;
        Camera& cam = service_.SceneAt(ActiveSceneId()).Camera();
        const auto t = cam.GetTarget();
        cam.SetTarget(t.x - walkDriftAccumulated_, t.y, t.z);
        walkDriftAccumulated_ = 0.0f;
    }
}

void ViewerApp::Tick(f32 dt) {
    // Publish the active document's scene BEFORE polling: GLFW input callbacks
    // fire inside glfwPollEvents and steer the active scene's camera, and every
    // Scene() read below must resolve to the active document too.
    PublishActiveScene();

    // Already inside event dispatch when repainting from a GLFW callback —
    // polling again there would recurse through the same message queue.
    if (!inCallbackRedraw_)
        glfwPollEvents();
    if (!window_ || glfwWindowShouldClose(window_))
        return;

    // Run a queued animation export before anything else — it owns the frame
    // (its own ImGui frame + render loop) and skips the normal tick.
    if (exportPending_) {
        exportPending_ = false;
        RunAnimationExport(pendingExport_);
        return;
    }

    // The export dialog's live preview poses the model from the same schedule
    // the recording uses. It runs here rather than inside the ImGui frame
    // because it mutates the actor, which the frame build must not.
    if (ui_)
        ui_->Export().Tick(dt);

    // Drive the async content provider's completion queue from the host
    // thread — texture stubs swap to their real pixels here, MDX-load
    // Wait()s wake up here, etc. Done before any per-frame asset access so
    // callbacks land before the rest of the tick reads what they produced.
    if (auto* cp = service_.Scene().ActiveContentProvider())
        cp->Pump();

    // Task completions, on the same thread and for the same reason: an OnDone
    // touches documents, scenes and the UI. Must come after the provider's
    // pump — a task body blocked in ReadFile is woken by that, and delivering
    // its completion first would report a task that has not returned yet.
    tasks_.Pump();

    // Startup-picker paths, one per frame and only while nothing else is
    // loading, so each gets its own bar instead of racing the one before it.
    // Before the ImGui pass below, so the modal opens in the same frame the
    // task is submitted rather than a frame later.
    if (!pendingInitialOpens_.empty() && !tasks_.Busy()) {
        const std::filesystem::path next = pendingInitialOpens_.front();
        pendingInitialOpens_.erase(pendingInitialOpens_.begin());
        OpenModelAsync(next);
    }

    // Per-frame size sync. The framebuffer-size callback alone isn't
    // reliable — GLFW on Windows can swallow callbacks during the maximize
    // transition's modal sizing loop, and HiDPI display changes also slip
    // through. Comparing against the last sized value is cheap and catches
    // every missed event. Skip the rest of the frame when minimised
    // (width or height 0) — swap chains hate zero extents.
    {
        i32 fbW = 0;
        i32 fbH = 0;
        glfwGetFramebufferSize(window_, &fbW, &fbH);
        if (fbW <= 0 || fbH <= 0)
            return;
        if ((fbW != lastFbW_ || fbH != lastFbH_) && service_.Pipeline().IsDeviceReady()) {
            service_.Pipeline().ResizePrimaryTarget(fbW, fbH);
            lastFbW_ = fbW;
            lastFbH_ = fbH;
        }
    }

    // Advance the active document's wall clock. test_main pumps the DEFAULT
    // scene's clock for the legacy single-scene path; a document on its own
    // scene needs its clock ticked here. Frozen (inactive) tabs keep their
    // clock, so switching back resumes where they left off.
    if (activeDoc_ >= 0)
        service_.Scene().Update(dt);

    // ---- Walk-drift along the camera's X axis (orbital mode only) ----
    constexpr f32 kDefaultWalkSpeed = 100.0f;
    auto effectiveMoveSpeed = [](const SequenceInfo& s) {
        if (!ContainsCi(s.name, "walk"))
            return 0.0f;
        return s.moveSpeed != 0.0f ? s.moveSpeed : kDefaultWalkSpeed;
    };

    auto* hero = FocusActorPtr();
    if (hero && service_.Scene().Camera().GetMode() == Camera::Mode::Orbital) {
        const i32 idx = hero->animation.ActiveSequenceIndex();
        f32 delta = 0.0f;
        if (idx != walkDriftPrevSeqIdx_) {
            delta = -walkDriftAccumulated_;
            walkDriftPrevSeqIdx_ = idx;
        } else if (idx >= 0 && idx < static_cast<i32>(sequenceRanges_.size())) {
            // The scene's dt, not the frame's: this drift IS the walk cycle
            // moving the model, so a pause has to stop it too. The unwind above
            // stays on the raw path — switching sequence while paused still
            // snaps the model back to where it started.
            const f32 ms = effectiveMoveSpeed(sequenceRanges_[idx]);
            if (ms != 0.0f)
                delta = ms * service_.Scene().EffectiveDt(dt);
        }
        if (delta != 0.0f) {
            walkDriftAccumulated_ += delta;
            hero->worldTransform.data[3][0] += delta;
            const auto t = service_.Scene().Camera().GetTarget();
            service_.Scene().Camera().SetTarget(t.x + delta, t.y, t.z);
        }
    }

    if (auto* dnc = service_.GetDncService())
        dnc->Advance(dt);

    // Storage Explorer: pump its (separate) CASC provider + apply staged folder
    // navigation, and mark its thumbnail cells not-yet-visible. Must run before
    // the panel's BuildWindow (in ui_->BuildFrame) acquires visible cells.
    if (storageExplorerOpen_ && storageExplorer_) {
        storageExplorer_->NewFrame(dt);
        PollStorageExplorerState(dt);
    }

    // ---- ImGui frame ----
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ui_->BuildFrame();
    ImGui::Render();

    // ---- Per-frame engine update ----
    const i32 curParentMs = service_.Scene().GetAnimationTime();
    i32 parentDtMs = curParentMs - lastParentTimeMs_;
    if (parentDtMs < 0)
        parentDtMs = 0;
    if (parentDtMs > 100)
        parentDtMs = 100;
    lastParentTimeMs_ = curParentMs;
    const f32 parentDt = static_cast<f32>(parentDtMs) / 1000.0f;

    UpdateCameraPresetAnimator();
    (void)service_.Replaceables().ConsumeDirty();
    (void)service_.Settings().ConsumeRenderModeDirty();

    // Push the camera pose to the sound emitter before the tick fires SND
    // events, so 3D-positioned event objects pan / attenuate against where
    // the camera is this frame.
    {
        const auto& cam = service_.Scene().Camera();
        const Vector3f eye = cam.GetSource();
        const Vector3f fwd = cam.GetTarget() - eye;
        service_.Sound().SetListener(eye, fwd, cam.GetUp());
    }

    // Tick the active document's scene (Tick publishes it then restores the
    // default scene on exit, so re-publish for the effect-framing + render).
    service_.Ticker().Tick(service_.SceneAt(ActiveSceneId()), parentDt);
    PublishActiveScene();

    // Reframe a freshly-loaded .pkb once its particle cloud has developed.
    // Wait a short warm-up so the AABB reflects the steady-state spread, then
    // keep retrying until particles exist (some effects spawn on a delay),
    // giving up after a bounded window so we don't poll forever.
    if (effectFrameTicks_ >= 0) {
        constexpr i32 kEffectFrameWarmupTicks = 12;
        constexpr i32 kEffectFrameMaxTicks = 90;
        ++effectFrameTicks_;
        if (effectFrameTicks_ >= kEffectFrameWarmupTicks) {
            if (FrameCameraToEffect() || effectFrameTicks_ >= kEffectFrameMaxTicks)
                effectFrameTicks_ = -1;
        }
    }

    // Storage Explorer: render every visible thumbnail cell into its offscreen
    // target BEFORE the main pass composites the ImGui draw data that samples
    // them. RenderThumbnails snapshots/restores the global RenderSettings (and
    // juggles the active scene per cell), so re-publish the document scene after.
    if (storageExplorerOpen_ && storageExplorer_) {
        storageExplorer_->RenderThumbnails(dt);
        PublishActiveScene();
    }

    // Render the active document's scene into the window target. RenderViewport
    // (unlike the RenderFrame shim) lets us name the scene explicitly, so a
    // document on a non-default scene composites correctly.
    {
        Viewport vp;
        vp.scene = ActiveSceneId();
        vp.target = targetId_;
        vp.camera = &service_.SceneAt(ActiveSceneId()).Camera();
        service_.Pipeline().RenderViewport(vp);
    }
    service_.Pipeline().Present(targetId_);

    // ---- FPS title-bar update ----
    fpsAccum_ += static_cast<f64>(dt);
    fpsFrames_ += 1;
    if (fpsAccum_ >= 1.0) {
        i32 nGeo = 0, nTex = 0, nNodes = 0, nParts = 0, nSegs = 0;
        service_.Pipeline().GetFrameStats(nGeo, nTex, nNodes, nParts, nSegs);
        char title[300];
        std::snprintf(title, sizeof(title),
                      "WhiteoutFlakes — %d FPS | %d geo, %d tex, %d nodes, %d parts, %d segs",
                      fpsFrames_, nGeo, nTex, nNodes, nParts, nSegs);
        glfwSetWindowTitle(window_, title);
        fpsAccum_ = 0.0;
        fpsFrames_ = 0;
    }
}

} // namespace whiteout::flakes
