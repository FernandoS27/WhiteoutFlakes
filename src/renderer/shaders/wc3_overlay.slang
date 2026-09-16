// ============================================================================
// WhiteoutFlakes Renderer — Warcraft III geometry through the mesh overlay.
//
// Positions come from the retail vertex programs' own steps, imported from
// Wc3Shaders: the HD entry's skinning policy and clip transform (hd_vs.slang
// steps 1-2), and the SD highspec entry's (sd_highspec_vs.slang), with the
// policies compile_all_slang.py specialises them with. Nothing is restated, so
// an edge lies where the shipped program put the triangle.
//
// Each family takes a constant-buffer slot its own vertex programs never read:
// b0 for HD (sdHighspecVsCB0's slot), b1 for SD (the cascade bank's). Compiled
// with -fvk-use-dx-layout, like wc3_debug.slang.
// ============================================================================

import wc3_shaders;
import mesh_overlay;

[[vk::binding(0, 0)]] ConstantBuffer<MeshOverlayData> wc3HdOverlay : register(b0);
[[vk::binding(1, 0)]] ConstantBuffer<MeshOverlayData> wc3SdOverlay : register(b1);

float4 Wc3HdOverlayClip<TSkin : IHDSkinning>(MeshOverlayVertex v)
{
    float3 pos, nrm, tan;
    TSkin.transform(v.position, float3(0, 0, 1), float3(1, 0, 0), v.weights, v.indices, pos, nrm,
                    tan);
    return transformClipPos(pos, hdVsCB2.worldViewProj);
}

struct Wc3HdOverlayRigid : IMeshOverlaySkin
{
    static float4 ClipPosition(MeshOverlayVertex v)
    {
        return Wc3HdOverlayClip<HDRigid>(v);
    }
};

struct Wc3HdOverlayPalette : IMeshOverlaySkin
{
    static float4 ClipPosition(MeshOverlayVertex v)
    {
        return Wc3HdOverlayClip<HDFourBoneSkinning<ConstantBonePalette>>(v);
    }
};

struct Wc3HdOverlayBoneBuffer : IMeshOverlaySkin
{
    static float4 ClipPosition(MeshOverlayVertex v)
    {
        return Wc3HdOverlayClip<HDFourBoneSkinning<StructuredBonePalette>>(v);
    }
};

float4 Wc3SdOverlayClip<TSkin : ISkinning>(MeshOverlayVertex v)
{
    float3 pos, nrm, tan;
    TSkin.transform<ClassicBonePalette>(v.position, float3(0, 0, 1), float4(0, 0, 0, 1), v.weights,
                                        v.indices, pos, nrm, tan);
    return transformClipPos(pos, sdHighspecVsCB0.worldViewProj);
}

struct Wc3SdOverlayRigid : IMeshOverlaySkin
{
    static float4 ClipPosition(MeshOverlayVertex v)
    {
        return Wc3SdOverlayClip<Rigid>(v);
    }
};

struct Wc3SdOverlaySkinned : IMeshOverlaySkin
{
    static float4 ClipPosition(MeshOverlayVertex v)
    {
        return Wc3SdOverlayClip<SDFourBoneSkinning>(v);
    }
};

[shader("vertex")]
MeshOverlayVSOut vsWc3HdOverlay(uint vid : SV_VertexID)
{
    return MeshOverlayVertexStage<Wc3HdOverlayRigid>(wc3HdOverlay, vid);
}

[shader("vertex")]
MeshOverlayVSOut vsWc3HdOverlayPalette(uint vid : SV_VertexID)
{
    return MeshOverlayVertexStage<Wc3HdOverlayPalette>(wc3HdOverlay, vid);
}

[shader("vertex")]
MeshOverlayVSOut vsWc3HdOverlayBoneBuffer(uint vid : SV_VertexID)
{
    return MeshOverlayVertexStage<Wc3HdOverlayBoneBuffer>(wc3HdOverlay, vid);
}

[shader("vertex")]
MeshOverlayVSOut vsWc3SdOverlay(uint vid : SV_VertexID)
{
    return MeshOverlayVertexStage<Wc3SdOverlayRigid>(wc3SdOverlay, vid);
}

[shader("vertex")]
MeshOverlayVSOut vsWc3SdOverlaySkinned(uint vid : SV_VertexID)
{
    return MeshOverlayVertexStage<Wc3SdOverlaySkinned>(wc3SdOverlay, vid);
}
