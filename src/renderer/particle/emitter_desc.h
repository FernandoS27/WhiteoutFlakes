#pragma once

// ============================================================================
// EmitterDesc — the immutable half of an emitter.
//
// Everything here is fixed at load time and identical for every actor spawned
// from the same model, so it is built once per ModelTemplate and shared by
// shared_ptr. Only the animated state (emission rate, speed, cone, plane size,
// visibility, transform) plus the pool and RNG live on the emitter instance.
//
// This is also the seam the other formats target: an M2 or M3 adapter fills in
// the same struct, so nothing downstream of here knows which format a given
// emitter came from.
// ============================================================================

#include "particle_constants.h"
#include "particle_curve.h"
#include "particle_material.h"
#include "particle_motion.h"
#include "particle_output.h"
#include "particle_shape.h"
#include "sc2_emitter_desc.h"
#include "types.h"
#include "whiteout/flakes/types.h"
#include "whiteout/flakes/util/coordinate_system.h"

#include <memory>
#include <string>
#include <vector>

namespace whiteout::flakes::renderer {
struct ParticleEmitterConfig;
}

namespace whiteout::flakes::renderer::particle {

// Sprite-sheet layout. `log2Cols` and the reciprocals are derived once at build
// time because the geometry builder needs them per particle per frame.
struct SpriteSheet {
    u32 rows = 1;
    u32 cols = 1;
    u32 log2Cols = 0;
    f32 ooWidth = 1.0f;
    f32 ooHeight = 1.0f;

    void Set(u32 r, u32 c) {
        rows = (r > 0) ? r : 1;
        cols = (c > 0) ? c : 1;
        ooWidth = 1.0f / static_cast<f32>(cols);
        ooHeight = 1.0f / static_cast<f32>(rows);
        log2Cols = 0;
        for (u32 n = cols; n > 1; n >>= 1)
            ++log2Cols;
    }
};

// How an emitter releases particles. WC3 is Continuous plus an optional
// one-shot burst when its emission rate crosses zero ("squirt"); M3's
// emit-N-total is the third mode this leaves room for.
struct EmissionDesc {
    bool squirtAtStart = false;
};

struct EmitterDesc {
    // Where particles are born and which way they head. Stateless and shared;
    // the animated half travels in SpawnParams.
    std::shared_ptr<const IParticleShape> shape;

    // Which client's simulation this emitter's stages run. The ONE selector:
    // `Emitter2::SetDesc` turns it into a runtime pointer and every other
    // touch point tests that pointer, so nothing downstream reads the enum
    // (SC2_PARTICLE_DESIGN.md §2.1). Orthogonal to ParticleBehavior, which
    // tunes the WC3-family simulation rather than choosing between families.
    enum class Family : u8 { Wc3, Sc2 };
    Family family = Family::Wc3;

    // Read only when `family == Family::Sc2`; inert otherwise, and inert
    // everywhere as of phase P0.
    Sc2EmitterDesc sc2;

    ParticleOutput output = ParticleOutput::Billboard;

    /// Which client's child-model particle a WC3-family `ChildModel` output is.
    /// The two place their children differently — a PE1 is a scaled copy at
    /// the particle, an M2 model particle tumbles and takes its size off the
    /// scale track — so they are different emitter classes, and this is what
    /// `EmitterFactory` chooses between. An SC2 one is told by `family`.
    enum class ChildModelKind : u8 { Pe1, M2 };
    ChildModelKind childModelKind = ChildModelKind::Pe1;

    // Child-model output only: which models a particle may become, and its
    // scale. A list because an SC2 `PAR_` carries a TABLE of model paths and
    // each particle draws one at birth (`ChildModelEvent::pathIndex`); WC3,
    // M2 and D3 author exactly one and always fill and read index 0.
    std::vector<std::string> childModelPaths;
    f32 childScale = 1.0f;

    /// The single authored path, for the dialects that have exactly one.
    /// Returns an empty string rather than throwing on an emitter with none,
    /// because "no child model" is a legal desc that the trace prints.
    const std::string& ChildModelPath() const {
        static const std::string kNone;
        return childModelPaths.empty() ? kNone : childModelPaths.front();
    }

