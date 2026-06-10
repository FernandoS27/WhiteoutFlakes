#include "explorer_app.h"

#include "renderer/render_pipeline.h"

#include "whiteout/flakes/util/path_utf8.h"

#include "gfx/gfx.h"

#include "imgui_theme.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3.h>
#if defined(_WIN32)
#include <GLFW/glfw3native.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

#include <cstdio>

namespace whiteout::flakes::tools {

namespace {
constexpr const char* kWindowTitle = "WhiteoutFlakes Model Explorer";
} // namespace

ExplorerApp::~ExplorerApp() {
    Close();
}

bool ExplorerApp::Open(int width, int height, gfx::GfxApi api) {
    backend_ = api;
#if defined(_WIN32)
    {
        using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        HMODULE u = ::GetModuleHandleW(L"user32.dll");
        auto setCtx =
            u ? reinterpret_cast<SetCtxFn>(::GetProcAddress(u, "SetProcessDpiAwarenessContext"))
              : nullptr;
        if (!setCtx || !setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            ::SetProcessDPIAware();
    }
#endif
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit FAILED\n");
        return false;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    float dpi = 1.0f;
    if (GLFWmonitor* m = glfwGetPrimaryMonitor()) {
        float xs = 1.0f, ys = 1.0f;
        glfwGetMonitorContentScale(m, &xs, &ys);
        if (xs > 0.0f)
            dpi = xs;
    }
    window_ = glfwCreateWindow(static_cast<int>(width * dpi), static_cast<int>(height * dpi),
                               kWindowTitle, nullptr, nullptr);
    if (!window_) {
        std::fprintf(stderr, "glfwCreateWindow FAILED\n");
        glfwTerminate();
        return false;
    }
    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, &ExplorerApp::FramebufferSizeCallback);

#if defined(_WIN32)
    {
        HWND hwnd = glfwGetWin32Window(window_);
        const BOOL useDark = TRUE;
        const COLORREF chrome = RGB(38, 45, 56);
        ::DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
        ::DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &chrome, sizeof(chrome));
        ::DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &chrome, sizeof(chrome));
    }
#endif

    InitImGui(); // before InitDevice (EnsureImGui runs in InitBlsShaders)

    if (!svc_.Pipeline().InitDevice(api)) {
        std::fprintf(stderr, "InitDevice FAILED\n");
        Close();
        return false;
    }

    int fbW = width, fbH = height;
    glfwGetFramebufferSize(window_, &fbW, &fbH);
    if (fbW <= 0)
        fbW = width;
    if (fbH <= 0)
        fbH = height;

    void* swapHandle = nullptr;
#if defined(_WIN32)
    swapHandle = static_cast<void*>(glfwGetWin32Window(window_));
#else
    if (api == gfx::GfxApi::WebGPU || api == gfx::GfxApi::Metal) {
        swapHandle = static_cast<void*>(window_);
    } else {
        std::fprintf(stderr, "This explorer build supports D3D/WebGPU/Metal swapchains.\n");
        Close();
        return false;
    }
#endif
    targetId_ = svc_.Pipeline().CreateSwapChainTarget(swapHandle, fbW, fbH);
    if (targetId_ == 0) {
        std::fprintf(stderr, "CreateSwapChainTarget FAILED\n");
        Close();
        return false;
    }
    svc_.Pipeline().SetPrimaryTarget(targetId_);
    lastFbW_ = fbW;
    lastFbH_ = fbH;

    // The panel manages its own thumbnail render settings (per-cell mode, grid
    // off, background) around its cell render — see StorageExplorer.
    explorer_ = std::make_unique<StorageExplorer>(svc_);
    return true;
}

void ExplorerApp::Close() {
    // Destroy the explorer (its thumbnail scenes + targets) while the device is
    // still alive, before Pipeline().Shutdown() tears it down.
    explorer_.reset();
    if (imguiInitialised_)
        ShutdownImGui();
    if (svc_.Pipeline().IsDeviceReady())
        svc_.Pipeline().Shutdown();
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
        glfwTerminate();
    }
    targetId_ = 0;
}

bool ExplorerApp::ShouldClose() const {
    return !window_ || glfwWindowShouldClose(window_);
}

void ExplorerApp::InitImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ApplyImGuiTheme();
    float xs = 1.0f, ys = 1.0f;
    glfwGetWindowContentScale(window_, &xs, &ys);
    ApplyImGuiDpiScale(xs);
    ImGui_ImplGlfw_InitForOther(window_, true);
    imguiInitialised_ = true;
}

void ExplorerApp::ShutdownImGui() {
    svc_.ShutdownImGui();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    imguiInitialised_ = false;
}

void ExplorerApp::FramebufferSizeCallback(GLFWwindow* w, int width, int height) {
    auto* self = static_cast<ExplorerApp*>(glfwGetWindowUserPointer(w));
    if (self && self->svc_.Pipeline().IsDeviceReady() && width > 0 && height > 0) {
        self->svc_.Pipeline().ResizePrimaryTarget(width, height);
        self->lastFbW_ = width;
        self->lastFbH_ = height;
    }
}

bool ExplorerApp::OpenCasc(const std::string& root) {
    return explorer_ && explorer_->OpenCasc(root);
}

void ExplorerApp::Tick(float dt) {
    glfwPollEvents();
    if (!window_ || glfwWindowShouldClose(window_))
        return;

    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(window_, &fbW, &fbH);
    if (fbW <= 0 || fbH <= 0)
        return;
    if ((fbW != lastFbW_ || fbH != lastFbH_) && svc_.Pipeline().IsDeviceReady()) {
        svc_.Pipeline().ResizePrimaryTarget(fbW, fbH);
        lastFbW_ = fbW;
        lastFbH_ = fbH;
    }

    if (explorer_)
        explorer_->NewFrame(dt);

    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    // The standalone shell shows the panel as a non-closable window filling the
    // app. The grid/menu/thumbnails all live in StorageExplorer.
    if (explorer_)
        explorer_->BuildWindow(nullptr);
    ImGui::Render();

    // Render each visible cell into its target BEFORE the main ImGui pass
    // samples those textures.
    if (explorer_)
        explorer_->RenderThumbnails(dt);

    svc_.Pipeline().RenderFrame(targetId_);
    svc_.Pipeline().Present(targetId_);
}

} // namespace whiteout::flakes::tools
