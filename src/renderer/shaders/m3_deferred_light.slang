// ============================================================================
// WhiteoutFlakes Renderer — M3 deferred local lights (M3_SIMPLE_MATERIAL_DESIGN
// §4). One full-screen triangle, a loop over the frame's collected M3 omni
// lights, additively blended onto SceneColor (the PSO carries One/One; this
// shader just returns the contribution, zero where nothing applies).
//
// Reads the sidecar the M3 MRT pass wrote, packed into the 4-attachment
// ceiling: LinearDepth (real view-space Z, 1e5 sky sentinel), Normal (.xyz
// view-space *0.5+0.5, .w = Blinn exponent as sqrt(p/512)) and GBufferDiffuse
// (.rgb linear albedo, .a = mono specular intensity). Unshaded pixels wrote
// zeroed albedo/specular, so they survive this pass untouched — the engine's
// own collapse, not a stencil.
//
// Not SC2's per-light volume quads: a viewer scene has a handful of lights
// and one loop keeps the host side to a CB upload. Attenuation is the linear
// M3 window (attenuationStart..End); the per-light decay exponent does not
// survive into LightState yet, so it is not applied.
// ============================================================================

static const int kM3MaxDeferredLights = 16;

struct M3DeferredLightData
{
    // .x = 1/proj[0][0], .y = 1/proj[1][1] — view-ray reconstruction for a
    // standard centred perspective, the same one GTAO uses. .z = light count.
    float4 unproject;
    float4 lightPos[kM3MaxDeferredLights];   // view space; .w = attenuationEnd
    float4 lightColor[kM3MaxDeferredLights]; // .rgb diffuse; .w = attenuationStart
}

#if defined(WGSL_TARGET) || defined(METAL_TARGET)
static const int WF_PS_BIND_OFFSET = 12;
#else
static const int WF_PS_BIND_OFFSET = 16;
#endif

[[vk::binding(0, 0)]] ConstantBuffer<M3DeferredLightData> cb : register(b0);

[[vk::binding(WF_PS_BIND_OFFSET + 0, 1)]] Texture2D<float>  g_linearDepth : register(t0);
[[vk::binding(WF_PS_BIND_OFFSET + 1, 1)]] Texture2D<float4> g_normalSpec  : register(t1);
[[vk::binding(WF_PS_BIND_OFFSET + 2, 1)]] Texture2D<float4> g_diffuseSpec : register(t2);
[[vk::binding(WF_PS_BIND_OFFSET + 0, 2)]] SamplerState g_sampler : register(s0);

// Past this, LinearDepth still holds its 1e5 clear — no opaque draw landed.
static const float kDepthSentinel = 9.0e4;

struct VsOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

[shader("vertex")]
VsOut vsM3DeferredLight(uint vid : SV_VertexID)
{
    // The standard vertex-id full-screen triangle, matching gtao.slang.
    VsOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

[shader("pixel")]
float4 psM3DeferredLight(VsOut input) : SV_Target0
{
    let viewZ = g_linearDepth.SampleLevel(g_sampler, input.uv, 0);
    if (viewZ >= kDepthSentinel)
        return float4(0.0, 0.0, 0.0, 0.0);

    // Reconstruct the view-space position: clip.xy = view.xy * proj diag,
    // w = viewZ, so view.xy = ndc.xy * viewZ / diag.
    float2 ndc = float2(input.uv.x * 2.0 - 1.0, 1.0 - input.uv.y * 2.0);
    float3 viewPos = float3(ndc * cb.unproject.xy * viewZ, viewZ);

    let normalSpec = g_normalSpec.SampleLevel(g_sampler, input.uv, 0);
    float3 n = normalize(normalSpec.xyz * 2.0 - 1.0);
    let specPower = max(normalSpec.w * normalSpec.w * 512.0, 1.0);
    let diffuseSpec = g_diffuseSpec.SampleLevel(g_sampler, input.uv, 0);
    let toEye = -normalize(viewPos);

    float3 c = float3(0.0, 0.0, 0.0);
    let count = int(cb.unproject.z);
    for (int i = 0; i < kM3MaxDeferredLights; ++i)
    {
        if (i >= count)
            break;
        let toLight = cb.lightPos[i].xyz - viewPos;
        let d = length(toLight);
        let attenEnd = cb.lightPos[i].w;
        let attenStart = cb.lightColor[i].w;
        let atten = saturate((attenEnd - d) / max(attenEnd - attenStart, 1e-4));
        if (atten <= 0.0)
            continue;
        let l = toLight / max(d, 1e-4);
        let ndl = saturate(dot(n, l));
        if (ndl <= 0.0)
            continue;
        float3 term = diffuseSpec.rgb * ndl;
        let h = normalize(l + toEye);
        term += diffuseSpec.a * pow(saturate(dot(n, h)), specPower);
        c += term * cb.lightColor[i].rgb * atten;
    }
    return float4(c, 0.0);
}
