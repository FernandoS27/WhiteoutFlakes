#include "renderer/particle/particle_adapters.h"

#include "particle.h" // ParticleEmitterConfig

#include <cmath>
#include <vector>

namespace whiteout::flakes::renderer::particle {

namespace {

FilterMode LegacyFilterToService(i32 legacy) {

    switch (legacy) {
    case 1:
        return FilterMode::AlphaKey;
    case 2:
        return FilterMode::Blend;
    case 3:
        return FilterMode::Additive;
    case 4:
        return FilterMode::Additive;
    case 5:
        return FilterMode::Modulate;
    case 6:
        return FilterMode::Modulate2X;
    default:
        return FilterMode::Blend;
    }
}

// WC3 stores its segment colours as bytes, so the key values are quantised the
// same way before being normalised to 0..1. Skipping this would shift every
// colour by up to one 255th relative to the original.
u8 Quant8(f32 v) {
    if (v <= 0.0f)
        return 0;
    if (v >= 255.0f)
        return 255;
    return static_cast<u8>(v);
}

Vector3f QuantColor(const Vector3f& rgb) {
    return {Quant8(rgb.x * 255.0f) / 255.0f, Quant8(rgb.y * 255.0f) / 255.0f,
            Quant8(rgb.z * 255.0f) / 255.0f};
}

// WC3 samples each segment over [bias, 1-bias] rather than [0, 1].
constexpr f32 kWc3SampleBias = 0.005f;

} // namespace

std::shared_ptr<const EmitterDesc> DescFromWc3Config(const ParticleEmitterConfig& cfg) {
    auto desc = std::make_shared<EmitterDesc>();

    desc->sheet.Set(static_cast<u32>(cfg.rows > 0 ? cfg.rows : 1),
                    static_cast<u32>(cfg.cols > 0 ? cfg.cols : 1));
    desc->lifeSpan = cfg.lifeSpan;
    desc->tailLength = cfg.tailLength;

    desc->hasHead = (cfg.particleType == 1 || cfg.particleType == 3);
    desc->hasTail = (cfg.particleType == 2 || cfg.particleType == 3);

    desc->sortZ = cfg.sortZ;
    desc->modelSpace = cfg.modelSpace;
    desc->xyQuads = cfg.xyQuad;

    // A line emitter is a plane emitter with the longitudinal sweep collapsed.
    desc->shape = std::make_shared<PlaneShape>();
    desc->longitude = cfg.lineEmitter ? 0.0f : 6.2831853071795864769f;

    desc->angularVelocity = 0.0f;
    desc->priorityPlane = cfg.priorityPlane;

    desc->material.textureId = cfg.textureId;
    desc->material.filterMode = LegacyFilterToService(cfg.filterMode);
    desc->material.unshaded = cfg.unshaded;
    desc->material.unfogged = cfg.unfogged;
    desc->material.replaceableId = cfg.replaceableId;

    desc->emission.mode = EmissionDesc::Mode::Continuous;
    desc->emission.squirtAtStart = cfg.squirt;

    // WC3's start/mid/end triple becomes a three-key curve. The mid key lands at
    // t = cfg.midTime exactly: its absolute time is midTime * lifeSpan, so
    // normalising by lifeSpan gives midTime back with no rounding.
    const f32 midT = cfg.midTime;

    auto& c = desc->curves;

    c.color.SetInterp(Interp::Linear);
    c.color.SetBias(kWc3SampleBias);
    c.color.AddKey(0.0f, QuantColor(cfg.startColor));
    c.color.AddKey(midT, QuantColor(cfg.midColor));
    c.color.AddKey(1.0f, QuantColor(cfg.endColor));

    c.alpha.SetInterp(Interp::Linear);
    c.alpha.SetBias(kWc3SampleBias);
    c.alpha.AddKey(0.0f, Quant8(cfg.startAlpha) / 255.0f);
    c.alpha.AddKey(midT, Quant8(cfg.midAlpha) / 255.0f);
    c.alpha.AddKey(1.0f, Quant8(cfg.endAlpha) / 255.0f);

    // WC3 scale is uniform; the curve carries 2D because M2/M3 are not.
    c.size.SetInterp(Interp::Linear);
    c.size.SetBias(kWc3SampleBias);
    c.size.AddKey(0.0f, Vector2f{cfg.startScale, cfg.startScale});
    c.size.AddKey(midT, Vector2f{cfg.midScale, cfg.midScale});
    c.size.AddKey(1.0f, Vector2f{cfg.endScale, cfg.endScale});

    c.headCells.SetBias(kWc3SampleBias);
    c.headCells.AddSegment(midT, cfg.headLifeStart, cfg.headLifeEnd, cfg.headLifeRepeat);
    c.headCells.AddSegment(1.0f, cfg.headDecayStart, cfg.headDecayEnd, cfg.headDecayRepeat);

    c.tailCells.SetBias(kWc3SampleBias);
    c.tailCells.AddSegment(midT, cfg.tailLifeStart, cfg.tailLifeEnd, cfg.tailLifeRepeat);
    c.tailCells.AddSegment(1.0f, cfg.tailDecayStart, cfg.tailDecayEnd, cfg.tailDecayRepeat);

    desc->coordSpace = kDefaultCoordSpace;

    return desc;
}

namespace {

// M2 blend index -> the service's filter modes. The loader's own file->id map
// (M2ParticleFile.h) collapses to these six draw states.
FilterMode M2BlendToFilter(i32 blend) {
    switch (blend) {
    case 0:
        // "Opaque". Particles draw in the transparent pass regardless, and this
        // enum has no opaque state, so it lands on the same default the MDX
        // adapter uses.
        return FilterMode::Blend;
    case 1:
        return FilterMode::AlphaKey;
    case 2:
        return FilterMode::Blend;
    case 3:
        return FilterMode::Additive;
    case 4:
        return FilterMode::Additive;
    case 5:
        return FilterMode::Modulate;
    case 6:
        return FilterMode::Modulate2X;
    default:
        return FilterMode::Blend;
    }
}

// Display-referred -> linear. The record's colour keys were authored against a
// gamma display, so an HDR profile that shades linearly has to de-gamma them or
// every particle reads washed out.
inline f32 Degamma(f32 c) {
    return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

// M2 tracks are already normalised to [0,1] over the particle's life and are
// linearly interpolated with no sampling bias — WC3's 0.99/0.005 skew is a WC3
// quirk and must not leak into another format's curves.
template <class Curve, class Values>
void FillCurve(Curve& curve, const std::vector<f32>& times, const Values& values) {
    curve.SetInterp(Interp::Linear);
    if (values.empty())
        return;
    if (values.size() == 1 || times.size() != values.size()) {
        curve.AddKey(0.0f, values[0]);
        return;
    }
    for (usize i = 0; i < values.size(); ++i)
        curve.AddKey(times[i], values[i]);
}

// Solve the FollowPosition factor's line through the record's two
// (speed, scale) sample points, as CParticleEmitter2::SetFollowParams does.
// A degenerate pair (the two speeds equal) clears both terms rather than
// dividing, which makes the factor zero and the feature inert.
void SolveFollowLine(EmitterDesc& desc, const M2ParticleEmitterConfig& cfg) {
    const f32 ds = cfg.followSpeed2 - cfg.followSpeed1;
    if (std::fabs(ds) < 2.3841858e-7f) {
        desc.followBias = 0.0f;
        desc.followSlope = 0.0f;
        return;
    }
    const f32 slope = (cfg.followScale2 - cfg.followScale1) / ds;
    desc.followSlope = slope;
    desc.followBias = cfg.followScale1 - slope * cfg.followSpeed1;
}

// A cell track is a sequence of held integer cells, not an interpolated value,
// so each key becomes a one-cell segment ending where the next begins.
void FillCells(CellAnimTrack& track, const std::vector<f32>& times,
               const std::vector<f32>& values) {
    if (values.empty())
        return;
    if (values.size() == 1 || times.size() != values.size()) {
        const i32 cell = static_cast<i32>(values[0]);
        track.AddSegment(1.0f, cell, cell, 1);
        return;
    }
    for (usize i = 0; i < values.size(); ++i) {
        const f32 end = (i + 1 < values.size()) ? times[i + 1] : 1.0f;
        const i32 cell = static_cast<i32>(values[i]);
        track.AddSegment(end, cell, cell, 1);
    }
}

} // namespace

std::shared_ptr<const EmitterDesc> DescFromM2Config(const M2ParticleEmitterConfig& cfg,
                                                    bool linearColor) {
    auto desc = std::make_shared<EmitterDesc>();

    desc->sheet.Set(static_cast<u32>(cfg.rows > 0 ? cfg.rows : 1),
                    static_cast<u32>(cfg.cols > 0 ? cfg.cols : 1));
    desc->lifeSpan = cfg.lifeSpan;
    desc->lifespanVariation = cfg.lifespanVariation;
    desc->emissionRateVariation = cfg.emissionRateVariation;
    desc->tailLength = cfg.tailLength;
    desc->hasHead = cfg.hasHead;
    desc->hasTail = cfg.hasTail;
    desc->sortZ = cfg.sortZ;
    desc->modelSpace = cfg.modelSpace;
    desc->xyQuads = cfg.xyQuad;
    desc->priorityPlane = cfg.priorityPlane;

    switch (cfg.generator) {
    case M2ParticleEmitterConfig::Generator::Sphere:
        desc->shape = std::make_shared<WowSphereShape>(cfg.hemisphereUp);
        break;
    case M2ParticleEmitterConfig::Generator::Spline:
        desc->shape = std::make_shared<WowSplineShape>(cfg.splinePoints);
        break;
    case M2ParticleEmitterConfig::Generator::Bone:
        desc->shape = std::make_shared<WowBoneShape>();
        break;
    case M2ParticleEmitterConfig::Generator::Plane:
    default:
        desc->shape = std::make_shared<WowPlaneShape>();
        break;
    }

    desc->velocityOrient = cfg.velocityOrient;
    desc->inheritBoneScale = cfg.inheritBoneScale;
    desc->negateSpinRandom = cfg.negateSpinRandom;
    desc->clampTailToAge = cfg.clampTailToAge;
    desc->offsetHeadBySpin = cfg.offsetHeadBySpin;
    desc->unscaledSizeVariation = cfg.unscaledSizeVariation;
    desc->chooseRandomTexture = cfg.chooseRandomTexture;
    desc->randFlipbookStart = cfg.randFlipbookStart;

    desc->baseSpin = cfg.baseSpin;
    desc->baseSpinVariation = cfg.baseSpinVariation;
    desc->spinSpeed = cfg.spinSpeed;
    desc->spinSpeedVariation = cfg.spinSpeedVariation;
    desc->sizeVariation = cfg.scaleVariation;

    desc->twinkleSpeed = cfg.twinkleSpeed;
    desc->twinklePercent = cfg.twinklePercent;
    // SetTwinkleScale keeps the record's {min, max} as base and span, so the
    // multiplier is uniform in [min, max] — and a record that stores {0,0}
    // scales its particles away, which is the record's choice, not a fallback
    // to be second-guessed here.
    desc->twinkleBase = cfg.twinkleScale.x;
    desc->twinkleVary = cfg.twinkleScale.y - cfg.twinkleScale.x;

    desc->randomEmissionSpacing = cfg.randomEmissionSpacing;
    desc->lodIgnoreDistance = cfg.lodIgnoreDistance;
    desc->implosionFilter = cfg.implosionFilter;
    desc->followPosition = cfg.followPosition;
    desc->inheritVelocity = cfg.inheritVelocity;
    desc->inheritVelocityScale = cfg.inheritVelocity ? cfg.inheritVelocityScale : 0.0f;
    SolveFollowLine(*desc, cfg);

    desc->motion.drag = cfg.drag;
    desc->motion.wind = cfg.windVector;

    desc->material.textureId = cfg.textureId;
    desc->material.filterMode = M2BlendToFilter(cfg.filterMode);
    desc->material.unshaded = cfg.unshaded;
    desc->material.unfogged = cfg.unfogged;
    desc->material.multiTexture = cfg.multiTexture;
    desc->material.multiTexUse3Colors = cfg.multiTexUse3Colors;
    desc->material.multiTexModx4 = cfg.multiTexModx4;
    desc->material.textureId2 = cfg.textureId2;
    desc->material.textureId3 = cfg.textureId3;

    desc->refraction = cfg.refraction;
    desc->multiTexture = cfg.multiTexture;
    for (usize layer = 0; layer < 2; ++layer) {
        desc->multiTexScale[layer] = cfg.multiTexScale[layer];
        desc->multiTexScrollMid[layer] = cfg.multiTexScrollMid[layer];
        desc->multiTexScrollRange[layer] = cfg.multiTexScrollRange[layer];
    }

    desc->emission.mode = EmissionDesc::Mode::Continuous;
    // Squirt is the same mechanism in both clients: the emitter stops emitting
    // continuously and instead bursts `(int)rate` particles each time the rate
    // track rises through zero. WoW's loader expresses that by clearing the
    // continuous-emission bit so only the burst path can fire; WC3 gates
    // visibility on the same flag. The rising edge itself is detected once, at
    // the actor layer, for both — see ApplyParticleFrameStates.
    desc->emission.squirtAtStart = cfg.squirt;

    auto& c = desc->curves;
    if (linearColor) {
        std::vector<Vector3f> linear;
        linear.reserve(cfg.colorValues.size());
        for (const Vector3f& v : cfg.colorValues)
            linear.push_back({Degamma(v.x), Degamma(v.y), Degamma(v.z)});
        FillCurve(c.color, cfg.colorTimes, linear);
    } else {
        FillCurve(c.color, cfg.colorTimes, cfg.colorValues);
    }
    FillCurve(c.alpha, cfg.alphaTimes, cfg.alphaValues);
    FillCurve(c.size, cfg.scaleTimes, cfg.scaleValues);
    FillCells(c.headCells, cfg.headCellTimes, cfg.headCellValues);
    FillCells(c.tailCells, cfg.tailCellTimes, cfg.tailCellValues);

    // Model particles: same sim, same spawn shape, different output. The scale
    // stays 1 — an M2 model particle takes its size from the emitter's scale
    // track every frame, not from a fixed multiplier the way PE1 does.
    if (!cfg.geometryModelPath.empty()) {
        desc->output = ParticleOutput::ChildModel;
        desc->childModelPath = cfg.geometryModelPath;
    }
    // Not an output kind: a trail emitter's particles are ordinary billboards.
    // What this names is a second model whose emitters ride this one's
    // particles, which the loader resolves and attaches.
    desc->trailModelPath = cfg.recursionModelPath;
    desc->tumbleBase = cfg.tumbleMin;
    desc->tumbleVary = {cfg.tumbleMax.x - cfg.tumbleMin.x, cfg.tumbleMax.y - cfg.tumbleMin.y,
                        cfg.tumbleMax.z - cfg.tumbleMin.z};

    desc->coordSpace = kDefaultCoordSpace;
    return desc;
}

model::FrameState::ParticleFrameState TrailStateFromM2Config(const M2ParticleEmitterConfig& cfg) {
    model::FrameState::ParticleFrameState st{};
    st.emitterId = -1;
    st.emissionRate = cfg.initial.emissionRate;
    st.speed = cfg.initial.speed;
    st.variation = cfg.initial.variation;
    st.coneAngle = cfg.initial.coneAngle;
    st.horizontalRange = cfg.initial.horizontalRange;
    st.width = cfg.initial.width;
    st.length = cfg.initial.length;
    st.zSource = cfg.initial.zSource;
    st.lifeSpan = cfg.initial.lifeSpan;
    st.gravityVector = cfg.initial.gravityVector;
    st.gravity = -cfg.initial.gravityVector.z;
    st.hasGravityVector = true;
    st.enabled = true;
    st.visibility = 1.0f;
    // A trail is driven particle by particle and never emits from its own
    // position, so a squirt edge cannot reach it.
    st.squirting = false;
    st.boneGenerator = cfg.generator == M2ParticleEmitterConfig::Generator::Bone;
    return st;
}

std::shared_ptr<const EmitterDesc>
DescFromWc3ChildModelConfig(const model::PE1EmitterConfig& cfg) {
    auto desc = std::make_shared<EmitterDesc>();

    desc->output = ParticleOutput::ChildModel;
    desc->shape = std::make_shared<ConeShape>();
    desc->childModelPath = cfg.modelPath;
    desc->childScale = cfg.scale;
    desc->lifeSpan = cfg.lifespan;

    // PE1 animates its own longitude, so this is only the value the emitter
    // starts with. No curves: death is age against lifespan, which is exactly
    // why that had to stop depending on the key count.
    desc->longitude = 0.0f;
    desc->coordSpace = kDefaultCoordSpace;

    return desc;
}

} // namespace whiteout::flakes::renderer::particle
