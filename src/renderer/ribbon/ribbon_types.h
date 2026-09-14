#pragma once

// ============================================================================
// The ribbon data model: what one element is, what arrives each frame, and
// what leaves for the draw queue.
//
// Three lifetimes, named for their home (SC2_PARTICLE_DESIGN.md R2): an
// ELEMENT is per segment, a STATE is per frame and is transport, a DRAW LIST is
// what BUILD hands back. The fourth home, the DESC, is fixed at load and lives
// in ribbon_desc.h; the fifth, the RUNTIME that persists across frames, is
// ribbon_sc2_runtime.h.
//
// No behaviour here — this header is vocabulary, so the desc, the kernels and
// the emitter can each include what they actually read.
// ============================================================================

#include "ground_query.h"
#include "renderer/ribbon/ribbon_constants.h"
#include "types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::ribbon {

/// The renderer's shared ground query: answers a surface HEIGHT under a point,
/// the grid unless a host registered real terrain through
/// `RenderSettings::SetGroundQuery`. The SC2 legacy integrator sweeps its
/// segments against it (the stand-in for the map colliders the viewer lacks).
/// Declared once in ground_query.h; `ribbon::GroundQuery` stays spellable
/// because that is how actor_eval names it.
using GroundQuery = ::whiteout::flakes::renderer::GroundQuery;

/// @brief One trail element, superset of both families.
///
/// WC3/WoW use the BAKED `top/bot/age` triple exactly as the old RibbonEdge
/// did — the client stores a ring of ages beside a ring of `CGxVertexPCT`
/// pairs, and see RibbonBehavior::headEdgeIsProvisional for the one place the
/// ring's shape is observable. The extremes are derivable from a
/// center/up/extents form, but not float-order equal, and the WC3 path is
/// pixel-golden — so the baked pair stays (RIBBON_SERVICE.md §3.3).
///
/// The SC2 fields mirror the CRibbon segment element (SC2_RIBBON_RE.md §2.1);
/// idle for the WC3 family.
struct RibbonElement {
    Vector3f top = {0, 0, 0};
    Vector3f bot = {0, 0, 0};
    f32 age = 0;

    Vector3f birthPos = {0, 0, 0};
    Vector3f pos = {0, 0, 0};
    Vector3f velocity = {0, 0, 0};
    Vector3f up = {0, 0, 1};
    f32 birthU = 0, deathU = 0;
    Vector3f size3 = {1, 1, 1};
    Vector4f color3[ColorStop::kCount] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
    Vector3f rotation3 = {0, 0, 0};
    f32 invMass = 1.0f;
};

