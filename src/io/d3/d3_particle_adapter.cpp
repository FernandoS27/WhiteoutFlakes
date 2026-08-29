#include "io/d3/d3_particle_adapter.h"

#include "io/d3/d3_types.h"

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
    // 0xAABBGGRR — RED IS THE LOW BYTE, the same order `unpackVertexColor`
    // reads an `.app` vertex in. Two shipped files that name their own colour
    // settle it: `a1dun_cave_TorchGlow_Orange` stops on 0x003CFFF0 and
    // `a1dun_jail_Embers_blue` on 0xFFFA7D77, and only this order makes the
    // first orange and the second blue. Corpus-wide the skew is the same
    // shape — over the 2,005 fire/flame/torch/ember files the low byte is the
    // largest on 3,027 non-grey stops against 559 for the high one, which is
    // just to say that fire is warm.
    return {static_cast<f32>(c & 0xFFu) / 255.0f, static_cast<f32>((c >> 8) & 0xFFu) / 255.0f,
            static_cast<f32>((c >> 16) & 0xFFu) / 255.0f,
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

/// The four stage types `Particle_DrawBatch` binds, in the order it binds them:
/// `sys+300`, `+304`, `+308`, `+312`. A `.prt`'s material carries entries of
/// twelve other types between them (300 of 35,631) and no particle pass
/// declares one, so the order is also the filter.
constexpr i32 kParticleStageTypes[pd3::MaterialDesc::kMaxLayers] = {1, 19, 12, 14};

void BuildMaterial(const d3n::Particle& prt, pd3::MaterialDesc& out) {
    out.snoShaderMap = prt.tMaterial.snoShaderMap.valid() ? prt.tMaterial.snoShaderMap.id : -1;
    for (const i32 want : kParticleStageTypes) {
        for (const auto& e : prt.tMaterial.arTextures) {
            if (D3TextureTypeOf(e) != want)
                continue;
            pd3::MaterialLayer& L = out.layers[out.layerCount++];
            L.textureSno = e.snoTexture.valid() ? e.snoTexture.id : -1;
            L.rawType = want;
            L.wrapFlags = static_cast<u32>(D3UvFlagsOf(e) & kD3UvFlagWrapMask);
            L.uv = D3ReadUvXform(e);
            break; // A type never repeats inside one material — see d3_types.h.
        }
    }
}

} // namespace

std::shared_ptr<pd3::EmitterDesc> BuildD3EmitterDesc(const d3n::Particle& prt, i32 snoId) {
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

    BuildMaterial(prt, d->d3mat);
    // The narrow view the shared draw list carries. The texture id is still -1
    // here: a layer's id is an index into the OWNING ACTOR's texture scope, and
    // the file does not know which actor it is about to ride. D3BindParticleTextures
    // fills both halves in.
    d->material.filterMode = renderer::particle::FilterMode::Additive;
    d->material.unshaded = true;
    d->material.unfogged = false;

    d->DeriveCapabilities();
    return d;
}

std::shared_ptr<const pd3::EmitMesh> BuildD3EmitMesh(const d3n::Appearances& app,
                                                     std::span<const D3SubObjectRef> emitted) {
    auto mesh = std::make_shared<pd3::EmitMesh>();
    const d3n::GeoSet* sets[2] = {&app.tGeoSet0, &app.tGeoSet1};

    for (const D3SubObjectRef& r : emitted) {
        const auto& subs = sets[r.geoSet & 1]->arSubObjects;
        pd3::EmitMesh::SubMesh sm;
        sm.firstTri = static_cast<u32>(mesh->tris.size() / 3);
        if (r.index >= subs.size()) {
            mesh->subs.push_back(sm);
            continue;
        }
        const d3n::SubObject& sub = subs[r.index];

        const u32 base = static_cast<u32>(mesh->rest.size());
        const usize influenced =
            (std::min)(sub.arVertices.size(), sub.arVertexInfluences.size());
        for (usize v = 0; v < sub.arVertices.size(); ++v) {
            mesh->rest.push_back(sub.arVertices[v].vPosition);
            // Bone 0 at weight 0 for a vertex past a truncated influence array,
            // which SkinEmitMeshVertex reads as "no skin" and leaves at rest —
            // the same degradation GetSkinWeights makes for the mesh itself.
            std::array<i32, 3> b{0, 0, 0};
            Vector3f w{0, 0, 0};
            if (v < influenced) {
                const d3n::VertInfluences& src = sub.arVertexInfluences[v];
                const d3n::Influence* three[3] = {&src.tInfluence0, &src.tInfluence1,
                                                  &src.tInfluence2};
                f32* lane[3] = {&w.x, &w.y, &w.z};
                for (i32 k = 0; k < 3; ++k) {
                    b[k] = three[k]->nBoneIndex;
                    *lane[k] = three[k]->flWeight;
                }
            } else if (sub.arVertexInfluences.empty() && sub.nBoneIndex >= 0) {
                // A rigid sub-object names one bone for the whole of itself.
                b[0] = sub.nBoneIndex;
                w.x = 1.0f;
            }
            mesh->bones.push_back(b);
            mesh->weights.push_back(w);
        }

        f32 run = 0.0f;
        for (usize i = 0; i + 2 < sub.arIndices.size(); i += 3) {
            const u32 i0 = base + sub.arIndices[i + 0];
            const u32 i1 = base + sub.arIndices[i + 1];
            const u32 i2 = base + sub.arIndices[i + 2];
            if (i0 >= mesh->rest.size() || i1 >= mesh->rest.size() || i2 >= mesh->rest.size())
                continue;
            mesh->tris.push_back(i0);
            mesh->tris.push_back(i1);
            mesh->tris.push_back(i2);
            const Vector3f& a = mesh->rest[i0];
            const Vector3f& p = mesh->rest[i1];
            const Vector3f& q = mesh->rest[i2];
            const Vector3f e0{p.x - a.x, p.y - a.y, p.z - a.z};
            const Vector3f e1{q.x - a.x, q.y - a.y, q.z - a.z};
            const Vector3f n{e0.y * e1.z - e0.z * e1.y, e0.z * e1.x - e0.x * e1.z,
                             e0.x * e1.y - e0.y * e1.x};
            run += 0.5f * std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
            mesh->areaCdf.push_back(run);
            ++sm.triCount;
        }
        mesh->subs.push_back(sm);
    }

    // A sub-object with no skeleton anywhere leaves the two skin arrays as dead
    // weight; drop them so SkinEmitMeshVertex takes its rest-pose early-out on
    // the 63% of the corpus that has no bones at all.
    bool anyWeight = false;
    for (const Vector3f& w : mesh->weights)
        anyWeight |= (w.x > 0.0f || w.y > 0.0f || w.z > 0.0f);
    if (!anyWeight) {
        mesh->bones.clear();
        mesh->bones.shrink_to_fit();
        mesh->weights.clear();
        mesh->weights.shrink_to_fit();
    }

    return mesh->Empty() ? nullptr : std::shared_ptr<const pd3::EmitMesh>(std::move(mesh));
}

} // namespace whiteout::flakes::io::d3
