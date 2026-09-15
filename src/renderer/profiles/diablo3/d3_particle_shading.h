#pragma once

// ============================================================================
// The material half of a Diablo III particle — D3_PARTICLE_DESIGN.md §8, P5.
//
// Split from `d3_particle_adapter.cpp` for the same reason the surface table is
// split from the model adapter: the `.prt` states four texture ids and a
// ShaderMap, and turning those into a bound draw needs the ShaderMap chain
// (a cache) and an actor's texture scope (a live model). Neither belongs in a
// file-to-desc translation.
//
// Two calls, in order:
//
//   D3ResolveParticleMaterial  — the `.shm` -> `.shd` -> RenderPass chain, for
//                                blend, depth, alpha test and the fixed-function
//                                combine gains. Needs only the cache.
//   D3BindParticleTextures     — each layer's SNO -> an id in the owning actor's
//                                texture scope, staged if new. Needs the actor.
//
// They are separate because the first is a property of the FILE (every actor
// carrying this `.prt` gets the same answer) and the second is a property of
// the actor. A viewer that only wants to know what a `.prt` would draw calls
// the first alone, which is what the corpus gate does.
// ============================================================================

#include "gfx/gfx.h"
#include "renderer/particle/d3/d3_emitter_desc.h"
#include "renderer/particle/particle_service.h"
#include "renderer/stream_vertex_buffer.h"
#include "whiteout/flakes/types.h"

#include <memory>
#include <unordered_map>

namespace whiteout::sno::d3::native {
struct Particle;
}

namespace whiteout::flakes::io {
class D3SnoCache;
}

namespace whiteout::flakes::renderer::model {
struct Actor;
}

namespace whiteout::flakes::renderer {
class RenderService;
}

namespace whiteout::flakes::renderer::profiles::diablo3 {

namespace pd3 = ::whiteout::flakes::renderer::particle::d3;

/// @brief Where a particle's texture ids start in an actor's texture scope.
///
/// Past anything `CollectD3Textures` can produce, and by a margin: a look
/// change re-stages the model's own textures and can change how many there
/// are, so a particle id derived from that count would alias the model's after
/// a wardrobe change. The largest shipped appearance names 349.
inline constexpr i32 kD3ParticleTextureIdBase = 0x40000;

/// @brief Fill @p out's pass-derived state from @p prt's ShaderMap.
///
/// Leaves the defaults standing when nothing resolves — which is the shipped
/// answer, not a guess: every particle pass in the corpus culls nothing, writes
/// no depth and blends.
void D3ResolveParticleMaterial(const ::whiteout::sno::d3::native::Particle& prt,
                               ::whiteout::flakes::io::D3SnoCache* cache, pd3::MaterialDesc& out);

/// @brief Give every layer of @p desc a texture id on @p actor, and finish the
///        narrow @ref ParticleMaterialDesc the shared draw list carries.
///
/// Idempotent per (actor, texture SNO): two emitters naming the same `.tex` get
/// the same id and it is staged once. @p desc is taken by shared_ptr because
/// the material pointer the draw list carries ALIASES it — the chain is not
/// copied per frame, and it cannot outlive the emitter that owns it.
void D3BindParticleTextures(model::Actor& actor, const std::shared_ptr<pd3::EmitterDesc>& desc);

/// Everything a frame's particle draws share. Mirrors MultiTexFrameInputs: a
/// PSO is built against the attachments of the pass it runs in, and the
/// transparent pass is MRT in HD and single-target in SD.
struct D3ParticleFrameInputs {
    Matrix44f view = Matrix44f::identity();
    Matrix44f projection = Matrix44f::identity();

    gfx::Format rtvFormat = gfx::Format::Unknown;
    gfx::Format extraRtvFormats[gfx::GraphicsPipelineDesc::kMaxExtraColorAttachments] = {};
    u32 extraRtvCount = 0;
    gfx::Format dsvFormat = gfx::Format::D24_UNORM_S8_UINT;

    /// The shared particle stream. Only the fallback needs it — @ref
    /// D3ParticleShading::BeginFrame repacks it into a stream of its own — but
    /// it is carried here so `Draw` can bind whichever it uses, because
    /// `BindPipeline` re-sets the root signature on D3D12 and discards
    /// everything bound before it.
    gfx::BufferHandle vertexBuffer = gfx::BufferHandle::Invalid;
};

/// @brief The four stage binds, drawn.
///
/// Owns its shaders, its PSO cache and its constant buffers and draws INSIDE
/// the caller's already-open render pass — it is not a pass of its own, the
/// same split MultiTexParticleService uses. The vertex buffer is the caller's
/// shared particle stream, handed over in the frame inputs.
class D3ParticleShading {
public:
    explicit D3ParticleShading(RenderService& rs) : rs_(rs) {}
    ~D3ParticleShading() = default;

    D3ParticleShading(const D3ParticleShading&) = delete;
    D3ParticleShading& operator=(const D3ParticleShading&) = delete;

    /// Create the shaders and constant buffers. Idempotent, and a no-op before
    /// the device exists.
    void Init();
    /// Destroy every GPU object while the device is still alive.
    void ReleaseGpu();

    bool IsAvailable() const;

    /// @brief Repack this frame's D3 quads into the stream this program takes.
    ///
    /// A D3 particle needs FOUR texture coordinates per vertex, which the shared
    /// 48-byte `Vertex` has no room for — the engine's own particle vertex is 56
    /// bytes and spends four packed texcoords of it. So the emitter builds the
    /// ordinary stream plus a parallel `D3VertexStream`, and this interleaves the
    /// two into a vertex of its own once per frame. Index-parallel throughout,
    /// so a draw's `vertexOffset` addresses this buffer unchanged.
    ///
    /// Returns false when there is nothing to pack or the stream is absent, and
    /// @ref Draw then falls back to the shared vertex with its raw quad uv.
    bool BeginFrame(const std::vector<Vertex>& vertices, const particle::D3VertexStream& uv);

    /// @brief Draw one emitter's quads. False when it could not — no shader, no
    ///        PSO, no material — and the caller then falls back to the SD path,
    ///        which draws the diffuse layer alone.
    bool Draw(gfx::IGFXCommandList* cmd, const particle::EmitterDrawList& dl,
              const D3ParticleFrameInputs& frame, const model::Actor* owner);

private:
    gfx::PipelineHandle GetOrBuildPso(const pd3::MaterialDesc& m,
                                      const D3ParticleFrameInputs& frame);

    RenderService& rs_;
    bool initTried_ = false;
    /// The repacked stream and whether this frame filled it.
    StreamVertexBuffer vb_;
    bool frameReady_ = false;

    gfx::ShaderHandle vs_ = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle ps_ = gfx::ShaderHandle::Invalid;
    gfx::BufferHandle vsCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle psCb_ = gfx::BufferHandle::Invalid;

    std::unordered_map<u64, gfx::PipelineHandle> psos_;
};

} // namespace whiteout::flakes::renderer::profiles::diablo3
