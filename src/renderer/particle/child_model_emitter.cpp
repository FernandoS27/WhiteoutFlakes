#include "renderer/particle/child_model_emitter.h"

#include "whiteout/flakes/util/coordinate_system.h"

namespace whiteout::flakes::renderer::particle {

ChildModelEmitter::ChildModelEmitter(ModelId owner, i32 emitterId, HandleAllocator allocHandle)
    : owner_(owner), emitterId_(emitterId), allocHandle_(std::move(allocHandle)) {}

void ChildModelEmitter::ApplyPE1State(const model::FrameState::PE1FrameState& st) {
    emissionRate_ = st.emissionRate;
    motion_.gravity = {0.0f, 0.0f, -st.gravity};

    spawn_.speed.base = st.speed;
    spawn_.speed.variance = 0.0f; // PE1 has no speed variation
    spawn_.latitude = st.latitude;
    spawn_.longitude = st.longitude;

    SetVisible(st.visibility > 0.0f);
    modelToWorld_ = CoordinateSystem::ConvertTransform(CoordinateSystem::Default(),
                                                       desc_->coordSpace, st.transform);
}

Matrix44f ChildModelEmitter::TransformFor(const Particle2& p) const {
    const f32 s = desc_->childScale;
    return Matrix44f::scaling({s, s, s}) * Matrix44f::translation(p.position);
}

void ChildModelEmitter::OnPoolResized(usize capacity) {
    childHandles_.resize(capacity, 0);
}

void ChildModelEmitter::OnParticleBorn(u32 poolIndex) {
    if (poolIndex >= childHandles_.size())
        childHandles_.resize(pool_.Capacity(), 0);

    const u32 handle = allocHandle_ ? allocHandle_() : 0;
    childHandles_[poolIndex] = handle;
    if (handle == 0)
        return;

    ChildModelEvent ev;
    ev.kind = ChildModelEvent::Kind::Birth;
    ev.owner = owner_;
    ev.emitterId = emitterId_;
    ev.childHandle = handle;
    ev.transform = TransformFor(pool_[poolIndex]);
    pending_.push_back(ev);
}

void ChildModelEmitter::OnParticleDied(u32 poolIndex) {
    if (poolIndex >= childHandles_.size())
        return;
    const u32 handle = childHandles_[poolIndex];
    childHandles_[poolIndex] = 0;
    if (handle == 0)
        return;

    ChildModelEvent ev;
    ev.kind = ChildModelEvent::Kind::Death;
    ev.owner = owner_;
    ev.emitterId = emitterId_;
    ev.childHandle = handle;
    pending_.push_back(ev);
}

void ChildModelEmitter::CollectOutputEvents(std::vector<ChildModelEvent>& out) {
    // Births and deaths accumulated during the sim, then one Transform per
    // still-alive particle so the drain can drive the child actors.
    for (auto& ev : pending_)
        out.push_back(ev);
    pending_.clear();

    for (usize i = 0; i < pool_.AliveCount(); ++i) {
        const u32 idx = pool_.AliveAt(i);
        if (idx >= childHandles_.size())
            continue;
        const u32 handle = childHandles_[idx];
        if (handle == 0)
            continue;

        ChildModelEvent ev;
        ev.kind = ChildModelEvent::Kind::Transform;
        ev.owner = owner_;
        ev.emitterId = emitterId_;
        ev.childHandle = handle;
        ev.transform = TransformFor(pool_[idx]);
        out.push_back(ev);
    }
}

} // namespace whiteout::flakes::renderer::particle