    /// The `.m2` whose emitters trail every particle of THIS one (M2 RPID).
    /// Unrelated to @ref childModelPaths: those particles *are* models, these
    /// particles *drag* emitters. Resolved by the loader, which builds the
    /// trail emitters and hands them to this one — see M2_TRAIL_EMITTER_DESIGN.md.
    std::string trailModelPath;

    /// Model-particle tumble: each particle draws its own angular velocity
    /// (radians/s) from this range at birth. Kept as base-and-span the way
    /// `InitializeLoaded` @0x100f57e30 stores it, because that is the form the
    /// client's own draw reads — see M2ModelParticleEmitter for the two axes it
    /// reads wrong.
    Vector3f tumbleBase{0, 0, 0};
    Vector3f tumbleVary{0, 0, 0};

    // How particles are released, and how they move once released.
    EmissionDesc emission;
    MotionDesc motion;

    SpriteSheet sheet;

    // Colour / alpha / size / cell animation over normalised particle age.
    LifetimeCurves curves;

    f32 lifeSpan = 0.0f;

    // ---- WoW-only spread constants (M2 "FixedProp") ----
    // Each particle's lifespan is `lifeSpan + varQ/32768 * lifespanVariation`,
    // and the emission rate is re-jittered by `emissionRateVariation` every
    // frame. Zero here leaves both exactly inert, which is the MDX case.
    f32 lifespanVariation = 0.0f;
    f32 emissionRateVariation = 0.0f;

    // Scales the emitter motion a new particle inherits. This is the field the
    // M2 spec calls `burstMultiplier`; the runtime consumes it only here, so
    // the spec name is a misnomer (see M2_PARTICLE_DESIGN.md, answered Q4).
    f32 inheritVelocityScale = 1.0f;

    // Whether the emitter asked to inherit motion at all (M2 file flag 0x40).
    // Distinct from the scale above, which a record may legitimately set to
    // zero with the flag on. Read where the flag alone decides: a trail emitter
    // takes its driving particle's velocity only if this is set.
    bool inheritVelocity = false;

    // Pull on particles older than 2*dt, so a trail follows its emitter. How
    // much of the emitter's travel they inherit is a line in the emitter's own
    // speed, clamped to [0,1]: `bias + slope * speed`. The record stores the
    // line as two (speed, scale) sample points; SetFollowParams solves for
    // these two at load, so this is the runtime form, not the file's.
    f32 followBias = 0.0f;
    f32 followSlope = 0.0f;

    // M2 file flag InheritPosition. The client computes a random point along the
    // emitter's path and then spawns at the emitter's current position anyway —
    // see EmitStep for the two call sites that prove it. So what this flag
    // really selects is "one extra draw, and no path spacing at all", which is
    // still a behaviour of the emitter and not a cosmetic option.
    bool randomEmissionSpacing = false;

    // Skip the distance falloff on emission rate (M2 LodIgnoreDistance).
    bool lodIgnoreDistance = false;

    // Whether THIS emitter uses the two optional WoW motion features. The
    // dialect says the feature exists; these say the emitter asked for it —
    // both are per-emitter runtime flags in the client, set from the M2 record.
    // Enabling implosion dialect-wide would kill nearly every particle the
    // frame it moved outward, which is exactly what it is designed to do.
    bool implosionFilter = false;
    bool followPosition = false;
    // Initial longitudinal sweep. Seeds SpawnParams::longitude at registration;
    // PE1 animates it per frame, PE2 never does.
    f32 longitude = kWowTwoPi;
    f32 tailLength = 1.0f;
    f32 angularVelocity = 0.0f;

    bool hasHead = true;
    bool hasTail = false;
    bool sortZ = false;
    bool modelSpace = false;
    bool xyQuads = false;

    // ---- WoW-only appearance (see M2_PARTICLE_DESIGN.md C.3) ----
    // Every one of these is read only by the WoW geometry builder; a WC3 desc
    // leaves them off and never reaches that code at all.

