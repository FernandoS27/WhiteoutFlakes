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
// **There is no UV transform here.** Every stage's coordinate arrives already
// transformed, four per vertex, because that is where the engine puts it:
// `Particle_WriteQuadVertices` bakes four packed texcoords into a 56-byte vertex
// and `Particle_DrawBatch` uploads no texture matrix at all. It has to, because
// the transform belongs to the PARTICLE and not to the draw — the scroll phase
// and its rate are drawn at emit (12,361 of the corpus's 13,897 mode-2 entries
// randomise the phase and 9,235 jitter the rate), the rotation accumulates from
// birth, and each stage walks its own flip-book. A per-draw affine puts every
// particle of a puff on one tile of one sheet.
//
// What is left per layer is which of the four coordinates it samples at — the
// sets are positional over the stage types and a pass may route one stage onto
// another's — plus its combine op, its gain and its clamp. See
// D3_PARTICLE_DESIGN.md §19 and BuildGeometryInput::d3Uv01.
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
    // Per layer: .x which of the vertex's four baked texcoords this stage
    // samples at, .y the COLOUR op, .z the ALPHA op, .w spare. The ops are
    // 0 skip, 1 modulate, 2 add — kD3Stage* in io/d3/d3_types.h — and both are 0
    // for a layer the material does not bind and for one the pass declares no
    // stage for, which is why there is no layer count and no branch.
    float4 stage[kD3ParticleLayers];
    // Per layer: .x colour gain, .y alpha gain, .z/.w the two clamps as 0/1.
    float4 gains[kD3ParticleLayers];
    // .x the vertex-colour placement bits (1 colour first, 2 colour last,
    // 4 alpha first, 8 alpha last), .y the PROGRAM (0 chain, 1 blend-add,
    // 2 water-sim), .z the alpha-test reference (0 = no test), .w spare.
    float4 params;
    // .x the premultiplied-alpha output mode; .y the alpha-test comparison;
    // .z the first flow-map layer, negative where the program has none;
    // .w the last flow-map layer, inclusive.
    float4 params2;
    // .x the erosion exponent's LITERAL half (10), 0 for no dissolve tail; the
    // COLOR1.a it multiplies rides the vertex.
    float4 params3;
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
    float4 color    : COLOR;
    // The four baked stage texcoords, two per attribute. TANGENT rather than a
    // numbered TEXCOORD because slang mangles a digit on a vertex INPUT
    // semantic; see the input layout in d3_particle_shading.cpp.
    float4 uv01     : TEXCOORD;
    float4 uv23     : TANGENT;
    // The engine's SECOND D3DCOLOR, whose four lanes are all channel 6. Only
    // the erosion tail reads it, so one scalar is enough; BLENDWEIGHT because
    // the semantic has to be digit-free, same as TANGENT above.
    float  color1   : BLENDWEIGHT;
}

