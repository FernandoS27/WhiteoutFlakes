// ============================================================================
// WhiteoutFlakes Renderer — Diablo III geometry through the mesh overlay.
//
// The position is d3_standard's own, D3SkinBasis then D3ClipPosition, so the
// pass's depth bias rides along exactly as the surface draw applied it. The
// overlay constant buffer takes b3, the debug view's slot — see m3_overlay.slang.
// ============================================================================

import d3_standard;
import mesh_overlay;

[[vk::binding(3, 0)]] ConstantBuffer<MeshOverlayData> d3Overlay : register(b3);

struct D3OverlayRigid : IMeshOverlaySkin
{
    static float4 ClipPosition(MeshOverlayVertex v)
    {
        float4 worldPos, viewPos;
        return D3ClipPosition(v.position, worldPos, viewPos);
    }
};

struct D3OverlaySkinned : IMeshOverlaySkin
{
    static float4 ClipPosition(MeshOverlayVertex v)
    {
        float3 pos, nrm, tan;
        D3SkinBasis(v.position, float3(0, 0, 1), float3(1, 0, 0), v.weights, v.indices, pos, nrm,
                    tan);
        float4 worldPos, viewPos;
        return D3ClipPosition(pos, worldPos, viewPos);
    }
};

[shader("vertex")]
MeshOverlayVSOut vsD3Overlay(uint vid : SV_VertexID)
{
    return MeshOverlayVertexStage<D3OverlayRigid>(d3Overlay, vid);
}

[shader("vertex")]
MeshOverlayVSOut vsD3OverlaySkinned(uint vid : SV_VertexID)
{
    return MeshOverlayVertexStage<D3OverlaySkinned>(d3Overlay, vid);
}
