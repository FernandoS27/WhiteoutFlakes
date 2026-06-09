#include "explorer_app.h"

#include "io/file_content_provider.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_settings.h"

#include "whiteout/flakes/util/path_utf8.h"

#include "gfx/gfx.h"

#include "imgui_theme.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <nfd.hpp>

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3.h>
#if defined(_WIN32)
#include <GLFW/glfw3native.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace whiteout::flakes::tools {

namespace {
constexpr const char* kWindowTitle = "WhiteoutFlakes Model Explorer";

bool IsEffectName(const std::string& name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return false;
    std::string ext = name.substr(dot + 1);
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "pkb" || ext == "pkfx";
}
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

    // Thumbnail render settings: render everything in HD, hide the debug floor
    // grid, and clear to the ImGui window background so cells blend into the UI.
    {
        auto df = svc_.Settings().GetDisplayFlags();
        df.showGrid = false;
        df.renderMode = renderer::RenderMode::HD;
        svc_.Settings().SetDisplayFlags(df);
        const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        svc_.Settings().SetBackgroundColor(static_cast<unsigned char>(bg.x * 255.0f),
                                           static_cast<unsigned char>(bg.y * 255.0f),
                                           static_cast<unsigned char>(bg.z * 255.0f));
    }

    // One shared CASC-backed provider for every cell scene. Pool cap is sized
    // above a full 4-column screen of cells so scrolling within a folder doesn't
    // recycle (and thus destroy) a scene whose GPU work may still be in flight.
    provider_ = std::make_shared<io::FileContentProvider>();
    pool_ = std::make_unique<ThumbnailPool>(svc_, provider_, /*cap*/ 32, /*res*/ 256);
    return true;
}

void ExplorerApp::Close() {
    // Destroy the thumbnail cells (scenes + targets) while the device is still
    // alive, before Pipeline().Shutdown() tears it down.
    pool_.reset();
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
    std::string err;
    if (!browser_.Open(root, &err)) {
        lastError_ = "Failed to open CASC at '" + root + "': " + err;
        std::fprintf(stderr, "[explorer] %s\n", lastError_.c_str());
        return false;
    }
    provider_->SetInstallPath(browser_.Root());
    provider_->SetHdMode(true); // prefer the _hd.w3mod overlay so HD textures resolve
    if (pool_)
        pool_->Clear();
    selectedPath_.clear();
    lastError_.clear();
    return true;
}

void ExplorerApp::OpenCascDialog() {
    NFD::UniquePathU8 outPath;
    if (NFD::PickFolder(outPath) == NFD_OKAY && outPath)
        OpenCasc(outPath.get());
}

