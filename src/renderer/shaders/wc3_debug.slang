// ============================================================================
// WhiteoutFlakes Renderer — Warcraft III debug views (DEBUG_VIEW_DESIGN.md).
//
// The debug pixel programs for GeosetPassHd (HD, Crystal, SD-on-HD) and
// GeosetPassBls (SD classic). Each pairs with the retail VERTEX permutation the
// draw already selected, so the inputs are hd_ps's HDPSInput and
// sd_classic_ps's SDPSInput exactly, and every decode — normal map, team fold,
// ORM remap, lighting — is imported from Wc3Shaders rather than restated.
//
// Compiled with -fvk-use-dx-layout and Wc3Shaders on the include path, like
// the BLS build compiles the programs they stand in for.
// ============================================================================

import wc3_shaders;
import debug_view;

// PS b3 is free in every 3.0.0 mesh program (hd_ps reads b1/b2,
// sd_classic_ps b0); 2.0.0's DebugVisCB sat here.
[[vk::binding(PsCbBindingOffset + 3, 0)]] ConstantBuffer<DebugViewData> wc3Debug : register(b3);

// DebugViewData::ctl.w, per draw.
static const uint kWc3DbgPbr = 1u;        // HD or Crystal program; else SD-on-HD
static const uint kWc3DbgTeamLayer = 2u;  // the multi-layer (team colour) material
static const uint kWc3DbgAoMap = 4u;      // ORM.x at UV1 is a real occlusion map
static const uint kWc3DbgLit = 8u;        // the layer is not unshaded
static const uint kWc3DbgAlphaTest = 16u; // the real program's ALPHA_TEST axis

// hd_ps_lighting.slang's hdLookupCluster, light count only. It is `static`
// there and Wc3Shaders is not ours to change, so the lookup is copied.
uint Wc3DbgClusterLightCount(float2 svPositionXY)
{
    float2 tile01 = saturate((svPositionXY - hdPsCB1.viewportRect.xy)
                             / (hdPsCB1.viewportRect.zw - hdPsCB1.viewportRect.xy));
    uint2 tile = (uint2)(tile01 / hdPsCB1.invClusterDims);

    int stride = asint(hdPsCB1.gridStride);
    int index = (int)tile.y * stride + (int)tile.x;
    int maxIndex = stride * asint(hdPsCB1.gridRows) - 1;
    index = (int)min((uint)index, (uint)maxIndex);
    index = index + asint(hdPsCB1.clusterBase);

    uint record = t_hdClusters[(uint)index];
    return record & 1023;
}

// The lit result for a lighting view: hd_ps's lit branch over an overridden
// base colour and ORM, without emissive or the vertex tint, so what remains is
// what light does to the surface.
float3 Wc3DbgLit<let HAS_SHADOW_CASCADE : bool, let HAS_POINT_SHADOWS : bool>(
    HDPSInput input, HDSurface surf, float ao)
{
    float mainShadow = 1;
    if (HAS_SHADOW_CASCADE)
        mainShadow = hdMainLightShadow<false>(input.worldPos);
    float3 lit = float3(0, 0, 0);
    if (asint(hdPsCB2.mainLightEnable) != 0)
        lit = hdMainLight(surf, input.viewPos.xyz, mainShadow);
    lit = hdClusterLights<HAS_POINT_SHADOWS, false>(surf, input.viewPos.xyz, input.worldPos.xyz,
                                                    input.svPosition.xy, lit);
    return PI * lit * ao;
}

