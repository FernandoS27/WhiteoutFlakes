// ============================================================================
// WhiteoutFlakes Renderer — `.m3` geometry through the mesh overlay.
//
// The vertex position is m3_standard's own: M3SkinBasis, then M3ClipPosition —
// the two its vertex entries position through (never M3Skin / M3Vertex
// themselves: their input and output structs break the WGSL). The overlay
// constant buffer takes b3, the debug view's slot, because the D3D12 root
// signature has four constant buffers per stage and the surface draw uses them
// all; an overlay draw never reads the debug view's.
// ============================================================================

import m3_standard;
import mesh_overlay;

[[vk::binding(3, 0)]] ConstantBuffer<MeshOverlayData> m3Overlay : register(b3);

struct M3OverlayRigid : IMeshOverlaySkin
{
    static float4 ClipPosition(MeshOverlayVertex v)
    {
        float4 worldPos, viewPos;
        return M3ClipPosition(v.position, worldPos, viewPos);
    }
};

struct M3OverlaySkinned : IMeshOverlaySkin
{
    static float4 ClipPosition(MeshOverlayVertex v)
    {
        float3 pos, nrm, tan;
        M3SkinBasis(v.position, float3(0, 0, 1), float3(1, 0, 0), v.weights, v.indices, pos, nrm,
                    tan);
        float4 worldPos, viewPos;
        return M3ClipPosition(pos, worldPos, viewPos);
    }
};

[shader("vertex")]
MeshOverlayVSOut vsM3Overlay(uint vid : SV_VertexID)
{
    return MeshOverlayVertexStage<M3OverlayRigid>(m3Overlay, vid);
}

[shader("vertex")]
MeshOverlayVSOut vsM3OverlaySkinned(uint vid : SV_VertexID)
{
    return MeshOverlayVertexStage<M3OverlaySkinned>(m3Overlay, vid);
}
