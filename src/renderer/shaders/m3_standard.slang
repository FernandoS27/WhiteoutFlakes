// ============================================================================
// WhiteoutFlakes Renderer — M3 simple material (StarCraft II / HotS `.m3`).
//
// The shader half of M3StandardShading (M3_SIMPLE_MATERIAL_DESIGN.md). NOT the
// retail permutation machine: nine of StandardMaterial's eighteen layers
// (diffuse, decal, specular, both emissives, normal, both alpha masks, gloss,
// the environment map and its mask), each through its own animated UV
// transform and fresnel term,
// the retail lighting model (lighting.fx: shadowed Blinn key light + ambient +
// two diffuse-only fills, spec gated by sign(N.L)), the retail team-colour paths
// (psmateriallayer.fx: diffuse-alpha mask lerp + the TeamColor*Add combine
// ops), and the G-buffer sidecar the deferred local-light pass reads. The full
// retail-BLS system replaces this draw-for-draw behind the same surface table;
// this stays as its no-bundle fallback.
//
// Alpha follows psmaterial.fx:354-390: the material alpha is the alpha-mask
// layers times the alpha factor — the DIFFUSE alpha never reaches it (it is
// the team-colour mask, which is why using it as coverage tinted every unit's
// insignia instead of colouring it).
//
// An *engine* shader on the embedded slangc path, for m2_combiners' reason:
// `bls::PsoRequest` needs a `const BlsProgram*`, and producing a new `.bls`
// bundle needs WDX_BUILD_WC3_SHADERS — mutually exclusive with the
// WDX_USE_PREBUILT_SHADERS every fresh clone builds with.
//
// Layer presence is a uniform branch (`layerCtl[i].z`), not a permutation:
// m2_combiners' argument verbatim — a 2^6 matrix in compiled_shaders.h would
// buy back a few scalar selects. The one compile-time split is forward vs MRT,
// because Vulkan will not tolerate a PS declaring outputs the pass never
// binds — the same split WC3 keys as WC3_IS_MRT.
//
// G-buffer conventions match what GTAO already reads (gtao.slang): the normal
// MRT is VIEW-space packed *0.5+0.5 and faces the camera (z <= 0), LinearDepth
// is real view-space Z in scene units. The sidecar packs into the 4-attachment
// ceiling (gfx::kMaxColorAttachments, the WebGPU guaranteed minimum) the way
// the real engine does: Normal.w carries the Blinn exponent as sqrt(p/512) —
// our own encoding with our own decoder, so 8 bits suffice — and the diffuse
// target's alpha carries a mono specular intensity for the local-light pass;
// only the forward key light sees the full RGB specular colour.
// ============================================================================

// ---- Constant buffers ------------------------------------------------------
//
// Split by update frequency, both bound to both stages (Vulkan and WebGPU
// build one descriptor set from the shared layout — see m2_combiners.slang).

// The light rig is the recovered SC2 layout (core.sc2mod lightdata.xml): one
// shadowed key directional with its own specular colour, two diffuse-only
// fills ("Fill"/"Back"), and a flat ambient. lighting.fx applies the key's
// specular ONLY — fill SpecularColor exists in the data and the shader never
// reads it.
struct M3PassData
{
    float4x4 view;
    float4x4 projection;
    float4 cameraPosWS;      // .xyz eye in world space
    float4 keyLightDir;      // .xyz direction the light TRAVELS, normalised
    float4 keyLightDiffuse;  // .rgb colour x multiplier
    float4 keyLightSpecular; // .rgb SpecularColor x SpecColorMultiplier (x rig HDR spec)
    float4 ambient;          // .rgb; .w = rig emissive multiplier (HDREmisMultiplier)
    float4 fillLightDir;     // .xyz direction the light travels
    float4 fillLightDiffuse; // .rgb; a zero colour disables the light
    float4 backLightDir;     // .xyz
    float4 backLightDiffuse; // .rgb
    // p_fTeamColorIntensity: .x diffuse mask exponent, .y emissive mask
    // exponent. Engine-set with no recovered data source; (1,1) is neutral and
    // shipped masks are near-binary, so the exponent barely bites.
    float4 teamParams;
}

// Layer slots, in M3Surface order. Absent layers branch off on ctl.z and
// additionally bind the white default, belt and braces. Emissive2 is its own
// slot because retail pairs a team-mask emissive1 (TeamColor*Add) with the
// real glow in emissive2; AlphaMask2 is its own because retail multiplies both
// masks into the coverage rather than choosing between them.
static const int kM3LayerDiffuse = 0;
static const int kM3LayerDecal = 1;
static const int kM3LayerSpecular = 2;
static const int kM3LayerEmissive = 3;
static const int kM3LayerEmissive2 = 4;
static const int kM3LayerNormal = 5;
static const int kM3LayerAlphaMask = 6;
static const int kM3LayerGloss = 7;
static const int kM3LayerAlphaMask2 = 8;
static const int kM3LayerEnvironment = 9;
static const int kM3LayerEnvMask = 10;
static const int kM3LayerCount = 11;

