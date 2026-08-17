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
#include "types.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"

#include <vector>

namespace whiteout::flakes::renderer::ribbon {

using RibbonBehavior = core::RibbonBehavior;

/// @brief One emitted edge: the two vertices of a cross-section plus its age.
///
/// The client stores these as a ring of ages beside a ring of `CGxVertexPCT`
/// pairs; a vector of triples is the same information without the fixed
/// capacity. See RibbonBehavior::headEdgeIsProvisional for the one place the
/// ring's shape is observable.
struct RibbonEdge {
    Vector3f top = {0, 0, 0};
    Vector3f bot = {0, 0, 0};
    f32 age = 0;
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

    // Carried through to the draw unit rather than used by the simulation.
    i32 textureId = -1;
    i32 filterMode = 0;
    bool unshaded = false;
    bool twoSided = true;
    i32 priorityPlane = 0;
};

/// @brief Convert the public MDX-shaped config into the neutral desc.
RibbonDesc DescFromWc3Config(const RibbonEmitterConfig& cfg);

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

    /// @brief One simulation tick: retire, emit, then age + fall.
    void Update(f32 dt);

    /// @brief Append this emitter's triangles. Returns the vertex count added.
    i32 BuildStrip(std::vector<Vertex>& out) const;

    /// @brief Vertices BuildStrip would append, without building them.
    i32 VertexCount() const;

    const std::vector<RibbonEdge>& Edges() const {
        return edges_;
    }
    bool PositionSeeded() const {
        return posSet_;
    }

    /// @brief The lifespan the simulation actually uses — floored or raw per
    ///        RibbonBehavior::lifespanFloorAppliesToSim.
    f32 SimLifespan() const;

private:
    bool ShouldEmit(f32 dt) const;

    RibbonDesc desc_;
    RibbonBehavior behavior_ = RibbonBehavior::Wc3();
    RibbonState state_;

    std::vector<RibbonEdge> edges_;
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
};

} // namespace whiteout::flakes::renderer::ribbon
