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

    // Inverse view matrix for the current frame — used to reconstruct
    // world-space positions from view-space Z during temporal reprojection.
    // Host computes this analytically (cheaper than asking the shader to
    // transpose-and-flip-translation).
    float4x4 invView;

    // Previous-frame world→clip transform. On frame 0 the host sets this
    // equal to the current view*proj so history reprojection is a no-op
    // and the blend collapses to "current only" via params.w == 1.
    float4x4 prevViewProj;

    // .xy = viewport size in pixels, .zw = 1/size.
    float4 viewport;

    // .x = world-space sample radius (units of the scene).
    // .y = AO power / intensity (post-exponent).
    // .z = falloff distance multiplier (samples past this drop out).
    // .w = temporal blend weight for *current* frame (0..1). 1.0 disables
    //      history; ~0.1 = strong smoothing. Set by GtaoService each
    //      frame: 1.0 on the very first frame, ~0.1 thereafter.
    float4 params;

    // .x frame index (low bits used for jitter rotation).
    // .y debug mode: 0 = normal apply, 1 = AO only (apply pass writes
    //               vec3(ao) regardless of scene colour), 2 = inverted
    //               (1-ao) for occlusion-only highlight.
    // .zw unused.
    uint4 misc;
}

// PS binding offsets. Slang's WGSL emit preserves the literal
// `vk::binding(N)` value, so the offsets have to match each runtime
// backend's host-slot → @binding mapping. Same trick the BLS shaders
// use in cb_structs.slang. METAL_TARGET follows WebGPU because both
// runtimes share the lower offsets.
//
//   CBs:  host PS CB slot N → @binding(WF_PS_CB_OFFSET + N)
//   SRVs: host PS SRV slot N → @binding(WF_PS_BIND_OFFSET + N)
//
// Vulkan:  CB shift = SRV shift = 16 (kStageBindingShift).
// WebGPU:  CB shift = 4 (kPsCbBindingOffsetWgsl), SRV shift = 12
//          (kStageBindingShift). Tighter to fit the per-stage cap.
#if defined(WGSL_TARGET) || defined(METAL_TARGET)
static const int WF_PS_CB_OFFSET = 4;
static const int WF_PS_BIND_OFFSET = 12;
#else
static const int WF_PS_CB_OFFSET = 16;
static const int WF_PS_BIND_OFFSET = 16;
#endif

[[vk::binding(WF_PS_CB_OFFSET + 0, 0)]] ConstantBuffer<GtaoCB> g_cb : register(b0);

// Pixel-stage SRVs. Each pass samples a disjoint subset:
//   main_ps     — g_linearDepth, g_normal
//   denoise_ps  — g_linearDepth, g_aoNoisy
//   temporal_ps — g_linearDepth, g_aoNoisy (current denoised), g_aoHistory
//   apply_ps    — g_aoBuffer
// `register(tN)` pins the HLSL slot so per-entry emit can't reshuffle
// when textures are added or reordered.
[[vk::binding(WF_PS_BIND_OFFSET + 0, 1)]] Texture2D<float>  g_linearDepth : register(t0);
[[vk::binding(WF_PS_BIND_OFFSET + 1, 1)]] Texture2D<float4> g_normal      : register(t1);
[[vk::binding(WF_PS_BIND_OFFSET + 2, 1)]] Texture2D<float>  g_aoNoisy     : register(t2);
[[vk::binding(WF_PS_BIND_OFFSET + 3, 1)]] Texture2D<float>  g_aoBuffer    : register(t3);
[[vk::binding(WF_PS_BIND_OFFSET + 4, 1)]] Texture2D<float>  g_aoHistory   : register(t4);
[[vk::binding(WF_PS_BIND_OFFSET + 0, 2)]] SamplerState      g_sampler     : register(s0);

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

// Sentinel written into linearDepth at sky / no-opaque pixels by the
// HD opaque pass's clear. Every GTAO entry early-outs (or zero-weights)
// samples whose linearDepth is >= this value. Shared across main_ps,
// denoise_ps, and temporal_ps so the value can't drift.
static const float SKY_SENTINEL = 1.0e4;

