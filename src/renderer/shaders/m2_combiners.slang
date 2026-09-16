// ============================================================================
// WhiteoutFlakes Renderer — M2 combiners (World of Warcraft `.m2`).
//
// The shader half of M2CombinerShading. 16 vertex entry points and 36 pixel
// ones, named and numbered exactly as the client's `s_modelVertexShaders` /
// `s_modelPixelShaders`, because `M2GetVertexShaderID` / `M2GetPixelShaderID`
// index straight into them (m2_shader_select.h).
//
// An *engine* shader on the embedded slangc path, for the same reason
// unlit.slang is one: `bls::PsoRequest` needs a `const BlsProgram*`, and
// producing a new `.bls` bundle needs WDX_BUILD_WC3_SHADERS, which is mutually
// exclusive with the WDX_USE_PREBUILT_SHADERS that CI and every fresh clone
// build with.
//
// The client compiles fog mode, alpha test and lighting as separate
// permutations. Here they are uniform branches: 51 programs is already the
// interesting axis, and multiplying it by twenty for three booleans and a
// five-value enum would put ~1000 blobs in compiled_shaders.h to save a few
// scalar selects on two-instruction combiners.
//
// Any VS may pair with any PS, so every entry point shares one interstage
// signature. That is what makes the 16 x 35 matrix legal rather than 560
// hand-checked pairs.
// ============================================================================

// ---- Constant buffers ------------------------------------------------------
//
// Split by update frequency: b0 once per pass, b1 per draw. Both are bound to
// vertex *and* pixel stages — Vulkan and WebGPU build one descriptor set from
// the shared layout, so a declared binding left unbound in either stage is a
// validation error even when only one stage reads it.

import debug_view;

struct M2PassData
{
    float4x4 view;
    float4x4 projection;
    float4 cameraPosWS;  // .xyz eye in world space
    float4 fogParams;    // .x start, .y end, .z density (0 disables), .w unused
    float4 fogColor;     // .rgb scene fog colour
}

// Lighting lives here rather than in the pass block because the client resolves
// it per *model*: one CM2Lighting per CM2Model per frame, holding one
// directional term and the three nearest point lights. See m2_lighting.h.
static const int kM2PointLights = 3;

struct M2DrawData
{
    float4x4 world;
    // Two texture-transform slots, not four: CM2SceneRender::SetupTextureTransforms
    // only ever calls SetTexMtx(0) and SetTexMtx(1). A third or fourth unit
    // samples its UV set untransformed.
    float4x4 texMtx0;
    float4x4 texMtx1;
    float4 elementColor; // .rgb batch colour, .a element alpha
    float4 unitWeights;  // per-texture-unit weight; only unit 0 gates visibility
    float4 params;       // .x alphaRef, .y fogMode, .z lit, .w premultiply
    float4 lightAmbient; // .rgb summed ambient, unclamped
    float4 lightDiffuse; // .rgb the directional term, clamped per channel to 1
    float4 lightDir;     // .xyz direction the light TRAVELS, normalised
    float4 pointColor[kM2PointLights];
    float4 pointPos[kM2PointLights];   // .xyz world space
    float4 pointAtten[kM2PointLights]; // .xyz constant / linear / quadratic
}

// Per-backend binding shifts, mirroring gtao.slang / gaussian_blur.slang.
// Slang's WGSL and Metal targets keep `[[vk::binding(N)]]` literal, so these
// have to match each runtime's host-slot -> binding mapping exactly.
//
// The runtime uses three descriptor sets — 0 constant buffers, 1 sampled
// images, 2 samplers — and offsets the pixel stage above the vertex stage
// within each. Leaving Slang to assign these itself put the textures and
// samplers in set 0 alongside the constant buffers, where nothing ever writes
// them: D3D12 drew nothing and Vulkan lost the device.
#if defined(WGSL_TARGET) || defined(METAL_TARGET)
static const int WF_PS_BIND_OFFSET = 12;
#else
static const int WF_PS_BIND_OFFSET = 16;
#endif

// Both constant buffers are read by both stages, so they stay on the *vertex*
// slots: one global cannot carry two bindings, and the vertex-stage bind
// writes the set-0 entry the pixel stage then reads. Same arrangement
// unlit.slang relies on for its light CB.
// One bone matrix per slot, three float4 rows each — the transposed 4x3 that
// bls::PackBone writes, so the CB is byte-identical to the one WC3's skinned
// geosets bind. Declared as a flat float4 array rather than an array of a
// 48-byte struct: HLSL pads a struct array element to a 16-byte boundary, which
// 48 already is, but the flat form leaves nothing to get wrong across four
// backends. 256 slots x 3 rows = bls::kMaxBones.
struct M2BoneData
{
    float4 rows[768];
}

[[vk::binding(0, 0)]] ConstantBuffer<M2PassData> pass : register(b0);
[[vk::binding(1, 0)]] ConstantBuffer<M2DrawData> draw : register(b1);
// Vertex stage only — the combiners never see a bone. Mirrors how the WC3
// geoset passes bind their palette (vertex slot 3 there, b2 here).
[[vk::binding(2, 0)]] ConstantBuffer<M2BoneData> bones : register(b2);
// The debug entry only (DEBUG_VIEW_DESIGN.md); stripped from the others.
[[vk::binding(3, 0)]] ConstantBuffer<DebugViewData> debugView : register(b3);

// ---- Interstage signature --------------------------------------------------

