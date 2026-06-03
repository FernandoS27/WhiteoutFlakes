// ============================================================================
// WhiteoutFlakes Renderer — Separable Gaussian blur (bloom support).
//
// 15-tap horizontal/vertical separable Gaussian, driven by a per-frame CB
// that the host fills with normalised weights + per-sample UV offsets. Used
// by the post-process service's bloom path: extract → blur-H → blur-V →
// combine. The CB layout mirrors the engine's `PixelConstantsBlur` struct
// from GBuffer::ComputeBlurConstants (15 × float4 = 240 bytes) so a future
// port to the engine's shipped gaussianblur.bls is a drop-in swap.
//
// Binding layout (matches gtao.slang / blit.slang):
//   set 0 binding 16/4  — BlurCB (pixel-stage uniform).
//   set 1 binding 16/12 — source texture (the upstream pass's RTV).
//   set 2 binding 16/12 — linear-clamp sampler.
//
// Renderer-internal — compiled by slangc with WGSL_TARGET / METAL_TARGET
// defines for the appropriate per-backend binding offsets.
// ============================================================================

struct BlurCB
{
    // float4[15] packed as the engine packs it:
    //   data[0]            = (0, 0, centreWeight, sampleCount)
    //   data[2*i+1]        = (+offset.xy * (i+1), weight[i], 0)
    //   data[2*i+2]        = (-offset.xy * (i+1), weight[i], 0)
    // Weights are pre-normalised CPU-side so they sum to 1.
    float4 data[15];
}

// Per-backend binding shifts mirror gtao.slang. Slang's WGSL/Metal targets
// keep `[[vk::binding(N)]]` literal, so the offsets have to match each
// runtime's host-slot → @binding mapping or the descriptor set collides.
#if defined(WGSL_TARGET) || defined(METAL_TARGET)
static const int WF_PS_CB_OFFSET   = 4;
static const int WF_PS_BIND_OFFSET = 12;
#else
static const int WF_PS_CB_OFFSET   = 16;
static const int WF_PS_BIND_OFFSET = 16;
#endif

[[vk::binding(WF_PS_CB_OFFSET + 0, 0)]] ConstantBuffer<BlurCB>   g_cb      : register(b0);
[[vk::binding(WF_PS_BIND_OFFSET + 0, 1)]] Texture2D<float4>      g_source  : register(t0);
[[vk::binding(WF_PS_BIND_OFFSET + 0, 2)]] SamplerState           g_sampler : register(s0);

struct VsOut
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
}

// Full-screen triangle from SV_VertexID — same trick the GTAO + blit
// passes use. No vertex buffer, no input layout.
[shader("vertex")]
VsOut vsBlur(uint vid : SV_VertexID)
{
    VsOut o;
    o.uv       = float2((vid << 1) & 2, vid & 2);
    o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 psBlur(VsOut input) : SV_Target
{
    float2 uv = input.uv;
    // [unroll] keeps fxc happy on the 15-iteration loop — same pattern
    // the GTAO denoise uses. Each iteration is one texture tap + a mad.
    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    [unroll]
    for (int i = 0; i < 15; ++i)
    {
        float2 off    = g_cb.data[i].xy;
        float  weight = g_cb.data[i].z;
        float4 sample = g_source.SampleLevel(g_sampler, uv + off, 0);
        acc += sample * weight;
    }
    return acc;
}
