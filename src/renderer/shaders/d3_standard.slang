// ============================================================================
// WhiteoutFlakes Renderer — D3 standard material (Diablo III actors).
//
// The shader half of D3StandardShading. **Not** a translation of the original's
// programs: a `.shd` is a RenderPass table plus {offset, size} pairs into a blob
// of compiled GPU binaries, and the programs themselves are the 9,672 `pscod`
// and 4,845 `vscod` assets beside it — none of which can be executed,
// translated or reflected into this pipeline. Every one of the 86
// `eShaderConstant` slots is written only if the bound program declares it, so
// without the programs the whole constant system has no consumers.
//
// So this implements the material's **semantics**, which the data states, and
// not its program, which we cannot read — the same call M3_SIMPLE_MATERIAL_DESIGN
// made for StarCraft II.
//
// The semantics are Blinn-Phong against `MaterialColors`, because that is
// literally what the struct is: `{ diffuse, specular, emissive, ambient,
// shininess, flags }`, four float4 and a float — a fixed-function material.
// Deliberately NOT remapped to roughness/metalness: D3's material has no such
// parameters, and inventing them would be inventing data.
//
// An *engine* shader on the embedded slangc path, for m2_combiners' reason:
// `bls::PsoRequest` needs a `const BlsProgram*`, and producing a new `.bls`
// bundle needs WDX_BUILD_WC3_SHADERS — mutually exclusive with the
// WDX_USE_PREBUILT_SHADERS every fresh clone builds with.
//
// Slot presence is a uniform branch (`slotCtl[i].z`), not a permutation, for
// the same reason m2_combiners argues: a 2^6 matrix in compiled_shaders.h would
// buy back a few scalar selects.
// ============================================================================

// ---- Slots -----------------------------------------------------------------
//
// D3SlotKind, in shader order. An UberMaterial's texture entries are not an
// ordered layer list — each carries an EMaterialTextureType and the engine
// binds it to whichever *stage* of the bound pass declares that type — so the
// host resolves types to these slots and the shader only ever sees slots.
static const int kD3SlotDiffuse = 0;
static const int kD3SlotNormal = 1;
static const int kD3SlotSpecular = 2;
static const int kD3SlotEmissive = 3;
static const int kD3SlotLightmap = 4;
static const int kD3SlotIrradiance = 5;
static const int kD3SlotCount = 6;

// ---- Constant buffers ------------------------------------------------------
//
// Split by update frequency, both bound to both stages (Vulkan and WebGPU build
// one descriptor set from the shared layout — see m2_combiners.slang).

struct D3PassData
{
    float4x4 view;
    float4x4 projection;
    float4 cameraPosWS;      // .xyz eye in world space
    float4 keyLightDir;      // .xyz direction the light TRAVELS, normalised
    float4 keyLightDiffuse;  // .rgb
    float4 keyLightSpecular; // .rgb
    float4 ambient;          // .rgb scene ambient
    // Three point lights. The original's budget is authored per RenderPass and
    // clamped to 16 per type across five types (directional / point / spot /
    // cylindrical / point-linear); we have no passes, so v1 is a fixed
    // 1 directional + 3 point matching what the profile's rig supplies.
    float4 pointPos[3];   // .xyz world position, .w 1 = live
    float4 pointColor[3]; // .rgb
    float4 pointAtten[3]; // 1 / (x + y*d + z*d*d)
}

struct D3DrawData
{
    float4x4 world;
    // .x alphaTestRef (0 = no test; RenderPass+60 x 1/255 in the original),
    // .y shininess, .z two-sided, .w unused.
    float4 params0;
    // MaterialColors, verbatim.
    float4 matDiffuse;
    float4 matSpecular;
    float4 matEmissive;
    float4 matAmbient;
    // Per-slot texture matrix — the engine's matTex0..matTex11, which is why
    // the host caps at twelve entries. Row-vector: uv' = (uv, 0, 1) . M.
    float4x4 slotUv[kD3SlotCount];
    // .x uv set (0/1) in the low nibble and the sampler wrap variant in bits
    // 4-5; .y unused; .z mode (0 = slot absent, 1 = sample the texture);
    // .w unused.
    uint4 slotCtl[kD3SlotCount];
}

struct D3BoneData
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

[[vk::binding(0, 0)]] ConstantBuffer<D3PassData> pass : register(b0);
[[vk::binding(1, 0)]] ConstantBuffer<D3DrawData> draw : register(b1);
// Vertex stage only; the rigid entry never references it and Slang strips it
// from that entry's reflection.
[[vk::binding(2, 0)]] ConstantBuffer<D3BoneData> bones : register(b2);

