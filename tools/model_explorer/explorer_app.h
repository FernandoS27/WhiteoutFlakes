#pragma once

// ExplorerApp — the standalone model-explorer SHELL. Owns the GLFW window, the
// Dear ImGui context, and the gfx device (via RenderService), then hosts a
// StorageExplorer panel (the reusable browser + live thumbnail grid). All the
// browsing/rendering logic lives in the library's StorageExplorer; this class is
// only the window/device/ImGui boilerplate + per-frame dispatch.

#include "storage_explorer.h"

#include "renderer/render_service.h"
#include "renderer/types.h"
#include "whiteout/flakes/gfx_types.h"

#include <memory>
#include <string>

struct GLFWwindow;

namespace whiteout::flakes::tools {

class ExplorerApp {
public:
    explicit ExplorerApp(renderer::RenderService& svc) : svc_(svc) {}
    ~ExplorerApp();

    bool Open(int width, int height, gfx::GfxApi api);
    void Close();
    bool ShouldClose() const;
    void Tick(float dt);

    // Open a CASC archive root in the hosted explorer panel.
    bool OpenCasc(const std::string& root);

    GLFWwindow* Window() const {
        return window_;
    }

private:
    void InitImGui();
    void ShutdownImGui();

    static void FramebufferSizeCallback(GLFWwindow* w, int width, int height);

    renderer::RenderService& svc_;
    GLFWwindow* window_ = nullptr;
    gfx::GfxApi backend_ = gfx::GfxApi::D3D12;
    renderer::RenderTargetId targetId_ = 0;
    bool imguiInitialised_ = false;
    int lastFbW_ = 0;
    int lastFbH_ = 0;

    std::unique_ptr<StorageExplorer> explorer_;
};

} // namespace whiteout::flakes::tools
