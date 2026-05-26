#include "renderer/corn_effects/corn_effects_service.h"

#include <cstdio>

namespace whiteout::flakes::renderer::corn_effects {

CornEffectsService::CornEffectsService() = default;

CornEffectsService::~CornEffectsService() {
    std::lock_guard<std::mutex> lock(mutex_);
    emitters_.clear();
}

void CornEffectsService::AddCornEmitter(ActorId model, i32 emitterId,
                                        std::unique_ptr<CornEffectsEmitter> emitter) {
    std::lock_guard<std::mutex> lock(mutex_);
    emitter->gameToCornEffectsScale_ = gameToCornEffectsScale_;
    emitter->SetBackendInit(backendInit_);
    emitter->SetFrameArena(&frameArena_);
    emitters_[{model, emitterId}] = std::move(emitter);
}

void CornEffectsService::SetBackendInit(const std::optional<CornEffectsGfxBackend::Init>& init) {
    std::lock_guard<std::mutex> lock(mutex_);
    backendInit_ = init;
    for (auto& [k, e] : emitters_) {
        e->SetBackendInit(backendInit_);
    }
}

void CornEffectsService::SetFrameInputs(const CornEffectsFrameInputs& fi) {
    std::lock_guard<std::mutex> lock(mutex_);
    frameInputs_ = fi;
    if (gameToCornEffectsScale_ > 0.0f) {
        frameInputs_.cornEffectsScale = 1.0f / gameToCornEffectsScale_;
    }
    for (auto& [k, e] : emitters_) {
        e->SetFrameInputs(frameInputs_);
    }
}

void CornEffectsService::RemoveModel(ActorId model) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = emitters_.begin(); it != emitters_.end();) {
        if (it->first.model == model)
            it = emitters_.erase(it);
        else
            ++it;
    }
}

void CornEffectsService::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    emitters_.clear();
}

CornEffectsEmitter* CornEffectsService::GetEmitter(ActorId model, i32 emitterId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = emitters_.find({model, emitterId});
    return (it != emitters_.end()) ? it->second.get() : nullptr;
}

i32 CornEffectsService::EmitterCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<i32>(emitters_.size());
}

i32 CornEffectsService::TotalParticleCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    i32 total = 0;
    for (const auto& [k, e] : emitters_) {
        total += e->TotalAlive();
    }
    return total;
}

bool CornEffectsService::HasEmittersForModel(ActorId model) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [k, e] : emitters_) {
        if (k.model == model)
            return true;
    }
    return false;
}

void CornEffectsService::SetOwningAgentVisibilityForModel(ActorId model, bool visible) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [k, e] : emitters_) {
        if (k.model == model)
            e->SetOwningAgentVisibility(visible);
    }
}

void CornEffectsService::Simulate(f32 dt) {
    std::lock_guard<std::mutex> lock(mutex_);
    frameArena_.reset();
    for (auto& [k, e] : emitters_) {
        e->gameToCornEffectsScale_ = gameToCornEffectsScale_;
        e->Update(dt, false);
    }
}

} // namespace whiteout::flakes::renderer::corn_effects
