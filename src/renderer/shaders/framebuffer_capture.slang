// ============================================================================
// WhiteoutDex Renderer — Frame-capture compute shader
//
// Reads the rendered LDR colour target and writes tightly-packed RGBA8
// pixels into a buffer for CPU download (PNG / GIF export). Only dispatched
// when frame capture is explicitly enabled — off the normal render path.
// ============================================================================

struct CaptureParams
{
    uint width;
    uint height;
    uint pad0;
    uint pad1;
}

// Explicit Vulkan descriptor bindings (set 0) — the WhiteoutFlakes Vulkan
// backend builds a fixed compute descriptor layout with exactly these three
// slots. The attributes are ignored by the D3D / DXBC path, which keeps
// slang's automatic b0/t0/u0 register assignment.
[[vk::binding(0, 0)]] ConstantBuffer<CaptureParams> gParams;
[[vk::binding(1, 0)]] Texture2D<float4> gSource;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> gDest;

// The capture target is an sRGB render target, so sampling delinearises it.
// Re-encode to recover the stored 8-bit sRGB bytes that PNG / GIF expect.
float linearToSrgb(float c)
{
    return (c <= 0.0031308) ? (c * 12.92)
                            : (1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

uint toByte(float v)
{
    return uint(saturate(v) * 255.0 + 0.5);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csCapture(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= gParams.width || tid.y >= gParams.height)
        return;

    float4 c = gSource.Load(int3(int(tid.x), int(tid.y), 0));

    // Packed little-endian RGBA8: R in the low byte — matches what stb_image
    // / common PNG writers expect for a 4-channel buffer.
    uint packed = toByte(linearToSrgb(c.r)) | (toByte(linearToSrgb(c.g)) << 8) |
                  (toByte(linearToSrgb(c.b)) << 16) | (toByte(c.a) << 24);

    gDest[tid.y * gParams.width + tid.x] = packed;
}
