// ============================================================================
// WhiteoutFlakes Renderer — WoW refraction particles, pass 2 of 2
//
// Full-screen. Takes the central difference of the distortion buffer pass 1
// wrote, builds a surface normal from it, refracts the view ray through that
// normal with Snell's law, projects the refracted ray back to a screen offset,
// and samples a copy of the finished scene there. Pixels the mask never
// touched pass straight through.
//
// Transcribed from `pixel/refractionapply/perm_0000.hlsl` in
// C:/Projects/Wc3Shaders/wow_re_shaders; the client-side constants come from
// `RefractionBuffer::Render` @0x100e98fc0. Derivation in
// M2_REFRACTION_DESIGN.md §1.6. Pass 1 is refraction_mask.slang.
//
// Binding offsets follow gtao.slang / blit.slang — the runtime's slot N maps
// to @binding(WF_PS_*_OFFSET + N), which differs per backend.
//
// Every tap below is SampleLevel(…, 0) rather than Sample. The coverage test
// branches on the height taps, so the samples after it sit in control flow
// WGSL calls non-uniform, and an implicit-derivative Sample there is a
// validation error — Dawn rejects the whole module and the pass never
// compiles. Both sources are single-mip full-screen targets, so mip 0 is the
// level Sample would have chosen; this only drops the derivatives nothing
// here wanted.
// ============================================================================

#if defined(WGSL_TARGET) || defined(METAL_TARGET)
static const int WF_PS_CB_OFFSET = 4;
static const int WF_PS_BIND_OFFSET = 12;
#else
static const int WF_PS_CB_OFFSET = 16;
static const int WF_PS_BIND_OFFSET = 16;
#endif

struct RefractApplyCB
{
    // .xy — one texel of the distortion buffer, in UV, for the central
    //       differences. .zw — the projection's (m00, m11), which turn a
    //       view-space refracted direction into a screen offset.
    float4 texelAndProj;
    // .x — the index of refraction ratio (eta). The client's CVar default of 0
    //      resolves to 1/1.33; the host passes the resolved value.
    // .y — an overall multiplier on the screen offset. 1.0 is the client.
    // .z — non-zero shows the distortion buffer instead of the scene, which is
    //      what the client's `showRefractionBuffer` console command does
    //      (`RefractionBuffer::DebugDraw` @0x100e99290).
    // .w unused.
    float4 params;
}

[[vk::binding(WF_PS_CB_OFFSET + 0, 0)]] ConstantBuffer<RefractApplyCB> gApplyCb : register(b0);
[[vk::binding(WF_PS_BIND_OFFSET + 0, 1)]] Texture2D<float4> gSceneCopy : register(t0);
[[vk::binding(WF_PS_BIND_OFFSET + 1, 1)]] Texture2D<float4> gRefractBuffer : register(t1);
[[vk::binding(WF_PS_BIND_OFFSET + 0, 2)]] SamplerState gApplySampler : register(s0);

struct ApplyVSOut
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
}

// Full-screen triangle from the vertex id — no vertex buffer, same as vsBlit.
[shader("vertex")]
ApplyVSOut vsRefractApply(uint vid : SV_VertexID)
{
    ApplyVSOut o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// The height remap the retail shader applies to every tap: 2h below the middle,
// 1/(2h) above it. Both branches peak at 1 when h == 0.5, so a flat mask has no
// gradient and refracts nothing, and a tap of exactly 0 stays 0 — which is what
// the "did anything draw here" product below tests.
float refractHeight(float2 uv)
{
    float h = gRefractBuffer.SampleLevel(gApplySampler, uv, 0.0).r;
    float doubled = h + h;
    return (h > 0.5) ? (1.0 / doubled) : doubled;
}

[shader("fragment")]
float4 psRefractApply(ApplyVSOut input) : SV_Target
{
    // The client's `showRefractionBuffer`: height in red, coverage in green,
    // and nothing else — a flat plateau and an empty buffer look different here
    // and nowhere else, which is the whole reason the view exists.
    if (gApplyCb.params.z > 0.0) {
        float4 raw = gRefractBuffer.SampleLevel(gApplySampler, input.uv, 0.0);
        return float4(saturate(raw.r), saturate(raw.a), 0.0, 1.0);
    }

    float2 texel = gApplyCb.texelAndProj.xy;

    float hR = refractHeight(input.uv + float2(texel.x, 0.0));
    float hL = refractHeight(input.uv - float2(texel.x, 0.0));
    float hD = refractHeight(input.uv + float2(0.0, texel.y));
    float hU = refractHeight(input.uv - float2(0.0, texel.y));

    // All four taps must be non-zero. This is the client's own coverage test,
    // and it is why the mask is cleared to zero rather than to a neutral 0.5:
    // an untouched neighbourhood multiplies to 0 and passes straight through.
    if (hR * hL * hD * hU == 0.0)
        return gSceneCopy.SampleLevel(gApplySampler, input.uv, 0.0);

    // Height field → surface normal. The z component is 1 before normalising,
    // which is what makes rsqrt(dot(n,n)) equal to the normalised n.z — the
    // cosine the refraction below needs, obtained without a second dot.
    float3 n = float3(hL - hR, hD - hU, 1.0);
    float invLen = rsqrt(dot(n, n));
    n *= invLen;
    float cosI = invLen;

    float eta = gApplyCb.params.x;
    float k = 1.0 - eta * eta * (1.0 - cosI * cosI);
    if (k < 0.0)
        return gSceneCopy.SampleLevel(gApplySampler, input.uv, 0.0);

    // Snell, with the incident ray fixed at (0, 0, -1): the client refracts
    // against a flat view direction rather than a per-pixel one.
    float3 refracted = eta * float3(0.0, 0.0, -1.0) - (sqrt(k) - eta * cosI) * n;
    if (refracted.z == 0.0)
        return gSceneCopy.SampleLevel(gApplySampler, input.uv, 0.0);

    // Project the refracted direction to a screen offset. `-refracted.z` is the
    // perspective divide; the y flip is ours, because our UV runs down the
    // screen where the client's clip space runs up.
    float2 proj = gApplyCb.texelAndProj.zw;
    float2 offset = (proj * refracted.xy) / (-refracted.z);
    offset.y = -offset.y;
    offset *= gApplyCb.params.y;

    float2 dstUv = input.uv + offset;

    // Only take the offset sample when the destination is itself inside the
    // distortion. Without this the edge of a refracting quad drags unrelated
    // background across the silhouette.
    if (gRefractBuffer.SampleLevel(gApplySampler, dstUv, 0.0).r > 0.0)
        return gSceneCopy.SampleLevel(gApplySampler, dstUv, 0.0);
    return gSceneCopy.SampleLevel(gApplySampler, input.uv, 0.0);
}
