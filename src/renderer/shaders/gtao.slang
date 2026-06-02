// ============================================================================
// WhiteoutFlakes Renderer — Ground-Truth Ambient Occlusion (GTAO)
//
// Algorithm: simplified GTAO after Jiménez et al., "Practical Realtime
// Strategies for Accurate Indirect Occlusion" (Activision, 2016). Inputs
// are the HD opaque G-buffer's slot-1 linear view-Z and slot-2 encoded
// world-space normal. Output is a scalar visibility factor in [0, 1]
// written to R8_UNORM.
//
// V1 ships a single quality preset (4 slices × 4 steps per side, full-res,
// no temporal denoise) and a 2-pass pipeline: gtao_main_ps writes the AO
// buffer; gtao_apply_ps reads the AO buffer + the HDR scene colour and
// modulates the HDR in place via a multiplicative blend (the host opens
// the pass with loadOp=Load so the pre-existing HDR contents survive).
//
// Future axes (deferred to V2): quality permutations, spatial denoise,
// bent-normal output for indirect lighting, half-res + bilateral upsample,
// temporal reprojection. See the plan attached to this commit.
//
// Binding layout (matches blit.slang / framebuffer_capture.slang):
//   set 0 binding 0       — GtaoCB (pixel-stage uniform)
//   set 1 binding 16 / 17 — pixel SRVs (linearDepth, normalBuffer)
//                           OR hdrColor + aoBuffer for the apply pass
//   set 2 binding 16      — pixel sampler
// The Vulkan hints are honoured by the Vulkan pipeline layout; D3D and
// WGSL ignore them and use slang's auto-assigned t0/s0/b0 registers.
// ============================================================================

struct GtaoCB
{
    // View + projection matrices used to project AO sample taps into
    // screen space and reconstruct view-space positions from linear Z.
    float4x4 view;
    float4x4 proj;
    float4x4 invProj;

    // .xy = viewport size in pixels, .zw = 1/size.
    float4 viewport;

    // .x = world-space sample radius (units of the scene).
    // .y = AO power / intensity (post-exponent).
    // .z = falloff distance multiplier (samples past this drop out).
    // .w = unused; reserved for future temporal blend amount.
    float4 params;

    // .x frame index (low bits used for jitter rotation).
    // .y debug mode: 0 = normal apply, 1 = AO only (apply pass writes
    //               vec3(ao) regardless of scene colour), 2 = inverted
    //               (1-ao) for occlusion-only highlight.
    // .zw unused.
    uint4 misc;
}

[[vk::binding(0, 0)]] ConstantBuffer<GtaoCB> g_cb;

// Pixel-stage SRVs. The HD-side bindings live at slot offsets 16..27
// (see webgpu_init.cpp's kStageBindingShift) — using 16/17/18 here
// keeps the descriptor set aligned with the existing post-process
// passes. Each pass samples a disjoint subset:
//   main_ps    — g_linearDepth, g_normal
//   denoise_ps — g_linearDepth, g_aoNoisy
//   apply_ps   — g_aoBuffer
// For D3D, Slang's per-entry HLSL emit packs only the used textures
// into t-registers in declaration order. The host (GtaoService::Run)
// pins each pass's bindings to the slot D3D assigns; Vulkan + WGSL
// use the explicit binding hints.
[[vk::binding(16, 1)]] Texture2D<float>  g_linearDepth;
[[vk::binding(17, 1)]] Texture2D<float4> g_normal;
[[vk::binding(18, 1)]] Texture2D<float>  g_aoNoisy;
[[vk::binding(19, 1)]] Texture2D<float>  g_aoBuffer;
[[vk::binding(16, 2)]] SamplerState      g_sampler;

struct VsOut
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
}

