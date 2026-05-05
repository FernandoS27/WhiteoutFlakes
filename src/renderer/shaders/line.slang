// ============================================================================
// WhiteoutDex Renderer — Grid / Collision wireframe lines (unlit)
// ============================================================================

import common;

ConstantBuffer<PerFrameData> cb;

struct VSInput
{
    float3 position : POSITION;
    float4 color    : COLOR;
}

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR;
}

// ---- Vertex Shader ----

[shader("vertex")]
PSInput vsLine(VSInput input)
{
    PSInput output;

    let worldPos = mul(float4(input.position, 1.0), cb.world);
    let viewPos  = mul(worldPos, cb.view);
    output.position = mul(viewPos, cb.projection);
    output.color    = input.color;

    return output;
}

// ---- Pixel Shader ----

[shader("fragment")]
float4 psLine(PSInput input) : SV_Target
{
    return input.color;
}
