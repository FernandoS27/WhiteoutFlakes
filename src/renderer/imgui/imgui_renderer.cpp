#include "renderer/imgui/imgui_renderer.h"

#if WDX_ENABLE_IMGUI

#include "renderer/bls/bls_shader_cache.h"
#include "renderer/perf_zone.h"

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace whiteout::flakes::renderer::dear_imgui {

namespace {

// The BLS imgui VS declares a 4-attribute input signature
// (externals/Wc3Shaders/wc3_shaders/types/vs_io.slang::ImGuiVSInput):
//   ATTR0 float3 position
//   ATTR1 float3 _unused1    ← present in the OSGN, ignored by the shader
//   ATTR2 float4 vertColor
//   ATTR3 float2 uv
// ImDrawVert is only 20 bytes (pos2 / uv2 / col_u32), so we transcode each
// frame into this 48-byte engine vertex. The alternative — authoring a
// custom ImGui-specific BLS that matches ImDrawVert directly — was
// rejected: we want to keep the shipped Blizzard ImGui shader as the
// reference so the engine and the game render through bit-identical
// bytecode.
struct EngineImGuiVert {
    f32 position[3];  // ATTR0
    f32 _unused[3];   // ATTR1
    f32 vertColor[4]; // ATTR2
    f32 uv[2];        // ATTR3
};
static_assert(sizeof(EngineImGuiVert) == 48,
              "Vertex layout must match BLS imgui VS input signature");

// ImGuiVSPerDraw at register(b2): 8 float4 of padding then 4 float4
// projection columns. The shader only reads cb2[8] / cb2[9] / cb2[11];
// cb2[10] is reserved for a future Z column.
struct ImGuiVsCb {
    f32 _pad[8 * 4];
    f32 projection[4 * 4]; // 4 columns, column-major
};
static_assert(sizeof(ImGuiVsCb) == 192, "ImGuiVSPerDraw layout drift");

constexpr u32 kVbMinFloor = 4096;
constexpr u32 kIbMinFloor = 8192;

} // namespace

struct ImGuiRenderer::Impl {
    gfx::IGFXDevice* device = nullptr;
    bls::BlsShaderCache* shaderCache = nullptr;

    bls::BlsShader* vs = nullptr;
    bls::BlsShader* ps = nullptr;

    gfx::PipelineHandle pso = gfx::PipelineHandle::Invalid;
    // Cached attachment formats — used to rebuild the PSO when SetRtvFormat
    // sees a new colour-target format (macOS / WebGPU swapchains can land
    // on BGRA8 while the rest of the engine targets RGBA8).
    gfx::Format rtvFormat = gfx::Format::Unknown;
    gfx::Format dsvFormat = gfx::Format::Unknown;
    gfx::BufferHandle vsCb = gfx::BufferHandle::Invalid;
    gfx::TextureHandle fontAtlas = gfx::TextureHandle::Invalid;
    gfx::SamplerHandle sampler = gfx::SamplerHandle::Invalid;

    gfx::BufferHandle vb = gfx::BufferHandle::Invalid;
    gfx::BufferHandle ib = gfx::BufferHandle::Invalid;
    u32 vbCap = 0;
    u32 ibCap = 0;

    // Reusable transcode scratch.
    std::vector<EngineImGuiVert> vertScratch;
    std::vector<ImDrawIdx> idxScratch;

    // GPU state (PSO + CB + sampler + VB/IB) ready to bind. Set in the
    // ctor; doesn't depend on whether the host has created an ImGui
    // context yet.
    bool gpuReady = false;

    // The font atlas is built on demand from the active ImGui context the
    // first time Render() runs after one exists. The engine's
    // EnsureImGui() fires during Pipeline().InitDevice(), which a Max-
    // plugin host calls without ever creating an ImGui context — touching
    // ImGui::GetIO() there would crash. Hosts that *do* use ImGui (the
    // viewer) must just make sure ImGui::CreateContext() runs before the
    // first frame is rendered.
    bool fontReady = false;
};

namespace {

bool BuildFontAtlas(ImGuiRenderer::Impl& im) {
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int w = 0;
    int h = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &w, &h);
    if (!pixels || w <= 0 || h <= 0)
        return false;

    gfx::TextureDesc td;
    td.width = w;
    td.height = h;
    td.mipLevels = 1;
    td.arraySize = 1;
    // The PS sRGB-decode permute reads through an sRGB-typed SRV, which
    // matches how ImGui's default style colours are authored.
    td.format = gfx::Format::R8G8B8A8_UNORM_SRGB;
    td.usage = gfx::TextureUsage::ShaderResource;
    im.fontAtlas = im.device->CreateTexture(td, pixels);
    if (im.fontAtlas == gfx::TextureHandle::Invalid)
        return false;

    // ImGui stores per-draw texture identity as ImTextureID (ImU64 since
    // 1.92); we encode our gfx::TextureHandle (enum class : u64) verbatim
    // and reverse it inside Render().
    io.Fonts->SetTexID(static_cast<ImTextureID>(static_cast<u64>(im.fontAtlas)));
    return true;
}

bool EnsureVertexBuffer(ImGuiRenderer::Impl& im, u32 vertexCount) {
    if (vertexCount <= im.vbCap && im.vb != gfx::BufferHandle::Invalid)
        return true;
    im.device->Destroy(im.vb);
    im.vbCap = std::max(vertexCount, std::max<u32>(im.vbCap * 2u, kVbMinFloor));
    gfx::BufferDesc bd;
    bd.size = sizeof(EngineImGuiVert) * im.vbCap;
    bd.usage = gfx::BufferUsage::Vertex | gfx::BufferUsage::CpuWritable;
    im.vb = im.device->CreateBuffer(bd);
    return im.vb != gfx::BufferHandle::Invalid;
}

bool EnsureIndexBuffer(ImGuiRenderer::Impl& im, u32 indexCount) {
    if (indexCount <= im.ibCap && im.ib != gfx::BufferHandle::Invalid)
        return true;
    im.device->Destroy(im.ib);
    im.ibCap = std::max(indexCount, std::max<u32>(im.ibCap * 2u, kIbMinFloor));
    gfx::BufferDesc bd;
    bd.size = sizeof(ImDrawIdx) * im.ibCap;
    bd.usage = gfx::BufferUsage::Index | gfx::BufferUsage::CpuWritable;
    im.ib = im.device->CreateBuffer(bd);
    return im.ib != gfx::BufferHandle::Invalid;
}

} // namespace

