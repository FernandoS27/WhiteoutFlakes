#pragma once

#include "common_types.h"
#include "types.h"
#include "gfx/gfx.h"
#include "../io/replaceable_paths.h"

#include "camera.h"
#include "animation.h"
#include "particle.h"
#include "particle/particle_service.h"
#include "particle/splat_service.h"
#include "dnc/dnc_service.h"
#include "shadow/shadow_service.h"
#include "ribbon.h"
#include "sound_emitter.h"
#include "spn_spawner.h"
#include "model_types.h"
#include "model_instance.h"
#include "actor_manager.h"
#include "scene_manager.h"
#include "model_source.h"
#include "content_provider.h"
#include "file_content_provider.h"
#include "render_target.h"

namespace WhiteoutDex::bls {
    class BlsShaderCache;
    class BlsProgramCatalog;
    class BlsPsoBuilder;
    struct BlsProgram;
    struct BlsShader;
}

namespace WhiteoutDex {
    class SamplerAssetManager;
    class TextureAssetManager;
    class ReplaceableTextureManager;
    class ModelTemplateManager;
    class SceneManager;

    struct ModelTemplate;
    namespace shadow { class ShadowPass; }
}
#include <unordered_map>
#include <unordered_set>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <deque>

namespace WhiteoutDex {

struct LineVertex {
    Vector3f position;
    Vector4f color;
};

template <class> class BlsGeosetPass;
class GeosetPassBls;
class GeosetPassHd;
enum class GeosetBucket : u8;
class DebugRenderer;
class SpnSpawner;

class RenderService {
    template <class> friend class BlsGeosetPass;
    friend class GeosetPassBls;
    friend class GeosetPassHd;
    friend class DebugRenderer;
    friend class SpnSpawner;
    friend class shadow::ShadowPass;
public:

    RenderService();

    explicit RenderService(SceneManager& scene);
    ~RenderService();

    bool           InitDevice(gfx::GfxApi api = gfx::GfxApi::D3D12);
    RenderTargetId CreateSwapChainTarget(void* nativeWindowHandle, i32 width, i32 height);
    RenderTargetId CreateOffscreenTarget(i32 width, i32 height);
    void           DestroyRenderTarget(RenderTargetId id);
    void           ResizeRenderTarget(RenderTargetId id, i32 width, i32 height);
    void           RenderFrame(RenderTargetId targetId);
    void           Present(RenderTargetId targetId);
    bool           IsDeviceReady() const { return gfx_ != nullptr; }

    void UpdateMaterials(const std::vector<MaterialData>& materials,
                         const std::vector<TextureData>& textures);

    void ClearModel();

    void SetTeamColor(u8 r, u8 g, u8 b);

    void        SetTileset(io::Tileset ts);
    io::Tileset GetTileset() const;

    void ActivateCameraPreset(i32 idx);
    i32  GetActiveSequenceIndex() const;
    void SetActiveSequence(i32 index);
    std::optional<std::vector<CameraPreset>> TakePendingCameraPresets();
    std::optional<std::vector<std::string>>  TakePendingSequences();

    void ClearSplats();

    void RotateCamera(i32 dx, i32 dy);
    void PanCamera(i32 dx, i32 dy);
    void ZoomCamera(i32 delta);
    void ZoomCameraSmooth(i32 dy);
    void ResetCamera();
    void SnapCameraToFace(i32 faceIndex);

    void SetDisplayFlags(const DisplayFlags& flags);
    DisplayFlags GetDisplayFlags() const;
    bool ConsumeRenderModeDirty() { return renderModeDirty_.exchange(false); }

    void SetHdDebugMode(i32 mode) { hdDebugMode_.store(mode); }
    i32  GetHdDebugMode() const   { return hdDebugMode_.load(); }

    void SetLodOverride(i32 lod) { lodOverride_.store(lod); }
    i32  GetLodOverride() const  { return lodOverride_.load(); }

