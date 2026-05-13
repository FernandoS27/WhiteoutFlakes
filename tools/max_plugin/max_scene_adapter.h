#pragma once
// ============================================================================
// WhiteoutDex Max Scene Adapter — Implements IModelSource for 3ds Max scenes
// Refactored from WhiteoutDexExtractor (extract.h)
// All Max SDK dependencies are isolated here.
// ============================================================================

#include <MeshNormalSpec.h>
#include <bitmap.h>
#include <bmmlib.h>
#include <icustattribcontainer.h>
#include <inode.h>
#include <iparamb2.h>
#include <iskin.h>
#include <max.h>
#include <maxversion.h>
#include <modstack.h>
#include <stdmat.h>
#include <triobj.h>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "io/file_content_provider.h"
#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

// ============================================================================
// Known ClassIDs for WhiteoutDex custom MaxScript plugins
// ============================================================================
#define WARCRAFT3_MAT_CLASS_ID Class_ID(0x4b8e20a3, 0x1f6c3d57)
#define WC3_BITMAP_CLASS_ID Class_ID(0x3a7c10f1, 0x5e2d4b08)
#define WC3PARTICLES2_CLASS_ID Class_ID(0xD9F33BC9, 0x7A0DA37A)
#define WC3RIBBON_CLASS_ID Class_ID(0x937AA064, 0x9EFFA3DA)
// Wc3VertexMod is a MaxScript scripted plugin — its declared classID is NOT
// what `Modifier::ClassID()` returns at the C++ layer (Max wraps scripted-
// plugin ids opaquely). Use FindModifierByClassName with the plugin name
// instead. This define is kept for symmetry with the other WC3*_CLASS_ID
// macros but no production code path should resolve a scripted plugin
// through it.
#define WC3VERTEXMOD_CLASS_ID Class_ID(0x7A1B2C07, 0x3D4E5F07)
#define WC3PARTICLES1_CLASS_ID Class_ID(0x12E4F5A6, 0x3B7C8D9E)
#define WC3ATTACHPOINT_CLASS_ID Class_ID(0x1136ac20, 0x6f9cfeb7)

// Cross-DLL interface IDs
#define WC3P2_TEXTURE_PATH_IID 0x7B3C8D10
#define WC3P2_TEXTURE_PREFIX_IID 0x7B3C8D11
#define WC3P1_MODEL_PATH_IID 0x7B3C8D01

// ============================================================================
// Collected data structures (Max-specific; not exposed to renderer)
// ============================================================================
namespace whiteout::flakes {

// Inside this namespace, `renderer` and `io` already resolve to
// `::whiteout::flakes::renderer` and `::whiteout::flakes::io` via parent
// lookup — used directly below.

struct BoneInfo {
    INode* node = nullptr;
    Matrix3 inverseBind;
    i32 index = 0;
};

struct GeosetInfo {
    i32 geosetId = 0;
    INode* node = nullptr;
    i32 materialId = -1;
    std::vector<i32> faceVertMap; // expanded vertex → original vertex
    i32 expandedVertCount = 0;
};

struct MaterialLayerInfo {
    i32 filterMode = 0;
    i32 textureId = -1;
    f32 alpha = 1.0f;
    i32 replaceableTexture = 0;
    i32 flags = 0;
    // Shader type: 0=SD, 1=HD, 2=SDOnHD, 24=Crystal (from Wc3Material shaderType dropdown)
    i32 shaderId = 0;
    // HD subtexture slot IDs (-1 = not present)
    i32 normalMapId = -1;
    i32 ormMapId = -1;
    i32 emissiveMapId = -1;
    i32 teamColorMapId = -1;
    // Reforged PBR knobs (from Wc3Material reforged rollout)
    f32 emissiveGain = 0.0f;
    f32 fresnelOpacity = 0.0f;
    f32 fresnelTeamColor = 0.0f;
    Vector3f fresnelColor = {0.0f, 0.0f, 0.0f};
};

struct MaterialInfo {
    i32 materialId = 0;
    Mtl* mtl = nullptr;
    std::vector<MaterialLayerInfo> layers; // one per layer (composite sub-materials)
    i32 priorityPlane = 0;
    i32 sortOrder = 0;
};

struct TextureEntry {
    i32 textureId = 0;
    i32 replaceableId = 0;
    std::wstring filePath;
};

struct ParticleEmitterInfo {
    i32 emitterId = 0;
    INode* node = nullptr;
    i32 textureId = -1;
    i32 replaceableId = 0;
};

struct PE1EmitterInfo {
    i32 emitterId = 0;
    INode* node = nullptr;
    std::string modelPath;
};

struct AttachmentInfo {
    i32 index = 0;
    INode* node = nullptr;
    i32 attachmentId = 0;
    std::string modelPath;
};

struct RibbonEmitterInfo {
    i32 emitterId = 0;
    INode* node = nullptr;
    i32 textureId = -1;
};

struct CollisionShapeInfo {
    i32 type = 0;
    INode* node = nullptr;
};

// ============================================================================
// MaxSceneAdapter — implements IModelSource
// ============================================================================

class MaxSceneAdapter : public renderer::model::IModelSource {
public:
    MaxSceneAdapter();
    ~MaxSceneAdapter() override;

