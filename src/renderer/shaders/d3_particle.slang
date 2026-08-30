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
// The chain is FIXED-FUNCTION and it is a CHAIN, not a product: per stage a
// colour code and an alpha code, whose tens digit is the op (1 add, 2 modulate)
// and whose units digit carries both the MODULATE2X / MODULATE4X gain and
// whether the stage saturates before the next one reads it. Run in order here,
// one stage at a time, because two of those operations do not commute — see
// io/d3/d3_types.h for the grammar and where it was measured.
//
// **Every layer carries its own UV transform.** 9,600 of the corpus's mode-2
// entries scale their coordinates (3,372 by exactly 0.5, 0.5 — a quarter tile
// of a sprite sheet) and 13,996 scroll. The host evaluates each layer's affine
// against the emitter's own clock (D3UvAffine) and uploads six coefficients per
// layer, so a layer that scrolls scrolls and one that does not costs a
// multiply-add.
//
// **One layer may be a flip-book.** A uv mode 3 entry samples a sub-rect of a
// sprite sheet, and which sub-rect is a property of the PARTICLE, not of the
// draw — every particle seeds its own start frame and plays at its own rate. So
// that layer's transform splits in two: the tile SIZE is constant and rides the
// constant buffer like every other layer's, while the tile ORIGIN arrives in
// the vertex's NORMAL, which this family never reads for anything else.
// `params.w` names the layer, or is negative when there is none. See
// D3_PARTICLE_DESIGN.md §19.
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
    // .x D3DRS_DEPTHBIAS, a constant added to the [0,1] window depth; .yzw
    // spare. See D3PassState::depthBias.
    float4 params;
}

struct D3ParticlePSCB {
    // Per layer, the row-vector affine `D3UvAffine` produces, split across two
    // rows so each carries its own combine op:
    //   uvRow0 = (a0, a1, a2, the COLOUR op)
    //   uvRow1 = (a3, a4, a5, the ALPHA op)
    // 0 skip, 1 modulate, 2 add — kD3Stage* in io/d3/d3_types.h. Both are 0 for
    // a layer the material does not bind and for one the pass declares no stage
    // for, which is why there is no layer count and no branch.
    float4 uvRow0[kD3ParticleLayers];
    float4 uvRow1[kD3ParticleLayers];
    // Per layer: .x colour gain, .y alpha gain, .z/.w the two clamps as 0/1.
    float4 gains[kD3ParticleLayers];
    // .x the vertex-colour placement bits (1 colour first, 2 colour last,
    // 4 alpha first, 8 alpha last), .y spare, .z the alpha-test reference
    // (0 = no test), .w the flip-book layer's index, or negative for none.
    float4 params;
    // .x the premultiplied-alpha output mode; .y the alpha-test comparison.
    float4 params2;
}

[[vk::binding(0, 0)]] ConstantBuffer<D3ParticleVSCB> gVsCb : register(b0);
[[vk::binding(WF_PS_CB_OFFSET + 1, 0)]] ConstantBuffer<D3ParticlePSCB> gPsCb : register(b1);

[[vk::binding(WF_PS_BIND_OFFSET + 0, 1)]] Texture2D<float4> gTex0 : register(t0);
[[vk::binding(WF_PS_BIND_OFFSET + 1, 1)]] Texture2D<float4> gTex1 : register(t1);
[[vk::binding(WF_PS_BIND_OFFSET + 2, 1)]] Texture2D<float4> gTex2 : register(t2);
[[vk::binding(WF_PS_BIND_OFFSET + 3, 1)]] Texture2D<float4> gTex3 : register(t3);
// One sampler per layer rather than per wrap variant: a particle binds four
// textures at most and their address modes are per STAGE (TextureStageParams
// +0x04 and +0x08 on the bound pass), so four is both the ceiling and the
// exact count.
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
    // The atlas tile's origin, flat across the quad. Interpolated rather than
    // flat-qualified because every corner carries the same value anyway and a
    // `nointerpolation` qualifier is one more thing four backends must agree on.
    float2 atlasUv  : TEXCOORD1;
}

[shader("vertex")]
D3ParticleVSOut vsD3Particle(D3ParticleVSIn input) {
    D3ParticleVSOut o;
    // The quads are built in world space, so there is no world matrix to
    // apply — the same convention every other particle builder here uses.
    // Row-vector against column-major storage; the host transposes on upload.
    let viewPos = mul(float4(input.position, 1.0), gVsCb.view);
    o.position = mul(viewPos, gVsCb.projection);
    // The depth the rasteriser writes is z/w, so a constant added to it is a
    // bias*w added to z before the divide.
    o.position.z += gVsCb.params.x * o.position.w;
    o.color = input.color;
    o.uv = input.uv;
    o.atlasUv = input.normal.xy;
    return o;
}

// The fixed-function alpha test, which is a COMPARISON and a reference, not a
// threshold: `sub_5717F0` sets ALPHAFUNC and ALPHAREF together and passes (0, 0)
// when the enable is clear. 67 shipped passes compare LessEqual and 5 Less --
// the same cut-out with the kept and discarded halves swapped.
bool D3AlphaTestKeeps(float a, float reference, int func) {
    switch (func) {
    case 1: return false;              // Never
    case 2: return a <  reference;     // Less
    case 3: return a == reference;     // Equal
    case 4: return a <= reference;     // LessEqual
    case 5: return a >  reference;     // Greater
    case 6: return a != reference;     // NotEqual
    case 7: return a >= reference;     // GreaterEqual
    default: return true;              // 0 no test, 8 Always
    }
}

