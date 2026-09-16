// ============================================================================
// WhiteoutFlakes Renderer — UnlitShading geometry through the mesh overlay.
//
// unlit.slang's entries have no position helper to call, so the three-matrix
// chain they share is written out once here; skinning is its SkinMatrix. The
// overlay constant buffer takes b3, which no unlit program declares.
// ============================================================================

import unlit;
import mesh_overlay;

[[vk::binding(3, 0)]] ConstantBuffer<MeshOverlayData> unlitOverlay : register(b3);

float4 UnlitOverlayClip(float4 modelPos)
{
    let worldPos = mul(modelPos, cb.world);
    let viewPos = mul(worldPos, cb.view);
    return mul(viewPos, cb.projection);
}

struct UnlitOverlayRigid : IMeshOverlaySkin
{
    static float4 ClipPosition(MeshOverlayVertex v)
    {
        return UnlitOverlayClip(float4(v.position, 1.0));
    }
};

struct UnlitOverlaySkinned : IMeshOverlaySkin
{
    static float4 ClipPosition(MeshOverlayVertex v)
    {
        let skin = SkinMatrix(v.weights, v.indices);
        return UnlitOverlayClip(mul(float4(v.position, 1.0), skin));
    }
};

[shader("vertex")]
MeshOverlayVSOut vsUnlitOverlay(uint vid : SV_VertexID)
{
    return MeshOverlayVertexStage<UnlitOverlayRigid>(unlitOverlay, vid);
}

[shader("vertex")]
MeshOverlayVSOut vsUnlitOverlaySkinned(uint vid : SV_VertexID)
{
    return MeshOverlayVertexStage<UnlitOverlaySkinned>(unlitOverlay, vid);
}
