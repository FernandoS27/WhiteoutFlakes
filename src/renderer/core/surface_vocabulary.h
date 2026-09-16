#pragma once

// ============================================================================
// The value vocabulary the surface / pass / profile seams are written in.
//
// Header-only and deliberately behaviour-free: `IShadingModel`'s signature is
// spelled entirely in these types, so they have to exist before the interface
// can be declared at all (REFACTOR_PLAN.md P1b). Nothing here is constructed by
// the renderer yet except `DrawItem`, which mechanically replaces the old
// `OpaqueItem` / `TransparentItem` pair.
//
// Everything is a plain value type. No allocation, no virtuals, no gfx.
// ============================================================================

#include "bls/layer_material.h" // bls::DepthFill
#include "core/debug_view.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::core {

// ---------------------------------------------------------------------------
// Who shades a surface
// ---------------------------------------------------------------------------

// One value per (product, shading path) pair. Products are compile-time
// optional: a build with WDX_ENABLE_M3=OFF simply never registers the M3 ids
// and never sees them in a SurfaceKey, so the enum staying complete costs
// nothing. `None` is the "no model attempted yet" sentinel SurfacePass needs —
// it must not collide with a real id, which is why it is not 0.
enum class ShadingModelId : u8 {
    Wc3Sd = 0,
    Wc3Hd = 1,
    Unlit = 2,
    M2Combiners = 3,
    M3Standard = 4,
    M3Composite = 5,
    D3Standard = 6,

    Count,
    None = 0xFF,
};

// What the core needs to bucket a surface. Deliberately coarser than a filter
// mode: this is the sort axis, not the material.
enum class BlendClass : u8 { Opaque = 0, AlphaKey = 1, Transparent = 2 };

// The per-frame answer to "is this surface drawable right now, and how". WC3
// classification is genuinely animated — a geoset's visibility and its
// opaque-vs-faded state both move with the timeline — so this is a function
// result, never a cached field. See SurfaceKey::blend.
struct SurfaceClass {
    bool visible = false;
    BlendClass blend = BlendClass::Opaque;
    // The WC3 HD fading-opaque geoset's depth twin (RenderGeoset's
    // DEPTHFILL_COLOR). Not a blend class: it is a second draw of the same
    // surface, not a different bucket.
    bool needsDepthFill = false;
    // Actually enqueue that second draw, depth-only, immediately ahead of the
    // colour one — and hoist the pair above every other transparent surface in
    // its priority plane. M2 only: CM2Scene::BeginDraw duplicates a
    // depth-writing transparent batch, forces the copy's blend opaque, and
    // stamps FLT_MAX into the sort key both halves share. WC3 HD sets
    // needsDepthFill alone and does its fade inside the draw.
    bool needsDepthTwin = false;
    // A SECOND draw of this surface, into the distortion buffer rather than the
    // scene — Diablo III's phase-3 pass. Independent of `visible`: 26 of the 45
    // shipped distortion shaders have no scene pass at all, so a surface can be
    // invisible here and still distort. See D3Surface::distortion.
    bool needsDistortion = false;
};

// ---------------------------------------------------------------------------
// Passes
// ---------------------------------------------------------------------------

// The ordered chain a profile declares. Most of these are not per-surface
// concepts at all — see PassMask, which covers only the subset a surface can
// opt into.
enum class PassSlot : u8 {
    ShadowMap = 0,
    DepthPrepass = 1,
    OpaqueColor = 2,
    GBuffer = 3,
    Gtao = 4,
    SceneColorCopy = 5,
    TransparentScene = 6,
    Tonemap = 7,
    Bloom = 8,
    Dof = 9,
    Fxaa = 10,
    Debug = 11,
    ImGui = 12,
    // Appended, never inserted — the values above are baked into recorded
    // baselines. The M3 deferred local-light screen pass: reads the G-buffer
    // sidecar, adds onto SceneColor. Not a per-surface concept, so
    // PassMaskBit maps it to None like every other screen pass.
    DeferredLights = 13,
    /// WoW's refraction particles: a distortion mask, then a full-screen pass
    /// that bends the finished scene through it. Runs after the transparent
    /// scene and before the tonemap, which is where `CWorldSceneRender::Render`
    /// @0x10196d324 calls `RefractionBuffer::Render`. Not a per-surface concept.
    Refraction = 14,
    /// Diablo III's screen-space distortion: the phase-3 draws into a side
    /// buffer of signed screen offsets, then a full-screen pass that bends the
    /// finished scene through it. Runs after the transparent scene and before
    /// the tonemap, where `sub_741760` runs the post-effect chain. Unlike
    /// Refraction this pass DRAWS SURFACES — they are ordinary D3 geometry
    /// running the ordinary D3 programs, only into another target — so it is a
    /// per-surface concept after all, and SurfaceClass::needsDistortion is
    /// where a surface opts in.
    Distortion = 15,

