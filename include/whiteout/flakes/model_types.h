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

#include <array>
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
    i32 filterMode = 0;     ///< @ref FilterMode.
    i32 rows = 1, cols = 1; ///< Sprite-sheet grid.
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

/// @brief Static description of one `.m2` particle emitter.
///
/// Deliberately not folded into `ParticleEmitterConfig`: the two formats
/// describe genuinely different emitters (M2 has spawn generators, per-particle
/// spread, spin, twinkle, wind and drag; MDX has none of them), and widening
/// the MDX struct with two dozen always-default fields would make every WC3
/// call site carry them. Both translate into the same renderer-side
/// `EmitterDesc`, which is where the formats actually converge.
///
/// Animated values ride through `FrameState::particleStates` as usual.
struct M2ParticleEmitterConfig {
    /// Which generator produces spawn positions. Matches the on-disk
    /// `emitterType`: 1 Plane, 2 Sphere, 3 Spline, 4 Bone.
    enum class Generator : u8 { Plane = 1, Sphere = 2, Spline = 3, Bone = 4 };

    i32 textureId = -1;
    i32 filterMode = 0;     ///< @ref FilterMode.
    i32 rows = 1, cols = 1; ///< Sprite-sheet grid.
    bool unshaded = false;
    bool unfogged = false;

    /// @brief Draw this emitter as a screen-space distortion instead of colour.
    ///
    /// The record's `Refraction` flag, and only when `MultiTexture` is clear —
    /// the loader tests the two together and lets MultiTexture win
    /// (`InitializeLoaded` @0x100f57550). See M2_REFRACTION_DESIGN.md.
    bool refraction = false;

    /// @brief Draw this emitter through the three-texture combiner.
    ///
    /// The record's `MultiTexture` flag. Such an emitter packs THREE 5-bit
    /// texture indices into the one 16-bit field a plain emitter uses whole
    /// (`InitializeLoaded` @0x100f57aa8 splits it into bits 0-4, 5-9 and
    /// 10-14), and its particles carry the two extra scrolling UV sets below.
    /// See M2_MULTITEX_DESIGN.md.
    bool multiTexture = false;

    /// Layers 1 and 2. Valid only when @ref multiTexture is set; @ref textureId
    /// is layer 0 either way.
    i32 textureId2 = -1;
    i32 textureId3 = -1;

    /// The two `CParticleMat` bits the client uses to pick between the four
    /// shared multi-texture shader effects (`SetMaterial` @0x1016a42c0):
    /// three colour layers instead of two, and Mod4 instead of Mod2.
    bool multiTexUse3Colors = false;
    bool multiTexModx4 = false;

    /// @brief The two extra texture layers a refraction (or multi-texture)
    ///        particle carries, in the runtime's own form.
    ///
    /// Each layer draws a random UV origin at birth and scrolls it at a random
    /// rate; the quad's own corner coordinate is then scaled by
    /// @ref multiTexScale. `mid` is the centre of the scroll-rate range and
    /// `range` its half-width, which is exactly how `SetMultiTexParams`
    /// @0x1016a45d0 stores the record's two fixed-point pairs.
    f32 multiTexScale[2] = {0.0f, 0.0f};
    Vector2f multiTexScrollMid[2] = {{0, 0}, {0, 0}};
    Vector2f multiTexScrollRange[2] = {{0, 0}, {0, 0}};

    Generator generator = Generator::Plane;
    i32 boneId = 0;
    Vector3f position{0, 0, 0}; ///< Emitter offset in its bone's space.

    f32 lifeSpan = 1.0f;
    f32 lifespanVariation = 0.0f;
    f32 emissionRateVariation = 0.0f;

    bool hasHead = true;
    bool hasTail = false;
    f32 tailLength = 1.0f;

    // Spawn / motion flags, already decoded from the on-disk word.
    bool modelSpace = false;   ///< Cleared WorldSpace: particles follow the bone.
    bool sortZ = false;
    bool xyQuad = false;
    bool squirt = false;       ///< Burst on the emission-rate track's rising edge.
    bool hemisphereUp = false; ///< Sphere generator: force +Z velocity.
    bool implosionFilter = false;
    bool followPosition = false;
    bool randomEmissionSpacing = false; ///< InheritPosition.
    bool inheritVelocity = false;
    bool lodIgnoreDistance = false;
    f32 inheritVelocityScale = 1.0f;

