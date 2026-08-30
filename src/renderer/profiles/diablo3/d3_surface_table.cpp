#include "renderer/profiles/diablo3/d3_surface_table.h"

#include "io/d3/d3_model_adapter.h"
#include "io/d3/d3_sno_cache.h"

#include <whiteout/sno/d3/native/geometry.h>

#include <algorithm>
#include <map>
#include <vector>

namespace whiteout::flakes::renderer::profiles::diablo3 {

namespace {

using ::whiteout::flakes::io::D3SubObjectRef;
using ::whiteout::flakes::io::D3TextureRef;

// MaterialColors::dwMaterialFlags. The fallback for a sub-object whose
// ShaderMap does not resolve; where it does, the RenderPass wins, because that
// is the field the original reads. Never confirmed against the binary — no
// consumer of dwMaterialFlags has been found — which is the other reason the
// pass takes precedence.
enum : u32 {
    kMatFlagAlphaBlend = 0x1u,
    kMatFlagAlphaTest = 0x2u,
    kMatFlagTwoSided = 0x4u,
};

using ::whiteout::flakes::io::D3ReadStageArg;
using ::whiteout::flakes::io::D3ReadUvXform;
using ::whiteout::flakes::io::D3SlotIsAlphaMask;
using ::whiteout::flakes::io::D3SlotOfType;
using ::whiteout::flakes::io::D3TextureTypeOf;
using ::whiteout::flakes::io::D3TypeBit;
using ::whiteout::flakes::io::D3UvMode;
using ::whiteout::flakes::io::D3UvTransformId;

// ShaderMap_ResolveShaderOpaque's tag chain (0x71001B5420). A ShaderMap has no
// index: the runtime probes a fixed list and takes the first tag that resolves,
// so a map missing a tag silently falls through to a more generic program.
//
// The head of the chain is chosen by the global view mode and the tail is
// shared. We render one view mode, so only the tail plus its 0x30502 head is
// walked — and 0x30500, the last-resort base shader, is what shipped content
// overwhelmingly carries (Imperius's wing map holds exactly one entry, tagged
// 0x30500). The MSAA band (0x30861) is skipped: we do not run the original's
// MSAA path, and taking its program would be claiming a pass we never bind.
constexpr u32 kD3OpaqueTagChain[] = {0x30502u, 0x30850u, 0x30830u, 0x30600u, 0x30500u};

/// The shader-variant tag that turns the light block off -- TAG_VS_LIGHTING,
/// "Enable Lighting", in the tag registry the Windows build ships at
/// 0x148B680. Its neighbours are the five per-type light counts
/// Render_EnsureShaderVariant clamps to 16 (0xA0008 TAG_VS_NUM_POINT_LIGHTS,
/// 0xA0009 spot, 0xA000A directional, 0xA000C cylindrical, 0xA000D
/// point-linear); this one sits just above them and gates the lot.
constexpr u32 kD3TagLightingEnable = 0xA000Fu;

/// The tag the back pass of a two-sided pair raises. Measured over all 1,507
/// corpus `.shd`: worth 1 on exactly 12 passes and 0 on 15, and every one of
/// the twelve is pass 1 of a `cloth_*` shader culling CCW against a pass 0
/// culling CW. It is the flip-the-normal switch, which is what the second draw
/// is for -- and the registry agrees to the word: TAG_VS_FLIP_NORMAL_BACKFACE,
/// "Flip Normal (BackFace)".
constexpr u32 kD3TagBackFacePass = 0xA003Du;

/// The tag that selects a `_pma` program's output form -- TAG_VS_PMA_FUNC,
/// "PMA Func" -- 1 = premultiply and invert the alpha, 2 = premultiply and
/// write 1. Measured over the corpus's
/// 1,831 passes: present with value 1 on 43, 2 on 13, 0 on 4, and 55 of those
/// 56 live values sit on a pass whose blend src is 11 (BLENDFACTOR). The two
/// `src == 11` passes that carry no tag are read from the dst instead.
constexpr u32 kD3TagPremultipliedAlpha = 0xA002Bu;

/// The blend factor the engine's own enum reserves for the constant, and the
/// constant `sub_73D580` hard-codes beside it: 0x00FFFFFF -- (1, 1, 1) for the
/// colour and 0 for the alpha.
constexpr u32 kD3BlendFactorConstant = 11u;

/// @brief The premultiplied-alpha mode for a pass, or 0.
///
/// See D3PassState::pmaMode. The dst factor decides between the two forms that
/// carry no tag, because it is what the shipped program is written against:
/// `dst = SRCALPHA` needs the inverted alpha, `dst = INVSRCALPHA` does not.
u32 D3PmaModeOf(const d3n::RenderPass& pass, u32 blendSrc, u32 blendDst) {
    if (blendSrc != kD3BlendFactorConstant)
        return 0;
    for (const auto& t : pass.arShaderParams)
        if (t.dwTagId == kD3TagPremultipliedAlpha && t.dwValue == 2u)
            return 2;
    return blendDst == 5u ? 1u : 3u;
}

const d3n::ShaderTagMapEntry* FindTag(const d3n::RenderPass& pass, u32 id) {
    for (const auto& t : pass.arShaderParams) {
        if (t.dwTagId == id)
            return &t;
    }
    return nullptr;
}

/// @brief Does this sub-object carry no vertex colour at all?
///
/// The level bake, absent. Stops at the first non-zero: a graded mesh answers
/// on its first vertex and only a genuinely blank one walks the whole array.
bool VertexColorAllBlack(const d3n::SubObject& sub) {
    for (const auto& v : sub.arVertices) {
        const auto c = d3n::vertexColor(v);
        if (c.r != 0 || c.g != 0 || c.b != 0)
            return false;
    }
    return true;
}

i32 ShadersIdFor(const d3n::ShaderMap& map) {
    for (const u32 tag : kD3OpaqueTagChain) {
        for (const auto& e : map.arShaders) {
            if (e.dwTagId == tag && e.snoShader.valid())
                return e.snoShader.id;
        }
    }
    // Nothing on the chain. Shipped maps are small and single-tagged often
    // enough that refusing here would drop real state, so the first valid entry
    // stands in — a more generic program is exactly what the fall-through
    // produces anyway.
    for (const auto& e : map.arShaders) {
        if (e.snoShader.valid())
            return e.snoShader.id;
    }
    return -1;
}

i32 TextureIdOf(std::span<const D3TextureRef> textures, i32 sno) {
    for (usize i = 0; i < textures.size(); ++i) {
        if (textures[i].snoId == sno)
            return static_cast<i32>(i);
    }
    return -1;
}

// The verbatim 4x4 of UV mode 1. An all-zero block is what an entry with no
// authored matrix carries, and uploading it would collapse every texture
// coordinate to the origin.
bool UvMatrixOf(const d3n::MaterialTextureEntry& e, Matrix44f& out) {
    const Vector4f rows[4] = {e.vUvRow0, e.vUvRow1, e.vUvRow2, e.vUvRow3};
    bool authored = false;
    for (const Vector4f& v : rows) {
        if (v.x != 0.0f || v.y != 0.0f || v.z != 0.0f || v.w != 0.0f)
            authored = true;
    }
    if (!authored)
        return false;
    out = Matrix44f::identity();
    for (int r = 0; r < 4; ++r) {
        out.data[r][0] = rows[r].x;
        out.data[r][1] = rows[r].y;
        out.data[r][2] = rows[r].z;
        out.data[r][3] = rows[r].w;
    }
    return true;
}

bool SameStages(const d3n::RenderPass& a, const d3n::RenderPass& b) {
    if (a.arTextureStages.size() != b.arTextureStages.size())
        return false;
    for (usize i = 0; i < a.arTextureStages.size(); ++i) {
        const auto& x = a.arTextureStages[i];
        const auto& y = b.arTextureStages[i];
        if (x.dwTextureType != y.dwTextureType || x.dwAddressU != y.dwAddressU ||
            x.dwAddressV != y.dwAddressV || x.dwAddressW != y.dwAddressW ||
            x.dwFilter != y.dwFilter || x.flMipMapLodBias != y.flMipMapLodBias)
            return false;
    }
    return true;
}

/// Every RenderParams field but the cull mode. Field by field rather than a
/// memcmp: `bAlphaRef` is a byte between two ints, so the struct has padding
/// a comparison must not read.
bool SameRenderParamsButCull(const d3n::RenderParams& a, const d3n::RenderParams& b) {
    return a.dwZWriteEnable == b.dwZWriteEnable && a.dwZFunc == b.dwZFunc &&
           a.flDepthBias == b.flDepthBias && a.flUnknown10 == b.flUnknown10 &&
           a.dwStencilEnable == b.dwStencilEnable && a.dwStencilFunc == b.dwStencilFunc &&
           a.dwStencilRef == b.dwStencilRef && a.dwStencilPass == b.dwStencilPass &&
           a.dwStencilFail == b.dwStencilFail && a.dwStencilZFail == b.dwStencilZFail &&
           a.dwAlphaTestEnable == b.dwAlphaTestEnable && a.dwAlphaFunc == b.dwAlphaFunc &&
           a.bAlphaRef == b.bAlphaRef && a.dwAlphaToCoverage == b.dwAlphaToCoverage &&
           a.dwFogEnable == b.dwFogEnable && a.dwFillMode == b.dwFillMode &&
           a.dwColorWriteEnable == b.dwColorWriteEnable &&
           a.dwAlphaWriteEnable == b.dwAlphaWriteEnable &&
           a.dwAlphaBlendEnable == b.dwAlphaBlendEnable && a.dwBlendOp == b.dwBlendOp &&
           a.dwSrcBlend == b.dwSrcBlend && a.dwDestBlend == b.dwDestBlend &&
           a.dwConstantColor == b.dwConstantColor;
}

bool SameTagsButBackFace(const d3n::RenderPass& a, const d3n::RenderPass& b) {
    std::map<u32, u32> ta, tb;
    for (const auto& t : a.arShaderParams) {
        if (t.dwTagId != kD3TagBackFacePass)
            ta[t.dwTagId] = t.dwValue;
    }
    for (const auto& t : b.arShaderParams) {
        if (t.dwTagId != kD3TagBackFacePass)
            tb[t.dwTagId] = t.dwValue;
    }
    return ta == tb;
}

} // namespace

/// @brief Is this Shaders one TWO-SIDED draw written as two passes?
///
/// A RenderPass has no two-sided cull state — D3DCULL is {none, CW, CCW} and
/// nothing else — so content that wants a sheet lit from both sides ships the
/// same pass twice, the second with the opposite winding and
/// `kD3TagBackFacePass` raised to negate the normal. Twelve corpus shaders do
/// it, all named `cloth_*`, and Tyrael's cape is one of them.
///
/// The test is deliberately narrow: same programs, same stages, same state, same
/// tags, and the two culls covering both windings. Over the corpus's 293
/// multi-pass shaders that fires on exactly those twelve. Every other
/// multi-pass shader varies its program or its stages too, and is a second
/// effect layer this build still does not draw — collapsing one of those would
/// shade the extra layer with the first pass's program.
///
/// Collapsing rather than submitting twice is exact here because
/// `psD3Standard` already flips the normal on `SV_IsFrontFace`: one cull-none
/// draw shades each triangle the way whichever pass would have claimed it. The
/// one thing it does not keep is the order — the client draws every front face
/// before any back face, and a single draw follows the index buffer — which is
/// visible only where a cloth folds over itself and blends against itself.
bool D3IsTwoSidedPassPair(const d3n::Shaders& sh) {
    if (sh.arRenderPasses.size() != 2)
        return false;
    const auto& a = sh.arRenderPasses[0];
    const auto& b = sh.arRenderPasses[1];
    const u32 ca = static_cast<u32>(a.tRenderParams.dwCullMode);
    const u32 cb = static_cast<u32>(b.tRenderParams.dwCullMode);
    if (!((ca == 2 && cb == 3) || (ca == 3 && cb == 2)))
        return false;
    return a.szEffectFile == b.szEffectFile && a.szVertexShaderEntry == b.szVertexShaderEntry &&
           a.szPixelShaderEntry == b.szPixelShaderEntry && a.dwUnknown00 == b.dwUnknown00 &&
           a.dwUnknown04 == b.dwUnknown04 && a.dwPassFlags == b.dwPassFlags &&
           SameStages(a, b) && SameRenderParamsButCull(a.tRenderParams, b.tRenderParams) &&
           SameTagsButBackFace(a, b);
}

D3PassState D3PassStateFor(const d3n::SubObjectAppearance& variant,
                           ::whiteout::flakes::io::D3SnoCache* cache) {
    return D3PassStateFor(variant.tMaterial, cache);
}

D3PassState D3PassStateFor(const d3n::UberMaterial& material,
                           ::whiteout::flakes::io::D3SnoCache* cache) {
    D3PassState st;
    if (!cache || !material.snoShaderMap.valid())
        return st;
    const auto map = cache->ShaderMap(material.snoShaderMap.id);
    if (!map)
        return st;
    const i32 shadersId = ShadersIdFor(*map);
    if (shadersId < 0)
        return st;
    const auto shaders = cache->Shaders(shadersId);
    if (!shaders)
        return st;
    return D3PassStateOf(*shaders);
}

D3PassState D3PassStateOf(const d3n::Shaders& shadersAsset) {
    D3PassState st;
    const auto* shaders = &shadersAsset;
    if (shaders->arRenderPasses.empty())
        return st;

    // Pass 0. A multi-pass Shaders draws the same geometry more than once with
    // complementary colour-write masks (Imperius's wings are 1/0 then 0/1), and
    // reproducing that is a submission change, not a state one — so the first
    // pass is the one whose state this carries, and the extra passes are simply
    // not drawn.
    const auto& pass0 = shaders->arRenderPasses[0];
    const auto& r = pass0.tRenderParams;
    st.resolved = true;
    // RenderParams, whose every field was named off the Windows 2.8.x build's
    // `sub_5717F0` — it hands each one to a single D3D9 setter, so the map is a
    // reading and not a guess. Four fields are decoded and deliberately not
    // applied, for the reasons D3_MATERIAL_AUDIT.md §2.1 gives: the stencil
    // block (`sub_5717F0` never calls the stencil setters at all),
    // `dwAlphaToCoverage` (inert without MSAA, and this build has none),
    // `dwFogEnable` (there is no fog here), and `dwConstantColor` (the float4
    // the shader atlas names Constant0, which no shader here binds).
    st.cull = static_cast<u32>(r.dwCullMode);
    st.twoSidedPair = D3IsTwoSidedPassPair(*shaders);
    st.depthWrite = r.dwZWriteEnable != 0;
    st.depthFunc = static_cast<u32>(r.dwZFunc);
    st.depthBias = r.flDepthBias;
    st.fillMode = static_cast<u32>(r.dwFillMode);
    st.colorWrite = r.dwColorWriteEnable != 0;
    st.alphaWrite = r.dwAlphaWriteEnable != 0;
    st.alphaTestEnable = r.dwAlphaTestEnable != 0;
    st.alphaFunc = static_cast<u32>(r.dwAlphaFunc);
    st.alphaRef = r.bAlphaRef;
    st.blendEnable = r.dwAlphaBlendEnable != 0;
    st.blendSrc = static_cast<u32>(r.dwSrcBlend);
    st.blendDst = static_cast<u32>(r.dwDestBlend);
    st.pmaMode = D3PmaModeOf(pass0, st.blendSrc, st.blendDst);
    for (const auto& stage : pass0.arTextureStages) {
        st.declaredTypes |= D3TypeBit(stage.dwTextureType);
        if (st.stageCount < D3PassState::kMaxStages)
            st.stages[st.stageCount++] = {stage.dwTextureType, io::D3StageWrapBits(stage)};
    }
    st.effectFile = pass0.szEffectFile;
    // The fixed-function stage block. Present on every Legacy.fx pass and on no
    // other family this reproduces, so reading it needs no effect-file test:
    // a pass either carries the tags or it does not. See D3StageArg.
    //
    // The block is indexed by CONTENT stage -- `arTextureStages` position with
    // the scene-depth stage skipped, which only SoftBillboard.fx declares and
    // always first.
    std::vector<i32> content;
    content.reserve(pass0.arTextureStages.size());
    for (usize i = 0; i < pass0.arTextureStages.size(); ++i) {
        if (pass0.arTextureStages[i].dwTextureType != io::kD3TextureTypeSceneDepth)
            content.push_back(pass0.arTextureStages[i].dwTextureType);
    }
    bool colorSeen = false;
    bool alphaSeen = false;
    for (u32 i = 0; i < io::kD3StageArgCount; ++i) {
        const auto* color = FindTag(pass0, io::kD3TagStageColor + i);
        const auto* alpha = FindTag(pass0, io::kD3TagStageAlpha + i);
        if (!color && !alpha)
            continue;
        st.stageArgs = true;
        const u32 cCode = color ? color->dwValue : 0;
        const u32 aCode = alpha ? alpha->dwValue : 0;
        // A gain can sit on a stage past the texture list -- `x1_Death_Orb_am_uv2`
        // parks a x2 on stage 1 of a one-stage pass and its program applies it --
        // so the summary gains are accumulated over the whole block and the
        // channel bits only over the stages that name a type.
        const auto c = D3ReadStageArg(cCode);
        const auto a = D3ReadStageArg(aCode);
        st.colorGain *= c.gain;
        st.alphaGain *= a.gain;
        // A stage with no texture is a chain operation on what is already there:
        // 40 multiplies the vertex colour in at the END, 42 squares.
        if (io::D3StageIsVertexColorOnly(cCode))
            st.colorVcolLast = true;
        if (io::D3StageIsVertexColorOnly(aCode))
            st.alphaVcolLast = true;
        if (c.usesTexture && !colorSeen) {
            colorSeen = true;
            st.colorVcolFirst = io::D3StageTakesVertexColor(cCode);
        }
        if (a.usesTexture && !alphaSeen) {
            alphaSeen = true;
            st.alphaVcolFirst = io::D3StageTakesVertexColor(aCode);
        }
        if (i < content.size() && st.combineCount < D3PassState::kMaxStages) {
            D3PassState::Combine& cb = st.combines[st.combineCount++];
            cb.type = content[i];
            cb.colorOp = c.adds ? io::kD3StageAdd
                                : (c.usesTexture ? io::kD3StageModulate : io::kD3StageSkip);
            cb.alphaOp = a.adds ? io::kD3StageAdd
                                : (a.usesTexture ? io::kD3StageModulate : io::kD3StageSkip);
            cb.colorGain = c.gain;
            cb.alphaGain = a.gain;
            cb.colorClamp = c.clamps;
            cb.alphaClamp = a.clamps;
        }
        if (i >= pass0.arTextureStages.size())
            continue;
        const i32 type = pass0.arTextureStages[i].dwTextureType;
        const u64 bit = D3TypeBit(type);
        if (color || alpha)
            st.namedTypes |= bit;
        if (c.modulates)
            st.colorTypes |= bit;
        if (a.modulates)
            st.alphaTypes |= bit;
        if (c.usesTexture)
            st.colorSampledTypes |= bit;
        if (a.usesTexture)
            st.alphaSampledTypes |= bit;
    }
    // The pass's own tag map, which is where the light budget lives:
    // Render_EnsureShaderVariant reads five counts from it (0xA0008 point,
    // 0xA0009 spot, 0xA000A directional, 0xA000C cylindrical, 0xA000D
    // point-linear) and compiles one program per combination. 0xA000F is the
    // switch above those — see D3PassState::lit. Absent means "take the global
    // default", and for this one the default is on.
    for (const auto& t : pass0.arShaderParams) {
        if (t.dwTagId == kD3TagLightingEnable)
            st.lit = t.dwValue != 0;
    }
    st.vertexColorLights = pass0.szEffectFile == "Scene.fx" || pass0.szEffectFile == "Prop.fx";
    st.vertexAlpha = pass0.szEffectFile == "ActorIrrad.fx" ||
                     pass0.szVertexShaderEntry.find("vertalpha") != std::string::npos;
    st.glowLights = pass0.szEffectFile == "ActorIrrad.fx" || pass0.szEffectFile == "Scene.fx";
    return st;
}

std::unique_ptr<D3SurfaceTable>
BuildD3SurfaceTable(const d3n::Appearances& app, u32 lookIndex,
                    std::span<const D3TextureRef> textures, std::span<const D3SubObjectRef> emitted,
                    ::whiteout::flakes::io::D3SnoCache* cache, std::span<const u32> lookByGeoset,
                    D3TypeCensus* census) {
    auto table = std::make_unique<D3SurfaceTable>();
    auto& surfaces = table->Surfaces();
    surfaces.resize(emitted.size());

    std::map<i32, usize> typeCounts;
    usize unmatched = 0;
    usize pastCap = 0;

    for (usize g = 0; g < emitted.size(); ++g) {
        const d3n::GeoSet& set = (emitted[g].geoSet == 0) ? app.tGeoSet0 : app.tGeoSet1;
        if (emitted[g].index >= set.arSubObjects.size())
            continue;
        const d3n::SubObject& sub = set.arSubObjects[emitted[g].index];
        D3Surface& s = surfaces[g];
        s.rigid = sub.arVertexInfluences.empty();

        const u32 look = (g < lookByGeoset.size()) ? lookByGeoset[g] : lookIndex;
        const d3n::SubObjectAppearance* variant = ::whiteout::flakes::io::D3VariantFor(app, sub, look);
        if (!variant) {
            // A name that finds no material is content, not a bug: the surface
            // stays invalid and the actor keeps the unlit fallback for it.
            ++unmatched;
            continue;
        }

        std::shared_ptr<const d3n::Material> keepAlive;
        const d3n::UberMaterial* mat = ::whiteout::flakes::io::D3MaterialOf(*variant, cache, keepAlive);
        if (!mat)
            continue;

        const auto& colors = mat->tColors;
        s.diffuse = colors.vDiffuse;
        s.specular = colors.vSpecular;
        s.emissive = colors.vEmissive;
        s.ambient = colors.vAmbient;
        // Raw, with no invented default: 99.4% of shipped materials leave this
        // at 0, and substituting a plausible-looking 20 there fabricated a
        // broad highlight on every surface. The shader clamps to 1 and only
        // reads this at all once a specular map resolves.
        s.shininess = colors.flShininess;
        s.materialFlags = static_cast<u32>(colors.dwMaterialFlags);
        s.alphaBlend = (s.materialFlags & kMatFlagAlphaBlend) != 0;
        s.twoSided = (s.materialFlags & kMatFlagTwoSided) != 0;
        s.alphaTestThreshold = (s.materialFlags & kMatFlagAlphaTest) != 0 ? (1.0f / 255.0f) : 0.0f;

        // The RenderPass overrides all three where it resolves, because that is
        // where the original reads them: `dwMaterialFlags` is 0 on Imperius's
        // wing material and the wings are alpha-blended with depth writes off.
        s.pass = D3PassStateFor(*variant, cache);
        if (s.pass.resolved) {
            s.alphaBlend = s.pass.blendEnable;
            // Either spelling: a pass that asks for no culling, or a pair
            // of passes that between them cover both windings.
            s.twoSided = s.pass.cull == 1 || s.pass.twoSidedPair;
            // Three fields, not one. `sub_5717F0` passes (0, 0) to the
            // alpha-func setter when the enable is clear, so a pass carrying a
            // reference with the test off tests nothing -- 194 shipped passes
            // do exactly that, and reading the reference alone cuts holes in
            // all of them.
            s.alphaTestFunc = s.pass.alphaTestEnable ? s.pass.alphaFunc : 0u;
            s.alphaTestThreshold =
                s.pass.alphaTestEnable ? static_cast<f32>(s.pass.alphaRef) * (1.0f / 255.0f) : 0.0f;
            // An unlit pass takes the vertex colour AS its light — but only
            // where there is one to take. See D3Surface::unlit.
            s.unlit = !s.pass.lit && !VertexColorAllBlack(sub);
        }
        // No embedded translucent material means the original had no
        // translucent Shaders id either, and skipped the sub-object outright
        // when it faded. §6.3.
        s.noTranslucentVariant = !s.alphaBlend;

        // Every entry, not the first twelve. The matTex0..11 ceiling bounds
        // the engine's texture *matrices*, and a shipped material routinely
        // carries more entries than that (12,551 of 49,695 sampled entries sit
        // past twelve) because most of them are the shared core-asset block
        // this build does not consume. Counted, so the day the block is
        // understood the number is already in front of whoever reads it.
        if (mat->arTextures.size() > kD3MaxTextureStages)
            pastCap += mat->arTextures.size() - kD3MaxTextureStages;

        for (const d3n::MaterialTextureEntry& entry : mat->arTextures) {
            const i32 type = D3TextureTypeOf(entry);
            ++typeCounts[type];

            // The pass decides which entries are LIVE. The adapter's canonical
            // texture list does not know that — it is built without a cache and
            // so without a pass — which makes the list a superset of what the
            // slots below bind. That is the safe direction: a texture nothing
            // samples costs an upload, a texture the list is missing has no id.
            //
            // A material routinely
            // carries types the bound program never asks for — Cain's book
            // ships a lightmap its `actor2_opaque_glow_skin` pass does not
            // declare, and the whole 26..38 block is declared by none of the
            // 2,208 passes measured — and binding one is a term the original
            // never applies. Only where the pass resolved: without one there is
            // no stage list to consult and every entry stands, as before.
            if (s.pass.resolved && s.pass.declaredTypes != 0 &&
                (s.pass.declaredTypes & D3TypeBit(type)) == 0)
                continue;

            const D3SlotKind kind = D3SlotOfType(type);
            if (kind == D3SlotKind::Count)
                continue;
            // The glow map only where its family agrees on what to do with it
            // -- unless the pass states it per stage, which is the shipped
            // answer and outranks the rule. Imperius's wings are exactly that
            // case: `Legacy.fx`, and their type 6 MULTIPLIES the chain.
            if (kind == D3SlotKind::Emissive && s.pass.resolved && !s.pass.stageArgs &&
                !s.pass.glowLights)
                continue;
            // 12/14/19 are alpha masks only where they sit BESIDE a base map.
            // A pass that declares one of them and no type 1 is a Legacy-family
            // pass using its own numbering, where the same id is the base map —
            // and multiplying a base map's alpha into the surface would fade it
            // for no reason. Measured: 147 mask entries land in a pass that also
            // declares type 1, 13 in one that does not.
            if (D3SlotIsAlphaMask(kind) && s.pass.resolved && !s.pass.stageArgs &&
                (s.pass.declaredTypes & D3TypeBit(1)) == 0)
                continue;

            D3Slot& slot = s.slots[static_cast<u32>(kind)];
            if (slot.textureId >= 0)
                continue; // a type never repeats inside a material, so this is a re-entry
            slot.rawType = type;
            slot.textureId = TextureIdOf(textures, entry.snoTexture.id);
            // Which channels the surface takes from this sample. Stated only by
            // a pass that carries the stage block; zero everywhere else, and
            // the shader keeps the slot's family default.
            if (s.pass.stageArgs) {
                slot.channels = static_cast<u8>(
                    ((s.pass.colorTypes & D3TypeBit(type)) != 0 ? kD3ChannelRgb : 0) |
                    ((s.pass.alphaTypes & D3TypeBit(type)) != 0 ? kD3ChannelAlpha : 0));
            }
            // UV set 0 always. The two candidate selectors both turned out to be
            // something else — the field at 0x0C is the transform mode and the
            // one at 0x98 a flags word — and nothing recovered picks a set.
            // Reading either as "set 1" would put every D3 diffuse on the wrong
            // coordinates, and D3 authors both sets on every vertex, so nothing
            // about the geometry would say so.
            slot.uvSource = 0;
            // The PASS's address modes for this stage, not the entry's flags
            // word: that word's low bits randomise the scroll phase and say
            // nothing about addressing. See D3PassState::WrapBitsFor.
            slot.wrapFlags = s.pass.WrapBitsFor(type);

            const auto uv = D3ReadUvXform(entry);
            if (uv.mode == D3UvMode::Matrix) {
                UvMatrixOf(entry, slot.uvTransform);
            } else if (uv.mode == D3UvMode::ScaleRotateScroll) {
                slot.uvTransform = D3UvMatrix(uv, 0.0f);
                if (uv.animated)
                    slot.uvTransformId = D3UvTransformId(g, kind);
            }
            // Modes 3..6 drive the coordinates from an Anim2D frame table, the
            // camera or a bone; none is reproduced, and identity is what an
            // unreproduced one has to be — 49 entries in the whole corpus.
            if (slot.textureId >= 0)
                s.valid = true;
        }
    }

    if (census) {
        census->counts.assign(typeCounts.begin(), typeCounts.end());
        census->unmatchedMaterials = unmatched;
        census->entriesPastStageCap = pastCap;
    }
    return table;
}

core::SurfaceClass D3ClassifySurface(const D3Surface& surface) {
    core::SurfaceClass c;
    c.visible = surface.valid;
    // DEPTH WRITE is the discriminator, not the blend enable.
    //
    // 1,412 of the corpus's 1,831 passes enable blending, character bodies
    // included — a shipped body pass is blend (5, 6) *and* alpha test at 192
    // *and* depth write, which is a solid draw whose blend only softens a hair
    // card's edge. Bucketing on the blend enable alone would put every D3
    // character into the sorted transparent list, where it would be ordered
    // against its own cape.
    //
    // The pass that genuinely cannot be depth-resolved is the one that says so:
    // depth write off. That is Imperius's wings and Malthael's four wing
    // layers, and not one body.
    const bool sorted = surface.alphaBlend && !surface.pass.depthWrite;
    if (sorted)
        c.blend = core::BlendClass::Transparent;
    else if (surface.alphaTestThreshold > 0.0f)
        c.blend = core::BlendClass::AlphaKey;
    else
        c.blend = core::BlendClass::Opaque;
    return c;
}

} // namespace whiteout::flakes::renderer::profiles::diablo3