    Count,
};

// Which passes a surface opts into. Replaces a pile of would-be booleans and
// format-specific rules — M2's shadow batches opt into ShadowMap and out of
// OpaqueColor; an M3 unshaded material opts out of GBuffer — so the core reads
// bits instead of branching on the product.
//
// Defined here alongside PassSlot rather than in a later phase: splitting them
// would force whichever phase lands first to invent mask bits for an enum that
// does not exist yet.
enum class PassMask : u8 {
    None = 0,
    ShadowMap = 1 << 0,
    DepthPrepass = 1 << 1,
    OpaqueColor = 1 << 2,
    TransparentScene = 1 << 3,
    GBuffer = 1 << 4,
    // Diablo III's distortion buffer. NOT part of `Default`, and not something a
    // surface carries from load time either: the collector stamps it on the
    // second DrawItem it emits for a surface whose material declares a phase-3
    // pass. So the bit says "this item is the distortion draw", which is what
    // SurfacePass needs to let it through.
    Distortion = 1 << 5,

    // What a WC3 surface opts into today: it casts a shadow and draws in
    // whichever scene pass its blend class routes it to. Classification, not
    // this mask, decides opaque vs transparent — see SurfaceKey::blend.
    Default = ShadowMap | OpaqueColor | TransparentScene,
};

inline constexpr PassMask operator|(PassMask a, PassMask b) {
    return static_cast<PassMask>(static_cast<u8>(a) | static_cast<u8>(b));
}
inline constexpr PassMask operator&(PassMask a, PassMask b) {
    return static_cast<PassMask>(static_cast<u8>(a) & static_cast<u8>(b));
}
inline constexpr bool Any(PassMask m) {
    return static_cast<u8>(m) != 0;
}

// PassSlot → PassMask. Only the five per-surface slots have a bit; everything
// else (Tonemap, Bloom, ImGui, …) maps to None, so `Any(mask & Bit(slot))` is
// automatically false for the passes a surface has no say in.
inline constexpr PassMask PassMaskBit(PassSlot slot) {
    switch (slot) {
    case PassSlot::ShadowMap:
        return PassMask::ShadowMap;
    case PassSlot::DepthPrepass:
        return PassMask::DepthPrepass;
    case PassSlot::OpaqueColor:
        return PassMask::OpaqueColor;
    case PassSlot::TransparentScene:
        return PassMask::TransparentScene;
    case PassSlot::GBuffer:
        return PassMask::GBuffer;
    case PassSlot::Distortion:
        return PassMask::Distortion;
    default:
        return PassMask::None;
    }
}

// Which render targets a shader writes in a given pass. SC2 resolves a 9-bit
// version of this from the pass index; we already carry a one-bit version as
// RenderState::depthWrite doubling as the WC3_IS_MRT permutation axis
// (bls_permuter.h), so this generalises a hack rather than adding a concept.
enum class EmitMask : u16 {
    None = 0,
    Color = 1 << 0,
    Depth = 1 << 1,
    LinearDepth = 1 << 2,
    Normal = 1 << 3,
    Diffuse = 1 << 4,
    Specular = 1 << 5,
    SpecPower = 1 << 6,
    AmbientOcclusion = 1 << 7,
    Emissive = 1 << 8,

    // A plain forward colour draw with depth write: what every WC3 surface
    // emits outside the HD G-buffer pass.
    DefaultColor = Color | Depth,
};

inline constexpr EmitMask operator|(EmitMask a, EmitMask b) {
    return static_cast<EmitMask>(static_cast<u16>(a) | static_cast<u16>(b));
}
inline constexpr EmitMask operator&(EmitMask a, EmitMask b) {
    return static_cast<EmitMask>(static_cast<u16>(a) & static_cast<u16>(b));
}
inline constexpr bool Any(EmitMask m) {
    return static_cast<u16>(m) != 0;
}

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------