// Both UV sets arrive as one float4, not two float2s. They are 16 contiguous
// bytes in the `.m2` record, so this costs nothing — and it avoids declaring a
// non-zero semantic index, which this toolchain cannot round-trip: Slang
// appends its own '0' to every semantic it emits, so `TEXCOORD0` becomes
// `TEXCOORD00` (index 0, harmless) but `TEXCOORD1` becomes `TEXCOORD10` —
// index *ten*. D3D11's CreateInputLayout and D3D12's PSO creation then both
// reject a layout declaring TEXCOORD index 1 with E_INVALIDARG. Vulkan derives
// locations from array position and never noticed.
struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float4 uvs         : TEXCOORD0; // .xy = UV set 0, .zw = UV set 1
    float4 boneWeights : BLENDWEIGHT;
    uint4  boneIndices : BLENDINDICES;
}

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR0;     // the combiner table's `in`
    float4 uv01     : TEXCOORD0;  // .xy unit0, .zw unit1
    float4 uv23     : TEXCOORD1;  // .xy unit2, .zw unit3
    float3 worldPos : TEXCOORD2;
    float3 normalWS : TEXCOORD3;
}

// ---- Vertex side -----------------------------------------------------------

// Where one texture unit's UVs come from. The vertex shader's name spells this
// out per unit: T1/T2 select a UV set, Env replaces it with a sphere map.
static const int kSrcT1_M0 = 0; // uv0 through texMtx0
static const int kSrcT1_M1 = 1; // uv0 through texMtx1
static const int kSrcT2_M0 = 2; // uv1 through texMtx0
static const int kSrcT2_M1 = 3; // uv1 through texMtx1
static const int kSrcEnv   = 4; // view-space sphere map
static const int kSrcT1    = 5; // uv0, untransformed
static const int kSrcT2    = 6; // uv1, untransformed

// Blizzard's own sphere map, with vertex and normal in camera space. Reproduced
// from wowdev.wiki/M2/.skin#Environment_mapping.
float2 SphereMap(float3 posVS, float3 nrmVS)
{
    let normPos = -normalize(posVS);
    let t = normPos - nrmVS * (2.0 * dot(normPos, nrmVS));
    return normalize(float3(t.x, t.y, t.z + 1.0)).xy * 0.5 + 0.5;
}

// Texture transforms rotate about (0.5, 0.5), which the C++ side has already
// folded into the matrix — here it is a plain affine multiply.
float2 ApplyTexMtx(float2 uv, float4x4 m)
{
    return mul(float4(uv, 0.0, 1.0), m).xy;
}

float2 M2Coord(int src, VSInput input, float3 posVS, float3 nrmVS)
{
    switch (src)
    {
    case kSrcT1_M0: return ApplyTexMtx(input.uvs.xy, draw.texMtx0);
    case kSrcT1_M1: return ApplyTexMtx(input.uvs.xy, draw.texMtx1);
    case kSrcT2_M0: return ApplyTexMtx(input.uvs.zw, draw.texMtx0);
    case kSrcT2_M1: return ApplyTexMtx(input.uvs.zw, draw.texMtx1);
    case kSrcEnv:   return SphereMap(posVS, nrmVS);
    case kSrcT2:    return input.uvs.zw;
    default:        return input.uvs.xy;
    }
}

// Model space to *animated* model space. An `.m2` bone matrix folds its own
// pivot in, so there is no inverse bind matrix to undo first — the same matrix
// applies to a vertex straight out of the file.
//
// Weights are normalised at pack time (they quantise to 255 summing to 255), so
// there is nothing to divide by here. A geoset whose vertices are all unweighted
// still binds a palette; its lanes just sum to zero and the point stays put.
void M2Skin(VSInput input, out float3 pos, out float3 nrm)
{
    pos = float3(0.0, 0.0, 0.0);
    nrm = float3(0.0, 0.0, 0.0);
    let p = float4(input.position, 1.0);
    let n = float4(input.normal, 0.0);
    // No early-out on a zero weight: `continue` inside an unrolled loop is what
    // fxc refuses to unroll ("loop does not appear to terminate in a timely
    // manner"), and a zero-weight lane costs one madd it then multiplies away.
    // Its bone index is 0 by construction — every unused lane keeps the
    // VertexInfluence default, which the palette rewrite maps to the geoset's
    // first slot — so the read is always in range.
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
    }
}

// The whole vertex stage. Every named entry point below is this with four
// source codes; the client's sixteen vertex shaders differ in nothing else.
PSInput M2Vertex(VSInput input, int s0, int s1, int s2, int s3)
{
    PSInput o;

    float3 skinPos, skinNrm;
    M2Skin(input, skinPos, skinNrm);

    let worldPos = mul(float4(skinPos, 1.0), draw.world);
    let viewPos  = mul(worldPos, pass.view);
    o.position = mul(viewPos, pass.projection);
    o.worldPos = worldPos.xyz;

    // Uniform scale only (the actor world transform is rotation + translation +
    // WorldScale), so the world matrix carries normals correctly after the
    // renormalise — no inverse transpose needed.
    let nrmWS = normalize(mul(float4(skinNrm, 0.0), draw.world).xyz);
    let nrmVS = normalize(mul(float4(nrmWS, 0.0), pass.view).xyz);

    o.uv01 = float4(M2Coord(s0, input, viewPos.xyz, nrmVS),
                    M2Coord(s1, input, viewPos.xyz, nrmVS));
    o.uv23 = float4(M2Coord(s2, input, viewPos.xyz, nrmVS),
                    M2Coord(s3, input, viewPos.xyz, nrmVS));

    // The combiner table's `in`, *unlit*. Lighting is applied per pixel — see
    // M2In. Doing it here instead produced hard bands across every low-poly
    // surface: Lambert evaluated at three vertices and interpolated is flat
    // shading with extra steps, and a per-vertex face-forward flip makes it
    // worse, because two vertices either side of `dot(n, toEye) == 0` pick
    // opposite normals and the triangle between them interpolates across the
    // discontinuity.
    o.color = draw.elementColor;
    o.normalWS = nrmWS;

    return o;
}

