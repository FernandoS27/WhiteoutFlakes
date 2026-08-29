#include "renderer/profiles/diablo3/d3_particle_shading.h"

#include "compiled_shaders.h"
#include "io/d3/d3_types.h"
#include "renderer/assets/sampler_asset_manager.h"
#include "renderer/assets/texture_asset_manager.h"
#include "renderer/model/model_instance.h"
#include "renderer/profiles/diablo3/d3_standard_shading.h"
#include "renderer/profiles/diablo3/d3_surface_table.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"

#include <whiteout/sno/d3/native/types.h>

#include <cstddef>
#include <span>
#include <string>

namespace whiteout::flakes::renderer::profiles::diablo3 {

namespace d3n = ::whiteout::sno::d3::native;

namespace {

/// @brief The blend class the shared draw list carries.
///
/// A lossy summary, and knowingly so: the D3 path binds `blendSrc` / `blendDst`
/// straight onto the pipeline, and this exists for the fallback that draws a
/// D3 particle through the WC3 SD program when the D3 one is unavailable.
/// D3DBLEND numbering, from the shipped passes — (5, 6) on 115 of the corpus's
/// 223 `Billboard.fx` shaders and (5, 2) on 54.
particle::FilterMode BlendClassOf(const pd3::MaterialDesc& m) {
    if (!m.blendEnable)
        return m.alphaTest > 0.0f ? particle::FilterMode::AlphaKey : particle::FilterMode::Blend;
    if (m.blendDst == 2 /*ONE*/)
        return particle::FilterMode::Additive;
    if (m.blendSrc == 9 /*DESTCOLOR*/ || m.blendDst == 3 /*SRCCOLOR*/)
        return particle::FilterMode::Modulate;
    return particle::FilterMode::Blend;
}

} // namespace

void D3ResolveParticleMaterial(const d3n::Particle& prt, ::whiteout::flakes::io::D3SnoCache* cache,
                               pd3::MaterialDesc& out) {
    const D3PassState pass = D3PassStateFor(prt.tMaterial, cache);
    if (!pass.resolved)
        return;

    out.passResolved = true;
    out.blendEnable = pass.blendEnable;
    out.blendSrc = pass.blendSrc;
    out.blendDst = pass.blendDst;
    out.depthWrite = pass.depthWrite;
    out.alphaTest = static_cast<f32>(pass.alphaRef) / 255.0f;
    out.colorGain = pass.colorGain;
    out.alphaGain = pass.alphaGain;
    out.effectFile = pass.effectFile;

    // The fixed-function combine block, which every `Billboard.fx` pass carries
    // — and which says, per stage, whether its texture feeds the colour, the
    // alpha, or both. That is what separates the two jobs of type 19: over the
    // corpus's 243 billboard passes it is bound alpha-only by 55 and both ways
    // by 95, and it is named `alphaMap2Sampler` in the shipped programs.
    //
    // The gate is "does this stage SAMPLE the channel", not "does it modulate
    // it". A REPLACE or an ADD still feeds the channel and only differs in how,
    // and this shading model has no chain to put the difference in — while
    // gating on the modulate would drop the DIFFUSE's colour on 26 passes and
    // draw a plain quad. A stage the block does not name at all keeps the
    // layer's own default, which is "sample it and multiply it in".
    if (!pass.stageArgs)
        return;
    for (u32 i = 0; i < out.layerCount; ++i) {
        const u64 bit = ::whiteout::flakes::io::D3TypeBit(out.layers[i].rawType);
        if ((pass.namedTypes & bit) == 0)
            continue;
        out.layers[i].samplesColor = (pass.colorSampledTypes & bit) != 0;
        out.layers[i].samplesAlpha = (pass.alphaSampledTypes & bit) != 0;
    }
}

void D3BindParticleTextures(model::Actor& actor, const std::shared_ptr<pd3::EmitterDesc>& desc) {
    if (!desc)
        return;
    pd3::MaterialDesc& m = desc->d3mat;

    for (u32 i = 0; i < m.layerCount; ++i) {
        pd3::MaterialLayer& L = m.layers[i];
        if (L.textureSno < 0)
            continue;
        auto [it, fresh] = actor.d3ParticleTextures.try_emplace(
            L.textureSno,
            kD3ParticleTextureIdBase + static_cast<i32>(actor.d3ParticleTextures.size()));
        L.textureId = it->second;
        if (!fresh)
            continue;
        model::StagedTexture& st = actor.render.stagedTextures[L.textureId];
        // "#<snoId>" is the form UploadStagedTextures parses into an
        // id-addressed ContentRef; a scheme-prefixed key would be read as a
        // path and leave the layer white with no error anywhere.
        st.sharedKey = "#" + std::to_string(L.textureSno);
        st.wrapFlags = L.wrapFlags;
        actor.render.stagedDirty = true;
    }

    desc->material.textureId = m.DiffuseTextureId();
    desc->material.filterMode = BlendClassOf(m);
    desc->material.unshaded = true;
    desc->material.unfogged = false;
    // Aliasing: the chain stays inside the desc the emitter holds, so the draw
    // list carries a refcount rather than four layers and a string.
    desc->material.d3 = std::shared_ptr<const pd3::MaterialDesc>(desc, &desc->d3mat);
}


// ---------------------------------------------------------------------------
// The draw
// ---------------------------------------------------------------------------

namespace {

struct D3ParticleVsCb {
    Matrix44f view;
    Matrix44f projection;
};

struct D3ParticlePsCb {
    Vector4f uvRow0[pd3::MaterialDesc::kMaxLayers];
    Vector4f uvRow1[pd3::MaterialDesc::kMaxLayers];
    Vector4f params;
};

/// Everything that can vary between two of these draws, in one key. The
/// attachment formats are in it because a PSO belongs to the pass it runs in
/// and the transparent pass is MRT in HD and single-target in SD.
u64 PsoKeyOf(const pd3::MaterialDesc& m, const D3ParticleFrameInputs& f) {
    auto fmt = [](gfx::Format v) { return static_cast<u64>(v) & 0xFFull; };
    u64 k = m.blendEnable ? 1ull : 0ull;
    k |= static_cast<u64>(m.blendSrc & 0x1F) << 1;
    k |= static_cast<u64>(m.blendDst & 0x1F) << 6;
    k |= (m.depthWrite ? 1ull : 0ull) << 11;
    k |= fmt(f.rtvFormat) << 12;
    k |= fmt(f.dsvFormat) << 20;
    k |= static_cast<u64>(f.extraRtvCount & 0x3) << 28;
    k |= fmt(f.extraRtvFormats[0]) << 30;
    k |= fmt(f.extraRtvFormats[1]) << 38;
    k |= fmt(f.extraRtvFormats[2]) << 46;
    return k;
}

} // namespace

void D3ParticleShading::Init() {
    if (initTried_)
        return;
    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return;
    initTried_ = true;

    using namespace whiteout::flakes::Shaders;
    const gfx::GfxApi api = gfxDev->GetApi();
    auto mk = [&](gfx::ShaderStage stage, const u8* dxbc, usize dxbcN, const u8* spv, usize spvN,
                  const u8* wgsl, usize wgslN, const u8* mtl, usize mtlN) {
        switch (api) {
        case gfx::GfxApi::Vulkan:
            return gfxDev->CreateShader(stage, spv, spvN);
        case gfx::GfxApi::WebGPU:
            return gfxDev->CreateShader(stage, wgsl, wgslN);
        case gfx::GfxApi::Metal:
            return gfxDev->CreateShader(stage, mtl, mtlN);
        default:
            return gfxDev->CreateShader(stage, dxbc, dxbcN);
        }
    };
#define WDX_D3P_BLOB(name)                                                                         \
    k##name, sizeof(k##name), k##name##Spv, sizeof(k##name##Spv), k##name##Wgsl,                   \
        sizeof(k##name##Wgsl), k##name##Mtl, sizeof(k##name##Mtl)
    vs_ = mk(gfx::ShaderStage::Vertex, WDX_D3P_BLOB(D3ParticleVS));
    ps_ = mk(gfx::ShaderStage::Pixel, WDX_D3P_BLOB(D3ParticlePS));
#undef WDX_D3P_BLOB

    // Written once per frame; every draw in it shares the transforms.
    vsCb_ = gfxDev->CreateBuffer({
        .size = sizeof(D3ParticleVsCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });
    // Mapped once per DRAW — the UV affines move per emitter — so this one
    // needs the deep ring every hot per-draw CB gets.
    gfx::BufferDesc pd;
    pd.size = sizeof(D3ParticlePsCb);
    pd.usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable;
    pd.ringSlotsHint = 4096;
    psCb_ = gfxDev->CreateBuffer(pd);
}

void D3ParticleShading::ReleaseGpu() {
    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return;
    for (auto& [k, pso] : psos_) {
        if (pso != gfx::PipelineHandle::Invalid)
            gfxDev->Destroy(pso);
    }
    psos_.clear();
    for (gfx::BufferHandle* b : {&vsCb_, &psCb_}) {
        if (*b != gfx::BufferHandle::Invalid)
            gfxDev->Destroy(*b);
        *b = gfx::BufferHandle::Invalid;
    }
    vs_ = gfx::ShaderHandle::Invalid;
    ps_ = gfx::ShaderHandle::Invalid;
    initTried_ = false;
}

bool D3ParticleShading::IsAvailable() const {
    return vs_ != gfx::ShaderHandle::Invalid && ps_ != gfx::ShaderHandle::Invalid &&
           vsCb_ != gfx::BufferHandle::Invalid && psCb_ != gfx::BufferHandle::Invalid;
}

gfx::PipelineHandle D3ParticleShading::GetOrBuildPso(const pd3::MaterialDesc& m,
                                                     const D3ParticleFrameInputs& frame) {
    const u64 key = PsoKeyOf(m, frame);
    if (auto it = psos_.find(key); it != psos_.end())
        return it->second;
    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return gfx::PipelineHandle::Invalid;

    // The shared particle stream, declared by hand: it is a plain `Vertex` and
    // the element order has to match the VS input struct, because Vulkan,
    // WebGPU and Metal derive shader locations from array position.
    const gfx::InputElement elements[] = {
        {"POSITION", 0, gfx::Format::R32G32B32_FLOAT, offsetof(Vertex, position), 0},
        {"NORMAL", 0, gfx::Format::R32G32B32_FLOAT, offsetof(Vertex, normal), 0},
        {"COLOR", 0, gfx::Format::R32G32B32A32_FLOAT, offsetof(Vertex, color), 0},
        {"TEXCOORD", 0, gfx::Format::R32G32_FLOAT, offsetof(Vertex, uv), 0},
    };

    gfx::GraphicsPipelineDesc desc{};
    desc.vs = vs_;
    desc.ps = ps_;
    desc.inputLayout = std::span<const gfx::InputElement>(elements);
    desc.inputSlotStrides[0] = sizeof(Vertex);
    desc.topology = gfx::PrimitiveTopology::TriangleList;
    desc.blend.enable = m.blendEnable;
    desc.blend.srcColor = D3BlendFactor(m.blendSrc, gfx::BlendFactor::SrcAlpha);
    desc.blend.dstColor = D3BlendFactor(m.blendDst, gfx::BlendFactor::One);
    // The alpha channel takes the same pair: a RenderPass carries ONE (src,
    // dst), so splitting them here would be inventing a second one.
    desc.blend.srcAlpha = desc.blend.srcColor;
    desc.blend.dstAlpha = desc.blend.dstColor;
    desc.depthStencil.depthTest = true;
    // Clear on 223 of 223 shipped particle passes, but read rather than
    // assumed: an asset that writes depth is saying its quads are solid.
    desc.depthStencil.depthWrite = m.depthWrite;
    desc.depthStencil.depthCompare = gfx::CompareOp::LessEqual;
    // Cull is a pass property and every particle pass but one asks for none,
    // which is also the only answer a camera-facing quad can take: its winding
    // flips with the camera.
    desc.rasterizer.cull = gfx::CullMode::None;
    desc.rtvFormat = frame.rtvFormat;
    desc.dsvFormat = frame.dsvFormat;
    for (u32 i = 0; i < gfx::GraphicsPipelineDesc::kMaxExtraColorAttachments; ++i)
        desc.extraRtvFormats[i] = frame.extraRtvFormats[i];
    desc.extraRtvCount = frame.extraRtvCount;
    // Writes SV_Target0 alone; the extra attachments are declared only to match
    // the host pass and must keep their cleared contents.
    desc.extraColorWrite = false;

    const gfx::PipelineHandle pso = gfxDev->CreateGraphicsPipeline(desc);
    psos_[key] = pso;
    return pso;
}

bool D3ParticleShading::Draw(gfx::IGFXCommandList* cmd, const particle::EmitterDrawList& dl,
                             const D3ParticleFrameInputs& frame, const model::Actor* owner) {
    if (!cmd || !IsAvailable() || !dl.material.d3)
        return false;
    const pd3::MaterialDesc& m = *dl.material.d3;
    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return false;

    const gfx::PipelineHandle pso = GetOrBuildPso(m, frame);
    if (pso == gfx::PipelineHandle::Invalid)
        return false;

    if (auto* c = static_cast<D3ParticleVsCb*>(gfxDev->MapBuffer(vsCb_))) {
        c->view = frame.view.transpose();
        c->projection = frame.projection.transpose();
        gfxDev->UnmapBuffer(vsCb_);
    }
    if (auto* c = static_cast<D3ParticlePsCb*>(gfxDev->MapBuffer(psCb_))) {
        for (u32 i = 0; i < pd3::MaterialDesc::kMaxLayers; ++i) {
            f32 a[6] = {1, 0, 0, 0, 1, 0};
            f32 colorOn = 0.0f;
            f32 alphaOn = 0.0f;
            if (i < m.layerCount) {
                const pd3::MaterialLayer& L = m.layers[i];
                ::whiteout::flakes::io::D3UvAffine(L.uv, dl.materialTimeSec, a);
                colorOn = L.samplesColor ? 1.0f : 0.0f;
                alphaOn = L.samplesAlpha ? 1.0f : 0.0f;
            }
            c->uvRow0[i] = {a[0], a[1], a[2], colorOn};
            c->uvRow1[i] = {a[3], a[4], a[5], alphaOn};
        }
        c->params = {m.colorGain, m.alphaGain, m.alphaTest, 0.0f};
        gfxDev->UnmapBuffer(psCb_);
    }

    // Pipeline FIRST, then every root binding, and the vertex buffer with them:
    // `BindPipeline` re-sets the root signature on D3D12 and discards constant
    // buffers, textures and samplers bound before it.
    cmd->BindPipeline(pso);
    if (frame.vertexBuffer != gfx::BufferHandle::Invalid)
        cmd->BindVertexBuffer(0, frame.vertexBuffer, sizeof(Vertex));
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, vsCb_);
    // Slot 1, not 0: slang assigns registers per MODULE, so the two constant
    // buffers are pinned to b0 and b1 or they collide.
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1, psCb_);

    // Every declared slot, every draw: a binding Slang kept but the host left
    // unwritten is a Vulkan validation error, not a harmless omission.
    const auto& defaults = rs_.Textures().GetDefaults();
    for (u32 i = 0; i < pd3::MaterialDesc::kMaxLayers; ++i) {
        gfx::TextureHandle tex = gfx::TextureHandle::Invalid;
        u32 wrap = 0x3;
        if (i < m.layerCount) {
            wrap = m.layers[i].wrapFlags;
            if (owner && owner->render.textures && m.layers[i].textureId >= 0)
                tex = owner->render.textures->Get(m.layers[i].textureId);
        }
        // White, not black: an unbound stage multiplies by one, which is what
        // its channel switch already says. A texture that has not landed yet
        // must not turn the whole chain off.
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, i,
                                tex != gfx::TextureHandle::Invalid ? tex : defaults.White);
        cmd->BindSampler(gfx::ShaderStage::Pixel, i, rs_.Samplers().WrapVariant(wrap));
    }

    cmd->Draw(dl.vertexCount, dl.vertexOffset);
    return true;
}

} // namespace whiteout::flakes::renderer::profiles::diablo3
