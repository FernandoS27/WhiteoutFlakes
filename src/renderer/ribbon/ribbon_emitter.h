#pragma once

// ============================================================================
// Ribbon emitter — one trail, simulated the way Blizzard's CRibbonEmitter does.
//
// The WC3 and WoW ribbon runtimes are the same code, evolved. Reverse
// engineering WoW 6.0.1.18179's `Engine/Source/Services/RibbonEmitter.cpp`
// showed its `InitInterpDeltas` / `InterpEdge` pair evaluates *exactly* the
// blend this renderer already ran for MDX — same tangent scaling by
// |curr-prev|, same operand order — and its `Update` emits
// `floor(endTime-1)+1` edges at `t = (n - startTime)/(endTime - startTime)`
// with `age = -dt*t`, which is again what the MDX path did. So this is one
// simulation with a handful of measured divergences, not two simulations.
//
// Those divergences live in core::RibbonBehavior (core/ribbon_dialect.h): each
// field is one difference established from the disassembly, named rather than
// branched on a game enum, and answered by the profile.
// ============================================================================

#include "core/ribbon_dialect.h"
#include "ground_query.h"
#include "renderer/sc2/sc2_element.h"
#include "types.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <functional>
#include <vector>

namespace whiteout::flakes::renderer::ribbon {

using RibbonBehavior = core::RibbonBehavior;

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
    Vector3f tangent = {0, 0, 0};
    f32 birthU = 0, deathU = 0, arcFrac = 0;
    Vector3f size3 = {1, 1, 1};
    Vector4f color3[3] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
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
        Vector4f color3[3] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
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
        /// The static wave TYPES gate their use and live in the desc.
        f32 waveAmp[5] = {0, 0, 0, 0, 0};
        f32 waveFreq[5] = {0, 0, 0, 0, 0};
        f32 overlayPhase = 0;
        f32 splineWaveAmp[3] = {0, 0, 0};
        f32 splineWaveFreq[3] = {0, 0, 0};
        /// flags & 0x10 inherit-parent-velocity scale (0 when not inheriting).
        f32 parentVelocityScale = 0;
    } sc2;
};

/// @brief Static description of one emitter, format-neutral.
///
/// `RibbonEmitterConfig` (the public MDX-shaped type) converts into this via
/// DescFromWc3Config; the M2 adapter fills the same fields from its own record.
struct RibbonDesc {
    f32 edgesPerSecond = 10.0f;
    f32 edgeLifespan = 1.0f;
    f32 gravity = 0.0f;
    i32 rows = 1, cols = 1;

    /// Draw passes over the one strip, outermost first. Carried through to the
    /// draw unit rather than used by the simulation. Always at least one.
    std::vector<RibbonLayer> layers{RibbonLayer{}};
    i32 priorityPlane = 0;

    /// Which family of stage variants runs this emitter. Set at registration,
    /// never re-derived. Wc3 covers WC3 and WoW (RibbonBehavior is the
    /// sub-dialect); the Sc2 stage variants land phase by phase.
    enum class Family : u8 { Wc3, Sc2 };
    Family family = Family::Wc3;

    /// @brief StarCraft II statics (read only when family == Sc2). Field
    ///        semantics per SC2_RIBBON_RE.md; everything animated arrives per
    ///        frame in RibbonState::sc2.
    struct Sc2 {
        /// RIB_'s material ref resolved to an M3SurfaceTable entry at
        /// registration; -1 = unresolved (draws route down the BLS path).
        i32 m3Surface = -1;
        u32 flags = 0;
        u32 additionalFlags = 0; ///< bit 3 = world space.
        u8 ribbonType = 0;       ///< Cross-section: billboard/planar/cylinder/star.
        u8 cullMethod = 0;       ///< 0 time / 1 length.
        u8 simTechnique = 0;     ///< Ribbon_SelectSimTechnique, run once at load.
        f32 divisions = 20.0f;   ///< Authored density (RIB_+0x198).
        i32 edges = 5;
        f32 innerRadius = 0.5f;
        f32 midTime[4] = {0.5f, 0.5f, 0.5f, 0.5f}; ///< size/color/alpha/rotation,
        f32 midHold[4] = {0, 0, 0, 0};             ///< plain floats, ≤ 0.996.
        u8 sizeSmoothing = 0, colorSmoothing = 0;
        f32 drag = 1.0f, mass = 1.0f; ///< Static; drag clamped ≥ 0.01.
        Vector3f gravity3 = {0, 0, 0}; ///< Only .z reaches the analytic path.
        f32 friction = 1.0f, bounce = 0.0f;
        f32 noiseAmplitude = 0, noiseFrequency = 0, noiseCoherence = 0,
            noiseEdge = 1.0f;
        u32 waveTypes[5] = {0, 0, 0, 0, 0}; ///< yaw/pitch/speed/size/alpha.
        u8 lodReduce = 0, lodCut = 0; ///< Table ROW indices, not factors.
        bool hasSpline = false;
        /// SRIB record 0 — the only one the runtime ever reads.
        Sc2SplineRibbonConfig spline;
        f32 maxLengthBound = 0;  ///< Derived at load (catch-up + expiry). W3.
        f32 catchUpSeconds = 0;  ///< Derived at load per RE §3.1. W3.
    } sc2;
};

