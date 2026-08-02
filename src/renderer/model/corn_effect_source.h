#pragma once

// CornEffectSource — a minimal IModelSource that plays a standalone corn
// effect (.pkb / .pkfx) on its own, with no model/skeleton/animation around it.
//
// MDX files carry geometry + a list of animation sequences the viewer lets you
// switch between; a .pkb is just one particle effect, so this source exposes no
// meshes and a single always-on CornFx emitter that simply runs the effect. It
// Lives in the library rather than a host: it is a file format's model
// source, not UI, and `LoaderView::SpawnEffect` hands it to bindings that
// cannot implement `IModelSource` themselves.
//
// It reuses the whole spawn pipeline: ModelLoader::SpawnUnitFromSource snapshots
// this via Build() (which routes GetCornEmitterInits into the ModelTemplate),
// stages one CornEffectsEmitter for the .pkb, and calls Evaluate() each frame —
// where we emit a single visible CornFrameState so the effect keeps playing.
//
// A one looping placeholder sequence keeps the actor ticking so Evaluate runs
// every frame; the effect's own runtime governs whether it loops or plays once.

#include "whiteout/flakes/model_source.h"

#include <string>
#include <utility>
#include <vector>

namespace whiteout::flakes::renderer::model {

class CornEffectSource : public IModelSource {
public:
    explicit CornEffectSource(std::string pkbPath) : pkbPath_(std::move(pkbPath)) {}

    // No geometry, skeleton, textures, materials, or non-CornFx emitters.
    std::vector<MeshData> GetMeshes() override {
        return {};
    }
    std::vector<TextureData> GetTextures() override {
        return {};
    }
    std::vector<MaterialData> GetMaterials() override {
        return {};
    }
    SkeletonData GetSkeleton() override {
        return {};
    }
    std::vector<SkinWeightData> GetSkinWeights() override {
        return {};
    }
    std::vector<ParticleEmitterConfig> GetParticleConfigs() override {
        return {};
    }
    std::vector<effects::RibbonEmitterConfig> GetRibbonConfigs() override {
        return {};
    }
    std::vector<CollisionShapeData> GetCollisionShapes() override {
        return {};
    }

    // The whole point: one CornFx emitter bound to the .pkb. An empty
    // visibility guide means "always on", so the effect plays as soon as the
    // actor is visible and keeps running per its own runtime lifetime.
    std::vector<CornEmitterInit> GetCornEmitterInits() override {
        CornEmitterInit c;
        c.emitterId = 0;
        c.pkbPath = pkbPath_;
        c.animVisibilityGuide = ""; // always on
        // Identity multipliers: play the effect at its authored rate/life/speed
        // (per-frame CornFrameState also defaults these to 1, but seed them so
        // the very first frame before Evaluate runs emits too).
        c.defaultEmissionRate = 1.0f;
        c.defaultLifeSpan = 1.0f;
        c.defaultSpeed = 1.0f;
        c.defaultColor = {1, 1, 1, 1};
        return {c};
    }

    // One looping placeholder sequence so the actor is "animated" and Evaluate
    // is invoked every frame (the effect, not this clock, defines playback).
    std::vector<SequenceInfo> GetSequences() const override {
        SequenceInfo s;
        s.name = "Effect";
        s.startMs = 0;
        s.endMs = 1000;
        s.nonLooping = false;
        return {s};
    }

    FrameState Evaluate(i32 /*sequenceIdx*/, i32 /*timeMs*/, i32 /*globalTimeMs*/,
                        const Matrix44f& worldTransform,
                        const Vector3f& /*cameraPos*/) const override {
        FrameState fs;
        FrameState::CornFrameState cs; // defaults: visible, identity, full multipliers
        cs.emitterId = 0;
        // Match the engine's MDX→corn fx spawn frame: a +π/2 Z rotation treats
        // the emitter's local +Y as spawn-forward (see mdx_model_adapter.cpp's
        // kCornFxSpawnFrameRotation). Invisible for radially-symmetric glows,
        // but keeps directional effects (beams, cones) oriented as in-game.
        cs.transform = Matrix44f::rotation_z(1.5707963267948966f) * worldTransform;
        fs.cornStates.push_back(cs);
        return fs;
    }

private:
    std::string pkbPath_;
};

} // namespace whiteout::flakes::renderer::model
