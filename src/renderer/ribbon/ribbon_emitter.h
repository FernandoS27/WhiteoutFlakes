#pragma once

// ============================================================================
// RibbonEmitter — one trail, simulated the way Blizzard's CRibbonEmitter does.
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
// Those divergences live in core::RibbonBehavior (core/ribbon_dialect.h) and
// ride on the desc: each field is one difference established from the
// disassembly, named rather than branched on a game enum.
//
// StarCraft II is the general form of the same machine (RIBBON_SERVICE.md §1),
// so it is a second set of stage variants rather than a second class: emitters
// stay value-stored in the service's ordered map, and which variant runs is
// DATA — `desc_.family`, read once per tick in Update and once in BuildStage.
//
// Where the pieces live:
//   ribbon_constants.h    named literals, channel orders, technique enums
//   ribbon_types.h        element / state / draw-list
//   ribbon_desc.h         the statics + the two DescFrom*Config converters
//   ribbon_sc2_runtime.h  what an SC2 ribbon keeps between frames
//   ribbon_stages_sc2.h   the oracle-gated kernels
//   ribbon_emitter.cpp    Update/SetState + the WC3 stages
//   ribbon_tick_sc2.cpp   the SC2 tick stages and the spline tick
//   ribbon_build.cpp      all three BUILD paths + the shared section expander
// ============================================================================

#include "constants.h"
#include "renderer/ribbon/ribbon_constants.h"
#include "renderer/ribbon/ribbon_desc.h"
#include "renderer/ribbon/ribbon_sc2_runtime.h"
#include "renderer/ribbon/ribbon_stages_sc2.h"
#include "renderer/ribbon/ribbon_types.h"
#include "types.h"
#include "whiteout/flakes/types.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace whiteout::flakes::renderer::ribbon {

/// @brief One step of the fractional emission carry, which BOTH families run.
///
/// `InitInterpDeltas`' edge loop and `EmitSegments`' headU clock are the same
/// arithmetic under different names: accumulate `rate·dt` into a carry, lay
/// `floor()` of it, and keep the fraction. They agree on the count too —
/// WC3 computes `floor(end - 1) + 1` and SC2 `floor(end)`, which are equal for
/// every value including exact integers — so this is one kernel that was
/// written twice, once in golden-bound code.
///
/// Operand order is WC3's, because that is the half that is pixel-golden: the
/// span is a reciprocal MULTIPLY, not a divide, and edge *n* takes
/// `(f32)n - carryAtEntry` exactly as the client's `newEdgeTime` accumulator
/// does. @p carry is advanced whether or not the caller lays anything.
struct EmissionBurst {
    i32 count = 0;          ///< Segments whose interval lapsed this step.
    f32 carryAtEntry = 0;
    f32 ooSpan = 1.0f;      ///< Reciprocal of `rate·dt`, or 1 for a degenerate step.

    /// Where edge @p n (1-based) falls between the previous and current pose.
    f32 Fraction(i32 n) const {
        return std::clamp((static_cast<f32>(n) - carryAtEntry) * ooSpan, 0.0f, 1.0f);
    }
};

inline EmissionBurst AdvanceEmissionCarry(f32& carry, f32 dt, f32 rate) {
    EmissionBurst b;
    b.carryAtEntry = carry;
    const f32 end = carry + dt * rate;
    const f32 span = end - carry;
    b.count = static_cast<i32>(std::floor(end));
    b.ooSpan = (span > kVectorEpsilon) ? 1.0f / span : 1.0f;
    carry = end - std::floor(end);
    return b;
}

class RibbonEmitter {
public:
    RibbonEmitter() = default;
    explicit RibbonEmitter(const RibbonDesc& desc) {
        SetDesc(desc);
    }

    /// @brief Install the statics. Allocates the SC2 runtime block for an SC2
    ///        desc and drops it otherwise, so `sc2_` and `desc_.family` cannot
    ///        disagree about which machine this emitter is.
    void SetDesc(const RibbonDesc& d);
    const RibbonDesc& Desc() const {
        return desc_;
    }
    const RibbonBehavior& Behavior() const {
        return desc_.behavior;
    }

    /// @brief Push this frame's sampled pose. Shifts curr → prev; the first
    ///        call seeds both and arms emission (the client's `posSet` flag).
    void SetState(const RibbonState& st);
    const RibbonState& State() const {
        return state_;
    }

    /// @brief One simulation tick. Runs the family's stage variants — the WC3
    ///        family in the client's own order (retire → emit → move).
    void Update(f32 dt);

    /// @brief The BUILD stage: append this emitter's triangles and one draw
    ///        record per pass (WC3: one per layer; SC2: one, with m3Surface).
    ///        Returns the vertex count added; records carry offsets relative
    ///        to `out`'s state at entry and zeroed model/emitterId for the
    ///        caller to stamp.
    i32 BuildStage(const RibbonBuildContext& ctx, std::vector<Vertex>& out,
                   std::vector<RibbonDrawList>& outDraws) const;

    /// @brief Append this emitter's WC3-family triangles. Returns the vertex
    ///        count added.
    i32 BuildStrip(std::vector<Vertex>& out) const;

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
        // Every SC2 clock at once — the block IS the list, so a field added to
        // the runtime cannot be forgotten here.
        if (sc2_)
            *sc2_ = Sc2Runtime{};
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