/// @brief Convert the public MDX-shaped config into the neutral desc.
RibbonDesc DescFromWc3Config(const RibbonEmitterConfig& cfg);

/// @brief `Ribbon_SelectSimTechnique`, verbatim from the truth table oracle
///        O1 pins (`tools/sc2_ribbon_oracle/golden/o1_simtech.json`):
///        0 GPU-only, 1 spline, 2 mixed (length), 3 mixed precomputed
///        tangent, 4 legacy CPU. CPU-side 0/2/3 share one integrator; the id
///        is kept where behaviour differs. Note the forces operand is the
///        FALLBACK pair dword and the noise compare is strictly-greater than
///        float32(0.001) — both settled by the oracle, not by the wiki.
u8 SelectSc2SimTechnique(const Sc2RibbonEmitterConfig& cfg);

/// @brief Convert the public SC2 config into a Family::Sc2 desc — including
///        the load-time technique derivation. `m3Surface`/`priorityPlane` are
///        stamped by the loader, which owns the surface table.
RibbonDesc DescFromSc2Config(const Sc2RibbonEmitterConfig& cfg);

/// @brief The animated vertex fillers' noise offset (SC2_RIBBON_RE §4.3, gate
///        O13) at trail parameter @p t.
///
/// The particle system's seed-0 table sampled in 3-D at `(t·frequency,
/// coherence·headU, {0, 0.33, 0.66})`, times an amplitude muted by `t/edge`
/// below the edge — or, only on a spline, by `(1−t)/edge` above `1 − edge`.
/// The two mutes are alternatives, never a product, and an edge of 0 mutes
/// nothing. Returned in the ribbon's own space.
Vector3f Sc2NoiseDisplacement(f32 t, f32 headU, f32 amplitude, f32 frequency, f32 coherence,
                              f32 edge, bool spline);