    // Appearance flags. All of these change how the quad is built rather than
    // where the particle is, so they are read by the geometry builder.
    bool velocityOrient = false;       ///< Align the head quad along the velocity.
    bool inheritBoneScale = false;     ///< Size scales with the emitter bone.
    bool negateSpinRandom = false;     ///< Odd-seeded particles spin the other way.
    bool clampTailToAge = false;       ///< Tail no longer than the particle is old.
    bool offsetHeadBySpin = false;     ///< Push the head quad along its spun up-axis.
    bool unscaledSizeVariation = false; ///< Draw X and Y size jitter separately.
    bool chooseRandomTexture = false;  ///< Random cell when the head track is empty.
    bool randFlipbookStart = false;    ///< Random per-emitter cell offset, drawn at load.

    /// 2D billboard rotation, radians. `baseSpin` is the angle at birth and
    /// `spinSpeed` the rate; each has a symmetric per-particle variation drawn
    /// from the particle's own seed.
    f32 baseSpin = 0.0f, baseSpinVariation = 0.0f;
    f32 spinSpeed = 0.0f, spinSpeedVariation = 0.0f;

    /// Twinkle: particles blink and pulse in size on a shared 128-entry random
    /// table indexed by `(seed + age*speed) & 0x7F`. `twinkleScale` is the
    /// {min, max} of the size multiplier — note that a record asking for
    /// {0,0} really does scale its particles to nothing.
    f32 twinkleSpeed = 0.0f;
    f32 twinklePercent = 1.0f;
    Vector2f twinkleScale{1, 1};

    f32 drag = 0.0f;
    Vector3f windVector{0, 0, 0};

    /// Two (speed, scale) sample points defining the FollowPosition factor as a
    /// line in emitter speed. Kept in the record's own form; the runtime slope
    /// and bias are derived once when the desc is built.
    f32 followSpeed1 = 0.0f, followScale1 = 0.0f;
    f32 followSpeed2 = 0.0f, followScale2 = 0.0f;

    std::vector<Vector3f> splinePoints;

    /// @brief The `.m2` each particle *is*, instead of a billboard quad.
    ///
    /// From the record's `particleModelFilename`, or `#<fileDataID>` when the
    /// GPID chunk names it instead. Empty for a billboard emitter, which is
    /// nearly all of them.
    std::string geometryModelPath;

    /// @brief The `.m2` whose emitters trail every particle of this one.
    ///
    /// From the record's `childEmittersModelFilename`, or `#<fileDataID>` when
    /// the RPID chunk names it instead. Eight emitters in five models carry it.
    /// Unrelated to @ref geometryModelPath — see M2_TRAIL_EMITTER_DESIGN.md.
    std::string recursionModelPath;

    /// @brief The animated tracks sampled at the start of sequence 0.
    ///
    /// For an emitter nothing ever animates. A trail emitter belongs to a model
    /// the scene never places, so the client's per-frame `AnimateParticleST`
    /// never reaches it and it runs on whatever the loader left in its
    /// generator. Every shipped trail record has a single key on each of these,
    /// so "first key" and "animated" are the same values.
    struct InitialAnimatedState {
        f32 emissionRate = 0.0f;
        f32 speed = 0.0f;
        f32 variation = 0.0f;
        f32 coneAngle = 0.0f;
        f32 horizontalRange = 0.0f;
        f32 width = 0.0f;
        f32 length = 0.0f;
        f32 zSource = 0.0f;
        f32 lifeSpan = 1.0f;
        Vector3f gravityVector{0, 0, 0};
    } initial;

    /// @brief Per-particle angular velocity, radians/s, drawn between the two
    ///        (the record's `tumble` box). Model particles only — a billboard
    ///        spins in 2D off @ref baseSpin / @ref spinSpeed instead.
    Vector3f tumbleMin{0, 0, 0};
    Vector3f tumbleMax{0, 0, 0};

