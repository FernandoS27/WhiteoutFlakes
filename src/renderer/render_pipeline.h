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

#include "gfx/gfx.h"
#include "render_target.h"
#include "types.h"
#include "viewport.h"
#include "whiteout/flakes/types.h"

#include <memory>
#include <string>
#include <vector>

namespace whiteout::flakes::renderer {

class RenderService;
class GeosetPassBls;
class GeosetPassHd;
class Camera;
enum class GeosetBucket : u8;

namespace shading {
class IShadingModel;
}
namespace core {
class IRenderProfile;
class VertexLayoutCache;
}

namespace particle {
struct EmitterDrawList;
}
namespace bls {
struct FrameInputs;
}
namespace model {
struct Actor;
}

// One ribbon emitter's strip, surfaced for the unified transparent queue. Holds
// the owning actor (for its per-actor ribbon VB + textures) and the resolved
// material bits so DrawRibbonStrip can run it interleaved with other producers.
struct RibbonDrawUnit {
    model::Actor* actor = nullptr;
    i32 filterMode = 0;
    i32 matFlags = 0;
    i32 textureId = -1;
    i32 count = 0;
    i32 offset = 0;
    Vector3f origin = {0, 0, 0};
    i32 priorityPlane = 0;
};

class RenderPipeline {
public:
    explicit RenderPipeline(RenderService& rs);
    ~RenderPipeline();

    // ---- Device lifecycle ----
    bool InitDevice(gfx::GfxApi api = gfx::GfxApi::D3D12);
    bool IsDeviceReady() const;
    void Shutdown(); // was RenderService::ShutdownDevice

    // ---- Render targets ----
    RenderTargetId CreateSwapChainTarget(void* nativeWindowHandle, i32 width, i32 height);
    // Headless sibling of CreateSwapChainTarget: a render target with no
    // swap-chain. RenderViewport draws into it like any other target; Present
    // no-ops on it. The composited image lands in RenderTarget::color (in
    // `colorFormat`); read it back via the frame-capture ring
    // (EnableFrameCapture + RenderViewport + DownloadCaptureSlot), which
    // works on headless targets because its swap-chain mirror blit no-ops.
    RenderTargetId CreateOffscreenTarget(i32 width, i32 height,
                                         gfx::Format colorFormat = kSdSceneFormat);
    void DestroyTarget(RenderTargetId id);
    void SetPrimaryTarget(RenderTargetId id);
    void ResizePrimaryTarget(i32 width, i32 height);

    // ---- Frame loop ----
    // RenderViewport is the real entry point: it renders the scene seen
    // through vp.camera into vp.target. RenderFrame is the legacy shim — it
    // renders the scene's single camera into targetId and is what current
    // single-viewport hosts call. Multi-viewport hosts build a Viewport per
    // surface and call RenderViewport for each, then Present the windowed ones.
    void RenderViewport(const Viewport& vp);
    void RenderFrame(RenderTargetId targetId);
    void Present(RenderTargetId targetId);

    // ---- Optional frame capture (for PNG / GIF export) ----
    // Off by default and zero-cost on the normal render path. When enabled,
    // each frame's final composited image is redirected to an off-screen
    // target, mirrored to the back buffer for display, and copied into a
    // ring of CPU-readable buffers by a compute shader. Enabling allocates a
    // capture target + the readback ring sized to the primary surface;
    // disabling frees them. Keep it off except while actually exporting.
    void EnableFrameCapture(bool enable);
    bool IsFrameCaptureEnabled() const;
    // Capture-ring depth — the exporter submits this many frames, calls
    // Gfx()->WaitIdle() once, then drains every slot.
    i32 FrameCaptureRingSize() const;
    // Ring slot the most recent RenderFrame wrote its capture into, or -1 if
    // capture is off or that frame produced no capture.
    i32 LastCapturedSlot() const;
    // Copy a capture ring slot out as tightly-packed RGBA8 (row pitch =
    // width*4). The slot's GPU work must have completed first — call
    // Gfx()->WaitIdle() before draining a batch. Returns false on an
    // out-of-range slot or when capture is disabled.
    bool DownloadCaptureSlot(i32 slot, std::vector<u8>& outRgba, i32& width, i32& height);

    // Direct render-target readback as tightly-packed RGBA8 — a portable
    // alternative to the frame-capture ring for off-screen / headless targets.
    // Synchronous (submits + WaitIdle internally). Backed by
    // IGFXDevice::ReadbackTexture; returns false on backends that don't
    // implement it (use the capture ring there). Reads the target's composited
    // color.
    bool ReadbackTarget(RenderTargetId id, std::vector<u8>& outRgba, i32& width, i32& height);

    // The target's composited color texture (the same image RenderViewport
    // produced). Hosts displaying an offscreen target in ImGui pass this as an
    // ImTextureID: ImGui::Image((ImTextureID)(u64)GetTargetColorTexture(id), …).
    // Returns Invalid for an unknown id. The gfx layer auto-transitions a
    // render-target texture to a sampleable state when it's bound as a shader
    // resource, so no explicit barrier is needed before the ImGui pass samples
    // it. Valid only until the target is resized/destroyed.
    gfx::TextureHandle GetTargetColorTexture(RenderTargetId id) const;

