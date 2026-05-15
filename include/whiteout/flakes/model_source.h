#pragma once

/// @file model_source.h
/// @brief Host-implementable interfaces that feed model + animation data
///        into the renderer.
///
/// The renderer doesn't know how to parse MDX itself — `MdxModelAdapter`
/// (in `src/io/`) implements `IModelSource` on top of WhiteoutLib's MDX
/// parser, and the 3ds Max plugin implements it on top of the Max SDK.
/// Hosts that have their own asset pipeline can implement the interface
/// to drive the renderer from any source.
///
/// `ModelLoader` takes a `shared_ptr<IModelSource>` at spawn time,
/// snapshots the static data via `Build()`, then calls
/// `IAnimationSource::Evaluate()` once per frame for live playback.

#include "display.h" // SequenceInfo
#include "model_types.h"
#include "types.h"

#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace whiteout::flakes::renderer::model {

/// @brief Aggregated static-model snapshot returned by
///        `IModelDataSource::Build()`.
///
/// Every field is owned by-value; the renderer can keep this struct
/// alive independently of the source.
struct ModelData {
    std::vector<MeshData> meshes;
    std::vector<TextureData> textures;
    std::vector<MaterialData> materials;
    SkeletonData skeleton;
    std::vector<SkinWeightData> skinWeights;
    std::vector<ParticleEmitterConfig> pe2Configs;
    std::vector<effects::RibbonEmitterConfig> ribbonConfigs;
    std::vector<CollisionShapeData> collisionConfigs;
    std::vector<AttachmentConfig> attachmentConfigs;
    std::vector<PE1EmitterConfig> pe1Configs;
    std::vector<CornEmitterInit> cornEmitterInits;
    std::vector<EventObjectConfig> eventObjects;
    std::vector<CameraPreset> cameraPresets;
    std::vector<SequenceInfo> sequences;
    std::vector<u32> globalSequences; ///< Global-sequence durations in ms.
};

/// @brief Static-data side of a model source.
///
/// Implementations build the @ref ModelData snapshot on demand. The
/// `TextureCacheQuery` is an optional hook the renderer sets so the
/// source can skip decoding textures already in the GPU cache (helps
/// the live Max-plugin adapter avoid redundant BLP decodes).
class IModelDataSource {
public:
    virtual ~IModelDataSource() = default;

    /// @brief Produce the static-model snapshot.
    virtual ModelData Build() = 0;

    /// @brief Predicate: returns `true` if a texture with key @p key is
    ///        already cached on the renderer side.
    using TextureCacheQuery = std::function<bool(std::string_view)>;

    /// @brief Install (or replace) the texture-cache predicate.
    void SetTextureCacheQuery(TextureCacheQuery q) {
        textureCacheQuery_ = std::move(q);
    }

protected:
    /// @brief Helper for subclasses to consult the installed predicate.
    bool IsTextureCached(std::string_view key) const {
        return textureCacheQuery_ && textureCacheQuery_(key);
    }

private:
    TextureCacheQuery textureCacheQuery_;
};

/// @brief Animation side of a model source.
///
/// Called per actor per frame. Implementations sample bone tracks,
/// particle / emitter tracks, fresnel / alpha / texanim, fill in the
/// @ref FrameState, and return it by value.
class IAnimationSource {
public:
    virtual ~IAnimationSource() = default;

    /// @brief Evaluate the animation at a given time.
    /// @param sequenceIdx     Index into @ref IAnimationSource::GetSequences (`-1` ⇒ T-pose).
    /// @param timeMs          Local time within the active sequence (ms).
    /// @param globalTimeMs    Global wall-clock time for global-sequence
    ///                        tracks (`-1` ⇒ use @p timeMs).
    /// @param worldTransform  World-space root transform of this actor.
    /// @param cameraPos       Camera position in world space, for
    ///                        camera-anchored billboard bones.
    virtual FrameState Evaluate(i32 sequenceIdx, i32 timeMs, i32 globalTimeMs,
                                const Matrix44f& worldTransform,
                                const Vector3f& cameraPos) const = 0;

    /// @brief Return the sequence table (name, start/end ms, move speed).
    virtual std::vector<SequenceInfo> GetSequences() const = 0;
};

/// @brief Composite source that hosts implement to drive everything from
///        one object.
///
/// Provides per-section virtual accessors plus a default `Build()` that
/// stitches them into a @ref ModelData. Override per-section methods
/// that apply to your data source; the rest default to empty.
class IModelSource : public IModelDataSource, public IAnimationSource {
public:
    virtual std::vector<MeshData> GetMeshes() = 0;
    virtual std::vector<TextureData> GetTextures() = 0;
    virtual std::vector<MaterialData> GetMaterials() = 0;
    virtual SkeletonData GetSkeleton() = 0;
    virtual std::vector<SkinWeightData> GetSkinWeights() = 0;
    virtual std::vector<ParticleEmitterConfig> GetParticleConfigs() = 0;
    virtual std::vector<effects::RibbonEmitterConfig> GetRibbonConfigs() = 0;
    virtual std::vector<CollisionShapeData> GetCollisionShapes() = 0;
    virtual std::vector<AttachmentConfig> GetAttachmentConfigs() {
        return {};
    }
    virtual std::vector<PE1EmitterConfig> GetPE1Configs() {
        return {};
    }
    virtual std::vector<CornEmitterInit> GetCornEmitterInits() {
        return {};
    }
    virtual std::vector<EventObjectConfig> GetEventObjects() {
        return {};
    }
    virtual std::vector<u32> GetGlobalSequences() {
        return {};
    }

    /// @brief Default `Build()` — calls each `GetXxx` once and aggregates
    ///        the results. Subclasses normally don't need to override.
    ModelData Build() override {
        ModelData d;
        d.meshes = GetMeshes();
        d.textures = GetTextures();
        d.materials = GetMaterials();
        d.skeleton = GetSkeleton();
        d.skinWeights = GetSkinWeights();
        d.pe2Configs = GetParticleConfigs();
        d.ribbonConfigs = GetRibbonConfigs();
        d.collisionConfigs = GetCollisionShapes();
        d.attachmentConfigs = GetAttachmentConfigs();
        d.pe1Configs = GetPE1Configs();
        d.cornEmitterInits = GetCornEmitterInits();
        d.eventObjects = GetEventObjects();
        d.globalSequences = GetGlobalSequences();
        d.sequences = GetSequences();
        return d;
    }
};

} // namespace whiteout::flakes::renderer::model

namespace whiteout::flakes {
using ::whiteout::flakes::renderer::model::IAnimationSource;
using ::whiteout::flakes::renderer::model::IModelDataSource;
using ::whiteout::flakes::renderer::model::IModelSource;
using ::whiteout::flakes::renderer::model::ModelData;
} // namespace whiteout::flakes