[shader("vertex")] PSInput vsM2Diffuse_T1(VSInput i)
    { return M2Vertex(i, kSrcT1_M0, kSrcT1_M0, kSrcT1, kSrcT1); }
[shader("vertex")] PSInput vsM2Diffuse_Env(VSInput i)
    { return M2Vertex(i, kSrcEnv, kSrcEnv, kSrcT1, kSrcT1); }
[shader("vertex")] PSInput vsM2Diffuse_T1_T2(VSInput i)
    { return M2Vertex(i, kSrcT1_M0, kSrcT2_M1, kSrcT1, kSrcT2); }
[shader("vertex")] PSInput vsM2Diffuse_T1_Env(VSInput i)
    { return M2Vertex(i, kSrcT1_M0, kSrcEnv, kSrcT1, kSrcT1); }
[shader("vertex")] PSInput vsM2Diffuse_Env_T1(VSInput i)
    { return M2Vertex(i, kSrcEnv, kSrcT1_M1, kSrcT1, kSrcT1); }
[shader("vertex")] PSInput vsM2Diffuse_Env_Env(VSInput i)
    { return M2Vertex(i, kSrcEnv, kSrcEnv, kSrcT1, kSrcT1); }
[shader("vertex")] PSInput vsM2Diffuse_T1_Env_T1(VSInput i)
    { return M2Vertex(i, kSrcT1_M0, kSrcEnv, kSrcT1, kSrcT1); }
[shader("vertex")] PSInput vsM2Diffuse_T1_T1(VSInput i)
    { return M2Vertex(i, kSrcT1_M0, kSrcT1_M1, kSrcT1, kSrcT1); }
// 12.1 deleted this one and gave row 18 plain Diffuse_T1, so all three of the
// crossfade's samplers read the single T1 coord — one UV set through texture
// matrix 0, not a matrix each. Kept as a name because our coords are positional.
[shader("vertex")] PSInput vsM2Diffuse_T1_T1_T1(VSInput i)
    { return M2Vertex(i, kSrcT1_M0, kSrcT1_M0, kSrcT1_M0, kSrcT1_M0); }
// EdgeFade needs the EDGF chunk, which nothing reads yet — these behave as
// their non-fading twins until it does.
[shader("vertex")] PSInput vsM2Diffuse_EdgeFade_T1(VSInput i)
    { return M2Vertex(i, kSrcT1_M0, kSrcT1_M0, kSrcT1, kSrcT1); }
[shader("vertex")] PSInput vsM2Diffuse_T2(VSInput i)
    { return M2Vertex(i, kSrcT2_M0, kSrcT2_M0, kSrcT2, kSrcT2); }
[shader("vertex")] PSInput vsM2Diffuse_T1_Env_T2(VSInput i)
    { return M2Vertex(i, kSrcT1_M0, kSrcEnv, kSrcT2, kSrcT2); }
[shader("vertex")] PSInput vsM2Diffuse_EdgeFade_T1_T2(VSInput i)
    { return M2Vertex(i, kSrcT1_M0, kSrcT2_M1, kSrcT1, kSrcT2); }
// Likewise row 22, which 12.1 names Diffuse_T1_T2: the three crossfade layers
// share T1, and only the mask reads T2.
[shader("vertex")] PSInput vsM2Diffuse_T1_T1_T1_T2(VSInput i)
    { return M2Vertex(i, kSrcT1_M0, kSrcT1_M0, kSrcT1_M0, kSrcT2_M1); }
[shader("vertex")] PSInput vsM2Diffuse_EdgeFade_Env(VSInput i)
    { return M2Vertex(i, kSrcEnv, kSrcEnv, kSrcT1, kSrcT1); }
[shader("vertex")] PSInput vsM2Diffuse_T1_T2_T1(VSInput i)
    { return M2Vertex(i, kSrcT1_M0, kSrcT2_M1, kSrcT1, kSrcT1); }

// ---- Pixel side ------------------------------------------------------------

[[vk::binding(WF_PS_BIND_OFFSET + 0, 1)]] Texture2D tex0 : register(t0);
[[vk::binding(WF_PS_BIND_OFFSET + 1, 1)]] Texture2D tex1 : register(t1);
[[vk::binding(WF_PS_BIND_OFFSET + 2, 1)]] Texture2D tex2 : register(t2);
[[vk::binding(WF_PS_BIND_OFFSET + 3, 1)]] Texture2D tex3 : register(t3);
[[vk::binding(WF_PS_BIND_OFFSET + 0, 2)]] SamplerState samp0 : register(s0);
[[vk::binding(WF_PS_BIND_OFFSET + 1, 2)]] SamplerState samp1 : register(s1);
[[vk::binding(WF_PS_BIND_OFFSET + 2, 2)]] SamplerState samp2 : register(s2);
[[vk::binding(WF_PS_BIND_OFFSET + 3, 2)]] SamplerState samp3 : register(s3);

