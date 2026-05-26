#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <whiteout/models/mdx/types.h>
#include "file_resolver.h"
#include "io/mdx_animation.h"
#include "renderer/particle/plane_emitter.h"
#include "whiteout/flakes/model_source.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::io {

class IContentProvider;

class MdxModelAdapter : public ::whiteout::flakes::renderer::model::IModelSource {
public:
    explicit MdxModelAdapter(whiteout::mdx::Model model, std::filesystem::path basePath = {},
                             IContentProvider* contentProvider = nullptr);

    std::vector<::whiteout::flakes::renderer::model::MeshData> GetMeshes() override;
    std::vector<::whiteout::flakes::renderer::model::TextureData> GetTextures() override;
    std::vector<::whiteout::flakes::renderer::model::MaterialData> GetMaterials() override;
    ::whiteout::flakes::renderer::model::SkeletonData GetSkeleton() override;
    std::vector<::whiteout::flakes::renderer::model::SkinWeightData> GetSkinWeights() override;
    std::vector<::whiteout::flakes::renderer::ParticleEmitterConfig> GetParticleConfigs() override;
    std::vector<::whiteout::flakes::renderer::effects::RibbonEmitterConfig> GetRibbonConfigs()
        override;

    std::vector<::whiteout::flakes::renderer::particle::PlaneEmitterInit> GetPlaneEmitterInits()
        const;
    std::vector<::whiteout::flakes::renderer::model::CollisionShapeData> GetCollisionShapes()
        override;
    std::vector<::whiteout::flakes::renderer::model::AttachmentConfig> GetAttachmentConfigs()
        override;
    std::vector<::whiteout::flakes::renderer::model::PE1EmitterConfig> GetPE1Configs() override;
    std::vector<::whiteout::flakes::renderer::model::CornEmitterInit> GetCornEmitterInits()
        override;
    std::vector<::whiteout::flakes::renderer::model::EventObjectConfig> GetEventObjects() override;
    std::vector<u32> GetGlobalSequences() override;

    ::whiteout::flakes::renderer::model::FrameState Evaluate(
        i32 sequenceIdx, i32 timeMs, i32 globalTimeMs, const Matrix44f& worldTransform,
        const Vector3f& cameraPos) const override;

    std::vector<::whiteout::flakes::renderer::model::SequenceInfo> GetSequences() const override;

    std::vector<::whiteout::flakes::renderer::model::CameraPreset> GetCameraPresets() const;

    // The parsed source model, kept intact for animation Evaluate(). Exposed
    // so hosts can re-serialise it (Save As) without re-reading the file.
    const whiteout::mdx::Model& SourceModel() const {
        return model_;
    }

private:
    whiteout::mdx::Model model_;
    std::filesystem::path basePath_;
    FileResolver resolver_;
    IContentProvider* contentProvider_ = nullptr;
    MdxHierarchy hierarchy_;

    std::vector<i32> boneGateGeoset_;

    i32 MapPE2FilterMode(whiteout::u32 mdxMode) const;
    i32 MapShadingFlags(whiteout::mdx::Layer::ShadingFlag sf) const;
};

} // namespace whiteout::flakes::io
