// The two config -> desc converters and the load-time technique derivation.
// This is where the public words become named bits, so nothing downstream has
// to know what 0x8000 meant (SC2_PARTICLE_DESIGN.md R7).

#include "renderer/ribbon/ribbon_desc.h"

#include <algorithm>

namespace whiteout::flakes::renderer::ribbon {

RibbonDesc DescFromWc3Config(const RibbonEmitterConfig& cfg) {
    RibbonDesc d;
    d.edgesPerSecond = cfg.emission;
    d.edgeLifespan = cfg.life;
    d.gravity = cfg.gravity;
    d.rows = cfg.rows;
    d.cols = cfg.cols;
    d.priorityPlane = cfg.priorityPlane;
    // An MDX ribbon is one pass, so the scalar fields ARE its single layer.
    // A config that filled `layers` (the `.m2` route) keeps them verbatim.
    if (cfg.layers.empty()) {
        RibbonLayer l;
        l.textureId = cfg.textureId;
        l.filterMode = cfg.filterMode;
        l.unshaded = cfg.unshaded;
        l.twoSided = cfg.twoSided;
        d.layers = {l};
    } else {
        d.layers = cfg.layers;
    }
    return d;
}

SimTechnique SelectSc2SimTechnique(const Sc2RibbonEmitterConfig& cfg) {
    // Bit-for-bit with the binary's decision order (oracle O1, 720 vectors):
    // ForceCPUSim wins outright; a spline is demoted to legacy only by noise;
    // forces/collide/UseLengthAndTime/noise force legacy; then accurate
    // tangents, then the length cull, then pure GPU.
    //
    // The config is the loader-facing shape and still carries the raw word, so
    // this is the one place the bits are named against it.
    const auto has = [&](m3::RibbonFlag f) {
        return (cfg.flags & static_cast<u32>(f)) != 0;
    };
    const bool noisy = cfg.noiseAmplitude > kNoiseThreshold;

    if (has(m3::RibbonFlag::ForceCPUSim))
        return SimTechnique::Legacy;
    if (!cfg.splines.empty())
        return noisy ? SimTechnique::Legacy : SimTechnique::Spline;
    if (cfg.forcesFallback != 0 || has(m3::RibbonFlag::CollideTerrain) ||
        has(m3::RibbonFlag::CollideObjects) || has(m3::RibbonFlag::UseLengthAndTime) || noisy)
        return SimTechnique::Legacy;
    if (has(m3::RibbonFlag::AccurateGPUTangents))
        return SimTechnique::MixedTangent;
    if (cfg.cullMethod == static_cast<u8>(CullMethod::Length))
        return SimTechnique::MixedLength;
    return SimTechnique::GpuOnly;
}

RibbonDesc DescFromSc2Config(const Sc2RibbonEmitterConfig& cfg) {
    RibbonDesc d;
    d.family = RibbonDesc::Family::Sc2;
    // The WC3-family scalars stay at defaults; the SC2 stages never read them.
    // The public config is the loader-facing shape and carries raw words; the
    // desc is what the stages read, so the conversion to named bits happens
    // here and exactly once (R7).
    d.sc2.flags = static_cast<m3::RibbonFlag>(cfg.flags);
    d.sc2.additionalFlags = static_cast<m3::RibbonAdditionalFlag>(cfg.additionalFlags);
    d.sc2.ribbonType = static_cast<m3::RibbonType>(cfg.ribbonType);
    d.sc2.cullMethod = static_cast<CullMethod>(cfg.cullMethod);
    d.sc2.simTechnique = SelectSc2SimTechnique(cfg);
    d.sc2.divisions = cfg.divisions;
    d.sc2.edges = cfg.edges;
    d.sc2.innerRadius = cfg.innerRadius;
    for (i32 i = 0; i < MidChannel::kCount; ++i) {
        // The mid-time ceiling is 5.0's load-time rule (RE §0); the mid-time is
        // a divisor in the VS two-piece interpolators, so 1.0 exactly would
        // divide by zero in the second piece.
        d.sc2.midTime[i] = (std::min)(cfg.midTime[i], kMidTimeCeil);
        d.sc2.midHold[i] = cfg.midHold[i];
    }
    d.sc2.sizeSmoothing = cfg.sizeSmoothing;
    d.sc2.colorSmoothing = cfg.colorSmoothing;
    d.sc2.drag = (std::max)(cfg.drag, kDragFloor); // the engine's load-time floor
    d.sc2.mass = cfg.mass;
    d.sc2.gravity3 = cfg.gravity3;
    d.sc2.friction = cfg.friction;
    d.sc2.bounce = cfg.bounce;
    d.sc2.noiseAmplitude = cfg.noiseAmplitude;
    d.sc2.noiseFrequency = cfg.noiseFrequency;
    d.sc2.noiseCoherence = cfg.noiseCoherence;
    d.sc2.noiseEdge = cfg.noiseEdge;
    for (i32 i = 0; i < WaveChannel::kCount; ++i)
        d.sc2.waveTypes[i] = cfg.waveTypes[i];
    d.sc2.lodReduce = cfg.lodReduce;
    d.sc2.lodCut = cfg.lodCut;
    d.sc2.hasSpline = !cfg.splines.empty();
    if (d.sc2.hasSpline)
        d.sc2.spline = cfg.splines.front(); // splineData = splineRibbons.ptr[0]
    return d;
}
} // namespace whiteout::flakes::renderer::ribbon
