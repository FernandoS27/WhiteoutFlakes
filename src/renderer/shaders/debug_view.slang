// ============================================================================
// WhiteoutFlakes Renderer — debug views, output half.
//
// Every product's debug pixel entry fills a DebugSurface from the SAME code its
// real shader shades with, then hands it to DebugPbr (Reforged HD materials) or
// DebugLegacy (everything else). The view logic, the ramps, the "channel not
// present" hatch and the display encoding live here once — see
// DEBUG_VIEW_DESIGN.md. core/debug_view.h is the C++ half.
//
// The constant buffer is NOT declared here: its slot is each product's to pick
// (b3 everywhere, but under two binding conventions), so the adapters declare
// `ConstantBuffer<DebugViewData>` themselves and pass the value in.
// ============================================================================

// DebugView values (include/whiteout/flakes/enums.h).
public static const uint kDbgAlbedo = 1;
public static const uint kDbgNormal = 2;
public static const uint kDbgTextureMip = 3;
public static const uint kDbgLightCount = 4;
public static const uint kDbgLightingWhite = 5;
public static const uint kDbgLightingGrey = 6;
public static const uint kDbgSpecularOnly = 7;
public static const uint kDbgNoOrm = 8;
public static const uint kDbgAoOnly = 9;
public static const uint kDbgRoughness = 10;
public static const uint kDbgMetalness = 11;
public static const uint kDbgMaterialAo = 12;
public static const uint kDbgEmissive = 13;
public static const uint kDbgTeamMask = 14;
public static const uint kDbgVertexNormal = 15;
public static const uint kDbgSpecular = 16;
public static const uint kDbgGloss = 17;
public static const uint kDbgVertexColor = 18;
public static const uint kDbgOpacity = 19;

// DebugSurface::present bits.
public static const uint kDbgHasAlbedo = 1u << 0;
public static const uint kDbgHasNormal = 1u << 1;
public static const uint kDbgHasVertexNormal = 1u << 2;
public static const uint kDbgHasSpecular = 1u << 3;
public static const uint kDbgHasGloss = 1u << 4;
public static const uint kDbgHasRoughness = 1u << 5;
public static const uint kDbgHasMetalness = 1u << 6;
public static const uint kDbgHasMaterialAo = 1u << 7;
public static const uint kDbgHasEmissive = 1u << 8;
public static const uint kDbgHasTeamMask = 1u << 9;
public static const uint kDbgHasVertexColor = 1u << 10;
public static const uint kDbgHasLit = 1u << 11;
public static const uint kDbgHasLightCount = 1u << 12;
public static const uint kDbgHasMip = 1u << 13;

// DebugViewData::ctl.y bits (core::DebugTargetInfo).
public static const uint kDbgColorSamplesLinear = 1u;
public static const uint kDbgTargetEncodesSrgb = 2u;

public struct DebugViewData
{
    // .x DebugView, .y target flags, .z the draw's light count for products
    // that choose lights per draw, .w product-specific.
    public uint4 ctl;
    // .x light count at which the ramp reaches red, .yzw product-specific.
    public float4 params;
};

public struct DebugSurface
{
    public uint present;
    // Colour channels. Linear when the frame acquired its colour textures
    // through sRGB views (ctl.y bit 0), texel values otherwise.
    public float3 albedo;
    public float3 specular;
    public float3 emissive;
    // Data channels, shown as they are.
    public float3 normalWS;
    public float normalLen;       // pre-normalise length of the shading normal
    public float3 vertexNormalWS;
    public float gloss;
    public float roughness;
    public float metalness;
    public float materialAo;
    public float teamMask;
    public float3 vertexColor;
    public float opacity;
    public float lightCount;
    public float mipLevel;
    // Radiance for the lighting views, already lit by the product's own code.
    public float3 lit;
};

public DebugSurface DebugSurfaceInit()
{
    DebugSurface s;
    s.present = 0;
    s.albedo = float3(0, 0, 0);
    s.specular = float3(0, 0, 0);
    s.emissive = float3(0, 0, 0);
    s.normalWS = float3(0, 0, 1);
    s.normalLen = 1.0;
    s.vertexNormalWS = float3(0, 0, 1);
    s.gloss = 0;
    s.roughness = 0;
    s.metalness = 0;
    s.materialAo = 1;
    s.teamMask = 0;
    s.vertexColor = float3(1, 1, 1);
    s.opacity = 1;
    s.lightCount = 0;
    s.mipLevel = 0;
    s.lit = float3(0, 0, 0);
    return s;
}

