#pragma once

// ============================================================================
// RenderSettings — application-tunable knobs that influence rendering.
//
// Pure data: atomics for things UI/render threads both touch, plain values for
// things only the render path reads. No GPU resources, no scene state, no
// pipeline behavior. The pipeline reads from here every frame; settings never
// reach back into the pipeline.
//
// IBL mode + render mode raise a one-shot dirty flag; the pipeline polls it
// and reacts on the next frame.
// ============================================================================

#include "whiteout/flakes/types.h"
#include "whiteout/flakes/gfx_types.h"  // gfx::GfxApi
#include "render_target.h"  // DisplayFlags, RenderMode, LightingMode, IblMode

#include <atomic>
#include <string>

namespace whiteout::flakes::renderer {

class RenderSettings {
public:
    RenderSettings() = default;

    // ---- Display flags (what to draw) ----
    DisplayFlags GetDisplayFlags() const {
        DisplayFlags df;
        df.showGrid       = showGrid_;
        df.showParticles  = showParticles_;
        df.showRibbons    = showRibbons_;
        df.showCollisions = showCollisions_;
        df.showLights     = showLights_;
        df.showEvents     = showEvents_;
        df.renderMode     = renderMode_;
        return df;
    }
    void SetDisplayFlags(const DisplayFlags& f) {
        showGrid_       = f.showGrid;
        showParticles_  = f.showParticles;
        showRibbons_    = f.showRibbons;
        showCollisions_ = f.showCollisions;
        showLights_     = f.showLights;
        showEvents_     = f.showEvents;
        SetRenderMode(f.renderMode);
    }

    bool ShowGrid()       const { return showGrid_; }
    bool ShowParticles()  const { return showParticles_; }
    bool ShowRibbons()    const { return showRibbons_; }
    bool ShowCollisions() const { return showCollisions_; }
    bool ShowLights()     const { return showLights_; }
    bool ShowEvents()     const { return showEvents_; }

    // ---- Render mode (HD vs SD) ----
    // App sets the mode based on what it loaded; pipeline polls the dirty
    // flag to know when to rebuild PSOs / IBL state.
    RenderMode GetRenderMode() const { return renderMode_; }
    void       SetRenderMode(RenderMode m) {
        if (renderMode_ != m) {
            renderMode_      = m;
            renderModeDirty_ = true;
        }
    }
    bool ConsumeRenderModeDirty() { return renderModeDirty_.exchange(false); }

    // ---- Debug visualization ----
    i32  HdDebugMode() const   { return hdDebugMode_.load(); }
    void SetHdDebugMode(i32 m) { hdDebugMode_.store(m); }

    i32  LodOverride() const    { return lodOverride_.load(); }
    void SetLodOverride(i32 l)  { lodOverride_.store(l); }

    // ---- Lighting / clear color ----
    LightingMode GetLightingMode() const           { return static_cast<LightingMode>(lightingMode_.load()); }
    void         SetLightingMode(LightingMode m)   { lightingMode_.store(static_cast<u8>(m)); }

    u32  BackgroundColorRaw() const { return backgroundColor_.load(); }
    void SetBackgroundColor(u8 r, u8 g, u8 b) {
        backgroundColor_.store(u32(r) | (u32(g) << 8) | (u32(b) << 16));
    }

    // ---- IBL ----
    // Pipeline polls ConsumeIblModeDirty() each frame; if set, it reloads
    // probe textures based on GetIblMode().
    IblMode GetIblMode() const { return iblMode_; }
    void    SetIblMode(IblMode m) {
        if (iblMode_ != m) {
            iblMode_      = m;
            iblModeDirty_ = true;
        } else {
            iblModeDirty_ = true;
        }
    }
    bool ConsumeIblModeDirty() { return iblModeDirty_.exchange(false); }

    // ---- Tonemap ----
    f32  GetTonemapExposure() const     { return tonemapExposure_; }
    void SetTonemapExposure(f32 e)      { tonemapExposure_ = e; }

    // ---- Graphics debug ----
    // Mirrored as an init-time flag: read once at device creation in
    // RenderPipeline::InitDevice and passed into gfx::CreateDevice,
    // which routes it to the per-backend validation layer (Vulkan
    // VK_LAYER_KHRONOS_validation, d3d11 DEBUG flag, d3d12 debug layer
    // + InfoQueue break-on-error). Changing it mid-run has no effect
    // until the next process start — the host persists it to .ini so
    // the next launch picks the new value up.
    bool GraphicsDebug()    const     { return graphicsDebug_; }
    void SetGraphicsDebug(bool v)     { graphicsDebug_ = v; }

    // ---- Default GFX backend ----
    // The host (basic_viewer) reads this when no `--backend` argument
    // was given on the command line. RenderSettings stores it so the
    // existing INI save/load + Settings-window infrastructure can be
    // reused; the renderer itself never inspects it (its backend is
    // already fixed by the time RenderPipeline::InitDevice runs).
    gfx::GfxApi DefaultBackend() const         { return defaultBackend_; }
    void        SetDefaultBackend(gfx::GfxApi b) { defaultBackend_ = b; }

    // ---- Preferred GFX device ----
    // Exact-match name of the physical adapter the host wants the
    // selected backend to open (compared verbatim against the names
    // returned by gfx::EnumerateDevices). Empty (the default) means
    // "let the backend pick — highest VRAM / discrete over integrated".
    // Plumbed in InitDevice via gfx::SetPreferredDevice before
    // gfx::CreateDevice. Takes effect on the next process launch.
    const std::string& PreferredDevice() const     { return preferredDevice_; }
    void               SetPreferredDevice(std::string name) {
        preferredDevice_ = std::move(name);
    }

private:
    // Display flags — plain bools; readers tolerate single-byte tearing.
    bool showGrid_       = true;
    bool showParticles_  = true;
    bool showRibbons_    = true;
    bool showCollisions_ = false;
    bool showLights_     = false;
    bool showEvents_     = true;

    // Render mode + dirty flag.
    RenderMode        renderMode_      = RenderMode::SD;
    std::atomic<bool> renderModeDirty_{false};

    // Debug + LOD.
    std::atomic<i32> hdDebugMode_{0};
    std::atomic<i32> lodOverride_{0};

    // Lighting + clear color.
    std::atomic<u8>  lightingMode_{static_cast<u8>(LightingMode::InGame)};
    std::atomic<u32> backgroundColor_{0x00453A35u};

    // IBL.
    IblMode           iblMode_ = IblMode::Portrait;
    std::atomic<bool> iblModeDirty_{true};  // pipeline does an initial apply

    // Tonemap exposure.
    f32 tonemapExposure_ = 1.0f;

    // Graphics-debug (validation layers). Off by default — turning it
    // on costs frame time and noise. Set before InitDevice runs.
    bool graphicsDebug_ = false;

    // Host-only: default backend when --backend is not on the command
    // line. D3D12 matches the long-standing test_main default.
    gfx::GfxApi defaultBackend_ = gfx::GfxApi::D3D12;

    // Host-only: preferred physical device name. Empty = default pick.
    std::string preferredDevice_;
};

}
