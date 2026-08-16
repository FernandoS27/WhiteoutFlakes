#pragma once

// ============================================================================
// Ribbon dialect — which variant of Blizzard's CRibbonEmitter a product runs.
//
// The Warcraft III and World of Warcraft ribbon runtimes are the same code,
// evolved. Reverse engineering WoW 6.0.1.18179's
// `Engine/Source/Services/RibbonEmitter.cpp` showed that its
// `InitInterpDeltas` @0x100e7c0b0 / `InterpEdge` @0x100e7c340 pair evaluates
// exactly the blend the MDX path in this renderer already ran — same tangent
// scaling by |curr-prev|, same operand order — and that `Update` @0x100e7dab0
// emits `floor(endTime-1)+1` edges at `t = (n - startTime)/(endTime - startTime)`
// with `age = -dt*t`, which is again what the MDX path did.
//
// So this is one simulation with a short list of measured divergences. Each
// field below is one of them, named rather than switched on a game enum: a
// profile answers with a behaviour, and `ribbon::RibbonEmitter` reads it.
//
// Vocabulary only — no simulation state, no dependencies. Lives in core/ so
// `IRenderProfile` can return one without core depending on the ribbon module.
// ============================================================================

#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::core {

struct RibbonBehavior {
    /// WoW's first tick uses `1/edgesPerSecond + 1e-4` instead of the elapsed
    /// time, so a ribbon commits exactly one edge on the frame it appears
    /// rather than however many the first dt happens to buy.
    bool firstFrameEmitsOneEdge = false;

    /// WoW clamps dt to the edge lifespan (`fminf`, negatives to zero). The MDX
    /// path clamps to a global maximum and, past the lifespan, throws the whole
    /// trail away and restarts it at the current pose.
    bool clampDtToLifespan = false;

    /// The 0.25s floor. In WoW it applies only where `InitEdges` sizes the ring
    /// (G3-gated against the binary, 250/250) — retirement and UV mapping use
    /// the raw lifespan. The MDX path floors the lifespan it does both with.
    bool lifespanFloorAppliesToSim = true;

    /// `InitInterpDeltas` returns 0 when |curr-prev| < 0.001 and it is not the
    /// first tick, which suppresses that frame's *emission loop* — the head is
    /// still re-placed and the carry still advances. A stationary WoW ribbon
    /// lays no new edges; the MDX path keeps stacking coincident ones.
    bool skipEmitWhenStationary = false;

    /// WoW writes the head edge with `InterpEdge(0, 1, advance=0)`: it lands at
    /// the ring's write position, is drawn, and is overwritten by the next
    /// frame's emission rather than committed. The MDX path appends a head each
    /// frame and keeps it, so its trail gains an edge per *frame* on top of the
    /// ones the emission rate asks for.
    bool headEdgeIsProvisional = false;

    /// The gravity step is `g*dt^2 + 2*g*age*dt` in both — which is
    /// `g*((age+dt)^2 - age^2)`, i.e. `z(t) = z0 + g*t^2`, NOT the textbook
    /// `0.5*g*t^2`. A half-gravity integrator drifts by a factor of two. Only
    /// the sign differs: WoW adds it to z, the MDX path subtracts.
    f32 gravitySign = -1.0f;

    /// The MDX path skips the entire emit block when the rate is zero or no
    /// time passed. WoW runs it regardless — a zero rate simply commits nothing
    /// while the provisional head keeps tracking the pose.
    bool requirePositiveRateToEmit = true;
    bool requirePositiveDtToEmit = true;

    static RibbonBehavior Wc3() {
        return RibbonBehavior{};
    }

    static RibbonBehavior Wow() {
        RibbonBehavior b;
        b.firstFrameEmitsOneEdge = true;
        b.clampDtToLifespan = true;
        b.lifespanFloorAppliesToSim = false;
        b.skipEmitWhenStationary = true;
        b.headEdgeIsProvisional = true;
        b.gravitySign = 1.0f;
        b.requirePositiveRateToEmit = false;
        b.requirePositiveDtToEmit = false;
        return b;
    }
};

} // namespace whiteout::flakes::renderer::core