float2 D3LayerUv(float2 uv, int i, float2 atlasUv) {
    let r0 = gPsCb.uvRow0[i];
    let r1 = gPsCb.uvRow1[i];
    // The atlas layer's rows carry the tile size and a zero translation, so
    // adding the per-particle origin here IS its affine — one select, no branch.
    let o = (i == (int)gPsCb.params.w) ? atlasUv : float2(0.0, 0.0);
    return float2(r0.x * uv.x + r0.y * uv.y + r0.z + o.x,
                  r1.x * uv.x + r1.y * uv.y + r1.z + o.y);
}

[shader("fragment")]
float4 psD3Particle(D3ParticleVSOut input) : SV_Target {
    float4 s[kD3ParticleLayers];
    s[0] = gTex0.Sample(gSamp0, D3LayerUv(input.uv, 0, input.atlasUv));
    s[1] = gTex1.Sample(gSamp1, D3LayerUv(input.uv, 1, input.atlasUv));
    s[2] = gTex2.Sample(gSamp2, D3LayerUv(input.uv, 2, input.atlasUv));
    s[3] = gTex3.Sample(gSamp3, D3LayerUv(input.uv, 3, input.atlasUv));

    // The chain, in order and one stage at a time. Order matters now for two
    // reasons the accumulated product had no room for: an ADD, and a
    // MODULATE2X/4X that SATURATES before the next stage reads it. From
    // `particle_transparent_colorMult2x_alphaMult4x_pma`, compiled:
    //
    //     mul r0.xyzw, r0.wxyz, v1.wxyz      // stage 0, texture x vertex
    //     mov_sat r0.x, r0.x                 // <- the units-8 clamp, mid-chain
    //     mul r0.x, r0.x, r1.w               // stage 1
    //     mul_sat r0.x, r0.x, l(4.0)         // <- am4x, and its own clamp
    //
    // Folding those two clamps into one at the end multiplies the whole chain
    // by 4 instead of the last stage, which turns a soft alpha ramp opaque.
    let vcolBits = (int)gPsCb.params.x;
    float3 rgb = (vcolBits & 1) != 0 ? input.color.rgb : float3(1.0, 1.0, 1.0);
    float alpha = (vcolBits & 4) != 0 ? input.color.a : 1.0;
    [unroll]
    for (int i = 0; i < kD3ParticleLayers; ++i) {
        let cop = (int)gPsCb.uvRow0[i].w;
        let aop = (int)gPsCb.uvRow1[i].w;
        let g = gPsCb.gains[i];
        if (cop != 0) {
            rgb = (cop == 1 ? rgb * s[i].rgb : rgb + s[i].rgb) * g.x;
            if (g.z > 0.0)
                rgb = saturate(rgb);
        }
        if (aop != 0) {
            alpha = (aop == 1 ? alpha * s[i].a : alpha + s[i].a) * g.y;
            if (g.w > 0.0)
                alpha = saturate(alpha);
        }
    }
    // A stage with no texture whose argument is the vertex colour: the chain
    // takes it at the END instead of (or as well as) at the head.
    if ((vcolBits & 2) != 0)
        rgb *= input.color.rgb;
    if ((vcolBits & 8) != 0)
        alpha *= input.color.a;

    // The colour is not saturated, for d3_standard.slang's reason: the
    // original's colour half runs unclamped into a UNORM target and the ROP
    // does the clamping, so doing it here would cost the HDR path its headroom.
    float4 outColor = float4(rgb, saturate(alpha));
    // The premultiplied family, which is not a blend-state variant but a
    // different PROGRAM: it multiplies the colour by the alpha and then writes
    // something else in the alpha channel, because the pass pairs
    // `src = BLENDFACTOR (1,1,1,0)` with `dst = SRCALPHA`. Mode 1 writes
    // `1 - a`, so `C*a + dst*(1-a)` is the ordinary over-composite; mode 2
    // writes 1 and the destination survives whole, which is the additive one;
    // mode 3 is the `dst = INVSRCALPHA` pair, already the right way round.
    // See D3PassState::pmaMode.
    let pma = (int)gPsCb.params2.x;
    if (pma != 0) {
        outColor.rgb *= outColor.a;
        if (pma == 1)
            outColor.a = 1.0 - outColor.a;
        else if (pma == 2)
            outColor.a = 1.0;
    }
    // Last, and on what was WRITTEN: `particle_transparent_colorMult2x_
    // alphaMult4x_aphatest_pma` closes `ge r0.x, alphaTestRef.w, r1.x` with
    // r1.x the `1 - coverage` it just stored, so a premultiplied pass tests the
    // inverse. The blend state stays on where it fires, so this is a cut-out
    // inside a blended draw.
    if (!D3AlphaTestKeeps(outColor.a, gPsCb.params.z, (int)gPsCb.params2.y))
        discard;
    return outColor;
}