public uint DebugMode(DebugViewData d)
{
    return d.ctl.x;
}

// Which lighting override the product has to evaluate for this view: 0 none,
// 1 white base colour, 2 grey, 3 black, 4 neutral ORM with the real base.
public uint DebugLightingOverride(DebugViewData d)
{
    switch (d.ctl.x)
    {
    case kDbgLightingWhite: return 1;
    case kDbgLightingGrey: return 2;
    case kDbgSpecularOnly: return 3;
    case kDbgNoOrm: return 4;
    default: return 0;
    }
}

// The base colour a lighting override substitutes, or `albedo` when it does
// not. Linear, like the lit path consumes it.
public float3 DebugOverrideAlbedo(DebugViewData d, float3 albedo)
{
    switch (DebugLightingOverride(d))
    {
    case 1: return float3(1, 1, 1);
    case 2: return float3(0.5, 0.5, 0.5);
    case 3: return float3(0, 0, 0);
    default: return albedo;
    }
}

// Mip level a sampler would pick for `uv` on a texture of `size` texels.
// Derivatives, so call it before any branch a sampled value could steer —
// Dawn rejects them under non-uniform control flow.
public float DebugMipLevel(float2 uv, float2 size)
{
    let dx = ddx(uv * size);
    let dy = ddy(uv * size);
    let rho = max(dot(dx, dx), dot(dy, dy));
    return max(0.5 * log2(max(rho, 1e-8)), 0.0);
}

float3 DbgLinearToSrgb(float3 c)
{
    c = saturate(c);
    let lo = c * 12.92;
    let hi = 1.055 * pow(c, float3(1.0 / 2.4, 1.0 / 2.4, 1.0 / 2.4)) - 0.055;
    return lerp(hi, lo, step(c, float3(0.0031308, 0.0031308, 0.0031308)));
}

float3 DbgSrgbToLinear(float3 c)
{
    c = saturate(c);
    let lo = c / 12.92;
    let hi = pow((c + 0.055) / 1.055, float3(2.4, 2.4, 2.4));
    return lerp(hi, lo, step(c, float3(0.04045, 0.04045, 0.04045)));
}

// A display value — what should reach the screen — for the target the frame
// writes. An sRGB view would encode it a second time.
float4 DbgOut(DebugViewData d, float3 display, float alpha)
{
    display = saturate(display);
    if ((d.ctl.y & kDbgTargetEncodesSrgb) != 0)
        display = DbgSrgbToLinear(display);
    return float4(display, alpha);
}

float3 DbgColor(DebugViewData d, float3 c)
{
    return ((d.ctl.y & kDbgColorSamplesLinear) != 0) ? DbgLinearToSrgb(c) : saturate(c);
}

float4 DbgAbsent(DebugViewData d, float2 svPos)
{
    // Diagonal hatch: black is a legitimate channel value, so "this surface has
    // no such channel" must not look like one.
    let band = (uint(svPos.x + svPos.y) / 6u) & 1u;
    let c = (band != 0u) ? float3(0.55, 0.0, 0.55) : float3(0.12, 0.0, 0.12);
    return DbgOut(d, c, 1.0);
}

float3 DbgNormal(float3 n, float len)
{
    return (n * len + float3(1, 1, 1)) * 0.5;
}

float3 DbgMipRamp(float level)
{
    // 2.0.0's palette, one hue per whole level and brightness across the step.
    let whole = uint(level);
    let shade = frac(level) * 0.5 + 0.5;
    switch (whole)
    {
    case 0: return float3(1, 0, 0) * shade;
    case 1: return float3(0, 1, 0) * shade;
    case 2: return float3(0, 0, 1) * shade;
    case 3: return float3(1, 1, 0) * shade;
    case 4: return float3(1, 0, 1) * shade;
    default: return float3(1, 1, 1);
    }
}

float3 DbgCountRamp(float count, float redAt)
{
    if (count < 0.5)
        return float3(0, 0, 0);
    let top = max(redAt, 1.0);
    if (count > top + 0.5)
        return float3(1, 1, 1);
    // One light is blue, `redAt` is red: blue, cyan, green, yellow, red.
    let x = 4.0 * saturate((count - 1.0) / max(top - 1.0, 1.0));
    return float3(saturate(x - 2.0), saturate(x) - saturate(x - 3.0), 1.0 - saturate(x - 1.0));
}