struct M3DrawData
{
    float4x4 world;
    // .x alphaRef (0 = no test), .y unshaded, .z specular exponent (the
    // material constant; a gloss layer modulates it per pixel),
    // .w flag bits: 1 = DoubleLambert (N.L remapped to 2*N.L - 1),
    //               2 = TwoSided (back faces flip the shading normal),
    //               4 = the energy-conserving specular dim is NOT folded into
    //                   the tint and must be evaluated per pixel,
    //               8 = the environment lookup reflects the view vector,
    //              16 = the environment cube is read at a mip biased by
    //                   (1 - gloss): SimulateRoughness beside a gloss layer.
    float4 params0;
    // `.m3` UVs are i16/2048 declared R16G16B16A16_SNORM, so the sampled value
    // is raw/32767 and the true coordinate is sample * (32767 * uvMultiply) +
    // uvOffset, where REGN v5+ carries per-region uvMultiply/uvOffset and older
    // regions imply 1/2048 and 0. The host folds the 32767 in; .x mul, .y add.
    // .z = material hdrEmissiveMultiplier (psmaterial.fx applies it to the
    // Add-family emissive sum, team adds included, so it cannot ride a tint).
    // .w = AlphaFactor (psmaterial.fx:358) — the per-draw coverage the alpha
    // mask layers multiply into, carrying the composite section multiplier.
    float4 uvTransform;
    // The instance's palette pair (gamedata.xml <TeamColors>): p_cDiffuseTeamColor
    // and p_cEmissiveTeamColor. .rgb only.
    float4 teamDiffuse;
    float4 teamEmissive;
    // .rgb = layer colour tint x rgbMultiply (the specular tint also folds
    // hdrSpecularMultiplier and the energy-conserving dim), .a = layer alpha.
    float4 layerTint[kM3LayerCount];
    // .x = rgbAdd, the additive half of psmateriallayer.fx's
    // `cResult.rgba = cResult.rgba * multiply + add`. Its own array because an
    // HLSL cbuffer array strides by 16 bytes whatever the element type, so
    // packing it into a spare component of another array would save nothing.
    // .y on the environment slot = the blur's mip range (the cube's last
    // level), read when params0.w bit 16 is set; on the specular slot = the
    // extra multiplier folded into its tint, which the team lerp needs alone.
    float4 layerAdd[kM3LayerCount];
    // .x = uv set (0/1) in the low nibble, sampler wrap variant (0..3, the
    //      layer's UVWrapX/Y bits) in bits 4-5, ColorInvert in bit 6 and
    //      ColorClamp in bit 7;
    // .y ColorChannelSelect; .z mode (0 off, 1 texture, 2 solid colour —
    // TextureLayerFlag 0x400); .w = LayerBlendOp for the decal and emissive
    // slots, TEAMCOLOR_DIFFUSE mode (1) for the diffuse slot.
    uint4 layerCtl[kM3LayerCount];
    // p_m<L>UVTransform, the layer's animated 2x4 — psmateriallayer.fx feeds
    // it (u, v, 0, 1) and keeps .xy, so the third column and row can never
    // reach the output and only these two rows exist. .xy is the linear pair,
    // .z the translation, .w unused. Identity on 95% of shipped layers.
    float4 layerUvRow0[kM3LayerCount];
    float4 layerUvRow1[kM3LayerCount];
    // CalcFresnelTerm's constants (psmateriallayer.fx).
    //   layerFresnel      .xyz p_v<L>FresnelExponentBiasScale, .w FresnelMode
    //                     (0 off, 1 standard, 2 inverted)
    //   layerFresnelMask  .xyz the transform's per-axis mask, .w != 0 = apply
    //   layerFresnelTrans .xyz its translation, .w != 0 = re-normalise after
    float4 layerFresnel[kM3LayerCount];
    float4 layerFresnelMask[kM3LayerCount];
    float4 layerFresnelTrans[kM3LayerCount];
}

struct M3BoneData
{
    float4 rows[768]; // 256 bones x 3 transposed rows, bls::PackBone's layout.
}

// Per-backend binding shifts — see m2_combiners.slang for why these must be
// stated: left to itself Slang co-locates textures with the CBs in set 0,
// where nothing ever writes them.
#if defined(WGSL_TARGET) || defined(METAL_TARGET)
static const int WF_PS_BIND_OFFSET = 12;
#else
static const int WF_PS_BIND_OFFSET = 16;
#endif

[[vk::binding(0, 0)]] ConstantBuffer<M3PassData> pass : register(b0);
[[vk::binding(1, 0)]] ConstantBuffer<M3DrawData> draw : register(b1);
// Vertex stage only; the rigid entry never references it and Slang strips it
// from that entry's reflection.
[[vk::binding(2, 0)]] ConstantBuffer<M3BoneData> bones : register(b2);