    // Collect scene data (call once on main thread before Get*() calls)
    void CollectScene();

    // Re-read material properties and textures from the scene.
    // Returns true if anything changed and the renderer should be updated.
    struct MaterialRefreshResult {
        std::vector<renderer::model::MaterialData> materials;
        std::vector<renderer::model::TextureData> textures;
        bool changed = false;
    };
    MaterialRefreshResult RefreshMaterials();

    // IModelSource interface
    std::vector<renderer::model::MeshData> GetMeshes() override;
    std::vector<renderer::model::TextureData> GetTextures() override;
    std::vector<renderer::model::MaterialData> GetMaterials() override;
    renderer::model::SkeletonData GetSkeleton() override;
    std::vector<renderer::model::SkinWeightData> GetSkinWeights() override;
    std::vector<renderer::ParticleEmitterConfig> GetParticleConfigs() override;
    std::vector<renderer::effects::RibbonEmitterConfig> GetRibbonConfigs() override;
    std::vector<renderer::model::CollisionShapeData> GetCollisionShapes() override;
    std::vector<renderer::model::AttachmentConfig> GetAttachmentConfigs() override;
    std::vector<renderer::model::PE1EmitterConfig> GetPE1Configs() override;

    // ---- IAnimationSource ----
    renderer::model::FrameState Evaluate(i32 sequenceIdx, i32 timeMs, i32 globalTimeMs,
                                         const Matrix44f& worldTransform,
                                         const Vector3f& cameraPos) const override;
    std::vector<renderer::model::SequenceInfo> GetSequences() const override;

    // Camera presets from scene (Max cameras + "Active Viewport")
    std::vector<whiteout::flakes::renderer::model::CameraPreset> GetCameraPresets();

private:
    // Collection phases
    void CollectGeometry();
    void CollectMaterials();
    i32 LoadTexture(const std::wstring& filePath, i32 replaceableId);
    i32 LoadTextureFromContentProvider(const std::string& archivePath, i32 replaceableId);
    // SD TEAMCOLOR / TEAMGLOW slots are reserved through LoadTexture(L"", 1|2)
    // — pixel bake lives in ReplaceableTextureManager renderer-side.
    void CollectBones();
    void CollectAttachments();
    void CollectParticleEmitters();
    void CollectRibbonEmitters();
    void CollectCollisionShapes();

