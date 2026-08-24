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
#include "pose_request.h"
#include "pose_stage.h"
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
/// @brief A model's world-space extent in its own units, format-neutral.
///
/// Exists because camera framing used to reach through the template into
/// `whiteout::mdx::Model` for per-sequence extents. That is fine while every
/// model is MDX and useless the moment one is not: a non-MDX template fell back
/// to a hard-coded distance of 260 against camera constants sized in the
/// hundreds, and a World of Warcraft creature is 2–5 yards. The result is a
/// sub-pixel dot, which a golden image cannot tell apart from a load failure.
///
/// `valid` is false when a source has no usable extent, which is a real state
/// (a mesh-less template, an M2 whose `.skin` has not landed yet) and not the
/// same as a zero-size box.
struct ModelBounds {
    Vector3f min = {0.0f, 0.0f, 0.0f};
    Vector3f max = {0.0f, 0.0f, 0.0f};
    bool valid = false;
};

struct ModelData {
    std::vector<MeshData> meshes;
    std::vector<TextureData> textures;
    std::vector<MaterialData> materials;
    SkeletonData skeleton;
    std::vector<SkinWeightData> skinWeights;
    std::vector<ParticleEmitterConfig> pe2Configs;
    /// `.m2` emitters. Mutually exclusive with `pe2Configs` in practice — a
    /// model comes from one format — so the two share an emitter id space.
    std::vector<M2ParticleEmitterConfig> m2ParticleConfigs;
    std::vector<effects::RibbonEmitterConfig> ribbonConfigs;
    std::vector<CollisionShapeData> collisionConfigs;
    std::vector<ClothOverlayData> clothOverlays;
    std::vector<AttachmentConfig> attachmentConfigs;
    std::vector<PE1EmitterConfig> pe1Configs;
    std::vector<CornEmitterInit> cornEmitterInits;
    std::vector<EventObjectConfig> eventObjects;
    std::vector<CameraPreset> cameraPresets;
    std::vector<SequenceInfo> sequences;
    std::vector<u32> globalSequences; ///< Global-sequence durations in ms.
    ModelBounds bounds;               ///< What camera framing measures against.
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

    /// @brief Evaluate the animation for one pose.
    ///
    /// A single-clip implementation reads @ref PoseRequest::PrimaryClip and
    /// ignores the rest; that is what all three current implementations do,
    /// and it is the whole of Warcraft III's use. See @ref PoseRequest for
    /// why the parameter object is shaped the way it is.
    virtual FrameState Evaluate(const PoseRequest& req) const = 0;

    /// @brief Return the sequence table (name, start/end ms, move speed).
    virtual std::vector<SequenceInfo> GetSequences() const = 0;

    /// @brief What a bare sequence switch should do for this format.
    ///
    /// Defaults to a hard cut, which is what Warcraft III and World of
    /// Warcraft do. StarCraft II overrides it.
    virtual TransitionPolicy DefaultTransition() const {
        return {};
    }

    /// @brief Append this model's post-sampling corrections, in run order.
    ///
    /// Called once per actor at load. Warcraft III, World of Warcraft, and any
    /// `.m3` without solver chunks append nothing, which is why the default is
    /// a no-op rather than pure.
    ///
    /// **Order is the contract**: solvers first (they correct the animated
    /// pose), physics last (it consumes the corrected one) — StarCraft II's own
    /// frame order. The runner executes what it is given and never sorts.
    ///
    /// Plural from the start on purpose. See @ref IPoseStage.
    virtual void CreatePoseStages(animation::PoseStageList& out) const {
        (void)out;
    }
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
    /// @brief The model's cloths, for the debug overlay. Empty for every format
    ///        but `.m3`, which is the only one with a soft-body solver behind it.
    virtual std::vector<ClothOverlayData> GetClothOverlays() {
        return {};
    }
    virtual std::vector<AttachmentConfig> GetAttachmentConfigs() {
        return {};
    }
    virtual std::vector<PE1EmitterConfig> GetPE1Configs() {
        return {};
    }
    /// @brief `.m2` particle emitters. Empty for every other format — a model
    ///        never has both these and `GetParticleConfigs()`, so the two share
    ///        one emitter id space in the service without colliding.
    virtual std::vector<M2ParticleEmitterConfig> GetM2ParticleConfigs() {
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
    /// @brief Camera poses authored into the model. MDX has them; nothing
    ///        else does yet.
    virtual std::vector<CameraPreset> GetCameraPresets() {
        return {};
    }

    /// @brief The model's extent, for camera framing. See @ref ModelBounds.
    ///
    /// The default unions every mesh's vertex positions — a correct bind-pose
    /// box for any format, and what a source gets for free by implementing
    /// `GetMeshes`. A source that knows better overrides: MDX's animated
    /// sequence extents bound the model as it actually *moves*, which is a
    /// bigger and more useful box than the bind pose.
    virtual ModelBounds GetBounds() {
        ModelBounds b;
        for (const MeshData& mesh : GetMeshes()) {
            for (const Vector3f& p : mesh.positions) {
                if (!b.valid) {
                    b.min = b.max = p;
                    b.valid = true;
                    continue;
                }
                b.min.x = (p.x < b.min.x) ? p.x : b.min.x;
                b.min.y = (p.y < b.min.y) ? p.y : b.min.y;
                b.min.z = (p.z < b.min.z) ? p.z : b.min.z;
                b.max.x = (p.x > b.max.x) ? p.x : b.max.x;
                b.max.y = (p.y > b.max.y) ? p.y : b.max.y;
                b.max.z = (p.z > b.max.z) ? p.z : b.max.z;
            }
        }
        return b;
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
        d.m2ParticleConfigs = GetM2ParticleConfigs();
        d.ribbonConfigs = GetRibbonConfigs();
        d.collisionConfigs = GetCollisionShapes();
        d.clothOverlays = GetClothOverlays();
        d.attachmentConfigs = GetAttachmentConfigs();
        d.pe1Configs = GetPE1Configs();
        d.cornEmitterInits = GetCornEmitterInits();
        d.eventObjects = GetEventObjects();
        d.globalSequences = GetGlobalSequences();
        d.sequences = GetSequences();
        d.cameraPresets = GetCameraPresets();
        d.bounds = GetBounds();
        return d;
    }
};

} // namespace whiteout::flakes::renderer::model

namespace whiteout::flakes {
using ::whiteout::flakes::renderer::model::IAnimationSource;
using ::whiteout::flakes::renderer::model::IModelDataSource;
using ::whiteout::flakes::renderer::model::IModelSource;
using ::whiteout::flakes::renderer::model::ModelBounds;
using ::whiteout::flakes::renderer::model::ModelData;
} // namespace whiteout::flakes
