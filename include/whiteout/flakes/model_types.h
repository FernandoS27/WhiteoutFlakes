#pragma once

/// @file model_types.h
/// @brief Public canonical home for every value type the renderer's model
///        loader and per-frame evaluator exchange with hosts.
///
/// Covers meshes, textures, materials, skeletons, skinning, collision
/// shapes, attachment / PE1 / corn / event configs, particle and ribbon
/// configs, and `FrameState` (the per-frame evaluation output).
///
/// Types live in their original sub-namespaces (`renderer::model::*`,
/// `renderer::*`, `renderer::effects::*`) to keep all existing internal
/// source code compiling unchanged; the public top-level namespace
/// `whiteout::flakes` re-exports the consumer-facing names via
/// using-aliases at the bottom of the file.

#include "display.h"
#include "enums.h"
#include "gfx_types.h"
#include "types.h"

#include <functional>
#include <string>
#include <vector>

// ----------------------------------------------------------------------------
// Particle / ribbon emitter configs.
// Particle emitter SIMULATION state (ParticleEmitterState, RibbonEmitterState,
// RibbonSegment, RibbonEmitter, RibbonSystem) lives in src/renderer/particle.h
// and src/renderer/effects/ribbon.h — internal to the renderer.
// ----------------------------------------------------------------------------
namespace whiteout::flakes::renderer {

/// @brief Static description of one MDX ParticleEmitter2 (PE2) emitter.
///
/// Mirrors the on-disk PE2 chunk. Per-frame animated values
/// (emissionRate, speed, visibility, …) ride through the per-actor
/// `FrameState::particleStates`.
struct ParticleEmitterConfig {
    i32 textureId = -1;
    i32 filterMode = 0;          ///< @ref FilterMode.
    i32 rows = 1, cols = 1;      ///< Sprite-sheet grid.
    bool unshaded = false;

    f32 lifeSpan = 1.0f;
    bool squirt = false;

    Vector3f startColor = {1, 1, 1};
    Vector3f midColor = {0.5f, 0.5f, 0.5f};
    Vector3f endColor = {0, 0, 0};
    f32 startAlpha = 255, midAlpha = 128, endAlpha = 0;
    f32 startScale = 10, midScale = 10, endScale = 10;
    f32 midTime = 0.5f;

    i32 particleType = 1;
    f32 tailLength = 1.0f;

    i32 headLifeStart = 0, headLifeEnd = 0, headLifeRepeat = 1;
    i32 headDecayStart = 0, headDecayEnd = 0, headDecayRepeat = 1;
    i32 tailLifeStart = 0, tailLifeEnd = 0, tailLifeRepeat = 1;
    i32 tailDecayStart = 0, tailDecayEnd = 0, tailDecayRepeat = 1;

    bool modelSpace = false;
    bool xyQuad = false;
    bool sortZ = false;
    bool lineEmitter = false;
    bool unfogged = false;

    i32 count = 0;
    i32 priorityPlane = 0;
    i32 replaceableId = 0;
};

} // namespace whiteout::flakes::renderer

namespace whiteout::flakes::renderer::effects {

/// @brief Static description of one MDX ribbon emitter. Per-frame state
///        (above/below/alpha/colour) flows through
///        `FrameState::ribbonStates`.
struct RibbonEmitterConfig {
    i32 textureId = -1;
    i32 filterMode = 0;      ///< @ref FilterMode.
    i32 rows = 1, cols = 1;  ///< Sprite-sheet grid.
    bool unshaded = false;
    bool twoSided = true;
    f32 emission = 10.0f;    ///< Segments-per-second emission rate.
    f32 life = 1.0f;         ///< Segment lifetime in seconds.
    f32 gravity = 0.0f;

    i32 priorityPlane = 0;
};

} // namespace whiteout::flakes::renderer::effects

