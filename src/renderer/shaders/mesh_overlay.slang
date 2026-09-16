// ============================================================================
// WhiteoutFlakes Renderer — mesh overlay: edges, vertex dots and flat faces.
//
// What the wireframe debug views draw, and what a model editor will select
// with. Nothing here knows a product. Each product's overlay entry points
// (m3_overlay.slang, wc3_overlay.slang, …) implement IMeshOverlaySkin with that
// product's OWN vertex code, so an edge lies exactly on the surface the product
// drew — a wireframe that drifts off its mesh looks exactly like a skinning bug.
//
// Geometry is pulled, not streamed. One StructuredBuffer<float4> per geoset
// holds the vertex records, the unique edges, the triangles and every
// element's state (core/mesh_overlay.h packs it), and SV_VertexID walks it: no
// input layout and no index buffer, so one draw shape serves a WC3 interleaved
// vertex, an `.m3` blob and a Diablo III cloth alike. Selecting an element is
// rewriting one state value; nothing here has to be rebuilt.
//
// The constant buffer is not declared here, for debug_view.slang's reason: the
// products read it at different slots, so their entries declare it and pass
// the value in.
// ============================================================================

// MeshOverlayData::ctl.x
public static const uint kMeshOverlayFaces = 0;
public static const uint kMeshOverlayEdges = 1;
public static const uint kMeshOverlayVertices = 2;

// MeshOverlayData::ctl.y
public static const uint kMeshOverlayTargetEncodesSrgb = 1u;
// Faces pass that draws only the marked (selected / hovered) faces, as a tint
// over surfaces the product already drew.
public static const uint kMeshOverlayMarkedFacesOnly = 2u;

// Element state bits (core::MeshElementFlag).
public static const uint kMeshElementSelected = 1u;
public static const uint kMeshElementHovered = 2u;
public static const uint kMeshElementHidden = 4u;

public struct MeshOverlayData
{
    // .x kind, .y flags
    public uint4 ctl;
    // Where each section starts in the element buffer: .x vertex records
    // (three per vertex), .y edges, .z faces.
    public uint4 sections;
    // .xy half the viewport in pixels, .z edge width, .w vertex dot diameter.
    public float4 viewport;
    // .x clip-space z subtracted so an element wins against its own surface.
    public float4 depth;
    public float4 faceColor;
    public float4 edgeColor;
    public float4 vertexColor;
    public float4 selectedColor;
    public float4 hoveredColor;
};

// VS t17, numbered past both stages' texture slots the way cb_structs.slang
// numbers t_hdBones (VsBufferBindingOffset): the backends' 2 * stage shift.
#ifdef WGSL_TARGET
[[vk::binding(41, 1)]] StructuredBuffer<float4> gMeshOverlayElements : register(t17);
#else
[[vk::binding(49, 1)]] StructuredBuffer<float4> gMeshOverlayElements : register(t17);
#endif

// One vertex, in the form the input stage would have handed the product:
// UNORM weights already divided, indices as integers.
public struct MeshOverlayVertex
{
    public float3 position;
    public float4 weights;
    public uint4 indices;
};

public interface IMeshOverlaySkin
{
    // Clip-space position of one vertex through the product's vertex stage.
    static float4 ClipPosition(MeshOverlayVertex v);
};

public struct MeshOverlayVSOut
{
    public float4 position : SV_Position;
    public float4 color : COLOR0;
    // Dots only: the ring drawn around the fill.
    public float4 rim : COLOR1;
    // .xy pixel offset from the element's centre (lines use .y alone), .z the
    // half extent in pixels, .w shape: 0 flat, 1 line, 2 dot.
    public noperspective float4 shape : TEXCOORD0;
};

static const float kMeshOverlayNearW = 1e-4;

MeshOverlayVertex MeshOverlayFetch(MeshOverlayData d, uint v, out uint state)
{
    let base = d.sections.x + v * 3u;
    let p = gMeshOverlayElements[base];
    MeshOverlayVertex o;
    o.position = p.xyz;
    o.weights = gMeshOverlayElements[base + 1u];
    // Small integers, exact in a float.
    o.indices = uint4(gMeshOverlayElements[base + 2u] + 0.5);
    state = uint(p.w + 0.5);
    return o;
}

float3 MeshOverlaySrgbToLinear(float3 c)
{
    c = saturate(c);
    let lo = c / 12.92;
    let hi = pow((c + 0.055) / 1.055, float3(2.4, 2.4, 2.4));
    return lerp(hi, lo, step(c, float3(0.04045, 0.04045, 0.04045)));
}