// ---- SC2 time-mode stage kernels (RIBBON_SERVICE_PLAN.md W3) ----------------
// Pure reproductions of the shipped CPU stages, replayed at their stated
// tolerances against the Unicorn goldens: the emit clock (O3 `UpdateEmit`), the
// head-element writer (O4 `UpdateHeadSegment`, emitter-LOCAL subset), and the
// pre-roll clock (O5 `CatchUpEmission`). The emitter's stage methods and the
// oracle-replay test call this SAME code, so a divergence shows up as a red
// gate rather than a silent drift.
namespace sc2 {

/// UpdateEmit's mutable clock state (the CRibbon fields it carries frame to
/// frame). renderFlags bit 1 is the "emitting" flag.
struct EmitClock {
    f32 dtAccumulator = 0; ///< CRibbon+0x3F0.
    u16 renderFlags = 0;   ///< CRibbon+0x444.
    u8 renderFlagsHi = 0;  ///< CRibbon+0x446.
};

/// Everything UpdateEmit reads that is not in EmitClock.
struct EmitGateInputs {
    bool splinePresent = false;
    bool active = false;      ///< CRibbon activeFlag.
    bool worldReemit = false; ///< RIB_ additionalFlags & 8 (re-emission gates).
    u8 simTechnique = 0;
    bool haveHead = false;           ///< a live head element exists to gate on.
    f32 headU = 0, headBirthU = 0;   ///< head element birthU (byte 112).
    Vector3f headElemPos = {0, 0, 0};///< head element pos (byte 100).
    Vector3f headPos = {0, 0, 0};    ///< CRibbon headPos.
    i32 quality = 0, lodCut = 0, lodReduce = 0;
    u8 cullMethod = 0;
    f32 emissionScale = 1.0f, divisions = 1.0f;
    f32 lifetimeAux = 0, maxLengthAux = 0;
    f32 dt = 0;
    bool sampledActive = true; ///< what the RIB_.active Bool32 sample reports.
    bool nodeActive = true;    ///< transform-node byte & 2.
    u32 elementCount = 0;
};

struct EmitGateResult {
    f32 ret = 0;          ///< 0 no-emit / 1 startBlend / 3 re-activation.
    bool sampled = false; ///< the active sample ran (a period lapsed).
    u32 activeState = 0;
};

/// One UpdateEmit call; mutates `clk`.
EmitGateResult Sc2EmitGate(EmitClock& clk, const EmitGateInputs& in);

/// The head element's launch state in emitter-LOCAL space (identity basis),
/// as UpdateHeadSegment writes it for the W3 subset (local, no inherit, no
/// overlay waves). The world transform is applied later by BUILD.
struct HeadElement {
    Vector3f velocity = {0, 0, 0}; ///< element +16 == +184 in this subset.
    /// The emission direction (YPR row2), local space and pre-speed. The caller
    /// needs it because the 1e-4 stationary floor runs AFTER the world transform
    /// and inherit add, replacing velocity with the (transformed) direction·1e-4
    /// (UpdateHeadSegment step 5, DRIFT-1) — it cannot be applied here.
    Vector3f dir = {0, 0, 1};
    Vector3f up = {0, 0, 1};
    Vector3f size3 = {0, 0, 0};
    Vector3f rotation3 = {0, 0, 0};
    f32 invMass = 1.0f;
    f32 birthU = 0, deathU = 0;
    u32 expireFrameMs = 0;
    /// The alpha overlay wave (0 when no alpha wave); the caller adds it to each
    /// colour stop's alpha and clamps [0,1]. Colours are not the head kernel's
    /// to own, so it hands this back rather than mutating them.
    f32 alphaWave = 0;
};

struct HeadInputs {
    u8 simTechnique = 0;
    u8 ribbonType = 0;
    u8 cullMethod = 0;
    bool swapYawPitch = false; ///< RIB_ flags & 0x8000.
    f32 headU = 0;
    f32 yawDeg = 0, pitchDeg = 0, speed = 0, lifetime = 1.0f;
    Vector3f size3 = {1, 1, 1};
    Vector3f rotation3 = {0, 0, 0};
    f32 mass = 1.0f, maxLengthBound = 0;
    u32 nowMs = 0, prevExpireMs = 0;
    /// Overlay waves (W6). `waveTypes` gate per channel (0 = inert, so the W3
    /// oracle subset that leaves them 0 is byte-identical); `overlayTime` is the
    /// emission clock the phase runs on. yaw/pitch/speed/size ADD to their base;
    /// the alpha wave comes back on the HeadElement.
    u32 waveTypes[5] = {0, 0, 0, 0, 0};
    f32 waveAmp[5] = {0, 0, 0, 0, 0};
    f32 waveFreq[5] = {0, 0, 0, 0, 0};
    f32 overlayPhase = 0, overlayTime = 0;
};

HeadElement Sc2WriteHead(const HeadInputs& in);

/// The overlay-wave sampler, moved to `sc2/sc2_element.h` — it samples an
/// ELEMENT's wave and a `PAR_` needs it as much as a `RIB_` does (R6). Named
/// here so ribbon code keeps spelling it `sc2::SampleWave`; there is one
/// definition.
using ::whiteout::flakes::renderer::sc2::SampleWave;

/// CatchUpEmission's pre-roll duration expressed as 33 ms tick count. `speed`
/// and `lifetime` are the ALREADY-SAMPLED reduced values (min speed / max
/// lifetime) — the track reduction stays out of here per the emulation
/// contract. `earlyOut` is the length-mode too-slow skip (speed < 1e-3).
struct CatchUpResult {
    i32 ticks = 0;
    bool earlyOut = false;
};
CatchUpResult Sc2CatchUpTicks(u8 cullMethod, f32 speed, f32 lifetime,
                              f32 maxLengthBound);

} // namespace sc2

/// @brief Per-frame camera state BUILD needs: billboard and camera-flattened
///        frames expand on the CPU here where retail's VS gets a constant.
///        The WC3 variant ignores it; headless callers pass a default.
struct RibbonBuildContext {
    Vector3f cameraDir = {0, -1, 0};
    Vector3f cameraPos = {0, 0, 0};
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
    bool twoSided = true;
    /// SC2: index into the actor's M3SurfaceTable; -1 routes down the
    /// existing BLS path.
    i32 m3Surface = -1;
    /// Strip head in world space — sort key for the back-to-front transparent
    /// pass, where ribbons interleave with geosets, particles and corn.
    Vector3f worldOrigin = {0, 0, 0};
};