/// @brief Per-frame values sampled from the model's animation tracks.
struct RibbonState {
    Matrix44f transform = Matrix44f::identity();
    f32 above = 20.0f;
    f32 below = 20.0f;
    f32 alpha = 1.0f;
    Vector3f color = {1, 1, 1};
    f32 visibility = 1.0f;
    i32 slot = 0;
    /// Model units → renderer units. `above`/`below` and the desc's `gravity`
    /// are authored in model units while the edges live in renderer ones.
    f32 unitScale = 1.0f;
    /// Texture-coordinate transform: `uv' = (row0, row1) · (u, v, 0, 1)`.
    /// Identity is the no-transform case, so BuildStrip applies it unbranched.
    f32 texAnimRow0[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    f32 texAnimRow1[4] = {0.0f, 1.0f, 0.0f, 0.0f};

    /// Terrain-collision ground query, pushed with the frame (the grid unless a
    /// host registered terrain). The SC2 legacy integrator sweeps segments
    /// against it when the emitter's terrain-collision flag is set; empty ⇒ no
    /// collision, so the WC3 family and non-colliding SC2 ribbons ignore it.
    GroundQuery groundQuery;

    /// @brief SC2 per-frame sampled block (`RIB_`/`SRIB` tracks), mirror of
    ///        `FrameState::RibbonFrameState::sc2`. Inert for the WC3 family.
    struct Sc2 {
        f32 speed = 0;
        f32 yawDeg = 0, pitchDeg = 0; ///< Degrees; the head kernel converts.
        f32 lifetime = 1.0f;
        f32 maxLength = 0;
        Vector3f size3 = {1, 1, 1};
        Vector4f color3[ColorStop::kCount] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
        Vector3f rotation3 = {0, 0, 0}; ///< Twist keys, radians.
        bool active = true;             ///< Gates NEW segments, never the trail.
        /// Spline block (Family::Sc2 + hasSpline). `splineNodeTransform` is the
        /// SRIB bone's world matrix (the Bezier end frame); the two yaw/pitch
        /// pairs rotate the start (RIB) and end (SRIB) tangents; the two factors
        /// scale them. All idle for non-spline ribbons.
        Matrix44f splineNodeTransform = Matrix44f::identity();
        f32 velocityBaseFactor = 1.0f, velocityEndFactor = 1.0f;
        f32 splineYawDeg = 0, splinePitchDeg = 0;
        /// Overlay waves (W6). Per-frame amplitude/frequency for the five main
        /// channels (yaw/pitch/speed/size/alpha) and the three spline ones
        /// (yaw/pitch/velocity); `overlayPhase` is the sampled `overlay` track.
        /// The static wave TYPES gate their use and live in the desc. Indexed
        /// by @ref WaveChannel and @ref SplineWaveChannel.
        f32 waveAmp[WaveChannel::kCount] = {0, 0, 0, 0, 0};
        f32 waveFreq[WaveChannel::kCount] = {0, 0, 0, 0, 0};
        f32 overlayPhase = 0;
        f32 splineWaveAmp[SplineWaveChannel::kCount] = {0, 0, 0};
        f32 splineWaveFreq[SplineWaveChannel::kCount] = {0, 0, 0};
        /// flags & 0x10 inherit-parent-velocity scale (0 when not inheriting).
        f32 parentVelocityScale = 0;
    } sc2;
};
/// One centreline sample mid-construction: the displaced world position and
/// frame, then (once fAge is settled) the interpolated scalars. `src` INDEXES
/// the element list so the attribute pass can resample - length mode
/// reparameterises fAge by arc length, so the scalars cannot be sampled until
/// the strip has been measured and trimmed. An index, not a pointer: the live
/// head is a temporary, and a raw pointer to it asked every future reader to
/// keep that in mind.
struct Sc2Node {
    Vector3f pos, tangent, up;
    f32 fAge, twist, size, v;
    Vector4f color;
    usize src;
};
/// @brief Per-frame camera state BUILD needs: billboard and camera-flattened
///        frames expand on the CPU here where retail's VS gets a constant.
///        The WC3 variant ignores it; headless callers pass a default.
struct RibbonBuildContext {
    Vector3f cameraDir = {0, -1, 0};
};

/// @brief One submittable ribbon: a vertex range plus the material state the
///        pipeline needs to pick a PSO. Emitted by the BUILD stage; the
///        service stamps `model`/`emitterId` after the fact.
struct RibbonDrawList {
    u32 model = 0;
    i32 emitterId = 0;
    i32 vertexOffset = 0;
    i32 vertexCount = 0;
    i32 priorityPlane = 0;
    i32 textureId = -1;
    i32 filterMode = 0;
    bool unshaded = false;
    /// The `.m2` adapter fills `RibbonLayer::unfogged` from the material's
    /// flags and `bls_mat_params` honours MAT_UNFOGGED for every other draw;
    /// without this field the bit had nowhere to go, so unfogged WoW ribbons
    /// were fogged (RIBBON_REFACTOR_PLAN.md D8).
    bool unfogged = false;
    bool twoSided = true;
    /// SC2: index into the actor's M3SurfaceTable; -1 routes down the
    /// existing BLS path.
    i32 m3Surface = -1;
    /// Strip head in world space — sort key for the back-to-front transparent
    /// pass, where ribbons interleave with geosets, particles and corn.
    Vector3f worldOrigin = {0, 0, 0};
};
} // namespace whiteout::flakes::renderer::ribbon
