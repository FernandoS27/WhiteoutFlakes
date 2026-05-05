// ============================================================================
// WhiteoutDex Renderer — Shared shader types and utilities
// This module is imported by mesh, line, and skin shaders.
// ============================================================================

/// Per-frame constant buffer layout.
/// Must match the C++ CBPerFrame struct in types.h exactly.
public struct PerFrameData
{
    public float4x4 world;
    public float4x4 view;
    public float4x4 projection;
    public float4   lightDir;
    public float4   lightColor;
    public float4   ambientColor;   // .a = alpha test threshold
    public float4   extraParams;    // .x = geoset alpha, .yzw = color tint
    public float4   texAnimParams;  // .x = uOffset, .y = vOffset, .z = uTiling, .w = vTiling
    public float4   materialFlags;  // .x = unshaded, .y = unused, .z = texRotation
}

/// Apply texture coordinate animation matching Previewd's RotateTexture ->
/// ScaleTexture -> TranslateTexture stack. Rotation and scale both pivot
/// around (0.5, 0.5); translation is in UV units with no pivot.
public float2 applyTextureAnimation(float2 uv, float rotation, float4 animParams)
{
    let center = float2(0.5, 0.5);
    if (rotation != 0.0)
    {
        let rel = uv - center;
        let c = cos(rotation);
        let s = sin(rotation);
        uv = float2(c * rel.x - s * rel.y, s * rel.x + c * rel.y) + center;
    }
    // Scale around the UV center (matches Previewd's ScaleTexture).
    if (animParams.z != 0.0 || animParams.w != 0.0)
        uv = (uv - center) * animParams.zw + center;
    // Translation is additive in UV space (not pivoted).
    uv += animParams.xy;
    return uv;
}