[[vk::binding(WF_PS_BIND_OFFSET + 0, 1)]] Texture2D texDiffuse : register(t0);
[[vk::binding(WF_PS_BIND_OFFSET + 1, 1)]] Texture2D texDecal : register(t1);
[[vk::binding(WF_PS_BIND_OFFSET + 2, 1)]] Texture2D texSpecular : register(t2);
[[vk::binding(WF_PS_BIND_OFFSET + 3, 1)]] Texture2D texEmissive : register(t3);
[[vk::binding(WF_PS_BIND_OFFSET + 4, 1)]] Texture2D texEmissive2 : register(t4);
[[vk::binding(WF_PS_BIND_OFFSET + 5, 1)]] Texture2D texNormal : register(t5);
[[vk::binding(WF_PS_BIND_OFFSET + 6, 1)]] Texture2D texAlphaMask : register(t6);
[[vk::binding(WF_PS_BIND_OFFSET + 7, 1)]] Texture2D texGloss : register(t7);
[[vk::binding(WF_PS_BIND_OFFSET + 8, 1)]] Texture2D texAlphaMask2 : register(t8);
[[vk::binding(WF_PS_BIND_OFFSET + 9, 1)]] Texture2D texEnvMask : register(t9);
// The environment map, and the one cube in this shader. t13 rather than the
// next free register because the shared SRV bind-group layout types only
// t13/t14 as cube arrays — that layout is a slot-TYPE map, and the WC3 HD IBL
// probe that also lives at t13 never draws in the same frame as an `.m3`.
// CubeArray rather than Cube because D3D builds TEXTURECUBEARRAY SRVs for
// every cube; a lone cube is a one-element array on all four backends.
[[vk::binding(WF_PS_BIND_OFFSET + 13, 1)]] TextureCubeArray<float4> texEnv : register(t13);
// One sampler PER WRAP VARIANT, not per layer: the D3D12 root signature (and
// its siblings) budget four samplers per stage, and four is also exactly how
// many distinct states exist — wrap/clamp x U/V. Slot = the layer's wrap bits.
[[vk::binding(WF_PS_BIND_OFFSET + 0, 2)]] SamplerState sampWrap0 : register(s0);
[[vk::binding(WF_PS_BIND_OFFSET + 1, 2)]] SamplerState sampWrap1 : register(s1);
[[vk::binding(WF_PS_BIND_OFFSET + 2, 2)]] SamplerState sampWrap2 : register(s2);
[[vk::binding(WF_PS_BIND_OFFSET + 3, 2)]] SamplerState sampWrap3 : register(s3);

// ---- Vertex side -----------------------------------------------------------

// The `.m3` record as the adapter describes it. Normal and tangent arrive
// UNORM (retail's `ubyte4n`) and decode `2 * v - 1`; the bitangent handedness
// is normal.w, not tangent.w (which is 255 on every shipped vertex) — see
// vsmodelvertexformat.fx TranslateVert and model.fx EmitModelBinormal. Both
// UV sets ride one float4 for the TEXCOORD1 -> TEXCOORD10 toolchain reason
// m2_combiners.slang documents.
struct VSInput
{
    float3 position    : POSITION;
    float4 boneWeights : BLENDWEIGHT;  // R8G8B8A8_UNORM
    uint4  boneIndices : BLENDINDICES; // R8G8B8A8_UINT
    float4 normal      : NORMAL;       // R8G8B8A8_UNORM: .xyz normal, .w handedness
    float4 uvs         : TEXCOORD0;    // .xy set 0, .zw set 1 (SNORM raw)
    float4 tangent     : TANGENT;      // R8G8B8A8_UNORM: .xyz tangent, .w unused
}

struct VSInputRigid
{
    float3 position : POSITION;
    float4 normal   : NORMAL;
    float4 uvs      : TEXCOORD0;
    float4 tangent  : TANGENT;
}

// One interstage signature for both PS entries, so either VS pairs with
// either PS.
struct PSInput
{
    float4 position  : SV_POSITION;
    float4 uv01      : TEXCOORD0; // decoded UV set 0 (.xy) and set 1 (.zw)
    float4 worldPos  : TEXCOORD1; // .xyz world position, .w view-space Z
    float3 normalWS  : TEXCOORD2;
    float4 tangentWS : TEXCOORD3; // .xyz tangent, .w handedness sign
    // Per-vertex modulation. 1 on every geoset (M3Vertex writes it), the
    // ribbon's interpolated element rgba on the ribbon VS — Ribbon.fx multiplies
    // it into the material output, and the forward PS does the same.
    float4 vColor    : COLOR0;
}

float3 M3DecodeBasis(float3 unorm)
{
    return unorm * 2.0 - 1.0;
}

float M3Handedness(float4 normal)
{
    return (normal.w < 0.5) ? -1.0 : 1.0;
}

// Model space to animated model space, m2_combiners' M2Skin with the tangent
// along for the ride. Weights are normalised at pack time; unused lanes carry
// weight 0 and index 0, so every read is in range.
void M3Skin(VSInput input, out float3 pos, out float3 nrm, out float3 tan)
{
    pos = float3(0.0, 0.0, 0.0);
    nrm = float3(0.0, 0.0, 0.0);
    tan = float3(0.0, 0.0, 0.0);
    let p = float4(input.position, 1.0);
    let n = float4(M3DecodeBasis(input.normal.xyz), 0.0);
    let t = float4(M3DecodeBasis(input.tangent.xyz), 0.0);
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        let w = input.boneWeights[i];
        let b = input.boneIndices[i] * 3;
        let r0 = bones.rows[b + 0];
        let r1 = bones.rows[b + 1];
        let r2 = bones.rows[b + 2];
        pos += w * float3(dot(p, r0), dot(p, r1), dot(p, r2));
        nrm += w * float3(dot(n, r0), dot(n, r1), dot(n, r2));
        tan += w * float3(dot(t, r0), dot(t, r1), dot(t, r2));
    }
}

PSInput M3Vertex(float3 pos, float3 nrm, float3 tan, float tanSign, float4 rawUvs)
{
    PSInput o;
    let worldPos = mul(float4(pos, 1.0), draw.world);
    let viewPos = mul(worldPos, pass.view);
    o.position = mul(viewPos, pass.projection);
    o.worldPos = float4(worldPos.xyz, viewPos.z);
    // Uniform scale only in the world transform, so no inverse transpose.
    //
    // No negation: decoded UNORM, the `.m3` normal agrees with its triangle
    // winding 96% of the time, the same as `.m2` (normal_orientation_test).
    // The negation this used to carry was compensating for the SNORM misread.
    o.normalWS = normalize(mul(float4(nrm, 0.0), draw.world).xyz);
    o.tangentWS = float4(normalize(mul(float4(tan, 0.0), draw.world).xyz), tanSign);
    o.uv01 = rawUvs * draw.uvTransform.x + draw.uvTransform.y;
    o.vColor = float4(1.0, 1.0, 1.0, 1.0);
    return o;
}