    /// Lifetime tracks over normalised particle age, already decompressed.
    /// Times are in [0,1]; a single key means "constant".
    std::vector<f32> colorTimes;
    std::vector<Vector3f> colorValues; ///< 0..1, already converted from 0..255.
    std::vector<f32> alphaTimes;
    std::vector<f32> alphaValues;
    std::vector<f32> scaleTimes;
    std::vector<Vector2f> scaleValues;
    Vector2f scaleVariation{0, 0};
    std::vector<f32> headCellTimes;
    std::vector<f32> headCellValues;
    std::vector<f32> tailCellTimes;
    std::vector<f32> tailCellValues;

    i32 priorityPlane = 0;
};

} // namespace whiteout::flakes::renderer

namespace whiteout::flakes::renderer::effects {

/// @brief Static description of one MDX ribbon emitter. Per-frame state
///        (above/below/alpha/colour) flows through
///        `FrameState::ribbonStates`.
/// @brief One draw pass over a ribbon's strip.
///
/// A `.m2` ribbon is layered: `CRibbonEmitter::Render` draws the SAME triangle
/// strip once per `CRibbonMat`, rebinding texture and blend state each pass.
/// MDX has one pass, so WC3 fills exactly one of these.
struct RibbonLayer {
    i32 textureId = -1;
    i32 filterMode = 0; ///< @ref FilterMode.
    bool unshaded = false;
    bool unfogged = false;
    bool twoSided = true;
};

struct RibbonEmitterConfig {
    i32 textureId = -1;
    i32 filterMode = 0;     ///< @ref FilterMode.
    i32 rows = 1, cols = 1; ///< Sprite-sheet grid.
    bool unshaded = false;
    bool twoSided = true;
    f32 emission = 10.0f; ///< Segments-per-second emission rate.
    f32 life = 1.0f;      ///< Segment lifetime in seconds.
    f32 gravity = 0.0f;

    i32 priorityPlane = 0;

    /// Draw passes, outermost first. Empty means "one pass from the scalar
    /// fields above" — the MDX shape, kept so the WC3 path is untouched.
    std::vector<RibbonLayer> layers;
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
    std::string id; ///< Event id (e.g. "SPLBhfoo"); looked up via @ref FindSpn etc.
    i32 nodeIndex = -1;

    Vector3f pivot = {0, 0, 0};
    u32 globalSequenceId = 0xFFFFFFFFu; ///< Sentinel: not a global sequence.
    std::vector<u32> eventTrackTimes;   ///< Sample times in ms when the event fires.

    /// @brief The sequence this config belongs to, or `-1` for "every sequence".
    ///
    /// MDX and M2 events sit on one model-wide track that the active sequence's
    /// window slices, so they leave this at `-1` and behave as they always did.
    /// StarCraft II authors events *inside* a sub-track container, and every
    /// sequence's tracks restart at 0 — so without this an Attack container's
    /// footstep fires while the model is walking, because both windows begin at
    /// the same millisecond.
    i32 sequenceIndex = -1;
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

/// @brief What one attribute in a @ref MeshBuffer means.
///
/// Maps to a shader semantic name, not to a slot: the renderer pairs
/// `{semantic, semanticIndex}` with the element names its input layouts
/// use ("POSITION", "NORMAL", "TEXCOORD0", …).
enum class VertexSemantic : u8 {
    Position,
    Normal,
    Tangent,
    TexCoord,
    Color,
    BoneIndices,
    BoneWeights,
};

/// @brief One attribute inside a @ref MeshBuffer's interleaved record.
struct VertexAttribute {
    VertexSemantic semantic = VertexSemantic::Position;
    u8 semanticIndex = 0;
    gfx::Format format = gfx::Format::Unknown;
    u16 offset = 0; ///< Byte offset within one vertex record.
};

/// @brief Vertex data an adapter hands over already GPU-ready.
///
/// The second shape geometry can reach the renderer in. `.m2` and `.m3`
/// both store a single interleaved blob that is already a valid vertex
/// buffer; decoding it into parallel arrays only to re-interleave it into
/// a different layout loses every attribute the target layout has no slot
/// for. So: `attributes` *describes* `data` and the renderer uploads it
/// verbatim, never decoding it.
///
/// Empty means "use the parallel-array path", which is every MDX mesh.
struct MeshBuffer {
    std::vector<u8> data;
    u32 stride = 0;
    std::vector<VertexAttribute> attributes;