// Fog, after combining. The mode is not on/off: an additive surface fogged
// toward the scene colour brightens instead of receding, so the client
// substitutes the colour that is a no-op for that blend rather than skipping
// the blend. s_fogModeList picks it from the blend mode (m2_material.h).
float3 ApplyFog(float3 color, float3 worldPos, float mode)
{
    if (mode < 0.5 || pass.fogParams.z <= 0.0)
        return color;

    let d = distance(worldPos, pass.cameraPosWS.xyz);
    let f = saturate((d - pass.fogParams.x) / max(pass.fogParams.y - pass.fogParams.x, 1e-4)) *
            pass.fogParams.z;

    float3 fc = pass.fogColor.rgb;
    if (mode > 1.5 && mode < 2.5) fc = float3(0.0, 0.0, 0.0);
    else if (mode > 2.5 && mode < 3.5) fc = float3(1.0, 1.0, 1.0);
    else if (mode > 3.5) fc = float3(0.5, 0.5, 0.5);

    return lerp(color, fc, f);
}

// The one exit every combiner takes: alpha test, then fog. The comparison is
// always >=, so a fragment at exactly alphaRef survives — matching
// D3DCMP_GREATEREQUAL.
//
// params.w premultiplies. `CM2Model::InitializeLoaded`'s M2Blend-to-EGxBlend
// table is {0, 1, 2, 10, 3, 4, 5, 13}, so blend mode 7 is EGxBlend 13 —
// (ONE, INV_SRC_ALPHA), which is premultiplied "over" and reads nothing from
// the source's alpha unless the source carries it in its colour. Left
// unpremultiplied, alpha does not attenuate mode 7 at all, and a batch the
// artist switched off by zeroing its colour track drew at full brightness:
// `phoenix2mount` section 15 is 504 triangles whose alpha is 0 in every one of
// its 63 sequences.
float4 M2Finish(float3 rgb, float a, PSInput input)
{
    if (a < draw.params.x)
        discard;
    let out = lerp(rgb, rgb * a, draw.params.w);
    return float4(ApplyFog(out, input.worldPos, draw.params.y), a);
}

float4 M2Finish(float4 mixed, PSInput input)
{
    return M2Finish(mixed.rgb, mixed.a, input);
}

// The combiner table's `in`, lit — the client's own light model, resolved per
// model on the CPU (m2_lighting.h) and evaluated here:
//
//     lit = saturate(ambient
//                  + diffuse * saturate(dot(N, -lightDir))
//                  + Σ pointColor[i] * saturate(dot(N, toLight)) * atten)
//
// with `atten = 1 / (k0 + k1·d + k2·d²)`, GL's form — GLDevice::SetLight hands
// CM2Light's three floats to GL_CONSTANT/LINEAR/QUADRATIC_ATTENUATION, and the
// unused shader slots default k0 to 1, which is the only value that makes a
// reciprocal safe there. Which blend modes and flags switch the whole thing off
// is m2_material.h's call, carried in params.z.
//
// `saturate` on the total, not on the parts: the client's ambient is a raw sum
// and its portrait preset already reaches 1.45 at full N·L, so a lit batch is
// meant to clip against an unlit neighbour rather than be normalised down to
// meet it.
//
// Two properties it has to hold:
//
//   Direction is where the light *goes*, not where it comes from. CM2Lighting
//   stores propagation and GLDevice::SetLight negates on the way to GL_POSITION,
//   so the N·L term negates here.
//
//   Face toward the viewer, per pixel. Two-sided materials (flag 0x04) are the
//   wing membranes and fins, and one side of every such triangle has a normal
//   pointing away; without this they collapse to flat ambient and read as
//   holes. Per *pixel* because the flip is a discontinuity — interpolating
//   across it is what banded the wings. This is our one deliberate deviation:
//   the client lights per vertex, in the sixteen vertex shaders.
// The light M2In multiplies the combiner input by, 1 on an unlit batch.
float3 M2Light(PSInput i)
{
    if (draw.params.z <= 0.5)
        return float3(1.0, 1.0, 1.0);

    float3 n = normalize(i.normalWS);
    let toEye = normalize(pass.cameraPosWS.xyz - i.worldPos);
    n = (dot(n, toEye) < 0.0) ? -n : n;

    float3 acc = draw.lightAmbient.rgb;
    acc += draw.lightDiffuse.rgb * saturate(dot(n, -draw.lightDir.xyz));

    // No count test and no early out: an unfilled slot arrives with colour 0 and
    // attenuation (1, 0, 0), which is what ComputeLocalLights writes into the
    // slots CM2Lighting did not fill, and it costs one madd that multiplies
    // away. Branching instead is what fxc refuses to unroll — the same
    // "does not appear to terminate in a timely manner" M2Skin documents.
    [unroll]
    for (int L = 0; L < kM2PointLights; ++L)
    {
        let d = draw.pointPos[L].xyz - i.worldPos;
        let distSq = dot(d, d);
        let dist = sqrt(max(distSq, 1e-8));
        let a = draw.pointAtten[L].xyz;
        let atten = 1.0 / max(a.x + a.y * dist + a.z * distSq, 1e-4);
        acc += draw.pointColor[L].rgb * saturate(dot(n, d / dist)) * atten;
    }

    return saturate(acc);
}

