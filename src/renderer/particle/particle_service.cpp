#include "renderer/particle/particle_service.h"

namespace whiteout::flakes::renderer::particle {

namespace {

ImVector DefaultFog(const Vector3f&) {

    return {255, 255, 255, 255};
}

} // namespace

ParticleService::ParticleService() : fogSampler_(&DefaultFog) {}

ParticleService::~ParticleService() = default;

void ParticleService::AddPlaneEmitter(ModelId model, i32 emitterId,
                                      std::unique_ptr<PlaneEmitter> emitter) {
    std::lock_guard<std::mutex> lock(mutex_);
    emitters_[{model, emitterId}] = std::move(emitter);
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

PlaneEmitter* ParticleService::GetEmitter(ModelId model, i32 emitterId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = emitters_.find({model, emitterId});
    return (it != emitters_.end()) ? it->second.get() : nullptr;
}

i32 ParticleService::EmitterCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<i32>(emitters_.size());
}

i32 ParticleService::TotalParticleCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    i32 total = 0;
    for (const auto& [k, e] : emitters_) {
        total += e->TotalAlive();
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

void ParticleService::Simulate(f32 dt) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [k, e] : emitters_) {
        e->Update(dt);
    }
}

void ParticleService::BuildGeometry(const Matrix44f& worldToView, std::vector<Vertex>& outVertices,
                                    std::vector<EmitterDrawList>& outDrawLists) const {
    std::lock_guard<std::mutex> lock(mutex_);

    BuildGeometryInput in{};
    in.worldToView = &worldToView;
    in.fogEnabled = fogEnabled_;
    in.fogSampler = fogSampler_;

    for (const auto& [k, e] : emitters_) {
        const i32 offset = (i32)outVertices.size();
        i32 vcount = BuildEmitterGeometry(*e, in, outVertices);
        if (vcount > 0) {
            outDrawLists.push_back(
                {k.model, k.id, offset, vcount, e->PriorityPlane(), e->Material()});
        }
    }
}

void ParticleService::SetGlobalScaler(f32 s) {
    SetGlobalEmissionScaler(s);
}

f32 ParticleService::GlobalScaler() const {
    return GetGlobalEmissionScaler();
}

void ParticleService::SetFogSampler(FogSampler sampler) {
    std::lock_guard<std::mutex> lock(mutex_);
    fogSampler_ = sampler ? std::move(sampler) : FogSampler(&DefaultFog);
}

} // namespace whiteout::flakes::renderer::particle