    bool Valid() const {
        return stride != 0 && !attributes.empty() && !data.empty() &&
               data.size() % stride == 0;
    }
    u32 VertexCount() const {
        return stride ? static_cast<u32>(data.size() / stride) : 0;
    }
};

/// @brief One geoset of mesh data — vertex streams + index buffer.
///
/// Each MDX geoset becomes one `MeshData`; multiple LODs of the same
/// geoset all share `geosetId` and differ by `lod`. The `tangents` field
/// is `Vector4f` so the bitangent sign rides along in `w`.
struct MeshData {
    i32 geosetId;
    i32 materialId;

    u32 lod = 0; ///< LOD index (0 = highest detail).
    std::vector<Vector3f> positions;
    std::vector<Vector3f> normals;
    std::vector<Vector2f> uvs;
    std::vector<Vector2f> uvs1;     ///< Optional second UV channel (HD models).
    std::vector<Vector4f> tangents; ///< xyz = tangent, w = bitangent sign.
    std::vector<u32> indices;

    /// @brief Additional standalone UV sets, beyond the two `uvs`/`uvs1`
    ///        channels the interleaved WC3 vertex bakes in. Requested via
    ///        @ref VertexNeeds::uvSets; empty for every WC3 model, which
    ///        needs no standalone stream at all.
    ///
    ///        Additive, not a replacement: `positions`/`normals`/`uvs` still
    ///        feed the interleaved base buffer exactly as before.
    std::array<std::vector<Vector2f>, 5> uvSets;
    /// @brief Optional standalone per-vertex colour stream (BGRA8), for
    ///        formats whose colours are not folded into the base vertex.
    std::vector<u32> colors;

    /// @brief Already-interleaved, already-GPU-ready vertex data. When
    ///        valid it replaces the arrays above as the *upload* source.
    ///
    /// `positions` stays populated alongside it and is not redundant: it is
    /// the CPU-side copy, and two things read it that never touch the GPU
    /// buffer — @ref IModelSource::GetBounds's union-over-positions default,
    /// and the per-geoset sort centroid. Both adapters that fill `baked`
    /// already decode positions anyway, so it costs nothing; recovering them
    /// out of the blob instead would be the renderer decoding a buffer it
    /// just promised not to decode.
    MeshBuffer baked;
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
    u32 wrapFlags = 0x3; ///< Bit0 = repeat-U, Bit1 = repeat-V.
    /// @brief Decode this file into a cubemap. StarCraft II's environment
    ///        layer is the only user; see AssetManager's kTextureCubeSubKind
    ///        for why the request has to travel with the reference.
    bool cubeMap = false;

    std::string sharedKey;
};

/// @brief One blend-stage of a material.
///
/// SD models use only `textureId` + `alpha` + `filterMode`; HD models
/// add the PBR slots (`normalMapId`, `ormMapId`, `emissiveMapId`,
/// `teamColorMapId`) plus fresnel coefficients and per-layer emissive gain.
struct MaterialLayerData {
    i32 filterMode; ///< @ref FilterMode.
    i32 textureId;
    f32 alpha;
    i32 flags;                   ///< @ref MaterialFlags.
    i32 textureAnimationId = -1; ///< `-1` if none.

    i32 coordId = 0;  ///< Which UV channel to sample (0 or 1).
    i32 shaderId = 0; ///< HD shader-permutation selector.

    i32 normalMapId = -1;
    i32 ormMapId = -1;
    i32 emissiveMapId = -1;
    i32 teamColorMapId = -1; ///< Or @ref kHdTeamColorActive for synth.

    f32 emissiveGain = 0.0f; ///< Multiplier on the emissive contribution.
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
    /// @brief Per-vertex influences, when the source hands them over decoded.
    ///
    /// Left empty by a format that already carries `BoneWeights` /
    /// `BoneIndices` inside its baked vertex blob — see
    /// @ref paletteLocalVertexIndices. The loader builds the separate
    /// `BoneVertex` stream only when this is populated.
    std::vector<VertexInfluence> influences;
    std::vector<GroupAverageRecord> groupAverages;
    /// Bones-this-geoset-actually-touches subset (matrix-palette index ⇒
    /// global node index); enables a smaller per-geoset bone palette.
    std::vector<i32> subsetNodeIndices;

