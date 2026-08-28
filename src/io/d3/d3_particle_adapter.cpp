#include "io/d3/d3_particle_adapter.h"

#include "whiteout/sno/d3/native/types.h"

#include <cmath>

namespace whiteout::flakes::io::d3 {

namespace d3n = ::whiteout::sno::d3::native;
using pd3::Path;
using pd3::PathNode;

namespace {

/// Frames at 60 fps -> seconds. Every `tm*` field in the file is a frame count
/// and the engine multiplies all of them by 0.016667 on load.
constexpr f32 kFrameToSeconds = 1.0f / 60.0f;

void CopyHeader(const d3n::InterpolationPathHeader& h, Path& out) {
    out.loopStart = h.flLoopStart;
    out.loopEnd = h.flLoopEnd;
    out.distribution = h.eDistribution;
    out.driver.mode = h.tDriver.nMode;
    out.driver.lo = h.tDriver.flMin;
    out.driver.hi = h.tDriver.flMax;
}

// Four node shapes cover all ten path types: scalar float, scalar int, vector,
// and packed colour. Everything else is a units difference the SIMULATION
// applies (x60 for velocity, x3600 for acceleration), not a storage one, so it
// deliberately does not happen here — the raw sample is what a trace compares.

template <typename PathT>
Path FromScalar(const PathT& src) {
    Path p;
    CopyHeader(src.tHeader, p);
    p.components = 1;
    p.nodes.reserve(src.arNodes.size());
    for (const auto& n : src.arNodes) {
        PathNode node;
        node.start = {static_cast<f32>(n.flStart), 0, 0, 0};
        node.end = {static_cast<f32>(n.flEnd), 0, 0, 0};
        node.time = n.flTime;
        p.nodes.push_back(node);
    }
    return p;
}

Path FromInt(const d3n::IntPath& src) {
    Path p;
    CopyHeader(src.tHeader, p);
    p.components = 1;
    p.nodes.reserve(src.arNodes.size());
    for (const auto& n : src.arNodes) {
        PathNode node;
        node.start = {static_cast<f32>(n.nStart), 0, 0, 0};
        node.end = {static_cast<f32>(n.nEnd), 0, 0, 0};
        node.time = n.flTime;
        p.nodes.push_back(node);
    }
    return p;
}

Path FromTime(const d3n::TimePath& src) {
    Path p;
    CopyHeader(src.tHeader, p);
    p.components = 1;
    p.nodes.reserve(src.arNodes.size());
    for (const auto& n : src.arNodes) {
        PathNode node;
        node.start = {static_cast<f32>(n.tmStart), 0, 0, 0};
        node.end = {static_cast<f32>(n.tmEnd), 0, 0, 0};
        node.time = n.flTime;
        p.nodes.push_back(node);
    }
    return p;
}

template <typename PathT>
Path FromVector(const PathT& src) {
    Path p;
    CopyHeader(src.tHeader, p);
    p.components = 3;
    p.nodes.reserve(src.arNodes.size());
    for (const auto& n : src.arNodes) {
        PathNode node;
        node.start = {n.vStart.x, n.vStart.y, n.vStart.z, 0};
        node.end = {n.vEnd.x, n.vEnd.y, n.vEnd.z, 0};
        node.time = n.flTime;
        p.nodes.push_back(node);
    }
    return p;
}

Vector4f UnpackColor(u32 c) {
    // 0xAARRGGBB, the packing the engine writes at particle+232 and the same
    // one the `.app` vertex colours use.
    return {static_cast<f32>((c >> 16) & 0xFFu) / 255.0f,
            static_cast<f32>((c >> 8) & 0xFFu) / 255.0f, static_cast<f32>(c & 0xFFu) / 255.0f,
            static_cast<f32>((c >> 24) & 0xFFu) / 255.0f};
}

Path FromColor(const d3n::ColorPath& src) {
    Path p;
    CopyHeader(src.tHeader, p);
    p.components = 4;
    p.nodes.reserve(src.arNodes.size());
    for (const auto& n : src.arNodes) {
        PathNode node;
        node.start = UnpackColor(n.dwStartColor);
        node.end = UnpackColor(n.dwEndColor);
        node.time = n.flTime;
        p.nodes.push_back(node);
    }
    return p;
}

} // namespace

std::shared_ptr<const pd3::EmitterDesc> BuildD3EmitterDesc(const d3n::Particle& prt, i32 snoId) {
    auto d = std::make_shared<pd3::EmitterDesc>();

    d->snoId = (snoId != -1) ? snoId : prt.dwSnoId;
    d->systemType = prt.eSystemType;
    d->prtFlags = static_cast<u32>(prt.dwFlags);
    d->renderMode = prt.nRenderMode;

    d->lifetime = static_cast<f32>(prt.tmLifetime) * kFrameToSeconds;
    d->emissionPeriod = static_cast<f32>(prt.tmEmissionPeriod) * kFrameToSeconds;
    d->preSimulate = static_cast<f32>(prt.tmPreSimulate) * kFrameToSeconds;

    d->mass = prt.flMass;
    d->maxInstances = prt.nMaxInstances;
    d->maxDistance = prt.flMaxDistance;
    d->cameraDistScale = prt.flCameraDistScale;

    d->burstZOffset = prt.flBurstZOffset;
    d->swayFrequency = prt.flSwayFrequency;
    d->swayDamping = prt.flSwayDamping;
    d->swayMaxOffset = prt.flSwayMaxOffset;
    d->swayGustAmount = prt.flSwayGustAmount;
    d->swayBaseAmount = prt.flSwayBaseAmount;

    d->snoActor = prt.snoActor.valid() ? prt.snoActor.id : -1;

    // ---- emitter shape ----
    d->shape = static_cast<pd3::Shape>(prt.tEmitter.eEmitterShape);
    d->shapeExtent0 = FromScalar(prt.tEmitter.tShapeExtent0);
    d->shapeExtent1 = FromScalar(prt.tEmitter.tShapeExtent1);
    d->shapeExtent2 = FromVector(prt.tEmitter.tShapeExtent2);

    // ---- the thirteen emitter channels ----
    d->channels[pd3::kChSizeScale] = FromScalar(prt.arSizeScalePath);
    d->channels[pd3::kChTargetCount] = FromInt(prt.arCountPath);
    d->channels[pd3::kChEffectScale] = FromScalar(prt.arEffectScalePath);
    d->channels[pd3::kChParticleLife] = FromTime(prt.arParticleLifePath);
    d->channels[pd3::kChBirthSize] = FromScalar(prt.arInitialSizePath);
    d->channels[pd3::kChSpreadAngle] = FromScalar(prt.arSpreadAnglePath);
    d->channels[pd3::kChInitialVelocity] = FromVector(prt.arInitialVelocityPath);
    d->channels[pd3::kChWorldVelocity] = FromVector(prt.arInitialVelocityWorldPath);
    d->channels[pd3::kChEmissionRate] = FromScalar(prt.arEmissionRatePath);
    // Slots 9 and 10. Declaration order permits either assignment and can never
    // decide it; the corpus does — 194 files set slot 9 with no count and no
    // rate channel, and every one of them is named `*trail*`. A trail is
    // exactly "emit per unit of distance moved".
    d->channels[pd3::kChDistanceRate] = FromScalar(prt.arEmitterRatePathA);
    d->channels[pd3::kChSpeedLifeCut] = FromScalar(prt.arEmitterRatePathB);
    d->channels[pd3::kChRetiredVector] = FromVector(prt.arUnknownEmitterVectorPath);
    d->channels[pd3::kChRetiredFloat] = FromScalar(prt.arUnknownEmitterFloatPath);

    // ---- the twenty-four particle channels ----
    d->channels[pd3::kChColor] = FromColor(prt.arColorPath);
    d->channels[pd3::kChScale] = FromScalar(prt.arScalePath);
    d->channels[pd3::kChAlpha] = FromScalar(prt.arAlphaPath);
    d->channels[pd3::kChSize] = FromScalar(prt.arSizePath);
    d->channels[pd3::kChChildScalar] = FromScalar(prt.arSize2Path);
    d->channels[pd3::kChRollAngle] = FromScalar(prt.arRotationPath);
    d->channels[pd3::kChRollRate] = FromScalar(prt.arRotationRatePath);
    d->channels[pd3::kChSpinRate] = FromScalar(prt.arRotation2RatePath);
    d->channels[pd3::kChSpinAngle] = FromScalar(prt.arRotation2Path);
    d->channels[pd3::kChSpinAxis] = FromVector(prt.arAxisPath);
    d->channels[pd3::kChOrbitRadius] = FromScalar(prt.arScalarPathCh07);
    d->channels[pd3::kChOrbitRadSpeed] = FromScalar(prt.arRatePathCh08);
    d->channels[pd3::kChOrbitAngSpeed] = FromScalar(prt.arSpinRatePathCh09);
    d->channels[pd3::kChOrbitAxis] = FromVector(prt.arAxis2Path);
    d->channels[pd3::kChRadialOffset] = FromScalar(prt.arScalarPathCh11);
    d->channels[pd3::kChRadialSpeed] = FromScalar(prt.arRatePathCh12);
    d->channels[pd3::kChOffsetA] = FromVector(prt.arOffsetPath);
    d->channels[pd3::kChVelocityA] = FromVector(prt.arVelocityPath);
    d->channels[pd3::kChAccelA] = FromVector(prt.arAccelerationPath);
    d->channels[pd3::kChOffsetB] = FromVector(prt.arOffset2Path);
    d->channels[pd3::kChVelocityB] = FromVector(prt.arVelocity2Path);
    d->channels[pd3::kChAccelB] = FromVector(prt.arAcceleration2Path);
    d->channels[pd3::kChSeekSpeed] = FromScalar(prt.arRatePathCh13);
    d->channels[pd3::kChSeekOffset] = FromScalar(prt.arScalarPathCh14);

    // The ShaderMap id is the whole of a particle's material resolve: a
    // particle skips the UberMaterial, the Material SNO and the texture-entry
    // array that geometry walks, and binds four textures by STAGE TYPE off the
    // system record. The renderer-side half of that lands with the shading
    // phase; carry the id so it has something to resolve.
    d->material.textureId = prt.tMaterial.snoShaderMap.valid() ? prt.tMaterial.snoShaderMap.id : -1;
    d->material.filterMode = renderer::particle::FilterMode::Additive;
    d->material.unshaded = true;
    d->material.unfogged = false;

    d->DeriveCapabilities();
    return d;
}

} // namespace whiteout::flakes::io::d3
