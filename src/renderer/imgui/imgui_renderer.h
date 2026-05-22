#pragma once

// ============================================================================
// Engine-side Dear ImGui renderer adapter.
//
// Uses the engine's imgui.bls (built from externals/Wc3Shaders/wc3_shaders/
// imgui_{vs,ps}.slang) to submit ImGui draw lists through the gfx
// abstraction. The host is responsible for:
//
//   • ImGui::CreateContext() / DestroyContext()
//   • Per-frame input (ImGui_ImplGlfw_NewFrame or similar)
//   • ImGui::NewFrame() / widget code / ImGui::Render()
//
// RenderPipeline::RenderFrame calls Render() after the tonemap pass and
// before Present, so widgets land on top of the final scene with the
// swapchain backbuffer as the RTV.
//
// Disabled at compile time when WDX_ENABLE_IMGUI=0 — RenderService::ImGui()
// still resolves but always returns nullptr, so callers can check once and
// no-op the rest of their UI code.
// ============================================================================

#include "gfx/gfx.h"
#include "whiteout/flakes/types.h"

#include <memory>

namespace whiteout::flakes::renderer::bls {
class BlsShaderCache;
struct BlsShader;
} // namespace whiteout::flakes::renderer::bls

namespace whiteout::flakes::renderer::dear_imgui {

#if WDX_ENABLE_IMGUI

class ImGuiRenderer {
public:
    // The RTV format must match what RenderFrame draws into when ImGui's
    // turn arrives — currently always the swapchain backbuffer (sRGB
    // R8G8B8A8). The DSV format is forwarded so the PSO key matches the
    // pipeline that the render-pass started with, even though depth test
    // is disabled for ImGui draws.
    ImGuiRenderer(gfx::IGFXDevice& device, bls::BlsShaderCache& shaderCache, gfx::Format rtvFormat,
                  gfx::Format dsvFormat);
    ~ImGuiRenderer();

    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

    bool IsReady() const;

    // Submits ImGui::GetDrawData() onto `cmd`. Safe to call when no ImGui
    // context exists or the draw data is empty — both no-op. `viewportW`
    // / `viewportH` are the framebuffer pixel size and become the scissor
    // clip + the 2D projection extent.
    void Render(gfx::IGFXCommandList& cmd, i32 viewportW, i32 viewportH);

    // Public so the .cpp's anonymous-namespace helpers (EnsureVertexBuffer
    // / EnsureIndexBuffer / BuildFontAtlas) can take an Impl& — they only
    // exist inside the TU so this is impl-detail in practice, but having
    // it in the namespace public surface keeps the helpers simple.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

#else // WDX_ENABLE_IMGUI

// Forward-only stub: declared so RenderService::ImGui() keeps its return
// type but the class body and translation unit don't exist.
class ImGuiRenderer;

#endif // WDX_ENABLE_IMGUI

} // namespace whiteout::flakes::renderer::dear_imgui