[shader("vertex")]
PSInput vsM3Standard(VSInputRigid input)
{
    return M3Vertex(input.position, M3DecodeBasis(input.normal.xyz),
                    M3DecodeBasis(input.tangent.xyz), M3Handedness(input.normal), input.uvs);
}

[shader("vertex")]
PSInput vsM3StandardSkinned(VSInput input)
{
    float3 pos, nrm, tan;
    M3Skin(input, pos, nrm, tan);
    return M3Vertex(pos, nrm, tan, M3Handedness(input.normal), input.uvs);
}

// ---- Pixel side ------------------------------------------------------------

// ColorChannelSelect. RGB forces alpha to 1; the single-channel modes splat,
// which is what lets one helper serve colour layers and mask layers alike.
float4 SelectChannels(float4 t, uint sel)
{
    switch (sel)
    {
    case 1u: return t;                            // RGBA
    case 2u: return float4(t.a, t.a, t.a, t.a);   // Alpha
    case 3u: return float4(t.r, t.r, t.r, t.r);   // Red
    case 4u: return float4(t.g, t.g, t.g, t.g);   // Green
    case 5u: return float4(t.b, t.b, t.b, t.b);   // Blue
    default: return float4(t.rgb, 1.0);           // RGB
    }
}

// One layer's contribution: channel select, solid-colour override, team
// colour, tint. Callers branch on ctl.z != 0 themselves so an absent layer
// costs nothing.
//
// The team-colour lerp is psmateriallayer.fx:213-215 verbatim: gated on
// ChannelSelect != RGB, keyed on the RAW sampled alpha (pre-select, pre-tint),
// and it forces the layer alpha to 1. Only the diffuse slot carries mode 1 —
// emissive team colour arrives through the TeamColor*Add combine ops instead.
float4 M3LayerValue(uint slot, float4 texel, float3 n, float3 toEye)
{
    let ctl = draw.layerCtl[slot];
    float4 v = SelectChannels(texel, ctl.y);
    if (ctl.z == 2u)
        v = float4(1.0, 1.0, 1.0, 1.0);
    if (ctl.w == 1u && ctl.y != 0u)
    {
        let mask = saturate(pow(max(texel.a, 0.0), pass.teamParams.x));
        v = float4(lerp(draw.teamDiffuse.rgb, v.rgb, mask), 1.0);
    }
    // psmateriallayer.fx ComputeLayerColorInternal, its last four steps:
    // ColorInvert, then `rgba * multiply + add`, then ColorClamp, then the
    // fresnel term scaling the whole thing. `add` is not cosmetic — the
    // environment layer that ships with the golden Adept is authored (x1.5,
    // +0.4) under a Mod op, so dropping the +0.4 multiplied the whole model
    // down to bronze.
    if ((ctl.x & 0x40u) != 0u)
        v = float4(1.0, 1.0, 1.0, 1.0) - v;
    v = v * draw.layerTint[slot] + draw.layerAdd[slot].x;
    if ((ctl.x & 0x80u) != 0u)
        v = saturate(v);
    if (draw.layerFresnel[slot].w != 0.0)
        v *= M3FresnelTerm(slot, n, toEye);
    return v;
}

// The layer's UV set, through its own UV transform — pscommon.fx GetUVEmitter,
// which is where a scrolling texture actually scrolls.
float2 M3LayerUV(uint slot, float4 uv01)
{
    let uv = ((draw.layerCtl[slot].x & 0xFu) == 1u) ? uv01.zw : uv01.xy;
    let p = float3(uv, 1.0);
    return float2(dot(p, draw.layerUvRow0[slot].xyz), dot(p, draw.layerUvRow1[slot].xyz));
}

// Sample @p t with the layer's wrap variant. The selector is a per-draw
// constant, so the branch is uniform and the implicit gradients are safe.
float4 M3SampleLayer(Texture2D t, uint slot, float4 uv01)
{
    let uv = M3LayerUV(slot, uv01);
    switch ((draw.layerCtl[slot].x >> 4) & 0x3u)
    {
    case 1u: return t.Sample(sampWrap1, uv);
    case 2u: return t.Sample(sampWrap2, uv);
    case 3u: return t.Sample(sampWrap3, uv);
    default: return t.Sample(sampWrap0, uv);
    }
}

// psmateriallayer.fx CalcFresnelTerm. `V` is EyeToVertexFresnel, which
// model_vs.fx emits as normalize(worldPos - eyePos) — the negation of the
// view vector this shader already carries.
//
// The `abs` arm, not the `saturate(-dot)` one: b_i<Layer>FresnelSaturate has
// no identified source field in the `.m3` (see ResolveFresnel), the two agree
// on every front-facing pixel, and `abs` is what keeps a rim on the far side
// of a two-sided surface.
float M3FresnelTerm(uint slot, float3 n, float3 toEye)
{
    let p = draw.layerFresnel[slot];
    let mask = draw.layerFresnelMask[slot];
    let trans = draw.layerFresnelTrans[slot];
    float3 v = -toEye;
    if (mask.w != 0.0)
    {
        // p_m<L>FresnelTransform reduced to its diagonal and translation
        // column, which is all shipped content puts in it.
        v = v * mask.xyz + trans.xyz;
        if (trans.w != 0.0)
            v = normalize(v);
    }
    float f = 1.0 - abs(dot(n, v));
    f = pow(max(f, 0.0001), p.x);
    if (p.w == 2.0)
        f = 1.0 - f;
    return saturate(f * p.z + p.y);
}

