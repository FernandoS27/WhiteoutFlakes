#include "renderer/profiles/diablo3/d3_standard_shading.h"

#include "core/render_detail.h"
#include "renderer/assets/asset_manager.h"
#include "renderer/scene_manager.h"
#include "whiteout/flakes/content_ref.h"
#include "core/render_profile.h"
#include "core/surface_table.h"
#include "renderer/assets/sampler_asset_manager.h"
#include "renderer/assets/texture_asset_manager.h"
#include "renderer/camera.h"
#include "renderer/debug/draw_trace_hooks.h"
#include "renderer/distortion/distortion_service.h"
#include "renderer/model/render_model.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"

#include "compiled_shaders.h"

#include <cmath>
#include <cstdio>

namespace whiteout::flakes::renderer::profiles::diablo3 {

using model::GPUGeoset;

namespace {

Vector3f Cross(const Vector3f& a, const Vector3f& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Vector3f Normalized(const Vector3f& v, const Vector3f& fallback) {
    const f32 l2 = v.x * v.x + v.y * v.y + v.z * v.z;
    if (l2 < 1e-12f)
        return fallback;
    const f32 inv = 1.0f / std::sqrt(l2);
    return {v.x * inv, v.y * inv, v.z * inv};
}
f32 SrgbToLinear(f32 c) {
    return (c <= 0.04045f) ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

using BF = gfx::BlendFactor;

constexpr gfx::BlendDesc kD3Opaque = {.enable = false};
constexpr gfx::BlendDesc kD3AlphaBlend = {.enable = true,
                                          .srcColor = BF::SrcAlpha,
                                          .dstColor = BF::InvSrcAlpha,
                                          .srcAlpha = BF::One,
                                          .dstAlpha = BF::InvSrcAlpha};

} // namespace

gfx::BlendFactor D3BlendFactor(u32 engine, gfx::BlendFactor fallback, bool alpha) {
    switch (engine) {
    case 1:
        return BF::Zero;
    case 2:
        return BF::One;
    case 3:
        return BF::SrcColor;
    case 4:
        return BF::InvSrcColor;
    case 5:
        return BF::SrcAlpha;
    case 6:
        return BF::InvSrcAlpha;
    case 7:
        return BF::DstColor;
    case 8:
        return BF::InvDstColor;
    case 9:
        return BF::DstAlpha;
    case 10:
        return BF::InvDstAlpha;
    case 11:
        // D3DRS_BLENDFACTOR, which `sub_73D580` sets to 0x00FFFFFF and never
        // changes: white with a zero alpha.
        return alpha ? BF::Zero : BF::One;
    default:
        return fallback;
    }
}

gfx::CompareOp D3CompareOp(u32 func) {
    switch (func) {
    case 1:
        return gfx::CompareOp::Never;
    case 2:
        return gfx::CompareOp::Less;
    case 3:
        return gfx::CompareOp::Equal;
    case 4:
        return gfx::CompareOp::LessEqual;
    case 5:
        return gfx::CompareOp::Greater;
    case 6:
        // D3DCMP_NOTEQUAL, which this gfx layer has no value for. One corpus
        // pass asks for it and Always is the nearest of the seven.
        return gfx::CompareOp::Always;
    case 7:
        return gfx::CompareOp::GreaterEqual;
    default:
        return gfx::CompareOp::Always;
    }
}

namespace {

// The animated UV transform for a slot, or the table's resting one. Same seam
// `.m3` layers use: the palette entry is a 2x4 affine (`u' = row0.x*u +
// row0.y*v + row0.w`), and widening it to a 4x4 loses nothing because the
// shader feeds (u, v, 0, 1) and reads .xy back.
Matrix44f D3SlotUvMatrix(const render_detail::RenderableView& view, const D3Slot& slot) {
    if (slot.uvTransformId < 0 || !view.texAnimPalette ||
        static_cast<usize>(slot.uvTransformId) >= view.texAnimPalette->size())
        return slot.uvTransform;
    const auto& e = (*view.texAnimPalette)[static_cast<usize>(slot.uvTransformId)];
    Matrix44f m = Matrix44f::identity();
    m.data[0][0] = e.row0[0];
    m.data[1][0] = e.row0[1];
    m.data[3][0] = e.row0[3];
    m.data[0][1] = e.row1[0];
    m.data[1][1] = e.row1[1];
    m.data[3][1] = e.row1[3];
    return m;
}

} // namespace

D3StandardShading::~D3StandardShading() {
    // Empty on purpose: every handle belongs to a device RenderPipeline
    // destroys first. ReleaseGpu is the ordered teardown.
}

void D3StandardShading::Init() {
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
#define WDX_D3_BLOB(name)                                                                          \
    k##name, sizeof(k##name), k##name##Spv, sizeof(k##name##Spv), k##name##Wgsl,                   \
        sizeof(k##name##Wgsl), k##name##Mtl, sizeof(k##name##Mtl)
    vs_ = mk(gfx::ShaderStage::Vertex, WDX_D3_BLOB(D3StandardVS));
    vsSkinned_ = mk(gfx::ShaderStage::Vertex, WDX_D3_BLOB(D3StandardSkinnedVS));
    ps_ = mk(gfx::ShaderStage::Pixel, WDX_D3_BLOB(D3StandardPS));
    psDebug_ = mk(gfx::ShaderStage::Pixel, WDX_D3_BLOB(D3StandardDebugPS));
#undef WDX_D3_BLOB

    // One map per draw -> the Vulkan CB ring needs room for a busy frame.
    constexpr u32 kCbRingSlots = 4096;
    drawCb_ = gfxDev->CreateBuffer({
        .size = sizeof(D3DrawCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kCbRingSlots,
    });
    debugCb_ = gfxDev->CreateBuffer({
        .size = sizeof(core::DebugViewCbData),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kCbRingSlots,
    });
    // Written once per pass; no ring.
    passCb_ = gfxDev->CreateBuffer({
        .size = sizeof(D3PassCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });
}

void D3StandardShading::ReleaseGpu() {
    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return;
    for (auto& [key, pso] : psos_) {
        if (pso != gfx::PipelineHandle::Invalid)
            gfxDev->Destroy(pso);
    }
    psos_.clear();
    if (drawCb_ != gfx::BufferHandle::Invalid)
        gfxDev->Destroy(drawCb_);
    if (passCb_ != gfx::BufferHandle::Invalid)
        gfxDev->Destroy(passCb_);
    if (debugCb_ != gfx::BufferHandle::Invalid)
        gfxDev->Destroy(debugCb_);
    debugCb_ = gfx::BufferHandle::Invalid;
    psDebug_ = gfx::ShaderHandle::Invalid;
    drawCb_ = gfx::BufferHandle::Invalid;
    passCb_ = gfx::BufferHandle::Invalid;
    vs_ = gfx::ShaderHandle::Invalid;
    vsSkinned_ = gfx::ShaderHandle::Invalid;
    ps_ = gfx::ShaderHandle::Invalid;
    initTried_ = false;
}

bool D3StandardShading::IsAvailable() const {
    return vs_ != gfx::ShaderHandle::Invalid && ps_ != gfx::ShaderHandle::Invalid &&
           drawCb_ != gfx::BufferHandle::Invalid && passCb_ != gfx::BufferHandle::Invalid;
}

const D3SurfaceTable* D3StandardShading::TableOf(const render_detail::RenderableView& view) {
    return core::SurfaceTableCast<D3SurfaceTable>(view.surfaceTable);
}

gfx::PipelineHandle D3StandardShading::GetOrBuildPso(const PsoKey& key) {
    if (auto it = psos_.find(key); it != psos_.end())
        return it->second;

    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return gfx::PipelineHandle::Invalid;

    // Elements by hand rather than VertexLayoutCache::Subset, because the two
    // UV sets have to arrive as ONE float4 TEXCOORD0 — declaring TEXCOORD1
    // trips the slangc TEXCOORD10 semantic trap m2_combiners.slang documents.
    // The adapter lays the two sets down contiguously for exactly that reason.
    //
    // Order is load-bearing: Vulkan, WebGPU and Metal derive shader locations
    // from array position, so this array matches the VS input struct's field
    // order exactly.
    const auto& layouts = rs_.Pipeline().VertexLayouts();
    const auto attrs = layouts.Attributes(key.layoutId);
    auto find = [&](core::VertexSemantic s, u8 idx) -> const core::VertexAttribute* {
        for (const auto& a : attrs)
            if (a.semantic == s && a.semanticIndex == idx)
                return &a;
        return nullptr;
    };
    const auto* pos = find(core::VertexSemantic::Position, 0);
    const auto* nrm = find(core::VertexSemantic::Normal, 0);
    const auto* uv0 = find(core::VertexSemantic::TexCoord, 0);
    const auto* uv1 = find(core::VertexSemantic::TexCoord, 1);
    const auto* tan = find(core::VertexSemantic::Tangent, 0);
    const auto* col = find(core::VertexSemantic::Color, 0);
    if (!pos || !nrm || !uv0 || !uv1 || !tan || !col)
        return gfx::PipelineHandle::Invalid;
    // The merged float4 only works because the two sets are adjacent, which is
    // the adapter's contract — checked rather than assumed, because a silent
    // disagreement here reads as scrambled texture coordinates.
    if (uv0->format != gfx::Format::R32G32_FLOAT || uv1->format != gfx::Format::R32G32_FLOAT ||
        uv1->offset != uv0->offset + 8)
        return gfx::PipelineHandle::Invalid;

    std::vector<gfx::InputElement> elements;
    elements.push_back({"POSITION", 0, pos->format, pos->offset, 0});
    elements.push_back({"NORMAL", 0, nrm->format, nrm->offset, 0});
    elements.push_back({"TEXCOORD", 0, gfx::Format::R32G32B32A32_FLOAT, uv0->offset, 0});
    elements.push_back({"TANGENT", 0, tan->format, tan->offset, 0});
    elements.push_back({"COLOR", 0, col->format, col->offset, 0});
    if (key.skinned) {
        // Slot 1 is the bone stream — a `BoneVertex`, the same 8-byte record
        // every skinned WC3 geoset uploads, so the two share PackBoneVertex and
        // the palette CB layout.
        elements.push_back(
            {"BLENDWEIGHT", 0, gfx::Format::R8G8B8A8_UNORM, offsetof(BoneVertex, weights), 1});
        elements.push_back(
            {"BLENDINDICES", 0, gfx::Format::R8G8B8A8_UINT, offsetof(BoneVertex, indices), 1});
    }

    gfx::GraphicsPipelineDesc desc{};
    desc.vs = key.skinned ? vsSkinned_ : vs_;
    desc.ps = key.debug ? psDebug_ : ps_;
    desc.inputLayout = std::span<const gfx::InputElement>(elements);
    // The declared elements stop short of the record's end, so the true stride
    // has to be stated or the backends that bake it into the PSO walk the
    // buffer wrong.
    desc.inputSlotStrides[0] = key.stride;
    desc.inputSlotStrides[1] = sizeof(BoneVertex);
    desc.topology = gfx::PrimitiveTopology::TriangleList;
    if (key.blend != 0) {
        desc.blend = kD3AlphaBlend;
        desc.blend.srcColor = D3BlendFactor(key.blendSrc, BF::SrcAlpha);
        desc.blend.dstColor = D3BlendFactor(key.blendDst, BF::InvSrcAlpha);
        // Same enum both channels, but factor 11 is the constant 0x00FFFFFF,
        // whose alpha is zero where its colour is one — so the pair has to be
        // resolved per channel or the eleven `_pma` surface shaders composite
        // against their own alpha and go black.
        desc.blend.srcAlpha = D3BlendFactor(key.blendSrc, BF::One, true);
        desc.blend.dstAlpha = D3BlendFactor(key.blendDst, BF::InvSrcAlpha, true);
    } else {
        desc.blend = kD3Opaque;
    }
    // Two flags, not one: 111 shipped passes write the alpha alone and 73
    // write neither channel. See D3PassState::colorWrite.
    desc.blend.colorWrite = key.colorWrite;
    desc.blend.alphaWrite = key.alphaWrite;
    // `sub_73DA60` sets ZFUNC and then ZENABLE = (func != Always), so a pass
    // asking for Always is asking for the test to be off.
    desc.depthStencil.depthTest = key.depthFunc != 8;
    desc.depthStencil.depthWrite = key.depthWrite;
    desc.depthStencil.depthCompare = D3CompareOp(key.depthFunc);
    // Cull mode is a *pass* property in the original, and the surface now reads
    // it from there: 713 of the corpus's 1,831 passes ask for none.
    desc.rasterizer.cull = key.twoSided ? gfx::CullMode::None : gfx::CullMode::Back;
    desc.rasterizer.fill =
        key.fillMode == 1 ? gfx::FillMode::Wireframe : gfx::FillMode::Solid;
    desc.rasterizer.frontCCW = true;
    desc.rtvFormat = key.rtv;
    desc.dsvFormat = key.dsv;
    desc.extraRtvFormats[0] = key.extra0;
    desc.extraRtvFormats[1] = key.extra1;
    desc.extraRtvFormats[2] = key.extra2;
    desc.extraRtvCount = key.extraRtvCount;
    // This shader writes SV_Target0 alone. The attachment count is declared
    // only so a pass that binds more is not rejected; the cleared extra targets
    // must be left alone.
    desc.extraColorWrite = false;

    const auto pso = gfxDev->CreateGraphicsPipeline(desc);
    psos_.emplace(key, pso);
    return pso;
}

bool D3StandardShading::ResolveSkinned(const render_detail::RenderableView& view,
                                       const GPUGeoset& geo,
                                       gfx::BufferHandle& outPalette) const {
    outPalette = gfx::BufferHandle::Invalid;
    // A deformed geoset arrives already posed — the cloth solver put every
    // vertex where it belongs in model space — so skinning it would apply the
    // bones a second time. This is the whole reason the client draws its
    // rebuilt stream unskinned.
    if (geo.deformActive)
        return false;
    if (!geo.hasSkinning || geo.boneVb == gfx::BufferHandle::Invalid)
        return false;
    if (view.skinning && view.skinning->UsesPerActorPalette())
        outPalette = view.skinning->ActorPaletteCb();
    else
        outPalette = geo.bonePaletteCb;
    return outPalette != gfx::BufferHandle::Invalid;
}

bool D3StandardShading::BeginPass(const core::PassContext& ctx,
                                  const render_detail::CollectedDrawLists& lists) {
    (void)lists; // the rig is pass-global; D3 has no per-model light records
    Init();
    if (!IsAvailable())
        return false;
    passView_ = ctx.view;
    passProj_ = ctx.projection;
    passCameraPos_ = ctx.cameraPos;
    passDebug_ = ctx.debug;
    passDebugTarget_ = ctx.debugTarget;

    auto* gfxDev = rs_.Pipeline().Gfx();
    if (auto* c = static_cast<D3PassCb*>(gfxDev->MapBuffer(passCb_))) {
        c->view = passView_.transpose();
        c->projection = passProj_.transpose();
        c->cameraPosWS = {passCameraPos_.x, passCameraPos_.y, passCameraPos_.z, 0.0f};

        // Authored in display space; de-gamma'd only when the profile shades
        // linearly, which Diablo3Profile does not. Constants picked by eye are
        // display-referred and stay that way unless something downstream
        // reinterprets them.
        const bool linear = ctx.profile && ctx.profile->LinearShading();
        auto lin = [linear](f32 r, f32 g, f32 b) -> Vector4f {
            if (!linear)
                return {r, g, b, 0.0f};
            return {SrgbToLinear(r), SrgbToLinear(g), SrgbToLinear(b), 0.0f};
        };

        // ---- The rig ---------------------------------------------------------
        //
        // A VIEWER rig, stated as one: a scene's brightness in Diablo III comes
        // from the level's Light SNOs, and a model viewer has no level. What the
        // RE settles is the SHAPE — `colAmbient` plus lights of five types, each
        // carrying its own ambient as well as a diffuse — and the shape is what
        // this fills in. The numbers are picked; the arithmetic consuming them
        // is the original's. They sum past 1.0 on a camera-facing fragment by
        // design — the Diablo3Profile renders into an HDR target and tonemaps
        // (D3's frame buffer is HDR), so a bright albedo rolls off rather than
        // clipping. A gamma-LDR scene target would blow every light material
        // (skin, white cloth) to white; that is what the tonemap prevents.
        // The magnitudes are tuned for the tonemapped result — scaled to ~0.56
        // of a first cut that read hot post-tonemap — so a naked and a
        // fully-plated model both sit in range.
        //
        // Two lights, which is the shipped budget: one directional (stored the
        // way the engine stores a demoted one, `.w` 0 with attenuation (1,0,0))
        // and one cylindrical, D3's hero light.
        const Vector3f eye = passCameraPos_;
        const auto& cam = rs_.Pipeline().FrameCamera();
        const Vector3f at = cam.GetTarget();
        const Vector3f fwd =
            Normalized({at.x - eye.x, at.y - eye.y, at.z - eye.z}, {0.0f, 1.0f, 0.0f});
        const Vector3f right = Normalized(Cross(fwd, {0.0f, 0.0f, 1.0f}), {1.0f, 0.0f, 0.0f});
        const Vector3f up = Cross(right, fwd);

        for (u32 i = 0; i < kLights; ++i) {
            c->lightPos[i] = {0.0f, 0.0f, 1.0f, 0.0f};
            c->lightAtten[i] = {1.0f, 0.0f, 0.0f, 0.0f}; // .w 0 = dead
            c->lightAmbient[i] = {0.0f, 0.0f, 0.0f, 0.0f};
            c->lightDiffuse[i] = {0.0f, 0.0f, 0.0f, 0.0f};
            c->lightSpecular[i] = {0.0f, 0.0f, 0.0f, 0.0f};
        }

        // Slot 0: a three-quarter key placed relative to the camera —
        // UnlitShading's rule and for its reason: the model is a thing being
        // *inspected*, so the side facing the viewer has to be legible and a
        // fixed world direction leaves it black from half the orbit.
        // Deterministic: the gate poses the camera identically every run.
        //
        // Stored as a DIRECTION TOWARD THE LIGHT, because that is what the
        // engine writes into the point array when a directional overflows —
        // `Render_UploadLightConstants` negates it there — and the shader's
        // `L = pos.xyz - P * pos.w` reads it back directly.
        const Vector3f key = Normalized({-(fwd.x + 0.45f * right.x - 0.35f * up.x),
                                         -(fwd.y + 0.45f * right.y - 0.35f * up.y),
                                         -(fwd.z + 0.45f * right.z - 0.35f * up.z)},
                                        {0.0f, -1.0f, 0.0f});
        c->lightPos[0] = {key.x, key.y, key.z, 0.0f};
        c->lightAtten[0] = {1.0f, 0.0f, 0.0f, 1.0f};
        c->lightDiffuse[0] = lin(0.49f, 0.48f, 0.46f);
        c->lightSpecular[0] = lin(0.31f, 0.31f, 0.31f);

        // Slot 1 is the hero light's counterpart: a dim fill from behind and
        // below, which is what the engine's *summed* `colAmbient` does for a
        // real level and what a single constant cannot. Directional too.
        const Vector3f fill = Normalized({-key.x - 0.2f * up.x, -key.y - 0.2f * up.y,
                                          -key.z - 0.2f * up.z},
                                         {0.0f, 1.0f, 0.0f});
        c->lightPos[1] = {fill.x, fill.y, fill.z, 0.0f};
        c->lightAtten[1] = {1.0f, 0.0f, 0.0f, 1.0f};
        c->lightDiffuse[1] = lin(0.17f, 0.18f, 0.21f);

        // The cylindrical light: a vertical tube standing on the model, lighting
        // it from above and falling off linearly with distance from the axis.
        // Sized off the camera's framing distance, which is the only measure of
        // the model this seam has and which the viewer sets from its bounds.
        const f32 span = (std::max)(cam.GetDistance(), 1.0f);
        const f32 radius = span * 0.55f;
        c->cylPos = {at.x, at.y, at.z + span * 0.9f, 1.0f};
        c->cylAxis = {0.0f, 0.0f, 1.0f, 0.0f};
        // {1 / (end - start), end} — the engine computes exactly this
        // reciprocal, clamped, and the falloff is (end - perp) * that.
        c->cylRange = {1.0f / (std::max)(radius - radius * 0.25f, 0.001f), radius, 0.0f, 0.0f};
        c->cylAmbient = lin(0.11f, 0.11f, 0.13f);
        c->cylDiffuse = lin(0.29f, 0.29f, 0.27f);

        // The scene ambient. In the engine this is a sum, not a setting — a
        // base plus every directional light's own ambient — so it is legitimate
        // for it to be well above any single light's ambient.
        c->colAmbient = lin(0.28f, 0.28f, 0.30f);
        // The engine's `SpecularPower` is a GLOBAL (constant 39), which is what
        // makes `flShininess` being 0.0 on 99.4% of shipped materials harmless.
        // Its own default is 1.0 — a hemisphere-wide highlight — and the real
        // value arrives per render record from data we do not read, so this is
        // a viewer number: tight enough to read as a highlight rather than as a
        // second diffuse term.
        c->specularPower = {24.0f, 0.0f, 0.0f, 0.0f};
        gfxDev->UnmapBuffer(passCb_);
    }
    return true;
}

gfx::TextureHandle D3StandardShading::DyeRampTexture() {
    if (rampSlot_ == assets::AssetManager::kInvalidSlot) {
        // The core-asset registry resolves `dye_ramp` by NAME in the Textures
        // group; a CASC storage answers the path with a file id (the SNO), a
        // plain content tree serves the corpus snapshot's file directly.
        ContentRef ref = ContentRef::FromPath("Textures/dye_ramp.tex");
        if (auto* provider = rs_.Scene().ActiveContentProvider()) {
            if (const u32 id = provider->FileIdForPath("Base/Textures/dye_ramp.tex"))
                ref = ContentRef::FromFileId(id);
        }
        rampSlot_ = rs_.Assets().Acquire(assets::AssetKind::Texture, assets::kSoleSubKind, ref);
    }
    const gfx::TextureHandle tex = rs_.Assets().TextureOf(rampSlot_);
    return tex != gfx::TextureHandle::Invalid ? tex : rs_.Textures().GetDefaults().White;
}

void D3StandardShading::Draw(const render_detail::DrawItem& item, const core::PassContext& ctx) {
    if (!item.view || item.geoIdx < 0 || !item.view->geosets)
        return;
    const auto& geosets = *item.view->geosets;
    if (static_cast<usize>(item.geoIdx) >= geosets.size())
        return;
    const GPUGeoset& geo = geosets[static_cast<usize>(item.geoIdx)];

    const D3SurfaceTable* table = TableOf(*item.view);
    const D3Surface* surf = table ? table->Surface(item.key.surface) : nullptr;
    if (!surf || !surf->valid)
        return;

    // The distortion pass draws the SAME geometry through the SAME programs —
    // only the surface and the target change. `D3Surface::distortion` is the
    // phase-3 pass resolved as a surface of its own, and from here down nothing
    // knows the difference.
    const bool distortionPass = ctx.pass == core::PassSlot::Distortion;
    if (distortionPass) {
        surf = surf->distortion.get();
        if (!surf || !surf->valid)
            return;
    }

    auto* gfxDev = rs_.Pipeline().Gfx();
    auto* cmd = gfxDev ? gfxDev->GetImmediateContext() : nullptr;
    if (!cmd)
        return;

    const f32 elementAlpha = geo.geosetAlpha * item.view->parentVisibility;

    PsoKey key;
    key.rtv = distortionPass ? distortion::DistortionService::kBufferFormat
                             : rs_.Pipeline().SceneTargetFormat();
    key.dsv = rs_.Pipeline().DepthStencilFormat();
    gfx::Format extra[3] = {gfx::Format::Unknown, gfx::Format::Unknown, gfx::Format::Unknown};
    // One target. The distortion buffer is a side buffer, not the scene, and a
    // G-buffer sidecar bound beside it would be written with an offset vector.
    key.extraRtvCount = distortionPass ? 0 : rs_.Pipeline().SceneExtraRtvFormats(extra);
    key.extra0 = extra[0];
    key.extra1 = extra[1];
    key.extra2 = extra[2];
    key.layoutId = geo.layoutId;
    key.stride = geo.baseStride;
    key.blend = (surf->alphaBlend || elementAlpha < 1.0f) ? 1u : 0u;
    key.blendSrc = surf->pass.resolved ? surf->pass.blendSrc : 5u;
    key.blendDst = surf->pass.resolved ? surf->pass.blendDst : 6u;
    // A pass that blends and still writes depth is real (436 of 1,412), so this
    // is the pass's own answer where there is one. Without a pass, the old rule
    // stands: blended geometry does not write depth.
    key.depthWrite = surf->pass.resolved ? surf->pass.depthWrite : (key.blend == 0);
    key.depthFunc = surf->pass.resolved ? surf->pass.depthFunc : 4u;
    key.colorWrite = !surf->pass.resolved || surf->pass.colorWrite;
    key.alphaWrite = !surf->pass.resolved || surf->pass.alphaWrite;
    key.fillMode = surf->pass.resolved ? surf->pass.fillMode : 0u;
    key.twoSided = surf->twoSided;
    gfx::BufferHandle paletteCb = gfx::BufferHandle::Invalid;
    key.skinned = ResolveSkinned(*item.view, geo, paletteCb);
    // A debug view swaps the pixel entry, and shows its channel even on a pass
    // that writes alpha alone or nothing — otherwise those read as holes.
    key.debug = passDebug_.debugSurfaces && !distortionPass &&
                psDebug_ != gfx::ShaderHandle::Invalid;
    if (key.debug) {
        key.colorWrite = true;
        key.alphaWrite = true;
    }
    const gfx::PipelineHandle pso = GetOrBuildPso(key);
    if (pso == gfx::PipelineHandle::Invalid)
        return;

    if (auto* c = static_cast<D3DrawCb*>(gfxDev->MapBuffer(drawCb_))) {
        c->world = item.view->worldTransform.transpose();
        // .w: how the bound pass consumes the vertex colour and its light.
        // Bit 0 adds the attribute's RGB into the light sum at twice unit
        // weight, bit 1 takes its alpha, bit 2 says the pass is UNLIT and the
        // attribute IS the light. All stay clear when no pass resolved — the
        // attribute means nothing on its own, and the wrong guess either blacks
        // out a prop or blows out a character.
        const u32 vcMode = (surf->pass.vertexColorLights ? 0x1u : 0u) |
                           (surf->pass.vertexAlpha ? 0x2u : 0u) | (surf->unlit ? 0x4u : 0u);
        c->params0 = {surf->alphaTestThreshold, surf->shininess, surf->twoSided ? 1.0f : 0.0f,
                      static_cast<f32>(vcMode)};
        c->params1 = {surf->pass.colorGain, surf->pass.alphaGain,
                      static_cast<f32>(surf->pass.pmaMode),
                      static_cast<f32>(surf->alphaTestFunc)};
        // .y: the `ps_distortion2tex` re-centring, which is a bias on the whole
        // chain rather than a stage of it. See D3PassState::distortionTwoTex.
        // The dye term: the row is the engine's own maths (dye 2..22 onto 21
        // row centres), the enable doubles as the mask-slot gate in the PS.
        const i32 dye = geo.dye;
        const f32 dyeV = (dye >= 2 && dye <= 22)
                             ? (static_cast<f32>(dye - 2) + 0.5f) / 21.0f
                             : 0.0f;
        c->params2 = {surf->pass.resolved ? surf->pass.depthBias : 0.0f,
                      surf->pass.distortionTwoTex ? 1.0f : 0.0f, dyeV,
                      (dye >= 2 && dye <= 22) ? 1.0f : 0.0f};
        // Where the vertex colour and the texture factor enter each channel of
        // a `Legacy.fx` chain. Mirrors d3_standard.slang's kD3Chain* bits.
        const u32 chainBits =
            (surf->pass.colorVcolFirst ? 0x01u : 0u) | (surf->pass.colorVcolLast ? 0x02u : 0u) |
            (surf->pass.alphaVcolFirst ? 0x04u : 0u) | (surf->pass.alphaVcolLast ? 0x08u : 0u) |
            (surf->pass.colorFactorFirst ? 0x10u : 0u) |
            (surf->pass.colorFactorLast ? 0x20u : 0u) |
            (surf->pass.alphaFactorFirst ? 0x40u : 0u) | (surf->pass.alphaFactorLast ? 0x80u : 0u);
        // `2 * edgealphaParams.x`, and `edgealphaParams.x` is
        // `appearanceFX.x * surfaceTag(0x30100)` — both 1.0 unless a Surface
        // asset animates them, which nothing this renderer opens does.
        c->params3 = {static_cast<f32>(surf->chainCount), static_cast<f32>(chainBits),
                      static_cast<f32>(surf->pass.edgeAlpha), 2.0f};
        // The draw's colour. No tint here, so the live half is the fade — which
        // is where the original puts it too: the chain has no material alpha to
        // ride on, and `vs_legacy` closes its edge term on `Factor.w`.
        c->factor = {1.0f, 1.0f, 1.0f, elementAlpha};
        c->matDiffuse = surf->diffuse;
        // The element alpha rides the material's own, which is what the
        // fixed-function pipeline does with it and what makes a fade a fade.
        c->matDiffuse.w *= elementAlpha;
        c->matSpecular = surf->specular;
        c->matEmissive = surf->emissive;
        c->matAmbient = surf->ambient;
        for (u32 i = 0; i < kSlotCount; ++i) {
            // A chain surface puts its stages in the first six slots and leaves
            // the rest empty; the shader reads them through the same
            // `slotUv`/`slotCtl` pair either way, so only the source differs.
            const D3Slot& s = (i < surf->chainCount) ? surf->chain[i] : surf->slots[i];
            const bool chained = i < surf->chainCount;
            c->slotUv[i] = D3SlotUvMatrix(*item.view, s).transpose();
            const bool resolved = (chained || surf->chainCount == 0) && s.textureId >= 0 &&
                                  item.view->textures &&
                                  item.view->textures->Get(s.textureId) !=
                                      gfx::TextureHandle::Invalid;
            c->slotCtl[i][0] =
                s.uvSource | ((s.wrapFlags & assets::kSamplerWrapBitsMask) << 4);
            c->slotCtl[i][1] = chained ? (s.colorOp | (static_cast<u32>(s.alphaOp) << 4)) : 0u;
            c->slotGain[i] = {chained ? s.colorGain : 1.0f, chained ? s.alphaGain : 1.0f,
                              (chained && s.colorClamp) ? 1.0f : 0.0f,
                              (chained && s.alphaClamp) ? 1.0f : 0.0f};
            // A slot whose texture has not landed yet stays *off* rather than
            // sampling the white default. For the normal slot that is the
            // difference between a neutral surface and every pixel facing the
            // viewer; for the others it is the type's documented default, which
            // is what "off" means in the shader. Flips on by itself the frame
            // the texture arrives.
            c->slotCtl[i][2] = resolved ? 1u : 0u;
            // Which channels of the sample the surface takes, where the pass
            // said so. Zero is "it did not", and each slot keeps the default
            // its type implies.
            c->slotCtl[i][3] = s.channels;
        }
        gfxDev->UnmapBuffer(drawCb_);
    }

    cmd->BindPipeline(pso);
    cmd->BindVertexBuffer(0, geo.Stream(core::StreamId::Base), geo.baseStride);
    if (key.skinned) {
        cmd->BindVertexBuffer(1, geo.boneVb, sizeof(BoneVertex));
        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 2, paletteCb);
    }
    cmd->BindIndexBuffer(geo.ib, gfx::Format::R32_UINT);
    // Both CBs in both stages — Vulkan and WebGPU build one descriptor set per
    // stage from the shared layout, and a declared binding left unbound is a
    // validation error rather than a harmless omission.
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, passCb_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, passCb_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 1, drawCb_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1, drawCb_);
    if (key.debug) {
        if (auto* c = static_cast<core::DebugViewCbData*>(gfxDev->MapBuffer(debugCb_))) {
            *c = core::MakeDebugViewCb(passDebug_, passDebugTarget_, 0, 0, 5.0f);
            gfxDev->UnmapBuffer(debugCb_);
        }
        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 3, debugCb_);
        cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 3, debugCb_);
    }

    // Every slot, every draw — which of them survive into the compiled PS is
    // Slang's dead-code decision, and binding fewer than it kept is a
    // descriptor the runtime never writes (m2_shading.cpp's lesson).
    const auto& defaults = rs_.Textures().GetDefaults();
    for (u32 u = 0; u < kSlotCount; ++u) {
        const D3Slot& s = (u < surf->chainCount) ? surf->chain[u] : surf->slots[u];
        gfx::TextureHandle tex = gfx::TextureHandle::Invalid;
        // A chain leaves the slots past its own stages unbound: sampling a
        // named slot there would be the surface's own diffuse arriving in a
        // register the chain never asked for.
        if (s.textureId >= 0 && item.view->textures &&
            (u < surf->chainCount || surf->chainCount == 0))
            tex = item.view->textures->Get(s.textureId);
        if (tex == gfx::TextureHandle::Invalid)
            tex = defaults.White;
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, u, tex);
    }
    cmd->BindShaderResource(gfx::ShaderStage::Pixel, kDyeRampRegister, DyeRampTexture());
    for (u32 w = 0; w <= assets::kSamplerWrapBitsMask; ++w)
        cmd->BindSampler(gfx::ShaderStage::Pixel, w, rs_.Samplers().WrapVariant(w));

