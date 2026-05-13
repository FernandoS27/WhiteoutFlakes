#include "renderer/effects/spn_spawner.h"

#include "io/mdx_model_adapter.h"
#include "renderer/assets/replaceable_texture_manager.h"
#include "renderer/model/actor_manager.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_template.h"
#include "renderer/model/model_template_manager.h"
#include "renderer/render_service.h"
#include "renderer/render_service_impl.h"
#include "renderer/scene_manager.h"

#include <algorithm>
#include <cstdio>

namespace whiteout::flakes::renderer::effects {

using namespace ::whiteout::flakes::renderer::model;

void SpnSpawner::Spawn(u32 parentActor, const std::string& mdxPath,
                       const Matrix44f& parentNodeWorld, i32 nowMs) {
    if (mdxPath.empty())
        return;
    Pending p;
    p.parentActor = parentActor;
    p.mdxPath = mdxPath;
    p.parentWorld = parentNodeWorld;
    p.birthMs = nowMs;
    pending_.push_back(std::move(p));
}

void SpnSpawner::Tick(i32 nowMs) {

    if (!pending_.empty()) {
        std::vector<Pending> stillPending;
        stillPending.reserve(pending_.size());
        for (auto& p : pending_) {

            auto pit = rs_.Scene().Actors().All().find(p.parentActor);
            if (pit == rs_.Scene().Actors().All().end())
                continue;

            auto tmpl = rs_.Scene().Templates().GetOrLoadAsync(p.mdxPath);
            if (!tmpl) {
                stillPending.push_back(std::move(p));
                continue;
            }

            i32 durationMs = 1000;
            std::vector<SequenceInfo> seqs;
            if (tmpl->adapter)
                seqs = tmpl->adapter->GetSequences();
            if (!seqs.empty()) {
                const i32 span = seqs[0].endMs - seqs[0].startMs;
                durationMs = (span > 0) ? span : 1000;
            }

            auto* child =
                rs_.Loader().SpawnChild(*pit->second, ActorRole::SPN, tmpl, p.parentWorld);
            if (!child) {
                stillPending.push_back(std::move(p));
                continue;
            }
            child->animation.SetActiveSequenceIndex(0);
            child->animation.SetBirthTimeMs(p.birthMs);

            Active a;
            a.parentActor = p.parentActor;
            a.handle = child->handle;
            a.expiryMs = p.birthMs + durationMs;
            active_.push_back(a);
        }
        pending_ = std::move(stillPending);
    }

    if (active_.empty())
        return;
    auto it = active_.begin();
    while (it != active_.end()) {
        if (nowMs < it->expiryMs) {
            ++it;
            continue;
        }
        rs_.Loader().DestroyActor(it->handle);
        it = active_.erase(it);
    }
}

void SpnSpawner::RemoveSpawnsOf(u32 parentActor) {
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [&](const Pending& p) { return p.parentActor == parentActor; }),
                   pending_.end());
    auto it = active_.begin();
    while (it != active_.end()) {
        if (it->parentActor != parentActor) {
            ++it;
            continue;
        }
        rs_.Loader().DestroyActor(it->handle);
        it = active_.erase(it);
    }
}

void SpnSpawner::Clear() {
    pending_.clear();
    for (auto& a : active_)
        rs_.Loader().DestroyActor(a.handle);
    active_.clear();
}

} // namespace whiteout::flakes::renderer::effects