[[vk::binding(WF_PS_BIND_OFFSET + 0, 1)]] Texture2D texDiffuse : register(t0);
[[vk::binding(WF_PS_BIND_OFFSET + 1, 1)]] Texture2D texNormal : register(t1);
[[vk::binding(WF_PS_BIND_OFFSET + 2, 1)]] Texture2D texSpecular : register(t2);
[[vk::binding(WF_PS_BIND_OFFSET + 3, 1)]] Texture2D texEmissive : register(t3);
[[vk::binding(WF_PS_BIND_OFFSET + 4, 1)]] Texture2D texLightmap : register(t4);
[[vk::binding(WF_PS_BIND_OFFSET + 5, 1)]] Texture2D texIrradiance : register(t5);
// One sampler PER WRAP VARIANT, not per slot: the D3D12 root signature budgets
// four samplers per stage, and four is exactly how many distinct states exist.
[[vk::binding(WF_PS_BIND_OFFSET + 0, 2)]] SamplerState sampWrap0 : register(s0);
[[vk::binding(WF_PS_BIND_OFFSET + 1, 2)]] SamplerState sampWrap1 : register(s1);
[[vk::binding(WF_PS_BIND_OFFSET + 2, 2)]] SamplerState sampWrap2 : register(s2);
[[vk::binding(WF_PS_BIND_OFFSET + 3, 2)]] SamplerState sampWrap3 : register(s3);

// ---- Vertex side -----------------------------------------------------------
//
// The repacked D3 record (DescribeD3Vertex). Both UV sets arrive as one float4
// for the TEXCOORD1 -> TEXCOORD10 toolchain reason m2_combiners documents, and
// both vertex colours arrive as one UNORM16x4 for the same reason applied to
// COLOR1: `lane = c0 | (c1 << 8)`, recovered exactly because a u16 round-trips
// through f32's 24-bit mantissa.
//
// Skinning weights and indices ride the separate `BoneVertex` stream at input
// slot 1 — the same 8-byte record every skinned WC3 geoset uploads — because
// D3 influence bone indices are GLOBAL skeleton indices and the loader's
// per-geoset palette remap is what turns them into palette slots.

struct VSInputRigid
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 uvs      : TEXCOORD0; // .xy set 0, .zw set 1
    float4 tangent  : TANGENT;   // .xyz tangent, .w bitangent sign
    float4 colors   : COLOR0;    // packed pair, see above
}

struct VSInput
{
    float3 position    : POSITION;
    float3 normal      : NORMAL;
    float4 uvs         : TEXCOORD0;
    float4 tangent     : TANGENT;
    float4 colors      : COLOR0;
    float4 boneWeights : BLENDWEIGHT;  // R8G8B8A8_UNORM
    uint4  boneIndices : BLENDINDICES; // R8G8B8A8_UINT
}

// One interstage signature for both entries, so either VS pairs with the PS.
struct PSInput
{
    float4 position   : SV_POSITION;
    float4 uv01       : TEXCOORD0; // .xy set 0, .zw set 1
    float4 worldPos   : TEXCOORD1; // .xyz world position, .w view-space Z
    float3 normalWS   : TEXCOORD2;
    float4 tangentWS  : TEXCOORD3; // .xyz tangent, .w bitangent sign
    float4 vertexCol  : COLOR0;
    // The second vertex colour (`dwAuxColor`). Carried rather than silently
    // dropped: the fixed-function pipeline had a use for it and we have not
    // recovered which stage consumed it, so it reaches the pixel shader and no
    // term reads it. Deleting it would make recovering it later a change to the
    // vertex record, the input layout and both entry points.
    float4 auxCol     : COLOR1;
}

// Unpack the two colours the vertex record packed two-to-a-lane.
void UnpackColorPair(float4 packed, out float4 primary, out float4 aux)
{
    let l0 = uint(round(packed.x * 65535.0));
    let l1 = uint(round(packed.y * 65535.0));
    let l2 = uint(round(packed.z * 65535.0));
    let l3 = uint(round(packed.w * 65535.0));
    primary = float4(float(l0 & 0xFFu), float(l1 & 0xFFu), float(l2 & 0xFFu),
                     float(l3 & 0xFFu)) * (1.0 / 255.0);
    aux = float4(float(l0 >> 8u), float(l1 >> 8u), float(l2 >> 8u), float(l3 >> 8u)) *
          (1.0 / 255.0);
}