// Decode the G-buffer normal. Despite the engine field being called
// `normalWS`, hd_vs.slang transforms with `worldView` — the value the
// HD PS writes to the normal MRT is **view-space**, packed as
// n * 0.5 + 0.5 into RGBA8. Same naming trap as `worldPos`, which the
// HD VS comment calls out explicitly.
float3 decodeNormalVS(float2 uv)
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
    // UV.y=0 at the top of the screen, but NDC.y=+1 at the top — flip Y
    // when going UV → NDC. (vsGtao does the same flip on the way out;
    // here we undo it on the way in.) main_ps survives an unflipped
    // version because it only takes deltas of viewPos values, but the
    // temporal pass needs absolute view-space positions to reproject
    // through the previous frame's view * proj.
    //
    // Depth value doesn't matter for the .xy back-projection — we'll
    // rescale by viewZ at the end. Pick z=0.5 so the perspective divide
    // is well-conditioned.
    float4 ndc = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.5, 1.0);
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
// Main GTAO pass — writes scalar visibility + view-space bent normal via
// MRT. The trace body is parameterised over slice/step counts via Slang
// `let` generics so each quality preset compiles to fully-unrolled
// bytecode. Cost is roughly 2 × SLICES × STEPS texture taps + horizon
// math per pixel.
//
// Bent normal: per slice we average the unoccluded arc midpoint in the
// (normal, sliceTangent) plane, weighted by the slice's visibility, then
// normalise across slices. View-space output; encoded as RGB = n*0.5+0.5
// with the AO factor mirrored to alpha so consumers can sample one
// texture for both terms.
// ----------------------------------------------------------------------------
struct GtaoMainOut {
    float  ao         : SV_Target0;
    float4 bentNormal : SV_Target1;
};

GtaoMainOut gtaoTrace<let SLICES : int, let STEPS : int>(VsOut input)
{
    GtaoMainOut o;
    float2 uv     = input.uv;
    float  viewZ  = g_linearDepth.SampleLevel(g_sampler, uv, 0).x;

    // linearDepth holds real view-space Z (scene units), not normalised
    // [0,1] depth. Sky pixels were cleared to 1e5; treat anything past
    // SKY_SENTINEL as "no opaque draw landed here" and return fully
    // unoccluded. Bent normal defaults to view-space camera-forward
    // (+Z encoded).
    if (viewZ <= 0.001 || viewZ >= SKY_SENTINEL)
    {
        o.ao         = 1.0;
        o.bentNormal = float4(0.5, 0.5, 1.0, 1.0);
        return o;
    }

    // Normal MRT already holds view-space — no further rotation needed.
    // The engine's HD opaque PS runs `orientNormal()` before the MRT
    // write, so the stored normal already faces the camera in LH view
    // (normalVS.z <= 0 for any rendered fragment). A defensive runtime
    // flip on z > 0 sounds safe but causes silhouette artefacts: at
    // grazing angles the encoded z hovers near zero and 8-bit RGBA8
    // quantisation lets adjacent pixels straddle the flip threshold,
    // alternating normal orientation and producing camera-dependent
    // bright streaks in cavities. Trust the MRT contents.
    float3 normalVS = decodeNormalVS(uv);

    float3 viewPos = viewPosFromUV(uv, viewZ);
    float3 bentN   = float3(0.0, 0.0, 0.0);

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

        // Bent-normal contribution — closed-form integral of the
        // unoccluded-direction vector across this slice's visible arc.
        //
        // In the (normal, sliceTangent) plane, parametrise direction by
        // angle φ from normal toward +sliceTangent. The unoccluded arc
        // spans φ ∈ [-acos(h2), +acos(h1)] (an occluder at horizon
        // cosine h blocks angles closer to the normal than acos(h);
        // angles past acos(h) toward tangent stay unoccluded). Clamp h
        // to [0, 1] so "no occluder found" (h<0) caps the integral at
        // the tangent plane rather than wrapping past it.
        //
        //   ∫ sin(φ) dφ  = h2c − h1c
        //   ∫ cos(φ) dφ  = √(1 − h1c²) + √(1 − h2c²)
        //
        // The unnormalised result automatically encodes per-slice
        // visibility magnitude (fully-blocked slice → zero contribution;
        // fully-open slice → full normal+tangent integral), so no
        // separate vis weight is needed.
        float3 sliceTanVS = float3(
             c * 2.0 / max(g_cb.proj[0][0], 1e-4),
            -si * 2.0 / max(g_cb.proj[1][1], 1e-4),
             0.0);
        sliceTanVS -= dot(sliceTanVS, normalVS) * normalVS;
        float tLen = length(sliceTanVS);
        if (tLen > 1e-4)
        {
            sliceTanVS /= tLen;
            float h1c        = saturate(h1);
            float h2c        = saturate(h2);
            float tangentAmt = h2c - h1c;
            float normalAmt  = sqrt(max(0.0, 1.0 - h1c * h1c))
                             + sqrt(max(0.0, 1.0 - h2c * h2c));
            bentN += tangentAmt * sliceTanVS + normalAmt * normalVS;
        }
    }

    visibility /= float(SLICES);
    visibility  = saturate(visibility);

    // Power curve. params.y = 1.0 leaves the linear result; > 1 darkens
    // the occlusion (stronger AO), < 1 lightens it.
    o.ao = pow(visibility, max(g_cb.params.y, 0.01));

    // Bent normal: normalise the accumulator, fall back to the surface
    // normal if it collapsed. Encode view-space xyz into 0..1 and stash
    // the AO factor in alpha so IBL can sample one texture.
    float bLen = length(bentN);
    float3 bDirVS = (bLen > 1e-4) ? (bentN / bLen) : normalVS;
    o.bentNormal = float4(bDirVS * 0.5 + 0.5, o.ao);
    return o;
}