ImGuiRenderer::ImGuiRenderer(gfx::IGFXDevice& device, bls::BlsShaderCache& shaderCache,
                             gfx::Format rtvFormat, gfx::Format dsvFormat)
    : impl_(std::make_unique<Impl>()) {
    impl_->device = &device;
    impl_->shaderCache = &shaderCache;

    impl_->vs = shaderCache.Acquire(gfx::ShaderStage::Vertex, "imgui");
    impl_->ps = shaderCache.Acquire(gfx::ShaderStage::Pixel, "imgui");
    if (!impl_->vs || impl_->vs->permuteHandles.empty() || !impl_->ps ||
        impl_->ps->permuteHandles.empty()) {
        std::fprintf(stderr,
                     "[imgui] BLS shaders missing — imgui.bls not packaged? "
                     "vs=%p ps=%p\n",
                     (void*)impl_->vs, (void*)impl_->ps);
        return;
    }

    impl_->dsvFormat = dsvFormat;
    // PSO is built lazily by SetRtvFormat below. Doing it eagerly here
    // would lock in whatever rtvFormat the caller guessed at ctor time —
    // but the actual swapchain backbuffer format isn't known until the
    // first swap chain is created, which happens after EnsureImGui. On
    // macOS / WebGPU / Dawn-D3D12 the swapchain ends up BGRA8 instead of
    // RGBA8 and the eager PSO mismatches the renderpass, silently dropping
    // every UI draw (and the present blit alongside it).

    gfx::BufferDesc cbd;
    cbd.size = sizeof(ImGuiVsCb);
    cbd.usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable;
    // The CB is mapped once per Render() — no need for a huge ring.
    cbd.ringSlotsHint = 64;
    impl_->vsCb = device.CreateBuffer(cbd);

    gfx::SamplerDesc sd;
    sd.minFilter = gfx::Filter::Linear;
    sd.magFilter = gfx::Filter::Linear;
    sd.addressU = gfx::AddressMode::Clamp;
    sd.addressV = gfx::AddressMode::Clamp;
    sd.addressW = gfx::AddressMode::Clamp;
    impl_->sampler = device.CreateSampler(sd);

    impl_->gpuReady = true;

    // Seed the PSO with the format the caller predicted; SetRtvFormat
    // will rebuild it if RenderPipeline updates us with the actual
    // swapchain format before the first draw.
    SetRtvFormat(rtvFormat);

    // ImGui 1.92+ asserts inside ImGui::NewFrame() if the atlas hasn't
    // been built and the backend hasn't opted into
    // ImGuiBackendFlags_RendererHasTextures. Build it now if a context
    // exists; for hosts that don't use ImGui (Max plugin) we just skip
    // and the lazy path in Render() never fires either.
    if (ImGui::GetCurrentContext()) {
        if (BuildFontAtlas(*impl_)) {
            impl_->fontReady = true;
        } else {
            std::fprintf(stderr, "[imgui] font atlas build FAILED in ctor\n");
        }
    }
}

