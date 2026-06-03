// ============================================================================
// WhiteoutDex Renderer — Full-screen blit
//
// Copies an off-screen capture target onto the swap-chain back buffer so the
// frame still displays while frame capture redirects the composite off-screen.
// Used only when frame capture is enabled.
// ============================================================================

struct BlitVSOut
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
}

// Full-screen triangle generated from the vertex id — no vertex buffer.
[shader("vertex")]
BlitVSOut vsBlit(uint vid : SV_VertexID)
{
    BlitVSOut o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Per-backend binding offsets mirror gtao.slang / gaussian_blur.slang.
// Vulkan + D3D pixel SRV/sampler slots start at binding 16; WebGPU's
// kStageBindingShift packs them at 12. The runtime always binds via
// `BindShaderResource(Pixel, 0, …)` and translates slot 0 → binding
// (12 or 16) per backend, so the shader has to declare its bindings
// at the SAME per-backend offset or the WGSL pipeline misses the
// runtime's slot-0 write and samples a default (black) view.
#if defined(WGSL_TARGET) || defined(METAL_TARGET)
static const int WF_PS_BIND_OFFSET = 12;
#else
static const int WF_PS_BIND_OFFSET = 16;
#endif
[[vk::binding(WF_PS_BIND_OFFSET + 0, 1)]] Texture2D<float4> gImage;
[[vk::binding(WF_PS_BIND_OFFSET + 0, 2)]] SamplerState gSampler;

[shader("fragment")]
float4 psBlit(BlitVSOut input) : SV_Target
{
    return gImage.Sample(gSampler, input.uv);
}
