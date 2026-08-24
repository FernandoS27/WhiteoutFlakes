// ============================================================================
// WhiteoutFlakes Renderer — WoW multi-texture particles
//
// An M2 emitter with the `MultiTexture` flag draws THREE textures through
// three UV sets: the sprite cell for layer 0, and two independently scrolling
// layers on top. The client picks one of four shared shader effects for it
// (`CParticleEmitter2::Init` @0x10169efe0):
//
//     Particle_Unlit_2ColorTex_3AlphaTex_Mod2 / _Mod4
//     Particle_Unlit_3ColorTex_3AlphaTex_Mod2 / _Mod4
//
// which is two independent choices — `CParticleMat` bit 4 (2 vs 3 COLOUR
// textures) and bit 3 (Mod2 vs Mod4 on the ALPHA chain), selected by
// `SetMaterial` @0x1016a42c0. Both arrive here as constants rather than as
// four pipelines, exactly as the retail uber shader carries them
// (`cb0[6].x`).
//
// Transcribed from `pixel/particle_3colortex_3alphatex/uber.hlsl` in
// C:/Projects/Wc3Shaders/wow_re_shaders (its no-TEX_COORD_ANIM, unlit,
// lightMode==0 permutation — the one the M2 path takes), paired with the
// vertex shader `vertex/particle_color_t5`. Derivation, including why the
// vertex colour needs no rescaling here, is in M2_MULTITEX_DESIGN.md.
//
// Two slang quirks shape the declarations below, both inherited from
// refraction_mask.slang and load-bearing for the same reasons:
//
//   * Slang appends its own index to every semantic, so a vertex input
//     `: TEXCOORD1` is emitted as `TEXCOORD10` and read back as index 10.
//     Vertex inputs must therefore use digit-free semantics — hence the two
//     scrolling UV sets packed into one TEXCOORD and the third on TANGENT.
//   * Register assignment is per MODULE, not per stage, so the two constant
//     buffers cannot both be `b0`. Both are pinned, and the pixel one is
//     bound at PS slot 1.
// ============================================================================

#if defined(WGSL_TARGET) || defined(METAL_TARGET)
static const int WF_PS_CB_OFFSET = 4;
static const int WF_PS_BIND_OFFSET = 12;
#else
static const int WF_PS_CB_OFFSET = 16;
static const int WF_PS_BIND_OFFSET = 16;
#endif

struct MultiTexVSCB {
    float4x4 world;
    float4x4 view;
    float4x4 projection;
}

struct MultiTexPSCB {
    // .x — 1 when the material asked for three colour textures, 0 for two.
    //      Selects the colour chain AND its multiplier, both off this single
    //      bit, exactly as the retail DXBC does:
    //
    //          and  r2.xyzw, r1.wwww, l(1, 1, 1, 2)
    //          movc r3.xyzw, r2.xyzw, l(4,4,4,4), l(2,2,2,2)
    //          movc r2.xyzw, r2.zzzz, threeLayer, twoLayerRgb_threeLayerA
    //
    //      One extra modulate stage, one extra doubling — the fixed-function
    //      MODULATE2X / MODULATE4X compensation.
    // .y — 1 for Mod4, 0 for Mod2. Independent of .x, and it reaches the alpha
    //      multiplier alone: the alpha chain is always all three layers
    //      ("3AlphaTex" in every one of the four effect names), so Mod2/Mod4 is
    //      the only choice left for it.
    // .z — 1 to alpha-test. The blend state for an alpha-keyed particle is
    //      "no blending", so without this its quads would paint solid; the
    //      threshold and the fact that the test reads the TEXTURE alpha rather
    //      than the final one are both the retail shader's
    //      (`alphaTestVal = scaledTex.w - 0.501960814`).
    // .w — unused.
    float4 params;
}

[[vk::binding(0, 0)]] ConstantBuffer<MultiTexVSCB> gVsCb : register(b0);
[[vk::binding(WF_PS_CB_OFFSET + 1, 0)]] ConstantBuffer<MultiTexPSCB> gPsCb : register(b1);

[[vk::binding(WF_PS_BIND_OFFSET + 0, 1)]] Texture2D<float4> gTex0 : register(t0);
[[vk::binding(WF_PS_BIND_OFFSET + 1, 1)]] Texture2D<float4> gTex1 : register(t1);
[[vk::binding(WF_PS_BIND_OFFSET + 2, 1)]] Texture2D<float4> gTex2 : register(t2);
[[vk::binding(WF_PS_BIND_OFFSET + 0, 2)]] SamplerState gSampler : register(s0);

struct MultiTexVSIn {
    float3 position : POSITION;
    float4 color    : COLOR;
    // .xy = the sprite-cell UV (layer 0), .zw = layer 1.
    float4 uv01     : TEXCOORD;
    // Layer 2. TANGENT only because it is a digit-free semantic the input
    // layout can name unambiguously — see the header.
    float2 uv2      : TANGENT;
}

struct MultiTexVSOut {
    float4 position : SV_POSITION;
    float4 color    : COLOR;
    float4 uv01     : TEXCOORD0;
    float2 uv2      : TEXCOORD1;
}

[shader("vertex")]
MultiTexVSOut vsMultiTexParticle(MultiTexVSIn input) {
    MultiTexVSOut o;
    // Row-vector convention against column-major storage, as in unlit.slang:
    // the host transposes before upload.
    let worldPos = mul(float4(input.position, 1.0), gVsCb.world);
    let viewPos = mul(worldPos, gVsCb.view);
    o.position = mul(viewPos, gVsCb.projection);
    o.color = input.color;
    o.uv01 = input.uv01;
    o.uv2 = input.uv2;
    return o;
}

[shader("fragment")]
float4 psMultiTexParticle(MultiTexVSOut input) : SV_Target {
    float4 s0 = gTex0.Sample(gSampler, input.uv01.xy);
    float4 s1 = gTex1.Sample(gSampler, input.uv01.zw);
    float4 s2 = gTex2.Sample(gSampler, input.uv2);

    // The retail chain verbatim: layers 0 and 1 first, then layer 2 on top.
    // Alpha always takes the three-layer product ("3AlphaTex" in every one of
    // the four effect names); only the colour chooses.
    float4 twoLayer = s1 * s0;
    float4 threeLayer = twoLayer * s2;

    float3 texColor = lerp(twoLayer.rgb, threeLayer.rgb, gPsCb.params.x) *
                      lerp(2.0, 4.0, gPsCb.params.x);
    float texAlpha = threeLayer.a * lerp(2.0, 4.0, gPsCb.params.y);

    if (gPsCb.params.z > 0.0 && texAlpha < 0.501960814)
        discard;

    // The retail vertex shader folds a 0.5 into vertex colour and the pixel
    // shader multiplies by 2 on the way out, so the pair cancels and the
    // vertex colour arrives here at full strength. Alpha is untouched by both.
    return float4(input.color.rgb * texColor, input.color.a * texAlpha);
}