// What the core needs to bucket and sort one drawable, and nothing more.
struct SurfaceKey {
    ShadingModelId model = ShadingModelId::Wc3Sd; // who binds this
    u32 surface = 0;                              // index into that model's table
    // Load-time hint only, used to pre-reserve buckets. `Classify` is the sole
    // authority at draw time, because WC3 classification is animated: a geoset
    // is "currently visible" and an HD opaque layer has "faded below full".
    // Never read this to decide which list a draw goes in.
    BlendClass blend = BlendClass::Opaque;
    i32 priorityPlane = 0;
    i16 sortOrder = 0;
    PassMask passes = PassMask::Default;
};

// The vertex buffers a draw can bind. Base is WC3's fully-interleaved
// `Vertex` {position, normal, color, uv} at sizeof == 48 — NOT a position
// stream. BaseUv1 is its twin, a second complete copy differing only in which
// UV set is baked into `uv` (PickSlot0Vb selects between them per layer by
// coordId). Tangent and Bone are the two real siblings.
//
// So this enum names four buffers that already exist. De-interleaving Base
// would mean changing every VertexLayoutKind, every input layout, every PSO and
// the BLS shader input signatures — explicitly out of scope for this refactor.
// `Uv` and `Colors` are the standalone streams a non-WC3 surface can ask for;
// nothing WC3 ever requests them.
enum class StreamId : u8 {
    Base = 0,
    BaseUv1 = 1,
    Tangent = 2,
    Bone = 3,
    Uv = 4,
    Colors = 5,

    Count,
};

// Vertex streams and derived data a surface requires. WC3 asks for none of the
// optional ones: its `Vertex` is fully interleaved and stays that way, so
// nothing about its upload path or input layout moves.
struct VertexNeeds {
    bool position = true;
    bool normal = false;
    bool tangent = false;
    u8 uvSets = 0;
    bool colors = false;
    bool boneWeights = false;
    bool generateTangents = false;
};

// One animatable parameter a shading model exposes. Declared here so P1b lands
// the whole vocabulary at once; the `surfaceParams` blob that consumes it
// arrives with the first animated format.
struct SurfaceParamDecl {
    const char* name = nullptr;
    u16 offset = 0;
    u8 componentCount = 1;
};

// ---------------------------------------------------------------------------
// Draws
// ---------------------------------------------------------------------------

// `DrawItem` itself lives in draw_list.h, next to its comparators and the
// RenderableView it points into.

// ---------------------------------------------------------------------------
// Pass context
// ---------------------------------------------------------------------------

class IRenderProfile;

// How UnlitShading lights a surface, chosen by the profile.
//
// Bring-up lighting, and only that: it exists because flat white cannot tell a
// correct vertex-layout description from a wrong one, while anything that reads
// the normal can. Nothing here claims to be WoW's or SC2's real shading.
enum class UnlitLightingModel : u8 { Flat = 0, Lambert = 1, BlinnPhong = 2 };

// What a shading model needs to know about the pass it is being asked to draw
// into. Carries the slot because SC2 resolves a distinct compiled VS/PS *and*
// an output mask from the pass index, so the slot has to reach the PSO key —
// retrofitting that later means revisiting every model and every pass twice.
struct PassContext {
    PassSlot pass = PassSlot::OpaqueColor;
    Matrix44f view = Matrix44f::identity();
    Matrix44f projection = Matrix44f::identity();
    Vector3f cameraPos = {0.0f, 0.0f, 0.0f};
    i32 viewportWidth = 0;
    i32 viewportHeight = 0;
    // The frame this pass belongs to. Filled where view/projection are, for
    // the same reason: which profile is running is a property of the pass,
    // not something a shading model should reach back through the pipeline
    // to rediscover. Null in a pass built without one.
    const IRenderProfile* profile = nullptr;
    // The frame's debug view. A model draws its real shaders unless
    // `debug.debugSurfaces` asks for its debug pixel shader.
    DebugFrame debug;
    DebugTargetInfo debugTarget;
};

} // namespace whiteout::flakes::renderer::core