    /// @brief The vertex stream's bone indices are already palette-local
    ///        slots into @ref subsetNodeIndices, and must not be rewritten.
    ///
    /// `.m3` stores a vertex's bone as an index into its region's window of
    /// `MODL.boneLookup`, which is precisely a per-geoset palette — the format
    /// ships pre-coalesced for exactly this. Such a geoset has to stay on the
    /// per-geoset palette path: the per-actor path re-addresses every index to
    /// a global node slot, and with the indices living in a blob nobody
    /// decodes, that rewrite cannot happen and the vertices would read another
    /// region's bones.
    bool paletteLocalVertexIndices = false;
};

/// @brief Shape encoding of @ref CollisionShapeData::type, mirroring MDX's
///        CLID chunk (`whiteout::mdx::CollisionShape::ShapeType`).
enum class CollisionShapeType : i32 {
    Box = 0,      ///< vertices[0/1] are two opposite corners.
    Plane = 1,    ///< vertices[0/1] are two opposite corners of a quad.
    Sphere = 2,   ///< vertices[0] is the centre, `radius` the radius.
    Cylinder = 3, ///< vertices[0/1] are the end-cap centres, `radius` the radius.
    /// `hullPoints` + `hullEdges`; `vertices`/`radius` unused. Not an MDX kind —
    /// StarCraft II's rigid bodies are convex hulls almost exclusively, and the
    /// bounding box of one is not a picture of what the solver collides with.
    Hull = 4,
    /// `vertices[0/1]` are the two **cap centres** and `radius` the radius, so
    /// the shape reaches `radius` past each of them. Also not an MDX kind, and
    /// deliberately not folded into @ref Cylinder: both `.phys` and `PHSH`
    /// author far more capsules than anything else, and a flat-capped tube
    /// drawn over one hides a radius of reach at either end — the exact margin
    /// that decides whether a collider covers the limb it drives.
    Capsule = 5,
};

/// @brief Per-actor collision primitive (used for ground-clamp /
///        attack-event hit-tests on the engine side; visualised as
///        wireframes when @ref DisplayFlags::showCollisions is on).
///
/// Vertices are in the space that the shape's node matrix
/// (@ref FrameState::collisionTransforms) maps to model space — i.e. the pivot
/// is already folded in, so consumers transform them directly. Sources are
/// responsible for normalising to that: MDX authors Box/Plane/Cylinder corners
/// *relative* to the pivot but a Sphere's centre in model space, and
/// MdxModelAdapter reconciles the two on load.
/// @brief How a shape's owning rigid body is driven, for @ref
///        CollisionShapeData::bodyKind.
///
/// `None` is a plain collision primitive with no body behind it — every MDX
/// CLID shape — and is what the *Collision Markers* overlay draws. The other
/// three belong to a physics rig and get their own overlay, one toggle and
/// colour per kind, because "which of these is the solver allowed to move" is
/// the first question asked of a rig that misbehaves.
enum class CollisionBodyKind : i32 {
    None = 0,      ///< Not a physics body.
    Static = 1,    ///< Never moves. No `.phys` version encodes one.
    Kinematic = 2, ///< Animation-driven; the solver reads it but never writes it.
    Dynamic = 3,   ///< Simulated, and written back over the animation.
};

struct CollisionShapeData {
    i32 type; ///< @ref CollisionShapeType.
    Vector3f vertices[2];
    f32 radius;
    Vector3f pivot = {0, 0, 0}; ///< Node pivot, informational — already applied.
    i32 bodyKind = 0;           ///< @ref CollisionBodyKind, as authored.
    /// @brief Which rigid body owns this shape — an index into
    ///        @ref FrameState::physicsBodyDynamic, or -1 for none.
    ///
    /// @ref bodyKind is what the file says at rest, and for StarCraft II that is
    /// only the opening frame: a body's type is a channel. The shape list is
    /// per-template and built once, so the *current* type has to be looked up
    /// per frame, and this is the key it is looked up by.
    i32 bodyIndex = -1;
    /// @name Convex hull (@ref CollisionShapeType::Hull only)
    /// Vertices, and index pairs into them, in the same space as @ref vertices.
    /// @{
    std::vector<Vector3f> hullPoints;
    std::vector<u16> hullEdges;
    /// @}
};

/// @brief One `PHCC` capsule a cloth collides against.
///
/// A **tapered** capsule along its own local +X — two end radii and a full
/// length — which is not a shape @ref CollisionShapeType::Capsule can carry,
/// and not a rigid body either: a cloth collider belongs to no `PHRB` and never
/// appears in the physics overlay. It is also the first thing to look at when a
/// cape passes through a shoulder, so it gets drawn.
struct ClothColliderData {
    i32 node = -1;   ///< Palette node it rides; -1 = model space.
    /// The collider's frame in that node's space, composed from the rotation
    /// and position the solver holds rather than from the authored matrix —
    /// the two differ by a scale the client re-applies, and the solver's is the
    /// one being drawn.
    Matrix44f local = Matrix44f::identity();
    f32 radius0 = 0.0f; ///< At the -X end.
    f32 radius1 = 0.0f; ///< At the +X end, which is why it is not a capsule.
    f32 length = 0.0f;  ///< Between the two end centres, in full.
};

/// @brief One cloth's debug wireframe: the particles, what holds them together,
///        and what they hang off.
///
/// Needs no per-frame channel of its own. A cloth's particles are appended to
/// the skinning palette after the real skeleton and each one carries a full
/// output frame (`sc2_cloth.h`), so a particle's live position is its node's
/// origin and the overlay reads it out of the same array the shader does.
struct ClothOverlayData {
    /// Palette node per particle, in the solver's particle order.
    std::vector<i32> particleNodes;
    /// Index pairs into @ref particleNodes, one per distance constraint. This
    /// is the picture: a cloth with its edges drawn shows where it is stretched
    /// and where it has collapsed, neither of which the skinned mesh admits to.
    std::vector<u16> links;
    /// Particles `[0, pinnedCount)` are pinned. The build reorders them first,
    /// and they are the ones the animation drives rather than the simulation —
    /// a cloth that hangs off the wrong ones is the common authoring failure.
    u32 pinnedCount = 0;
    std::vector<ClothColliderData> colliders;
    /// Index into @ref FrameState::clothActive, or -1 when the cloth is always
    /// on. A cloth whose channel is off is not stepped at all, which from the
    /// outside is indistinguishable from one that failed to build.
    i32 activeIndex = -1;
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
    /// Per-geoset "this submesh is not part of the model right now" — a
    /// character's unchosen hairstyles, the hand a glove replaces.
    ///
    /// Not the same thing as `geosetAlphas[i] == 0`, and it cannot be expressed
    /// as one: an additive batch draws at zero model alpha on purpose (it is
    /// premultiplied, and the client fades a model without dropping the light it
    /// adds), so an eye-glow geoset "hidden" by alpha stays on screen. The
    /// client's own `SetGeometryVisible` takes the submesh out of the draw list
    /// instead, which is what this is.
    std::vector<u8> geosetHidden;
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