float4 M2In(PSInput i)
{
    if (draw.params.z <= 0.5)
        return i.color;
    return float4(i.color.rgb * M2Light(i), i.color.a);
}

float4 T0(PSInput i) { return tex0.Sample(samp0, i.uv01.xy); }
float4 T1(PSInput i) { return tex1.Sample(samp1, i.uv01.zw); }
float4 T2(PSInput i) { return tex2.Sample(samp2, i.uv23.xy); }
float4 T3(PSInput i) { return tex3.Sample(samp3, i.uv23.zw); }

// ---------------------------------------------------------------------------
// The combiner table.
//
// The fifteen shaders wowdev.wiki/M2/Rendering publishes RGB and alpha logic
// for are transcribed from it directly. The rest — the three- and four-texture
// ones, the _Wgt / _UnshAlpha / _Dual_Crossfade family, Guild*, Illum and
// Mod_Depth — are not documented there and are written from the naming grammar
// the documented fifteen establish. They are marked DERIVED and are the ones to
// check first against the shipped `shaders/pixel/*/combiners_*.bls` when a
// model looks wrong.
// ---------------------------------------------------------------------------

float4 M2Mix_Combiners_Opaque(PSInput i, float4 c)
{
    let t0 = T0(i);
    return float4(c.rgb * t0.rgb, c.a);
}
[shader("fragment")] float4 psM2Combiners_Opaque(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Opaque(i, M2In(i)), i);
}

float4 M2Mix_Combiners_Mod(PSInput i, float4 c)
{
    let t0 = T0(i);
    return float4(c.rgb * t0.rgb, c.a * t0.a);
}
[shader("fragment")] float4 psM2Combiners_Mod(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Mod(i, M2In(i)), i);
}

float4 M2Mix_Combiners_Opaque_Mod(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    return float4(c.rgb * t0.rgb * t1.rgb, c.a * t1.a);
}
[shader("fragment")] float4 psM2Combiners_Opaque_Mod(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Opaque_Mod(i, M2In(i)), i);
}

float4 M2Mix_Combiners_Opaque_Mod2x(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    return float4(c.rgb * t0.rgb * t1.rgb * 2.0, c.a * t1.a * 2.0);
}
[shader("fragment")] float4 psM2Combiners_Opaque_Mod2x(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Opaque_Mod2x(i, M2In(i)), i);
}

float4 M2Mix_Combiners_Opaque_Mod2xNA(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    return float4(c.rgb * t0.rgb * t1.rgb * 2.0, c.a);
}
[shader("fragment")] float4 psM2Combiners_Opaque_Mod2xNA(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Opaque_Mod2xNA(i, M2In(i)), i);
}

float4 M2Mix_Combiners_Opaque_Opaque(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    return float4(c.rgb * t0.rgb * t1.rgb, c.a);
}
[shader("fragment")] float4 psM2Combiners_Opaque_Opaque(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Opaque_Opaque(i, M2In(i)), i);
}

float4 M2Mix_Combiners_Mod_Mod(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    return float4(c.rgb * t0.rgb * t1.rgb, c.a * t0.a * t1.a);
}
[shader("fragment")] float4 psM2Combiners_Mod_Mod(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Mod_Mod(i, M2In(i)), i);
}

float4 M2Mix_Combiners_Mod_Mod2x(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    return float4(c.rgb * t0.rgb * t1.rgb * 2.0, c.a * t0.a * t1.a * 2.0);
}
[shader("fragment")] float4 psM2Combiners_Mod_Mod2x(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Mod_Mod2x(i, M2In(i)), i);
}

float4 M2Mix_Combiners_Mod_Add(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    return float4(c.rgb * t0.rgb + t1.rgb, c.a * t0.a + t1.a);
}
[shader("fragment")] float4 psM2Combiners_Mod_Add(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Mod_Add(i, M2In(i)), i);
}

float4 M2Mix_Combiners_Mod_Mod2xNA(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    return float4(c.rgb * t0.rgb * t1.rgb * 2.0, c.a * t0.a);
}
[shader("fragment")] float4 psM2Combiners_Mod_Mod2xNA(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Mod_Mod2xNA(i, M2In(i)), i);
}

float4 M2Mix_Combiners_Mod_AddNA(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    return float4(c.rgb * t0.rgb + t1.rgb, c.a * t0.a);
}
[shader("fragment")] float4 psM2Combiners_Mod_AddNA(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Mod_AddNA(i, M2In(i)), i);
}

float4 M2Mix_Combiners_Mod_Opaque(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    return float4(c.rgb * t0.rgb * t1.rgb, c.a * t0.a);
}
[shader("fragment")] float4 psM2Combiners_Mod_Opaque(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Mod_Opaque(i, M2In(i)), i);
}

float4 M2Mix_Combiners_Opaque_Mod2xNA_Alpha(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    let rgb = c.rgb * t0.rgb * lerp(t1.rgb * 2.0, float3(1.0, 1.0, 1.0), t0.a);
    return float4(rgb, c.a);
}
[shader("fragment")] float4 psM2Combiners_Opaque_Mod2xNA_Alpha(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Opaque_Mod2xNA_Alpha(i, M2In(i)), i);
}

float4 M2Mix_Combiners_Opaque_AddAlpha(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    return float4(c.rgb * t0.rgb + t1.rgb * t1.a, c.a);
}
[shader("fragment")] float4 psM2Combiners_Opaque_AddAlpha(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Opaque_AddAlpha(i, M2In(i)), i);
}

