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

// Explicit Vulkan descriptor bindings. The WhiteoutFlakes Vulkan backend's
// graphics pipeline layout is set 0 = constant buffers, set 1 = sampled
// images, set 2 = samplers, with pixel-stage slots offset by 16. A pixel
// texture at slot 0 must therefore land at (set 1, binding 16) and its
// sampler at (set 2, binding 16); without these hints slang would auto-
// assign both into set 0 and collide with the CB set. Ignored by the
// D3D / DXBC path, which keeps slang's t0 / s0 register assignment.
[[vk::binding(16, 1)]] Texture2D<float4> gImage;
[[vk::binding(16, 2)]] SamplerState gSampler;

[shader("fragment")]
float4 psBlit(BlitVSOut input) : SV_Target
{
    return gImage.Sample(gSampler, input.uv);
}
