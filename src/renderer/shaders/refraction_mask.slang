// ============================================================================
// WhiteoutFlakes Renderer — WoW refraction particles, pass 1 of 2
//
// A refraction emitter's quads, drawn into a scalar distortion buffer instead
// of into the scene. One texture, sampled three times through three UV sets;
// the R, G and B channels of those taps multiply into a single "height",
// scaled and faded out with view distance. Red carries the height, alpha the
// coverage the blend weighs it by; green and blue are written zero, exactly as
// the retail shader does.
//
// Transcribed from `pixel/refraction/uber.hlsl` in
// C:/Projects/Wc3Shaders/wow_re_shaders (its non-depth-filter permutations,
// which are the ones the particle path takes), paired with the vertex shader
// `vertex/particle_color_t4`. Derivation in M2_REFRACTION_DESIGN.md §1.6.
// Pass 2 is refraction_apply.slang.
//
// Two slang quirks shape the declarations below, both learned the hard way:
//
//   * Slang appends its own index to every semantic, so `: TEXCOORD1` is
//     emitted as `TEXCOORD10` and read back by D3D as TEXCOORD **index 10**.
//     A vertex input must therefore use semantics with no digits of their own
//     — hence the two UV sets packed into one TEXCOORD and the third riding
//     TANGENT. VS-out/PS-in are matched by the emitted string on both sides,
//     so they are unaffected and keep their natural numbering.
//   * Register assignment is per MODULE, not per stage, so two constant
//     buffers in one file cannot both be `b0` even when one is vertex-only.
//     Both are pinned explicitly here, and the pixel one is bound at PS slot 1.
// ============================================================================

#if defined(WGSL_TARGET) || defined(METAL_TARGET)
static const int WF_PS_CB_OFFSET = 4;
static const int WF_PS_BIND_OFFSET = 12;
#else
static const int WF_PS_CB_OFFSET = 16;
static const int WF_PS_BIND_OFFSET = 16;
#endif

struct RefractMaskVSCB {
    float4x4 world;
    float4x4 view;
    float4x4 projection;
}

struct RefractMaskPSCB {
    // .x — view-distance fade coefficient. The client's fade is
    //      `1 - saturate(viewZ * 0.01)`, i.e. gone by 100 yards; this is that
    //      0.01 divided by the profile's WorldScale so it stays 100 YARDS and
    //      not 100 renderer units.
    // .y — the constant the retail vertex shader folds into vertex colour
    //      (`float4(0.5,0.5,0.5,1) * vColor`), kept as a parameter so the
    //      strength is tunable without editing the transcription.
    // .z — mask gain, 4.0 in the retail shader.
    // .w — unused.
    float4 params;
}

[[vk::binding(0, 0)]] ConstantBuffer<RefractMaskVSCB> gVsCb : register(b0);
[[vk::binding(WF_PS_CB_OFFSET + 1, 0)]] ConstantBuffer<RefractMaskPSCB> gPsCb : register(b1);

[[vk::binding(WF_PS_BIND_OFFSET + 0, 1)]] Texture2D<float4> gTex : register(t0);
[[vk::binding(WF_PS_BIND_OFFSET + 0, 2)]] SamplerState gSampler : register(s0);

struct RefractMaskVSIn {
    float3 position : POSITION;
    float4 color    : COLOR;
    // .xy = the sprite-cell UV, .zw = extra texture layer 1.
    float4 uv01     : TEXCOORD;
    // Extra texture layer 2. TANGENT only because it is a digit-free semantic
    // the input layout can name unambiguously — see the header.
    float2 uv2      : TANGENT;
}

struct RefractMaskVSOut {
    float4 position : SV_POSITION;
    float4 color    : COLOR;
    // .xy unused, .z = view-space depth for the distance fade.
    float3 viewPos  : TEXCOORD0;
    float4 uv01     : TEXCOORD1;
    float2 uv2      : TEXCOORD2;
}

[shader("vertex")]
RefractMaskVSOut vsRefractParticle(RefractMaskVSIn input) {
    RefractMaskVSOut o;
    // Row-vector convention against column-major storage, as in unlit.slang:
    // the host transposes before upload.
    let worldPos = mul(float4(input.position, 1.0), gVsCb.world);
    let viewPos = mul(worldPos, gVsCb.view);
    o.position = mul(viewPos, gVsCb.projection);
    o.viewPos = viewPos.xyz;
    o.color = input.color;
    o.uv01 = input.uv01;
    o.uv2 = input.uv2;
    return o;
}

[shader("fragment")]
float4 psRefractParticle(RefractMaskVSOut input) : SV_Target {
    // R from the sprite UV, G from layer 1, B from layer 2. The retail DXBC's
    // per-permutation resource swizzles look different but always extract this
    // same triple — see the reconstruction's "Non-obvious finding".
    float m0 = gTex.Sample(gSampler, input.uv01.xy).r;
    float m1 = gTex.Sample(gSampler, input.uv01.zw).g;
    float m2 = gTex.Sample(gSampler, input.uv2).b;

    float mask = m0 * m1 * m2 * gPsCb.params.z;

    // View depth is negative in a right-handed view space and the client's is
    // positive-into-the-screen, so take the magnitude before fading.
    float viewDepth = abs(input.viewPos.z);
    mask *= 1.0 - saturate(viewDepth * gPsCb.params.x);

    float strength = mask * input.color.r * gPsCb.params.y;
    return float4(strength, 0.0, 0.0, mask * input.color.a);
}