        // ---- `.m2` additions ----
        //
        // Defaulted so the MDX adapter is untouched and its emitters keep their
        // existing meaning. `coneAngle` carries the vertical range and
        // `width`/`length` the two area floats, which is the client's own reuse
        // rather than an economy taken here.

        /// Vector gravity. M2 animates a direction, not just a magnitude; the
        /// MDX path leaves this zero and keeps using the scalar `gravity`.
        Vector3f gravityVector{0, 0, 0};
        bool hasGravityVector = false;

        /// Animated lifespan. Re-read every frame for every live particle under
        /// the WoW dialect, so a keyframe resizes particles already in flight.
        f32 lifeSpan = 0.0f;
        /// Azimuth range, animated separately from the polar range.
        f32 horizontalRange = 0.0f;
        /// Velocity aims from (0,0,zSource) once above 0.001.
        f32 zSource = 0.0f;
        /// Model alpha. Multiplies particle alpha rather than gating emission.
        f32 modelAlpha = 1.0f;
        /// The `enabledIn` track. Drives emission; distinct from `visibility`,
        /// which decides whether the emitter is simulated at all.
        bool enabled = true;
        /// Emitter position in world space, for path-interpolated spawning and
        /// inherited emitter velocity.
        Vector3f worldPosition{0, 0, 0};
        /// Renderer units per model unit for this actor (`Actor::worldScale`,
        /// 100 for WoW). Particle sizes and forces are authored in model units
        /// while the emitter draws in renderer ones. MDX leaves it 1.
        f32 unitScale = 1.0f;
        /// This emitter spawns off the skeleton, so it needs @ref
        /// FrameState::boneSpawnTable rather than an area on its own bone.
        bool boneGenerator = false;
        /// This emitter's particles are models, not quads, so it lives in the
        /// service's ChildModel id space rather than the Billboard one.
        bool modelParticle = false;
    };
    std::vector<ParticleFrameState> particleStates;

