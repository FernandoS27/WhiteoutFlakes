#pragma once

// ExplorerApp — the model-explorer window. Owns the GLFW window, the Dear ImGui
// context, the gfx device (via RenderService), the CASC browser, and the live
// thumbnail pool. Per frame it builds the folder grid (each model/effect cell
// shows a live, animating render of its file) and presents.

#include "casc_browser.h"
#include "thumbnail_pool.h"

#include "renderer/render_service.h"
#include "renderer/types.h"
#include "whiteout/flakes/gfx_types.h"

#include <cstdint>
#include <memory>
#include <string>

struct GLFWwindow;

namespace whiteout::flakes::io {
class FileContentProvider;
}

namespace whiteout::flakes::tools {

class ExplorerApp {
public:
    explicit ExplorerApp(renderer::RenderService& svc) : svc_(svc) {}
    ~ExplorerApp();

    bool Open(int width, int height, gfx::GfxApi api);
    void Close();
    bool ShouldClose() const;
    void Tick(float dt);

    // Open a CASC archive root and point the shared content provider at it.
    bool OpenCasc(const std::string& root);

    GLFWwindow* Window() const {
        return window_;
    }

private:
    void InitImGui();
    void ShutdownImGui();
    void BuildUI();        // menu bar + breadcrumb + grid
    void OpenCascDialog(); // native folder picker

    static void FramebufferSizeCallback(GLFWwindow* w, int width, int height);

    renderer::RenderService& svc_;
    GLFWwindow* window_ = nullptr;
    gfx::GfxApi backend_ = gfx::GfxApi::D3D12;
    renderer::RenderTargetId targetId_ = 0;
    bool imguiInitialised_ = false;
    int lastFbW_ = 0;
    int lastFbH_ = 0;

    CascBrowser browser_;
    std::shared_ptr<io::FileContentProvider> provider_;
    std::unique_ptr<ThumbnailPool> pool_;

    std::uint64_t frameCounter_ = 0;
    std::string selectedPath_;
    std::string lastError_;

    // Navigation staged by the UI, applied at the start of the next frame (see
    // ExplorerApp::Tick) so it can't invalidate the listing mid-iteration or
    // destroy thumbnails the current frame still references.
    bool navPending_ = false;
    bool navAscend_ = false;
    std::string navTarget_;
};

} // namespace whiteout::flakes::tools