namespace whiteout::flakes::renderer::model {

// CameraPreset / SequenceInfo are canonical in display.h; using-import them
// here so existing internal code that says
// `whiteout::flakes::renderer::model::CameraPreset` /
// `whiteout::flakes::renderer::model::SequenceInfo` keeps compiling.
using ::whiteout::flakes::CameraPreset;
using ::whiteout::flakes::SequenceInfo;

/// @brief Slot binding for an attachment point: which attachment-id this
///        node represents and the optional child MDX hooked into it.
struct AttachmentConfig {
    i32 attachmentId = 0;
    std::string modelPath;
};

/// @brief PE1 (legacy ParticleEmitter1) emitter — spawns a child MDX
///        whose own emitters drive the particle simulation.
struct PE1EmitterConfig {
    std::string modelPath;
    f32 lifespan = 1.0f;
    f32 scale = 1.0f;
};

/// @brief Static config for one CornFx (CornEmitter) MDX node.
///
/// Mirrors what `CParticleEmitterCornEffects` pulls out of MDLDATA at
/// load time: `.pkb` path, animation visibility guide, initial
/// multipliers, and the `cornEffectsScaling` node-flag bit. Used once at
/// registration; per-frame values flow through `FrameState::cornStates`.
struct CornEmitterInit {
    i32 emitterId = -1;              ///< Index into `model.cornEmitters`.
    std::string pkbPath;             ///< `CornEmitter::path` (`.pkb` / `.pkfx`).
    std::string animVisibilityGuide; ///< Anim-state gate (parsed by emitter).
    f32 defaultLifeSpan = 0.0f;
    f32 defaultEmissionRate = 0.0f;
    f32 defaultSpeed = 0.0f;
    Vector4f defaultColor = {1, 1, 1, 1};
    i32 replaceableId = 0;
    bool cornEffectsScaling = false; ///< Node flag bit 0x40000.
};

/// @brief One MDX event-object (the prefix-keyed nodes that fire SPN /
///        SPL / UBR / FPT / SND effects on specific animation frames).
struct EventObjectConfig {
    enum class Kind : u8 { SPN, SPL, UBR, FPT, SND, Unknown };

    std::string name;
    Kind kind = Kind::Unknown;
    std::string id;                  ///< Event id (e.g. "SPLBhfoo"); looked up via @ref FindSpn etc.
    i32 nodeIndex = -1;

    Vector3f pivot = {0, 0, 0};
    u32 globalSequenceId = 0xFFFFFFFFu; ///< Sentinel: not a global sequence.
    std::vector<u32> eventTrackTimes;   ///< Sample times in ms when the event fires.
};

// Canonical filter mode + helpers + material flags + bone billboard flags
// live in enums.h. Re-export under this nested namespace so existing
// internal code that writes `whiteout::flakes::renderer::model::FILTER_BLEND`
// or `model::MapFilterMode(...)` keeps compiling.
using ::whiteout::flakes::FILTER_ADD_ALPHA;
using ::whiteout::flakes::FILTER_ADDITIVE;
using ::whiteout::flakes::FILTER_BLEND;
using ::whiteout::flakes::FILTER_MODULATE;
using ::whiteout::flakes::FILTER_MODULATE_2X;
using ::whiteout::flakes::FILTER_NONE;
using ::whiteout::flakes::FILTER_TRANSPARENT;
using ::whiteout::flakes::FilterMode;
using ::whiteout::flakes::MapFilterMode;
using ::whiteout::flakes::MapPE2BlendMode;

using ::whiteout::flakes::MAT_CONSTANT_COLOR;
using ::whiteout::flakes::MAT_NO_DEPTH_SET;
using ::whiteout::flakes::MAT_NO_DEPTH_TEST;
using ::whiteout::flakes::MAT_TWO_SIDED;
using ::whiteout::flakes::MAT_UNFOGGED;
using ::whiteout::flakes::MAT_UNSHADED;
using ::whiteout::flakes::MaterialFlags;

/// @brief One geoset of mesh data — vertex streams + index buffer.
///
/// Each MDX geoset becomes one `MeshData`; multiple LODs of the same
/// geoset all share `geosetId` and differ by `lod`. The `tangents` field
/// is `Vector4f` so the bitangent sign rides along in `w`.
struct MeshData {
    i32 geosetId;
    i32 materialId;

