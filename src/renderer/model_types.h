#pragma once

#include "common_types.h"
#include "types.h"
#include "../gfx/gfx_types.h"
#include <vector>
#include <string>
#include <functional>

namespace WhiteoutDex {

struct CameraPreset {
    std::wstring name;
    bool isLive = false;

    Vector3f position{0.f, 0.f, 0.f};
    Vector3f target  {0.f, 0.f, 0.f};
    f32      fovDiagonal = 0.95f;
    f32      zNear       = 1.0f;
    f32      zFar        = 10000.0f;
    f32      staticRoll  = 0.0f;

    f32 pitch    = 0.0f;
    f32 yaw      = 0.0f;
    f32 distance = 100.0f;

    std::function<void(Vector3f& pos, Vector3f& target,
                       f32& roll, i32 timeMs,
                       i32 seqStart, i32 seqEnd)> animator;
};

struct AttachmentConfig {
    i32 attachmentId = 0;
    std::string modelPath;
};

struct PE1EmitterConfig {
    std::string modelPath;
    f32 lifespan = 1.0f;
    f32 scale    = 1.0f;
};

struct EventObjectConfig {
    enum class Kind : u8 { SPN, SPL, UBR, FPT, SND, Unknown };

    std::string  name;
    Kind         kind             = Kind::Unknown;
    std::string  id;
    i32          nodeIndex        = -1;

    Vector3f     pivot            = {0, 0, 0};
    u32          globalSequenceId = 0xFFFFFFFFu;
    std::vector<u32> eventTrackTimes;
};

enum FilterMode {
    FILTER_NONE        = 0,
    FILTER_TRANSPARENT = 1,
    FILTER_BLEND       = 2,
    FILTER_ADDITIVE    = 3,
    FILTER_ADD_ALPHA   = 4,
    FILTER_MODULATE    = 5,
    FILTER_MODULATE_2X = 6,
};

inline i32 MapFilterMode(i32 raw) {
    if (raw < 0) return FILTER_NONE;
    if (raw > 6) return FILTER_MODULATE_2X;
    return raw;
}

inline i32 MapPE2BlendMode(i32 blendMode) {
    static constexpr i32 table[] = {
        FILTER_BLEND, FILTER_ADDITIVE, FILTER_MODULATE, FILTER_MODULATE_2X, FILTER_TRANSPARENT
    };
    if (blendMode >= 0 && blendMode < 5) return table[blendMode];
    return FILTER_BLEND;
}

enum MaterialFlags {
    MAT_TWO_SIDED    = 1,
    MAT_UNSHADED     = 2,
    MAT_UNFOGGED     = 4,
    MAT_NO_DEPTH_TEST = 8,
    MAT_NO_DEPTH_SET  = 16,
    MAT_CONSTANT_COLOR = 32,
};

struct MeshData {
    i32 geosetId;
    i32 materialId;

    u32 lod = 0;
    std::vector<Vector3f> positions;
    std::vector<Vector3f> normals;
    std::vector<Vector2f> uvs;

    std::vector<Vector2f> uvs1;

    std::vector<Vector4f> tangents;
    std::vector<u32>      indices;
};

inline constexpr i32 kHdTeamColorActive = -2;

struct TextureData {
    i32 textureId;
    i32 replaceableId;

    std::vector<u8> pixels;
    gfx::Format format = gfx::Format::R8G8B8A8_UNORM;
    i32 width, height;
    i32 mipLevels = 1;
    u32 wrapFlags = 0x3;

    std::string sharedKey;
};

struct MaterialLayerData {
    i32 filterMode;
    i32 textureId;
    f32 alpha;
    i32 flags;
    i32 textureAnimationId = -1;

    i32 coordId = 0;

    i32 shaderId = 0;

    i32 normalMapId    = -1;
    i32 ormMapId       = -1;
    i32 emissiveMapId  = -1;

    i32 teamColorMapId = -1;