    void         SetLightingMode(LightingMode m) { lightingMode_.store(static_cast<u8>(m)); }
    LightingMode GetLightingMode() const         { return static_cast<LightingMode>(lightingMode_.load()); }

    void SetBackgroundColor(u8 r, u8 g, u8 b);
    u32  GetBackgroundColorRaw() const { return backgroundColor_.load(); }

    void SetEnvProbe(const std::string& relPath);

    void SetDayNightProbes(const std::string& dayPath,
                           const std::string& nightPath);

    void    SetIblMode(IblMode mode);
    IblMode GetIblMode() const { return iblMode_; }

private:

    void ApplyIblMode(IblMode mode);

public:

    gfx::Format         SceneTargetFormat() const {
        return renderMode_ == RenderMode::HD ? kHdrSceneFormat : kSdSceneFormat;
    }

    gfx::PipelineHandle CurrentLinePSO() const;

    void  SetTonemapExposure(f32 exposure) { tonemapExposure_ = exposure; }
    f32   GetTonemapExposure() const       { return tonemapExposure_; }

    void SetIgnoreNonLooping(bool on);
    bool GetIgnoreNonLooping() const { return ignoreNonLooping_; }

    dnc::DncService*       GetDncService()       { return dncService_.get(); }
    const dnc::DncService* GetDncService() const { return dncService_.get(); }

    gfx::IGFXDevice*       GetGfxDevice()       { return gfx_.get(); }
    const gfx::IGFXDevice* GetGfxDevice() const { return gfx_.get(); }

    shadow::ShadowService*       GetShadowService()       { return shadowService_.get(); }
    const shadow::ShadowService* GetShadowService() const { return shadowService_.get(); }

    void SetSoundEmitter(std::unique_ptr<ISoundEmitter> emitter);

    void  SetSoundVolume(f32 v);
    f32   GetSoundVolume() const { return soundVolume_; }

    void ResizePrimaryTarget(i32 width, i32 height);

    void GetFrameStats(i32& geosets, i32& textures, i32& nodes,
                       i32& particles, i32& segments) const;

    void Tick(f32 dt);

    void ShutdownDevice();

    void SetPrimaryTarget(RenderTargetId id) { primaryTargetId_ = id; }

    SceneManager&              Scene()       { return *scene_; }
    const SceneManager&        Scene() const { return *scene_; }
    SamplerAssetManager&       Samplers()    { return *samplers_; }
    TextureAssetManager&       Textures()    { return *textures_; }
    ReplaceableTextureManager& Replaceables(){ return *replaceables_; }
    DebugRenderer&             Debug()       { return *debug_; }
    gfx::IGFXDevice&           Gfx()         { return *gfx_; }

    Actor* SpawnActorFromMdx(const std::string& mdxPath);
    Actor* LoadActorFromMdx(const std::string& mdxPath);
    Actor* SpawnActorFromLiveSource(std::shared_ptr<IModelSource> source);

    void EvaluateAndApply(Actor& actor);

    static bool GeosetPassesLod(u32 geosetLod, i32 selectedLod) {
        return geosetLod == 0xFFFFFFFFu || (i32)geosetLod == selectedLod;
    }

    static i32 GetRenderOrder(i32 filterMode) {
        switch (filterMode) {
            case FILTER_NONE:        return 1;
            case FILTER_TRANSPARENT: return 2;
            case FILTER_BLEND:       return 3;
            default:                 return 4;
        }
    }

private:

    u32 AddModel(const std::vector<MeshData>& meshes,
                 const std::vector<TextureData>& textures,
                 const std::vector<MaterialData>& materials,
                 const SkeletonData& skeleton,
                 const std::vector<SkinWeightData>& skinWeights,
                 const std::vector<ParticleEmitterConfig>& particleConfigs,
                 const std::vector<RibbonEmitterConfig>& ribbonConfigs,
                 const std::vector<CollisionShapeData>& collisions);

    void SetAttachmentConfigs(u32 handle,
                              const std::vector<AttachmentConfig>& configs);
    void SetPE1Configs(u32 handle,
                       const std::vector<PE1EmitterConfig>& configs);