float4 Wc3HdDebug<let HAS_SHADOW_CASCADE : bool, let HAS_POINT_SHADOWS : bool>(
    HDPSInput input, bool isFrontFace)
{
    let d = wc3Debug;
    let flags = d.ctl.w;
    let uv = input.uv.xy;
    let pbr = (flags & kWc3DbgPbr) != 0u;
    let team = (flags & kWc3DbgTeamLayer) != 0u;

    DebugSurface s = DebugSurfaceInit();
    uint aw, ah;
    t_hdAlbedo.GetDimensions(aw, ah);
    s.mipLevel = DebugMipLevel(uv, float2(aw, ah));

    let V = normalize(-input.viewPos.xyz);
    // hd_vs hands over VIEW-space normals under the WS name (the shading and
    // the G-buffer both want them there); the views show world space, like
    // every other product's.
    let toWorld = (float3x3)hdPsCB2.invView;
    let vertexN = orientNormal(normalize(input.normalWS), isFrontFace);
    s.vertexNormalWS = normalize(mul(toWorld, vertexN));
    s.vertexColor = input.vertColor.xyz;
    s.lightCount = (float)Wc3DbgClusterLightCount(input.svPosition.xy);
    s.present = kDbgHasMip | kDbgHasVertexNormal | kDbgHasVertexColor | kDbgHasLightCount |
                kDbgHasAlbedo | kDbgHasNormal | kDbgHasLit;

    float3 N;
    float ndv;
    HDSurface surf;
    float ao = 1;
    if (pbr)
    {
        // hd_ps steps 1 and 4, both material variants: which one runs is the
        // real program's MULTI_LAYER axis.
        HDMaterial mat = team ? sampleHDMaterial<MultiLayerMaterial, true>(uv)
                              : sampleHDMaterial<StandardMaterial, true>(uv);
        float2 enc = encodeScaledNormal(t_hdNormal.Sample(s_hdNormal, uv).xyw,
                                        hdPsCB2.normalStrength);
        float3 tsN = decodeEncodedNormal(enc);
        // tangentToWorld renormalises; 2.0.0's normal view also showed how far
        // the basis was from unit length, so rebuild the sum it normalises.
        float3 bt = (input.normalWS.yzx * input.tangentWS.zxy -
                     input.tangentWS.yzx * input.normalWS.zxy) * input.tangentWS.w;
        s.normalLen = length(tsN.x * input.tangentWS.xyz + tsN.y * bt + tsN.z * input.normalWS);
        N = hdWorldNormal(input, enc, isFrontFace);
        ndv = saturate(dot(N, V));

        if ((flags & kWc3DbgAoMap) != 0u)
        {
            ao = t_hdOrm.Sample(s_hdOrm, input.uv.zw).x;
            s.materialAo = ao;
            s.present |= kDbgHasMaterialAo;
        }

        s.albedo = mat.albedo.xyz;
        s.emissive = mat.emissive * hdPsCB2.emissiveGain;
        s.roughness = saturate(mat.roughMetal.x) * HD_ROUGHNESS_SCALE + HD_ROUGHNESS_BIAS;
        s.metalness = mat.roughMetal.y;
        s.specular = mat.roughMetal.y * mat.albedo.xyz;
        s.gloss = 1.0 - s.roughness;
        s.present |= kDbgHasEmissive | kDbgHasRoughness | kDbgHasMetalness | kDbgHasSpecular |
                     kDbgHasGloss;
        if (team)
        {
            s.teamMask = mat.teamWeight;
            s.present |= kDbgHasTeamMask;
        }

        let fresnel = team ? hdFresnelAndCloak<MultiLayerMaterial>(float3(0, 0, 0), ndv, mat,
                                                                   input.vertColor.w)
                           : hdFresnelAndCloak<StandardMaterial>(float3(0, 0, 0), ndv, mat,
                                                                 input.vertColor.w);
        s.opacity = fresnel.w;

        let lighting = DebugLightingOverride(d);
        if (lighting != 0u)
        {
            let base = DebugOverrideAlbedo(d, mat.albedo.xyz);
            let orm = (lighting == 4u) ? float2(1, 0) : mat.roughMetal;
            surf = hdBuildSurface(N, V, base, orm);
            if ((flags & kWc3DbgLit) != 0u)
                s.lit = Wc3DbgLit<HAS_SHADOW_CASCADE, HAS_POINT_SHADOWS>(
                    input, surf, (lighting == 4u) ? 1.0 : ao);
            else
                s.lit = base;
        }
    }
    else
    {
        // sd_on_hd_ps: one map, the geometric normal, a fixed rough dielectric.
        let albedo = t_hdAlbedo.Sample(s_hdAlbedo, uv);
        N = vertexN;
        ndv = saturate(dot(N, V));
        s.albedo = albedo.xyz;
        s.opacity = sdOnHdCloakAlpha(ndv, albedo.w, input.vertColor.w);

        let lighting = DebugLightingOverride(d);
        if (lighting != 0u)
        {
            let base = DebugOverrideAlbedo(d, albedo.xyz);
            surf = sdOnHdSurface(base, N, V);
            s.lit = ((flags & kWc3DbgLit) != 0u)
                        ? Wc3DbgLit<HAS_SHADOW_CASCADE, HAS_POINT_SHADOWS>(input, surf, 1.0)
                        : base;
        }
    }
    s.normalWS = mul(toWorld, N);

    if ((flags & kWc3DbgAlphaTest) != 0u && s.opacity - hdPsCB2.alphaRef < 0)
        discard;
    // Water clipping, as the real program does it.
    if (input.viewPos.w < 0)
        discard;

    return pbr ? DebugPbr(s, d, input.svPosition.xy) : DebugLegacy(s, d, input.svPosition.xy);
}

// Channel views, and lighting views in a frame without shadow maps. The shadow
// variants exist because a program may only name a shadow map the pass bound:
// Vulkan leaves an unbound descriptor unwritten, and the retail program keys
// the same choice on its SHADOW_CASCADE / POINT_SHADOWS axes.
[shader("fragment")]
float4 psWc3HdDebug(HDPSInput input, bool isFrontFace : SV_IsFrontFace) : SV_Target
{
    return Wc3HdDebug<false, false>(input, isFrontFace);
}

[shader("fragment")]
float4 psWc3HdDebugCsm(HDPSInput input, bool isFrontFace : SV_IsFrontFace) : SV_Target
{
    return Wc3HdDebug<true, false>(input, isFrontFace);
}

