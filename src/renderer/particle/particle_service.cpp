#include "renderer/particle/particle_service.h"

namespace whiteout::flakes::renderer::particle {

namespace {

ImVector DefaultFog(const Vector3f&) {

    return {255, 255, 255, 255};
}

} // namespace

ParticleService::ParticleService() : fogSampler_(&DefaultFog) {}

ParticleService::~ParticleService() = default;

void ParticleService::AddEmitter(ModelId model, ParticleOutput output, i32 emitterId,
                                 std::unique_ptr<Emitter2> emitter) {
    std::lock_guard<std::mutex> lock(mutex_);
    emitters_[{model, output, emitterId}] = std::move(emitter);
}

void ParticleService::RemoveModel(ModelId model) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = emitters_.begin(); it != emitters_.end();) {
        if (it->first.model == model)
            it = emitters_.erase(it);
        else
            ++it;
    }
}

void ParticleService::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    emitters_.clear();
}

Emitter2* ParticleService::GetEmitter(ModelId model, ParticleOutput output, i32 emitterId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = emitters_.find({model, output, emitterId});
    return (it != emitters_.end()) ? it->second.get() : nullptr;
}

i32 ParticleService::EmitterCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    i32 n = static_cast<i32>(emitters_.size());
    for (const auto& [k, e] : emitters_)
        n += static_cast<i32>(e->Trails().size());
    return n;
}

i32 ParticleService::TotalParticleCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    i32 total = 0;
    for (const auto& [k, e] : emitters_) {
        total += e->TotalAlive();
        // Trails are not in the map — their owner is — so the counter has to
        // walk them or under-report every trail particle on screen.
        for (const auto& t : e->Trails())
            total += t->TotalAlive();
    }
    return total;
}

bool ParticleService::HasEmittersForModel(ModelId model) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [k, e] : emitters_) {
        if (k.model == model)
            return true;
    }
    return false;
}

void ParticleService::ForEachEmitter(
    const std::function<void(const EmitterKey&, const Emitter2&)>& fn) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [k, e] : emitters_) {
        fn(k, *e);
        // Under the same key the draw lists use — synthetic id, and always the
        // billboard space, because that is what a trail's particles are.
        i32 childIdx = 0;
        for (const auto& t : e->Trails())
            fn({k.model, ParticleOutput::Billboard, TrailEmitterId(k.id, childIdx++)}, *t);
    }
}

void ParticleService::Simulate(f32 dt) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [k, e] : emitters_) {
        e->Update(dt, emissionScaler_);
        e->CollectOutputEvents(childEvents_);
    }
}

void ParticleService::DrainChildModelEvents(std::vector<ChildModelEvent>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    out = std::move(childEvents_);
    childEvents_.clear();
}

void ParticleService::BuildGeometry(const Matrix44f& worldToView, std::vector<Vertex>& outVertices,
                                    std::vector<EmitterDrawList>& outDrawLists) const {
    std::lock_guard<std::mutex> lock(mutex_);

    BuildGeometryInput in{};
    in.worldToView = &worldToView;
    in.fogEnabled = fogEnabled_;
    in.fogSampler = fogSampler_;

    // One emitter's own particles, then its trails'. A trail is an ordinary
    // billboard emitter with its own texture, blend mode and sheet, so it needs
    // its own draw list; it sorts on its OWNER's origin so the two stay
    // together in the transparent pass instead of drifting apart.
    auto build = [&](const Emitter2& e, ModelId model, i32 id, const Vector3f& origin) {
        const i32 offset = (i32)outVertices.size();
        const i32 vcount = BuildEmitterGeometry(e, in, outVertices);
        if (vcount > 0)
            outDrawLists.push_back(
                {model, id, offset, vcount, e.PriorityPlane(), e.Material(), origin});
    };

    for (const auto& [k, e] : emitters_) {
        const Vector3f origin = whiteout::transform_point({0, 0, 0}, e->ModelToWorld());
        if (e->Output() == ParticleOutput::Billboard)
            build(*e, k.model, k.id, origin);
        // Trails are billboards whatever their owner's output is: a
        // model-particle emitter can carry one, and its trail still draws
        // quads. An emitter index belongs to one output space or the other,
        // never both, so the synthetic ids cannot collide across the two.
        i32 childIdx = 0;
        for (const auto& t : e->Trails())
            build(*t, k.model, TrailEmitterId(k.id, childIdx++), origin);
    }
}

void ParticleService::SetEmissionScaler(f32 s) {
    std::lock_guard<std::mutex> lock(mutex_);
    emissionScaler_ = s;
}

f32 ParticleService::EmissionScaler() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return emissionScaler_;
}

void ParticleService::SetFogSampler(FogSampler sampler) {
    std::lock_guard<std::mutex> lock(mutex_);
    fogSampler_ = sampler ? std::move(sampler) : FogSampler(&DefaultFog);
}

} // namespace whiteout::flakes::renderer::particle
