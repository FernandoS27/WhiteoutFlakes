#include "renderer/ribbon/ribbon_service.h"

namespace whiteout::flakes::renderer::ribbon {

void RibbonService::AddEmitter(ModelId model, i32 emitterId, const RibbonDesc& desc,
                               const RibbonBehavior& behavior) {
    std::lock_guard<std::mutex> lock(mutex_);
    emitters_[{model, emitterId}] = RibbonEmitter(desc, behavior);
}

void RibbonService::RemoveModel(ModelId model) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = emitters_.begin(); it != emitters_.end();) {
        if (it->first.model == model)
            it = emitters_.erase(it);
        else
            ++it;
    }
}

void RibbonService::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    emitters_.clear();
}

RibbonEmitter* RibbonService::GetEmitter(ModelId model, i32 emitterId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = emitters_.find({model, emitterId});
    return (it != emitters_.end()) ? &it->second : nullptr;
}

const RibbonEmitter* RibbonService::GetEmitter(ModelId model, i32 emitterId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = emitters_.find({model, emitterId});
    return (it != emitters_.end()) ? &it->second : nullptr;
}

bool RibbonService::HasEmittersForModel(ModelId model) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = emitters_.lower_bound({model, INT32_MIN});
    return it != emitters_.end() && it->first.model == model;
}

i32 RibbonService::EmitterCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (i32)emitters_.size();
}

i32 RibbonService::TotalEdgeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    i32 total = 0;
    for (const auto& [key, em] : emitters_)
        total += (i32)em.Edges().size();
    return total;
}

i32 RibbonService::EdgeCountForModel(ModelId model) const {
    std::lock_guard<std::mutex> lock(mutex_);
    i32 total = 0;
    for (auto it = emitters_.lower_bound({model, INT32_MIN});
         it != emitters_.end() && it->first.model == model; ++it)
        total += (i32)it->second.Edges().size();
    return total;
}

void RibbonService::SetState(ModelId model, i32 emitterId, const RibbonState& st) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = emitters_.find({model, emitterId});
    if (it != emitters_.end())
        it->second.SetState(st);
}

void RibbonService::SimulateModel(ModelId model, f32 dt) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = emitters_.lower_bound({model, INT32_MIN});
         it != emitters_.end() && it->first.model == model; ++it)
        it->second.Update(dt);
}

void RibbonService::Simulate(f32 dt) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [key, em] : emitters_)
        em.Update(dt);
}

void RibbonService::BuildGeometry(ModelId model, std::vector<Vertex>& outVertices,
                                  std::vector<RibbonDrawList>& outDrawLists) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = emitters_.lower_bound({model, INT32_MIN});
         it != emitters_.end() && it->first.model == model; ++it) {
        const RibbonEmitter& em = it->second;
        const i32 offset = (i32)outVertices.size();
        const i32 added = em.BuildStrip(outVertices);
        if (added <= 0)
            continue;

        const RibbonDesc& d = em.Desc();
        RibbonDrawList dl;
        dl.model = model;
        dl.emitterId = it->first.id;
        dl.vertexOffset = offset;
        dl.vertexCount = added;
        dl.priorityPlane = d.priorityPlane;
        dl.textureId = d.textureId;
        dl.filterMode = d.filterMode;
        dl.unshaded = d.unshaded;
        dl.twoSided = d.twoSided;
        dl.worldOrigin = outVertices[(usize)offset].position;
        outDrawLists.push_back(dl);
    }
}

void RibbonService::ForEachEmitter(
    const std::function<void(const EmitterKey&, const RibbonEmitter&)>& fn) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, em] : emitters_)
        fn(key, em);
}

} // namespace whiteout::flakes::renderer::ribbon