void ExplorerApp::BuildUI() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##explorer", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_MenuBar);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open CASC folder…"))
                OpenCascDialog();
            if (ImGui::MenuItem("Exit"))
                glfwSetWindowShouldClose(window_, GLFW_TRUE);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    if (!browser_.IsOpen()) {
        ImGui::TextWrapped("Open a CASC archive folder (File ▸ Open CASC folder…) to browse "
                           "models and effects.");
        if (!lastError_.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", lastError_.c_str());
        }
        ImGui::End();
        return;
    }

    // Navigation is DEFERRED: changing the current folder rebuilds the browser's
    // listing, so doing it mid-loop would invalidate the very container we're
    // iterating. Record the request here and apply it once after the grid.
    enum class Nav { None, To, Ascend };
    Nav navAction = Nav::None;
    std::string navTarget;

    // Breadcrumb.
    if (ImGui::SmallButton("root")) {
        navAction = Nav::To;
        navTarget.clear();
    }
    const auto& crumbs = browser_.Breadcrumb();
    std::string acc;
    for (size_t i = 0; i < crumbs.size(); ++i) {
        ImGui::SameLine();
        ImGui::TextUnformatted("/");
        ImGui::SameLine();
        if (!acc.empty())
            acc.push_back('\\');
        acc += crumbs[i];
        if (ImGui::SmallButton((crumbs[i] + "##bc" + std::to_string(i)).c_str())) {
            navAction = Nav::To;
            navTarget = acc;
        }
    }
    ImGui::Separator();

    ImGui::BeginChild("grid", ImVec2(0, 0), false);

    ImGuiStyle& style = ImGui::GetStyle();
    // Fixed 4-column grid (the cell size follows the window width).
    constexpr int cols = 4;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float cell = (std::max)(64.0f, (avail - style.ItemSpacing.x * (cols - 1)) / cols);

    auto folderColor = ImGui::GetColorU32(ImVec4(0.85f, 0.72f, 0.35f, 1.0f));
    auto placeholderColor = ImGui::GetColorU32(ImVec4(0.25f, 0.28f, 0.34f, 1.0f));

    int col = 0;
    auto newRow = [&]() {
        if (col != 0) {
            col = 0;
        }
    };

    // Folders first.
    const auto& listing = browser_.Current();
    int idx = 0;
    auto beginCell = [&]() {
        if (col > 0)
            ImGui::SameLine();
    };
    auto endCell = [&]() {
        if (++col >= cols)
            col = 0;
    };

    // ".." up entry.
    if (!browser_.CurrentPath().empty()) {
        beginCell();
        ImGui::BeginGroup();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##up", ImVec2(cell, cell));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            navAction = Nav::Ascend;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(p0.x + cell * 0.18f, p0.y + cell * 0.30f),
                          ImVec2(p0.x + cell * 0.82f, p0.y + cell * 0.74f), folderColor, 6.0f);
        ImGui::TextUnformatted("..");
        ImGui::EndGroup();
        endCell();
    }

    for (const auto& folder : listing.folders) {
        beginCell();
        ImGui::PushID(idx++);
        ImGui::BeginGroup();
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##f", ImVec2(cell, cell));
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            navAction = Nav::To;
            navTarget = browser_.CurrentPath();
            if (!navTarget.empty())
                navTarget.push_back('\\');
            navTarget += folder;
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(p0.x + cell * 0.16f, p0.y + cell * 0.34f),
                          ImVec2(p0.x + cell * 0.84f, p0.y + cell * 0.72f), folderColor, 6.0f);
        dl->AddRectFilled(ImVec2(p0.x + cell * 0.16f, p0.y + cell * 0.26f),
                          ImVec2(p0.x + cell * 0.46f, p0.y + cell * 0.36f), folderColor, 4.0f);
        std::string label = folder;
        if (label.size() > 18)
            label = label.substr(0, 17) + "…";
        ImGui::TextWrapped("%s", label.c_str());
        ImGui::EndGroup();
        ImGui::PopID();
        endCell();
    }

    // Model / effect files — live thumbnails.
    for (const auto& file : listing.modelFiles) {
        beginCell();
        ImGui::PushID(idx++);
        ImGui::BeginGroup();
        const std::string archivePath = browser_.ChildPath(file);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 p1 = ImVec2(p0.x + cell, p0.y + cell);

        const bool onScreen = ImGui::IsRectVisible(p0, p1);
        gfx::TextureHandle tex = gfx::TextureHandle::Invalid;
        if (onScreen)
            tex = pool_->Acquire(archivePath, IsEffectName(file), frameCounter_);

        ImGui::InvisibleButton("##m", ImVec2(cell, cell));
        if (ImGui::IsItemClicked())
            selectedPath_ = archivePath;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (tex != gfx::TextureHandle::Invalid) {
            dl->AddImage(static_cast<ImTextureID>(static_cast<std::uint64_t>(tex)), p0, p1);
        } else {
            dl->AddRectFilled(p0, p1, placeholderColor, 4.0f);
        }
        if (selectedPath_ == archivePath)
            dl->AddRect(p0, p1, ImGui::GetColorU32(ImVec4(0.4f, 0.7f, 1.0f, 1.0f)), 4.0f, 0, 2.0f);

        std::string label = file;
        if (label.size() > 18)
            label = label.substr(0, 17) + "…";
        ImGui::TextWrapped("%s", label.c_str());
        ImGui::EndGroup();
        ImGui::PopID();
        endCell();
    }
    (void)newRow;

    ImGui::EndChild();
    ImGui::End();

    // Stage any navigation for the START of the next frame. Applying it here
    // would (a) be mid-iteration of the listing and (b) destroy cell textures
    // this frame's ImGui draw data still references — both crash. Tick() applies
    // it before the next frame builds anything.
    if (navAction == Nav::Ascend) {
        navPending_ = true;
        navAscend_ = true;
    } else if (navAction == Nav::To) {
        navPending_ = true;
        navAscend_ = false;
        navTarget_ = navTarget;
    }
}

void ExplorerApp::Tick(float dt) {
    glfwPollEvents();
    if (!window_ || glfwWindowShouldClose(window_))
        return;

    // Apply navigation staged by last frame's UI, before anything references the
    // (about-to-be-cleared) thumbnails. Clear() waits for the GPU to finish with
    // the old cells' targets first.
    if (navPending_) {
        navPending_ = false;
        if (navAscend_)
            browser_.Ascend();
        else
            browser_.NavigateTo(navTarget_);
        if (pool_)
            pool_->Clear();
        selectedPath_.clear();
    }

    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(window_, &fbW, &fbH);
    if (fbW <= 0 || fbH <= 0)
        return;
    if ((fbW != lastFbW_ || fbH != lastFbH_) && svc_.Pipeline().IsDeviceReady()) {
        svc_.Pipeline().ResizePrimaryTarget(fbW, fbH);
        lastFbW_ = fbW;
        lastFbH_ = fbH;
    }

    if (provider_)
        provider_->Pump();

    ++frameCounter_;
    if (pool_)
        pool_->BeginFrame(frameCounter_);

    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    BuildUI();
    ImGui::Render();

    // Render each visible cell into its target BEFORE the main ImGui pass
    // samples those textures.
    if (pool_)
        pool_->RenderVisible(dt);

    svc_.Pipeline().RenderFrame(targetId_);
    svc_.Pipeline().Present(targetId_);
}

} // namespace whiteout::flakes::tools