class RibbonEmitter {
public:
    RibbonEmitter() = default;
    RibbonEmitter(const RibbonDesc& desc, const RibbonBehavior& behavior)
        : desc_(desc), behavior_(behavior) {}

    void SetDesc(const RibbonDesc& d) {
        desc_ = d;
    }
    const RibbonDesc& Desc() const {
        return desc_;
    }
    void SetBehavior(const RibbonBehavior& b) {
        behavior_ = b;
    }
    const RibbonBehavior& Behavior() const {
        return behavior_;
    }

    /// @brief Push this frame's sampled pose. Shifts curr → prev; the first
    ///        call seeds both and arms emission (the client's `posSet` flag).
    void SetState(const RibbonState& st);
    const RibbonState& State() const {
        return state_;
    }

    /// @brief One simulation tick. Runs the family's EMIT/MOVE/RETIRE stage
    ///        variants — the WC3 family in the client's own order
    ///        (retire → emit → move).
    void Update(f32 dt);

    /// @brief The BUILD stage: append this emitter's triangles and one draw
    ///        record per pass (WC3: one per layer; SC2: one, with m3Surface).
    ///        Returns the vertex count added; records carry offsets relative
    ///        to `out`'s state at entry and zeroed model/emitterId for the
    ///        caller to stamp.
    i32 BuildStage(const RibbonBuildContext& ctx, std::vector<Vertex>& out,
                   std::vector<RibbonDrawList>& outDraws) const;

    /// @brief Append this emitter's triangles. Returns the vertex count added.
    i32 BuildStrip(std::vector<Vertex>& out) const;

    /// @brief Vertices BuildStrip would append, without building them.
    i32 VertexCount() const;

    const std::vector<RibbonElement>& Edges() const {
        return edges_;
    }
    bool PositionSeeded() const {
        return posSet_;
    }

    /// @brief The lifespan the simulation actually uses — floored or raw per
    ///        RibbonBehavior::lifespanFloorAppliesToSim.
    f32 SimLifespan() const;

    /// @brief Drop the live trail and re-arm seeding, keeping the emitter
    ///        registered. What a rewind needs: emitters are registered once at
    ///        spawn, so RibbonService::Clear would retire them for good.
    void ResetTrail() {
        edges_.clear();
        accumEmission_ = 0;
        posSet_ = false;
        updatedOnce_ = false;
        headPending_ = false;
        sc2HeadU_ = 0;
        sc2EmitAccum_ = 0;
        sc2CaughtUp_ = false;
        sc2SplineAge_ = 0;
        for (Vector3f& s : sc2Sag_)
            s = {0, 0, 0};
        sc2Smoothed_.Reset();
    }

private:
    bool ShouldEmit(f32 dt) const;

    /// Shared per-tick context the stages hand each other. `emittedHead` is
    /// written by EMIT and read by MOVE (a head placed this frame is not aged).
    struct TickCtx {
        f32 dt = 0;
        f32 lifeSpan = 0;
        bool firstTick = false;
        bool emittedHead = false;
    };
    void PrepWc3(TickCtx& t, f32 dt);
    void RetireWc3(const TickCtx& t);
    void EmitWc3(TickCtx& t);
    void MoveWc3(const TickCtx& t);

