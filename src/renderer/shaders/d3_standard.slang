// ============================================================================
// WhiteoutFlakes Renderer — D3 standard material (Diablo III actors).
//
// The shader half of D3StandardShading. **Not** a translation of the original's
// programs: a `.shd` is a RenderPass table plus {offset, size} pairs into a blob
// of compiled GPU binaries, and those binaries — the 9,672 `pscod` and 4,845
// `vscod` assets beside it — cannot be executed, translated or reflected into
// this pipeline.
//
// The programs can nonetheless be *read*. The game ships the OpenGL build of
// the same `.fx` sources as 7,236 plain-text ARB assemblies under
// `OpenGLShaders/`, named for the `Shaders` asset that binds them, with their
// sampler names in a string table. So the terms below are transcribed from the
// original's own arithmetic rather than inferred from the material struct, and
// where a comment cites a program by name — `scene_opaque`, `actor2_alphatest`,
// `actor_complex_Transparent_Ground` — that program is the evidence.
//
// What is still not reproduced is the *shape* of the original's shading: it has
// a program per material family, we have one, and the 86-slot `eShaderConstant`
// system feeding them has no consumer here. This implements the semantics those
// programs agree on, in one pass — the call M3_SIMPLE_MATERIAL_DESIGN made for
// StarCraft II.
//
// The material semantics are fixed-function against `MaterialColors`, because
// that is literally what the struct is: `{ diffuse, specular, emissive,
// ambient, shininess, flags }`, four float4 and a float. Deliberately NOT
// remapped to roughness/metalness: D3's material has no such parameters, and
// inventing them would be inventing data.
//
// ---- The light model is the original's, transcribed --------------------------
//
// `Render_UploadLightConstants` resolves the bound pass to a variant holding
// five clamped light counts and writes each scene light into its type's array:
// 0 directional (3 float4), 1 point (5), 2 spot (6), 3 cylindrical (6),
// 4 point-linear (4). `colAmbient` is separate and is a SUM — the engine starts
// it from a scene base and adds every directional light's own ambient onto it.
//
// Two things make that reproducible in one loop:
//
//  * **A directional is a point with `w` = 0.** When the directional array is
//    full the engine re-homes the light into the point array with its direction
//    negated and attenuation (1, 0, 0) — `L = pos.xyz - P * pos.w` then reads a
//    constant direction and the reciprocal is 1. The shipped vertex programs
//    rely on this: they isolate the directional term for shadowing as
//    `diffuse * (1 - pos.w)`, which only works because both live in one array.
//
//  * **Every shipped forward program has at most one point block.** Measured
//    over the corpus's 1,466 distinct vertex programs: 796 have exactly one,
//    670 have none, none has two. Plus one cylindrical on 470 of them. The
//    16-per-type ceiling is a bound nothing reaches.
//
// Per light, from `scene_foliage_alphatest` and its `_spec` sibling, which
// differ only by the specular term:
//
//     L      = lightPos.xyz - P * lightPos.w      ; w = 0 -> a direction
//     atten  = 1 / (k0 + k1*d + k2*d*d)
//     ambient += lightAmbient * atten             ; the per-light ambient is
//     diffuse += lightDiffuse * max(N.L, 0) * atten   ; attenuated too
//     spec    += lightSpecular * pow(max(dot(L, reflect(V, N)), 0), P) * atten
//
// and for the cylindrical light, whose axis the engine builds by pushing world
// +Z through the view rotation:
//
//     Lc   = cylPos.xyz - P
//     perp = sqrt(|Lc|^2 - dot(Lc, axis)^2)       ; distance from the axis
//     f    = saturate((end - perp) * invRange)
//     ambient += cylAmbient * f
//     diffuse += cylDiffuse * max(dot(N, normalize(Lc)), 0) * f
//
// Three deliberate differences from the original, each for one reason:
//
//  1. **Per pixel, not per vertex.** The original evaluates all of the above in
//     the VERTEX program and hands the pixel program two interpolants (the
//     total light, halved so it survives the clamped colour register, and the
//     ambient half unclamped in a texcoord). This shader is normal-mapped, so
//     the light has to be evaluated where the normal is.
//  2. **World space, not view space.** The engine transforms every light by the
//     view matrix and works with the eye at the origin. The arithmetic above is
//     invariant under that rigid transform; only the specular's view vector
//     changes form, from `normalize(P)` to `normalize(P - eye)`.
//  3. **Specular is PHONG, not Blinn.** `scene_foliage_alphatest_spec` builds
//     `R = P - 2N(P.N)` and takes `dot(L, R)`. The exponent is the engine's
//     `SpecularPower` constant, a global, not the material's `flShininess`.
//
// An *engine* shader on the embedded slangc path, for m2_combiners' reason:
// `bls::PsoRequest` needs a `const BlsProgram*`, and producing a new `.bls`
// bundle needs WDX_BUILD_WC3_SHADERS — mutually exclusive with the
// WDX_USE_PREBUILT_SHADERS every fresh clone builds with.
//
// Slot presence is a uniform branch (`slotCtl[i].z`), not a permutation, for
// the same reason m2_combiners argues: a 2^9 matrix in compiled_shaders.h would
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
static const int kD3SlotAlphaMask0 = 6;
static const int kD3SlotAlphaMask1 = 7;
static const int kD3SlotAlphaMask2 = 8;
static const int kD3SlotCount = 9;

