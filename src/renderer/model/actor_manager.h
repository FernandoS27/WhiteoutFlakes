#pragma once

#include "whiteout/flakes/types.h"
#include "model/model_instance.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes::renderer::model {

using ActorId = u32;

class ActorManager {
public:
    using Map = std::unordered_map<ActorId, std::unique_ptr<Actor>>;

    Actor* Spawn(ActorId id) {
        auto a = std::make_unique<Actor>();
        a->handle = id;
        Actor* raw = a.get();
        actors_[id] = std::move(a);
        return raw;
    }

    Actor* Adopt(std::unique_ptr<Actor> actor) {
        if (!actor) return nullptr;
        const ActorId id = actor->handle;
        Actor* raw = actor.get();
        actors_[id] = std::move(actor);
        return raw;
    }

    std::unique_ptr<Actor> Despawn(ActorId id) {
        auto it = actors_.find(id);
        if (it == actors_.end()) return nullptr;
        auto out = std::move(it->second);
        actors_.erase(it);
        return out;
    }

    Actor* Find(ActorId id) const {
        auto it = actors_.find(id);
        return (it != actors_.end()) ? it->second.get() : nullptr;
    }

    bool   Empty() const { return actors_.empty(); }
    usize  Size()  const { return actors_.size(); }
    void   Clear()       { actors_.clear(); }

    Map&       All()       { return actors_; }
    const Map& All() const { return actors_; }

    ActorId FirstId() const {
        return actors_.empty() ? 0 : actors_.begin()->first;
    }

private:
    Map actors_;
};

// Walk up the parent chain from `start` to find the top-level (Unit/External)
// ancestor and return its cursor's actorTimeMs. Children of paused parents
// inherit the ancestor's frozen clock so their animation cursors pause too.
// Returns `start.cursor.actorTimeMs` for actors that are themselves top-level.
inline i32 AncestorActorTimeMs(const Actor& start, const ActorManager& actors) {
    const Actor* a = &start;
    while (a->IsChild()) {
        const Actor* p = actors.Find(a->parent);
        if (!p) break;
        a = p;
    }
    return a->cursor.actorTimeMs;
}

}