    // SC2 time-mode stages (RIBBON_SERVICE_PLAN.md W3). The per-element launch
    // state comes from the oracle-gated kernels in ribbon_stages_sc2.cpp; the
    // frame loop follows RIBBON_SERVICE.md §5.1 (design-driven, not
    // oracle-pinned): headU is the emission-time clock advancing by dt, and a
    // segment is committed each `emitPeriod = lifetime / divisions`.
    void PrepSc2(TickCtx& t, f32 dt);
    void EmitSc2(TickCtx& t);
    void MoveSc2(const TickCtx& t);
    /// Advance headU by dt and commit any segments whose interval lapsed
    /// (EmitSegments' headU clock + append). Shared by EmitSc2 and the spawn
    /// pre-roll.
    void Sc2Append(f32 dt);
    /// Drop segments aged past their deathU (Simulate_Type0's retire walk).
    void Sc2Retire();
    /// Legacy per-segment Euler integration (Simulate_Type4, tech 4): advance
    /// each element's velocity/position semi-implicitly under gravity, then
    /// (flags & 0x2) sweep it against the ground query and reflect off the
    /// surface, then apply drag damping — the binary's integrate/collide/drag
    /// order. Collision is the grid stand-in for the map colliders.
    void Sc2LegacyIntegrate(f32 dt);
    /// Push this tick's emitter motion into the 8-tap ring and recompute
    /// `sc2Smoothed_` (inherit-parent-velocity). No-op unless inheriting.
    void Sc2UpdateSmoothedVelocity(f32 dt);
    /// Build one segment at the interpolated emitter pose, birthU-stamped (the
    /// UpdateHeadSegment field write). Used both for committed history and for
    /// the live head strip synthesises each frame.
    RibbonElement Sc2MakeSegment(f32 birthU, f32 fracToCurr) const;
    /// One committed segment at the interpolated emitter pose, birthU-stamped.
    void CommitSc2Segment(f32 birthU, f32 fracToCurr);
    /// Build the SC2 strip: per-element the vertex shader math on the CPU
    /// (drag displacement, camera/planar frame, size/colour interpolation,
    /// V = fAge), then the emitter world transform. Time-mode billboard/planar
    /// (W3); length/tube/spline are W4/W5.
    i32 BuildStripSc2(const RibbonBuildContext& ctx, std::vector<Vertex>& out) const;
    /// Spline tick (Simulate_Spline, RIBBON_SERVICE_PLAN.md W5): advance the
    /// whole-ribbon age clock, accumulate the persistent quadratic sag, loop on
    /// lifetime. A spline has no per-segment emission — the strip is rebuilt
    /// from the four control points every frame — so it replaces the emit/move
    /// stages, not augments them.
    void TickSc2Spline(f32 dt);
    /// Build the SC2 spline strip: one cubic Bezier from the SRIB control
    /// points, sampled at 32 points, expanded through the shared cross-section.
    i32 BuildStripSc2Spline(const RibbonBuildContext& ctx,
                            std::vector<Vertex>& out) const;
    /// emitPeriod's reciprocal (segments per second): the density that keeps
    /// `divisions` segments alive over one lifetime. lodKeepFactor and
    /// emissionScale are 1 at viewer quality (RIBBON_SERVICE.md §5.1).
    f32 Sc2SegmentsPerSecond() const;

    RibbonDesc desc_;
    RibbonBehavior behavior_ = RibbonBehavior::Wc3();
    RibbonState state_;

    std::vector<RibbonElement> edges_;
    f32 accumEmission_ = 0;  ///< The client's `m_startTime`: fractional edge carry.
    bool posSet_ = false;
    bool updatedOnce_ = false;
    /// Set when Update emitted a provisional head; that head is the last
    /// element and must be dropped before the next emission commits over it.
    bool headPending_ = false;

    Vector3f prevPos_ = {0, 0, 0};
    Vector3f currPos_ = {0, 0, 0};
    Vector3f prevDir_ = {0, 0, 1};
    Vector3f currDir_ = {0, 0, 1};
    Vector3f prevVertical_ = {0, 1, 0};
    Vector3f currVertical_ = {0, 1, 0};

    // SC2 time-mode runtime. headU is the emission-time clock (seconds); the
    // accumulator laps emitPeriod to commit segments; caughtUp gates the
    // one-shot spawn pre-roll (CatchUpEmission).
    f32 sc2HeadU_ = 0;
    f32 sc2EmitAccum_ = 0;
    bool sc2CaughtUp_ = false;

    // SC2 spline runtime (W5). splineAge is the whole-ribbon clock (the emit
    // dtAccumulator, which splines only bank); sag[0..3] are the
    // per-control-point gravity offsets = accel·age² (Simulate_Spline recomputes
    // them from age each frame, not incrementally).
    f32 sc2SplineAge_ = 0;
    Vector3f sc2Sag_[4] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};

    // The overlay-wave clock (CRibbon+0x184) advances by dt identically to the
    // emission clock headU, so a segment's wave phase runs on its own birthU —
    // no separate field is kept.

    // 8-tap inherit-velocity smoothing (W6, CRibbon+0x1B0/0x230/0x250): a ring
    // of recent per-tick position deltas and dt weights; smoothedVel =
    // Σ posDelta / Σ dt (the emitter's smoothed velocity). Retail pushes one tap
    // per emission sub-step; a per-tick tap is the CPU-side approximation.
    ::whiteout::flakes::renderer::sc2::SmoothedVelocity sc2Smoothed_;
};

} // namespace whiteout::flakes::renderer::ribbon
