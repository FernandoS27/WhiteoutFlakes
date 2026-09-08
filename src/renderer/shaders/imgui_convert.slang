// ============================================================================
// WhiteoutFlakes Renderer — ImGui colour-space-converting image draws.
//
// The retail imgui.bls pair passes the sampled texel through untouched, which
// is only correct when the texel's arrival space matches the RTV's expected
// space: an sRGB-typed texture arrives linear, a UNORM one arrives
// display-referred, and the two ImGui frame flavours (sRGB RTV in tonemapped
// frames, UNORM RTV in gamma frames) each expect one of those. ImGui::Image
// draws of engine textures (live thumbnails) cross that grid freely — a
// tonemapped thumbnail cell shown while a gamma document is up used to display
// its linear pixels raw, a gamma step too dark.
//
// These entries are the mismatch half of the matrix; ImGuiRenderer swaps to
// them per draw when IGFXDevice::IsTextureSrgb disagrees with the RTV's
// sRGB-ness. The matched half keeps the retail bytecode (imgui_ps.slang is
// differentially tested against the shipped game, so its permutation set
// cannot grow — which is why this is an engine shader in compiled_shaders.h
// and not a BLS one).
//
//   psImGuiToLinear   UNORM texture on an sRGB RTV — decode the texel (and,
//                     as retail perm_000 does on this RTV, the vertex tint).
//   psImGuiToDisplay  sRGB texture on a UNORM RTV — encode the texel back to
//                     display bytes; the vertex tint stays raw like perm_001.
//
// The VS is a self-contained twin of the retail imgui VS (same 48-byte input
// vertex, same 192-byte CB layout) so the pair never depends on cross-module
// interstage layout agreement.
// ============================================================================

struct ImGuiConvertCb
{
    // Matches the C++ ImGuiVsCb: 8 float4 of padding, then the four
    // projection columns the retail VS reads as cb2[8], [9], [10], [11].
    float4 pad[8];
    float4 colX; // projection column 0
    float4 colY; // projection column 1
    float4 colZ; // projection column 2 (unused, like the retail cb2[10])
    float4 colT; // translation column
}

ConstantBuffer<ImGuiConvertCb> cb;

// Digit-free semantics on purpose: this slangc appends a field suffix digit
// to user semantics ("ATTR1" comes out as ATTR index 10 in the DXBC ISGN,
// and the PSO fails against a layout saying index 1). "POSITION"/"COLOR"/
// "TEXCOORD" gain a harmless trailing 0 instead. The explicit locations pin
// the SPIR-V/WGSL side to the element ORDER of kImGuiConvertInput, which is
// what the non-ATTR fallback in each backend's layout translation uses.
struct VSInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color    : COLOR;
    [[vk::location(2)]] float2 uv       : TEXCOORD;
}

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color    : COLOR0;
    float2 uv       : TEXCOORD0;
}

[shader("vertex")]
PSInput vsImGuiConvert(VSInput input)
{
    PSInput output;
    output.position = cb.colX * input.position.x + cb.colY * input.position.y + cb.colT;
    output.color = input.color;
    output.uv = input.uv;
    return output;
}

// The VS consumes three of the four 48-byte EngineImGuiVert attributes (the
// engine vertex's ATTR1 padding is skipped by offset, not by an element).

// Per-backend binding offsets mirror blit.slang / gtao.slang. Vulkan + D3D
// pixel SRV/sampler slots start at binding 16; WebGPU/Metal pack them at 12.
// The runtime binds via `BindShaderResource(Pixel, 0, …)` and translates
// slot 0 → binding (12 or 16) per backend.
#if defined(WGSL_TARGET) || defined(METAL_TARGET)
static const int WF_PS_BIND_OFFSET = 12;
#else
static const int WF_PS_BIND_OFFSET = 16;
#endif
[[vk::binding(WF_PS_BIND_OFFSET + 0, 1)]] Texture2D<float4> gTexture;
[[vk::binding(WF_PS_BIND_OFFSET + 0, 2)]] SamplerState gSampler;

// IEC 61966-2-1 transfer pair, constants verbatim from the engine's
// math/color.slang so a decode here inverts the encode a gamma frame wrote.
float3 wfLinearToSrgb(float3 linear)
{
    float3 curve = exp2(log2(abs(linear)) * 0.416666657);
    curve = curve * 1.05499995 + (-0.0549999997);
    float3 lin = 12.9200001 * linear;
    return select(linear <= float3(0.00313080009), lin, curve);
}

float3 wfSrgbToLinear(float3 srgb)
{
    float3 curve = exp2(log2(abs((srgb + 0.0549999997) * 0.947867334)) * 2.4000001);
    float3 lin = 0.0773993805 * srgb;
    return select(srgb <= float3(0.0404499993), lin, curve);
}

// UNORM texture (display-referred bytes) drawn into an sRGB RTV (expects
// linear): decode the texel; the vertex tint decodes too, matching how the
// retail perm_000 treats tints on this RTV. Alpha is never gamma-mapped.
[shader("fragment")]
float4 psImGuiToLinear(PSInput input) : SV_Target0
{
    float4 sample = gTexture.Sample(gSampler, input.uv);
    return float4(wfSrgbToLinear(input.color.rgb) * wfSrgbToLinear(sample.rgb),
                  input.color.a * sample.a);
}

// sRGB texture (texel arrives linear) drawn into a UNORM RTV (expects display
// bytes): encode the texel; the vertex tint stays raw like retail perm_001.
[shader("fragment")]
float4 psImGuiToDisplay(PSInput input) : SV_Target0
{
    float4 sample = gTexture.Sample(gSampler, input.uv);
    return float4(input.color.rgb * wfLinearToSrgb(sample.rgb),
                  input.color.a * sample.a);
}