    // ---- Stats ----
    void GetFrameStats(i32& geosets, i32& textures, i32& nodes, i32& particles,
                       i32& segments) const;

    // ---- Promoted from RenderService private — used by friend subsystems ----
    gfx::IGFXDevice* Gfx();
    const gfx::IGFXDevice* Gfx() const;
    gfx::PipelineHandle CurrentLinePSO() const;
    /// @brief `CurrentLinePSO` with the depth test off.
    ///
    /// For markers that annotate geometry rather than sit in the scene with it:
    /// a `.phys` collider lives *inside* the silhouette it belongs to, so a
    /// depth-tested wireframe of one is drawn correctly and then hidden by the
    /// character wrapped around it — which looks exactly like drawing nothing.
    gfx::PipelineHandle CurrentOverlayLinePSO() const;
    gfx::Format SceneTargetFormat() const;
    // Depth-stencil format picked at InitDevice time. AMD's Vulkan
    // driver doesn't expose D24_UNORM_S8_UINT, so the gfx layer
    // queries each device for the best supported format
    // (D24_UNORM_S8_UINT preferred → D32_FLOAT_S8_UINT fallback) and
    // we cache the answer here. Every CreateDepthTarget call and
    // every PSO `dsvFormat` field in the renderer should plumb this
    // value through; hard-coding D24_UNORM_S8_UINT will crash on AMD
    // d3d12 and fail validation on AMD Vulkan.
    gfx::Format DepthStencilFormat() const;

    // The frame this scene runs: pass order, conditions, target set, colour
    // space, world scale. Selected by the scene's ProductId, falling back to
    // RenderMode's choice between the two WC3 profiles. Built alongside the
    // shading models and validated once.
    //
    // Public because the profile is the answer to "what does this product's
    // frame look like", and ModelLoader has to ask it for WorldScale at spawn.
    // Keeping it private would mean a one-off forwarder per question.
    core::IRenderProfile& ActiveProfile();

    // The same frame, selected for work that runs BETWEEN frames — asset loads.
    //
    // ActiveProfile answers off the frame's latched render mode, which is what
    // keeps every decision inside one frame consistent. A load is not inside a
    // frame: the host sets the render mode for the model it is about to spawn
    // and only the next frame latches it, so during the load the latch still
    // holds the previous model's mode. Anything a load decides and caches (a
    // texture's colour space) must read the live mode instead, or an HD model
    // loaded after an SD one bakes SD's answer.
    core::IRenderProfile& LoadTimeProfile();

    // Interned MeshBuffer vertex layouts. One per pipeline because a layout
    // id has to mean the same thing in a shading model's PSO key as it did
    // at upload; per-model caches would collide on id 1.
    core::VertexLayoutCache& VertexLayouts();

    // Extra colour attachments the in-flight frame's scene pass binds, written
    // into @p out. Returns 0 for a single-attachment frame, 2 for an MRT one
    // (slot 1 = linear depth, slot 2 = world normal).
    //
    // Every PSO submitted into the scene pass has to declare the same
    // attachment count or Vulkan and WebGPU reject the bind — including
    // shaders that write SV_Target0 alone, which is most of them. Ask this
    // rather than the render mode: the two agree for WC3 and stop agreeing for
    // a profile selected by product.
    u32 SceneExtraRtvFormats(gfx::Format out[2]) const;

    // There is deliberately no FrameRenderMode() accessor. It existed so
    // per-frame code could branch on HD vs SD, and every one of those branches
    // was really asking about the frame's shape — scene format, colour space,
    // attachment count, world scale. All of those live on IRenderProfile now,
    // and a profile selected by the scene's ProductId makes the mode's answer
    // wrong. `Impl::frameRenderMode_` survives as the host's input to
    // ActiveProfile / ActiveShadingModel and nothing else.

    // ---- Surface size + per-frame CB exposed for non-friend consumers
    //      (DebugRenderer, BLS pass templates, etc.) ----
    i32 Width() const;
    i32 Height() const;
    // The camera the in-flight frame is rendering from. Valid only while a
    // RenderFrame/RenderViewport call is on the stack; outside a frame it
    // falls back to the scene's camera. Passes and DebugRenderer read this
    // instead of Scene().Camera() so each viewport renders from its own
    // camera (Phase 3) without every call site knowing about viewports.
    const Camera& FrameCamera() const;
    gfx::BufferHandle CbPerFrame() const;
    RenderTarget* PrimaryTarget();
    i32 ComputeSelectedLod() const;

    // ---- Shadow PSO/CB handles read by shadow::ShadowPass.
    //      Bundled rather than friended so the pass class doesn't need
    //      access to the rest of RenderPipeline::Impl. ----
    struct ShadowResources {
        gfx::PipelineHandle psoSkinned;
        gfx::PipelineHandle psoRigid;
        gfx::BufferHandle vsCb;
    };
    ShadowResources Shadow() const;