    u32 AddModelByPath(const std::string& mdxPath);
    u32 LoadModelByPath(const std::string& mdxPath);

    void ApplyFrameState(u32 handle, const FrameState& state, i32 timeMs);

    void UpdateMaterials(u32 handle, const std::vector<MaterialData>& materials,
                         const std::vector<TextureData>& textures);

    bool IsTextureCached(std::string_view key) const;

    void CleanupD3D();
    bool CreateShaders();
    bool CreatePipelines();
    bool CreateDefaultResources();

    void ProcessStagedData();
    void UploadStagedTextures(Actor& mi);
    void UploadStagedGeosets(Actor& mi);
    void CreateNodePalette(Actor& mi);
    void ReleaseModelGPU();

    void UpdateAnimation();

    void UpdateParticles(f32 dt);

    void UpdateAttachments();

    void UpdatePE1(f32 dt);
    void EvaluatePE1Children();

    void EvaluateTopLevelActors();

    void UpdateRibbons(f32 dt);
    void RenderRibbons();

    i32  ComputeSelectedLod() const;

    void ApplyBoneMatrices(Actor& mi, const FrameState& state);
    void ApplyParticleFrameStates(Actor& mi, const FrameState& state);
    void ApplyAttachmentStates(Actor& mi, const FrameState& state, i32 timeMs);

    void RenderGeosets(GeosetBucket bucket);

    particle::ParticleService particleService_;

    std::unique_ptr<dnc::DncService>  dncService_;

    std::unique_ptr<shadow::ShadowService> shadowService_;

    particle::SplatService    splatService_;
    std::unique_ptr<SpnSpawner>     spnSpawner_;
    std::unique_ptr<ISoundEmitter>  soundEmitter_;

    f32                             soundVolume_ = 0.2f;

    mutable std::mutex    dataMutex_;

    i32                   width_ = 800;
    i32                   height_ = 600;

    bool                  showGrid_       = true;
    bool                  showParticles_  = true;
    bool                  showRibbons_    = true;
    bool                  showCollisions_ = false;
    bool                  showLights_     = false;
    bool                  showEvents_     = true;

    RenderMode            renderMode_     = RenderMode::SD;

    std::atomic<bool>     renderModeDirty_{false};

    std::atomic<i32>      hdDebugMode_{0};

    std::atomic<i32>      lodOverride_{0};

    std::atomic<u8>       lightingMode_{static_cast<u8>(LightingMode::InGame)};

    std::atomic<u32>      backgroundColor_{0x00453A35u};

    std::unique_ptr<SceneManager> ownedScene_;
    SceneManager*                 scene_ = nullptr;

    Actor* focusModel() const;
    Actor* getModel(u32 h) const;

    void stageModelFromTemplate(Actor* mi,
                                std::shared_ptr<ModelTemplate> tmpl);

    void uploadTemplateGpu(ModelTemplate& tmpl);

    std::unique_ptr<gfx::IGFXDevice> gfx_;

    std::unordered_map<RenderTargetId, RenderTarget> targets_;
    RenderTargetId nextTargetId_    = 1;
    RenderTargetId primaryTargetId_ = 0;

    RenderTarget* primaryTarget() {
        auto it = targets_.find(primaryTargetId_);
        return (it != targets_.end()) ? &it->second : nullptr;
    }

    gfx::ShaderHandle lineVS_     = gfx::ShaderHandle::Invalid;
    gfx::ShaderHandle linePS_     = gfx::ShaderHandle::Invalid;

    gfx::PipelineHandle linePSOHdr_  = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle linePSOSd_   = gfx::PipelineHandle::Invalid;
    gfx::PipelineHandle tonemapPSO_  = gfx::PipelineHandle::Invalid;

    gfx::BufferHandle  cbPerFrame_     = gfx::BufferHandle::Invalid;

    std::unique_ptr<SamplerAssetManager>       samplers_;
    std::unique_ptr<TextureAssetManager>       textures_;

