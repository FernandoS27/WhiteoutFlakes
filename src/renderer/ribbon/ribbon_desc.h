#pragma once

// ============================================================================
// RibbonDesc — one emitter's statics, converted once at load and never
// rewritten.
//
// Both public configs (`RibbonEmitterConfig` for the MDX/`.m2` shape,
// `Sc2RibbonEmitterConfig` for `RIB_`) land here through the DescFrom*Config
// pair, which is where the raw file words become named bits: everything
// downstream reads `m3::RibbonFlag` and the enums in ribbon_constants.h, never
// a hex literal (SC2_PARTICLE_DESIGN.md R7).
//
// The WC3<->WoW sub-dialect rides here too, as `behavior`. It used to be a
// second constructor argument beside the desc, which meant an SC2 emitter was
// handed a WC3 behaviour POD that described neither of the runtimes it runs —
// two selectors that had to agree (R1). RIBBON_SERVICE.md 3.1 specified it as
// a desc field from the start.
// ============================================================================

#include "core/ribbon_dialect.h"
#include "renderer/ribbon/ribbon_constants.h"
#include "renderer/ribbon/ribbon_types.h"
#include "types.h"
#include "whiteout/flakes/model_types.h"
#include "whiteout/flakes/types.h"
#include "whiteout/models/m3/structures/base.h"

#include <algorithm>
#include <vector>

namespace whiteout::flakes::renderer::ribbon {

namespace m3 = ::whiteout::m3;

using RibbonBehavior = core::RibbonBehavior;

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
    /// never re-derived — and the ONLY thing that says which machine this is
    /// (R1). Wc3 covers WC3 and WoW; which of those two is `behavior` below.
    enum class Family : u8 { Wc3, Sc2 };
    Family family = Family::Wc3;

    /// The WC3<->WoW sub-dialect, answered by the profile at registration. It
    /// describes the Wc3 family's stage variants only; an Sc2 desc leaves it at
    /// its default, which no SC2 stage reads.
    RibbonBehavior behavior = RibbonBehavior::Wc3();

    /// @brief StarCraft II statics (read only when family == Sc2). Field
    ///        semantics per SC2_RIBBON_RE.md; everything animated arrives per
    ///        frame in RibbonState::sc2.
    ///
    /// The raw bits are kept as WhiteoutLib's enums rather than as `u32`, so a
    /// consumer names the bit it wants and the compiler catches the wrong word
    /// (SC2_PARTICLE_DESIGN.md R7). Where the RE settled a name the file enum
    /// does not use — `UseLocator` is the yaw/pitch operand swap — the
    /// predicate below is where the two are reconciled, once.
    struct Sc2 {
        /// RIB_'s material ref resolved to an M3SurfaceTable entry at
        /// registration; -1 = unresolved (draws route down the BLS path).
        i32 m3Surface = -1;

        // ---- raw bits, read through the enums (R7) ----
        m3::RibbonFlag flags{};
        m3::RibbonAdditionalFlag additionalFlags{};

        m3::RibbonType ribbonType = m3::RibbonType::Billboard;
        CullMethod cullMethod = CullMethod::Time;
        SimTechnique simTechnique = SimTechnique::GpuOnly; ///< Run once at load.
        f32 divisions = 20.0f;   ///< Authored density (RIB_+0x198).
        i32 edges = 5;
        f32 innerRadius = 0.5f;
        /// Indexed by @ref MidChannel; plain floats, ≤ @ref kMidTimeCeil.
        f32 midTime[MidChannel::kCount] = {0.5f, 0.5f, 0.5f, 0.5f};
        f32 midHold[MidChannel::kCount] = {0, 0, 0, 0};
        u8 sizeSmoothing = 0, colorSmoothing = 0;
        f32 drag = 1.0f, mass = 1.0f; ///< Static; drag clamped ≥ kDragFloor.
        Vector3f gravity3 = {0, 0, 0}; ///< Only .z reaches the analytic path.
        f32 friction = 1.0f, bounce = 0.0f;
        f32 noiseAmplitude = 0, noiseFrequency = 0, noiseCoherence = 0,
            noiseEdge = 1.0f;
        /// Indexed by @ref WaveChannel. 0 = off.
        u32 waveTypes[WaveChannel::kCount] = {0, 0, 0, 0, 0};
        u8 lodReduce = 0, lodCut = 0; ///< Table ROW indices, not factors.
        bool hasSpline = false;
        /// SRIB record 0 — the only one the runtime ever reads.
        Sc2SplineRibbonConfig spline;
        /// UNFILLED — see RIBBON_REFACTOR_PLAN.md C7c. The load-time
        /// derivations RE §3.1 describes were never written, so the head
        /// kernel's length-mode expiry runs on a constant 0. The inputs reach
        /// `Sc2RibbonEmitterConfig` (`*Init`) and stop there.
        f32 maxLengthBound = 0;