float4 M2Mix_Combiners_Opaque_AddAlpha_Alpha(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    return float4(c.rgb * t0.rgb + t1.rgb * t1.a * t0.a, c.a);
}
[shader("fragment")] float4 psM2Combiners_Opaque_AddAlpha_Alpha(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Opaque_AddAlpha_Alpha(i, M2In(i)), i);
}

// DERIVED — Opaque_Mod2xNA_Alpha with a third unit added on top.
float4 M2Mix_Combiners_Opaque_Mod2xNA_Alpha_Add(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i); let t2 = T2(i);
    let rgb = c.rgb * t0.rgb * lerp(t1.rgb * 2.0, float3(1.0, 1.0, 1.0), t0.a) +
              t2.rgb * t2.a;
    return float4(rgb, c.a);
}
[shader("fragment")] float4 psM2Combiners_Opaque_Mod2xNA_Alpha_Add(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Opaque_Mod2xNA_Alpha_Add(i, M2In(i)), i);
}

// DERIVED
float4 M2Mix_Combiners_Mod_AddAlpha(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    return float4(c.rgb * t0.rgb + t1.rgb * t1.a, c.a * t0.a);
}
[shader("fragment")] float4 psM2Combiners_Mod_AddAlpha(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Mod_AddAlpha(i, M2In(i)), i);
}

// DERIVED
float4 M2Mix_Combiners_Mod_AddAlpha_Alpha(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    let rgb = c.rgb * t0.rgb + t1.rgb * t1.a * t0.a;
    return float4(rgb, c.a * (t0.a + t1.a * t1.a));
}
[shader("fragment")] float4 psM2Combiners_Mod_AddAlpha_Alpha(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Mod_AddAlpha_Alpha(i, M2In(i)), i);
}

// DERIVED
float4 M2Mix_Combiners_Opaque_Alpha_Alpha(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    let rgb = lerp(lerp(c.rgb * t0.rgb, t1.rgb, t1.a), c.rgb * t0.rgb, t0.a);
    return float4(rgb, c.a);
}
[shader("fragment")] float4 psM2Combiners_Opaque_Alpha_Alpha(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Opaque_Alpha_Alpha(i, M2In(i)), i);
}

// DERIVED — the "3s" variant folds a third unit into the shine mask.
float4 M2Mix_Combiners_Opaque_Mod2xNA_Alpha_3s(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i); let t2 = T2(i);
    let rgb = c.rgb * t0.rgb * lerp(t1.rgb * 2.0, t2.rgb, t0.a);
    return float4(rgb, c.a);
}
[shader("fragment")] float4 psM2Combiners_Opaque_Mod2xNA_Alpha_3s(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Opaque_Mod2xNA_Alpha_3s(i, M2In(i)), i);
}

// DERIVED — _Wgt scales the added unit by the batch's per-unit weight.
float4 M2Mix_Combiners_Opaque_AddAlpha_Wgt(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    return float4(c.rgb * t0.rgb + t1.rgb * t1.a * draw.unitWeights.y, c.a);
}
[shader("fragment")] float4 psM2Combiners_Opaque_AddAlpha_Wgt(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Opaque_AddAlpha_Wgt(i, M2In(i)), i);
}

// DERIVED
float4 M2Mix_Combiners_Mod_Add_Alpha(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    return float4(c.rgb * t0.rgb + t1.rgb, c.a * t0.a + t1.a);
}
[shader("fragment")] float4 psM2Combiners_Mod_Add_Alpha(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Mod_Add_Alpha(i, M2In(i)), i);
}

// DERIVED
float4 M2Mix_Combiners_Opaque_ModNA_Alpha(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    let rgb = c.rgb * t0.rgb * lerp(t1.rgb, float3(1.0, 1.0, 1.0), t0.a);
    return float4(rgb, c.a);
}
[shader("fragment")] float4 psM2Combiners_Opaque_ModNA_Alpha(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Opaque_ModNA_Alpha(i, M2In(i)), i);
}

// DERIVED
float4 M2Mix_Combiners_Mod_AddAlpha_Wgt(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    let rgb = c.rgb * t0.rgb + t1.rgb * t1.a * draw.unitWeights.y;
    return float4(rgb, c.a * t0.a);
}
[shader("fragment")] float4 psM2Combiners_Mod_AddAlpha_Wgt(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Mod_AddAlpha_Wgt(i, M2In(i)), i);
}

// DERIVED
float4 M2Mix_Combiners_Opaque_Mod_Add_Wgt(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    let rgb = c.rgb * lerp(t0.rgb, t1.rgb, t1.a) + t0.rgb * draw.unitWeights.x;
    return float4(rgb, c.a);
}
[shader("fragment")] float4 psM2Combiners_Opaque_Mod_Add_Wgt(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Opaque_Mod_Add_Wgt(i, M2In(i)), i);
}

// DERIVED — UnshAlpha keeps the *unshaded* colour out of the shine term.
float4 M2Mix_Combiners_Opaque_Mod2xNA_Alpha_UnshAlpha(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i); let t2 = T2(i);
    let glow = draw.unitWeights.z;
    let rgb = lerp(c.rgb * t0.rgb * lerp(t1.rgb * 2.0, float3(1.0, 1.0, 1.0), t0.a),
                   t0.rgb, glow * t2.a);
    return float4(rgb, c.a);
}
[shader("fragment")] float4 psM2Combiners_Opaque_Mod2xNA_Alpha_UnshAlpha(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Opaque_Mod2xNA_Alpha_UnshAlpha(i, M2In(i)), i);
}

