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

ConstantBuffer<PerFrameData> cb;

Texture2D    texDiffuse;
SamplerState samLinear;

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