struct D3ParticleVSOut {
    float4 position : SV_POSITION;
    float4 color    : COLOR;
    float4 uv01     : TEXCOORD0;
    float4 uv23     : TEXCOORD1;
    float  color1   : TEXCOORD2;
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
    o.uv01 = input.uv01;
    o.uv23 = input.uv23;
    o.color1 = input.color1;
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

// Which of the vertex's four coordinates this stage samples at. The sets are
// POSITIONAL over the stage types (0 type 1, 1 type 19, 2 type 12, 3 type 14)
// and the pass's texcoord block picks one per stage, so it is usually but not
// always the stage's own.
float2 D3LayerUv(D3ParticleVSOut input, int i) {
    let s = (int)gPsCb.stage[i].x;
    if (s == 0) return input.uv01.xy;
    if (s == 1) return input.uv01.zw;
    if (s == 2) return input.uv23.xy;
    return input.uv23.zw;
}

float4 D3SampleLayer(int i, float2 uv) {
    switch (i) {
    case 0: return gTex0.Sample(gSamp0, uv);
    case 1: return gTex1.Sample(gSamp1, uv);
    case 2: return gTex2.Sample(gSamp2, uv);
    default: return gTex3.Sample(gSamp3, uv);
    }
}

// The flow shaders' UV distortion, which is one expression for all three map
// counts. Each map contributes its .xy, the product is scaled back up by
// 2^(n-1) because two unorm textures multiplied land around a quarter, and the
// result is recentred on zero and halved:
//
//     ps_particle_flow      one map    (f - 0.5) * 0.5
//     ps_legacy_flow        two maps   (2*f1*f2 - 0.5) * 0.5
//     ..._flowmult_masked   three      (4*f1*f2*f3 - 0.5) * 0.5
//
// The maps themselves are sampled at their OWN transformed coordinates and are
// not part of the combine chain — the pass's stage block may still park a code
// on their slot, and the host drops it.
float2 D3FlowOffset(D3ParticleVSOut input, int first, int last) {
    float2 product = float2(1.0, 1.0);
    [unroll]
    for (int k = 0; k < kD3ParticleLayers; ++k) {
        if (k >= first && k <= last)
            product *= D3SampleLayer(k, D3LayerUv(input, k)).xy;
    }
    return (product * exp2((float)(last - first)) - 0.5) * 0.5;
}

[shader("fragment")]
float4 psD3Particle(D3ParticleVSOut input) : SV_Target {
    let flowFirst = (int)gPsCb.params2.z;
    let flowLast = (int)gPsCb.params2.w;
    float2 flowOff = float2(0.0, 0.0);
    if (flowFirst >= 0)
        flowOff = D3FlowOffset(input, flowFirst, flowLast);

    float4 s[kD3ParticleLayers];
    [unroll]
    for (int li = 0; li < kD3ParticleLayers; ++li)
        s[li] = D3SampleLayer(li, D3LayerUv(input, li) + flowOff);

    let program = (int)gPsCb.params.y;
    if (program != 0) {
        // Neither of the other two is a chain. `ps_blend_add*` and
        // `ps_billboard_blendAdd_flowMult` sum two ALREADY-premultiplied layers
        // — the base as-is, the second tinted by the vertex colour — and carry
        // the base's coverage in the alpha; `ps_particle_water_sim` scales the
        // diffuse by the vertex ALPHA and nothing else.
        if (program == 2)
            return s[0] * input.color.a;
        float baseAlpha = s[0].a * input.color.a;
        float3 sum = s[0].rgb * s[0].a + (s[1].rgb * s[1].a) * input.color.rgb;
        float4 outBa = float4(sum * input.color.a, baseAlpha);
        // The premultiplied arm writes the INVERSE coverage; the colour is
        // already premultiplied per layer, so it is not scaled again here.
        let pmaBa = (int)gPsCb.params2.x;
        if (pmaBa == 1)
            outBa.a = 1.0 - baseAlpha;
        else if (pmaBa == 2)
            outBa.a = 1.0;
        return outBa;
    }
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
        let cop = (int)gPsCb.stage[i].y;
        let aop = (int)gPsCb.stage[i].z;
        let g = gPsCb.gains[i];
        if (cop != 0) {
            // 3 is SELECTARG1(TEXTURE): the stage REPLACES the channel and
            // everything before it is dead. `actor_complex_Transparent_Ground`
            // is what pins it — see io/d3/d3_types.h::kD3StageReplace.
            rgb = (cop == 1 ? rgb * s[i].rgb : cop == 2 ? rgb + s[i].rgb : s[i].rgb) * g.x;
            if (g.z > 0.0)
                rgb = saturate(rgb);
        }
        if (aop != 0) {
            alpha = (aop == 1 ? alpha * s[i].a : aop == 2 ? alpha + s[i].a : s[i].a) * g.y;
            if (g.w > 0.0)
                alpha = saturate(alpha);
        }
    }
    // The dissolve tail, before the vertex colour lands and not after -- the
    // shipped order, and it matters because the exponent would otherwise act
    // on a value the vertex alpha has already scaled.
    // `10 * COLOR1.a`: the 10 is the program's literal, COLOR1.a is channel 6
    // and is per particle. 21,328 of 21,593 files leave it at 1.0, which is the
    // flat exponent this used to assume.
    let erode = gPsCb.params3.x * input.color1;
    if (erode > 0.0)
        alpha = min(1.0, pow(max(alpha, 0.0), erode));
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