[shader("fragment")]
float4 psWc3HdDebugCube(HDPSInput input, bool isFrontFace : SV_IsFrontFace) : SV_Target
{
    return Wc3HdDebug<false, true>(input, isFrontFace);
}

[shader("fragment")]
float4 psWc3HdDebugCsmCube(HDPSInput input, bool isFrontFace : SV_IsFrontFace) : SV_Target
{
    return Wc3HdDebug<true, true>(input, isFrontFace);
}

// SD classic. Lighting is baked into the vertex colour by the retail VS, and the
// VS hands over no normal — the normal views draw through the HD entry instead.
[shader("fragment")]
float4 psWc3SdDebug(SDPSInput input) : SV_Target
{
    let d = wc3Debug;
    let uv = input.uv.xy;

    DebugSurface s = DebugSurfaceInit();
    uint aw, ah;
    t_albedo.GetDimensions(aw, ah);
    s.mipLevel = DebugMipLevel(uv, float2(aw, ah));

    let tex0 = t_albedo.Sample(s_albedo, uv);
    s.albedo = tex0.xyz;
    // Stage 0 modulates on every SD mesh layer the permuter selects.
    s.opacity = saturate(input.vertColor.w * tex0.w);
    s.lightCount = (float)d.ctl.z;
    s.present = kDbgHasMip | kDbgHasAlbedo | kDbgHasLightCount | kDbgHasLit;

    let lighting = DebugLightingOverride(d);
    if (lighting == 1u || lighting == 2u)
        s.lit = DebugOverrideAlbedo(d, float3(1, 1, 1)) * input.vertColor.xyz;

    if ((d.ctl.w & kWc3DbgAlphaTest) != 0u && s.opacity - sdClassicPsCB0.alphaRef < 0)
        discard;

    return DebugLegacy(s, d, input.svPosition.xy);
}

// ---- SD classic normal views ----------------------------------------------
//
// sd_highspec_vs lights per vertex and hands the pixel stage no normal, so the
// two normal views draw SD geometry through this vertex stage instead. It reads
// the HD per-draw bank (b2) and the palette SD already binds at b3, so the host
// fills nothing new.

struct Wc3SdDebugVSIn
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD;
};

struct Wc3SdDebugVSInSkinned
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD;
    float4 weights  : BLENDWEIGHT;
    uint4  indices  : BLENDINDICES;
};

struct Wc3SdDebugVSOut
{
    float4 svPosition : SV_Position;
    float4 vertColor  : COLOR0;
    float2 uv         : TEXCOORD0;
    float3 normalWS   : TEXCOORD1;
};

Wc3SdDebugVSOut Wc3SdDebugVertex(float3 pos, float3 nrm, float4 color, float2 uv)
{
    Wc3SdDebugVSOut o;
    o.svPosition = mul(hdVsCB2.worldViewProj, float4(pos, 1.0));
    o.normalWS = mul((float3x3)hdVsCB2.world, nrm);
    o.uv = transformUV(uv, hdVsCB2.texMtx0Row0, hdVsCB2.texMtx0Row1);
    o.vertColor = color * hdVsCB2.diffuseColor;
    return o;
}

[shader("vertex")]
Wc3SdDebugVSOut vsWc3SdDebug(Wc3SdDebugVSIn input)
{
    return Wc3SdDebugVertex(input.position, input.normal, input.color, input.uv);
}

[shader("vertex")]
Wc3SdDebugVSOut vsWc3SdDebugSkinned(Wc3SdDebugVSInSkinned input)
{
    float3 pos = float3(0, 0, 0);
    float3 nrm = float3(0, 0, 0);
    let p = float4(input.position, 1.0);
    let n = float4(input.normal, 0.0);
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        let b = hdVsCB3.bones[input.indices[i]];
        let w = input.weights[i];
        pos += w * float3(dot(b.row0, p), dot(b.row1, p), dot(b.row2, p));
        nrm += w * float3(dot(b.row0, n), dot(b.row1, n), dot(b.row2, n));
    }
    return Wc3SdDebugVertex(pos, nrm, input.color, input.uv);
}

[shader("fragment")]
float4 psWc3SdDebugNormal(Wc3SdDebugVSOut input, bool isFrontFace : SV_IsFrontFace) : SV_Target
{
    let d = wc3Debug;
    let tex0 = t_albedo.Sample(s_albedo, input.uv);
    DebugSurface s = DebugSurfaceInit();
    let n = normalize(input.normalWS);
    s.normalWS = isFrontFace ? n : -n;
    s.vertexNormalWS = s.normalWS;
    s.opacity = saturate(input.vertColor.w * tex0.w);
    s.present = kDbgHasNormal | kDbgHasVertexNormal;
    if ((d.ctl.w & kWc3DbgAlphaTest) != 0u && s.opacity - sdClassicPsCB0.alphaRef < 0)
        discard;
    return DebugLegacy(s, d, input.svPosition.xy);
}
