// ============================================================================
// WhiteoutFlakes Renderer — Diablo III's screen-space distortion, pass 2 of 2
//
// Pass 1 is not a shader of its own: a D3 distortion draw runs the SAME
// programs every other D3 draw runs (43 of the 48 shipped distortion passes are
// `ps_legacy`), and only its render target differs. What it leaves behind is a
// buffer of signed screen offsets encoded around 0.5, and this is the
// full-screen pass that reads them back and bends the finished scene through
// them.
//
// Transcribed from the shipped `v14 PostFX Distortion` program — an orphan with
// no `.shd`, found in `Corpus/D3/OpenGLShaders` as readable ARB. The whole
// pixel stage is 14 instructions and the arithmetic is four of them:
//
//     TEX R0.xy, R0, texture[1], 2D      ; texDistortion
//     MAD R0.x, R0, c[1].y, -c[1].w      ; * 2 - 1
//     MAD R0.y, R0, c[1],   -c[1].w
//     MAD R0.x, R0, c[1], fragment.texcoord[0]   ; * 0.03 + uv
//     MAD R0.y, R0, c[1].x, fragment.texcoord[0]
//     TEX result.color, R0, texture[0], 2D       ; texOriginal
//
// with `c[1] = {0.03, 2, 1e-5, 1}` and `c[0] = viewport` (scale, offset per
// axis). The other ten instructions are that viewport map and its MIN/MAX clamp,
// applied to BOTH taps — the engine draws into a shared atlas and this build
// does not, so here the map is the identity and the clamp is the [0, 1] edge.
//
// The 1e-5 is the original's: it clamps to one hair inside the far edge so a
// bilinear tap cannot reach the neighbouring viewport. Kept because the
// alternative is a guess, and it costs nothing.
//
// Binding offsets follow refraction_apply.slang / gtao.slang — the runtime's
// slot N maps to @binding(WF_PS_*_OFFSET + N), which differs per backend.
// ============================================================================

#if defined(WGSL_TARGET) || defined(METAL_TARGET)
static const int WF_PS_CB_OFFSET = 4;
static const int WF_PS_BIND_OFFSET = 12;
#else
static const int WF_PS_CB_OFFSET = 16;
static const int WF_PS_BIND_OFFSET = 16;
#endif

struct D3DistortApplyCB
{
    // .x — the offset gain, in viewport UV. `0.03` is the shipped constant and
    //      the only tunable thing in the program.
    // .y — non-zero shows the distortion buffer itself instead of the bent
    //      scene: the decoded offset as a colour, amplified so a live field is
    //      distinguishable from the neutral it blends over. The engine has no
    //      such view; this exists because through the resolve an empty buffer
    //      and a buffer full of zero offsets are the same picture.
    // .zw — unused.
    float4 params;
}

[[vk::binding(WF_PS_CB_OFFSET + 0, 0)]] ConstantBuffer<D3DistortApplyCB> gCb : register(b0);
[[vk::binding(WF_PS_BIND_OFFSET + 0, 1)]] Texture2D<float4> gSceneCopy : register(t0);
[[vk::binding(WF_PS_BIND_OFFSET + 1, 1)]] Texture2D<float4> gDistortion : register(t1);
[[vk::binding(WF_PS_BIND_OFFSET + 0, 2)]] SamplerState gSampler : register(s0);

struct D3DistortVSOut
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
}

// Full-screen triangle from the vertex id — no vertex buffer, same as vsBlit.
[shader("vertex")]
D3DistortVSOut vsD3DistortionApply(uint vid : SV_VertexID)
{
    D3DistortVSOut o;
    o.uv = float2((vid << 1) & 2, vid & 2);
    o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

[shader("fragment")]
float4 psD3DistortionApply(D3DistortVSOut input) : SV_Target
{
    // The original's `MIN R0.xy, R0, R0.zwzw` / `MAX R0.xy, R0, c[0].ywzw` with
    // the viewport map collapsed to the identity: scale 1, offset 0.
    const float kEdge = 1.0 - 9.9999997e-06;

    float2 encoded = gDistortion.Sample(gSampler, saturate(input.uv)).rg;
    float2 offset = (encoded * 2.0 - 1.0) * gCb.params.x;

    if (gCb.params.y > 0.0)
    {
        // The buffer, RAW: the two encoded axes in red and green, the neutral
        // 0.5 left where it is, and blue at 0.5 so an untouched pixel reads mid
        // grey rather than a colour.
        //
        // Deliberately NOT amplified. An earlier version scaled the recentred
        // offset by 4 to make a weak field legible, and it lied twice over: it
        // clipped every real field to flat red/green so magnitudes could not be
        // read at all, and it turned the half-LSB the 0.5 clear rounds to into
        // a visible step, so every billboard's quad showed up as a hard-edged
        // rectangle that looked like a format mismatch and was not.
        return float4(encoded.x, encoded.y, 0.5, 1.0);
    }

    float2 dst = clamp(input.uv + offset, float2(0.0, 0.0), float2(kEdge, kEdge));
    return gSceneCopy.Sample(gSampler, dst);
}