ImGuiRenderer::~ImGuiRenderer() {
    if (!impl_ || !impl_->device)
        return;
    if (impl_->pso != gfx::PipelineHandle::Invalid)
        impl_->device->Destroy(impl_->pso);
    if (impl_->vsCb != gfx::BufferHandle::Invalid)
        impl_->device->Destroy(impl_->vsCb);
    if (impl_->fontAtlas != gfx::TextureHandle::Invalid)
        impl_->device->Destroy(impl_->fontAtlas);
    if (impl_->sampler != gfx::SamplerHandle::Invalid)
        impl_->device->Destroy(impl_->sampler);
    if (impl_->vb != gfx::BufferHandle::Invalid)
        impl_->device->Destroy(impl_->vb);
    if (impl_->ib != gfx::BufferHandle::Invalid)
        impl_->device->Destroy(impl_->ib);
    if (impl_->shaderCache) {
        impl_->shaderCache->Release(impl_->vs);
        impl_->shaderCache->Release(impl_->ps);
    }
    // Drop ImGui's reference to the destroyed atlas if a context is still
    // alive (the host owns the context lifetime).
    if (ImGui::GetCurrentContext())
        ImGui::GetIO().Fonts->SetTexID(ImTextureID_Invalid);
}

bool ImGuiRenderer::IsReady() const {
    return impl_ && impl_->gpuReady && impl_->pso != gfx::PipelineHandle::Invalid;
}