    // G1 hook, after the PSO resolved and the CB was written.
    if (debug::DrawTraceEnabled()) {
        debug::TraceDraw d;
        d.shadingModel = static_cast<u8>(debug::TraceShadingModel::D3Standard);
        d.blendClass = static_cast<u8>(item.key.blend);
        d.streamMask = debug::kStreamBase;
        d.surface = static_cast<i32>(item.key.surface);
        d.matFlags = static_cast<i32>(surf->materialFlags);
        d.filterMode = static_cast<i32>(key.blend);
        for (u32 u = 0; u < kSlotCount && u < static_cast<u32>(debug::kTraceTexSlots); ++u)
            d.texIds[u] = (u < surf->chainCount) ? surf->chain[u].textureId
                          : (surf->chainCount == 0) ? surf->slots[u].textureId
                                                    : -1;
        d.psoKey = debug::TracePsoKey({
            .psPermute = static_cast<u32>(key.blend) | (key.skinned ? 0x200u : 0u) |
                         (key.twoSided ? 0x400u : 0u),
            .vertexLayout = key.layoutId,
            .extraRtvCount = key.extraRtvCount,
        });
        d.palettePath = key.skinned ? 2 : 0;
        d.paletteSlots = (key.skinned && item.view->skinning)
                             ? item.view->skinning->GeosetPaletteSize(geo.geosetId)
                             : 0;
        debug::RecordUnlitDraw(d, *item.view, geo, ctx);
    }