// Quality entries. Each instantiation locks (SLICES, STEPS) at compile
// time, so [unroll] inside gtaoTrace produces fully-flattened code.
//   Low    — 2×2 taps  (4 pairs).  ~4× cheaper than medium.
//   Medium — 4×4 taps  (16 pairs). Default; matches the V1 main pass.
//   High   — 8×8 taps  (64 pairs). ~4× more expensive than medium.
[shader("fragment")]
GtaoMainOut gtao_main_low_ps(VsOut input) { return gtaoTrace<2, 2>(input); }

[shader("fragment")]
GtaoMainOut gtao_main_ps(VsOut input) { return gtaoTrace<4, 4>(input); }

[shader("fragment")]
GtaoMainOut gtao_main_high_ps(VsOut input) { return gtaoTrace<8, 8>(input); }

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
// Temporal denoise — camera-only reprojection. Reconstructs world-space
// position from view-space Z via invView, then projects through the
// previous frame's view*proj to find prev-frame UV. Samples history at
// that UV and blends with current via an exponential moving average.
// Rejects on out-of-frame UV; for AO's low-frequency content that's
// enough — disocclusion ghosts wash out within ~1/alpha frames anyway.
//
// Animated meshes smear slightly because we have no motion vectors —
// V2 trade-off; the smear stays well below the magnitude of the noise
// it replaces. Add motion vectors in V3 if needed.
// ----------------------------------------------------------------------------
[shader("fragment")]
float gtao_temporal_ps(VsOut input) : SV_Target
{
    float2 uv      = input.uv;
    float  viewZ   = g_linearDepth.SampleLevel(g_sampler, uv, 0).x;
    float  current = g_aoNoisy.SampleLevel(g_sampler, uv, 0).x;
    float  alpha   = saturate(g_cb.params.w);

    // Sky / no-geometry pixels and "history-disabled" frames pass the
    // current sample through untouched.
    if (viewZ <= 0.001 || viewZ >= SKY_SENTINEL || alpha >= 0.999)
        return current;

    // view-space → world-space → prev clip → prev UV.
    float3 vp       = viewPosFromUV(uv, viewZ);
    float3 wp       = mul(g_cb.invView, float4(vp, 1.0)).xyz;
    float4 prevClip = mul(g_cb.prevViewProj, float4(wp, 1.0));
    if (prevClip.w <= 0.0)
        return current;
    float2 prevUv = prevClip.xy / prevClip.w * float2(0.5, -0.5) + float2(0.5, 0.5);
    if (any(prevUv < 0.0) || any(prevUv > 1.0))
        return current;

    float history = g_aoHistory.SampleLevel(g_sampler, prevUv, 0).x;
    return lerp(history, current, alpha);
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
