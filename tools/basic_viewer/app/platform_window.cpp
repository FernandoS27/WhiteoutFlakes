#include "app/platform_window.h"

#include "imgui_theme.h"
#include "localization.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "settings_ini.h"
#include "ui/ui_metrics.h"
#include "whiteout/flakes/util/path_utf8.h"

#include "gfx/gfx.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>

// glfwCreateWindowSurface (cross-platform) for the Vulkan backend. Including
// <vulkan/vulkan.h> before <GLFW/glfw3.h> makes it visible without GLFW pulling
// in its own copy of the header.
#include <vulkan/vulkan.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#if defined(__APPLE__)
// MoltenVK 1.4 + GLFW 3.4: glfwCreateWindowSurface sets the contentView's layer
// before flipping wantsLayer=YES, which leaves the CAMetalLayer un-installed on
// macOS 13+ and fails vkCreateMetalSurfaceEXT. The shim in
// vulkan_surface_macos.mm does it in the right order.
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
namespace whiteout::flakes {
VkResult CreateVulkanSurfaceMacOS(VkInstance instance, void* nsWindow, VkSurfaceKHR* outSurface);
void SetCocoaWindowChrome(void* nsWindow, float r, float g, float b);
} // namespace whiteout::flakes
#endif
#if defined(_WIN32)
#include "resource.h" // IDI_WHITEOUT_ICON
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
// Newer DWM attributes, defined locally so no particular SDK is needed.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
// Per-monitor V2 DPI context — declared in Win10 1607+ SDKs; older ones need
// the cast-from-int the SDK header itself uses.
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT) - 4)
#endif
#endif

#include <cstdio>
#include <cstring>
#include <string>

namespace whiteout::flakes {

namespace {

#if defined(_WIN32)
// Per-monitor V2 awareness before any HWND exists. Without it Windows
// bitmap-stretches the whole window on scaled displays, which is what makes
// ImGui glyphs soft. SetProcessDpiAwarenessContext is Windows 10 1703+.
void EnableDpiAwareness() {
    using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    auto setCtx = user32 ? reinterpret_cast<SetCtxFn>(::GetProcAddress(user32, "SetProcessDpiAwarenessContext"))
                         : nullptr;
    if (!setCtx || !setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        ::SetProcessDPIAware();
}
#endif

// The embedded icon, and the title bar and border tinted to the ImGui menu bar
// so the OS chrome runs into the menu strip.
void ApplyWindowChrome(GLFWwindow* window) {
#if defined(_WIN32)
    // GLFW's glfwSetWindowIcon takes RGBA pixels; the embedded Win32 resource is
    // already what WM_SETICON wants.
    HWND hwnd = glfwGetWin32Window(window);
    HMODULE hMod = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&ApplyWindowChrome),
                         &hMod);
    HINSTANCE hInst = hMod ? reinterpret_cast<HINSTANCE>(hMod) : ::GetModuleHandle(nullptr);
    if (HICON hIcon = ::LoadIconW(hInst, MAKEINTRESOURCEW(IDI_WHITEOUT_ICON))) {
        ::SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
        ::SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon));
    }
    // Silently ignored on older Windows.
    const BOOL useDark = TRUE;
    const COLORREF chrome = RGB(kMenuBarBgRgb[0], kMenuBarBgRgb[1], kMenuBarBgRgb[2]);
    ::DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
    ::DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &chrome, sizeof(chrome));
    ::DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &chrome, sizeof(chrome));
#elif defined(__APPLE__)
    SetCocoaWindowChrome(glfwGetCocoaWindow(window), kMenuBarBgRgb[0] / 255.0f, kMenuBarBgRgb[1] / 255.0f,
                         kMenuBarBgRgb[2] / 255.0f);
#else
    (void)window; // the window manager picks the icon
#endif
}

