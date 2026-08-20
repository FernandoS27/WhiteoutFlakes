// ============================================================================
// WhiteoutFlakes Renderer — M3 simple material (StarCraft II / HotS `.m3`).
//
// The shader half of M3StandardShading (M3_SIMPLE_MATERIAL_DESIGN.md). NOT the
// retail permutation machine: seven of StandardMaterial's eighteen layers
// (diffuse, decal, specular, both emissives, normal, alpha mask), the retail
// lighting model (lighting.fx: shadowed Blinn key light + ambient + two
// diffuse-only fills, spec gated by sign(N.L)), the retail team-colour paths
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

// Layer slots, in M3Surface order. Fixed seven: absent layers branch off on
// ctl.z and additionally bind the white default, belt and braces. Emissive2
// is its own slot because retail pairs a team-mask emissive1 (TeamColor*Add)
// with the real glow in emissive2.
static const int kM3LayerDiffuse = 0;
static const int kM3LayerDecal = 1;
static const int kM3LayerSpecular = 2;
static const int kM3LayerEmissive = 3;
static const int kM3LayerEmissive2 = 4;
static const int kM3LayerNormal = 5;
static const int kM3LayerAlphaMask = 6;
static const int kM3LayerCount = 7;

struct M3DrawData
{
    float4x4 world;
    // .x alphaRef (0 = no test), .y unshaded, .z specular exponent,
    // .w flag bits: 1 = DoubleLambert (N.L remapped to 2*N.L - 1),
    //               2 = TwoSided (back faces flip the shading normal).
    float4 params0;
    // `.m3` UVs are i16/2048 declared R16G16B16A16_SNORM, so the sampled value
    // is raw/32767 and the true coordinate is sample * (32767 * uvMultiply) +
    // uvOffset, where REGN v5+ carries per-region uvMultiply/uvOffset and older
    // regions imply 1/2048 and 0. The host folds the 32767 in; .x mul, .y add.
    // .z = material hdrEmissiveMultiplier (psmaterial.fx applies it to the
    // Add-family emissive sum, team adds included, so it cannot ride a tint).
    float4 uvTransform;
    // The instance's palette pair (gamedata.xml <TeamColors>): p_cDiffuseTeamColor
    // and p_cEmissiveTeamColor. .rgb only.
    float4 teamDiffuse;
    float4 teamEmissive;
    // .rgb = layer colour tint x rgbMultiply (the specular tint also folds
    // hdrSpecularMultiplier and the energy-conserving dim), .a = layer alpha.
    float4 layerTint[kM3LayerCount];
    // .x = uv set (0/1) in the low nibble, sampler wrap variant (0..3, the
    //      layer's UVWrapX/Y bits) in bits 4-5;
    // .y ColorChannelSelect; .z mode (0 off, 1 texture, 2 solid colour —
    // TextureLayerFlag 0x400); .w = LayerBlendOp for the decal and emissive
    // slots, TEAMCOLOR_DIFFUSE mode (1) for the diffuse slot.
    uint4 layerCtl[kM3LayerCount];
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
// One sampler PER WRAP VARIANT, not per layer: the D3D12 root signature (and
// its siblings) budget four samplers per stage, and four is also exactly how
// many distinct states exist — wrap/clamp x U/V. Slot = the layer's wrap bits.
[[vk::binding(WF_PS_BIND_OFFSET + 0, 2)]] SamplerState sampWrap0 : register(s0);
[[vk::binding(WF_PS_BIND_OFFSET + 1, 2)]] SamplerState sampWrap1 : register(s1);
[[vk::binding(WF_PS_BIND_OFFSET + 2, 2)]] SamplerState sampWrap2 : register(s2);
[[vk::binding(WF_PS_BIND_OFFSET + 3, 2)]] SamplerState sampWrap3 : register(s3);

// ---- Vertex side -----------------------------------------------------------

// The `.m3` record as the adapter describes it: normal.w is a second
// handedness byte the lib ignores in favour of tangent.w, and both UV sets
// ride one float4 for the TEXCOORD1 -> TEXCOORD10 toolchain reason
// m2_combiners.slang documents.
struct VSInput
{
    float3 position    : POSITION;
    float4 boneWeights : BLENDWEIGHT;  // R8G8B8A8_UNORM
    uint4  boneIndices : BLENDINDICES; // R8G8B8A8_UINT
    float4 normal      : NORMAL;       // .xyz normal, .w unused here
    float4 uvs         : TEXCOORD0;    // .xy set 0, .zw set 1 (SNORM raw)
    float4 tangent     : TANGENT;      // .xyz tangent, .w handedness sign
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
    let n = float4(input.normal.xyz, 0.0);
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

PSInput M3Vertex(float3 pos, float3 nrm, float3 tan, float tanSign, float4 rawUvs)
{
    PSInput o;
    let worldPos = mul(float4(pos, 1.0), draw.world);
    let viewPos = mul(worldPos, pass.view);
    o.position = mul(viewPos, pass.projection);
    o.worldPos = float4(worldPos.xyz, viewPos.z);
    // Uniform scale only in the world transform, so no inverse transpose.
    //
    // Negated: `.m3` stores the vertex normal anti-parallel to its own triangle
    // winding, where `.m2` — same renderer, same rasterizer state — stores it
    // parallel. Measured over both corpora and pinned by
    // normal_orientation_test: 4.8% winding agreement against `.m2`'s 99.7%.
    // Corrected here rather than in the adapter so the baked buffer stays a
    // verbatim copy of the file's bytes, which mesh_buffer_test asserts.
    o.normalWS = normalize(mul(float4(-nrm, 0.0), draw.world).xyz);
    o.tangentWS = float4(normalize(mul(float4(tan, 0.0), draw.world).xyz), tanSign);
    o.uv01 = rawUvs * draw.uvTransform.x + draw.uvTransform.y;
    return o;
}

[shader("vertex")]
PSInput vsM3Standard(VSInputRigid input)
{
    let s = (input.tangent.w < 0.0) ? -1.0 : 1.0;
    return M3Vertex(input.position, input.normal.xyz, input.tangent.xyz, s, input.uvs);
}

[shader("vertex")]
PSInput vsM3StandardSkinned(VSInput input)
{
    float3 pos, nrm, tan;
    M3Skin(input, pos, nrm, tan);
    let s = (input.tangent.w < 0.0) ? -1.0 : 1.0;
    return M3Vertex(pos, nrm, tan, s, input.uvs);
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
float4 M3LayerValue(uint4 ctl, float4 tint, float4 texel)
{
    float4 v = SelectChannels(texel, ctl.y);
    if (ctl.z == 2u)
        v = float4(1.0, 1.0, 1.0, 1.0);
    if (ctl.w == 1u && ctl.y != 0u)
    {
        let mask = saturate(pow(max(texel.a, 0.0), pass.teamParams.x));
        v = float4(lerp(draw.teamDiffuse.rgb, v.rgb, mask), 1.0);
    }
    return v * tint;
}

float2 M3LayerUV(uint4 ctl, float4 uv01)
{
    return ((ctl.x & 0xFu) == 1u) ? uv01.zw : uv01.xy;
}

// Sample @p t with the layer's wrap variant. The selector is a per-draw
// constant, so the branch is uniform and the implicit gradients are safe.
float4 M3SampleLayer(Texture2D t, uint4 ctl, float4 uv01)
{
    let uv = M3LayerUV(ctl, uv01);
    switch ((ctl.x >> 4) & 0x3u)
    {
    case 1u: return t.Sample(sampWrap1, uv);
    case 2u: return t.Sample(sampWrap2, uv);
    case 3u: return t.Sample(sampWrap3, uv);
    default: return t.Sample(sampWrap0, uv);
    }
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

    // Albedo: diffuse layer, then the decal over it.
    float4 albedo = float4(1.0, 1.0, 1.0, 1.0);
    let ctlDiffuse = draw.layerCtl[kM3LayerDiffuse];
    if (ctlDiffuse.z != 0u)
        albedo = M3LayerValue(ctlDiffuse, draw.layerTint[kM3LayerDiffuse],
                              M3SampleLayer(texDiffuse, ctlDiffuse, input.uv01));
    let ctlDecal = draw.layerCtl[kM3LayerDecal];
    if (ctlDecal.z != 0u)
    {
        let decal = M3LayerValue(ctlDecal, draw.layerTint[kM3LayerDecal],
                                 M3SampleLayer(texDecal, ctlDecal, input.uv01));
        albedo.rgb = M3ApplyBlendOp(albedo.rgb, decal, ctlDecal.w);
    }

    // Material alpha (psmaterial.fx:354-390): the alpha-mask layer times the
    // alpha factor — NOT the diffuse alpha, which is the team-colour mask.
    // Then the test; >= survives, as everywhere else in this renderer.
    float alpha = 1.0;
    let ctlMask = draw.layerCtl[kM3LayerAlphaMask];
    if (ctlMask.z != 0u)
        alpha = M3LayerValue(ctlMask, draw.layerTint[kM3LayerAlphaMask],
                             M3SampleLayer(texAlphaMask, ctlMask, input.uv01)).a;
    if (draw.params0.x > 0.0 && alpha < draw.params0.x)
        discard;

    // Shading normal. SC2 normal maps are DXT5nm — x rides alpha, y green —
    // so the decode is .ag, with z reconstructed.
    float3 n = normalize(input.normalWS);
    let ctlNormal = draw.layerCtl[kM3LayerNormal];
    if (ctlNormal.z == 1u)
    {
        let t = normalize(input.tangentWS.xyz);
        let b = cross(n, t) * input.tangentWS.w;
        float2 nxy = M3SampleLayer(texNormal, ctlNormal, input.uv01).ag * 2.0 - 1.0;
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
    let toEye = normalize(pass.cameraPosWS.xyz - input.worldPos.xyz);
    if (twoSided && !isFront)
        n = -n;

    // Specular colour: the layer's channels (the material's SpecularMode is
    // folded into ctl.y host-side), hdrSpecularMultiplier folded into the tint.
    float3 specColor = float3(0.0, 0.0, 0.0);
    let ctlSpec = draw.layerCtl[kM3LayerSpecular];
    if (ctlSpec.z != 0u)
        specColor = M3LayerValue(ctlSpec, draw.layerTint[kM3LayerSpecular],
                                 M3SampleLayer(texSpecular, ctlSpec, input.uv01)).rgb;

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
        let specPower = max(draw.params0.z, 1.0);

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
        let emis = M3LayerValue(ctlEmis, draw.layerTint[kM3LayerEmissive],
                                M3SampleLayer(texEmissive, ctlEmis, input.uv01));
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
        let emis = M3LayerValue(ctlEmis2, draw.layerTint[kM3LayerEmissive2],
                                M3SampleLayer(texEmissive2, ctlEmis2, input.uv01));
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
    return float4(s.color, s.alpha);
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
