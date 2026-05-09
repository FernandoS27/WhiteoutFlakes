#include "renderer/particle/plane_emitter.h"
#include "particle.h"

#include <cmath>

namespace whiteout::flakes::renderer::particle {

PlaneEmitter::PlaneEmitter() {
    type_ = EmitterType::Plane;
}

void ApplyInit(PlaneEmitter& e, const PlaneEmitterInit& init) {
    e.SetTextureDimensions(init.textureRows, init.textureCols);
    e.SetKey(0, init.keys[0]);
    e.SetKey(1, init.keys[1]);
    e.SetLifeSpan(init.lifeSpan);
    e.SetParticleStyle(init.hasHead, init.hasTail, init.tailLength);
    e.SetAngularVelocity(init.angularVelocity);
    e.SetSortZ(init.sortZ);
    e.SetUseModelSpace(init.modelSpace);
    e.SetXYQuads(init.xyQuads);
    e.SetPriorityPlane(init.priorityPlane);
    e.SetReplaceableId(init.replaceableId);
    e.SetMaterial(init.material);
    e.SetCoordSpace(init.coordSpace);
    e.SetLongitude(init.longitude);
    if (init.squirtAtStart) e.Squirt();
}

namespace {

FilterMode LegacyFilterToService(i32 legacy) {

    switch (legacy) {
        case 1:  return FilterMode::AlphaKey;
        case 2:  return FilterMode::Blend;
        case 3:  return FilterMode::Additive;
        case 4:  return FilterMode::Additive;
        case 5:  return FilterMode::Modulate;
        case 6:  return FilterMode::Modulate2X;
        default: return FilterMode::Blend;
    }
}

ImVector RgbFloatToImVector(const Vector3f& rgb, f32 alpha) {

    auto clamp8 = [](f32 v) -> u8 {
        if (v <= 0.0f) return 0;
        if (v >= 255.0f) return 255;
        return static_cast<u8>(v);
    };
    return {
        clamp8(alpha),
        clamp8(rgb.x * 255.0f),
        clamp8(rgb.y * 255.0f),
        clamp8(rgb.z * 255.0f),
    };
}

}

PlaneEmitterInit InitFromLegacyConfig(const ParticleEmitterConfig& cfg) {
    PlaneEmitterInit init;

    init.textureRows = static_cast<u32>(cfg.rows  > 0 ? cfg.rows  : 1);
    init.textureCols = static_cast<u32>(cfg.cols  > 0 ? cfg.cols  : 1);
    init.lifeSpan    = cfg.lifeSpan;
    init.tailLength  = cfg.tailLength;

    init.hasHead = (cfg.particleType == 1 || cfg.particleType == 3);
    init.hasTail = (cfg.particleType == 2 || cfg.particleType == 3);

    init.sortZ      = cfg.sortZ;
    init.modelSpace = cfg.modelSpace;
    init.xyQuads    = cfg.xyQuad;

    init.longitude = cfg.lineEmitter ? 0.0f : 6.2831853071795864769f;

    init.angularVelocity = 0.0f;
    init.priorityPlane   = cfg.priorityPlane;
    init.replaceableId   = cfg.replaceableId;

    init.material.textureId     = cfg.textureId;
    init.material.filterMode    = LegacyFilterToService(cfg.filterMode);
    init.material.unshaded      = cfg.unshaded;
    init.material.unfogged      = cfg.unfogged;
    init.material.replaceableId = cfg.replaceableId;

    init.squirtAtStart = cfg.squirt;

    const ImVector startColor = RgbFloatToImVector(cfg.startColor, cfg.startAlpha);
    const ImVector midColor   = RgbFloatToImVector(cfg.midColor,   cfg.midAlpha);
    const ImVector endColor   = RgbFloatToImVector(cfg.endColor,   cfg.endAlpha);

    auto& k0 = init.keys[0];
    k0.endTime        = cfg.midTime * cfg.lifeSpan;
    k0.startColor     = startColor;
    k0.endColor       = midColor;
    k0.startScale     = cfg.startScale;
    k0.endScale       = cfg.midScale;
    k0.headCellStart  = cfg.headLifeStart;
    k0.headCellEnd    = cfg.headLifeEnd;
    k0.headCellRepeat = cfg.headLifeRepeat;
    k0.tailCellStart  = cfg.tailLifeStart;
    k0.tailCellEnd    = cfg.tailLifeEnd;
    k0.tailCellRepeat = cfg.tailLifeRepeat;

    auto& k1 = init.keys[1];
    k1.endTime        = cfg.lifeSpan;
    k1.startColor     = midColor;
    k1.endColor       = endColor;
    k1.startScale     = cfg.midScale;
    k1.endScale       = cfg.endScale;
    k1.headCellStart  = cfg.headDecayStart;
    k1.headCellEnd    = cfg.headDecayEnd;
    k1.headCellRepeat = cfg.headDecayRepeat;
    k1.tailCellStart  = cfg.tailDecayStart;
    k1.tailCellEnd    = cfg.tailDecayEnd;
    k1.tailCellRepeat = cfg.tailDecayRepeat;

    init.coordSpace = kDefaultCoordSpace;

    return init;
}

void PlaneEmitter::CreateParticle(Particle2& p, f32 elapsed) {

    f32 r = CRandom::real_(randSeed_);
    p.keyFrame = 0;
    p.age = elapsed * r;

    f32 heightTerm = CRandom::reals_(randSeed_) * height_ * 0.5f;
    f32 widthTerm  = CRandom::reals_(randSeed_) * width_  * 0.5f;

    Vector3f localPos{ widthTerm, heightTerm, 0.0f };
    if ((flags_ & kFlagUseModelSpace) != 0) {
        p.position = localPos;
    } else {
        p.position = whiteout::transform_point(localPos, modelToWorld_);
    }

    f32 rotY = latitude_  * CRandom::reals_(randSeed_);
    f32 rotZ = longitude_ * CRandom::reals_(randSeed_);

    f32 speed = CalcVelocity();

    f32 vx = speed * std::sin(rotY);
    f32 vz = speed * std::cos(rotY);

    f32 vy = vx * std::sin(rotZ);
    vx     = vx * std::cos(rotZ);

    Vector3f localVel{ vx, vy, vz };
    if ((flags_ & kFlagUseModelSpace) != 0) {
        p.velocity = localVel;
    } else {
        p.velocity = whiteout::transform_normal(localVel, modelToWorld_);
    }
}

}
