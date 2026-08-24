#pragma once

// ============================================================================
// MultiTexParticleService — WoW's multi-texture particles.
//
// An M2 emitter with the `MultiTexture` flag combines THREE textures through
// three UV sets. Nothing else in the engine has three UV sets, so these
// emitters get their own vertex stream (built alongside the ordinary one by
// ParticleService) and their own pipeline; everything else about them — the
// blend state, the depth state, the back-to-front sort — is the same as a plain
// particle's, because in the client they are the same transparent pass.
//
// This owns the shaders, the PSO cache, the constant buffers and the vertex
// buffer, and draws INSIDE the caller's already-open render pass, exactly the
// way RenderPipeline::DrawParticleEmitter does. It is not a pass of its own —
// that is what makes it different from RefractionService, which is.
//
// Derivation of the combiner is in M2_MULTITEX_DESIGN.md.
// ============================================================================

#include "gfx/gfx.h"
#include "renderer/bls/bls_mat_params.h"
#include "renderer/particle/particle_service.h"
#include "renderer/types.h"
#include "whiteout/flakes/types.h"

#include <unordered_map>

namespace whiteout::flakes::renderer::particle {

/// Everything a frame's draws share. The render-target formats are here
/// because a PSO has to be built against the attachments of the pass it runs
/// in, and the transparent pass is MRT in HD and single-target in SD.
struct MultiTexFrameInputs {
    Matrix44f view = Matrix44f::identity();
    Matrix44f projection = Matrix44f::identity();

    gfx::Format rtvFormat = gfx::Format::Unknown;
    gfx::Format extraRtvFormats[gfx::GraphicsPipelineDesc::kMaxExtraColorAttachments] = {};
    u32 extraRtvCount = 0;
    gfx::Format dsvFormat = gfx::Format::D24_UNORM_S8_UINT;

    gfx::SamplerHandle wrapSampler = gfx::SamplerHandle::Invalid;
};

class MultiTexParticleService {
public:
    MultiTexParticleService() = default;
    ~MultiTexParticleService() = default;

    MultiTexParticleService(const MultiTexParticleService&) = delete;
    MultiTexParticleService& operator=(const MultiTexParticleService&) = delete;

    void Init(gfx::IGFXDevice& gfx, gfx::GfxApi api);
    void Shutdown();

    bool IsReady() const {
        return gfx_ != nullptr && shadersReady_;
    }

    /// Upload one frame's stream and latch the shared transforms. False when
    /// there is nothing to draw or the upload failed, in which case @ref Draw
    /// does nothing for the rest of the frame.
    bool BeginFrame(const MultiTexGeometry& geo, const MultiTexFrameInputs& frame);

    /// One emitter's quads, into the caller's open render pass. The three
    /// textures are resolved by the caller — the service knows nothing about
    /// actors or texture scopes, the same split DrawParticleEmitter uses.
    void Draw(gfx::IGFXCommandList* cmd, const EmitterDrawList& dl, gfx::TextureHandle tex0,
              gfx::TextureHandle tex1, gfx::TextureHandle tex2);

private:
    gfx::PipelineHandle GetOrBuildPso(const bls::MatParams& mat);

    gfx::IGFXDevice* gfx_ = nullptr;
    gfx::GfxApi api_ = gfx::GfxApi::D3D12;
    bool shadersReady_ = false;

    gfx::ShaderHandle vs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle ps_ = gfx::ShaderHandle::Invalid;

    gfx::BufferHandle vb_ = gfx::BufferHandle::Invalid;
    i32 vbCapacity_ = 0;
    gfx::BufferHandle vsCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle psCb_ = gfx::BufferHandle::Invalid;

    // Keyed on the material's alpha mode plus the pass's attachment formats,
    // which is everything that can vary between two draws here.
    std::unordered_map<u64, gfx::PipelineHandle> psoCache_;

    MultiTexFrameInputs frame_;
    bool frameReady_ = false;
};

} // namespace whiteout::flakes::renderer::particle
