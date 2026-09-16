// ============================================================================
// WhiteoutFlakes Renderer — `.m2` geometry through the mesh overlay.
//
// The position is M2Vertex's — M2SkinBasis then M2ClipPosition — which every one
// of the client's sixteen vertex shaders shares. The overlay constant buffer
// takes b3, the debug view's slot — see m3_overlay.slang.
// ============================================================================

import m2_combiners;
import mesh_overlay;

[[vk::binding(3, 0)]] ConstantBuffer<MeshOverlayData> m2Overlay : register(b3);

struct M2OverlaySkinned : IMeshOverlaySkin
{
    static float4 ClipPosition(MeshOverlayVertex v)
    {
        float3 pos, nrm;
        M2SkinBasis(v.position, float3(0, 0, 1), v.weights, v.indices, pos, nrm);
        float4 worldPos, viewPos;
        return M2ClipPosition(pos, worldPos, viewPos);
    }
};

[shader("vertex")]
MeshOverlayVSOut vsM2Overlay(uint vid : SV_VertexID)
{
    return MeshOverlayVertexStage<M2OverlaySkinned>(m2Overlay, vid);
}
