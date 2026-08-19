#pragma once

// M3DeferredLightService — the DeferredLights pass (M3_SIMPLE_MATERIAL_DESIGN
// §4): one additive full-screen triangle over the M3 G-buffer sidecar, a loop
// over the frame's collected omni lights. The host half of
// m3_deferred_light.slang.
//
// Lifecycle mirrors GtaoService: Init at gfx-device creation, Shutdown before
// the device drops (CleanupGFX — the D3D12 crash-on-exit lesson), Run once
// per frame between the scene pass's EndRenderPass and the transparent queue.
// The caller collects and view-transforms the lights; this owns only GPU
// state, so it never reaches into the scene.

#include "gfx/gfx.h"
#include "whiteout/flakes/types.h"

#include <span>

namespace whiteout::flakes::renderer {
struct RenderTarget;
}

namespace whiteout::flakes::renderer::sc2 {

class M3DeferredLightService {
public:
    static constexpr u32 kMaxLights = 16;

    /// One light, already in view space, mirroring the shader CB's packing.
    struct Light {
        Vector3f posVS = {0.0f, 0.0f, 0.0f};
        f32 attenEnd = 0.0f;
        Vector3f color = {0.0f, 0.0f, 0.0f};
        f32 attenStart = 0.0f;
    };

    M3DeferredLightService() = default;
    ~M3DeferredLightService() = default;

    M3DeferredLightService(const M3DeferredLightService&) = delete;
    M3DeferredLightService& operator=(const M3DeferredLightService&) = delete;

    void Init(gfx::IGFXDevice& gfx, gfx::GfxApi api);
    void Shutdown();

    bool IsReady() const {
        return gfx_ != nullptr && vs_ != gfx::ShaderHandle::Invalid &&
               ps_ != gfx::ShaderHandle::Invalid;
    }

    /// @brief Add @p lights onto `target.hdrColor` (loadOp=Load, One/One
    ///        blend), reading the linearDepth / normalBuffer / gbufDiffuse
    ///        sidecar. A no-op when @p lights is empty. At most kMaxLights
    ///        are consumed; the caller decides which ones matter.
    ///        @p hdrFmt is hdrColor's format — the PSO is rebuilt if it moves.
    void Run(gfx::IGFXCommandList* cmd, RenderTarget& target, const Matrix44f& proj,
             std::span<const Light> lights, gfx::Format hdrFmt);

private:
    void EnsurePso(gfx::Format hdrFmt);

    // Mirror of m3_deferred_light.slang's M3DeferredLightData.
    struct alignas(16) Cb {
        Vector4f unproject; // .x 1/p00, .y 1/p11, .z light count
        Vector4f lightPos[kMaxLights];
        Vector4f lightColor[kMaxLights];
    };

    gfx::IGFXDevice* gfx_ = nullptr;
    gfx::ShaderHandle vs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle ps_ = gfx::ShaderHandle::Invalid;
    gfx::PipelineHandle pso_ = gfx::PipelineHandle::Invalid;
    gfx::Format psoHdrFmt_ = gfx::Format::Unknown;
    gfx::BufferHandle cb_ = gfx::BufferHandle::Invalid;
    gfx::SamplerHandle pointSampler_ = gfx::SamplerHandle::Invalid;
};

} // namespace whiteout::flakes::renderer::sc2