    /// @brief One bone of the bone-emitter table: a segment to spawn along and
    ///        the plane to scatter around it.
    ///
    /// Per *model*, not per emitter — the client builds one table and every
    /// bone generator on the model picks out of it
    /// (`CBoneGeneratorBase::CreateBoneEmitterTable` @0x10169d1a0).
    struct BoneSpawn {
        Vector3f pos{0, 0, 0};       ///< The bone's own position, model space.
        Vector3f parentPos{0, 0, 0}; ///< Its parent's; equal to @ref pos when it has none.
        Vector3f axisA{1, 0, 0};     ///< Bone direction; the scatter plane's first axis.
        Vector3f axisB{0, 0, 1};     ///< A perpendicular; the plane's second axis.
        bool hasParent = false;
    };
    /// Filled only when some emitter reports @ref ParticleFrameState::boneGenerator.
    std::vector<BoneSpawn> boneSpawnTable;

    /// @brief Per-ribbon sampled state. `slot` is the texture slot
    ///        selected by KRTX tracks (0..3 typically).
    struct RibbonFrameState {
        i32 emitterId;
        Matrix44f transform;
        f32 above, below, alpha;
        Vector3f color;
        f32 visibility;
        i32 slot;
        /// Renderer units per model unit, as on @ref ParticleFrameState. The
        /// simulation runs on positions the transform has already scaled, so
        /// `above`/`below`/`gravity` — authored in model units — need the same
        /// factor. MDX leaves it 1.
        f32 unitScale = 1.0f;
        /// The emitter's texture-coordinate transform, in @ref TexAnimMatrix's
        /// form: `uv' = (row0, row1) · (u, v, 0, 1)`. Identity when the record
        /// names no transform, which is every MDX ribbon and most `.m2` ones.
        f32 texAnimRow0[4] = {1.0f, 0.0f, 0.0f, 0.0f};
        f32 texAnimRow1[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    };
    std::vector<RibbonFrameState> ribbonStates;

    /// Per-collision-shape node transforms, in model space — unlike the
    /// emitter/attachment states these do *not* bake the actor's
    /// worldTransform, so consumers must post-multiply it themselves.
    std::vector<Matrix44f> collisionTransforms;

    /// @brief Per-rigid-body "simulate me this frame", one entry per body in
    ///        file order. Empty for a format whose bodies do not animate it.
    ///
    /// StarCraft II authors a body's type as a *channel*, not a constant: a
    /// hero's collision proxies are animation-driven until a sequence keys them
    /// dynamic and the model goes limp. Only the sampler can read that channel —
    /// it needs the layer stack, and a pose stage sees a finished `FrameState` —
    /// so the source samples it here and the physics stage flips body types from
    /// what it finds.
    ///
    /// Indexed by body, not by bone: a body names its bone and not the reverse,
    /// and two bodies can share one.
    ///
    /// The source writes the *sampled channel*; the physics stage then resolves
    /// it in place, so what a consumer reads after the pose stages have run is
    /// the type each body actually has this frame — inherit chains followed,
    /// static bodies pinned, bodies with no link left alone.
    std::vector<u8> physicsBodyDynamic;

    /// @brief Per-cloth "simulate me this frame", one entry per `PHCL` in file
    ///        order. Empty for a format whose cloths do not animate it.
    ///
    /// The same shape as @ref physicsBodyDynamic and for the same reason: only
    /// the source has the layer stack the channel is sampled against. 69 of the
    /// corpus's 392 cloth records key it, and in StarCraft II it gates the whole
    /// write-back — an inactive cloth stops feeding the mesh entirely rather
    /// than simulating into a hidden buffer.
    std::vector<u8> clothActive;

    /// @brief Per-layer 2D texture-coord transform (offset / tile / rotation).
    struct TexAnimState {
        i32 materialId;
        i32 layerIndex;
        f32 uOff, vOff, uTile, vTile;
        f32 rotation;
    };
    std::vector<TexAnimState> texAnims;

