// ============================================================================
// WhiteoutFlakes Renderer — Diablo III particles
//
// The pixel half of `Billboard.fx` / `vs_legacy` / `ps_legacy`, which is what
// every shipped `.prt` binds: of the corpus's 223 Billboard.fx shaders, 186 use
// `ps_legacy` and all 223 are a single pass that writes no depth and culls
// nothing. `Particle_DrawBatch` @0x71000B63A0 binds four textures by STAGE TYPE
// (1, 19, 12, 14) and nothing else — no UberMaterial resolve, no Material SNO,
// pass index literal 0 — so the whole material is four samplers, a combine
// chain and a blend state.
//
// The chain is FIXED-FUNCTION, and its grammar is the one io/d3/d3_types.h
// already decodes for `Legacy.fx`: per stage, a colour code and an alpha code
// whose tens digit is the op (2 = modulate) and whose units digit is the
// MODULATE2X / MODULATE4X gain. Reproduced here as a product over the stages
// the pass says modulate, times the accumulated gain — which reaches 4 on the
// colour and 32 on the alpha in shipped assets, so it is not a rounding term.
//
// **Every layer carries its own UV transform.** 9,600 of the corpus's mode-2
// entries scale their coordinates (3,372 by exactly 0.5, 0.5 — a quarter tile
// of a sprite sheet) and 13,996 scroll. The host evaluates each layer's affine
// against the emitter's own clock (D3UvAffine) and uploads six coefficients per
// layer, so a layer that scrolls scrolls and one that does not costs a
// multiply-add.
//
// What is NOT here: uv mode 3, the `Anim2D` frame table, which 8,390 entries
// use (7,553 of them on the diffuse). That needs an asset this build does not
// read, and the layer falls back to its untransformed coordinates — the whole
// sheet on one quad. See D3_PARTICLE_DESIGN.md §15.7.
// ============================================================================

#if defined(WGSL_TARGET) || defined(METAL_TARGET)
static const int WF_PS_CB_OFFSET = 4;
static const int WF_PS_BIND_OFFSET = 12;
#else
static const int WF_PS_CB_OFFSET = 16;
static const int WF_PS_BIND_OFFSET = 16;
#endif

static const int kD3ParticleLayers = 4;

struct D3ParticleVSCB {
    float4x4 view;
    float4x4 projection;
}

struct D3ParticlePSCB {
    // Per layer, the row-vector affine `D3UvAffine` produces, split across two
    // rows so each carries its own channel switch:
    //   uvRow0 = (a0, a1, a2, 1 when this stage feeds the COLOUR)
    //   uvRow1 = (a3, a4, a5, 1 when this stage feeds the ALPHA)
    // Both switches are 0 for a layer the material does not bind, which is why
    // there is no layer count and no branch: an absent layer multiplies by one.
    float4 uvRow0[kD3ParticleLayers];
    float4 uvRow1[kD3ParticleLayers];
    // .x colour gain, .y alpha gain, .z alpha-test reference (0 = no test),
    // .w unused.
    float4 params;
}

[[vk::binding(0, 0)]] ConstantBuffer<D3ParticleVSCB> gVsCb : register(b0);
[[vk::binding(WF_PS_CB_OFFSET + 1, 0)]] ConstantBuffer<D3ParticlePSCB> gPsCb : register(b1);

[[vk::binding(WF_PS_BIND_OFFSET + 0, 1)]] Texture2D<float4> gTex0 : register(t0);
[[vk::binding(WF_PS_BIND_OFFSET + 1, 1)]] Texture2D<float4> gTex1 : register(t1);
[[vk::binding(WF_PS_BIND_OFFSET + 2, 1)]] Texture2D<float4> gTex2 : register(t2);
[[vk::binding(WF_PS_BIND_OFFSET + 3, 1)]] Texture2D<float4> gTex3 : register(t3);
// One sampler per layer rather than per wrap variant: a particle binds four
// textures at most and their address modes are per ENTRY (bits 0 and 1 of the
// UV flags word), so four is both the ceiling and the exact count.
[[vk::binding(WF_PS_BIND_OFFSET + 0, 2)]] SamplerState gSamp0 : register(s0);
[[vk::binding(WF_PS_BIND_OFFSET + 1, 2)]] SamplerState gSamp1 : register(s1);
[[vk::binding(WF_PS_BIND_OFFSET + 2, 2)]] SamplerState gSamp2 : register(s2);
[[vk::binding(WF_PS_BIND_OFFSET + 3, 2)]] SamplerState gSamp3 : register(s3);