// LayerBlendOp — psmateriallayer.fx : CombineLayerColor, all seven ops. The
// TeamColor ops add the team colour scaled by the layer's (tinted) alpha; the
// layer's own rgb never reaches them.
float3 M3ApplyBlendOp(float3 base, float4 layer, uint op)
{
    switch (op)
    {
    case 1u: return base * layer.rgb * 2.0;                 // Mod2x
    case 2u: return base + layer.rgb * layer.a;             // Add
    case 3u: return lerp(base, layer.rgb, layer.a);         // Lerp
    case 4u:                                                // TeamColorEmissiveAdd
        return base + saturate(pow(max(layer.a, 0.0), pass.teamParams.y)) *
                          draw.teamEmissive.rgb;
    case 5u: return base + layer.a * draw.teamDiffuse.rgb;  // TeamColorDiffuseAdd
    case 6u: return base + layer.rgb;                       // AddNoAlpha
    default: return base * layer.rgb;                       // Mod
    }
}

// True for the ops that fold straight into the lit colour; the Add family
// accumulates and is scaled by the emissive multipliers (psmaterial.fx:240-276).
bool M3OpIsModFamily(uint op)
{
    return op == 0u || op == 1u || op == 3u;
}

struct M3Shaded
{
    float3 color;
    float alpha;
    float3 normalWS;    // shading normal, after the map and the viewer flip
    float3 albedo;      // what the sidecar's diffuse target carries
    float3 specColor;   // what the sidecar's specular target carries
    float specPower;
};