    /// @brief MDX light type. Mirrors `mdx::Light::LightType`; only `Omni` is
    ///        positional — WC3 drives both `Directional` and `Ambient` through
    ///        its directional light path (`CreateLight_1`: omni iff type == 0).
    enum class LightKind : u8 { Directional = 0, Omni = 1, Ambient = 2 };
    /// @brief Per-MDX-light sampled state (KLAC / KLAI / KLBC / KLBI tracks).
    struct LightState {
        LightKind kind = LightKind::Directional;
        Vector3f worldPos = {0, 0, 0};
        Vector3f worldDir = {0, 0, -1};
        /// @brief KLAC colour premultiplied by KLAI intensity, shader-ready.
        Vector3f diffuse = {0, 0, 0};
        /// @brief Specular colour x multiplier, shader-ready. `.m3` lights
        ///        carry their own pair (LITE specularColor/specularMultiplier)
        ///        and SC2's deferred pass tints the highlight with it rather
        ///        than with `diffuse` (deferredlight.fx:220). MDX has no
        ///        equivalent, so WC3 leaves this zero with @ref useSpecular
        ///        false and the highlight is skipped, as it is today.
        Vector3f specular = {0, 0, 0};
        /// @brief The light contributes a specular highlight at all —
        ///        m3::LightFlag::Specular.
        bool useSpecular = false;
        /// @brief KLBC ambient colour, clamped 0..1. Only reaches the shader
        ///        through the SD-on-HD compensation term; see BuildLightPalette.
        Vector3f ambientColor = {0, 0, 0};
        f32 dirIntensity = 0.0f;
        f32 ambIntensity = 0.0f;
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

    /// @brief Per-surface animated material values.
    ///
    /// The animated twin of whatever the actor's `ISurfaceTable` holds as a
    /// load-time constant, indexed by the same `SurfaceKey::surface`. It exists
    /// because a surface is finer than a geoset: an `.m2` submesh carries N
    /// batches, each with its own colour, alpha and per-texture-unit weight
    /// tracks, so `geosetAlphas` cannot express them.
    ///
    /// Warcraft III fills none of this — its per-layer values already have
    /// `layerAlphas` / `layerFresnels`, which are keyed by material rather than
    /// by surface.
    struct SurfaceState {
        i32 surface = 0;
        Vector3f color = {1.0f, 1.0f, 1.0f};
        f32 alpha = 1.0f;
        /// One weight per texture unit; only the shaders that declare the
        /// per-unit weight input read past `[0]`.
        f32 unitWeights[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    };
    std::vector<SurfaceState> surfaceStates;
};

} // namespace whiteout::flakes::renderer::model

// Public re-exports.
namespace whiteout::flakes {
using ::whiteout::flakes::renderer::ParticleEmitterConfig;
using ::whiteout::flakes::renderer::effects::RibbonLayer;
using ::whiteout::flakes::renderer::effects::RibbonEmitterConfig;
using ::whiteout::flakes::renderer::model::AttachmentConfig;
using ::whiteout::flakes::renderer::model::CollisionShapeData;
using ::whiteout::flakes::renderer::model::CollisionShapeType;
using ::whiteout::flakes::renderer::model::CornEmitterInit;
using ::whiteout::flakes::renderer::model::EventObjectConfig;
using ::whiteout::flakes::renderer::model::FrameState;
using ::whiteout::flakes::renderer::model::GroupAverageRecord;
using ::whiteout::flakes::renderer::model::MaterialData;
using ::whiteout::flakes::renderer::model::MaterialLayerData;
using ::whiteout::flakes::renderer::model::MeshBuffer;
using ::whiteout::flakes::renderer::model::MeshData;
using ::whiteout::flakes::renderer::model::PE1EmitterConfig;
using ::whiteout::flakes::renderer::model::SkeletonData;
using ::whiteout::flakes::renderer::model::SkinWeightData;
using ::whiteout::flakes::renderer::model::TextureData;
using ::whiteout::flakes::renderer::model::VertexAttribute;
using ::whiteout::flakes::renderer::model::VertexInfluence;
using ::whiteout::flakes::renderer::model::VertexSemantic;
} // namespace whiteout::flakes