// DERIVED — two diffuse maps crossfaded by the batch weight.
float4 M2Mix_Combiners_Mod_Dual_Crossfade(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i); let t2 = T2(i);
    let a = saturate(draw.unitWeights.y);
    let b = saturate(draw.unitWeights.z);
    let blended = lerp(lerp(t0, t1, a), t2, b);
    return float4(c.rgb * blended.rgb, c.a * blended.a);
}
[shader("fragment")] float4 psM2Combiners_Mod_Dual_Crossfade(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Mod_Dual_Crossfade(i, M2In(i)), i);
}

// DERIVED
float4 M2Mix_Combiners_Opaque_Mod2xNA_Alpha_Alpha(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i); let t2 = T2(i);
    let secondary = lerp(t1.rgb * 2.0, float3(1.0, 1.0, 1.0), t0.a);
    let rgb = lerp(c.rgb * t0.rgb * secondary, t2.rgb, t2.a);
    return float4(rgb, c.a);
}
[shader("fragment")] float4 psM2Combiners_Opaque_Mod2xNA_Alpha_Alpha(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Opaque_Mod2xNA_Alpha_Alpha(i, M2In(i)), i);
}

// DERIVED — the crossfade above, masked by a fourth unit.
float4 M2Mix_Combiners_Mod_Masked_Dual_Crossfade(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i); let t2 = T2(i); let t3 = T3(i);
    let a = saturate(draw.unitWeights.y);
    let b = saturate(draw.unitWeights.z);
    let blended = lerp(lerp(t0, t1, a), t2, b);
    return float4(c.rgb * blended.rgb, c.a * blended.a * t3.a);
}
[shader("fragment")] float4 psM2Combiners_Mod_Masked_Dual_Crossfade(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Mod_Masked_Dual_Crossfade(i, M2In(i)), i);
}

// DERIVED
float4 M2Mix_Combiners_Opaque_Alpha(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    return float4(lerp(c.rgb * t0.rgb, t1.rgb, t1.a), c.a);
}
[shader("fragment")] float4 psM2Combiners_Opaque_Alpha(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Opaque_Alpha(i, M2In(i)), i);
}

// DERIVED — guild tabard compositing needs the emblem/border colour constants
// the design defers; these draw the base layer so a tabard is not a hole.
float4 M2Mix_Guild(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i); let t2 = T2(i);
    let emblem = lerp(t1.rgb, t2.rgb, t2.a);
    return float4(c.rgb * lerp(t0.rgb, emblem, t1.a), c.a * t0.a);
}
[shader("fragment")] float4 psM2Guild(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Guild(i, M2In(i)), i);
}

// DERIVED
float4 M2Mix_Guild_NoBorder(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    return float4(c.rgb * lerp(t0.rgb, t1.rgb, t1.a), c.a * t0.a);
}
[shader("fragment")] float4 psM2Guild_NoBorder(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Guild_NoBorder(i, M2In(i)), i);
}

// DERIVED
float4 M2Mix_Guild_Opaque(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i); let t2 = T2(i);
    let emblem = lerp(t1.rgb, t2.rgb, t2.a);
    return float4(c.rgb * lerp(t0.rgb, emblem, t1.a), c.a);
}
[shader("fragment")] float4 psM2Guild_Opaque(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Guild_Opaque(i, M2In(i)), i);
}

// DERIVED — the depth-fading twin of Combiners_Mod. Without the EDGF chunk the
// fade term is 1, so this is Mod exactly.
float4 M2Mix_Combiners_Mod_Depth(PSInput i, float4 c)
{
    let t0 = T0(i);
    return float4(c.rgb * t0.rgb, c.a * t0.a);
}
[shader("fragment")] float4 psM2Combiners_Mod_Depth(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Mod_Depth(i, M2In(i)), i);
}

// DERIVED — the two-texture twin of Combiners_Mod_Depth, and 12.1's pixel
// shader 36 (row 35). Same standing as Mod_Depth: without the EDGF chunk the
// fade term is 1, which leaves this exactly Mod_Mod.
float4 M2Mix_Combiners_Mod_Mod_Depth(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    return float4(c.rgb * t0.rgb * t1.rgb, c.a * t0.a * t1.a);
}
[shader("fragment")] float4 psM2Combiners_Mod_Mod_Depth(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Combiners_Mod_Mod_Depth(i, M2In(i)), i);
}

// DERIVED — self-illuminated: the second unit is an emissive mask added
// unlit, which is what separates Illum from Opaque_Add.
float4 M2Mix_Illum(PSInput i, float4 c)
{
    let t0 = T0(i); let t1 = T1(i);
    let rgb = c.rgb * t0.rgb + t0.rgb * t1.rgb * draw.unitWeights.y;
    return float4(rgb, c.a * t0.a);
}
[shader("fragment")] float4 psM2Illum(PSInput i) : SV_Target
{
    return M2Finish(M2Mix_Illum(i, M2In(i)), i);
}