// Views both families share. Returns false when `mode` is not one of them.
bool DbgShared(DebugSurface s, DebugViewData d, float2 svPos, out float4 o)
{
    let mode = d.ctl.x;
    o = float4(0, 0, 0, 1);
    switch (mode)
    {
    case kDbgAlbedo:
        o = ((s.present & kDbgHasAlbedo) != 0) ? DbgOut(d, DbgColor(d, s.albedo), s.opacity)
                                               : DbgAbsent(d, svPos);
        return true;
    case kDbgNormal:
        o = ((s.present & kDbgHasNormal) != 0)
                ? DbgOut(d, DbgNormal(s.normalWS, s.normalLen), s.opacity)
                : DbgAbsent(d, svPos);
        return true;
    case kDbgTextureMip:
        o = ((s.present & kDbgHasMip) != 0) ? DbgOut(d, DbgMipRamp(s.mipLevel), s.opacity)
                                            : DbgAbsent(d, svPos);
        return true;
    case kDbgLightCount:
        o = ((s.present & kDbgHasLightCount) != 0)
                ? DbgOut(d, DbgCountRamp(s.lightCount, d.params.x), s.opacity)
                : DbgAbsent(d, svPos);
        return true;
    case kDbgLightingWhite:
    case kDbgLightingGrey:
    case kDbgSpecularOnly:
        // Radiance: the frame's tonemap still runs over it.
        o = ((s.present & kDbgHasLit) != 0) ? float4(s.lit, s.opacity) : DbgAbsent(d, svPos);
        return true;
    case kDbgEmissive:
        o = ((s.present & kDbgHasEmissive) != 0)
                ? DbgOut(d, DbgColor(d, s.emissive), s.opacity)
                : DbgAbsent(d, svPos);
        return true;
    default:
        return false;
    }
}

public float4 DebugPbr(DebugSurface s, DebugViewData d, float2 svPos)
{
    float4 o;
    if (DbgShared(s, d, svPos, o))
        return o;
    switch (d.ctl.x)
    {
    case kDbgNoOrm:
        return ((s.present & kDbgHasLit) != 0) ? float4(s.lit, s.opacity) : DbgAbsent(d, svPos);
    case kDbgRoughness:
        return ((s.present & kDbgHasRoughness) != 0)
                   ? DbgOut(d, float3(s.roughness, s.roughness, s.roughness), s.opacity)
                   : DbgAbsent(d, svPos);
    case kDbgMetalness:
        return ((s.present & kDbgHasMetalness) != 0)
                   ? DbgOut(d, float3(s.metalness, s.metalness, s.metalness), s.opacity)
                   : DbgAbsent(d, svPos);
    case kDbgMaterialAo:
        return ((s.present & kDbgHasMaterialAo) != 0)
                   ? DbgOut(d, float3(s.materialAo, s.materialAo, s.materialAo), s.opacity)
                   : DbgAbsent(d, svPos);
    case kDbgTeamMask:
        return ((s.present & kDbgHasTeamMask) != 0)
                   ? DbgOut(d, float3(s.teamMask, s.teamMask, s.teamMask), s.opacity)
                   : DbgAbsent(d, svPos);
    default:
        // A legacy view on a PBR surface.
        return DbgAbsent(d, svPos);
    }
}

public float4 DebugLegacy(DebugSurface s, DebugViewData d, float2 svPos)
{
    float4 o;
    if (DbgShared(s, d, svPos, o))
        return o;
    switch (d.ctl.x)
    {
    case kDbgVertexNormal:
        return ((s.present & kDbgHasVertexNormal) != 0)
                   ? DbgOut(d, DbgNormal(s.vertexNormalWS, 1.0), s.opacity)
                   : DbgAbsent(d, svPos);
    case kDbgSpecular:
        return ((s.present & kDbgHasSpecular) != 0)
                   ? DbgOut(d, DbgColor(d, s.specular), s.opacity)
                   : DbgAbsent(d, svPos);
    case kDbgGloss:
        return ((s.present & kDbgHasGloss) != 0)
                   ? DbgOut(d, float3(s.gloss, s.gloss, s.gloss), s.opacity)
                   : DbgAbsent(d, svPos);
    case kDbgVertexColor:
        return ((s.present & kDbgHasVertexColor) != 0)
                   ? DbgOut(d, s.vertexColor, s.opacity)
                   : DbgAbsent(d, svPos);
    case kDbgOpacity:
        return DbgOut(d, float3(s.opacity, s.opacity, s.opacity), 1.0);
    default:
        // A PBR view on a legacy surface.
        return DbgAbsent(d, svPos);
    }
}
