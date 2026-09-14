#pragma once

// ============================================================================
// Sc2Runtime — everything an SC2 ribbon keeps between frames, in one block.
//
// The emitter owns it through `std::unique_ptr<Sc2Runtime> sc2_`, allocated by
// SetDesc when `desc.family == Family::Sc2` and freed otherwise. That pointer
// is then the family test wherever a stage needs one, and `sizeof(RibbonEmitter)`
// grows by a pointer rather than by a dozen fields for every WC3 ribbon on
// screen.
//
// One block rather than twelve `sc2*_` members on the emitter — which is the
// shape `particle/sc2_runtime.h` cites this module for and declined to repeat.
// The concrete cost of the old shape was `ResetTrail`: a hand-written list of
// every clock, in the header, that a new field had to be remembered into. A
// rewind that forgot one kept last life's clock. `*sc2_ = Sc2Runtime{}` cannot
// forget.
//
// Grouped by the stage that reads it, as the desc is.
// ============================================================================

#include "renderer/sc2/sc2_element.h"
#include "types.h"
#include "whiteout/flakes/types.h"

namespace whiteout::flakes::renderer::ribbon {

struct Sc2Runtime {
    // ---- EMIT ----------------------------------------------------------
    /// The emission-time clock, in seconds. Every element's `birthU`/`deathU`
    /// is stamped against it and BUILD measures age from it, so it advances
    /// whether or not the emitter is laying segments — `active` gates NEW
    /// segments, never the live trail.
    f32 headU = 0;
    /// Fractional carry across the emission period, lapping once per segment.
    f32 emitAccum = 0;
    /// One-shot: the spawn pre-roll (CatchUpEmission) has run.
    bool caughtUp = false;

    // ---- SPLINE (W5) ---------------------------------------------------
    /// The whole-ribbon clock. A spline has no per-segment aging: it is rebuilt
    /// from four control points every frame and loops on its lifetime.
    f32 splineAge = 0;
    /// Per-control-point gravity offset, `accel·age²`. Simulate_Spline
    /// recomputes these from the age each frame rather than integrating.
    Vector3f sag[4] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}};

    // ---- MOVE ----------------------------------------------------------
    /// 8-tap inherit-velocity smoothing (CRibbon+0x1B0/0x230/0x250): a ring of
    /// recent per-tick position deltas and dt weights, `smoothedVel =
    /// Σ posDelta / Σ dt`. Retail pushes one tap per emission sub-step; a
    /// per-tick tap is the CPU-side approximation.
    ::whiteout::flakes::renderer::sc2::SmoothedVelocity smoothed;

    // The overlay-wave clock (CRibbon+0x184) advances by dt identically to
    // `headU`, so a segment's wave phase runs on its own birthU and no separate
    // field is kept.
};

} // namespace whiteout::flakes::renderer::ribbon
