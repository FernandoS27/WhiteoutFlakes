#pragma once

#include "common_types.h"
#include "model_source.h"
#include "io/mdx_animation.h"
#include "file_resolver.h"
#include "renderer/particle/plane_emitter.h"
#include <whiteout/models/mdx/types.h>
#include <string>
#include <filesystem>
#include <vector>

namespace WhiteoutDex {

class IContentProvider;

class MdxModelAdapter : public IModelSource {
public:

    explicit MdxModelAdapter(whiteout::mdx::Model model,
                             std::filesystem::path basePath = {},
                             IContentProvider* contentProvider = nullptr);

    std::vector<MeshData>              GetMeshes()          override;
    std::vector<TextureData>           GetTextures()        override;
    std::vector<MaterialData>          GetMaterials()       override;
    SkeletonData                       GetSkeleton()        override;
    std::vector<SkinWeightData>        GetSkinWeights()     override;
    std::vector<ParticleEmitterConfig> GetParticleConfigs() override;
    std::vector<RibbonEmitterConfig>   GetRibbonConfigs()   override;

    std::vector<particle::PlaneEmitterInit> GetPlaneEmitterInits() const;
    std::vector<CollisionShapeData>    GetCollisionShapes() override;
    std::vector<AttachmentConfig>      GetAttachmentConfigs() override;
    std::vector<PE1EmitterConfig>      GetPE1Configs()      override;
    std::vector<EventObjectConfig>     GetEventObjects()    override;
    std::vector<u32>                   GetGlobalSequences() override;

    FrameState Evaluate(i32 sequenceIdx, i32 timeMs, i32 globalTimeMs,
                        const Matrix44f& worldTransform,
                        const Vector3f&  cameraPos) const override;

    std::vector<SequenceInfo> GetSequences() const override;

    std::vector<CameraPreset> GetCameraPresets() const;

private:
    whiteout::mdx::Model model_;
    std::filesystem::path basePath_;
    FileResolver resolver_;
    IContentProvider* contentProvider_ = nullptr;
    MdxHierarchy hierarchy_;

    std::vector<i32> boneGateGeoset_;

    i32 MapPE2FilterMode(whiteout::u32 mdxMode) const;
    i32 MapShadingFlags(whiteout::mdx::Layer::ShadingFlag sf) const;

    TextureData LoadTextureFile(const std::string& path, i32 textureId,
                                i32 replaceableId) const;

};

}