// ---- Debug views (DEBUG_VIEW_DESIGN.md) ------------------------------------
//
// A combiner names no channels, so the debug entry runs the batch's own one at
// fixed light: with none it shows what the surface emits, and what light adds
// on top of that is its diffuse. The case order is M2PixelShader's — the index
// the host keys its pixel shader array on, borrowed rows included.
float4 M2MixById(uint id, PSInput i, float4 c)
{
    switch (id)
    {
    case 0u: return M2Mix_Combiners_Opaque(i, c);
    case 1u: return M2Mix_Combiners_Mod(i, c);
    case 2u: return M2Mix_Combiners_Opaque_Mod(i, c);
    case 3u: return M2Mix_Combiners_Opaque_Mod2x(i, c);
    case 4u: return M2Mix_Combiners_Opaque_Mod2xNA(i, c);
    case 5u: return M2Mix_Combiners_Opaque_Opaque(i, c);
    case 6u: return M2Mix_Combiners_Mod_Mod(i, c);
    case 7u: return M2Mix_Combiners_Mod_Mod2x(i, c);
    case 8u: return M2Mix_Combiners_Mod_Add(i, c);
    case 9u: return M2Mix_Combiners_Mod_Mod2xNA(i, c);
    case 10u: return M2Mix_Combiners_Mod_AddNA(i, c);
    case 11u: return M2Mix_Combiners_Mod_Opaque(i, c);
    case 12u: return M2Mix_Combiners_Opaque_Mod2xNA_Alpha(i, c);
    case 13u: return M2Mix_Combiners_Opaque_AddAlpha(i, c);
    case 14u: return M2Mix_Combiners_Opaque_AddAlpha_Alpha(i, c);
    case 15u: return M2Mix_Combiners_Opaque_Mod2xNA_Alpha_Add(i, c);
    case 16u: return M2Mix_Combiners_Mod_AddAlpha(i, c);
    case 17u: return M2Mix_Combiners_Mod_AddAlpha_Alpha(i, c);
    case 18u: return M2Mix_Combiners_Opaque_Alpha_Alpha(i, c);
    case 19u: return M2Mix_Combiners_Opaque_Mod2xNA_Alpha_3s(i, c);
    case 20u: return M2Mix_Combiners_Opaque_AddAlpha_Wgt(i, c);
    case 21u: return M2Mix_Combiners_Mod_Add_Alpha(i, c);
    case 22u: return M2Mix_Combiners_Opaque_ModNA_Alpha(i, c);
    case 23u: return M2Mix_Combiners_Mod_AddAlpha_Wgt(i, c);
    case 24u: return M2Mix_Combiners_Opaque_Mod_Add_Wgt(i, c);
    case 25u: return M2Mix_Combiners_Opaque_Mod2xNA_Alpha_UnshAlpha(i, c);
    case 26u: return M2Mix_Combiners_Mod_Dual_Crossfade(i, c);
    case 27u: return M2Mix_Combiners_Opaque_Mod2xNA_Alpha_Alpha(i, c);
    case 28u: return M2Mix_Combiners_Mod_Masked_Dual_Crossfade(i, c);
    case 29u: return M2Mix_Combiners_Opaque_Alpha(i, c);
    case 30u: return M2Mix_Guild(i, c);
    case 31u: return M2Mix_Guild_NoBorder(i, c);
    case 32u: return M2Mix_Guild_Opaque(i, c);
    case 33u: return M2Mix_Combiners_Mod_Depth(i, c);
    case 34u: return M2Mix_Illum(i, c);
    case 35u: return M2Mix_Combiners_Opaque_Mod2xNA_Alpha_Alpha(i, c);
    case 36u: return M2Mix_Combiners_Mod_Mod_Depth(i, c);
    default: return M2Mix_Combiners_Opaque(i, c);
    }
}

[shader("fragment")] float4 psM2Debug(PSInput i) : SV_Target
{
    let d = debugView;
    uint w, h;
    tex0.GetDimensions(w, h);
    DebugSurface s = DebugSurfaceInit();
    s.mipLevel = DebugMipLevel(i.uv01.xy, float2(w, h));

    let id = d.ctl.w;
    let shaded = M2MixById(id, i, M2In(i));
    if (shaded.a < draw.params.x)
        discard;
    let unlit = M2MixById(id, i, float4(0.0, 0.0, 0.0, i.color.a));
    let full = M2MixById(id, i, i.color);

    float3 n = normalize(i.normalWS);
    n = (dot(n, pass.cameraPosWS.xyz - i.worldPos) < 0.0) ? -n : n;

    s.albedo = full.rgb - unlit.rgb;
    s.emissive = unlit.rgb;
    s.normalWS = n;
    s.vertexNormalWS = n;
    s.opacity = shaded.a;
    float lights = any(draw.lightDiffuse.rgb > 0.0) ? 1.0 : 0.0;
    for (int L = 0; L < kM2PointLights; ++L)
        lights += any(draw.pointColor[L].rgb > 0.0) ? 1.0 : 0.0;
    s.lightCount = lights;
    // No specular term exists in this light model, so Specular Only is black.
    let lighting = DebugLightingOverride(d);
    s.lit = (lighting == 3u) ? float3(0.0, 0.0, 0.0)
                             : DebugOverrideAlbedo(d, float3(1.0, 1.0, 1.0)) * M2Light(i);
    s.present = kDbgHasMip | kDbgHasAlbedo | kDbgHasEmissive | kDbgHasNormal |
                kDbgHasVertexNormal | kDbgHasLightCount | kDbgHasLit;
    return DebugLegacy(s, d, i.position.xy);
}
