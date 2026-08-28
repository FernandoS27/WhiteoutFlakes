#include "renderer/profiles/diablo3/d3_surface_table.h"

#include "io/d3/d3_model_adapter.h"
#include "io/d3/d3_sno_cache.h"

#include <whiteout/sno/d3/native/geometry.h>

#include <algorithm>
#include <map>

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

/// The shader-variant tag that turns the light block off. Its neighbours are
/// the five per-type light counts Render_EnsureShaderVariant clamps to 16
/// (0xA0008 point, 0xA0009 spot, 0xA000A directional, 0xA000C cylindrical,
/// 0xA000D point-linear); this one sits just above them and gates the lot.
constexpr u32 kD3TagLightingEnable = 0xA000Fu;

/// The tag the back pass of a two-sided pair raises. Measured over all 1,507
/// corpus `.shd`: worth 1 on exactly 12 passes and 0 on 15, and every one of
/// the twelve is pass 1 of a `cloth_*` shader culling CCW against a pass 0
/// culling CW. It is the flip-the-normal switch, which is what the second draw
/// is for.
constexpr u32 kD3TagBackFacePass = 0xA003Du;

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
        if (x.dwUnknown00 != y.dwUnknown00 || x.dwUnknown04 != y.dwUnknown04 ||
            x.dwUnknown08 != y.dwUnknown08 || x.dwUnknown0C != y.dwUnknown0C ||
            x.dwUnknown10 != y.dwUnknown10 || x.flUnknown14 != y.flUnknown14)
            return false;
    }
    return true;
}

/// Every RenderParams field but the cull mode. Field by field rather than a
/// memcmp: `bUnknown34` is a byte between two ints, so the struct has padding
/// a comparison must not read.
bool SameRenderParamsButCull(const d3n::RenderParams& a, const d3n::RenderParams& b) {
    return a.dwUnknown04 == b.dwUnknown04 && a.dwUnknown08 == b.dwUnknown08 &&
           a.flUnknown0C == b.flUnknown0C && a.flUnknown10 == b.flUnknown10 &&
           a.dwUnknown14 == b.dwUnknown14 && a.dwUnknown18 == b.dwUnknown18 &&
           a.dwUnknown1C == b.dwUnknown1C && a.dwUnknown20 == b.dwUnknown20 &&
           a.dwUnknown24 == b.dwUnknown24 && a.dwUnknown28 == b.dwUnknown28 &&
           a.dwUnknown2C == b.dwUnknown2C && a.dwUnknown30 == b.dwUnknown30 &&
           a.bUnknown34 == b.bUnknown34 && a.dwUnknown38 == b.dwUnknown38 &&
           a.dwUnknown3C == b.dwUnknown3C && a.dwUnknown40 == b.dwUnknown40 &&
           a.dwUnknown44 == b.dwUnknown44 && a.dwUnknown48 == b.dwUnknown48 &&
           a.dwUnknown4C == b.dwUnknown4C && a.dwUnknown50 == b.dwUnknown50 &&
           a.dwUnknown54 == b.dwUnknown54 && a.dwUnknown58 == b.dwUnknown58;
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
    const u32 ca = static_cast<u32>(a.tRenderParams.dwUnknown00);
    const u32 cb = static_cast<u32>(b.tRenderParams.dwUnknown00);
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
    D3PassState st;
    if (!cache || !variant.tMaterial.snoShaderMap.valid())
        return st;
    const auto map = cache->ShaderMap(variant.tMaterial.snoShaderMap.id);
    if (!map)
        return st;
    const i32 shadersId = ShadersIdFor(*map);
    if (shadersId < 0)
        return st;
    const auto shaders = cache->Shaders(shadersId);
    if (!shaders || shaders->arRenderPasses.empty())
        return st;

    // Pass 0. A multi-pass Shaders draws the same geometry more than once with
    // complementary colour-write masks (Imperius's wings are 1/0 then 0/1), and
    // reproducing that is a submission change, not a state one — so the first
    // pass is the one whose state this carries, and the extra passes are simply
    // not drawn.
    const auto& pass0 = shaders->arRenderPasses[0];
    const auto& r = pass0.tRenderParams;
    st.resolved = true;
    st.cull = static_cast<u32>(r.dwUnknown00);
    st.twoSidedPair = D3IsTwoSidedPassPair(*shaders);
    st.depthWrite = r.dwUnknown04 != 0;
    st.alphaRef = r.bUnknown34;
    st.blendEnable = r.dwUnknown4C != 0;
    st.blendSrc = static_cast<u32>(r.dwUnknown54);
    st.blendDst = static_cast<u32>(r.dwUnknown58);
    for (const auto& stage : pass0.arTextureStages)
        st.declaredTypes |= D3TypeBit(stage.dwUnknown00);
    st.effectFile = pass0.szEffectFile;
    // The fixed-function stage block. Present on every Legacy.fx pass and on no
    // other family this reproduces, so reading it needs no effect-file test:
    // a pass either carries the tags or it does not. See D3StageArg.
    for (u32 i = 0; i < io::kD3StageArgCount; ++i) {
        const auto* color = FindTag(pass0, io::kD3TagStageColor + i);
        const auto* alpha = FindTag(pass0, io::kD3TagStageAlpha + i);
        if (!color && !alpha)
            continue;
        st.stageArgs = true;
        // A gain can sit on a stage past the texture list -- Imperius's second
        // wing pass parks its x4 on stage 3 of a two-stage pass -- so the gains
        // are accumulated over the whole block and the channel bits only over
        // the stages that name a type.
        const auto c = D3ReadStageArg(color ? color->dwValue : 0);
        const auto a = D3ReadStageArg(alpha ? alpha->dwValue : 0);
        st.colorGain *= c.gain;
        st.alphaGain *= a.gain;
        if (i >= pass0.arTextureStages.size())
            continue;
        const i32 type = pass0.arTextureStages[i].dwUnknown00;
        if (c.modulates)
            st.colorTypes |= D3TypeBit(type);
        if (a.modulates)
            st.alphaTypes |= D3TypeBit(type);
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
            s.alphaTestThreshold = static_cast<f32>(s.pass.alphaRef) * (1.0f / 255.0f);
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
            // The entry's own address modes -- see kD3UvFlagWrapMask. Forcing
            // wrap here tiled every fixed-matrix layer in the game.
            slot.wrapFlags = static_cast<u32>(io::D3UvFlagsOf(entry) & io::kD3UvFlagWrapMask);

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