    u32 lod = 0;                       ///< LOD index (0 = highest detail).
    std::vector<Vector3f> positions;
    std::vector<Vector3f> normals;
    std::vector<Vector2f> uvs;
    std::vector<Vector2f> uvs1;        ///< Optional second UV channel (HD models).
    std::vector<Vector4f> tangents;    ///< xyz = tangent, w = bitangent sign.
    std::vector<u32> indices;
};

/// @brief Sentinel layer-textureId meaning "synthesize the team-colour
///        texture at runtime" — used by the HD pipeline so adapters
///        don't have to ship a per-team .blp.
inline constexpr i32 kHdTeamColorActive = -2;

/// @brief Tightly-packed CPU-side texture data ready for GPU upload.
///
/// `sharedKey` is the renderer's content-addressed cache key — adapters
/// set it (e.g. to the source path or hash) so the same texture
/// referenced from multiple models uploads only once.
struct TextureData {
    i32 textureId;
    i32 replaceableId;

    std::vector<u8> pixels;
    gfx::Format format = gfx::Format::R8G8B8A8_UNORM;
    i32 width, height;
    i32 mipLevels = 1;
    u32 wrapFlags = 0x3;                ///< Bit0 = repeat-U, Bit1 = repeat-V.

    std::string sharedKey;
};

/// @brief One blend-stage of a material.
///
/// SD models use only `textureId` + `alpha` + `filterMode`; HD models
/// add the PBR slots (`normalMapId`, `ormMapId`, `emissiveMapId`,
/// `teamColorMapId`) plus fresnel coefficients and per-layer emissive gain.
struct MaterialLayerData {
    i32 filterMode;                      ///< @ref FilterMode.
    i32 textureId;
    f32 alpha;
    i32 flags;                           ///< @ref MaterialFlags.
    i32 textureAnimationId = -1;         ///< `-1` if none.

    i32 coordId = 0;                     ///< Which UV channel to sample (0 or 1).
    i32 shaderId = 0;                    ///< HD shader-permutation selector.

    i32 normalMapId = -1;
    i32 ormMapId = -1;
    i32 emissiveMapId = -1;
    i32 teamColorMapId = -1;             ///< Or @ref kHdTeamColorActive for synth.

    f32 emissiveGain = 0.0f;             ///< Multiplier on the emissive contribution.
    f32 fresnelOpacity = 0.0f;
    f32 fresnelTeamColor = 0.0f;
    Vector3f fresnelColor = {0.0f, 0.0f, 0.0f};
};

/// @brief A material is an ordered list of `MaterialLayerData` blended
///        top-down; `priorityPlane` + `sortOrder` resolve transparent
///        draw ordering between materials.
struct MaterialData {
    i32 materialId;
    std::vector<MaterialLayerData> layers;
    i32 priorityPlane;
    i32 sortOrder;
};

using ::whiteout::flakes::BONE_BILLBOARD_CAMERA_ANCHORED;
using ::whiteout::flakes::BONE_BILLBOARD_FULL;
using ::whiteout::flakes::BONE_BILLBOARD_LOCK_X;
using ::whiteout::flakes::BONE_BILLBOARD_LOCK_Y;
using ::whiteout::flakes::BONE_BILLBOARD_LOCK_Z;
using ::whiteout::flakes::BONE_BILLBOARD_NONE;
using ::whiteout::flakes::BoneBillboardFlag;

/// @brief Skeleton at bind pose.
///
/// `inverseBindMatrices` are the per-node bone-space-to-mesh-space
/// transforms used by the GPU skinning shader. `billboardFlags` is one
/// bitmask per node — see @ref BoneBillboardFlag.
struct SkeletonData {
    i32 nodeCount;
    std::vector<Matrix44f> inverseBindMatrices;
    std::vector<u32> billboardFlags;
    std::vector<Vector3f> nodePivots;
    std::vector<i32> nodeParents;
};

/// @brief Up-to-4-bone skinning influence for one vertex.
///
/// `boneIdx[i]` indexes into the skeleton, `weight[i]` is in `[0,1]`.
/// Unused slots have weight 0; weights are pre-normalised by the loader.
struct VertexInfluence {
    i32 boneIdx[4] = {0, 0, 0, 0};
    f32 weight[4] = {0, 0, 0, 0};
};

/// @brief MDX "vertex group" record — averages a set of bones into a
///        single virtual slot.
struct GroupAverageRecord {
    i32 pseudoSlot;
    std::vector<i32> nodeIndices;
};

/// @brief Skinning data for one geoset.
struct SkinWeightData {
    i32 geosetId;
    std::vector<VertexInfluence> influences;
    std::vector<GroupAverageRecord> groupAverages;
    /// Bones-this-geoset-actually-touches subset (matrix-palette index ⇒
    /// global node index); enables a smaller per-geoset bone palette.
    std::vector<i32> subsetNodeIndices;
};

/// @brief Per-actor collision primitive (used for ground-clamp /
///        attack-event hit-tests on the engine side; visualised as
///        wireframes when @ref DisplayFlags::showCollisions is on).
///
/// `type` mirrors MDX's encoding: 0 = box (vertices[0/1] are min/max),
/// 1 = sphere (radius around pivot), 2 = capsule.
struct CollisionShapeData {
    i32 type;
    Vector3f vertices[2];
    f32 radius;
    Vector3f pivot = {0, 0, 0};
};

/// @brief Per-frame evaluation output for one actor.
///
/// Returned by `IAnimationSource::Evaluate()`; the renderer applies it
/// to the actor's GPU state before the next draw.
struct FrameState {
    /// Bone palette uploaded to the skinning shader (one matrix per node).
    std::vector<Matrix44f> boneWorldMatrices;
    /// Optional per-geoset post-multiply (rarely non-identity outside
    /// billboarded geosets).
    std::vector<Matrix44f> geosetTransforms;
    /// Per-geoset opacity (KGAO track sample); used to gate full-geoset fades.
    std::vector<f32> geosetAlphas;
    /// Per-geoset RGB tint (KGAC); applied multiplicatively in the PS.
    std::vector<Vector3f> geosetColors;

