#include "renderer/spn_spawner.h"

#include "io/mdx_model_adapter.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "renderer/model_template_manager.h"
#include "renderer/model_template.h"
#include "renderer/model_instance.h"
#include "renderer/actor_manager.h"
#include "renderer/replaceable_texture_manager.h"

#include <algorithm>
#include <cstdio>

namespace WhiteoutDex {

void SpnSpawner::Spawn(u32                parentActor,
                       const std::string& mdxPath,
                       const Matrix44f&   parentNodeWorld,
                       i32                nowMs) {
    if (mdxPath.empty()) return;
    Pending p;
    p.parentActor = parentActor;
    p.mdxPath     = mdxPath;
    p.parentWorld = parentNodeWorld;
    p.birthMs     = nowMs;
    pending_.push_back(std::move(p));
}

void SpnSpawner::Tick(i32 nowMs) {

    if (rs_.scene_ && !pending_.empty()) {
        std::vector<Pending> stillPending;
        stillPending.reserve(pending_.size());
        for (auto& p : pending_) {

            auto pit = rs_.scene_->Actors().All().find(p.parentActor);
            if (pit == rs_.scene_->Actors().All().end()) continue;

            auto tmpl = rs_.scene_->Templates().GetOrLoadAsync(p.mdxPath);
            if (!tmpl) { stillPending.push_back(std::move(p)); continue; }

            const i32 parentDepth = pit->second->pe1Depth;

            i32 durationMs = 1000;
            std::vector<SequenceInfo> seqs;
            if (tmpl->adapter) seqs = tmpl->adapter->GetSequences();
            if (!seqs.empty()) {
                const i32 span = seqs[0].endMs - seqs[0].startMs;
                durationMs = (span > 0) ? span : 1000;
            }

            u32 childH = rs_.scene_->NextActorIdRef()++;
            auto child = std::make_unique<Actor>();
            child->handle         = childH;
            child->parent         = p.parentActor;
            child->isPE1Child     = true;
            child->pe1Depth       = parentDepth + 1;
            child->worldTransform = p.parentWorld;
            child->animation.Bind(std::static_pointer_cast<IAnimationSource>(tmpl->adapter));
            child->animation.SetActiveSequenceIndex(0);
            child->animation.SetBirthTimeMs(p.birthMs);

            rs_.stageModelFromTemplate(child.get(), tmpl);
            rs_.scene_->Actors().All()[childH] = std::move(child);

            Active a;
            a.parentActor = p.parentActor;
            a.handle      = childH;
            a.expiryMs    = p.birthMs + durationMs;
            active_.push_back(a);
        }
        pending_ = std::move(stillPending);
    }

    if (active_.empty() || !rs_.scene_) return;
    auto it = active_.begin();
    while (it != active_.end()) {
        if (nowMs < it->expiryMs) { ++it; continue; }
        auto found = rs_.scene_->Actors().All().find(it->handle);
        if (found != rs_.scene_->Actors().All().end()) {
            if (rs_.replaceables_) rs_.replaceables_->UnregisterModel(*found->second);
            if (rs_.gfx_)          found->second->ReleaseGPU(*rs_.gfx_);
            rs_.scene_->Actors().All().erase(found);
        }
        rs_.particleService_.RemoveModel(it->handle);
        it = active_.erase(it);
    }
}

void SpnSpawner::RemoveSpawnsOf(u32 parentActor) {
    pending_.erase(
        std::remove_if(pending_.begin(), pending_.end(),
            [&](const Pending& p) { return p.parentActor == parentActor; }),
        pending_.end());
    if (!rs_.scene_) { active_.clear(); return; }
    auto it = active_.begin();
    while (it != active_.end()) {
        if (it->parentActor != parentActor) { ++it; continue; }
        auto found = rs_.scene_->Actors().All().find(it->handle);
        if (found != rs_.scene_->Actors().All().end()) {
            if (rs_.replaceables_) rs_.replaceables_->UnregisterModel(*found->second);
            if (rs_.gfx_)          found->second->ReleaseGPU(*rs_.gfx_);
            rs_.scene_->Actors().All().erase(found);
        }
        rs_.particleService_.RemoveModel(it->handle);
        it = active_.erase(it);
    }
}

void SpnSpawner::Clear() {
    pending_.clear();
    if (!rs_.scene_) { active_.clear(); return; }
    for (auto& a : active_) {
        auto found = rs_.scene_->Actors().All().find(a.handle);
        if (found == rs_.scene_->Actors().All().end()) continue;
        if (rs_.replaceables_) rs_.replaceables_->UnregisterModel(*found->second);
        if (rs_.gfx_)          found->second->ReleaseGPU(*rs_.gfx_);
        rs_.scene_->Actors().All().erase(found);
        rs_.particleService_.RemoveModel(a.handle);
    }
    active_.clear();
}

}