    // -- WC3 family (ribbon_emitter.cpp) --------------------------------------
    void TickWc3(f32 dt);
    void PrepWc3(TickCtx& t, f32 dt);
    void RetireWc3(const TickCtx& t);
    void EmitWc3(TickCtx& t);
    void MoveWc3(const TickCtx& t);
    i32 BuildStageWc3(std::vector<Vertex>& out,
                      std::vector<RibbonDrawList>& outDraws) const;

    // -- SC2 family (ribbon_tick_sc2.cpp) -------------------------------------
    // The per-element launch state comes from the oracle-gated kernels in
    // ribbon_stages_sc2.cpp; the frame loop follows RIBBON_SERVICE.md §5.1
    // (design-driven, not oracle-pinned): headU is the emission-time clock
    // advancing by dt, and a segment is committed each
    // `emitPeriod = lifetime / divisions`.
    void TickSc2(f32 dt);
    void PrepSc2(TickCtx& t, f32 dt);
    /// One whole SC2 tick: append, integrate if the technique is a CPU one,
    /// retire. The frame runs it once; the spawn pre-roll runs it per 33 ms
    /// step, so "what a tick is" has exactly one definition.
    void StepSc2(f32 dt);
    /// Advance headU by dt and commit any segments whose interval lapsed
    /// (EmitSegments' headU clock + append).
    void AppendSc2(f32 dt);
    /// Drop segments aged past their deathU (Simulate_Type0's retire walk).
    void RetireSc2();
    /// Legacy per-segment Euler integration (Simulate_Type4): advance each
    /// element's velocity/position semi-implicitly under gravity, then
    /// (CollideTerrain) sweep it against the ground query and reflect off the
    /// surface, then apply drag damping — the binary's integrate/collide/drag
    /// order. Collision is the grid stand-in for the map colliders.
    void IntegrateLegacySc2(f32 dt);
    /// Push this tick's emitter motion into the 8-tap ring and recompute the
    /// runtime's smoothed velocity. No-op unless inheriting.
    void UpdateSmoothedVelocitySc2(f32 dt);
    /// Build one segment at the interpolated emitter pose, birthU-stamped (the
    /// UpdateHeadSegment field write). Used both for committed history and for
    /// the live head BUILD synthesises each frame.
    RibbonElement MakeSegmentSc2(f32 birthU, f32 fracToCurr) const;
    /// Fold the inherited parent velocity in, then apply the stationary floor —
    /// the two steps both anchoring modes run, in the binary's order. @p dir is
    /// the emission direction in the ELEMENT's own space, which is what a
    /// degenerate velocity is replaced by.
    void ApplyInheritAndStationaryFloor(Vector3f& velocity, const Vector3f& dir,
                                        SimTechnique tech) const;
    /// Spline tick (Simulate_Spline, RIBBON_SERVICE_PLAN.md W5): advance the
    /// whole-ribbon age clock, accumulate the persistent quadratic sag, loop on
    /// lifetime. A spline has no per-segment emission — the strip is rebuilt
    /// from the four control points every frame — so it replaces the emit/move
    /// stages rather than augmenting them.
    void TickSplineSc2(f32 dt);
    /// emitPeriod's reciprocal (segments per second): the density that keeps
    /// `divisions` segments alive over one lifetime. lodKeepFactor and
    /// emissionScale are 1 at viewer quality (RIBBON_SERVICE.md §5.1).
    f32 SegmentsPerSecondSc2() const;

    // -- BUILD (ribbon_build.cpp) ---------------------------------------------
    /// Per-element the vertex shader math on the CPU (drag displacement,
    /// camera/planar frame, size/colour interpolation, V = fAge), then the
    /// emitter world transform. Four steps, in the order the data settles.
    i32 BuildStripSc2(const RibbonBuildContext& ctx, std::vector<Vertex>& out) const;
    void CollectNodesSc2(std::vector<RibbonElement>& elements,
                         std::vector<Sc2Node>& nodes) const;
    void ResolveTangentsSc2(std::vector<Sc2Node>& nodes, bool velTangent) const;
    void ResolveAgeSc2(const std::vector<RibbonElement>& elements,
                       std::vector<Sc2Node>& nodes) const;
    void SampleAttributesSc2(const std::vector<RibbonElement>& elements,
                             std::vector<Sc2Node>& nodes) const;
    /// One cubic Bezier from the SRIB control points, sampled at
    /// @ref kSplineSamples points, expanded through the shared cross-section.
    i32 BuildStripSplineSc2(const RibbonBuildContext& ctx,
                            std::vector<Vertex>& out) const;
    /// The four world-space control points this frame, plus the two overlay-wave
    /// channels the SAMPLING loop applies rather than the control points.
    struct Sc2SplineFrame {
        Vector3f p[4] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        f32 sizeWave = 0, alphaWave = 0;
    };
    Sc2SplineFrame BuildSplineFrameSc2() const;
    Vector3f SplineUpSc2(const Sc2SplineFrame& f) const;
    i32 BuildStageSc2(const RibbonBuildContext& ctx, std::vector<Vertex>& out,
                      std::vector<RibbonDrawList>& outDraws) const;

    RibbonDesc desc_;
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

    /// Everything the SC2 stages keep between frames, or null for a WC3 ribbon
    /// (R8: the class grows by a handle, not by fields).
    std::unique_ptr<Sc2Runtime> sc2_;
};

} // namespace whiteout::flakes::renderer::ribbon
