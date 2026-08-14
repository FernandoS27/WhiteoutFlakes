#pragma once

#include "../gfx/gfx.h"
#include "animation/animation.h"
#include "assets/texture_asset_manager.h"
#include "effects/ribbon.h"
#include "particle.h"
#include "core/surface_table.h"
#include "core/surface_vocabulary.h"
#include "core/vertex_layout.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <memory>
#include <map>
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

    /// @brief Already-interleaved vertex data, uploaded verbatim. When this
    ///        is valid `vertices` is empty and the two fields below carry
    ///        what the upload would otherwise have read out of it.
    MeshBuffer baked;
    /// @brief Vertex count and local bounds centre for the baked path,
    ///        computed at stage time from the source positions — the
    ///        renderer never decodes `baked.data` to recover them.
    i32 bakedVertexCount = 0;
    Vector3f centroid = {0, 0, 0};
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

    // Byte stride of the Base stream, and which interned layout describes
    // it. WC3 leaves these at `sizeof(Vertex)` / kWc3Interleaved; a geoset
    // uploaded from a MeshBuffer carries that buffer's own stride and its
    // interned id. Only shading models that draw baked geometry read them
    // — every WC3 bind site passes sizeof(Vertex) as a literal, as before.
    u32 baseStride = sizeof(Vertex);
    u32 layoutId = core::VertexLayoutCache::kWc3Interleaved;

    // The four buffers named by core::StreamId. Base and BaseUv1 are two
    // complete interleaved copies differing only in which UV set is baked into
    // `uv`; the draw path picks between them per layer by coordId.
    gfx::BufferHandle Stream(core::StreamId s) const {
        switch (s) {
        case core::StreamId::Base:
            return unskinnedVb;
        case core::StreamId::BaseUv1:
            return unskinnedVb1;
        case core::StreamId::Tangent:
            return tangentVb;
        case core::StreamId::Bone:
            return boneVb;
        default:
            // Uv / Colors are standalone streams no WC3 geoset uploads.
            return gfx::BufferHandle::Invalid;
        }
    }

    f32 geosetAlpha = 1.0f;
    Vector3f geosetColor = {1, 1, 1};
    Matrix44f worldMatrix = Matrix44f::identity();
    i32 priorityPlane = 0;

    // Local-space bounds center, computed once at upload. Transformed by the
    // actor world matrix at collection time to give a per-geoset sort position
    // for the back-to-front transparent pass (mirrors WC3's geoset centroid).
    Vector3f localCentroid = {0, 0, 0};

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

    // Ordered by geoset id: UploadStagedGeosets drains this into gpuGeosets,
    // and that vector's order is the `geoIdx` OpaqueOrder tie-breaks on for
    // every SpawnUnitFromSource actor (Max plugin, CornEffectSource, M2/M3).
    std::map<i32, StagedGeoset> stagedGeosets;
    std::unordered_map<i32, StagedMaterial> stagedMaterials;
    std::unordered_map<i32, StagedTexture> stagedTextures;
    bool stagedDirty = false;
    bool stagedClear = false;

    std::vector<GPUGeoset> gpuGeosets;
    std::unique_ptr<assets::TextureAssetManager::ModelScope> textures;
    // The product's own material data, behind the core interface. Built by
    // the loader; recovered by a shading model via SurfaceTableCast.
    std::unique_ptr<core::ISurfaceTable> surfaceTable;
    // One key per surface, parallel to the table. Populated at upload; what
    // SurfaceKey::surface indexes into.
    std::vector<core::SurfaceKey> surfaces;

    animation::SkinningSystem skinning;
    bool skinDirty = false;
    std::vector<u32> billboardFlags;
    std::vector<Vector3f> nodePivots;
    std::vector<i32> nodeParents;

    std::vector<PE2State> pe2State;
    effects::RibbonSystem ribbons;
    gfx::BufferHandle ribbonVB = gfx::BufferHandle::Invalid;
    i32 ribbonVBSize = 0;
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
};

} // namespace whiteout::flakes::renderer::model