    // Helpers
    static Matrix44f PackMatrix(const Matrix3& tm);
    static bool PB2Float(Animatable* a, const wchar_t* name, TimeValue t, f32& out);
    static bool PB2Int(Animatable* a, const wchar_t* name, TimeValue t, i32& out);
    static bool PB2Bool(Animatable* a, const wchar_t* name, TimeValue t, BOOL& out);
    static bool PB2Color(Animatable* a, const wchar_t* name, TimeValue t, Color& out);
    static bool PB2Texmap(Animatable* a, const wchar_t* name, Texmap*& out);
    // PB2 reads with an in-place fallback default — return `def` when the param
    // is absent or the scan fails, otherwise the read value.
    static i32 PB2IntOr(Animatable* a, const wchar_t* name, TimeValue t, i32 def = 0);
    static f32 PB2FloatOr(Animatable* a, const wchar_t* name, TimeValue t, f32 def = 0.0f);
    static bool PB2BoolOr(Animatable* a, const wchar_t* name, TimeValue t, bool def = false);
    static Object* GetBaseObject(INode* node);
    static Modifier* FindSkinModifier(INode* node);
    static Modifier* FindModifierByClassID(INode* node, Class_ID cid);
    // Name-based modifier lookup. Required for scripted-plugin modifiers
    // (Wc3VertexMod etc.) — Max's MaxScript-defined plugins do NOT expose
    // their declared classID through Modifier::ClassID(), so the ClassID
    // path silently misses every scripted plugin. The exporter takes the
    // same approach in geoset_anim_extractor.cpp's findVertexMod().
    static Modifier* FindModifierByClassName(INode* node, const wchar_t* const* nameSubstrings);
    // Wc3Material reading helpers — shared between CollectMaterials, CollectScene,
    // ExtractWc3MaterialLayer, and RefreshMaterials.
    static i32 ReadWc3MaterialFlags(Mtl* mtl);
    std::wstring ResolveBitmapPath(Mtl* mtl, const wchar_t* paramName);
    MaterialLayerInfo ExtractWc3MaterialLayer(Mtl* mtl);
    // Texture registration: push (rgba, w, h) into the loaded-texture table and
    // return its new texId. Used by every loader path. `displayPath` is the
    // filePath stored on TextureEntry for diagnostics; when empty, the cache
    // key is reused (the common case — TeamColor-composited textures override
    // it so the entry shows the original texture path, not the "__TC__" key).
    i32 RegisterTexture(const std::wstring& key, i32 replaceableId, std::vector<u8>&& pixels,
                        i32 width, i32 height, const std::wstring& displayPath = L"",
                        std::string sharedKey = {});
    // HD-sentinel allocation removed — adapters set teamColorMapId to
    // kHdTeamColorActive directly when the HD layer flags its team-colour
    // slot as live-driven. The HD draw binds the per-actor HD swatch at t4.
    std::wstring GetMaxFilePath();

    // Loaded texture pixel data (kept for GetTextures()).
    // `sharedKey` carries the cross-model dedup key (normalised path) so
    // GetTextures can stamp it onto TextureData without an extra lookup.
    // Empty for procedural / sentinel textures and for cache-borrow
    // entries where rgba is empty (the renderer's shared cache already
    // owns the GPU resource — we just record the borrow).
    struct LoadedTexture {
        i32 textureId;
        i32 replaceableId;
        std::vector<u8> rgba;
        i32 width, height;
        std::string sharedKey;
    };
    std::vector<LoadedTexture> loadedTextures_;

    std::vector<BoneInfo> bones_;
    std::unordered_map<std::wstring, i32> boneNameToIdx_;
    std::vector<GeosetInfo> geosets_;
    std::vector<MaterialInfo> materials_;
    std::vector<TextureEntry> texEntries_;
    std::vector<ParticleEmitterInfo> particles_;
    std::vector<PE1EmitterInfo> pe1Emitters_;
    std::vector<AttachmentInfo> attachments_;
    std::vector<RibbonEmitterInfo> ribbons_;
    std::vector<CollisionShapeInfo> collisions_;

    std::unordered_map<std::wstring, i32> texPathToId_;
    std::unordered_map<Mtl*, i32> mtlToId_;
    i32 nextTexId_ = 0;
    i32 nextMatId_ = 0;

    io::FileContentProvider contentProvider_;

    // Material change detection: snapshot of per-material properties
    struct MaterialSnapshot {
        i32 filterMode = 0;
        i32 flags = 0;
        i32 priorityPlane = 0;
        i32 sortOrder = 0;
        i32 replaceableTexture = 0;
        i32 shaderId = 0;
        std::wstring texturePath;
        std::wstring normalTexPath;
        std::wstring ormTexPath;
        std::wstring emissiveTexPath;
        std::wstring teamColorTexPath;
    };
    std::unordered_map<i32, MaterialSnapshot> matSnapshots_; // materialId → snapshot

    // Capture the subset of Wc3Material properties used for change detection.
    MaterialSnapshot SnapshotMaterial(Mtl* mtl);
    // Rebuild matSnapshots_ from the current materials_ list.
    void UpdateMaterialSnapshots();
};

} // namespace whiteout::flakes
