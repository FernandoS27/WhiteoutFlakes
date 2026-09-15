#include "renderer/particle/output/multitex_particle_service.h"

#include "compiled_shaders.h"
#include "renderer/bls/bls_pso_builder.h"

#include <cstdio>

namespace whiteout::flakes::renderer::particle {

namespace {

// The interleaved vertex these draws take. Laid out exactly like the refraction
// mask's, and for the same reason: this is the engine's other stream with three
// UV sets, and the client builds both from the one `CGxVertexPCT3`.
struct MultiTexVertex {
    Vector3f position;
    Vector4f color;
    // The sprite UV and layer 1 share one attribute, and layer 2 rides TANGENT:
    // slang mangles a numbered semantic on a vertex INPUT (`TEXCOORD1` comes out
    // as index 10), so every element here has to name a digit-free semantic.
    Vector2f uv0;
    Vector2f uv1;
    Vector2f uv2;
};
static_assert(sizeof(MultiTexVertex) == 52, "MultiTexVertex must stay tightly packed");

struct MultiTexVsCb {
    Matrix44f world;
    Matrix44f view;
    Matrix44f projection;
};

struct MultiTexPsCb {
    f32 params[4];
};

} // namespace

void MultiTexParticleService::Init(gfx::IGFXDevice& gfx, gfx::GfxApi api) {
    gfx_ = &gfx;

    using namespace whiteout::flakes::Shaders;
    const u8* vs = kMultiTexParticleVS;
    usize vsN = sizeof(kMultiTexParticleVS);
    const u8* ps = kMultiTexParticlePS;
    usize psN = sizeof(kMultiTexParticlePS);
    if (api == gfx::GfxApi::Vulkan) {
        vs = kMultiTexParticleVSSpv;
        vsN = sizeof(kMultiTexParticleVSSpv);
        ps = kMultiTexParticlePSSpv;
        psN = sizeof(kMultiTexParticlePSSpv);
    } else if (api == gfx::GfxApi::WebGPU) {
        vs = kMultiTexParticleVSWgsl;
        vsN = sizeof(kMultiTexParticleVSWgsl);
        ps = kMultiTexParticlePSWgsl;
        psN = sizeof(kMultiTexParticlePSWgsl);
    } else if (api == gfx::GfxApi::Metal) {
        vs = kMultiTexParticleVSMtl;
        vsN = sizeof(kMultiTexParticleVSMtl);
        ps = kMultiTexParticlePSMtl;
        psN = sizeof(kMultiTexParticlePSMtl);
    }

    if (vsN > 1 && psN > 1) {
        vs_ = gfx_->CreateShader(gfx::ShaderStage::Vertex, vs, vsN);
        ps_ = gfx_->CreateShader(gfx::ShaderStage::Pixel, ps, psN);
    }

    vsCb_ = gfx_->CreateBuffer({
        .size = sizeof(MultiTexVsCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });
    // Mapped once per DRAW — the two combiner flags are per-emitter — so this
    // one needs the deep ring the other hot per-draw CBs get.
    gfx::BufferDesc pd;
    pd.size = sizeof(MultiTexPsCb);
    pd.usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable;
    pd.ringSlotsHint = 4096;
    psCb_ = gfx_->CreateBuffer(pd);

    shadersReady_ = vs_ != gfx::ShaderHandle::Invalid && ps_ != gfx::ShaderHandle::Invalid;
    if (!shadersReady_) {
        std::fprintf(stderr,
                     "[multitex] disabled — missing shader (vs=%llu ps=%llu); multi-texture "
                     "emitters will fall back to their first layer\n",
                     (unsigned long long)vs_, (unsigned long long)ps_);
    }
}

void MultiTexParticleService::Shutdown() {
    if (!gfx_)
        return;
    for (auto& [k, p] : psoCache_) {
        if (p != gfx::PipelineHandle::Invalid)
            gfx_->Destroy(p);
    }
    psoCache_.clear();
    for (gfx::ShaderHandle* s : {&vs_, &ps_}) {
        if (*s != gfx::ShaderHandle::Invalid)
            gfx_->Destroy(*s);
        *s = gfx::ShaderHandle::Invalid;
    }
    vb_.Release(*gfx_);
    for (gfx::BufferHandle* b : {&vsCb_, &psCb_}) {
        if (*b != gfx::BufferHandle::Invalid)
            gfx_->Destroy(*b);
        *b = gfx::BufferHandle::Invalid;
    }
    frameReady_ = false;
    shadersReady_ = false;
    gfx_ = nullptr;
}

bool MultiTexParticleService::BeginFrame(const MultiTexGeometry& geo,
                                         const MultiTexFrameInputs& frame) {
    frameReady_ = false;
    if (!IsReady())
        return false;
    const i32 count = static_cast<i32>(geo.vertices.size());
    if (count <= 0)
        return false;
    // The two arrays are built in lockstep by the geometry builder; a mismatch
    // means an emitter reached this stream without its UV sets, and drawing it
    // would read whatever the previous frame left behind.
    if (geo.extraUV.size() != geo.vertices.size())
        return false;

    if (!vb_.Upload<MultiTexVertex>(*gfx_, count, 4096,
                                    [&geo](MultiTexVertex* dst) { InterleaveMultiTex(geo, dst); }))
        return false;

    if (void* p = gfx_->MapBuffer(vsCb_)) {
        auto* cb = static_cast<MultiTexVsCb*>(p);
        cb->world = Matrix44f::identity();
        cb->view = frame.view.transpose();
        cb->projection = frame.projection.transpose();
        gfx_->UnmapBuffer(vsCb_);
    }

    frame_ = frame;
    frameReady_ = true;
    return true;
}

gfx::PipelineHandle MultiTexParticleService::GetOrBuildPso(const bls::MatParams& mat) {
    u64 key = static_cast<u64>(mat.alpha);
    key = key * 131 + (mat.disables & 0x1Fu);
    key = key * 131 + static_cast<u64>(frame_.rtvFormat);
    key = key * 131 + static_cast<u64>(frame_.dsvFormat);
    key = key * 131 + frame_.extraRtvCount;
    for (u32 i = 0; i < frame_.extraRtvCount; ++i)
        key = key * 131 + static_cast<u64>(frame_.extraRtvFormats[i]);

    if (auto it = psoCache_.find(key); it != psoCache_.end())
        return it->second;

    static const gfx::InputElement kLayout[] = {
        {"POSITION", 0, gfx::Format::R32G32B32_FLOAT, offsetof(MultiTexVertex, position)},
        {"COLOR", 0, gfx::Format::R32G32B32A32_FLOAT, offsetof(MultiTexVertex, color)},
        {"TEXCOORD", 0, gfx::Format::R32G32B32A32_FLOAT, offsetof(MultiTexVertex, uv0)},
        {"TANGENT", 0, gfx::Format::R32G32_FLOAT, offsetof(MultiTexVertex, uv2)},
    };

    gfx::GraphicsPipelineDesc d{};
    d.vs = vs_;
    d.ps = ps_;
    d.inputLayout = kLayout;
    d.inputSlotStrides[0] = sizeof(MultiTexVertex);
    d.topology = gfx::PrimitiveTopology::TriangleList;
    // Straight off the same MatParams the plain particle path builds, so a
    // multi-texture emitter blends and depth-tests exactly like the ordinary
    // ones it interleaves with — they are one pass in the client.
    d.blend = bls::BlendFor(mat.alpha);
    d.depthStencil = bls::DepthFor(mat);
    d.rasterizer.cull = gfx::CullMode::None;
    d.rasterizer.frontCCW = true;
    d.rtvFormat = frame_.rtvFormat;
    // Declared but not written: an MRT scene pass needs every PSO in it to
    // agree on the attachment count, and the cleared G-buffer data has to
    // survive a transparent draw.
    d.extraRtvCount = frame_.extraRtvCount;
    for (u32 i = 0; i < frame_.extraRtvCount; ++i)
        d.extraRtvFormats[i] = frame_.extraRtvFormats[i];
    d.extraColorWrite = false;
    d.dsvFormat = frame_.dsvFormat;

    const gfx::PipelineHandle pso = gfx_->CreateGraphicsPipeline(d);
    psoCache_.emplace(key, pso);
    return pso;
}

void MultiTexParticleService::Draw(gfx::IGFXCommandList* cmd, const EmitterDrawList& dl,
                                   gfx::TextureHandle tex0, gfx::TextureHandle tex1,
                                   gfx::TextureHandle tex2) {
    if (!cmd || !frameReady_ || dl.vertexCount <= 0)
        return;

    bls::MatParams mat = bls::FromParticleDesc(dl.material, bls::GxShaderID::SD);
    const gfx::PipelineHandle pso = GetOrBuildPso(mat);
    if (pso == gfx::PipelineHandle::Invalid)
        return;

    // Pipeline FIRST, then every root binding, and both re-issued per draw.
    // BindPipeline re-sets the root signature on D3D12, which discards constant
    // buffers, textures and samplers bound before it.
    cmd->BindPipeline(pso);
    cmd->BindVertexBuffer(0, vb_.Handle(), sizeof(MultiTexVertex));
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, vsCb_);

    if (void* p = gfx_->MapBuffer(psCb_)) {
        auto* cb = static_cast<MultiTexPsCb*>(p);
        cb->params[0] = dl.material.multiTexUse3Colors ? 1.0f : 0.0f;
        cb->params[1] = dl.material.multiTexModx4 ? 1.0f : 0.0f;
        cb->params[2] = (dl.material.filterMode == FilterMode::AlphaKey) ? 1.0f : 0.0f;
        cb->params[3] = 0.0f;
        gfx_->UnmapBuffer(psCb_);
    }
    // Slot 1, not 0: the shader pins its two constant buffers to b0 (vertex)
    // and b1 (pixel), because slang assigns registers per module rather than
    // per stage and the two would otherwise collide.
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1, psCb_);

    const gfx::TextureHandle tex[3] = {tex0, tex1, tex2};
    for (u32 i = 0; i < 3; ++i) {
        if (tex[i] != gfx::TextureHandle::Invalid)
            cmd->BindShaderResource(gfx::ShaderStage::Pixel, i, tex[i]);
    }
    // One sampler for all three layers: the client sets the wrap and filter
    // state once per emitter, not per texture slot.
    if (frame_.wrapSampler != gfx::SamplerHandle::Invalid)
        cmd->BindSampler(gfx::ShaderStage::Pixel, 0, frame_.wrapSampler);

    cmd->Draw(static_cast<u32>(dl.vertexCount), static_cast<u32>(dl.vertexOffset));
}

} // namespace whiteout::flakes::renderer::particle