    f32      emissiveGain    = 0.0f;
    f32      fresnelOpacity  = 0.0f;
    f32      fresnelTeamColor = 0.0f;
    Vector3f fresnelColor    = {0.0f, 0.0f, 0.0f};
};

struct MaterialData {
    i32 materialId;
    std::vector<MaterialLayerData> layers;
    i32 priorityPlane;
    i32 sortOrder;
};

enum BoneBillboardFlag : u32 {
    BONE_BILLBOARD_NONE            = 0,
    BONE_BILLBOARD_FULL            = 1,
    BONE_BILLBOARD_LOCK_X          = 2,
    BONE_BILLBOARD_LOCK_Y          = 4,
    BONE_BILLBOARD_LOCK_Z          = 8,
    BONE_BILLBOARD_CAMERA_ANCHORED = 16,
};

struct SkeletonData {
    i32 nodeCount;
    std::vector<Matrix44f> inverseBindMatrices;
    std::vector<u32>      billboardFlags;
    std::vector<Vector3f> nodePivots;
    std::vector<i32>      nodeParents;
};

struct VertexInfluence {
    i32   boneIdx[4] = {0, 0, 0, 0};
    f32   weight[4]  = {0, 0, 0, 0};
};

struct GroupAverageRecord {
    i32              pseudoSlot;
    std::vector<i32> nodeIndices;
};

struct SkinWeightData {
    i32 geosetId;
    std::vector<VertexInfluence> influences;
    std::vector<GroupAverageRecord> groupAverages;
    std::vector<i32> subsetNodeIndices;
};

struct CollisionShapeData {
    i32 type;
    Vector3f vertices[2];
    f32 radius;
    Vector3f pivot = {0, 0, 0};
};

struct FrameState {
    std::vector<Matrix44f>  boneWorldMatrices;
    std::vector<Matrix44f>  geosetTransforms;
    std::vector<f32>       geosetAlphas;
    std::vector<Vector3f>  geosetColors;

    struct ParticleFrameState {
        i32 emitterId;
        Matrix44f transform;
        f32 emissionRate, speed, variation, coneAngle;
        f32 gravity, width, length, visibility;
        bool squirting;
    };
    std::vector<ParticleFrameState> particleStates;

    struct RibbonFrameState {
        i32 emitterId;
        Matrix44f transform;
        f32 above, below, alpha;
        Vector3f color;
        f32 visibility;
        i32   slot;
    };
    std::vector<RibbonFrameState> ribbonStates;

    std::vector<Matrix44f>  collisionTransforms;

    struct TexAnimState {
        i32 materialId;
        i32 layerIndex;
        f32 uOff, vOff, uTile, vTile;
        f32 rotation;
    };
    std::vector<TexAnimState> texAnims;

    enum class LightKind : u8 { Directional = 0, Omni = 1, Ambient = 2 };
    struct LightState {
        LightKind kind       = LightKind::Directional;
        Vector3f  worldPos   = {0, 0, 0};
        Vector3f  worldDir   = {0, 0, -1};
        Vector3f  diffuse    = {0, 0, 0};
        Vector3f  ambient    = {0, 0, 0};
        f32       attenStart = 0.0f;
        f32       attenEnd   = 0.0f;
        bool      enabled    = true;
    };
    std::vector<LightState> lights;

    struct TexAnimMatrix {
        i32   textureAnimId;

        f32 row0[4];
        f32 row1[4];
    };
    std::vector<TexAnimMatrix> texAnimMatrices;

    struct LayerAlphaState {
        i32 materialId;
        i32 layerIndex;
        f32 alpha;
    };
    std::vector<LayerAlphaState> layerAlphas;

    enum class LayerTexSlot : u8 {
        Diffuse   = 0,
        Normal    = 1,
        ORM       = 2,
        Emissive  = 3,
        TeamColor = 4,
    };
    struct LayerTextureIdState {
        i32          materialId;
        i32          layerIndex;
        LayerTexSlot slot       = LayerTexSlot::Diffuse;
        i32          textureId;
    };
    std::vector<LayerTextureIdState> layerTextureIds;

    struct LayerFresnelState {
        i32      materialId;
        i32      layerIndex;
        Vector3f fresnelColor;
        f32      fresnelOpacity;
        f32      fresnelTeamColor;
        f32      emissiveGain;
    };
    std::vector<LayerFresnelState> layerFresnels;

    struct AttachmentFrameState {
        i32 attachmentIndex;
        Matrix44f transform;
        f32 visibility;
    };
    std::vector<AttachmentFrameState> attachmentStates;

    struct PE1FrameState {
        i32 emitterId;
        Matrix44f transform;
        f32 emissionRate, speed, latitude, longitude;
        f32 gravity, visibility;
    };
    std::vector<PE1FrameState> pe1States;
};

}