        bool Has(m3::RibbonFlag f) const {
            return (static_cast<u32>(flags) & static_cast<u32>(f)) != 0;
        }
        bool Has(m3::RibbonAdditionalFlag f) const {
            return (static_cast<u32>(additionalFlags) & static_cast<u32>(f)) != 0;
        }

        /// `RIB_` `UseLocator` (0x8000), under the name the RE settled for what
        /// it DOES: it exchanges the yaw and pitch operands of the emission
        /// basis (SC2_RIBBON_RE.md §3.2, oracle O4).
        bool SwapsYawPitch() const {
            return Has(m3::RibbonFlag::UseLocator);
        }
        /// Where a segment is anchored. Nearly every shipped hero ribbon is
        /// world-space: the strip traces the emitter's path through space.
        bool IsWorldSpace() const {
            return Has(m3::RibbonAdditionalFlag::WorldSpace);
        }
        bool InheritsParentVelocity() const {
            return Has(m3::RibbonFlag::InheritParentVelocity);
        }
        bool CollidesTerrain() const {
            return Has(m3::RibbonFlag::CollideTerrain);
        }
        /// Length mode maxes V with the time fraction rather than replacing it.
        bool UsesLengthAndTime() const {
            return Has(m3::RibbonFlag::UseLengthAndTime);
        }
        /// One predicate for "this ribbon authored noise" — the same test that
        /// demotes the technique to Legacy, so BUILD and the selector can never
        /// disagree about it.
        bool HasNoise() const {
            return noiseAmplitude > kNoiseThreshold;
        }
        /// The twist channel's mid-time, with the unauthored fallback both
        /// BUILD paths apply.
        f32 RotationMidTime() const {
            return (midTime[MidChannel::Rotation] > 0.0f) ? midTime[MidChannel::Rotation]
                                                          : kDefaultRotationMidTime;
        }
    } sc2;
};

/// @brief The cross-section one strip is expanded through: which shape, how
///        many ring vertices, and which frame branch.
///
/// Both BUILD paths derived this independently — the trail from its technique,
/// the spline with `smoothPath` pinned true — which is how the ring-count rule
/// (a star holds one inner and one outer point per authored edge) came to be
/// written twice.
struct Sc2Section {
    m3::RibbonType xsec = m3::RibbonType::Billboard;
    i32 ringEdges = 1;     ///< 1 for a flat strip; the ring size for a tube.
    bool tube = false;     ///< Cylinder or star: a closed ring, bridged rung to rung.
    bool smoothPath = false; ///< Ribbon.fx:457's non-flattened frame branch.
};

/// @p smoothPath is the caller's, because a spline always takes the smooth
/// branch while a trail takes it per technique (@ref UsesSmoothFrame).
inline Sc2Section Sc2SectionFor(const RibbonDesc::Sc2& s, bool smoothPath) {
    Sc2Section sec;
    sec.xsec = s.ribbonType;
    sec.tube = (s.ribbonType == m3::RibbonType::Cylinder ||
                s.ribbonType == m3::RibbonType::Star);
    const i32 baseEdges =
        sec.tube ? std::clamp(s.edges, kRingEdgeMin, kRingEdgeMax) : 1;
    // BuildCrossSection type 3: a star ring holds 2·edges points, one inner
    // and one outer per authored edge.
    sec.ringEdges = (s.ribbonType == m3::RibbonType::Star) ? baseEdges * 2 : baseEdges;
    sec.smoothPath = smoothPath;
    return sec;
}

/// @brief Convert the public MDX-shaped config into the neutral desc.
RibbonDesc DescFromWc3Config(const RibbonEmitterConfig& cfg);

/// @brief `Ribbon_SelectSimTechnique`, verbatim from the truth table oracle
///        O1 pins (`tools/sc2_ribbon_oracle/golden/o1_simtech.json`):
///        0 GPU-only, 1 spline, 2 mixed (length), 3 mixed precomputed
///        tangent, 4 legacy CPU. CPU-side 0/2/3 share one integrator; the id
///        is kept where behaviour differs. Note the forces operand is the
///        FALLBACK pair dword and the noise compare is strictly-greater than
///        float32(0.001) — both settled by the oracle, not by the wiki.
SimTechnique SelectSc2SimTechnique(const Sc2RibbonEmitterConfig& cfg);

/// @brief Convert the public SC2 config into a Family::Sc2 desc — including
///        the load-time technique derivation. `m3Surface`/`priorityPlane` are
///        stamped by the loader, which owns the surface table.
RibbonDesc DescFromSc2Config(const Sc2RibbonEmitterConfig& cfg);

} // namespace whiteout::flakes::renderer::ribbon