M3Shaded ShadeM3(PSInput input, bool isFront)
{
    M3Shaded o;
    let twoSided = (uint(draw.params0.w) & 2u) != 0u;
    let toEye = normalize(pass.cameraPosWS.xyz - input.worldPos.xyz);

    // Shading normal, FIRST: every layer's fresnel term reads it, and retail
    // decodes it once at the top of mainShading for exactly that reason
    // (psmainshading.fx:139 feeds the same vDeferredNormal to lighting and to
    // MaterialColor). SC2 normal maps are DXT5nm — x rides alpha, y green — so
    // the decode is .ag, with z reconstructed.
    //
    // `nGeom` is kept because the environment layer needs the geometric normal,
    // not this one — see the env block below.
    let nGeom = normalize(input.normalWS);
    float3 n = nGeom;
    let ctlNormal = draw.layerCtl[kM3LayerNormal];
    if (ctlNormal.z == 1u)
    {
        // model.fx EmitModelBinormal: cross(normal, tangent) * sign(normal.w).
        //
        // The sample is RAW — it goes through M3SampleLayer for the UV
        // transform and NOTHING else. psmaterial.fx DecodeNormal reaches for
        // the texture directly (model_normal.slang:126 decodeTextureNormal),
        // never through ComputeLayerColorInternal, so the channel select, the
        // tint and the fresnel term all skip this slot. That is not an
        // optimisation: a normal layer's colorType is RGB on shipped content,
        // and CHANNELSELECT_RGB forces alpha to 1 — which is the x component
        // here, so routing it through the layer pipeline pins nx to +1 and
        // collapses the shading normal onto the tangent. The tangent flips
        // sign across a mirrored UV seam, so that draws a hard vertical line
        // down the centre of every SC2 character.
        let t = normalize(input.tangentWS.xyz);
        let b = cross(n, t) * input.tangentWS.w;
        float2 nxy = M3SampleLayer(texNormal, kM3LayerNormal, input.uv01).ag * 2.0 - 1.0;
        let nz = sqrt(saturate(1.0 - dot(nxy, nxy)));
        n = normalize(t * nxy.x + b * nxy.y + n * nz);
    }
    // Two-sided back faces light from behind (psmaterial.fx:468-489). Keyed on
    // the rasterizer's winding bit and gated on the material flag, exactly as
    // retail is — NOT on dot(n, toEye), which was the previous spelling. A
    // view-vector test puts the flip boundary at a camera-dependent place on
    // the surface, so shading swam across the model as the camera orbited.
    //
    // Retail negates when its FrontFace bit is set because SC2's winding makes
    // that the back side; ours makes it the front, so the test is inverted.
    if (twoSided && !isFront)
        n = -n;

    // Albedo: diffuse layer, then the decal over it. The team share the
    // diffuse's select keys on is kept: retail's specular reads it too.
    float4 albedo = float4(0.0, 0.0, 0.0, 1.0);
    float teamShare = 1.0; // fTeamColorAlpha; 1 = the art's own texel
    let ctlDiffuse = draw.layerCtl[kM3LayerDiffuse];
    if (ctlDiffuse.z != 0u)
    {
        let diffuseTexel = M3SampleLayer(texDiffuse, kM3LayerDiffuse, input.uv01);
        albedo = M3LayerValue(kM3LayerDiffuse, diffuseTexel, n, toEye);
        if (ctlDiffuse.w == 1u && ctlDiffuse.y != 0u)
            teamShare = saturate(pow(max(diffuseTexel.a, 0.0), pass.teamParams.x));
    }
    let ctlDecal = draw.layerCtl[kM3LayerDecal];
    if (ctlDecal.z != 0u)
    {
        let decal = M3LayerValue(kM3LayerDecal,
                                 M3SampleLayer(texDecal, kM3LayerDecal, input.uv01), n, toEye);
        albedo.rgb = M3ApplyBlendOp(albedo.rgb, decal, ctlDecal.w);
    }

    // Material alpha (psmaterial.fx:354-390): the alpha-mask layers times the
    // alpha factor — NOT the diffuse alpha, which is the team-colour mask.
    // BOTH masks multiply in (`*=` each, lines 380-390); a material carrying
    // the pair used to render at whichever one the slot happened to hold, so
    // it came out too opaque. Then the test; >= survives, as everywhere else
    // in this renderer.
    float alpha = draw.uvTransform.w;
    let ctlMask = draw.layerCtl[kM3LayerAlphaMask];
    if (ctlMask.z != 0u)
        alpha *= M3LayerValue(kM3LayerAlphaMask,
                              M3SampleLayer(texAlphaMask, kM3LayerAlphaMask, input.uv01), n, toEye)
                     .a;
    let ctlMask2 = draw.layerCtl[kM3LayerAlphaMask2];
    if (ctlMask2.z != 0u)
        alpha *= M3LayerValue(kM3LayerAlphaMask2,
                              M3SampleLayer(texAlphaMask2, kM3LayerAlphaMask2, input.uv01), n,
                              toEye)
                     .a;
    if (draw.params0.x > 0.0 && alpha < draw.params0.x)
        discard;

    // Specular colour: the layer's channels (the material's SpecularMode is
    // folded into ctl.y host-side), hdrSpecularMultiplier folded into the tint.
    float3 specColor = float3(0.0, 0.0, 0.0);
    let ctlSpec = draw.layerCtl[kM3LayerSpecular];
    if (ctlSpec.z != 0u)
        specColor = M3LayerValue(kM3LayerSpecular,
                                 M3SampleLayer(texSpecular, kM3LayerSpecular, input.uv01), n, toEye)
                        .rgb;
    // b_useTeamColorSpecular (psmainshading.fx; the axis defaults on): under
    // a TEAMCOLOR_DIFFUSE diffuse the specular IS the team colour where the
    // team shows, by the same share. The team takes the extra multiplier the
    // tint carries (hdrSpecularMultiplier, the folded dim) and not the
    // layer's own colour; the host parks it in the slot's add.y.
    if (ctlSpec.z != 0u && ctlDiffuse.w == 1u && ctlDiffuse.y != 0u)
        specColor = lerp(draw.teamDiffuse.rgb * draw.layerAdd[kM3LayerSpecular].y, specColor,
                         teamShare);

    // psmaterial.fx:579 MaterialSpecularity — a gloss (SpecularExponent) layer
    // scales the material's exponent by its alpha SQUARED; without one the
    // exponent is the constant. Squared, so a mid-grey gloss pixel lands at a
    // quarter of the exponent, not half.
    float specPower = draw.params0.z;
    float gloss = 1.0;
    let ctlGloss = draw.layerCtl[kM3LayerGloss];
    if (ctlGloss.z != 0u)
    {
        let g = M3LayerValue(kM3LayerGloss,
                             M3SampleLayer(texGloss, kM3LayerGloss, input.uv01), n, toEye)
                    .a;
        gloss = g;
        specPower *= g * g;
        // FakeEnergyConservingSpec (psmaterial.fx:203) reads the SPECULARITY,
        // i.e. this gloss-modulated exponent — so with a gloss layer the dim
        // varies per pixel and the host cannot fold it into the tint. Flag bit
        // 4 says it did not; without a gloss layer the fold still happens
        // there and this branch never runs.
        if ((uint(draw.params0.w) & 4u) != 0u)
        {
            let pc = clamp(specPower, 1.0, 512.0);
            specColor *= saturate(-0.000004444 * pc * pc + 0.004333 * pc + 0.0020834);
        }
    }
    specPower = max(specPower, 1.0);

    // The lighting accumulation (lighting.fx : Lighting, per-pixel arm, no
    // regions): shadowed Blinn key light, flat ambient, then the diffuse-only
    // fill lights. Shadow and ambient occlusion are 1 here — no shadow map or
    // AO layer in the simple system. Unshaded (b_useLighting == 0) is a
    // light colour of exactly 1 with no specular.
    float3 color;
    if (draw.params0.y > 0.5)
    {
        color = albedo.rgb;
        // The sidecar collapse: unshaded pixels survive the deferred pass
        // untouched because their albedo and specular read back as zero.
        o.albedo = float3(0.0, 0.0, 0.0);
        o.specColor = float3(0.0, 0.0, 0.0);
        o.specPower = 1.0;
    }
    else
    {
        let dblLambert = (uint(draw.params0.w) & 1u) != 0u;

        // Key light — lightingmodels.fx : BlinnShading with the sign(N.L)
        // gate: the highlight dies exactly where the light does, without the
        // hard `if (ndl > 0)` cliff on the DoubleLambert remap.
        float3 lightDiffuse = pass.ambient.rgb;
        float3 lightSpecular = float3(0.0, 0.0, 0.0);
        {
            let l = -pass.keyLightDir.xyz;
            float nl = dot(n, l);
            if (dblLambert)
                nl = 2.0 * nl - 1.0;
            let h = normalize(l + toEye);
            let sp = saturate(pow(saturate(dot(n, h)), specPower) * sign(nl));
            lightDiffuse += pass.keyLightDiffuse.rgb * saturate(nl);
            lightSpecular = pass.keyLightSpecular.rgb * sp;
        }
        // Fill lights (directionals 1..2): diffuse only, no specular — the
        // shipped shader never reads their SpecularColor.
        {
            let l = -pass.fillLightDir.xyz;
            float nl = dot(n, l);
            if (dblLambert)
                nl = 2.0 * nl - 1.0;
            lightDiffuse += pass.fillLightDiffuse.rgb * saturate(nl);
        }
        {
            let l = -pass.backLightDir.xyz;
            float nl = dot(n, l);
            if (dblLambert)
                nl = 2.0 * nl - 1.0;
            lightDiffuse += pass.backLightDiffuse.rgb * saturate(nl);
        }

        color = albedo.rgb * lightDiffuse + specColor * lightSpecular;
        o.albedo = albedo.rgb;
        o.specColor = specColor;
        o.specPower = specPower;
    }

    // Emissive layers (psmaterial.fx:240-276): Mod/Mod2x/Lerp fold straight
    // into the lit colour; the Add family (the TeamColor adds included)
    // accumulates and is scaled by hdrEmissiveMultiplier x the rig's emissive
    // multiplier. The shipped oddity is kept: a Mod-family emissive2 lands on
    // the accumulator instead of the colour when emissive1 already added.
    float3 cEmissive = float3(0.0, 0.0, 0.0);
    bool addFamily = false;
    let ctlEmis = draw.layerCtl[kM3LayerEmissive];
    if (ctlEmis.z != 0u)
    {
        let emis = M3LayerValue(kM3LayerEmissive,
                                M3SampleLayer(texEmissive, kM3LayerEmissive, input.uv01), n, toEye);
        if (M3OpIsModFamily(ctlEmis.w))
        {
            color = M3ApplyBlendOp(color, emis, ctlEmis.w);
        }
        else
        {
            cEmissive = M3ApplyBlendOp(cEmissive, emis, ctlEmis.w);
            addFamily = true;
        }
    }
    let ctlEmis2 = draw.layerCtl[kM3LayerEmissive2];
    if (ctlEmis2.z != 0u)
    {
        let emis = M3LayerValue(kM3LayerEmissive2,
                                M3SampleLayer(texEmissive2, kM3LayerEmissive2, input.uv01), n,
                                toEye);
        if (M3OpIsModFamily(ctlEmis2.w) && !addFamily)
        {
            color = M3ApplyBlendOp(color, emis, ctlEmis2.w);
        }
        else
        {
            cEmissive = M3ApplyBlendOp(cEmissive, emis, ctlEmis2.w);
            addFamily = true;
        }
    }
    if (addFamily)
        color += cEmissive * (draw.uvTransform.z * pass.ambient.w);

    // Environment (reflection) layer — psmaterial.fx:281-338, after emissive
    // and on top of it, which is where retail runs it.
    let ctlEnv = draw.layerCtl[kM3LayerEnvironment];
    if (ctlEnv.z != 0u)
    {
        // psuvmapping.fx GenCubicEnvio: reflect the eye-to-vertex vector about
        // the GEOMETRIC normal. Retail builds its env mappings once at the top
        // of ComputeMaterial (psmaterial.fx:155), before the normal map and
        // before the two-sided flip — so in SC2 a normal map does NOT ripple
        // the reflection, and copying `n` here would be the wrong look rather
        // than a better one. The non-Reflect mappings use the normal directly.
        //
        // p_mEnvironmentMapTransform stays identity: it is engine-set with no
        // source in the `.m3`, and our world space already IS the z-up space
        // both these cubemaps and the light rig are authored in (the rig
        // directions reach this shader unrotated for the same reason).
        //
        // The flat maps behind the Spherical mappings were projected into
        // cubes at decode time (BuildCubeFromSphereMap), so there is one
        // lookup here rather than two texture slots — which is what makes this
        // fit the shared binding layout at all.
        let envReflect = (uint(draw.params0.w) & 8u) != 0u;
        let dir = envReflect ? reflect(-toEye, nGeom) : nGeom;
        // b_iBlurEnvironmentMap (psmainshading.fx:368): under SimulateRoughness
        // the cube is read `(1 - glossiness) * range` levels down, the gloss
        // layer's alpha being the glossiness -- a rough texel reflects a blur,
        // a glossy one the mirror at mip 0.
        let envBlur = (uint(draw.params0.w) & 16u) != 0u;
        let envCoord = float4(dir, 0.0);
        let envTexel = envBlur
            ? texEnv.SampleLevel(sampWrap0, envCoord,
                                 (1.0 - gloss) * draw.layerAdd[kM3LayerEnvironment].y)
            : texEnv.Sample(sampWrap0, envCoord);
        let env = M3LayerValue(kM3LayerEnvironment, envTexel, n, toEye);

        // The mask defaults to the env layer's own alpha and the mask layer
        // multiplies into it, rather than replacing it (psmaterial.fx:320-325).
        float4 mask = float4(env.a, env.a, env.a, env.a);
        let ctlEnvMask = draw.layerCtl[kM3LayerEnvMask];
        if (ctlEnvMask.z != 0u)
        {
            float4 m = M3LayerValue(kM3LayerEnvMask,
                                    M3SampleLayer(texEnvMask, kM3LayerEnvMask, input.uv01), n,
                                    toEye);
            if (ctlEnvMask.y == 1u) // CHANNELSELECT_RGBA
                m = float4(m.rgb * m.a, m.a);
            mask = m * env.a;
        }

        // ApplyEnv (psmaterial.fx:79-88) — only these three ops do anything.
        // This is the `b_iEnvioMultipliers == false` arm: measured over 228
        // v20 materials, hdrEnvironmentDiffuse and hdrEnvironmentSpecular are
        // zero on every one, so the lighting-modulated arm is dead in shipped
        // content and its lone live term (hdrEnvironmentConstant) is folded
        // into the layer tint host-side.
        switch (ctlEnv.w)
        {
        case 0u: color = color * env.rgb * mask.rgb; break;   // Mod
        case 2u: color = color + env.rgb * mask.rgb; break;   // Add
        case 3u: color = lerp(color, env.rgb, mask.a); break; // Lerp
        default: break;
        }
    }

    o.color = color;
    o.alpha = alpha;
    o.normalWS = n;
    return o;
}

