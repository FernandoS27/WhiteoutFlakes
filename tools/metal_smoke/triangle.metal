// Trivial triangle for metal_smoke. No vertex buffer — 3 hardcoded
// positions selected via vertex_id. Per-vertex color drives a smooth
// gradient so we can eyeball that interpolation lands correctly.

#include <metal_stdlib>
using namespace metal;

struct V2F {
    float4 position [[position]];
    float3 color;
};

vertex V2F vs_triangle(uint vid [[vertex_id]]) {
    const float2 positions[3] = {
        float2( 0.0f,  0.7f),
        float2(-0.7f, -0.6f),
        float2( 0.7f, -0.6f),
    };
    const float3 colors[3] = {
        float3(1.0f, 0.0f, 0.0f),
        float3(0.0f, 1.0f, 0.0f),
        float3(0.0f, 0.0f, 1.0f),
    };
    V2F out;
    out.position = float4(positions[vid], 0.0f, 1.0f);
    out.color = colors[vid];
    return out;
}

fragment float4 ps_triangle(V2F in [[stage_in]]) {
    return float4(in.color, 1.0f);
}