    /// @brief Per-PE2-emitter sampled state (one entry per emitter that
    ///        contributes this frame).
    struct ParticleFrameState {
        i32 emitterId;
        Matrix44f transform;
        f32 emissionRate, speed, variation, coneAngle;
        f32 gravity, width, length, visibility;
        bool squirting;
    };
    std::vector<ParticleFrameState> particleStates;

    /// @brief Per-ribbon sampled state. `slot` is the texture slot
    ///        selected by KRTX tracks (0..3 typically).
    struct RibbonFrameState {
        i32 emitterId;
        Matrix44f transform;
        f32 above, below, alpha;
        Vector3f color;
        f32 visibility;
        i32 slot;
    };
    std::vector<RibbonFrameState> ribbonStates;

    /// Per-collision-shape world transforms (debug overlay).
    std::vector<Matrix44f> collisionTransforms;

    /// @brief Per-layer 2D texture-coord transform (offset / tile / rotation).
    struct TexAnimState {
        i32 materialId;
        i32 layerIndex;
        f32 uOff, vOff, uTile, vTile;
        f32 rotation;
    };
    std::vector<TexAnimState> texAnims;

    enum class LightKind : u8 { Directional = 0, Omni = 1, Ambient = 2 };
    /// @brief Per-MDX-light sampled state (KLAC / KLAI / KLAS tracks).
    struct LightState {
        LightKind kind = LightKind::Directional;
        Vector3f worldPos = {0, 0, 0};
        Vector3f worldDir = {0, 0, -1};
        Vector3f diffuse = {0, 0, 0};
        Vector3f ambient = {0, 0, 0};
        f32 attenStart = 0.0f;
        f32 attenEnd = 0.0f;
        bool enabled = true;
    };
    std::vector<LightState> lights;

    /// @brief Pre-baked 2x4 texture-coord matrix for fast per-vertex apply
    ///        (row0/row1 of the 3x3 affine matrix, stored as `vec4` so
    ///        the shader can mad it).
    struct TexAnimMatrix {
        i32 textureAnimId;
        f32 row0[4];
        f32 row1[4];
    };
    std::vector<TexAnimMatrix> texAnimMatrices;

    /// @brief Per-(material, layer) alpha override sampled from KMTA.
    struct LayerAlphaState {
        i32 materialId;
        i32 layerIndex;
        f32 alpha;
    };
    std::vector<LayerAlphaState> layerAlphas;