// Full-screen triangle, no vertex buffer. Same trick as vsBlit.
[shader("vertex")]
VsOut vsGtao(uint vid : SV_VertexID)
{
    VsOut o;
    o.uv       = float2((vid << 1) & 2, vid & 2);
    o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// ----------------------------------------------------------------------------
// Utilities
// ----------------------------------------------------------------------------

static const float PI = 3.14159265358979323846;

// Decode the G-buffer normal. Our HD opaque pass writes the world-space
// normal mapped to [0,1] in RGB (the .xyz = n * 0.5 + 0.5 packing used by
// the BLS shaders, mirroring the engine's GBuffer slot-2 convention).
float3 decodeNormalWS(float2 uv)
{
    float3 n = g_normal.SampleLevel(g_sampler, uv, 0).xyz;
    n = n * 2.0 - 1.0;
    // Guard against zero / NaN at sky pixels where no opaque draw landed.
    float len = max(length(n), 1e-4);
    return n / len;
}

// Reconstruct a view-space position from a screen UV + the corresponding
// linear view-Z. Uses invProj so the reconstruction is independent of FOV
// / aspect.
float3 viewPosFromUV(float2 uv, float viewZ)
{
    // NDC at the same depth: depth value doesn't matter for the .xy back-
    // projection — we'll rescale by viewZ at the end. Pick z=0.5 so the
    // perspective divide is well-conditioned.
    float4 ndc = float4(uv * 2.0 - 1.0, 0.5, 1.0);
    float4 v   = mul(g_cb.invProj, ndc);
    v.xyz /= v.w;
    // v.z is the reconstructed view Z at depth=0.5; scale the whole vector
    // so its .z matches the real viewZ from the depth buffer.
    float invVz = 1.0 / max(abs(v.z), 1e-4);
    return v.xyz * viewZ * invVz;
}

// A cheap pseudo-blue-noise hash to rotate slices per pixel + per frame.
// Eliminates the visible banding a deterministic slice angle would produce.
float jitter(float2 pixelCoord, uint frameIdx)
{
    float t = float(frameIdx & 0xFFu) * (1.0 / 256.0);
    float h = frac(sin(dot(pixelCoord + t * 17.0, float2(12.9898, 78.233))) * 43758.5453);
    return h;
}

// ----------------------------------------------------------------------------
// Main GTAO pass — writes scalar visibility to the AO buffer. The trace
// body is parameterised over slice/step counts via Slang `let` generics
// so each quality preset compiles to fully-unrolled bytecode. Cost is
// roughly 2 × SLICES × STEPS texture taps + horizon math per pixel.
// ----------------------------------------------------------------------------
float gtaoTrace<let SLICES : int, let STEPS : int>(VsOut input)
{
    float2 uv     = input.uv;
    float  viewZ  = g_linearDepth.SampleLevel(g_sampler, uv, 0).x;

    // linearDepth holds real view-space Z (scene units), not normalised
    // [0,1] depth. Sky pixels were cleared to 1e5; treat anything past
    // 1e4 as "no opaque draw landed here" and return fully unoccluded.
    const float SKY_SENTINEL = 1.0e4;
    if (viewZ <= 0.001 || viewZ >= SKY_SENTINEL)
        return 1.0;

    float3 normalWS = decodeNormalWS(uv);
    float3 normalVS = normalize(mul((float3x3)g_cb.view, normalWS));

    // Force the normal to face the camera (view-space +Z points away).
    if (normalVS.z > 0.0)
        normalVS = -normalVS;

    float3 viewPos = viewPosFromUV(uv, viewZ);

    // Sampling parameters. The screen-space radius shrinks with depth so
    // far-away geometry doesn't trace huge taps across the buffer.
    const float radiusWS = g_cb.params.x;
    const float falloff  = g_cb.params.z * radiusWS;

    // Convert the world-space radius into a screen-space radius via the
    // projection's diagonal. proj[0][0] = 1/(tanHalfFovX*aspect),
    // proj[1][1] = 1/tanHalfFovY. At depth viewZ a unit world step covers
    // proj.x * 0.5 NDC units, which is half the viewport width.
    float radiusNdcX = g_cb.proj[0][0] * radiusWS / max(viewZ, 0.1);
    float radiusNdcY = g_cb.proj[1][1] * radiusWS / max(viewZ, 0.1);
    float2 radiusUv  = float2(radiusNdcX, radiusNdcY) * 0.5; // NDC→UV scale

    float2 pixelCoord = uv * g_cb.viewport.xy;
    float  jt         = jitter(pixelCoord, g_cb.misc.x);

    float visibility = 0.0;

    [unroll]
    for (int s = 0; s < SLICES; ++s)
    {
        float sliceAngle = (float(s) + jt) * (PI / float(SLICES));
        float c          = cos(sliceAngle);
        float si         = sin(sliceAngle);
        float2 sliceDirUV = float2(c, si) * radiusUv / float(STEPS);

        // Cosine of the maximum horizon angle in each direction. Start at
        // a very obtuse angle (cos = -1) so any positive sample wins.
        float h1 = -1.0;
        float h2 = -1.0;

        [unroll]
        for (int t = 1; t <= STEPS; ++t)
        {
            float2 uv1 = uv + sliceDirUV * float(t);
            float2 uv2 = uv - sliceDirUV * float(t);

            float z1 = g_linearDepth.SampleLevel(g_sampler, uv1, 0).x;
            if (z1 > 0.001 && z1 < SKY_SENTINEL)
            {
                float3 vp1   = viewPosFromUV(uv1, z1);
                float3 dir1  = vp1 - viewPos;
                float  d1    = length(dir1);
                if (d1 > 0.001)
                {
                    float fall = saturate(1.0 - d1 / max(falloff, 0.001));
                    float cosH = dot(dir1, normalVS) / d1;
                    h1         = max(h1, cosH * fall);
                }
            }

            float z2 = g_linearDepth.SampleLevel(g_sampler, uv2, 0).x;
            if (z2 > 0.001 && z2 < SKY_SENTINEL)
            {
                float3 vp2   = viewPosFromUV(uv2, z2);
                float3 dir2  = vp2 - viewPos;
                float  d2    = length(dir2);
                if (d2 > 0.001)
                {
                    float fall = saturate(1.0 - d2 / max(falloff, 0.001));
                    float cosH = dot(dir2, normalVS) / d2;
                    h2         = max(h2, cosH * fall);
                }
            }
        }

        // Simplified cosine integral: 1 - mean(max(0, h1), max(0, h2)).
        // The full Jiménez integral folds the slice-plane projection of
        // the normal into the visibility weighting; the simplified form
        // is close enough at the slice/step counts we ship.
        float vis = 1.0 - 0.5 * (max(0.0, h1) + max(0.0, h2));
        visibility += vis;
    }

    visibility /= float(SLICES);
    visibility  = saturate(visibility);

    // Power curve. params.y = 1.0 leaves the linear result; > 1 darkens
    // the occlusion (stronger AO), < 1 lightens it.
    return pow(visibility, max(g_cb.params.y, 0.01));
}

// Quality entries. Each instantiation locks (SLICES, STEPS) at compile
// time, so [unroll] inside gtaoTrace produces fully-flattened code.
//   Low    — 2×2 taps  (4 pairs).  ~4× cheaper than medium.
//   Medium — 4×4 taps  (16 pairs). Default; matches the V1 main pass.
//   High   — 8×8 taps  (64 pairs). ~4× more expensive than medium.
[shader("fragment")]
float gtao_main_low_ps(VsOut input) : SV_Target { return gtaoTrace<2, 2>(input); }

[shader("fragment")]
float gtao_main_ps(VsOut input) : SV_Target { return gtaoTrace<4, 4>(input); }

[shader("fragment")]
float gtao_main_high_ps(VsOut input) : SV_Target { return gtaoTrace<8, 8>(input); }

// ----------------------------------------------------------------------------
// Spatial denoise — 5×5 cross-bilateral filter weighted by view-space
// depth similarity. Removes the high-frequency noise from the main pass
// without softening silhouette edges (large depth deltas get near-zero
// weight). Reads g_aoNoisy + g_linearDepth; writes a scalar to the
// denoised AO target.
// ----------------------------------------------------------------------------
[shader("fragment")]
float gtao_denoise_ps(VsOut input) : SV_Target
{
    float2 uv      = input.uv;
    float2 invSize = g_cb.viewport.zw;
    float  zCenter = g_linearDepth.SampleLevel(g_sampler, uv, 0).x;

    // Sky pixels — same sentinel as the main pass — pass through with no
    // smoothing so the boundary against opaque geometry stays sharp.
    const float SKY_SENTINEL = 1.0e4;
    if (zCenter >= SKY_SENTINEL)
        return g_aoNoisy.SampleLevel(g_sampler, uv, 0).x;

    // depthSigma scales with viewZ so distant geometry tolerates larger
    // absolute deltas than near geometry — keeps the bilateral well-
    // behaved at depth without manually tuning per scene scale.
    const float depthSigma = 0.05 * max(zCenter, 1.0);
    const float invDS2     = 1.0 / (2.0 * depthSigma * depthSigma);

    float sumAo = 0.0;
    float sumW  = 0.0;

    // 5×5 stencil flattened to a single unrolled loop — fxc can't unroll
    // a nested loop containing `continue`. Per-tap spatial weight is the
    // standard Gaussian approximation (σ ≈ 1.5 px). dx/dy are -2..2;
    // packed Chebyshev-distance LUT keeps the kernel symmetric.
    static const float kSpatial[5] = { 0.3679, 0.7788, 1.0, 0.7788, 0.3679 };

    [unroll]
    for (int i = 0; i < 25; ++i)
    {
        int    dx  = (i % 5) - 2;
        int    dy  = (i / 5) - 2;
        float2 ouv = uv + float2(dx, dy) * invSize;
        float  z   = g_linearDepth.SampleLevel(g_sampler, ouv, 0).x;
        float  ao  = g_aoNoisy.SampleLevel(g_sampler, ouv, 0).x;

        float skyMask = (z >= SKY_SENTINEL) ? 0.0 : 1.0;
        float wS      = kSpatial[dx + 2] * kSpatial[dy + 2];
        float dz      = z - zCenter;
        float wD      = exp(-(dz * dz) * invDS2);
        float w       = wS * wD * skyMask;
        sumAo += ao * w;
        sumW  += w;
    }

    return (sumW > 1e-5) ? (sumAo / sumW)
                         : g_aoNoisy.SampleLevel(g_sampler, uv, 0).x;
}

// ----------------------------------------------------------------------------
// Apply pass — reads HDR colour + AO and writes the modulated colour.
// The host opens this pass against hdrColor with loadOp=Load (so the pre-
// existing scene colour survives) and a multiplicative blend (DST × SRC).
// The shader's SRC is the AO factor; blend produces hdrColor *= ao.
//
// Debug modes (driven by g_cb.misc.y):
//   0 — normal modulation (AO factor as RGB; alpha = 1)
//   1 — AO only: pixel = vec3(ao). Host must select a non-blending PSO
//       so the SRV0 read still flows through unmodified.
//   2 — inverted: pixel = vec3(1 - ao). Same constraint as 1.
// ----------------------------------------------------------------------------
[shader("fragment")]
float4 gtao_apply_ps(VsOut input) : SV_Target
{
    float2 uv = input.uv;
    float  ao = g_aoBuffer.SampleLevel(g_sampler, uv, 0).x;

    uint mode = g_cb.misc.y;
    if (mode == 1u)
        return float4(ao, ao, ao, 1.0);
    if (mode == 2u)
        return float4(1.0 - ao, 1.0 - ao, 1.0 - ao, 1.0);

    return float4(ao, ao, ao, 1.0);
}