// Model space to animated model space, m2_combiners' M2Skin with the tangent
// along for the ride. Weights are normalised at pack time; unused lanes carry
// weight 0 and index 0, so every read is in range.
void D3Skin(VSInput input, out float3 pos, out float3 nrm, out float3 tan)
{
    pos = float3(0.0, 0.0, 0.0);
    nrm = float3(0.0, 0.0, 0.0);
    tan = float3(0.0, 0.0, 0.0);
    let p = float4(input.position, 1.0);
    let n = float4(input.normal, 0.0);
    let t = float4(input.tangent.xyz, 0.0);
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

PSInput D3Vertex(float3 pos, float3 nrm, float3 tan, float tanSign, float4 uvs, float4 packedCol)
{
    PSInput o;
    let worldPos = mul(float4(pos, 1.0), draw.world);
    let viewPos = mul(worldPos, pass.view);
    o.position = mul(viewPos, pass.projection);
    o.worldPos = float4(worldPos.xyz, viewPos.z);
    // Uniform scale only in the world transform, so no inverse transpose.
    o.normalWS = normalize(mul(float4(nrm, 0.0), draw.world).xyz);
    o.tangentWS = float4(normalize(mul(float4(tan, 0.0), draw.world).xyz), tanSign);
    o.uv01 = uvs;
    UnpackColorPair(packedCol, o.vertexCol, o.auxCol);
    return o;
}

[shader("vertex")]
PSInput vsD3Standard(VSInputRigid input)
{
    let s = (input.tangent.w < 0.0) ? -1.0 : 1.0;
    return D3Vertex(input.position, input.normal, input.tangent.xyz, s, input.uvs, input.colors);
}

[shader("vertex")]
PSInput vsD3StandardSkinned(VSInput input)
{
    float3 pos, nrm, tan;
    D3Skin(input, pos, nrm, tan);
    let s = (input.tangent.w < 0.0) ? -1.0 : 1.0;
    return D3Vertex(pos, nrm, tan, s, input.uvs, input.colors);
}

// ---- Pixel side ------------------------------------------------------------

float2 SlotUv(int slot, float4 uv01)
{
    let src = (draw.slotCtl[slot].x & 0xFu) != 0u ? uv01.zw : uv01.xy;
    // Row-vector, matching every matrix convention in this renderer and the
    // original's own (BoneXform_ToMatrix4x4 writes basis in rows 0-2 and
    // translation in row 3).
    let t = mul(float4(src, 0.0, 1.0), draw.slotUv[slot]);
    return t.xy;
}

float4 SampleSlot(int slot, Texture2D tex, float4 uv01)
{
    let uv = SlotUv(slot, uv01);
    let wrap = (draw.slotCtl[slot].x >> 4u) & 0x3u;
    switch (wrap)
    {
    case 1u: return tex.Sample(sampWrap1, uv);
    case 2u: return tex.Sample(sampWrap2, uv);
    case 3u: return tex.Sample(sampWrap3, uv);
    default: return tex.Sample(sampWrap0, uv);
    }
}

bool SlotOn(int slot)
{
    return draw.slotCtl[slot].z != 0u;
}

[shader("fragment")]
float4 psD3Standard(PSInput input, bool frontFace : SV_IsFrontFace) : SV_Target
{
    // Unresolved slots fall back to the type's documented default, and the
    // surface still draws. The original's residency gate returns *before* the
    // draw when a bound texture is not resident — an "invisible mesh" in D3 is
    // far more likely a not-yet-streamed texture than a bad transform — and we
    // deliberately do not reproduce it: turning every missing-texture bug into
    // a silent disappearance is the wrong trade in a viewer.
    //
    // The host binds white for an absent 2D slot, so `SlotOn` is what
    // distinguishes "authored and still loading" from "not authored".
    float4 albedo = draw.matDiffuse * input.vertexCol;
    if (SlotOn(kD3SlotDiffuse))
        albedo *= SampleSlot(kD3SlotDiffuse, texDiffuse, input.uv01);

    let alphaRef = draw.params0.x;
    if (alphaRef > 0.0 && albedo.a < alphaRef)
        discard;

    // The tangent frame D3 ships explicitly (dwTangent / dwBinormal). The
    // normal slot's unresolved default is `flat_NM` — the *identity* normal,
    // not black — which is what `SlotOn` false means here; confusing the two
    // makes every un-normal-mapped surface face the viewer.
    float3 N = normalize(input.normalWS);
    // Two-sided flips on the WINDING bit, never on the view vector: a
    // view-vector test disagrees with the rasterizer on any silhouette pixel,
    // which is exactly where a two-sided surface is visible from behind.
    if (draw.params0.z != 0.0 && !frontFace)
        N = -N;
    if (SlotOn(kD3SlotNormal))
    {
        let T = normalize(input.tangentWS.xyz - N * dot(N, input.tangentWS.xyz));
        let B = cross(N, T) * input.tangentWS.w;
        let nm = SampleSlot(kD3SlotNormal, texNormal, input.uv01).xyz * 2.0 - 1.0;
        N = normalize(T * nm.x + B * nm.y + N * nm.z);
    }

    let V = normalize(pass.cameraPosWS.xyz - input.worldPos.xyz);

    // Ambient. The irradiance slot (type 8) is an ambient term when authored;
    // its unresolved default is black, so an absent slot contributes nothing.
    float3 ambient = pass.ambient.rgb * draw.matAmbient.rgb;
    if (SlotOn(kD3SlotIrradiance))
        ambient += SampleSlot(kD3SlotIrradiance, texIrradiance, input.uv01).rgb;

    // Specular needs the MAP, not just the material colour. Measured over 200
    // corpus `.app` (1,764 materials): `vSpecular` is 1.0 on 99.0% of them and
    // `flShininess` is 0.0 on 99.4%. Neither carries a strength — they are a
    // tint and an unset field — so the per-pixel strength lives in the
    // specular map, and with the slot unresolved there is no signal at all.
    //
    // So an absent slot means NO specular rather than full white specular.
    // Taking the tint raw put a broad glossy highlight on every surface in the
    // game, which is a fabricated term, not a dim one: nothing in the file
    // asked for it.
    float3 specTint = float3(0.0, 0.0, 0.0);
    if (SlotOn(kD3SlotSpecular))
        specTint = draw.matSpecular.rgb * SampleSlot(kD3SlotSpecular, texSpecular, input.uv01).rgb;
    let shininess = max(draw.params0.y, 1.0);

    // Key directional.
    float3 diffuseSum = float3(0.0, 0.0, 0.0);
    float3 specSum = float3(0.0, 0.0, 0.0);
    {
        let L = -normalize(pass.keyLightDir.xyz); // toward the light
        let ndl = saturate(dot(N, L));
        diffuseSum += pass.keyLightDiffuse.rgb * ndl;
        if (ndl > 0.0)
        {
            let H = normalize(L + V);
            specSum += pass.keyLightSpecular.rgb * pow(saturate(dot(N, H)), shininess);
        }
    }

    // Three point lights, attenuated by 1 / (k0 + k1*d + k2*d*d). A dead slot
    // carries colour zero and a constant attenuation of 1, so the reciprocal is
    // defined without the shader testing a count.
    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        let d = pass.pointPos[i].xyz - input.worldPos.xyz;
        let dist = length(d);
        let L = (dist > 1e-4) ? d / dist : float3(0.0, 0.0, 1.0);
        let atten = pass.pointPos[i].w /
                    max(pass.pointAtten[i].x + pass.pointAtten[i].y * dist +
                            pass.pointAtten[i].z * dist * dist,
                        1e-4);
        let ndl = saturate(dot(N, L));
        diffuseSum += pass.pointColor[i].rgb * (ndl * atten);
        if (ndl > 0.0)
        {
            let H = normalize(L + V);
            specSum += pass.pointColor[i].rgb * (pow(saturate(dot(N, H)), shininess) * atten);
        }
    }

    // The lightmap (type 2) multiplies the lit result — that is what a baked
    // lighting term is — and its unresolved default is `default_lightmap`,
    // i.e. neutral, which `SlotOn` false stands in for.
    float3 lightmap = float3(1.0, 1.0, 1.0);
    if (SlotOn(kD3SlotLightmap))
        lightmap = SampleSlot(kD3SlotLightmap, texLightmap, input.uv01).rgb;

    float3 color = albedo.rgb * (ambient + diffuseSum * lightmap) + specSum * specTint;

    float3 emissive = draw.matEmissive.rgb;
    if (SlotOn(kD3SlotEmissive))
        emissive *= SampleSlot(kD3SlotEmissive, texEmissive, input.uv01).rgb;
    color += emissive;

    return float4(color, albedo.a * draw.matDiffuse.a);
}
