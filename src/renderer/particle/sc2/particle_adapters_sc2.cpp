#include "renderer/particle/particle_adapters.h"

#include "renderer/particle/base/particle_constants.h"

#include <algorithm>
#include <bit>

namespace whiteout::flakes::renderer::particle {

// ---------------------------------------------------------------------------
// StarCraft II `PAR_`
// ---------------------------------------------------------------------------

namespace {

/// `InitCopy`'s mid-time clamp, in 4.8 as in 5.0 — see sc2::EmitterDesc::Look::midTime.
f32 Sc2ClampMid(f32 v) {
    return v > renderer::sc2::kMidTimeCeil ? renderer::sc2::kMidTimeCeil : v;
}

} // namespace

std::shared_ptr<EmitterDesc>
DescFromSc2ParticleConfig(const effects::Sc2ParticleEmitterConfig& cfg,
                          std::span<const effects::Sc2ParticleEmitterConfig> siblings) {
    auto d = std::make_shared<EmitterDesc>();
    d->family = EmitterDesc::Family::Sc2;

    sc2::EmitterDesc& s = d->sc2;
    s.flags = static_cast<ParticleFlag>(cfg.flags);
    s.additionalFlags = static_cast<ParticleAdditionalFlag>(cfg.additionalFlags);
    s.rotationFlags = static_cast<ParticleRotationFlag>(cfg.rotationFlags);

    // ---- EMIT ----
    s.emit.shape = cfg.emitShape;
    s.emit.velocityType = static_cast<u8>(cfg.velocityType);
    // The pool is capped by the vertex arena, not by the author (RE §3.1). An
    // emitter asking for more gets fewer.
    s.emit.maxParticles = (std::min)(cfg.maxParticles, renderer::sc2::kMaxParticles);
    s.emit.lodReduce = cfg.lodReduce;
    s.emit.lodCut = cfg.lodCut;
    s.emit.slotBones = cfg.slotBones;
    s.emit.squirt = cfg.squirt;
    s.emit.shapeRegions = cfg.shapeRegions;
    s.emit.sizeRandom = cfg.sizeRandom;
    s.emit.rotationRandom = cfg.rotationRandom;
    s.emit.colorRandom = cfg.colorRandom;
    for (i32 k = 0; k < renderer::sc2::OverlayGroup::kCount; ++k)
        s.emit.overlayType[k] = cfg.overlayType[k];
    s.emit.preRollPeaks = cfg.preRollPeaks;
    s.emit.preRollInit = cfg.preRollInit;
    s.emit.worldSpace = s.Has(ParticleAdditionalFlag::WorldSpace);

    // ---- MOVE ----
    // `drag` is carried AUTHORED. The floor (`max(drag, 0.01)`, and an
    // `invDrag` of 100 chosen on the RAW value) is applied per particle at
    // spawn, not here — flooring at load would lose the raw value the fallback
    // is chosen on (RE §16 and §8.1).
    s.motion.drag = cfg.drag;
    s.motion.mass = cfg.mass;
    s.motion.massRandom = cfg.massRandom;
    s.motion.gravity3 = cfg.gravity3;
    s.motion.bounce = cfg.bounce;
    s.motion.friction = cfg.friction;
    s.motion.collisionDieBounce = cfg.collisionDieBounce;
    s.motion.windMultiplier = cfg.windMultiplier;
    s.motion.killRadius = cfg.killRadius;
    s.motion.noiseAmplitude = cfg.noiseAmplitude;
    s.motion.noiseFrequency = cfg.noiseFrequency;
    s.motion.noiseCoherence = cfg.noiseCoherence;
    s.motion.noiseEdge = cfg.noiseEdge;
    s.motion.forces = cfg.forces;
    s.motion.forcesFallback = cfg.forcesFallback;

    // ---- BUILD ----
    s.look.instanceType = cfg.instanceType;
    // As authored. The three legacy Bezier bits convert `Init`'s caches, and the
    // first `UpdateAnimatedParams` re-samples over that conversion before any
    // particle reads it (design §8), so they change nothing a port can see.
    s.look.sizeSmoothing = cfg.sizeSmoothing;
    s.look.colorSmoothing = cfg.colorSmoothing;
    s.look.rotationSmoothing = cfg.rotationSmoothing;
    for (i32 k = 0; k < renderer::sc2::MidChannel::kCount; ++k) {
        s.look.midTime[k] = Sc2ClampMid(cfg.midTime[k]);
        s.look.midHold[k] = cfg.midHold[k];
    }
    s.look.flipbookMidTime = Sc2ClampMid(cfg.flipbookMidTime);
    s.look.flipbookColumns = cfg.flipbookColumns;
    s.look.flipbookRows = cfg.flipbookRows;
    s.look.flipbookColumnFraction = cfg.flipbookColumnFraction;
    s.look.flipbookRowFraction = cfg.flipbookRowFraction;
    s.look.flipbookStartInit = cfg.flipbookStartInit;
    s.look.flipbookStartStop = cfg.flipbookStartStop;
    s.look.flipbookEndInit = cfg.flipbookEndInit;
    s.look.tailLength = cfg.tailLength;
    s.look.instanceAngle = cfg.instanceAngle;
    s.look.instanceDistance = cfg.instanceDistance;
    s.look.lit = s.Has(ParticleFlag::LitParts);
    // `b_useLighting` is cleared unless LitParts (RE §8.3), whatever the
    // material says; the M3 draw ORs this into the surface's own Unshaded.
    d->material.unshaded = !s.look.lit;

    // ---- OUTPUT ----
    s.children.collisionSpawnIndex = cfg.collisionSpawnIndex;
    s.children.collisionSpawnMin = cfg.collisionSpawnMin;
    s.children.collisionSpawnMax = cfg.collisionSpawnMax;
    s.children.collisionSpawnChance = cfg.collisionSpawnChance;
    s.children.collisionSpawnEnergy = cfg.collisionSpawnEnergy;
    // Resolved now so the MOVE stage never looks an emitter up per collision.
    if (cfg.collisionSpawnIndex >= 0 &&
        static_cast<usize>(cfg.collisionSpawnIndex) < siblings.size()) {
        constexpr u32 kWorldSpace = static_cast<u32>(ParticleAdditionalFlag::WorldSpace);
        s.children.collisionChildIsWorldSpace =
            (siblings[static_cast<usize>(cfg.collisionSpawnIndex)].additionalFlags & kWorldSpace) !=
            0;
    }
    s.children.trailLinkIndex = cfg.trailLinkIndex;
    s.children.trailChance = cfg.trailChance;
    s.children.splatProjectorIndex = cfg.splatProjectorIndex;
    s.children.splatChance = cfg.splatChance;
    // A NON-ZERO TEST, not a probability: see the desc's comment. On the raw
    // bits, as `UpdateModelParticle` reads them (`LODWORD(...)`), so a
    // negative zero counts where a float compare would not.
    s.children.modelOrientLegacy = std::bit_cast<u32>(cfg.modelOrientPreset) != 0u;
    s.children.modelOrientVariant = cfg.modelOrientVariant;

    // The derivation, once, on the finished block. Anything that reads
    // `motion.analytic` reads a decision, never a re-derivation (OP1's second
    // case is that this lands in the desc at all).
    s.motion.analytic = sc2::CanUseGpuMotion(s);

    // ---- the shared fields the block SETS rather than duplicates ----
    d->sortZ = s.Has(ParticleFlag::Sort);
    d->modelSpace = !s.emit.worldSpace;
    d->output = s.Has(ParticleFlag::ModelParticles) ? ParticleOutput::ChildModel
                                                    : ParticleOutput::Billboard;
    d->childModelPaths = cfg.modelPaths;
    // `m3Surface` and `priorityPlane` are stamped by the loader, which owns the
    // surface table; -1 leaves the draw on the BLS fallback.
    return d;
}

} // namespace whiteout::flakes::renderer::particle
