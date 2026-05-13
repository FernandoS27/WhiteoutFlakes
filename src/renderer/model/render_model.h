#pragma once

#include "../gfx/gfx.h"
#include "animation/animation.h"
#include "assets/texture_asset_manager.h"
#include "effects/pe1_system.h"
#include "effects/ribbon.h"
#include "particle.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::renderer::model {

struct StagedTexture {

    std::vector<u8> pixels;
    gfx::Format format = gfx::Format::R8G8B8A8_UNORM;
    i32 width = 0;
    i32 height = 0;
    i32 mipLevels = 1;
    i32 replaceableId = 0;
    u32 wrapFlags = 0x3;

    std::string sharedKey;
};

using StagedMaterialLayer = MaterialLayerData;

struct StagedMaterial {
    std::vector<StagedMaterialLayer> layers;
    i32 priorityPlane = 0;
    i32 sortOrder = 0;
};

struct StagedGeoset {
    std::vector<Vertex> vertices;
    std::vector<u32> indices;

    std::vector<Vector4f> tangents;
    i32 materialId = -1;
    u32 lod = 0;
};

struct GPUGeoset {
    i32 geosetId = -1;
    gfx::BufferHandle ib = gfx::BufferHandle::Invalid;

    gfx::BufferHandle unskinnedVb = gfx::BufferHandle::Invalid;

    gfx::BufferHandle unskinnedVb1 = gfx::BufferHandle::Invalid;

    gfx::BufferHandle tangentVb = gfx::BufferHandle::Invalid;

    gfx::BufferHandle boneVb = gfx::BufferHandle::Invalid;

    gfx::BufferHandle bonePaletteCb = gfx::BufferHandle::Invalid;
    i32 indexCount = 0;
    i32 vertexCount = 0;
    i32 materialId = -1;
    u32 lod = 0;

    bool hasSkinning = false;

    f32 geosetAlpha = 1.0f;
    Vector3f geosetColor = {1, 1, 1};
    Matrix44f worldMatrix = Matrix44f::identity();
    i32 priorityPlane = 0;

    void Release(gfx::IGFXDevice& gfx, bool freeSharedBuffers = true) {
        if (freeSharedBuffers) {
            gfx.Destroy(ib);
            gfx.Destroy(unskinnedVb);
            gfx.Destroy(unskinnedVb1);
            gfx.Destroy(tangentVb);
            gfx.Destroy(boneVb);
        }
        gfx.Destroy(bonePaletteCb);
        ib = gfx::BufferHandle::Invalid;
        unskinnedVb = gfx::BufferHandle::Invalid;
        unskinnedVb1 = gfx::BufferHandle::Invalid;
        tangentVb = gfx::BufferHandle::Invalid;
        boneVb = gfx::BufferHandle::Invalid;
        bonePaletteCb = gfx::BufferHandle::Invalid;
        indexCount = 0;
        vertexCount = 0;
    }
};

struct GPUMaterial {
    StagedMaterial cpu;
};

struct CollisionShape {
    i32 type = 0;
    Vector3f vmin = {0, 0, 0};
    Vector3f vmax = {0, 0, 0};
    f32 radius = 0;
    Vector3f pivot = {0, 0, 0};
    Matrix44f transform = Matrix44f::identity();
};

struct TexAnimData {
    f32 uOff = 0, vOff = 0, uTile = 1, vTile = 1, rotation = 0;
};

struct PE2State {
    f32 lastEmissionRate = 0.0f;
    bool emissionValid = false;
};

struct RenderModel {

    std::unordered_map<i32, StagedGeoset> stagedGeosets;
    std::unordered_map<i32, StagedMaterial> stagedMaterials;
    std::unordered_map<i32, StagedTexture> stagedTextures;
    bool stagedDirty = false;
    bool stagedClear = false;

    std::vector<GPUGeoset> gpuGeosets;
    std::unique_ptr<assets::TextureAssetManager::ModelScope> textures;
    std::vector<GPUMaterial> gpuMaterials;

    animation::SkinningSystem skinning;
    bool skinDirty = false;
    std::vector<u32> billboardFlags;
    std::vector<Vector3f> nodePivots;
    std::vector<i32> nodeParents;

    std::vector<PE2State> pe2State;
    effects::RibbonSystem ribbons;
    gfx::BufferHandle ribbonVB = gfx::BufferHandle::Invalid;
    i32 ribbonVBSize = 0;
    effects::PE1System pe1;
    std::vector<CollisionShape> collisionShapes;

    std::unordered_map<i32, TexAnimData> matTexAnim;

    struct TexAnimPaletteEntry {
        f32 row0[4];
        f32 row1[4];
    };
    std::vector<TexAnimPaletteEntry> texAnimPalette;

    std::vector<FrameState::LightState> activeLights;

    bool hasLods = false;

    void ApplyGeosetStates(const FrameState& state);
    void ApplyLayerStates(const FrameState& state);
    void ApplyRibbonFrameStates(const FrameState& state);
    void ApplyPE1FrameStates(const FrameState& state);
};

} // namespace whiteout::flakes::renderer::model