    /// @brief Slot a `LayerTextureIdState` overrides (diffuse for SD,
    ///        the four HD slots otherwise).
    enum class LayerTexSlot : u8 {
        Diffuse = 0,
        Normal = 1,
        ORM = 2,
        Emissive = 3,
        TeamColor = 4,
    };
    /// @brief Per-(material, layer, slot) texture-ID swap sampled from KMTF.
    struct LayerTextureIdState {
        i32 materialId;
        i32 layerIndex;
        LayerTexSlot slot = LayerTexSlot::Diffuse;
        i32 textureId;
    };
    std::vector<LayerTextureIdState> layerTextureIds;

    /// @brief Per-(material, layer) fresnel-coefficient overrides sampled
    ///        from KFC3 / KFCA / KFTC / KMTE tracks.
    struct LayerFresnelState {
        i32 materialId;
        i32 layerIndex;
        Vector3f fresnelColor;
        f32 fresnelOpacity;
        f32 fresnelTeamColor;
        f32 emissiveGain;
    };
    std::vector<LayerFresnelState> layerFresnels;

    /// @brief Per-attachment-slot transform + visibility (so the engine
    ///        can place child models / particle effects at the slot's
    ///        animated pose).
    struct AttachmentFrameState {
        i32 attachmentIndex;
        Matrix44f transform;
        f32 visibility;
    };
    std::vector<AttachmentFrameState> attachmentStates;

    /// @brief Per-PE1-emitter sampled state.
    struct PE1FrameState {
        i32 emitterId;
        Matrix44f transform;
        f32 emissionRate, speed, latitude, longitude;
        f32 gravity, visibility;
    };
    std::vector<PE1FrameState> pe1States;

    /// @brief Per-CornFx-emitter sampled state.
    ///
    /// Sampled by `MdxModelAdapter::Evaluate` from CornEmitter tracks
    /// (KPPL → `lifeSpanMul`, KPPE → `emissionRateMul`, KPPS → `speedMul`,
    /// KPPC → `color.xyz`, KPPA → `color.w`). The renderer's
    /// `ApplyCornFrameStates` pushes these into the matching
    /// `CornEffectsEmitter`.
    struct CornFrameState {
        i32 emitterId = 0;
        Matrix44f transform = Matrix44f::identity();
        f32 scale = 1.0f;              ///< Engine `m_scale` (avg of row magnitudes pre-strip).
        f32 lifeSpanMul = 1.0f;        ///< KPPL track / static lifeSpan.
        f32 emissionRateMul = 1.0f;    ///< KPPE track / static emissionRate.
        f32 speedMul = 1.0f;           ///< KPPS track / static speed.
        Vector4f color = {1, 1, 1, 1}; ///< .xyz = KPPC, .w = KPPA.
        f32 visibility = 1.0f;         ///< `gateByBoneAncestors(node)`: 0/1 bone-chain gate.
    };
    std::vector<CornFrameState> cornStates;
};

} // namespace whiteout::flakes::renderer::model

// Public re-exports.
namespace whiteout::flakes {
using ::whiteout::flakes::renderer::ParticleEmitterConfig;
using ::whiteout::flakes::renderer::effects::RibbonEmitterConfig;
using ::whiteout::flakes::renderer::model::AttachmentConfig;
using ::whiteout::flakes::renderer::model::CollisionShapeData;
using ::whiteout::flakes::renderer::model::CornEmitterInit;
using ::whiteout::flakes::renderer::model::EventObjectConfig;
using ::whiteout::flakes::renderer::model::FrameState;
using ::whiteout::flakes::renderer::model::GroupAverageRecord;
using ::whiteout::flakes::renderer::model::MaterialData;
using ::whiteout::flakes::renderer::model::MaterialLayerData;
using ::whiteout::flakes::renderer::model::MeshData;
using ::whiteout::flakes::renderer::model::PE1EmitterConfig;
using ::whiteout::flakes::renderer::model::SkeletonData;
using ::whiteout::flakes::renderer::model::SkinWeightData;
using ::whiteout::flakes::renderer::model::TextureData;
using ::whiteout::flakes::renderer::model::VertexInfluence;
} // namespace whiteout::flakes