    cmd->DrawIndexed(geo.indexCount);
}

core::SurfaceClass D3StandardShading::Classify(const render_detail::RenderableView& view,
                                               const GPUGeoset& geo) const {
    (void)view;
    (void)geo;
    // Only reached for a geoset with no surface range (table build failed).
    return {.visible = true, .blend = core::BlendClass::Opaque, .needsDepthFill = false};
}

core::SurfaceClass D3StandardShading::ClassifySurface(const render_detail::RenderableView& view,
                                                      const GPUGeoset& geo, u32 surface) const {
    const D3SurfaceTable* table = TableOf(view);
    const D3Surface* s = table ? table->Surface(surface) : nullptr;
    if (!s)
        return {.visible = false};

    core::SurfaceClass c = D3ClassifySurface(*s);

    // No early-out on `!c.visible`, and that is the whole reason this reads the
    // way it does: the gates below are SUB-OBJECT level in the engine
    // (`ActorModel_EmitSubObjectDrawCalls` skips the sub-object, not one pass
    // of it), so they own the distortion half too — and a phase-3-only surface
    // is never visible, which is exactly the surface whose only draw is that
    // half.

    // §6.3: the engine tests `alpha < 1.0` and swaps the whole shader —
    // `rec[24]` opaque, `rec[28]` translucent — and **skips the sub-object
    // entirely** when the translucent id is -1. We cannot swap programs we do
    // not have, so the fade lands on the blend class instead; the half that
    // does survive is the skip, which is what `noTranslucentVariant` carries.
    const f32 alpha = geo.geosetAlpha * view.parentVisibility;
    if (alpha < 1.0f) {
        if (s->noTranslucentVariant) {
            c.visible = false;
            c.needsDistortion = false;
            return c;
        }
        if (c.visible)
            c.blend = core::BlendClass::Transparent;
    }
    if (alpha <= 0.0f) {
        // A fully faded sub-object distorts nothing either: `Factor.w` is that
        // same alpha, and it multiplies the distortion pass's edge term.
        c.visible = false;
        c.needsDistortion = false;
    }
    return c;
}

core::VertexNeeds D3StandardShading::Needs(u32 surface) const {
    (void)surface;
    // boneWeights unconditionally, even though 91% of sub-objects are rigid:
    // Needs is per *surface index* and the loader asks it before the table is
    // built for every geoset, so a per-surface answer here would be read
    // against an empty table. A rigid sub-object simply has no weights to
    // stage, which the loader already handles by not building the stream.
    return core::VertexNeeds{.position = true,
                             .normal = true,
                             .tangent = true,
                             .uvSets = 2,
                             .boneWeights = true};
}

i32 D3StandardShading::SelectLights(bls::FrameInputs& frame, const bls::LightingContext& lighting,
                                    const Matrix44f& viewMat, const Vector3f& surfaceWS) const {
    (void)frame;
    (void)lighting;
    (void)viewMat;
    (void)surfaceWS;
    // The rig is pass-global (BeginPass writes it); nothing reaches pixels
    // through the WC3 light palette.
    return 0;
}

core::EmitMask D3StandardShading::Emits(u32 surface, core::PassSlot pass) const {
    (void)surface;
    if (pass == core::PassSlot::ShadowMap || pass == core::PassSlot::DepthPrepass)
        return core::EmitMask::Depth;
    // No G-buffer, no MRT: a gamma-LDR frame has no deferred pass to feed.
    return core::EmitMask::DefaultColor;
}

} // namespace whiteout::flakes::renderer::profiles::diablo3
