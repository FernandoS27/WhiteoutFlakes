#include "renderer/particle/child_model_emitter.h"

#include "whiteout/flakes/util/coordinate_system.h"

namespace whiteout::flakes::renderer::particle {

ChildModelEmitter::ChildModelEmitter(ModelId owner, i32 emitterId, HandleAllocator allocHandle) {
    children_.Bind(owner, emitterId, std::move(allocHandle));
}

void ChildModelEmitter::ApplyPE1State(const model::FrameState::PE1FrameState& st) {
    SetEmissionRate(st.emissionRate);
    Motion().gravity = {0.0f, 0.0f, -st.gravity};

    SpawnParams& spawn = Spawn();
    spawn.speed.base = st.speed;
    spawn.speed.variance = 0.0f; // PE1 has no speed variation
    spawn.latitude = st.latitude;
    spawn.longitude = st.longitude;

    SetVisible(st.visibility > 0.0f);
    SetModelToWorld(CoordinateSystem::ConvertTransform(CoordinateSystem::Default(),
                                                       Desc().coordSpace, st.transform));
}

Matrix44f ChildModelEmitter::TransformFor(u32 poolIndex) const {
    const f32 s = Desc().childScale;
    return Matrix44f::scaling({s, s, s}) * Matrix44f::translation(Pool()[poolIndex].position);
}

void ChildModelEmitter::OnPoolResized(usize capacity) {
    children_.Resize(capacity);
}

void ChildModelEmitter::OnParticleBorn(u32 poolIndex) {
    // The channel grows to the slot: an SC2 emitter's pool is empty and its
    // indices are store nodes, so the pool's capacity alone would leave the
    // slot out of range.
    children_.Birth(poolIndex, [&](ChildModelEvent& ev) {
        ev.pathIndex = PathIndexFor(poolIndex);
        ev.route = BirthRoute();
        const std::vector<std::string>& paths = Desc().childModelPaths;
        if (ev.pathIndex < paths.size())
            ev.path = paths[ev.pathIndex];
        ev.transform = TransformFor(poolIndex);
    });
}

void ChildModelEmitter::OnParticleDied(u32 poolIndex) {
    children_.Death(poolIndex);
}

void ChildModelEmitter::CollectOutputEvents(std::vector<ChildModelEvent>& out) {
    // Births and deaths accumulated during the sim, then one Transform per
    // still-alive particle so the drain can drive the child actors.
    children_.Drain(out);
    const ParticlePool& pool = Pool();
    for (usize i = 0; i < pool.AliveCount(); ++i) {
        const u32 idx = pool.AliveAt(i);
        if (children_.Holds(idx))
            children_.Transform(idx, TransformFor(idx), VisibilityFor(idx), out);
    }
}

} // namespace whiteout::flakes::renderer::particle