// SV_IsFrontFace on both entries even though only one is live per pass: Vulkan
// rejects two entry points that disagree about the system-value signature.
[shader("pixel")]
float4 psM3Standard(PSInput input, bool isFront : SV_IsFrontFace) : SV_Target0
{
    let s = ShadeM3(input, isFront);
    // vColor is 1 on every geoset (identical output) and the ribbon's element
    // colour on a ribbon draw.
    return float4(s.color * input.vColor.rgb, s.alpha * input.vColor.a);
}

struct MrtOut
{
    float4 color       : SV_Target0;
    float  linearDepth : SV_Target1; // view-space Z, scene units
    float4 normalSpec  : SV_Target2; // .xyz view normal *0.5+0.5, .w sqrt(p/512)
    float4 diffuseSpec : SV_Target3; // .rgb albedo, .a mono spec intensity
}

[shader("pixel")]
MrtOut psM3StandardMrt(PSInput input, bool isFront : SV_IsFrontFace)
{
    let s = ShadeM3(input, isFront);

    MrtOut o;
    o.color = float4(s.color, s.alpha);
    o.linearDepth = input.worldPos.w;
    // Camera-facing (z <= 0) is a G-buffer *encoding* contract, not a shading
    // decision: gtao.slang trusts the stored orientation and documents at
    // length why it will not re-flip defensively. The shading normal above
    // follows retail's two-sided rule and so no longer guarantees it, which is
    // why the orient lives here now — the same thing the HD path's
    // orientNormal() does before its own MRT write.
    let nVS0 = normalize(mul(float4(s.normalWS, 0.0), pass.view).xyz);
    let nVS = (nVS0.z > 0.0) ? -nVS0 : nVS0;
    o.normalSpec = float4(nVS * 0.5 + 0.5, sqrt(saturate(s.specPower / 512.0)));
    o.diffuseSpec =
        float4(s.albedo, dot(s.specColor, float3(0.299, 0.587, 0.114)));
    return o;
}

