// ============================================================================
// WhiteoutFlakes Renderer — Unlit: position-only, flat white.
//
// The shader half of UnlitShading (REFACTOR_PLAN.md P8). Deliberately an
// *engine* shader on the embedded slangc path rather than a BLS one: engine
// shaders never enter a `.bls` container, `bls::PsoRequest` requires a
// `const BlsProgram*`, and producing a new bundle needs WDX_BUILD_WC3_SHADERS,
// which is mutually exclusive with the WDX_USE_PREBUILT_SHADERS that CI and
// every fresh clone build with. So this compiles alongside blit / line / gtao
// and lands in compiled_shaders.h.
//
// Its own constant buffer, not common.slang's PerFrameData: that struct mirrors
// C++ CBPerFrame field for field and carries eight vectors of lighting state
// this shader has no use for. Three matrices is the whole contract.
//
// The vertex input declares POSITION only, while the bound buffer is WC3's
// fully interleaved 48-byte Vertex. That is intentional and legal — the input
// layout names one element at offset 0 and passes the full stride, so the
// remaining 36 bytes per vertex are simply not read. This is what "position-
// only vertex stream" means without de-interleaving anything.
// ============================================================================

struct UnlitData
{
    float4x4 world;
    float4x4 view;
    float4x4 projection;
}

ConstantBuffer<UnlitData> cb;

struct VSInput
{
    float3 position : POSITION;
}

struct PSInput
{
    float4 position : SV_POSITION;
}

// ---- Vertex Shader ----
//
// Row-vector convention against column-major storage, matching line.slang and
// every BLS shader: the C++ side transposes before upload, so the multiply
// order here is position × world × view × projection.

[shader("vertex")]
PSInput vsUnlit(VSInput input)
{
    PSInput output;

    let worldPos = mul(float4(input.position, 1.0), cb.world);
    let viewPos  = mul(worldPos, cb.view);
    output.position = mul(viewPos, cb.projection);

    return output;
}

// ---- Pixel Shader ----
//
// Writes SV_Target0 only. In the HD G-buffer pass the pipeline binds three
// attachments; the backend masks writes to the slots a shader does not declare,
// so the cleared linear-depth and normal values survive — the same arrangement
// the debug line PSO already relies on.

[shader("fragment")]
float4 psUnlit(PSInput input) : SV_Target
{
    return float4(1.0, 1.0, 1.0, 1.0);
}
