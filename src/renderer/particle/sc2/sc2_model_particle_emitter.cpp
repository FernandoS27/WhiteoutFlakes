#include "renderer/particle/sc2/sc2_model_particle_emitter.h"

#include "renderer/particle/sc2/sc2_runtime.h"

#include <cmath>

namespace whiteout::flakes::renderer::particle {

namespace {

bool Placeable(const sc2::ModelPose& p) {
    const auto ok = [](f32 v) { return std::isfinite(v); };
    return ok(p.position.x) && ok(p.position.y) && ok(p.position.z) && ok(p.scale.x) &&
           ok(p.scale.y) && ok(p.scale.z) && ok(p.rotation[0]) && ok(p.rotation[1]) &&
           ok(p.rotation[2]) && ok(p.rotation[3]);
}

} // namespace

void Sc2ModelParticleEmitter::CollectOutputEvents(std::vector<ChildModelEvent>& out) {
    children_.Drain(out);
    const sc2::Runtime* rt = StateSc2();
    if (!rt)
        return;

    const sc2::ElementList& list = rt->store.list;
    for (i32 node = list.head; node >= 0; node = list.next[static_cast<usize>(node)]) {
        const u32 idx = static_cast<u32>(node);
        if (children_.Holds(idx))
            children_.Transform(idx, TransformFor(idx), VisibilityFor(idx), out);
    }
}

Matrix44f Sc2ModelParticleEmitter::TransformFor(u32 node) const {
    const sc2::Runtime* rt = StateSc2();
    if (!rt || node >= rt->modelPose.size())
        return Matrix44f::identity();
    const sc2::ModelPose& p = rt->modelPose[node];
    if (!Placeable(p))
        return Matrix44f::scaling({0.0f, 0.0f, 0.0f});
    // The quaternion's rows are where the model's own axes go, which is
    // exactly the rows of the row-vector matrix that places it. The scale is
    // the pose's in SC2 units — the child actor applies its world scale on
    // top — and the position is already the renderer's.
    const std::array<Vector3f, 3> rows = sc2::QuatRows(p.rotation);
    Matrix44f r = Matrix44f::identity();
    for (usize i = 0; i < 3; ++i) {
        r.data[i][0] = rows[i].x;
        r.data[i][1] = rows[i].y;
        r.data[i][2] = rows[i].z;
    }
    return Matrix44f::scaling(p.scale) * r * Matrix44f::translation(p.position);
}

f32 Sc2ModelParticleEmitter::VisibilityFor(u32 node) const {
    const sc2::Runtime* rt = StateSc2();
    if (!rt || node >= rt->modelPose.size())
        return 0.0f;
    return Placeable(rt->modelPose[node]) ? 1.0f : 0.0f;
}

u32 Sc2ModelParticleEmitter::PathIndexFor(u32 node) const {
    const sc2::Runtime* rt = StateSc2();
    if (!rt || node >= rt->modelPath.size())
        return 0;
    return rt->modelPath[node];
}

} // namespace whiteout::flakes::renderer::particle
