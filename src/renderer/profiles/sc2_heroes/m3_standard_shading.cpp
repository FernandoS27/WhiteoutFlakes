#include "m3_standard_shading.h"

#include "core/render_detail.h"
#include "core/render_profile.h"
#include "core/surface_table.h"
#include "m3_surface_table.h"
#include "renderer/assets/sampler_asset_manager.h"
#include "renderer/assets/texture_asset_manager.h"
#include "renderer/bls/bls_frame.h"
#include "renderer/camera.h"
#include "renderer/debug/draw_trace_hooks.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/render_model.h"
#include "renderer/render_pipeline.h"
#include "renderer/render_service.h"
#include "renderer/types.h"

#include "compiled_shaders.h"

#include <cmath>

namespace whiteout::flakes::renderer::profiles::sc2_heroes {

using model::GPUGeoset;
using ::whiteout::m3::BlendMode;
using ::whiteout::m3::MaterialFlag;

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

// m3::BlendMode → blend state (M3_SIMPLE_MATERIAL_DESIGN.md §3). Separate
// alpha factors for the same reason M2's table keeps them: an additive draw
// must not erase the destination alpha a later blend reads.
constexpr gfx::BlendDesc kM3BlendTable[] = {
    // Opaque
    {.enable = false},
    // AlphaBlend
    {.enable = true,
     .srcColor = BF::SrcAlpha,
     .dstColor = BF::InvSrcAlpha,
     .srcAlpha = BF::One,
     .dstAlpha = BF::InvSrcAlpha},
    // Add — colour already carries its intensity.
    {.enable = true,
     .srcColor = BF::One,
     .dstColor = BF::One,
     .srcAlpha = BF::Zero,
     .dstAlpha = BF::One},
    // AlphaAdd
    {.enable = true,
     .srcColor = BF::SrcAlpha,
     .dstColor = BF::One,
     .srcAlpha = BF::Zero,
     .dstAlpha = BF::One},
    // Mod
    {.enable = true,
     .srcColor = BF::DstColor,
     .dstColor = BF::Zero,
     .srcAlpha = BF::DstAlpha,
     .dstAlpha = BF::Zero},
    // Mod2x
    {.enable = true,
     .srcColor = BF::DstColor,
     .dstColor = BF::SrcColor,
     .srcAlpha = BF::DstAlpha,
     .dstAlpha = BF::SrcAlpha},
};

gfx::BlendDesc M3BlendDesc(u8 mode) {
    return mode < std::size(kM3BlendTable) ? kM3BlendTable[mode] : kM3BlendTable[0];
}

bool M3BlendWritesDepth(u8 mode) {
    return static_cast<BlendMode>(mode) == BlendMode::Opaque;
}

// ---- The recovered SC2 light rigs ------------------------------------------
//
// mods/core.sc2mod/base.sc2data/gamedata/lightdata.xml. InGame is CLight
// id="DefaultLight" — the LightGroup:Tilesets fallback a melee map lights
// with; Glue is CLight id="GlueBackground" — the menu / portrait rig. The
// engine layout (lighting.fx): key directional with its own specular colour,
// two diffuse-only fills, flat ambient. Colours are display-referred editor
// picks; the multipliers are the XML ColorMultiplier / SpecColorMultiplier
// and apply after the de-gamma. Directions are the direction the light
// TRAVELS, normalised in the data, z-up world.
struct Sc2LightRig {
    Vector3f ambient;
    Vector3f keyDir;
    Vector3f keyColor;
    f32 keyMul;
    Vector3f keySpecColor;
    f32 keySpecMul;
    Vector3f fillDir;
    Vector3f fillColor;
    f32 fillMul;
    Vector3f backDir;
    Vector3f backColor;
    f32 backMul;
    f32 hdrSpecMul; ///< Rig HDRSpecMultiplier, folded into the key spec.
    f32 hdrEmisMul; ///< Rig HDREmisMultiplier, scales additive emissive.
};

constexpr Sc2LightRig kSc2DefaultLight = {
    .ambient = {0.454902f, 0.509804f, 0.454902f},
    .keyDir = {0.412953f, -0.598833f, -0.686199f},
    .keyColor = {1.0f, 0.992157f, 0.901961f},
    .keyMul = 1.02f,
    .keySpecColor = {1.0f, 1.0f, 1.0f},
    .keySpecMul = 1.25f,
    .fillDir = {0.939778f, -0.328921f, -0.092892f},
    .fillColor = {0.403922f, 0.458824f, 0.792157f},
    .fillMul = 0.73f,
    .backDir = {0.910194f, 0.309146f, 0.275637f},
    .backColor = {0.137255f, 0.392157f, 0.694118f},
    .backMul = 1.06f,
    .hdrSpecMul = 1.0f,
    .hdrEmisMul = 1.0f,
};

constexpr Sc2LightRig kSc2GlueBackground = {
    .ambient = {0.082353f, 0.145098f, 0.141176f},
    .keyDir = {0.169115f, -0.974000f, -0.150743f},
    .keyColor = {0.737255f, 0.886275f, 1.0f},
    .keyMul = 1.799f,
    .keySpecColor = {0.654902f, 0.847059f, 1.0f},
    .keySpecMul = 1.928f,
    .fillDir = {-0.226722f, 0.930757f, -0.286859f},
    .fillColor = {1.0f, 0.321569f, 0.301961f},
    .fillMul = 0.275f,
    .backDir = {0.753593f, -0.619669f, -0.219336f},
    .backColor = {0.349020f, 0.486275f, 1.0f},
    .backMul = 1.884f,
    .hdrSpecMul = 2.63f,
    .hdrEmisMul = 0.56f,
};

// ---- The recovered team-colour palette --------------------------------------
//
// gamedata.xml <TeamColors> (mods/core.sc2mod): the sixteen melee slots'
// Diffuse / Emissive pairs — exactly what the engine uploads as
// p_cDiffuseTeamColor / p_cEmissiveTeamColor. Display-referred, like every
// editor-authored colour. tc00 White .. tc15 Pink.
struct Sc2TeamColor {
    Vector3f diffuse;
    Vector3f emissive;
};

constexpr Sc2TeamColor kSc2TeamColors[] = {
    {{1.000000f, 1.000000f, 1.000000f}, {0.764600f, 0.764600f, 0.764600f}}, // White
    {{0.705882f, 0.078431f, 0.117647f}, {0.545098f, 0.145098f, 0.145098f}}, // Red
    {{0.000000f, 0.258700f, 1.000000f}, {0.000000f, 0.258700f, 1.000000f}}, // Blue
    {{0.109800f, 0.654700f, 0.917700f}, {0.066600f, 0.517500f, 0.733300f}}, // Teal
    {{0.301961f, 0.000000f, 0.784314f}, {0.274510f, 0.176471f, 0.627451f}}, // Purple
    {{0.921600f, 0.882300f, 0.161000f}, {0.588100f, 0.588100f, 0.117600f}}, // Yellow
    {{0.996000f, 0.541200f, 0.055000f}, {0.996000f, 0.541200f, 0.055000f}}, // Orange
    {{0.086100f, 0.502000f, 0.000000f}, {0.086100f, 0.502000f, 0.000000f}}, // Green
    {{0.800000f, 0.650980f, 0.988235f}, {0.800000f, 0.650980f, 0.988235f}}, // Light Pink
    {{0.121569f, 0.003922f, 0.788235f}, {0.121569f, 0.003922f, 0.788235f}}, // Violet
    {{0.321569f, 0.329412f, 0.580392f}, {0.078431f, 0.211765f, 0.317647f}}, // Light Grey
    {{0.062700f, 0.384200f, 0.274400f}, {0.062700f, 0.384200f, 0.274400f}}, // Dark Green
    {{0.306000f, 0.164700f, 0.015600f}, {0.306000f, 0.164700f, 0.015600f}}, // Brown
    {{0.588235f, 1.000000f, 0.568627f}, {0.517647f, 1.000000f, 0.309804f}}, // Light Green
    {{0.137255f, 0.137255f, 0.137255f}, {0.058824f, 0.058824f, 0.058824f}}, // Dark Grey
    {{0.898000f, 0.357000f, 0.690100f}, {0.898000f, 0.357000f, 0.690100f}}, // Pink
};

/// The palette pair for an instance's packed RGB (r | g<<8 | b<<16). The host
/// swatch stores one colour, the shader needs the diffuse+emissive pair, so
/// the nearest diffuse wins — an exact palette pick maps exactly, anything
/// else degrades to the closest slot rather than a made-up emissive.
const Sc2TeamColor& ResolveTeamColor(u32 packed) {
    const f32 r = static_cast<f32>(packed & 0xFF) / 255.0f;
    const f32 g = static_cast<f32>((packed >> 8) & 0xFF) / 255.0f;
    const f32 b = static_cast<f32>((packed >> 16) & 0xFF) / 255.0f;
    const Sc2TeamColor* best = &kSc2TeamColors[0];
    f32 bestD = 1e9f;
    for (const auto& c : kSc2TeamColors) {
        const f32 dr = c.diffuse.x - r, dg = c.diffuse.y - g, db = c.diffuse.z - b;
        const f32 d = dr * dr + dg * dg + db * db;
        if (d < bestD) {
            bestD = d;
            best = &c;
        }
    }
    return *best;
}

} // namespace