// The swap-chain handle the gfx layer expects for @p api: an HWND on Windows, a
// VkSurfaceKHR built here for Vulkan elsewhere (so gfx need not branch on
// xcb / xlib / wayland), and the GLFWwindow* itself for WebGPU and Metal, whose
// backends pull the native handles through glfw3native.
bool MakeSwapHandle(renderer::RenderService& service, GLFWwindow* window, gfx::GfxApi api, void*& out) {
#if defined(_WIN32)
    (void)service;
    (void)api;
    out = static_cast<void*>(glfwGetWin32Window(window));
    return true;
#else
    if (api == gfx::GfxApi::Vulkan) {
        gfx::IGFXDevice* dev = service.Pipeline().Gfx();
        VkInstance instance = dev ? static_cast<VkInstance>(dev->GetNativeInstance()) : nullptr;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
#if defined(__APPLE__)
        VkResult sr = instance ? CreateVulkanSurfaceMacOS(instance, glfwGetCocoaWindow(window), &surface)
                               : VK_ERROR_INITIALIZATION_FAILED;
#else
        VkResult sr = instance ? glfwCreateWindowSurface(instance, window, nullptr, &surface)
                               : VK_ERROR_INITIALIZATION_FAILED;
#endif
        if (!instance || sr != VK_SUCCESS) {
            std::fprintf(stderr, "Vulkan surface creation FAILED (instance=%p, VkResult=%d)\n",
                         (void*)instance, (int)sr);
            return false;
        }
        // A 64-bit non-dispatchable handle on x86_64, packed into the void*.
        std::memcpy(&out, &surface, sizeof(out));
        return true;
    }
    if (api == gfx::GfxApi::WebGPU || api == gfx::GfxApi::Metal) {
        out = static_cast<void*>(window);
        return true;
    }
    std::fprintf(stderr, "Backend not supported on this platform\n");
    return false;
#endif
}

} // namespace

PlatformWindow::PlatformWindow(renderer::RenderService& service) : service_(service) {}

PlatformWindow::~PlatformWindow() {
    Close();
}

bool PlatformWindow::Open(i32 width, i32 height, gfx::GfxApi api, bool visible,
                          std::optional<f32> dpiScale, WindowEvents events) {
    backend_ = api;
    events_ = std::move(events);
#if defined(_WIN32)
    EnableDpiAwareness();
#endif
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit FAILED\n");
        return false;
    }

    // No OpenGL context: the swap chain goes through the engine's gfx layer.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);

    // The size is in logical (96-DPI) pixels: scaled for the primary monitor so
    // a 1280x720 viewer does not shrink to a quarter of a 4K 200 % display.
    f32 initialScale = 1.0f;
    if (dpiScale) {
        initialScale = *dpiScale;
    } else if (GLFWmonitor* primary = glfwGetPrimaryMonitor()) {
        f32 xs = 1.0f, ys = 1.0f;
        glfwGetMonitorContentScale(primary, &xs, &ys);
        if (xs > 0.0f)
            initialScale = xs;
    }
    window_ = glfwCreateWindow(static_cast<i32>(static_cast<f32>(width) * initialScale),
                               static_cast<i32>(static_cast<f32>(height) * initialScale), ui::kWindowTitle,
                               nullptr, nullptr);
    if (!window_) {
        std::fprintf(stderr, "glfwCreateWindow FAILED\n");
        glfwTerminate();
        return false;
    }
    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, &PlatformWindow::FramebufferSizeCallback);
    glfwSetWindowRefreshCallback(window_, &PlatformWindow::WindowRefreshCallback);
    glfwSetMouseButtonCallback(window_, &PlatformWindow::MouseButtonCallback);
    glfwSetCursorPosCallback(window_, &PlatformWindow::CursorPosCallback);
    glfwSetScrollCallback(window_, &PlatformWindow::ScrollCallback);
    ApplyWindowChrome(window_);

    // The ImGui context before InitDevice: InitBlsShaders calls
    // RenderService::EnsureImGui, whose renderer reads io.Fonts on first Render.
    InitImGui(dpiScale);

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

    void* swapHandle = nullptr;
    if (!MakeSwapHandle(service_, window_, api, swapHandle)) {
        Close();
        return false;
    }
    target_ = service_.Pipeline().CreateSwapChainTarget(swapHandle, fbW, fbH);
    if (target_ == 0) {
        std::fprintf(stderr, "CreateSwapChainTarget FAILED\n");
        Close();
        return false;
    }
    service_.Pipeline().SetPrimaryTarget(target_);
    lastFbW_ = fbW;
    lastFbH_ = fbH;
    return true;
}

