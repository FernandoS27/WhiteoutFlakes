// ============================================================================
// WhiteoutDex Renderer — ViewCube shader
// Dedicated VS/PS pair for the debug ViewCube cube faces. The Home button
// still draws through line.slang; this shader only covers the 6 textured
// atlas-mapped faces and their simple directional lighting.
//
// Kept outside the BLS pipeline because the ViewCube is a debug overlay,
// not an MDX material, and piping it through blsSdProgram_ would force us
// to shape a FrameInputs / MatParams call site around a single draw. The
// CB layout reuses PerFrameData so the existing cbPerFrame_ upload in
// DebugRenderer::RenderViewCube feeds this shader unchanged.
// ============================================================================

import common;

// Explicit binding decorations so the SPIR-V the Vulkan backend consumes
// lands in the engine's fixed descriptor model:
//   set 0 = uniform buffers, set 1 = sampled images, set 2 = samplers,
//   with PS-stage slot N shifted to binding N + 16 (kStageBindingShift).
// `cb` is read by BOTH vsViewCube and psViewCube; set-0 binding-0 works
// for both because every binding in the engine's layout is VERTEX |
// FRAGMENT visible and DebugRenderer::RenderViewCube populates it via
// BindConstantBuffer(Vertex, 0, ...). texDiffuse / samLinear are PS-only
// at slot 0 → binding 16. The register() specifiers keep the DXBC build
// (which ignores vk::binding) pinned to the same b0/t0/s0 the engine's
// d3d backends expect.
[[vk::binding(0, 0)]]  ConstantBuffer<PerFrameData> cb         : register(b0);
[[vk::binding(16, 1)]] Texture2D                    texDiffuse : register(t0);
[[vk::binding(16, 2)]] SamplerState                 samLinear  : register(s0);

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float4 color    : COLOR;
    float2 uv       : TEXCOORD0;
}

struct PSInput
{
    float4 position : SV_POSITION;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
}

[shader("vertex")]
PSInput vsViewCube(VSInput input)
{
    PSInput output;
    let worldPos    = mul(float4(input.position, 1.0), cb.world);
    let viewPos     = mul(worldPos, cb.view);
    output.position = mul(viewPos, cb.projection);
    output.normal   = normalize(mul(input.normal, (float3x3)cb.world));
    output.uv       = input.uv;
    return output;
}

[shader("fragment")]
float4 psViewCube(PSInput input) : SV_Target
{
    let N = normalize(input.normal);
    let L = normalize(-cb.lightDir.xyz);

    let NdotL   = saturate(dot(N, L));
    let diffuse = cb.lightColor.rgb * NdotL;
    let ambient = cb.ambientColor.rgb;

    let texColor = texDiffuse.Sample(samLinear, input.uv);
    let lighting = ambient + diffuse;

    return float4(lighting * texColor.rgb, texColor.a);
}