// The colours arrive as display values; an sRGB target would encode them again.
float4 MeshOverlayOut(MeshOverlayData d, float4 c)
{
    if ((d.ctl.y & kMeshOverlayTargetEncodesSrgb) != 0u)
        c.rgb = MeshOverlaySrgbToLinear(c.rgb);
    return c;
}

float4 MeshOverlayStateColor(MeshOverlayData d, float4 base, uint state)
{
    let selected = (state & kMeshElementSelected) != 0u;
    let hovered = (state & kMeshElementHovered) != 0u;
    if (selected && hovered)
        return float4(lerp(d.selectedColor.rgb, float3(1, 1, 1), 0.35), d.selectedColor.a);
    if (selected)
        return d.selectedColor;
    if (hovered)
        return d.hoveredColor;
    return base;
}

// Outside the clip volume on x, so every corner of the element fails the same
// plane and the rasteriser drops the whole thing.
MeshOverlayVSOut MeshOverlayCulled()
{
    MeshOverlayVSOut o;
    o.position = float4(2.0, 2.0, 0.5, 1.0);
    o.color = float4(0, 0, 0, 0);
    o.rim = float4(0, 0, 0, 0);
    o.shape = float4(0, 0, 0, 0);
    return o;
}

float4 MeshOverlayPixelToClip(MeshOverlayData d, float2 px, float4 anchor)
{
    return float4(px / d.viewport.xy * anchor.w, anchor.z - d.depth.x, anchor.w);
}

// What one overlay corner needs before any product code runs: the element's
// state, the (up to two) vertices to position, and where this corner sits.
struct MeshOverlayCorner
{
    uint state;
    uint vertex[2];
    uint corner;
};

// Six corners per edge, two triangles: (end, side).
static const float kMeshOverlayEdgeEnd[6] = { 0, 1, 1, 0, 1, 0 };
static const float kMeshOverlayEdgeSide[6] = { -1, -1, 1, -1, 1, 1 };

static const float2 kMeshOverlayQuad[6] = {
    float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, -1), float2(1, 1), float2(-1, 1)
};

MeshOverlayCorner MeshOverlayResolveCorner(MeshOverlayData d, uint vid)
{
    MeshOverlayCorner c;
    switch (d.ctl.x)
    {
    case kMeshOverlayFaces:
    {
        let tri = gMeshOverlayElements[d.sections.z + vid / 3u];
        c.corner = vid % 3u;
        c.state = uint(tri.w + 0.5);
        c.vertex[0] = uint(tri[c.corner] + 0.5);
        c.vertex[1] = c.vertex[0];
        break;
    }
    case kMeshOverlayEdges:
    {
        let edge = gMeshOverlayElements[d.sections.y + vid / 6u];
        c.corner = vid % 6u;
        c.state = uint(edge.z + 0.5);
        c.vertex[0] = uint(edge.x + 0.5);
        c.vertex[1] = uint(edge.y + 0.5);
        break;
    }
    default:
    {
        c.corner = vid % 6u;
        c.vertex[0] = vid / 6u;
        c.vertex[1] = c.vertex[0];
        c.state = uint(gMeshOverlayElements[d.sections.x + c.vertex[0] * 3u].w + 0.5);
        break;
    }
    }
    return c;
}

MeshOverlayVSOut MeshOverlayFace(MeshOverlayData d, MeshOverlayCorner c, float4 clip)
{
    let markedOnly = (d.ctl.y & kMeshOverlayMarkedFacesOnly) != 0u;
    let marked = (c.state & (kMeshElementSelected | kMeshElementHovered)) != 0u;
    if (markedOnly && !marked)
        return MeshOverlayCulled();

    MeshOverlayVSOut o;
    o.position = clip;
    float4 color;
    if (markedOnly)
    {
        // A tint over the product's own surface, so it has to win that depth.
        o.position.z -= d.depth.x;
        color = MeshOverlayStateColor(d, float4(0, 0, 0, 0), c.state);
        color.a *= 0.4;
    }
    else
    {
        color = d.faceColor;
        if (marked)
            color.rgb = lerp(color.rgb, MeshOverlayStateColor(d, color, c.state).rgb, 0.6);
    }
    o.color = MeshOverlayOut(d, color);
    o.rim = float4(0, 0, 0, 0);
    o.shape = float4(0, 0, 0, 0);
    return o;
}