// ---- Constant buffers ------------------------------------------------------
//
// Split by update frequency, both bound to both stages (Vulkan and WebGPU build
// one descriptor set from the shared layout — see m2_combiners.slang).

// The engine's light budget is authored per RenderPass as tag-map entries that
// Render_EnsureShaderVariant clamps to 16 each; nothing shipped uses more than
// one point and one cylindrical. Four leaves a rig room to add fill.
static const int kD3MaxLights = 4;

struct D3PassData
{
    float4x4 view;
    float4x4 projection;
    float4 cameraPosWS;    // .xyz eye in world space
    float4 colAmbient;     // .rgb — the engine's constant 21, itself a sum
    float4 specularPower;  // .x — constant 39, a GLOBAL and not flShininess
    // One array for directional and point both, exactly as the engine stores
    // them: `.w` 0 makes `lightPos.xyz` a direction and the attenuation (1,0,0)
    // makes the reciprocal 1.
    float4 lightPos[kD3MaxLights];      // .xyz, .w 1 = point / 0 = direction
    float4 lightAtten[kD3MaxLights];    // k0, k1, k2 | .w 1 = slot live
    float4 lightAmbient[kD3MaxLights];  // .rgb, attenuated with the diffuse
    float4 lightDiffuse[kD3MaxLights];  // .rgb
    float4 lightSpecular[kD3MaxLights]; // .rgb
    float4 cylPos;     // .xyz, .w 1 = live
    float4 cylAxis;    // .xyz, unit
    float4 cylRange;   // .x = 1/(end - start), .y = end
    float4 cylAmbient; // .rgb
    float4 cylDiffuse; // .rgb
}