    /// Align the head quad along the particle's velocity instead of the screen,
    /// foreshortened by how much of that velocity faces the camera.
    bool velocityOrient = false;
    /// Multiply the drawn size by the square root of the emitter bone's scale.
    bool inheritBoneScale = false;
    /// Spin backwards for particles whose seed is odd — half of them, so a
    /// spinning emitter reads as tumbling rather than rotating as one.
    bool negateSpinRandom = false;
    /// Shorten the tail to the particle's age, so a fresh particle has none.
    bool clampTailToAge = false;
    /// Displace the head quad along its own spun up-axis, turning spin into an
    /// orbit around the particle's position.
    bool offsetHeadBySpin = false;
    /// Draw the X and Y size jitter separately (two draws) rather than sharing
    /// one multiplier. A draw-count difference, so it is per-emitter data that
    /// shifts this particle's render stream and nothing else's.
    bool unscaledSizeVariation = false;
    /// Pick a random sheet cell when the head-cell track is empty.
    bool chooseRandomTexture = false;
    /// Give the emitter a random cell offset, drawn once from its own seed when
    /// the seed is set — so two copies of one model flip books out of phase.
    bool randFlipbookStart = false;

    /// 2D billboard rotation, radians: `age * spinSpeed + baseSpin`. Both terms
    /// take a symmetric per-particle variation re-derived from the particle's
    /// seed every frame (`GetSpin` @0x1016a2120). Note the client tests only
    /// the two SPEED terms when deciding whether to rotate at all, so a
    /// baseSpin with no spinSpeed draws unrotated — reproduced, not repaired.
    f32 baseSpin = 0.0f, baseSpinVariation = 0.0f;
    f32 spinSpeed = 0.0f, spinSpeedVariation = 0.0f;

    /// Per-particle size jitter, `max(1 + rand[-1,1] * variation, 1e-4)`. With
    /// `unscaledSizeVariation` clear only the x component is consulted, for
    /// both axes — the client's own asymmetry.
    Vector2f sizeVariation{0, 0};

    /// Twinkle. A particle is culled outright when `twinklePercent` falls below
    /// its table entry, and its size is multiplied by
    /// `twinkleBase + twinkleVary * entry` — the record's {min, max} range
    /// stored as base and span, exactly as SetTwinkleScale keeps it.
    f32 twinkleSpeed = 0.0f;
    f32 twinklePercent = 1.0f;
    f32 twinkleBase = 1.0f;
    f32 twinkleVary = 0.0f;

    /// @brief This emitter draws a screen-space distortion, not colour.
    ///
    /// M2 `Refraction`. Such an emitter is excluded from the transparent pass
    /// entirely — the client gives it its own render pass and its own buffer
    /// (`AddParticleElement` @0x100f78590 buckets it into M2PASS_REFRACTION and
    /// nowhere else) — and its particles carry the two extra scrolling UV sets
    /// below. See M2_REFRACTION_DESIGN.md.
    bool refraction = false;

    /// @brief This emitter draws three textures through three UV sets.
    ///
    /// M2 `MultiTexture`. Unlike @ref refraction it stays in the transparent
    /// pass — it is ordinary colour, just combined from three layers — so what
    /// this changes is the shader and the vertex stream, not the pass. Nearly
    /// half the shipped `.m2` corpus carries one. See M2_MULTITEX_DESIGN.md.
    bool multiTexture = false;

    /// Whether this emitter's particles carry the two extra scrolling UV sets.
    /// Refraction and multi-texture are the same `CMultiTexParticle` in the
    /// client, and everything below the shader treats them alike.
    bool UsesMultiTexLayers() const {
        return refraction || multiTexture;
    }

    /// @brief The two extra texture layers, in the runtime's form.
    ///
    /// `multiTexScale[i]` scales the quad's own corner coordinate into layer
    /// i's UV; `scrollMid`/`scrollRange` are the centre and half-width of the
    /// per-particle scroll rate, drawn once at birth. Read only when
    /// @ref UsesMultiTexLayers — nothing else has three UV sets.
    f32 multiTexScale[2] = {0.0f, 0.0f};
    Vector2f multiTexScrollMid[2] = {{0, 0}, {0, 0}};
    Vector2f multiTexScrollRange[2] = {{0, 0}, {0, 0}};

    i32 priorityPlane = 0;
    ParticleMaterialDesc material;
    CoordSpace coordSpace = kDefaultCoordSpace;
};

} // namespace whiteout::flakes::renderer::particle