MeshOverlayVSOut MeshOverlayEdge(MeshOverlayData d, MeshOverlayCorner c, float4 a, float4 b)
{
    if (a.w <= kMeshOverlayNearW && b.w <= kMeshOverlayNearW)
        return MeshOverlayCulled();
    // Slide an end that sits behind the eye up to it; the rasteriser's own near
    // clip takes the rest.
    if (a.w <= kMeshOverlayNearW)
        a = lerp(a, b, (kMeshOverlayNearW - a.w) / (b.w - a.w));
    else if (b.w <= kMeshOverlayNearW)
        b = lerp(b, a, (kMeshOverlayNearW - b.w) / (a.w - b.w));

    let pa = a.xy / a.w * d.viewport.xy;
    let pb = b.xy / b.w * d.viewport.xy;
    var dir = pb - pa;
    let len = length(dir);
    dir = (len > 1e-4) ? dir / len : float2(1, 0);
    let across = float2(-dir.y, dir.x);
    // One pixel past the width to feather into, half of one past each end.
    let halfWidth = d.viewport.z * 0.5 + 1.0;
    let atB = kMeshOverlayEdgeEnd[c.corner] > 0.5;
    let side = kMeshOverlayEdgeSide[c.corner];
    let px = (atB ? pb : pa) + across * side * halfWidth + dir * (atB ? 0.5 : -0.5);

    MeshOverlayVSOut o;
    o.position = MeshOverlayPixelToClip(d, px, atB ? b : a);
    o.color = MeshOverlayOut(d, MeshOverlayStateColor(d, d.edgeColor, c.state));
    o.rim = float4(0, 0, 0, 0);
    o.shape = float4(0, side * halfWidth, d.viewport.z * 0.5 + 0.5, 1);
    return o;
}

MeshOverlayVSOut MeshOverlayDot(MeshOverlayData d, MeshOverlayCorner c, float4 clip)
{
    if (clip.w <= kMeshOverlayNearW)
        return MeshOverlayCulled();

    let radius = d.viewport.w * 0.5;
    let offset = kMeshOverlayQuad[c.corner] * (radius + 1.0);
    let fill = MeshOverlayStateColor(d, d.vertexColor, c.state);
    // A ring of the opposite brightness keeps a dot readable on any surface.
    let luma = dot(fill.rgb, float3(0.2126, 0.7152, 0.0722));
    let rim = (luma > 0.5) ? float4(0, 0, 0, fill.a) : float4(1, 1, 1, fill.a);

    MeshOverlayVSOut o;
    o.position = MeshOverlayPixelToClip(d, clip.xy / clip.w * d.viewport.xy + offset, clip);
    o.color = MeshOverlayOut(d, fill);
    o.rim = MeshOverlayOut(d, rim);
    o.shape = float4(offset, radius + 0.5, 2);
    return o;
}

public MeshOverlayVSOut MeshOverlayVertexStage<S : IMeshOverlaySkin>(MeshOverlayData d, uint vid)
{
    let c = MeshOverlayResolveCorner(d, vid);
    if ((c.state & kMeshElementHidden) != 0u)
        return MeshOverlayCulled();

    // The product's vertex code runs from this one site. fxc inlines every
    // call, and a skinning loop inlined once per element kind is more than it
    // will unroll.
    float4 clip[2];
    [unroll]
    for (int k = 0; k < 2; ++k)
    {
        uint vertexState;
        clip[k] = S.ClipPosition(MeshOverlayFetch(d, c.vertex[k], vertexState));
    }

    switch (d.ctl.x)
    {
    case kMeshOverlayFaces: return MeshOverlayFace(d, c, clip[0]);
    case kMeshOverlayEdges: return MeshOverlayEdge(d, c, clip[0], clip[1]);
    default: return MeshOverlayDot(d, c, clip[0]);
    }
}

[shader("fragment")]
float4 psMeshOverlay(MeshOverlayVSOut input) : SV_Target
{
    if (input.shape.w < 0.5)
        return input.color;

    if (input.shape.w < 1.5)
    {
        // Coverage across the line: full inside the width, a pixel of falloff.
        let cover = saturate(input.shape.z - abs(input.shape.y));
        if (cover <= 0.0)
            discard;
        return float4(input.color.rgb, input.color.a * cover);
    }

    let r = length(input.shape.xy);
    let cover = saturate(input.shape.z - r);
    if (cover <= 0.0)
        discard;
    // The outer pixel of the dot is its rim.
    let inner = saturate(input.shape.z - 1.0 - r);
    let c = lerp(input.rim, input.color, inner);
    return float4(c.rgb, c.a * cover);
}