// ---- Ribbon strip (SC2 `RIB_`) --------------------------------------------
//
// The ribbon shares the FULL material PS (psM3Standard, ShadeM3) — same layer
// stack, emissive, team colour, blend and lighting a geoset gets — so a glowing
// energy ribbon renders through its real material rather than a stripped
// diffuse-only copy. Only the VS differs: the CPU ribbon service already builds
// the strip in WORLD space (displacement, camera frame, size/colour interp and
// V = fAge done on the host, ribbon_vs_math.h, O12-gated), so this VS just
// projects and hands ShadeM3 a camera-facing billboard basis plus the
// interpolated element colour in vColor. Forward-only: ribbons are transparent
// and never write the G-buffer sidecar (extraColorWrite off in the PSO, as a
// transparent geoset draw stays on psM3Standard in the scene pass).
struct RibbonVSInput
{
    float3 position : POSITION;   // already world-space
    float4 color    : COLOR0;     // interpolated element rgba (renderer::Vertex)
    float2 uv       : TEXCOORD0;  // final [0,1], NOT the SNORM `.m3` raw
}

[shader("vertex")]
PSInput vsM3Ribbon(RibbonVSInput input)
{
    PSInput o;
    // draw.world is identity — the strip is world-space already — but it rides
    // the shared M3DrawData CB so ShadeM3's layer state is the geoset path's.
    let worldPos = mul(float4(input.position, 1.0), draw.world);
    let viewPos = mul(worldPos, pass.view);
    o.position = mul(viewPos, pass.projection);
    o.worldPos = float4(worldPos.xyz, viewPos.z);
    // Camera-facing billboard: the shading normal is the view vector, and any
    // tangent perpendicular to it will do (ribbons carry no tangent-space
    // basis; a material with a normal/env layer gets a stable, if arbitrary,
    // frame rather than a degenerate one).
    let toEye = normalize(pass.cameraPosWS.xyz - worldPos.xyz);
    o.normalWS = toEye;
    let up = (abs(toEye.z) < 0.99) ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    o.tangentWS = float4(normalize(cross(up, toEye)), 1.0);
    // No SNORM fold (draw.uvTransform.x/.y): the ribbon uv is a real coordinate.
    // The layer's own animated transform still applies, in M3LayerUV.
    o.uv01 = float4(input.uv, input.uv);
    o.vColor = input.color;
    return o;
}