struct D3DrawData
{
    float4x4 world;
    // .x alphaTestRef (0 = no test; RenderPass+60 x 1/255 in the original),
    // .y shininess, .z two-sided, .w how the pass's vertex program consumes the
    // vertex colour: bit 0 = its RGB is added into the light sum, bit 1 = its
    // alpha is the surface's. See D3PassState.
    float4 params0;
    // The fixed-function chain's output gain: .x colour, .y alpha. `Legacy.fx`
    // stages carry MODULATE2X / MODULATE4X steps and they multiply; 1 where
    // nothing scales, which is every other family. See D3PassState::colorGain.
    float4 params1;
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
// The alpha-mask layers. Their ALPHA multiplies into the surface's own and
// their colour is never read — `actor_complex_Transparent_Ground` is the whole
// rule in five instructions: `alpha = tex(type12).w * tex(type14).w * matAlpha`,
// with the base map contributing rgb only.
[[vk::binding(WF_PS_BIND_OFFSET + 6, 1)]] Texture2D texAlphaMask0 : register(t6);
[[vk::binding(WF_PS_BIND_OFFSET + 7, 1)]] Texture2D texAlphaMask1 : register(t7);
[[vk::binding(WF_PS_BIND_OFFSET + 8, 1)]] Texture2D texAlphaMask2 : register(t8);
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

// Which channels of a slot's sample the surface takes, where the bound pass
// said so per stage: bit 0 its RGB, bit 1 its ALPHA, 0 = it did not say. Only a
// `Legacy.fx` pass does; every other family leaves this clear and the slot keeps
// the default its type implies.
uint SlotChannels(int slot)
{
    return draw.slotCtl[slot].w;
}

bool SlotOn(int slot)
{
    return draw.slotCtl[slot].z != 0u;
}

// One mask layer's contribution, as a multiplier on (rgb, a). A slot the pass
// said nothing about is alpha-only, which is what types 12/14/19 are on every
// family that compiles its own program.
float4 MaskWeight(uint channels, float4 s)
{
    let rgb = (channels & 0x1u) != 0u ? s.rgb : float3(1.0, 1.0, 1.0);
    let a = (channels == 0u || (channels & 0x2u) != 0u) ? s.a : 1.0;
    return float4(rgb, a);
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
    //
    // THE VERTEX COLOUR NEVER MULTIPLIES THE ALBEDO. It used to, and that is
    // what rendered most of the game's meshes solid black: the static families
    // bake the level's light into the attribute and ship (0, 0, 0) when there
    // is no bake — 84.6% of shipped sub-objects — so multiplying it away leaves
    // nothing. Every family reads it as a LIGHT term instead, in one of three
    // ways the pass decides between (see the lighting block below): `vs_scene`
    // adds it into the light sum, `vs_irrad_*` reads only its alpha, and an
    // unlit program uses it AS the light. None of them touches the albedo.
    float4 albedo = draw.matDiffuse;
    if (SlotOn(kD3SlotDiffuse))
        albedo *= SampleSlot(kD3SlotDiffuse, texDiffuse, input.uv01);

    let vertexColorMode = uint(draw.params0.w);
    if ((vertexColorMode & 0x2u) != 0u)
        albedo.a *= input.vertexCol.a;

    // The alpha-mask layers multiply into the surface's alpha. This is where a
    // transparent D3 surface gets its shape: the base map of a smoke plume or a
    // wing is opaque, and the opacity comes from one to three separate masks.
    //
    // "Alpha mask" is the majority reading of types 12/14/19 and not a property
    // of the type: a `Legacy.fx` pass states per stage which channels its
    // texture feeds, and Imperius's wings multiply type 14's RGB into the
    // colour as well as its alpha into the alpha. Where the pass said so, that
    // wins; where it did not, `channels` is 0 and the alpha-only default holds.
    // Spelled out rather than looped: a dynamic texture select inside an
    // unrolled loop is what fxc refuses to unroll.
    if (SlotOn(kD3SlotAlphaMask0))
        albedo *= MaskWeight(SlotChannels(kD3SlotAlphaMask0),
                             SampleSlot(kD3SlotAlphaMask0, texAlphaMask0, input.uv01));
    if (SlotOn(kD3SlotAlphaMask1))
        albedo *= MaskWeight(SlotChannels(kD3SlotAlphaMask1),
                             SampleSlot(kD3SlotAlphaMask1, texAlphaMask1, input.uv01));
    if (SlotOn(kD3SlotAlphaMask2))
        albedo *= MaskWeight(SlotChannels(kD3SlotAlphaMask2),
                             SampleSlot(kD3SlotAlphaMask2, texAlphaMask2, input.uv01));

    // The chain's output gain, alpha half. Saturated because the fixed-function
    // pipeline clamps every stage and because this value is a blend factor: the
    // wings' x4 is what turns `diffuse.a * mask12.a * mask14.a` from a handful
    // of surviving peaks into the sheet the armour actually wears.
    albedo.a = saturate(albedo.a * draw.params1.y);

    // Against the whole alpha, masks included: the original's alpha test runs on
    // the value the pass is about to blend, not on the base map alone.
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

    // Specular needs the MAP, not just the material colour. Measured over 200
    // corpus `.app` (1,764 materials): `vSpecular` is 1.0 on 99.0% of them and
    // `flShininess` is 0.0 on 99.4%. Neither carries a strength — they are a
    // tint and an unset field — so the per-pixel strength lives in the
    // specular map, and with the slot unresolved there is no signal at all.
    //
    // The shipped `_spec` programs agree: they multiply the vertex specular by
    // the GLOSS map (type 5) and add the product after the albedo multiply,
    // never before. An absent slot therefore means NO specular rather than full
    // white specular — taking the tint raw put a broad glossy highlight on
    // every surface in the game, which is a fabricated term, not a dim one.
    float3 specTint = float3(0.0, 0.0, 0.0);
    if (SlotOn(kD3SlotSpecular))
        specTint = draw.matSpecular.rgb * SampleSlot(kD3SlotSpecular, texSpecular, input.uv01).rgb;

    // ---- The light sum, the original's shape -------------------------------
    //
    // Split in two because the original splits it: the ambient half rides a
    // texcoord unclamped and the pixel program can substitute it for the whole
    // sum when a surface is in shadow, so ambient and diffuse are never one
    // number in a shipped program either.
    float3 ambient = float3(0.0, 0.0, 0.0);
    float3 diffuseSum = float3(0.0, 0.0, 0.0);
    float3 specSum = float3(0.0, 0.0, 0.0);

    // AN UNLIT PASS TAKES NO LIGHT AND ITS VERTEX COLOUR IS THE LIGHT. The pass
    // says which it is (`D3PassState::lit`, tag 0xA000F, measured to separate
    // the corpus perfectly), and an unlit vertex program ends
    // `MOV result.color, vertex.attrib[3]` — the attribute verbatim, which the
    // pixel program then multiplies its texture chain by. Imperius's wings are
    // this: `Legacy.fx`, unlit, vertex colour white on all 726 vertices. Lighting
    // them with the scene's rig is what drew them as a dark sheet.
    if ((vertexColorMode & 0x4u) != 0u)
    {
        ambient = input.vertexCol.rgb;
    }
    else
    {
        // `colAmbient` lights the surface directly and `vAmbient` ADDS to it —
        // it does not gate it. Measured over 200 corpus `.app` (1,764
        // materials): `vAmbient` is all-zero on 98.1% of them, so multiplying
        // the two would erase the scene's ambient on essentially every D3
        // surface, which is what used to leave the unlit side black.
        ambient = pass.colAmbient.rgb + draw.matAmbient.rgb;

        // The baked per-vertex light, for the families whose vertex program
        // reads it. `vs_scene` closes `MAD R0.xyz, vertex.attrib[3], 2, R0` /
        // `MUL result.color.xyz, R0, 0.5`, and the pixel program opens
        // `MUL R2, fragment.color.primary, 2` — the halving is only there to
        // survive the clamped colour register, so the attribute enters the
        // light sum at TWICE unit weight, not once. Measured on 148 of the 167
        // Scene.fx vertex programs. Gated because the actor families leave the
        // attribute at white and compute their light in the vertex shader.
        if ((vertexColorMode & 0x1u) != 0u)
            ambient += input.vertexCol.rgb * 2.0;

        // Phong, not Blinn: the shipped `_spec` programs reflect the eye-to-
        // surface vector about the normal and dot it with L. In view space the
        // eye is the origin, so their `R = P - 2N(P.N)` is this `reflect(-V, N)`.
        let R = reflect(-V, N);
        let specPower = max(pass.specularPower.x, 1.0);

        [unroll]
        for (int i = 0; i < kD3MaxLights; ++i)
        {
            // `w` = 1 makes this a point light and `w` = 0 a directional whose
            // `.xyz` is already the direction TOWARD the light — the engine
            // negates it on the way in.
            let lp = pass.lightPos[i];
            let d = lp.xyz - input.worldPos.xyz * lp.w;
            let dist = length(d);
            let L = (dist > 1e-4) ? d / dist : float3(0.0, 0.0, 1.0);
            let a = pass.lightAtten[i];
            // `.w` is the live bit, so a dead slot attenuates to zero without
            // the shader testing a count — the engine clears unused slots from
            // a neutral template for the same reason.
            let atten = a.w / max(a.x + a.y * dist + a.z * dist * dist, 1e-4);
            ambient += pass.lightAmbient[i].rgb * atten;
            let ndl = saturate(dot(N, L));
            diffuseSum += pass.lightDiffuse[i].rgb * (ndl * atten);
            specSum += pass.lightSpecular[i].rgb * (pow(saturate(dot(L, R)), specPower) * atten);
        }

        // The cylindrical light — D3's hero light, a vertical tube around a
        // point with a linear radial falloff. Its ambient half is what keeps a
        // character's shadow side off the floor of the frame.
        {
            let lc = pass.cylPos.xyz - input.worldPos.xyz;
            let axial = dot(lc, pass.cylAxis.xyz);
            let perp = sqrt(max(dot(lc, lc) - axial * axial, 0.0));
            let f = saturate((pass.cylRange.y - perp) * pass.cylRange.x) * pass.cylPos.w;
            ambient += pass.cylAmbient.rgb * f;
            let lcLen = length(lc);
            let L = (lcLen > 1e-4) ? lc / lcLen : float3(0.0, 0.0, 1.0);
            diffuseSum += pass.cylDiffuse.rgb * (saturate(dot(N, L)) * f);
        }

    }

    // THE IRRADIANCE FAMILY'S AMBIENT CARRIES A FACTOR OF `2 * cube`, AND THIS
    // TREATS THE PAIR AS UNITY. `actor2_opaque` and `actor2_alphatest` — the
    // two commonest actor programs, and they agree instruction for instruction
    // — compose as
    //
    //     light = irradCube(N) * (secondary * 2) + primary
    //     color = light * albedo * c[4] * 2 + rim
    //
    // with the vertex program writing `secondary = ambient * 0.5` and
    // `primary = direct * 0.5`. Multiplied out, the ambient half arrives at
    // `2 * cube` and the direct half at `1`. The `2` is a literal in the
    // program; the cube is a texture this slot cannot sample — type 8 is a CUBE
    // read through `matWorldIrrad` and the slot is a Texture2D.
    //
    // So the factor is known and its companion is not, and the two choices for
    // an unsampled multiplicand disagree by 2x: the mechanical neutral for an
    // unbound multiply is 1, which doubles every character's ambient, while an
    // irradiance map normalised so a flat white environment integrates to 0.5
    // cancels the literal exactly. Taking the first would light every
    // ActorIrrad surface at twice the ambient of the Scene.fx prop standing
    // beside it, which is the one thing the pair certainly does not do — both
    // families light the same world. So: unity, stated, and the place to revisit
    // the day the cube is sampled for real.

    // The lightmap (type 2) multiplies the lit result — that is what a baked
    // lighting term is — and its unresolved default is `default_lightmap`,
    // i.e. neutral, which `SlotOn` false stands in for.
    float3 lightmap = float3(1.0, 1.0, 1.0);
    if (SlotOn(kD3SlotLightmap))
        lightmap = SampleSlot(kD3SlotLightmap, texLightmap, input.uv01).rgb;

    // Emissive SELF-ILLUMINATES THE SURFACE — it scales the albedo alongside
    // the light terms rather than being added as raw light on top of them.
    //
    // Unlike `vSpecular`, this is real authored data and cannot be gated away:
    // measured over 200 corpus `.app` (1,764 materials), `vEmissive` is zero on
    // 65.0%, between 0 and 1 on 34.5%, and >= 1.0 on 0.5%, mean 0.151. But
    // `color += emissive` makes any material at 1.0 a PURE WHITE pixel whatever
    // its texture says, and that is what rendered Diablo as a white silhouette:
    // his body material carries emissive 1.0, and one material covering most of
    // the screen was 80.9% pure white on its own.
    //
    // Folding it into the lighting term keeps the surface's own colour — a
    // fully-emissive Diablo reads as an unshaded Diablo rather than as a hole
    // in the frame — and leaves the 65% that author zero completely unchanged.
    // The GLOW MAP (type 6) enters the LIGHT sum, so it is still bounded by the
    // albedo: `actor2_opaque_glow_skin` is `albedo * (light * 2 + glow * k)`.
    // `scene_opaque_glow` adds it a step later, outside the albedo — the two
    // differ only where the albedo is dark, and inside is the safer of the two
    // to be wrong about.
    //
    // The host only fills this slot for the families that ADD it at all: the
    // Legacy programs multiply it instead, and adding where they multiply
    // washed a wing of fire out to white smoke.
    // Where the pass STATES that its glow stage feeds the colour, it means the
    // fixed-function multiply, not this add: `actor_glowTendril_cm2x_bloom_skin`
    // closes on `2 * vcol * glow.rgb * diffuse.rgb * mask14.rgb` and there is no
    // light term anywhere in it. Dropping the map entirely was the safe answer
    // while the two Legacy programs disagreed; the stage block is what settles
    // it, and the wings were a dim smear without it.
    float3 glowMul = float3(1.0, 1.0, 1.0);
    float3 emissive = draw.matEmissive.rgb;
    if (SlotOn(kD3SlotEmissive))
    {
        let g = SampleSlot(kD3SlotEmissive, texEmissive, input.uv01).rgb;
        if ((SlotChannels(kD3SlotEmissive) & 0x1u) != 0u)
            glowMul = g;
        else
            emissive += g;
    }

    float3 color = albedo.rgb * glowMul * (ambient + diffuseSum * lightmap + emissive) +
                   specSum * specTint;
    // The chain's output gain, colour half. Not saturated: the original does not
    // either — its `MUL result.color.xyz, R0, c[0].y` runs straight into a UNORM
    // target, and clamping here would cost the HDR path its headroom.
    color *= draw.params1.x;

    // matDiffuse.a once, not twice: the element/fade alpha rides on it host-side
    // and it was being squared here.
    return float4(color, albedo.a);
}