void ImGuiRenderer::SetRtvFormat(gfx::Format rtvFormat) {
    if (!impl_ || !impl_->device)
        return;
    if (rtvFormat == gfx::Format::Unknown)
        return;
    if (impl_->pso != gfx::PipelineHandle::Invalid && impl_->rtvFormat == rtvFormat)
        return;
    if (!impl_->vs || impl_->vs->permuteHandles.empty() || !impl_->ps ||
        impl_->ps->permuteHandles.empty())
        return;

    if (impl_->pso != gfx::PipelineHandle::Invalid)
        impl_->device->Destroy(impl_->pso);

    // PS permute 0 = HAS_SRGB_DECODE on. The backbuffer view is sRGB so the
    // hardware re-encodes on store; running the blend in linear space (and
    // therefore linearising ImGui's sRGB-authored vertex colours first)
    // matches what users see in every other engine that follows the same
    // ImGui-on-sRGB-RTV recipe.
    constexpr u32 kPsPermSrgbDecodeOn = 0;

    static const gfx::InputElement kImGuiInput[] = {
        {"ATTR", 0, gfx::Format::R32G32B32_FLOAT, 0},
        {"ATTR", 1, gfx::Format::R32G32B32_FLOAT, 12},
        {"ATTR", 2, gfx::Format::R32G32B32A32_FLOAT, 24},
        {"ATTR", 3, gfx::Format::R32G32_FLOAT, 40},
    };

    gfx::GraphicsPipelineDesc pd;
    pd.vs = impl_->vs->permuteHandles[0];
    pd.ps = impl_->ps->permuteHandles[kPsPermSrgbDecodeOn];
    pd.inputLayout = kImGuiInput;
    pd.topology = gfx::PrimitiveTopology::TriangleList;
    pd.blend.enable = true;
    pd.blend.srcColor = gfx::BlendFactor::SrcAlpha;
    pd.blend.dstColor = gfx::BlendFactor::InvSrcAlpha;
    pd.blend.opColor = gfx::BlendOp::Add;
    pd.blend.srcAlpha = gfx::BlendFactor::One;
    pd.blend.dstAlpha = gfx::BlendFactor::InvSrcAlpha;
    pd.blend.opAlpha = gfx::BlendOp::Add;
    pd.depthStencil.depthTest = false;
    pd.depthStencil.depthWrite = false;
    pd.rasterizer.cull = gfx::CullMode::None;
    pd.rasterizer.frontCCW = true;
    // ImGui drives clip rects via per-command scissors. Without this the
    // d3d12 / vulkan backends ignore SetScissor entirely.
    pd.rasterizer.scissorEnable = true;
    pd.rtvFormat = rtvFormat;
    pd.dsvFormat = impl_->dsvFormat;
    impl_->pso = impl_->device->CreateGraphicsPipeline(pd);
    if (impl_->pso == gfx::PipelineHandle::Invalid) {
        std::fprintf(stderr, "[imgui] PSO creation FAILED (rtvFormat=%d)\n",
                     static_cast<int>(rtvFormat));
        return;
    }
    impl_->rtvFormat = rtvFormat;
}

