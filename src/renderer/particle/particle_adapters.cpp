#include "renderer/particle/particle_adapters.h"

#include "particle.h" // ParticleEmitterConfig

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