    std::unique_ptr<ReplaceableTextureManager> replaceables_;

    std::unique_ptr<DebugRenderer> debug_;

    gfx::BufferHandle particleServiceVB_     = gfx::BufferHandle::Invalid;
    i32               particleServiceVBSize_ = 0;

    gfx::BufferHandle splatServiceVB_     = gfx::BufferHandle::Invalid;
    i32               splatServiceVBSize_ = 0;

    std::unique_ptr<bls::BlsShaderCache>    blsShaderCache_;
    std::unique_ptr<bls::BlsProgramCatalog> blsPrograms_;
    std::unique_ptr<bls::BlsPsoBuilder>     blsPsoBuilder_;
    const bls::BlsProgram*                  blsSdProgram_     = nullptr;
    const bls::BlsProgram*                  blsSdOnHdProgram_ = nullptr;
    const bls::BlsProgram*                  blsHdProgram_     = nullptr;
    const bls::BlsProgram*                  blsCrystalProgram_= nullptr;

    gfx::BufferHandle                       blsHdVsCb_        = gfx::BufferHandle::Invalid;

    gfx::BufferHandle                       blsHdShadowCb_    = gfx::BufferHandle::Invalid;

    gfx::BufferHandle                       blsHdShadowCountCb_ = gfx::BufferHandle::Invalid;

    gfx::PipelineHandle                     shadowPSO_        = gfx::PipelineHandle::Invalid;

    gfx::PipelineHandle                     shadowPSORigid_   = gfx::PipelineHandle::Invalid;

    gfx::BufferHandle                       shadowVsCb_       = gfx::BufferHandle::Invalid;
    gfx::BufferHandle                       blsHdPsCb_        = gfx::BufferHandle::Invalid;
    gfx::BufferHandle                       blsSdOnHdPsCb_    = gfx::BufferHandle::Invalid;

    gfx::BufferHandle                       blsHdDebugVisCb_  = gfx::BufferHandle::Invalid;

    static constexpr const char* kIblSplitSumLutName = "ibl.splitSumLut";
    static constexpr const char* kIblFromProbeName   = "ibl.fromProbe";
    static constexpr const char* kIblToProbeName     = "ibl.toProbe";

    static constexpr const char* kIblDayProbeName    = "ibl.dayProbe";
    static constexpr const char* kIblNightProbeName  = "ibl.nightProbe";
    f32                                     iblProbeMipEnd_   = 0.0f;
    f32                                     iblDayMipEnd_     = 0.0f;
    f32                                     iblNightMipEnd_   = 0.0f;
    bool                                    iblDayNightLoaded_ = false;
    IblMode                                 iblMode_           = IblMode::Portrait;

    gfx::BufferHandle                       blsSdVsCb_ = gfx::BufferHandle::Invalid;
    gfx::BufferHandle                       blsSdPsCb_ = gfx::BufferHandle::Invalid;

    static constexpr gfx::Format kHdrSceneFormat = gfx::Format::R11G11B10_FLOAT;

    static constexpr gfx::Format kSdSceneFormat  = gfx::Format::R8G8B8A8_UNORM_SRGB;
    bls::BlsShader*         blsSpriteVs_     = nullptr;
    bls::BlsShader*         blsTonemapPs_    = nullptr;
    gfx::BufferHandle       tonemapVB_       = gfx::BufferHandle::Invalid;
    gfx::BufferHandle       tonemapPsCb_     = gfx::BufferHandle::Invalid;
    gfx::SamplerHandle      tonemapSampler_  = gfx::SamplerHandle::Invalid;

    f32                     tonemapExposure_ = 1.0f;

    bool                    ignoreNonLooping_ = true;
    void RunTonemapPass(const RenderTarget& target);

    bool InitBlsShaders();
    void ShutdownBlsShaders();
    bool RenderParticlesBls();
    bool RenderSplatsBls();
    bool RenderGeosetsBls(GeosetBucket bucket);
    bool RenderGeosetsHd(GeosetBucket bucket);
};

}