void ImGuiRenderer::Render(gfx::IGFXCommandList& cmd, i32 viewportW, i32 viewportH) {
    if (!IsReady())
        return;
    if (!ImGui::GetCurrentContext())
        return;

    Impl& im = *impl_;

    // First frame after a context appears: build the font atlas. Done
    // here rather than in the ctor because a Max-plugin host calls
    // EnsureImGui() (via Pipeline().InitDevice) without ever creating an
    // ImGui context; touching io.Fonts there would crash.
    if (!im.fontReady) {
        if (!BuildFontAtlas(im)) {
            std::fprintf(stderr, "[imgui] font atlas build FAILED\n");
            return;
        }
        im.fontReady = true;
    }

    ImDrawData* draw = ImGui::GetDrawData();
    if (!draw || draw->CmdListsCount == 0 || draw->TotalVtxCount == 0 || viewportW <= 0 ||
        viewportH <= 0)
        return;

    if (!EnsureVertexBuffer(im, static_cast<u32>(draw->TotalVtxCount)))
        return;
    if (!EnsureIndexBuffer(im, static_cast<u32>(draw->TotalIdxCount)))
        return;

    // Transcode ImDrawVert (20 B) → EngineImGuiVert (48 B) per the format
    // mismatch documented at the top of this file. Indices come through as
    // ImDrawIdx with a per-drawlist vertex offset folded in so the merged
    // VB+IB can issue one BindVertexBuffer / BindIndexBuffer for the whole
    // frame.
    im.vertScratch.resize(static_cast<usize>(draw->TotalVtxCount));
    im.idxScratch.resize(static_cast<usize>(draw->TotalIdxCount));

    EngineImGuiVert* vDst = im.vertScratch.data();
    ImDrawIdx* iDst = im.idxScratch.data();
    u32 vtxOffset = 0;
    u32 idxOffset = 0;
    struct ListRange {
        u32 vtxBase;
        u32 idxBase;
        const ImDrawList* list;
    };
    std::vector<ListRange> ranges;
    ranges.reserve(static_cast<usize>(draw->CmdListsCount));

    for (int n = 0; n < draw->CmdListsCount; ++n) {
        const ImDrawList* list = draw->CmdLists[n];
        const ImDrawVert* vSrc = list->VtxBuffer.Data;
        const i32 vCount = list->VtxBuffer.Size;
        for (i32 i = 0; i < vCount; ++i) {
            const ImDrawVert& s = vSrc[i];
            vDst[i].position[0] = s.pos.x;
            vDst[i].position[1] = s.pos.y;
            vDst[i].position[2] = 0.0f;
            vDst[i].uv[0] = s.uv.x;
            vDst[i].uv[1] = s.uv.y;
            vDst[i]._unused[0] = 0.0f;
            vDst[i]._unused[1] = 0.0f;
            vDst[i]._unused[2] = 0.0f;
            const u32 c = s.col;
            // ImU32 is RGBA8 packed (R in low byte) per imconfig.h default.
            vDst[i].vertColor[0] = ((c >> 0) & 0xFFu) / 255.0f;
            vDst[i].vertColor[1] = ((c >> 8) & 0xFFu) / 255.0f;
            vDst[i].vertColor[2] = ((c >> 16) & 0xFFu) / 255.0f;
            vDst[i].vertColor[3] = ((c >> 24) & 0xFFu) / 255.0f;
        }

        const ImDrawIdx* iSrc = list->IdxBuffer.Data;
        const i32 iCount = list->IdxBuffer.Size;
        std::memcpy(iDst, iSrc, sizeof(ImDrawIdx) * static_cast<usize>(iCount));

        ranges.push_back({vtxOffset, idxOffset, list});
        vDst += vCount;
        iDst += iCount;
        vtxOffset += static_cast<u32>(vCount);
        idxOffset += static_cast<u32>(iCount);
    }

    if (void* p = im.device->MapBuffer(im.vb)) {
        std::memcpy(p, im.vertScratch.data(), sizeof(EngineImGuiVert) * im.vertScratch.size());
        im.device->UnmapBuffer(im.vb);
    }
    if (void* p = im.device->MapBuffer(im.ib)) {
        std::memcpy(p, im.idxScratch.data(), sizeof(ImDrawIdx) * im.idxScratch.size());
        im.device->UnmapBuffer(im.ib);
    }

    // Column-major 2D orthographic. ImGui's DisplayPos / DisplaySize lets a
    // host (multi-viewport, retina, etc.) put the visible UI somewhere
    // other than (0, 0). We bake (-1 - 2*L/W, 1 + 2*T/H) into the
    // translation column so the PS sees correct NDC coords regardless.
    const f32 L = draw->DisplayPos.x;
    const f32 T = draw->DisplayPos.y;
    const f32 W = draw->DisplaySize.x;
    const f32 H = draw->DisplaySize.y;
    if (W <= 0.0f || H <= 0.0f)
        return;

    if (void* p = im.device->MapBuffer(im.vsCb)) {
        ImGuiVsCb cb{};
        // col0 (cb2[8])
        cb.projection[0] = 2.0f / W;
        cb.projection[1] = 0.0f;
        cb.projection[2] = 0.0f;
        cb.projection[3] = 0.0f;
        // col1 (cb2[9])
        cb.projection[4] = 0.0f;
        cb.projection[5] = -2.0f / H;
        cb.projection[6] = 0.0f;
        cb.projection[7] = 0.0f;
        // col2 (cb2[10]) — unused by the shader, zero
        cb.projection[8] = 0.0f;
        cb.projection[9] = 0.0f;
        cb.projection[10] = 1.0f;
        cb.projection[11] = 0.0f;
        // col3 (cb2[11]) — translation
        cb.projection[12] = -1.0f - 2.0f * L / W;
        cb.projection[13] = 1.0f + 2.0f * T / H;
        cb.projection[14] = 0.0f;
        cb.projection[15] = 1.0f;
        std::memcpy(p, &cb, sizeof(cb));
        im.device->UnmapBuffer(im.vsCb);
    }

    cmd.BindPipeline(im.pso);
    cmd.BindVertexBuffer(0, im.vb, sizeof(EngineImGuiVert));
    cmd.BindIndexBuffer(im.ib,
                        sizeof(ImDrawIdx) == 2 ? gfx::Format::R16_UINT : gfx::Format::R32_UINT);
    cmd.BindConstantBuffer(gfx::ShaderStage::Vertex, 2, im.vsCb);
    cmd.BindSampler(gfx::ShaderStage::Pixel, 0, im.sampler);

    gfx::Viewport vp;
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = static_cast<f32>(viewportW);
    vp.height = static_cast<f32>(viewportH);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    cmd.SetViewport(vp);

    gfx::TextureHandle lastTex = gfx::TextureHandle::Invalid;

    for (const ListRange& r : ranges) {
        const ImDrawList* list = r.list;
        for (int c = 0; c < list->CmdBuffer.Size; ++c) {
            const ImDrawCmd& dc = list->CmdBuffer[c];
            if (dc.UserCallback) {
                // ImGui callbacks (e.g. ImDrawCallback_ResetRenderState) are
                // not supported — there's no notion of "reset render state"
                // for the gfx layer to honour. Skip silently.
                continue;
            }
            if (dc.ElemCount == 0)
                continue;

            const f32 clipMinX = dc.ClipRect.x - L;
            const f32 clipMinY = dc.ClipRect.y - T;
            const f32 clipMaxX = dc.ClipRect.z - L;
            const f32 clipMaxY = dc.ClipRect.w - T;
            if (clipMaxX <= clipMinX || clipMaxY <= clipMinY)
                continue;

            gfx::Scissor sc;
            sc.x = std::max(0, static_cast<i32>(clipMinX));
            sc.y = std::max(0, static_cast<i32>(clipMinY));
            sc.width = std::min(viewportW, static_cast<i32>(clipMaxX)) - sc.x;
            sc.height = std::min(viewportH, static_cast<i32>(clipMaxY)) - sc.y;
            if (sc.width <= 0 || sc.height <= 0)
                continue;
            cmd.SetScissor(sc);

            const auto tex = static_cast<gfx::TextureHandle>(static_cast<u64>(dc.GetTexID()));
            const gfx::TextureHandle bind =
                (tex != gfx::TextureHandle::Invalid) ? tex : im.fontAtlas;
            if (bind != lastTex) {
                cmd.BindShaderResource(gfx::ShaderStage::Pixel, 0, bind);
                lastTex = bind;
            }

            cmd.DrawIndexed(dc.ElemCount, r.idxBase + dc.IdxOffset,
                            static_cast<i32>(r.vtxBase + dc.VtxOffset));
        }
    }

    // Restore a full-target scissor so any work the pipeline does after
    // RenderFrame's tonemap + imgui block (none today, but hypothetically a
    // future overlay pass) doesn't inherit the last ImGui clip rect.
    gfx::Scissor full;
    full.x = 0;
    full.y = 0;
    full.width = viewportW;
    full.height = viewportH;
    cmd.SetScissor(full);
}

} // namespace whiteout::flakes::renderer::dear_imgui

#endif // WDX_ENABLE_IMGUI
