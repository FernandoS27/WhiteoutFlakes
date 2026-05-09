#pragma once

#include "common_types.h"
#include "model/model_types.h"
#include "particle.h"
#include "effects/ribbon.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout::flakes::renderer::model {

struct SequenceInfo {
    std::string name;
    i32         startMs = 0;
    i32         endMs   = 0;

    f32         moveSpeed = 0.0f;

    bool        nonLooping = false;
};

struct ModelData {
    std::vector<MeshData>              meshes;
    std::vector<TextureData>           textures;
    std::vector<MaterialData>          materials;
    SkeletonData                       skeleton;
    std::vector<SkinWeightData>        skinWeights;
    std::vector<ParticleEmitterConfig> pe2Configs;
    std::vector<effects::RibbonEmitterConfig> ribbonConfigs;
    std::vector<CollisionShapeData>    collisionConfigs;
    std::vector<AttachmentConfig>      attachmentConfigs;
    std::vector<PE1EmitterConfig>      pe1Configs;
    std::vector<EventObjectConfig>     eventObjects;
    std::vector<CameraPreset>          cameraPresets;
    std::vector<SequenceInfo>          sequences;

    std::vector<u32>                   globalSequences;
};

class IModelDataSource {
public:
    virtual ~IModelDataSource() = default;

    virtual ModelData Build() = 0;

    using TextureCacheQuery = std::function<bool(std::string_view)>;
    void SetTextureCacheQuery(TextureCacheQuery q) { textureCacheQuery_ = std::move(q); }

protected:
    bool IsTextureCached(std::string_view key) const {
        return textureCacheQuery_ && textureCacheQuery_(key);
    }

private:
    TextureCacheQuery textureCacheQuery_;
};

class IAnimationSource {
public:
    virtual ~IAnimationSource() = default;

    virtual FrameState Evaluate(i32 sequenceIdx, i32 timeMs, i32 globalTimeMs,
                                const Matrix44f& worldTransform,
                                const Vector3f&  cameraPos) const = 0;

    virtual std::vector<SequenceInfo> GetSequences() const = 0;
};

class IModelSource : public IModelDataSource, public IAnimationSource {
public:

    virtual std::vector<MeshData>              GetMeshes()           = 0;
    virtual std::vector<TextureData>           GetTextures()         = 0;
    virtual std::vector<MaterialData>          GetMaterials()        = 0;
    virtual SkeletonData                       GetSkeleton()         = 0;
    virtual std::vector<SkinWeightData>        GetSkinWeights()      = 0;
    virtual std::vector<ParticleEmitterConfig> GetParticleConfigs()  = 0;
    virtual std::vector<effects::RibbonEmitterConfig> GetRibbonConfigs()    = 0;
    virtual std::vector<CollisionShapeData>    GetCollisionShapes()  = 0;
    virtual std::vector<AttachmentConfig>      GetAttachmentConfigs() { return {}; }
    virtual std::vector<PE1EmitterConfig>      GetPE1Configs()       { return {}; }
    virtual std::vector<EventObjectConfig>     GetEventObjects()     { return {}; }
    virtual std::vector<u32>                   GetGlobalSequences()  { return {}; }

    ModelData Build() override {
        ModelData d;
        d.meshes            = GetMeshes();
        d.textures          = GetTextures();
        d.materials         = GetMaterials();
        d.skeleton          = GetSkeleton();
        d.skinWeights       = GetSkinWeights();
        d.pe2Configs        = GetParticleConfigs();
        d.ribbonConfigs     = GetRibbonConfigs();
        d.collisionConfigs  = GetCollisionShapes();
        d.attachmentConfigs = GetAttachmentConfigs();
        d.pe1Configs        = GetPE1Configs();
        d.eventObjects      = GetEventObjects();
        d.globalSequences   = GetGlobalSequences();
        d.sequences         = GetSequences();
        return d;
    }
};

}