    // ---- Scene-target formats (HD vs SD pipeline). Public so DebugRenderer
    //      and other consumers can build PSOs that match either path. ----
    static constexpr gfx::Format kHdrSceneFormat = gfx::Format::R11G11B10_FLOAT;
    // SD (classic WC3) renders gamma-space straight to an LDR target: textures
    // sample raw, geoset/light colours are gamma, and the result is written
    // WITHOUT a linear→sRGB encode (a plain UNORM RTV). Matches WC3's gamma
    // pipeline. (Was _SRGB, which linearised on write and desaturated tints.)
    static constexpr gfx::Format kSdSceneFormat = gfx::Format::R8G8B8A8_UNORM;
    // G-buffer slot-1 / slot-2 formats for the HD opaque MRT pass.
    // Match the engine's `s_worldFBHD` layout: linear view-space depth in
    // a full-precision float and an 8-bit packed world-space normal.
    static constexpr gfx::Format kLinearDepthFormat = gfx::Format::R32_FLOAT;
    static constexpr gfx::Format kNormalBufferFormat = gfx::Format::R8G8B8A8_UNORM;
    // GTAO output buffer: scalar visibility in [0, 1]. R8_UNORM is plenty
    // for the post-tonemap modulation budget — the noise floor of GTAO at
    // 4×4 sampling is already several LSBs above 8-bit precision.
    static constexpr gfx::Format kAoBufferFormat = gfx::Format::R8_UNORM;
    // Bent-normal G-buffer slot. xyz packs the view-space bent normal as
    // n*0.5+0.5; w carries the visibility factor so consumers don't need
    // to sample aoBuffer separately to weight the indirect term.
    static constexpr gfx::Format kBentNormalBufferFormat = gfx::Format::R8G8B8A8_UNORM;

private:
    // ---- Friends ----
    // GeosetPassBls / GeosetPassHd are defined inside render_pipeline.cpp
    // and reach BLS-specific internals (programs, CBs, PSO builder, IBL mip
    // extents). Everyone else uses the public accessors above.
    friend class GeosetPassBls;
    friend class GeosetPassHd;

    // Shared body of CurrentLinePSO / CurrentOverlayLinePSO.
    gfx::PipelineHandle LinePSO(bool depthTest, gfx::PipelineHandle hdrPso,
                                gfx::PipelineHandle& sdPso, gfx::Format& sdFormat) const;

    // Lazily builds every profile, then picks: the scene's product first, and
    // only the WC3 pair reads @p mode. ActiveProfile and LoadTimeProfile differ
    // solely in where that mode comes from.
    core::IRenderProfile& ProfileForMode(RenderMode mode);

    // ---- All formerly-RenderService private methods that touch GPU state ----
    void CleanupGFX();
    bool CreateShaders();
    bool CreatePipelines();
    bool CreateDefaultResources();
    void ReleaseModelGPU();
    void RunTonemapPass(const RenderTarget& target, gfx::TextureHandle dstColor);
    bool InitBlsShaders(gfx::GfxApi api);
    void ShutdownBlsShaders();
    bool RenderSplatsBls();
    // The shading model the active RenderMode selects, building the registry's
    // WC3 entries on first use. One model is live at a time; per-surface
    // selection is the mixed-shading work.
    shading::IShadingModel& ActiveShadingModel();
    void RenderGeosets(GeosetBucket bucket);
    // Unified back-to-front transparent pass: interleaves transparent geosets,
    // PE2 particles, ribbons and corn by camera distance (WC3's
    // IModelRenderSceneTransparent). Geoset draws go through SurfacePass, which
    // opens the naming model's pass on the first entry it dispatches.
    void RenderTransparentScene();
    void DrawParticleEmitter(const particle::EmitterDrawList& dl, const bls::FrameInputs& frame);
    // Build every actor's ribbon strips into their per-actor VBs and surface one
    // RibbonDrawUnit per emitter (with a world sort origin); `outFrame` is the
    // shared effect frame. DrawRibbonStrip renders one unit interleaved.
    void PrepareRibbons(std::vector<RibbonDrawUnit>& out, bls::FrameInputs& outFrame);
    void DrawRibbonStrip(const RibbonDrawUnit& u, const bls::FrameInputs& frame);
    void ApplyIblMode(IblMode mode);
    void SetEnvProbe(const std::string& relPath);
    void SetDayNightProbes(const std::string& dayPath, const std::string& nightPath);

    static constexpr const char* kIblSplitSumLutName = "ibl.splitSumLut";
    static constexpr const char* kIblFromProbeName = "ibl.fromProbe";
    static constexpr const char* kIblToProbeName = "ibl.toProbe";
    static constexpr const char* kIblDayProbeName = "ibl.dayProbe";
    static constexpr const char* kIblNightProbeName = "ibl.nightProbe";

    RenderService& rs_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace whiteout::flakes::renderer
