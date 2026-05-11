#pragma once

// ============================================================================
// RenderPipeline — GPU device + render-target + frame-loop subsystem extracted
// from RenderService.
//
// RenderService owns scene-side state (actors, particles, splats, settings,
// asset managers) and forwards GPU work to RenderPipeline. Tools use
// service.Pipeline().X(...) for the device lifecycle, render targets,
// per-frame submission, and stats. Other in-tree subsystems read pipeline
// state through public accessors (Gfx, Width, Height, CbPerFrame,
// PrimaryTarget, ComputeSelectedLod, Shadow, CurrentLinePSO,
// SceneTargetFormat). The only remaining friends — GeosetPassBls /
// GeosetPassHd, defined inside render_pipeline.cpp — reach BLS-specific
// internals (programs, CBs, PSO builder, IBL probe state) that don't merit
// individual public getters.
// ============================================================================

#include "whiteout/flakes/types.h"
#include "gfx/gfx.h"
#include "render_target.h"
#include "types.h"

#include <memory>
#include <string>

namespace whiteout::flakes::renderer {

class RenderService;
class GeosetPassBls;
class GeosetPassHd;
enum class GeosetBucket : u8;

class RenderPipeline {
public:
    explicit RenderPipeline(RenderService& rs);
    ~RenderPipeline();

    // ---- Device lifecycle ----
    bool InitDevice(gfx::GfxApi api = gfx::GfxApi::D3D12);
    bool IsDeviceReady() const;
    void Shutdown();                          // was RenderService::ShutdownDevice

    // ---- Render targets ----
    RenderTargetId CreateSwapChainTarget(void* nativeWindowHandle, i32 width, i32 height);
    void           SetPrimaryTarget(RenderTargetId id);
    void           ResizePrimaryTarget(i32 width, i32 height);

    // ---- Frame loop ----
    void RenderFrame(RenderTargetId targetId);
    void Present(RenderTargetId targetId);

    // ---- Stats ----
    void GetFrameStats(i32& geosets, i32& textures, i32& nodes,
                       i32& particles, i32& segments) const;

    // ---- Promoted from RenderService private — used by friend subsystems ----
    gfx::IGFXDevice*       Gfx();
    const gfx::IGFXDevice* Gfx() const;
    gfx::PipelineHandle    CurrentLinePSO() const;
    gfx::Format            SceneTargetFormat() const;
    // Depth-stencil format picked at InitDevice time. AMD's Vulkan
    // driver doesn't expose D24_UNORM_S8_UINT, so the gfx layer
    // queries each device for the best supported format
    // (D24_UNORM_S8_UINT preferred → D32_FLOAT_S8_UINT fallback) and
    // we cache the answer here. Every CreateDepthTarget call and
    // every PSO `dsvFormat` field in the renderer should plumb this
    // value through; hard-coding D24_UNORM_S8_UINT will crash on AMD
    // d3d12 and fail validation on AMD Vulkan.
    gfx::Format            DepthStencilFormat() const;
    // Render mode snapshot for the in-flight frame. See the comment on
    // `Impl::frameRenderMode_`. Use this anywhere a per-frame decision
    // depends on HD vs SD; reading `Settings().GetRenderMode()` mid-
    // frame is racy.
    RenderMode             FrameRenderMode() const;

    // ---- Surface size + per-frame CB exposed for non-friend consumers
    //      (DebugRenderer, BLS pass templates, etc.) ----
    i32                    Width() const;
    i32                    Height() const;
    gfx::BufferHandle      CbPerFrame() const;
    RenderTarget*          PrimaryTarget();
    i32                    ComputeSelectedLod() const;

    // ---- Shadow PSO/CB handles read by shadow::ShadowPass.
    //      Bundled rather than friended so the pass class doesn't need
    //      access to the rest of RenderPipeline::Impl. ----
    struct ShadowResources {
        gfx::PipelineHandle psoSkinned;
        gfx::PipelineHandle psoRigid;
        gfx::BufferHandle   vsCb;
    };
    ShadowResources        Shadow() const;

    // ---- Scene-target formats (HD vs SD pipeline). Public so DebugRenderer
    //      and other consumers can build PSOs that match either path. ----
    static constexpr gfx::Format kHdrSceneFormat = gfx::Format::R11G11B10_FLOAT;
    static constexpr gfx::Format kSdSceneFormat  = gfx::Format::R8G8B8A8_UNORM_SRGB;

private:
    // ---- Friends ----
    // GeosetPassBls / GeosetPassHd are defined inside render_pipeline.cpp
    // and reach BLS-specific internals (programs, CBs, PSO builder, IBL mip
    // extents). Everyone else uses the public accessors above.
    friend class GeosetPassBls;
    friend class GeosetPassHd;

    // ---- All formerly-RenderService private methods that touch GPU state ----
    void CleanupGFX();
    bool CreateShaders();
    bool CreatePipelines();
    bool CreateDefaultResources();
    void ReleaseModelGPU();
    void RunTonemapPass(const RenderTarget& target);
    bool InitBlsShaders(gfx::GfxApi api);
    void ShutdownBlsShaders();
    bool RenderParticlesBls();
    bool RenderSplatsBls();
    bool RenderGeosetsBls(GeosetBucket bucket);
    bool RenderGeosetsHd(GeosetBucket bucket);
    void RenderRibbons();
    void RenderCornEffects();
    void RenderGeosets(GeosetBucket bucket);
    void ApplyIblMode(IblMode mode);
    void SetEnvProbe(const std::string& relPath);
    void SetDayNightProbes(const std::string& dayPath,
                           const std::string& nightPath);

    static constexpr const char* kIblSplitSumLutName = "ibl.splitSumLut";
    static constexpr const char* kIblFromProbeName   = "ibl.fromProbe";
    static constexpr const char* kIblToProbeName     = "ibl.toProbe";
    static constexpr const char* kIblDayProbeName    = "ibl.dayProbe";
    static constexpr const char* kIblNightProbeName  = "ibl.nightProbe";

    RenderService& rs_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