struct D3ParticleVSIn {
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD;
}

struct D3ParticleVSOut {
    float4 position : SV_POSITION;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD0;
}

[shader("vertex")]
D3ParticleVSOut vsD3Particle(D3ParticleVSIn input) {
    D3ParticleVSOut o;
    // The quads are built in world space, so there is no world matrix to
    // apply — the same convention every other particle builder here uses.
    // Row-vector against column-major storage; the host transposes on upload.
    let viewPos = mul(float4(input.position, 1.0), gVsCb.view);
    o.position = mul(viewPos, gVsCb.projection);
    o.color = input.color;
    o.uv = input.uv;
    return o;
}

float2 D3LayerUv(float2 uv, int i) {
    let r0 = gPsCb.uvRow0[i];
    let r1 = gPsCb.uvRow1[i];
    return float2(r0.x * uv.x + r0.y * uv.y + r0.z, r1.x * uv.x + r1.y * uv.y + r1.z);
}

[shader("fragment")]
float4 psD3Particle(D3ParticleVSOut input) : SV_Target {
    float4 s[kD3ParticleLayers];
    s[0] = gTex0.Sample(gSamp0, D3LayerUv(input.uv, 0));
    s[1] = gTex1.Sample(gSamp1, D3LayerUv(input.uv, 1));
    s[2] = gTex2.Sample(gSamp2, D3LayerUv(input.uv, 2));
    s[3] = gTex3.Sample(gSamp3, D3LayerUv(input.uv, 3));

    float3 texColor = float3(1.0, 1.0, 1.0);
    float texAlpha = 1.0;
    [unroll]
    for (int i = 0; i < kD3ParticleLayers; ++i) {
        texColor *= lerp(float3(1.0, 1.0, 1.0), s[i].rgb, gPsCb.uvRow0[i].w);
        texAlpha *= lerp(1.0, s[i].a, gPsCb.uvRow1[i].w);
    }
    // The gain multiplies the WHOLE product, vertex colour included, and the
    // clamp lands once at the end — which is what the shipped programs do and
    // what d3_standard.slang already does for geometry. From
    // `Particle_transparent_am4x_errosion`, an am4x pass compiled out to ARB:
    //
    //     MUL R1.x, texAlphaMask.w, fragment.color.primary.w
    //     MUL R0.w, R1.x, texDiffuse.w      // mask * vertex * diffuse
    //     MUL R0.w, R0.w, 4                 // <- the am4x, on the product
    //     POW_SAT result.color.w, ...       // <- the only clamp
    //
    // Gaining and clamping the TEXTURE alone instead pins a fire sprite's soft
    // alpha ramp to 1 across most of the quad, which draws the sprite as an
    // opaque square, and does the same to its colour.
    float3 color = input.color.rgb * texColor * gPsCb.params.x;
    // Not saturated, for d3_standard.slang's reason: the original's colour half
    // runs unclamped into a UNORM target and the ROP does the clamping, so
    // doing it here would cost the HDR path its headroom.
    float4 outColor = float4(color, saturate(input.color.a * texAlpha * gPsCb.params.y));
    // The fixed-function alpha test, which compares the FINAL alpha — 38 of the
    // corpus's 223 particle shaders set a reference, and the blend state on
    // those is still on, so this is a cut-out inside a blended draw.
    if (gPsCb.params.z > 0.0 && outColor.a < gPsCb.params.z)
        discard;
    return outColor;
}