void PlatformWindow::InitImGui(std::optional<f32> dpiScale) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ApplyImGuiTheme();

    // Fonts and style sizes at the monitor's content scale: glyphs rasterise at
    // the scaled size instead of being upsampled, which is what fixes soft text.
    f32 xs = 1.0f;
    f32 ys = 1.0f;
    if (dpiScale)
        xs = ys = *dpiScale;
    else
        glfwGetWindowContentScale(window_, &xs, &ys);
    // The bundled Noto fonts cover every UI language; the language picker's
    // endonyms are hardcoded and can use glyphs no catalog does (體 in 繁體中文).
    std::string endonyms;
    for (const auto& e : i18n::languages()) {
        endonyms += e.endonym;
        endonyms += ' ';
    }
    ApplyImGuiDpiScale(xs, io::PathToUtf8(AssetDir() / "fonts"), endonyms);

    // GLFW handles input only; the engine adapter draws.
    ImGui_ImplGlfw_InitForOther(window_, true);
    imguiInitialised_ = true;
}

void PlatformWindow::Close() {
    if (imguiInitialised_) {
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        imguiInitialised_ = false;
    }
    if (service_.Pipeline().IsDeviceReady())
        service_.Pipeline().Shutdown();
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
        glfwTerminate();
    }
    target_ = 0;
}

bool PlatformWindow::ShouldClose() const {
    return !window_ || glfwWindowShouldClose(window_);
}

void PlatformWindow::RequestClose() {
    if (window_)
        glfwSetWindowShouldClose(window_, GLFW_TRUE);
}

void PlatformWindow::PollEvents() {
    if (!inCallbackRedraw_)
        glfwPollEvents();
}

bool PlatformWindow::SyncFramebufferSize() {
    i32 fbW = 0;
    i32 fbH = 0;
    glfwGetFramebufferSize(window_, &fbW, &fbH);
    if (fbW <= 0 || fbH <= 0)
        return false;
    if ((fbW != lastFbW_ || fbH != lastFbH_) && service_.Pipeline().IsDeviceReady()) {
        service_.Pipeline().ResizePrimaryTarget(fbW, fbH);
        lastFbW_ = fbW;
        lastFbH_ = fbH;
    }
    return true;
}

void PlatformWindow::BeginImGuiFrame() {
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

bool PlatformWindow::PollEscape() {
    glfwPollEvents();
    return window_ && glfwGetKey(window_, GLFW_KEY_ESCAPE) == GLFW_PRESS;
}

void PlatformWindow::SetTitle(const char* title) {
    if (window_)
        glfwSetWindowTitle(window_, title);
}

void PlatformWindow::OnFramebufferResize(i32 w, i32 h) {
    if (w <= 0 || h <= 0 || !service_.Pipeline().IsDeviceReady())
        return;
    if (w == lastFbW_ && h == lastFbH_)
        return;
    service_.Pipeline().ResizePrimaryTarget(w, h);
    lastFbW_ = w;
    lastFbH_ = h;
    // Paint the new size now: inside the modal sizing loop the grown swap chain
    // would otherwise stay unpresented for the whole drag.
    RedrawFromCallback();
}

void PlatformWindow::RedrawFromCallback() {
    if (inCallbackRedraw_ || !window_ || target_ == 0 || !events_.redraw)
        return;
    if (!service_.Pipeline().IsDeviceReady())
        return;
    inCallbackRedraw_ = true;
    events_.redraw();
    inCallbackRedraw_ = false;
}

void PlatformWindow::FramebufferSizeCallback(GLFWwindow* w, int width, int height) {
    if (auto* self = static_cast<PlatformWindow*>(glfwGetWindowUserPointer(w)))
        self->OnFramebufferResize(width, height);
}

void PlatformWindow::WindowRefreshCallback(GLFWwindow* w) {
    if (auto* self = static_cast<PlatformWindow*>(glfwGetWindowUserPointer(w)))
        self->RedrawFromCallback();
}

void PlatformWindow::MouseButtonCallback(GLFWwindow* w, int button, int action, int /*mods*/) {
    auto* self = static_cast<PlatformWindow*>(glfwGetWindowUserPointer(w));
    if (!self || !self->events_.mouseButton)
        return;
    f64 x = 0.0, y = 0.0;
    glfwGetCursorPos(w, &x, &y);
    self->events_.mouseButton(button, action, x, y);
}

void PlatformWindow::CursorPosCallback(GLFWwindow* w, double x, double y) {
    auto* self = static_cast<PlatformWindow*>(glfwGetWindowUserPointer(w));
    if (self && self->events_.cursorPos)
        self->events_.cursorPos(x, y);
}

void PlatformWindow::ScrollCallback(GLFWwindow* w, double /*xoff*/, double yoff) {
    auto* self = static_cast<PlatformWindow*>(glfwGetWindowUserPointer(w));
    if (self && self->events_.scroll)
        self->events_.scroll(yoff);
}

} // namespace whiteout::flakes