M3StandardShading::~M3StandardShading() {
    // Empty on purpose: every handle belongs to a device RenderPipeline
    // destroys first. ReleaseGpu is the ordered teardown.
}

void M3StandardShading::Init() {
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
#define WDX_M3_BLOB(name)                                                                          \
    k##name, sizeof(k##name), k##name##Spv, sizeof(k##name##Spv), k##name##Wgsl,                   \
        sizeof(k##name##Wgsl), k##name##Mtl, sizeof(k##name##Mtl)
    vs_ = mk(gfx::ShaderStage::Vertex, WDX_M3_BLOB(M3StandardVS));
    vsSkinned_ = mk(gfx::ShaderStage::Vertex, WDX_M3_BLOB(M3StandardSkinnedVS));
    ps_ = mk(gfx::ShaderStage::Pixel, WDX_M3_BLOB(M3StandardPS));
    psMrt_ = mk(gfx::ShaderStage::Pixel, WDX_M3_BLOB(M3StandardMrtPS));
    vsRibbon_ = mk(gfx::ShaderStage::Vertex, WDX_M3_BLOB(M3RibbonVS));
#undef WDX_M3_BLOB

    // One map per draw → the Vulkan CB ring needs room for a busy frame.
    constexpr u32 kCbRingSlots = 4096;
    drawCb_ = gfxDev->CreateBuffer({
        .size = sizeof(M3DrawCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = kCbRingSlots,
    });
    // Written once per pass; no ring.
    passCb_ = gfxDev->CreateBuffer({
        .size = sizeof(M3PassCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
    });
    // One ribbon draw per emitter per frame — a small ring covers a scene of
    // ribbon actors without churning the buffer.
    ribbonPassCb_ = gfxDev->CreateBuffer({
        .size = sizeof(M3PassCb),
        .usage = gfx::BufferUsage::Constant | gfx::BufferUsage::CpuWritable,
        .ringSlotsHint = 256,
    });
}

void M3StandardShading::ReleaseGpu() {
    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return;
    for (auto& [key, pso] : psos_) {
        if (pso != gfx::PipelineHandle::Invalid)
            gfxDev->Destroy(pso);
    }
    psos_.clear();
    for (auto& [key, pso] : ribbonPsos_) {
        if (pso != gfx::PipelineHandle::Invalid)
            gfxDev->Destroy(pso);
    }
    ribbonPsos_.clear();
    if (drawCb_ != gfx::BufferHandle::Invalid)
        gfxDev->Destroy(drawCb_);
    if (passCb_ != gfx::BufferHandle::Invalid)
        gfxDev->Destroy(passCb_);
    if (ribbonPassCb_ != gfx::BufferHandle::Invalid)
        gfxDev->Destroy(ribbonPassCb_);
    drawCb_ = gfx::BufferHandle::Invalid;
    passCb_ = gfx::BufferHandle::Invalid;
    ribbonPassCb_ = gfx::BufferHandle::Invalid;
    vs_ = gfx::ShaderHandle::Invalid;
    vsSkinned_ = gfx::ShaderHandle::Invalid;
    ps_ = gfx::ShaderHandle::Invalid;
    psMrt_ = gfx::ShaderHandle::Invalid;
    vsRibbon_ = gfx::ShaderHandle::Invalid;
    initTried_ = false;
}

bool M3StandardShading::IsAvailable() const {
    return vs_ != gfx::ShaderHandle::Invalid && ps_ != gfx::ShaderHandle::Invalid &&
           drawCb_ != gfx::BufferHandle::Invalid && passCb_ != gfx::BufferHandle::Invalid;
}

const M3SurfaceTable* M3StandardShading::TableOf(const render_detail::RenderableView& view) {
    return core::SurfaceTableCast<M3SurfaceTable>(view.surfaceTable);
}

gfx::PipelineHandle M3StandardShading::GetOrBuildPso(const PsoKey& key) {
    if (auto it = psos_.find(key); it != psos_.end())
        return it->second;

    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return gfx::PipelineHandle::Invalid;

    // Elements by hand rather than VertexLayoutCache::Subset, because the two
    // UV sets have to arrive as ONE float4 TEXCOORD0 — declaring TEXCOORD1
    // trips the slangc TEXCOORD10 semantic trap m2_combiners.slang documents.
    // The `.m3` UV layers are contiguous 4-byte records, so a 4-channel SNORM
    // element at set 0's offset covers set 1 for free; a one-set model gets a
    // 2-channel element and the shader's .zw defaults to (0, 1).
    //
    // Order is load-bearing: Vulkan, WebGPU and Metal derive shader locations
    // from array position, so each variant's array matches its VS input
    // struct's field order exactly.
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
    const auto* tan = find(core::VertexSemantic::Tangent, 0);
    const auto* uv0 = find(core::VertexSemantic::TexCoord, 0);
    const auto* wgt = find(core::VertexSemantic::BoneWeights, 0);
    const auto* idx = find(core::VertexSemantic::BoneIndices, 0);
    if (!pos || !nrm || !tan)
        return gfx::PipelineHandle::Invalid;
    if (key.skinned && (!wgt || !idx))
        return gfx::PipelineHandle::Invalid;

    const gfx::Format uvFormat = find(core::VertexSemantic::TexCoord, 1)
                                     ? gfx::Format::R16G16B16A16_SNORM
                                     : gfx::Format::R16G16_SNORM;
    // A UV-less model still has to feed TEXCOORD0 or the input layout is
    // rejected; offset 0 reads position bytes nothing samples.
    const u16 uvOffset = uv0 ? uv0->offset : u16{0};

    std::vector<gfx::InputElement> elements;
    elements.push_back({"POSITION", 0, pos->format, pos->offset, 0});
    if (key.skinned) {
        elements.push_back({"BLENDWEIGHT", 0, wgt->format, wgt->offset, 0});
        elements.push_back({"BLENDINDICES", 0, idx->format, idx->offset, 0});
    }
    elements.push_back({"NORMAL", 0, nrm->format, nrm->offset, 0});
    elements.push_back({"TEXCOORD", 0, uvFormat, uvOffset, 0});
    elements.push_back({"TANGENT", 0, tan->format, tan->offset, 0});

    gfx::GraphicsPipelineDesc desc{};
    desc.vs = key.skinned ? vsSkinned_ : vs_;
    desc.ps = key.mrt ? psMrt_ : ps_;
    desc.inputLayout = std::span<const gfx::InputElement>(elements);
    desc.inputSlotStrides[0] = key.stride;
    desc.topology = gfx::PrimitiveTopology::TriangleList;
    desc.blend = M3BlendDesc(key.blend);
    desc.depthStencil.depthTest = true;
    desc.depthStencil.depthWrite = M3BlendWritesDepth(key.blend);
    desc.depthStencil.depthCompare = gfx::CompareOp::LessEqual;
    // Backface culling except where the material asks for both sides, which is
    // what retail does. Safe to tighten now that the winding is measured rather
    // than assumed: SV_IsFrontFace reports the exterior shell as front-facing
    // for 79-99.7% of a model's pixels under frontCCW, the remainder being the
    // genuinely two-sided thin geometry this flag exists for.
    desc.rasterizer.cull = key.twoSided ? gfx::CullMode::None : gfx::CullMode::Back;
    desc.rasterizer.frontCCW = true;
    desc.rtvFormat = key.rtv;
    desc.dsvFormat = key.dsv;
    desc.extraRtvFormats[0] = key.extra0;
    desc.extraRtvFormats[1] = key.extra1;
    desc.extraRtvFormats[2] = key.extra2;
    desc.extraRtvCount = key.extraRtvCount;
    // The MRT entry really writes SV_Target1..3; everything else declares the
    // attachments only to satisfy the pass and must leave the cleared
    // G-buffer data alone.
    desc.extraColorWrite = key.mrt;

    const auto pso = gfxDev->CreateGraphicsPipeline(desc);
    psos_.emplace(key, pso);
    return pso;
}

bool M3StandardShading::ResolveSkinned(const render_detail::RenderableView& view,
                                       const GPUGeoset& geo) const {
    if (geo.bonePaletteCb == gfx::BufferHandle::Invalid)
        return false;
    if (geo.layoutId == core::VertexLayoutCache::kWc3Interleaved)
        return false;
    const auto& layouts = rs_.Pipeline().VertexLayouts();
    if (!layouts.Has(geo.layoutId, core::VertexSemantic::BoneWeights) ||
        !layouts.Has(geo.layoutId, core::VertexSemantic::BoneIndices))
        return false;
    if (view.skinning && view.skinning->UsesPerActorPalette())
        return false;
    return true;
}

bool M3StandardShading::BeginPass(const core::PassContext& ctx,
                                  const render_detail::CollectedDrawLists& lists) {
    (void)lists; // the key light is pass-global; local lights are the deferred pass's job
    Init();
    if (!IsAvailable())
        return false;
    passView_ = ctx.view;
    passProj_ = ctx.projection;
    passCameraPos_ = ctx.cameraPos;
    passSlot_ = ctx.pass;

    auto* gfxDev = rs_.Pipeline().Gfx();
    if (auto* c = static_cast<M3PassCb*>(gfxDev->MapBuffer(passCb_))) {
        WritePassCb(*c, passView_, passProj_, passCameraPos_,
                    !ctx.profile || ctx.profile->LinearShading());
        gfxDev->UnmapBuffer(passCb_);
    }
    return true;
}

void M3StandardShading::WritePassCb(M3PassCb& c, const Matrix44f& view, const Matrix44f& proj,
                                    const Vector3f& camPos, bool linearShading) {
    c = M3PassCb{};
    c.view = view.transpose();
    c.projection = proj.transpose();
    c.cameraPosWS = {camPos.x, camPos.y, camPos.z, 0.0f};

    // Authored in display space, de-gamma'd for this linear profile — the same
    // discipline (and budget) as UnlitShading's constants. The multiplier
    // applies after: the engine multiplies in shader-constant space, and >1
    // intensities must survive the transfer.
    auto lin = [&](f32 r, f32 g, f32 b, f32 mul) -> Vector4f {
        if (linearShading) {
            r = SrgbToLinear(r);
            g = SrgbToLinear(g);
            b = SrgbToLinear(b);
        }
        return {r * mul, g * mul, b * mul, 0.0f};
    };
    c.teamParams = {1.0f, 1.0f, 0.0f, 0.0f};

    const LightingMode mode = rs_.Settings().GetLightingMode();
    if (mode == LightingMode::Dynamic) {
        // The camera-relative three-quarter key UnlitShading builds, for the
        // same reason: a model being inspected must not go black from half the
        // orbit. Deterministic — the gate poses the camera identically every
        // run. Fills off.
        const Vector3f eye = camPos;
        const Vector3f at = rs_.Pipeline().FrameCamera().GetTarget();
        const Vector3f fwd =
            Normalized({at.x - eye.x, at.y - eye.y, at.z - eye.z}, {0.0f, 1.0f, 0.0f});
        const Vector3f right = Normalized(Cross(fwd, {0.0f, 0.0f, 1.0f}), {1.0f, 0.0f, 0.0f});
        const Vector3f up = Cross(right, fwd);
        // keyLightDir is the direction the light TRAVELS.
        const Vector3f l = Normalized({fwd.x + 0.45f * right.x - 0.35f * up.x,
                                       fwd.y + 0.45f * right.y - 0.35f * up.y,
                                       fwd.z + 0.45f * right.z - 0.35f * up.z},
                                      {0.0f, 1.0f, 0.0f});
        c.keyLightDir = {l.x, l.y, l.z, 0.0f};
        c.keyLightDiffuse = lin(0.62f, 0.62f, 0.62f, 1.0f);
        c.keyLightSpecular = lin(0.28f, 0.28f, 0.28f, 1.0f);
        c.ambient = lin(0.22f, 0.22f, 0.26f, 1.0f);
        c.ambient.w = 1.0f;
        c.fillLightDir = {0.0f, 0.0f, -1.0f, 0.0f};
        c.fillLightDiffuse = {0.0f, 0.0f, 0.0f, 0.0f};
        c.backLightDir = {0.0f, 0.0f, -1.0f, 0.0f};
        c.backLightDiffuse = {0.0f, 0.0f, 0.0f, 0.0f};
    } else {
        // The recovered rigs (core lightdata.xml): the game look for InGame,
        // the menu / portrait look for Glue. World-fixed directions, exactly as
        // a unit on a map is lit.
        const Sc2LightRig& rig =
            (mode == LightingMode::Glue) ? kSc2GlueBackground : kSc2DefaultLight;
        c.keyLightDir = {rig.keyDir.x, rig.keyDir.y, rig.keyDir.z, 0.0f};
        c.keyLightDiffuse = lin(rig.keyColor.x, rig.keyColor.y, rig.keyColor.z, rig.keyMul);
        c.keyLightSpecular = lin(rig.keySpecColor.x, rig.keySpecColor.y, rig.keySpecColor.z,
                                 rig.keySpecMul * rig.hdrSpecMul);
        c.ambient = lin(rig.ambient.x, rig.ambient.y, rig.ambient.z, 1.0f);
        c.ambient.w = rig.hdrEmisMul;
        c.fillLightDir = {rig.fillDir.x, rig.fillDir.y, rig.fillDir.z, 0.0f};
        c.fillLightDiffuse = lin(rig.fillColor.x, rig.fillColor.y, rig.fillColor.z, rig.fillMul);
        c.backLightDir = {rig.backDir.x, rig.backDir.y, rig.backDir.z, 0.0f};
        c.backLightDiffuse = lin(rig.backColor.x, rig.backColor.y, rig.backColor.z, rig.backMul);
    }
}

void M3StandardShading::Draw(const render_detail::DrawItem& item, const core::PassContext& ctx) {
    if (!item.view || item.geoIdx < 0 || !item.view->geosets)
        return;
    const auto& geosets = *item.view->geosets;
    if (static_cast<usize>(item.geoIdx) >= geosets.size())
        return;
    const GPUGeoset& geo = geosets[static_cast<usize>(item.geoIdx)];

    const M3SurfaceTable* table = TableOf(*item.view);
    const M3Surface* surf = table ? table->Surface(item.key.surface) : nullptr;
    if (!surf || !surf->valid)
        return;

    auto* gfxDev = rs_.Pipeline().Gfx();
    auto* cmd = gfxDev ? gfxDev->GetImmediateContext() : nullptr;
    if (!cmd)
        return;

    PsoKey key;
    key.rtv = rs_.Pipeline().SceneTargetFormat();
    key.dsv = rs_.Pipeline().DepthStencilFormat();
    gfx::Format extra[3] = {gfx::Format::Unknown, gfx::Format::Unknown, gfx::Format::Unknown};
    key.extraRtvCount = rs_.Pipeline().SceneExtraRtvFormats(extra);
    key.extra0 = extra[0];
    key.extra1 = extra[1];
    key.extra2 = extra[2];
    key.layoutId = geo.layoutId;
    key.stride = geo.baseStride;
    key.blend = static_cast<u8>(surf->blendMode);
    key.skinned = ResolveSkinned(*item.view, geo);
    key.twoSided = (surf->materialFlags & static_cast<u32>(MaterialFlag::TwoSided)) != 0;
    // The sidecar entry whenever the sidecar attachment is bound and the
    // surface is opaque. NOT keyed on ctx.pass: the pipeline dispatches the
    // scene block's opaque bucket as OpaqueColor even under a G-buffer
    // profile, and extraRtvCount == 3 already means "the sc2 scene pass" —
    // no other frame binds the fourth attachment. A transparent draw in the
    // same pass stays on the forward entry and writes SV_Target0 alone.
    key.mrt = key.extraRtvCount == 3 && M3BlendWritesDepth(key.blend);
    const gfx::PipelineHandle pso = GetOrBuildPso(key);
    if (pso == gfx::PipelineHandle::Invalid)
        return;

    const bool unshaded =
        (surf->materialFlags & static_cast<u32>(MaterialFlag::Unshaded)) != 0;
    const bool dblLambert =
        (surf->materialFlags & static_cast<u32>(MaterialFlag::DoubleLambert)) != 0;

    if (auto* c = static_cast<M3DrawCb*>(gfxDev->MapBuffer(drawCb_))) {
        c->world = item.view->worldTransform.transpose();
        c->params0 = {surf->alphaTestThreshold, unshaded ? 1.0f : 0.0f, surf->specularExponent,
                      static_cast<f32>((dblLambert ? 1u : 0u) | (key.twoSided ? 2u : 0u) |
                                       (surf->dimPerPixel ? 4u : 0u) |
                                       (surf->envReflect ? 8u : 0u) |
                                       (surf->envBlur ? 16u : 0u))};
        // The shader samples SNORM (raw/32767); fold the decode back so the
        // authored `uv = i16 * mul + add` comes out. .z rides the material's
        // emissive multiplier (see M3Surface::emissiveMultiplier), .w retail's
        // AlphaFactor (psmaterial.fx:358) — the per-draw coverage scale the
        // alpha-mask layers multiply into. It carries the composite section's
        // multiplier, which the source samples into FrameState::geosetAlphas.
        c->uvTransform = {32767.0f * surf->uvMultiply, surf->uvOffset,
                          surf->emissiveMultiplier,
                          geo.geosetAlpha * item.view->parentVisibility};
        // The instance's palette pair, de-gamma'd like every other authored
        // colour when the profile shades linearly.
        {
            const Sc2TeamColor& tc = ResolveTeamColor(item.view->teamColor);
            const bool linear = !ctx.profile || ctx.profile->LinearShading();
            auto enc = [&](const Vector3f& v) -> Vector4f {
                if (!linear)
                    return {v.x, v.y, v.z, 0.0f};
                return {SrgbToLinear(v.x), SrgbToLinear(v.y), SrgbToLinear(v.z), 0.0f};
            };
            c->teamDiffuse = enc(tc.diffuse);
            c->teamEmissive = enc(tc.emissive);
        }
        for (u32 i = 0; i < kLayerCount; ++i) {
            const M3Layer& l = surf->layers[i];
            u32 mode = l.mode;
            // Every other slot degrades gracefully to the white default, but
            // a white texel through the DXT5nm decode is a garbage normal —
            // so a normal layer whose texture has not resolved (still
            // loading, or a corpus with no assets beside it) switches off
            // instead. Flips on by itself the frame the texture lands.
            if (i == kM3LayerNormal && mode == 1) {
                const bool resolved = l.textureId >= 0 && item.view->textures &&
                                      item.view->textures->Get(l.textureId) !=
                                          gfx::TextureHandle::Invalid;
                if (!resolved)
                    mode = 0;
            }
            c->layerTint[i] = l.tint;
            // The environment slot's .y is the blur's mip range: the bound
            // cube's last level, which is what the engine-set
            // p_vEnvioTextureSize.z has to be for the guide's "sharp at 0,
            // very blurry at 4" to hold for its 1024 cubes.
            f32 addY = i == kM3LayerSpecular ? surf->specularScale : 0.0f;
            if (i == kM3LayerEnvironment && surf->envBlur && l.textureId >= 0 &&
                item.view->textures)
                addY = static_cast<f32>(
                    std::max(0, item.view->textures->MipLevels(l.textureId) - 1));
            c->layerAdd[i] = {l.add, addY, 0.0f, 0.0f};
            // Low nibble = UV set; bits 4-5 = which of the four wrap-variant
            // samplers this layer reads through; bit 6 invert, bit 7 clamp.
            c->layerCtl[i][0] = l.uvSource |
                                ((l.wrapFlags & assets::kSamplerWrapBitsMask) << 4) |
                                (l.invert ? 0x40u : 0u) | (l.clampColor ? 0x80u : 0u);
            c->layerCtl[i][1] = l.channels;
            c->layerCtl[i][2] = mode;
            // The diffuse slot's .w is its team mode; the decal and emissive
            // slots carry their LayerBlendOp.
            c->layerCtl[i][3] = (i == 0) ? l.teamColorMode : l.blendOp;

            // The layer's UV transform, out of the per-frame palette the
            // source fills (io::M3UvTransformId). A layer whose transform never
            // moves has no entry — that is the common case, and the identity
            // below is what it means.
            c->layerUvRow0[i] = {1.0f, 0.0f, 0.0f, 0.0f};
            c->layerUvRow1[i] = {0.0f, 1.0f, 0.0f, 0.0f};
            if (l.uvTransformId >= 0 && item.view->texAnimPalette &&
                static_cast<usize>(l.uvTransformId) < item.view->texAnimPalette->size()) {
                const auto& e = (*item.view->texAnimPalette)[static_cast<usize>(l.uvTransformId)];
                c->layerUvRow0[i] = {e.row0[0], e.row0[1], e.row0[3], 0.0f};
                c->layerUvRow1[i] = {e.row1[0], e.row1[1], e.row1[3], 0.0f};
            }

            // Fresnel. `.w` carries the mode on the first and the two
            // transform flags on the other two, which is what keeps the shader
            // to one compare per layer for the 99% that have none.
            const Vector3f& fbs = l.fresnelExponentBiasScale;
            c->layerFresnel[i] = {fbs.x, fbs.y, fbs.z, static_cast<f32>(l.fresnelMode)};
            c->layerFresnelMask[i] = {l.fresnelMask.x, l.fresnelMask.y, l.fresnelMask.z,
                                      (l.fresnelFlags & 0x1u) ? 1.0f : 0.0f};
            c->layerFresnelTrans[i] = {l.fresnelTranslation.x, l.fresnelTranslation.y,
                                       l.fresnelTranslation.z,
                                       (l.fresnelFlags & 0x2u) ? 1.0f : 0.0f};
        }
        gfxDev->UnmapBuffer(drawCb_);
    }

    cmd->BindPipeline(pso);
    cmd->BindVertexBuffer(0, geo.Stream(core::StreamId::Base), geo.baseStride);
    cmd->BindIndexBuffer(geo.ib, gfx::Format::R32_UINT);
    // Both CBs in both stages — Vulkan and WebGPU build one descriptor set per
    // stage from the shared layout, and a declared binding left unbound is a
    // validation error rather than a harmless omission.
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, passCb_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, passCb_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 1, drawCb_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1, drawCb_);
    if (key.skinned)
        cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 2, geo.bonePaletteCb);

    // Every texture slot, every draw — which of them survive into the
    // compiled PS is Slang's dead-code decision, and binding fewer than it
    // kept is a descriptor the runtime never writes (m2_shading.cpp's
    // lesson). Samplers are the four wrap VARIANTS, not one per layer: the
    // D3D12 root signature budgets four per stage, and four states is all
    // that exists — the layer picks its slot in the CB.
    const auto& defaults = rs_.Textures().GetDefaults();
    for (u32 u = 0; u < kLayerCount; ++u) {
        const M3Layer& l = surf->layers[u];
        const bool cubeSlot = u == kM3LayerEnvironment;
        gfx::TextureHandle tex = gfx::TextureHandle::Invalid;
        if (l.textureId >= 0 && item.view->textures)
            tex = item.view->textures->Get(l.textureId);
        if (tex == gfx::TextureHandle::Invalid)
            tex = cubeSlot ? defaults.BlackCube : defaults.White;
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, kLayerRegister[u], tex);
    }
    for (u32 w = 0; w <= assets::kSamplerWrapBitsMask; ++w)
        cmd->BindSampler(gfx::ShaderStage::Pixel, w, rs_.Samplers().WrapVariant(w));

    // G1 hook, after the PSO resolved and the CB was written.
    if (debug::DrawTraceEnabled()) {
        debug::TraceDraw d;
        d.shadingModel = static_cast<u8>(debug::TraceShadingModel::M3Standard);
        d.blendClass = static_cast<u8>(item.key.blend);
        d.streamMask = debug::kStreamBase;
        d.surface = static_cast<i32>(item.key.surface);
        d.matFlags = static_cast<i32>(surf->materialFlags);
        d.filterMode = static_cast<i32>(surf->blendMode);
        // Clamped to the trace record's width, which is a cross-profile file
        // format — widening it would invalidate every recorded baseline, and
        // the slots past it (gloss, the second alpha mask) are not what a
        // draw-order diff is looking at.
        for (u32 u = 0; u < kLayerCount && u < static_cast<u32>(debug::kTraceTexSlots); ++u)
            d.texIds[u] = surf->layers[u].textureId;
        d.psoKey = debug::TracePsoKey({
            .psPermute = static_cast<u32>(key.blend) | (key.mrt ? 0x100u : 0u) |
                         (key.skinned ? 0x200u : 0u),
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

gfx::PipelineHandle M3StandardShading::GetOrBuildRibbonPso(const RibbonPsoKey& key) {
    if (auto it = ribbonPsos_.find(key); it != ribbonPsos_.end())
        return it->second;
    auto* gfxDev = rs_.Pipeline().Gfx();
    if (!gfxDev)
        return gfx::PipelineHandle::Invalid;

    // renderer::Vertex, by hand: the ribbon strip carries POSITION / COLOR / UV,
    // no normal (the billboard has none) and no bone stream. The VS consumes
    // exactly these three, so the layout matches the DXIL signature on D3D12
    // with no over-declaration.
    const gfx::InputElement elements[] = {
        {"POSITION", 0, gfx::Format::R32G32B32_FLOAT, offsetof(Vertex, position), 0},
        {"COLOR", 0, gfx::Format::R32G32B32A32_FLOAT, offsetof(Vertex, color), 0},
        {"TEXCOORD", 0, gfx::Format::R32G32_FLOAT, offsetof(Vertex, uv), 0},
    };

    gfx::GraphicsPipelineDesc desc{};
    desc.vs = vsRibbon_;
    desc.ps = ps_; // the full material PS — same layer stack a geoset gets
    desc.inputLayout = std::span<const gfx::InputElement>(elements);
    desc.inputSlotStrides[0] = sizeof(Vertex);
    desc.topology = gfx::PrimitiveTopology::TriangleList;
    desc.blend = M3BlendDesc(key.blend);
    desc.depthStencil.depthTest = true;
    desc.depthStencil.depthWrite = M3BlendWritesDepth(key.blend);
    desc.depthStencil.depthCompare = gfx::CompareOp::LessEqual;
    desc.rasterizer.cull = key.twoSided ? gfx::CullMode::None : gfx::CullMode::Back;
    desc.rasterizer.frontCCW = true;
    desc.rtvFormat = key.rtv;
    desc.dsvFormat = key.dsv;
    desc.extraRtvFormats[0] = key.extra0;
    desc.extraRtvFormats[1] = key.extra1;
    desc.extraRtvFormats[2] = key.extra2;
    desc.extraRtvCount = key.extraRtvCount;
    // Declared only to satisfy the scene pass' attachment set; the forward
    // ribbon PS writes SV_Target0 alone and must leave the G-buffer cleared,
    // exactly as a transparent geoset draw does (Draw's key.mrt == false arm).
    desc.extraColorWrite = false;

    const auto pso = gfxDev->CreateGraphicsPipeline(desc);
    ribbonPsos_.emplace(key, pso);
    return pso;
}

void M3StandardShading::DrawRibbon(model::Actor& actor, i32 surfaceIndex, i32 vertexOffset,
                                   i32 vertexCount, const bls::FrameInputs& frame) {
    Init();
    if (vsRibbon_ == gfx::ShaderHandle::Invalid || ps_ == gfx::ShaderHandle::Invalid ||
        vertexCount <= 0)
        return;
    const gfx::BufferHandle vb = actor.render.ribbonVB;
    if (vb == gfx::BufferHandle::Invalid)
        return;

    const auto* table = core::SurfaceTableCast<M3SurfaceTable>(actor.render.surfaceTable.get());
    const M3Surface* surf = table ? table->Surface(static_cast<u32>(surfaceIndex)) : nullptr;
    if (!surf || !surf->valid)
        return;

    auto* gfxDev = rs_.Pipeline().Gfx();
    auto* cmd = gfxDev ? gfxDev->GetImmediateContext() : nullptr;
    if (!cmd)
        return;

    RibbonPsoKey key;
    key.rtv = rs_.Pipeline().SceneTargetFormat();
    key.dsv = rs_.Pipeline().DepthStencilFormat();
    gfx::Format extra[3] = {gfx::Format::Unknown, gfx::Format::Unknown, gfx::Format::Unknown};
    key.extraRtvCount = rs_.Pipeline().SceneExtraRtvFormats(extra);
    key.extra0 = extra[0];
    key.extra1 = extra[1];
    key.extra2 = extra[2];
    key.blend = static_cast<u8>(surf->blendMode);
    // A ribbon is dual-sided geometry regardless of its material's TwoSided
    // flag: a camera-facing billboard flips winding as the camera orbits, and a
    // tube's far wall faces away, so a single-sided cull drops half of it (the
    // "wrong winding" look). Draw cull-none, as the WC3 path already does via
    // dl.twoSided; the shader's back-face normal flip (params0.w bit 2, keyed on
    // this) then lights both walls. Ribbon.fx:532 calls planar ribbons dual-sided.
    key.twoSided = true;
    const gfx::PipelineHandle pso = GetOrBuildRibbonPso(key);
    if (pso == gfx::PipelineHandle::Invalid)
        return;

    // The ribbon's own pass CB, so the interleaved draw never disturbs passCb_
    // that a later geoset transparent draw still reads. It carries the SAME
    // lighting rig — a lit ribbon must light, not shade against zero and go
    // black (which is exactly what an empty CB did).
    if (auto* c = static_cast<M3PassCb*>(gfxDev->MapBuffer(ribbonPassCb_))) {
        WritePassCb(*c, frame.view, frame.projection, rs_.Pipeline().FrameCamera().GetSource(),
                    rs_.Pipeline().ActiveProfile().LinearShading());
        gfxDev->UnmapBuffer(ribbonPassCb_);
    }

    const bool unshaded = (surf->materialFlags & static_cast<u32>(MaterialFlag::Unshaded)) != 0;
    const bool dblLambert =
        (surf->materialFlags & static_cast<u32>(MaterialFlag::DoubleLambert)) != 0;

    if (auto* c = static_cast<M3DrawCb*>(gfxDev->MapBuffer(drawCb_))) {
        *c = M3DrawCb{};
        c->world = Matrix44f::identity(); // strip is world-space already
        // The material decides shading, exactly as a geoset — Unshaded and the
        // flag bits ride through so a lit ribbon lights and an emissive one glows.
        c->params0 = {surf->alphaTestThreshold, unshaded ? 1.0f : 0.0f, surf->specularExponent,
                      static_cast<f32>((dblLambert ? 1u : 0u) | (key.twoSided ? 2u : 0u) |
                                       (surf->dimPerPixel ? 4u : 0u) |
                                       (surf->envReflect ? 8u : 0u) |
                                       (surf->envBlur ? 16u : 0u))};
        // No SNORM fold for the ribbon uv (.x/.y unused by its VS); .z the
        // emissive multiplier, .w the AlphaFactor coverage (parent visibility).
        c->uvTransform = {1.0f, 0.0f, surf->emissiveMultiplier, actor.parentVisibility};
        {
            const bool linear = rs_.Pipeline().ActiveProfile().LinearShading();
            const Sc2TeamColor& tc = ResolveTeamColor(actor.teamColor);
            auto enc = [&](const Vector3f& v) -> Vector4f {
                if (!linear)
                    return {v.x, v.y, v.z, 0.0f};
                return {SrgbToLinear(v.x), SrgbToLinear(v.y), SrgbToLinear(v.z), 0.0f};
            };
            c->teamDiffuse = enc(tc.diffuse);
            c->teamEmissive = enc(tc.emissive);
        }
        for (u32 i = 0; i < kLayerCount; ++i) {
            const M3Layer& l = surf->layers[i];
            c->layerTint[i] = l.tint;
            // The environment slot's .y is the blur's mip range: the bound
            // cube's last level, which is what the engine-set
            // p_vEnvioTextureSize.z has to be for the guide's "sharp at 0,
            // very blurry at 4" to hold for its 1024 cubes.
            f32 addY = i == kM3LayerSpecular ? surf->specularScale : 0.0f;
            if (i == kM3LayerEnvironment && surf->envBlur && l.textureId >= 0 &&
                actor.render.textures)
                addY = static_cast<f32>(
                    std::max(0, actor.render.textures->MipLevels(l.textureId) - 1));
            c->layerAdd[i] = {l.add, addY, 0.0f, 0.0f};
            c->layerCtl[i][0] = l.uvSource |
                                ((l.wrapFlags & assets::kSamplerWrapBitsMask) << 4) |
                                (l.invert ? 0x40u : 0u) | (l.clampColor ? 0x80u : 0u);
            c->layerCtl[i][1] = l.channels;
            c->layerCtl[i][2] = l.mode;
            c->layerCtl[i][3] = (i == 0) ? l.teamColorMode : l.blendOp;
            // The full material UV matrix, exactly as the geoset path. A ribbon
            // takes the same centre-pivot TRS every other layer does (composed in
            // M3ComposeUvTransform), so the wings' -90° and its animated scroll
            // land where the artist authored them — width across, age along.
            c->layerUvRow0[i] = {1.0f, 0.0f, 0.0f, 0.0f};
            c->layerUvRow1[i] = {0.0f, 1.0f, 0.0f, 0.0f};
            if (l.uvTransformId >= 0 &&
                static_cast<usize>(l.uvTransformId) < actor.render.texAnimPalette.size()) {
                const auto& e = actor.render.texAnimPalette[static_cast<usize>(l.uvTransformId)];
                c->layerUvRow0[i] = {e.row0[0], e.row0[1], e.row0[3], 0.0f};
                c->layerUvRow1[i] = {e.row1[0], e.row1[1], e.row1[3], 0.0f};
            }
            const Vector3f& fbs = l.fresnelExponentBiasScale;
            c->layerFresnel[i] = {fbs.x, fbs.y, fbs.z, static_cast<f32>(l.fresnelMode)};
            c->layerFresnelMask[i] = {l.fresnelMask.x, l.fresnelMask.y, l.fresnelMask.z,
                                      (l.fresnelFlags & 0x1u) ? 1.0f : 0.0f};
            c->layerFresnelTrans[i] = {l.fresnelTranslation.x, l.fresnelTranslation.y,
                                       l.fresnelTranslation.z,
                                       (l.fresnelFlags & 0x2u) ? 1.0f : 0.0f};
        }
        gfxDev->UnmapBuffer(drawCb_);
    }

    cmd->BindPipeline(pso);
    cmd->BindVertexBuffer(0, vb, sizeof(Vertex));
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 0, ribbonPassCb_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 0, ribbonPassCb_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Vertex, 1, drawCb_);
    cmd->BindConstantBuffer(gfx::ShaderStage::Pixel, 1, drawCb_);

    const auto& defaults = rs_.Textures().GetDefaults();
    for (u32 u = 0; u < kLayerCount; ++u) {
        const M3Layer& l = surf->layers[u];
        const bool cubeSlot = u == kM3LayerEnvironment;
        gfx::TextureHandle tex = gfx::TextureHandle::Invalid;
        if (l.textureId >= 0 && actor.render.textures)
            tex = actor.render.textures->Get(l.textureId);
        if (tex == gfx::TextureHandle::Invalid)
            tex = cubeSlot ? defaults.BlackCube : defaults.White;
        cmd->BindShaderResource(gfx::ShaderStage::Pixel, kLayerRegister[u], tex);
    }
    for (u32 w = 0; w <= assets::kSamplerWrapBitsMask; ++w)
        cmd->BindSampler(gfx::ShaderStage::Pixel, w, rs_.Samplers().WrapVariant(w));

    if (debug::DrawTraceEnabled()) {
        debug::TraceDraw d;
        d.shadingModel = static_cast<u8>(debug::TraceShadingModel::M3Standard);
        d.blendClass = static_cast<u8>(M3ClassifySurface(*surf).blend);
        d.streamMask = debug::kStreamBase;
        d.surface = surfaceIndex;
        d.matFlags = static_cast<i32>(surf->materialFlags);
        d.filterMode = static_cast<i32>(surf->blendMode);
        d.vertexCount = vertexCount;
        d.actor.rootActor = debug::TraceRootOrdinal(rs_.Scene().Actors().All(), actor.handle);
        d.actor.role = static_cast<u8>(actor.role);
        d.actor.treeDepth = static_cast<u8>(actor.treeDepth);
        for (u32 u = 0; u < kLayerCount && u < static_cast<u32>(debug::kTraceTexSlots); ++u)
            d.texIds[u] = surf->layers[u].textureId;
        d.psoKey = debug::TracePsoKey({
            .psPermute = static_cast<u32>(key.blend) | 0x400u, // ribbon marker
            .extraRtvCount = key.extraRtvCount,
        });
        debug::RecordProducerDraw(d);
    }

    cmd->Draw(vertexCount, vertexOffset);
}

core::SurfaceClass M3StandardShading::Classify(const render_detail::RenderableView& view,
                                               const GPUGeoset& geo) const {
    (void)view;
    (void)geo;
    // Only reached for a geoset with no surface range (table build failed).
    return {.visible = true, .blend = core::BlendClass::Opaque, .needsDepthFill = false};
}

core::SurfaceClass M3StandardShading::ClassifySurface(const render_detail::RenderableView& view,
                                                      const GPUGeoset& geo, u32 surface) const {
    (void)geo;
    const M3SurfaceTable* table = TableOf(view);
    const M3Surface* s = table ? table->Surface(surface) : nullptr;
    if (!s)
        return {.visible = false};
    return M3ClassifySurface(*s);
}

core::VertexNeeds M3StandardShading::Needs(u32 surface) const {
    (void)surface;
    return core::VertexNeeds{.position = true,
                             .normal = true,
                             .tangent = true,
                             .uvSets = 2,
                             .boneWeights = true};
}

i32 M3StandardShading::SelectLights(bls::FrameInputs& frame, const bls::LightingContext& lighting,
                                    const Matrix44f& viewMat, const Vector3f& surfaceWS) const {
    (void)frame;
    (void)lighting;
    (void)viewMat;
    (void)surfaceWS;
    // The key light is pass-global (BeginPass writes it); local lights reach
    // pixels through the deferred pass, never the WC3 palette.
    return 0;
}

core::EmitMask M3StandardShading::Emits(u32 surface, core::PassSlot pass) const {
    (void)surface;
    if (pass == core::PassSlot::ShadowMap || pass == core::PassSlot::DepthPrepass)
        return core::EmitMask::Depth;
    if (pass == core::PassSlot::GBuffer) {
        return core::EmitMask::Color | core::EmitMask::Depth | core::EmitMask::LinearDepth |
               core::EmitMask::Normal | core::EmitMask::Diffuse | core::EmitMask::Specular |
               core::EmitMask::SpecPower;
    }
    return core::EmitMask::DefaultColor;
}

} // namespace whiteout::flakes::renderer::profiles::sc2_heroes
