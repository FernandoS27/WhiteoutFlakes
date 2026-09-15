#include "renderer/particle/particle_service.h"

#include "renderer/particle/sc2_runtime.h"

namespace whiteout::flakes::renderer::particle {

ParticleService::ParticleService() = default;

ParticleService::~ParticleService() = default;

void ParticleService::AddEmitter(ModelId model, i32 emitterId,
                                 std::unique_ptr<ParticleEmitter> emitter) {
    const ParticleOutput output =
        emitter ? emitter->DrawHeader().output : ParticleOutput::Billboard;
    std::lock_guard<std::mutex> lock(mutex_);
    // An emitter registered after the host installed its terrain has to get it
    // too, or an actor spawned mid-scene collides against a different surface
    // from its neighbours.
    if (groundQuery_ && emitter)
        emitter->SetGroundQuery(groundQuery_);
    if (Emitter2* e2 = emitter ? emitter->AsEmitter2() : nullptr)
        e2->AttachSc2PendingList(&sc2PendingModels_);
    emitters_[{model, output, emitterId}] = std::move(emitter);
}

void ParticleService::RemoveModel(ModelId model) {
    std::lock_guard<std::mutex> lock(mutex_);
    // The model's range, in key order — the order the whole-map walk erased in.
    auto it = emitters_.lower_bound(FirstKeyOf(model));
    while (it != emitters_.end() && it->first.model == model)
        it = emitters_.erase(it);
}

void ParticleService::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    emitters_.clear();
}

void ParticleService::ResetEmitters() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Collected here, as RemoveEmitter does: the deaths a reset reports are
    // owed to the host's next drain, not to whichever Simulate comes after.
    for (auto& [k, e] : emitters_) {
        e->ResetParticles();
        e->CollectOutputEvents(childEvents_);
    }
}

ParticleEmitter* ParticleService::GetEmitter(ModelId model, ParticleOutput output,
                                             i32 emitterId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = emitters_.find({model, output, emitterId});
    return (it != emitters_.end()) ? it->second.get() : nullptr;
}

i32 ParticleService::EmitterCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    i32 n = static_cast<i32>(emitters_.size());
    for (const auto& [k, e] : emitters_) {
        if (const Emitter2* e2 = e->AsEmitter2())
            n += static_cast<i32>(e2->Trails().size());
    }
    return n;
}

i32 ParticleService::TotalParticleCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    i32 total = 0;
    for (const auto& [k, e] : emitters_) {
        total += e->TotalAlive();
        // Trails are not in the map — their owner is — so the counter has to
        // walk them or under-report every trail particle on screen.
        if (const Emitter2* e2 = e->AsEmitter2()) {
            for (const auto& t : e2->Trails())
                total += t->TotalAlive();
        }
    }
    return total;
}

bool ParticleService::HasEmittersForModel(ModelId model) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = emitters_.lower_bound(FirstKeyOf(model));
    return it != emitters_.end() && it->first.model == model;
}

void ParticleService::ForEachEmitter(
    const std::function<void(const EmitterKey&, const ParticleEmitter&)>& fn) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [k, e] : emitters_) {
        fn(k, *e);
        const Emitter2* e2 = e->AsEmitter2();
        if (!e2)
            continue;
        // Under the same key the draw lists use — synthetic id, and always the
        // billboard space, because that is what a trail's particles are.
        i32 childIdx = 0;
        for (const auto& t : e2->Trails())
            fn({k.model, ParticleOutput::Billboard, TrailEmitterId(k.id, childIdx++)}, *t);
    }
}

bool ParticleService::RemoveEmitter(ModelId model, ParticleOutput output, i32 emitterId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = emitters_.find(EmitterKey{model, output, emitterId});
    if (it == emitters_.end())
        return false;
    it->second->CollectOutputEvents(childEvents_);
    emitters_.erase(it);
    return true;
}

void ParticleService::Simulate(f32 dt) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RoutedSpawnRequest> routed;
    for (auto& [k, e] : emitters_) {
        e->Update(dt, emissionScaler_);
        Emitter2* e2 = e->AsEmitter2();
        if (!e2)
            continue;
        // An SC2 emitter's requests of its children leave right after its own
        // step, into the child's inbox: a child later in this walk takes them
        // this frame and one earlier takes them next frame, as retail's update
        // order hands them over. The inbox's 128 cap is where every parent's
        // requests meet. A child is registered under whichever output it
        // draws, so both id spaces are asked.
        routed.clear();
        e2->DrainSpawnRequests(routed);
        for (const RoutedSpawnRequest& r : routed) {
            auto it = emitters_.find(EmitterKey{k.model, ParticleOutput::Billboard, r.targetEmitterId});
            if (it == emitters_.end())
                it = emitters_.find(
                    EmitterKey{k.model, ParticleOutput::ChildModel, r.targetEmitterId});
            if (Emitter2* target = it != emitters_.end() ? it->second->AsEmitter2() : nullptr)
                target->QueueSpawnRequest(r.req);
        }
    }

    // Every SC2 model particle registered this frame gets its model now, once
    // all emitters have run — the walk retail's frame driver makes after its
    // update job. The count is re-read each iteration, as both of retail's
    // loops re-read theirs.
    for (usize i = 0; i < sc2PendingModels_.size(); ++i) {
        const Sc2PendingModel entry = sc2PendingModels_[i];
        if (entry.runtime != nullptr && entry.runtime->host != nullptr)
            entry.runtime->host->ServiceSc2PendingModel(entry.node);
    }
    sc2PendingModels_.clear();

    // Collected after the walk rather than beside each Update, so an SC2 birth
    // leaves in the same frame it was drawn. Handles are minted at the birth,
    // not here, so no dialect's handle order moves.
    for (auto& [k, e] : emitters_)
        e->CollectOutputEvents(childEvents_);
}

void ParticleService::DrainChildModelEvents(std::vector<ChildModelEvent>& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    out = std::move(childEvents_);
    childEvents_.clear();
}

bool ParticleService::HasRefractionEmitters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [k, e] : emitters_) {
        if (e->DrawHeader().refraction)
            return true;
        const Emitter2* e2 = e->AsEmitter2();
        if (!e2)
            continue;
        for (const auto& t : e2->Trails())
            if (t->Desc().refraction)
                return true;
    }
    return false;
}

namespace {

/// The draw for @p count vertices from @p offset of the emitter @p h describes.
EmitterDrawList MakeDraw(const EmitterDrawHeader& h, ModelId model, i32 id, i32 offset, i32 count,
                         const Vector3f& origin) {
    return {model, id, offset, count, h.priorityPlane, *h.material, origin};
}

} // namespace

void ParticleService::BuildGeometry(const Matrix44f& worldToView, const ParticleStreams& out) const {
    std::lock_guard<std::mutex> lock(mutex_);

    BuildGeometryInput in{};
    in.worldToView = &worldToView;
    in.d3 = out.d3;

    // The two side streams put their VERTICES somewhere other than
    // `out.vertices`, so they must not also append to arrays kept parallel with
    // it. No shipped emitter is both Diablo III and multi-texture — the flags
    // are M2's and the D3 adapter never sets them — but the invariant is worth
    // holding structurally rather than by that argument.
    BuildGeometryInput refractIn = in;
    refractIn.d3 = nullptr;
    if (out.refraction)
        refractIn.extraUV = &out.refraction->extraUV;

    BuildGeometryInput multiTexIn = in;
    multiTexIn.d3 = nullptr;
    if (out.multiTex)
        multiTexIn.extraUV = &out.multiTex->extraUV;

    // One emitter's own particles, then its trails'. A trail is an ordinary
    // billboard emitter with its own texture, blend mode and sheet, so it needs
    // its own draw list; it sorts on its OWNER's origin so the two stay
    // together in the transparent pass instead of drifting apart.
    //
    // A refraction emitter is routed to its own arrays instead — it belongs to
    // one pass or the other, never both.
    //
    // A multi-texture emitter takes a third route: its vertices need the extra
    // UV sets so they go to their own stream, but the emitter itself is
    // ordinary transparent colour, so its DRAW joins the sorted list and the
    // material's `multiTexture` bit is what tells the dispatcher which stream
    // the offsets belong to. With nowhere to put them it falls back to the
    // single-texture path: the result is too bright and misses two layers, but
    // an approximate particle beats a missing one.
    auto build = [&](const ParticleEmitter& e, const EmitterDrawHeader& h, ModelId model, i32 id,
                     const Vector3f& origin) {
        if (h.refraction) {
            if (!out.refraction)
                return;
            const i32 offset = (i32)out.refraction->vertices.size();
            const i32 vcount = e.BuildGeometry(refractIn, out.refraction->vertices);
            if (vcount > 0)
                out.refraction->draws.push_back(MakeDraw(h, model, id, offset, vcount, origin));
            return;
        }
        if (h.multiTexture && out.multiTex) {
            const i32 offset = (i32)out.multiTex->vertices.size();
            const i32 vcount = e.BuildGeometry(multiTexIn, out.multiTex->vertices);
            if (vcount > 0)
                out.draws.push_back(MakeDraw(h, model, id, offset, vcount, origin));
            return;
        }
        // Levelled BEFORE the build, not after: a D3 emitter appends its
        // texcoords as it appends its vertices, so the arrays have to already be
        // the same length or its first quad's coordinates land under whatever an
        // earlier emitter's quads occupy.
        if (out.d3)
            out.d3->LevelTo(out.vertices.size());
        const i32 offset = (i32)out.vertices.size();
        const i32 vcount = e.BuildGeometry(in, out.vertices);
        if (vcount > 0) {
            EmitterDrawList dl = MakeDraw(h, model, id, offset, vcount, origin);
            // Nothing downstream can shade three layers off this stream, so the
            // draw must not claim it carries them.
            dl.material.multiTexture = false;
            out.draws.push_back(dl);
        }
    };

    for (const auto& [k, e] : emitters_) {
        const Vector3f origin = whiteout::transform_point({0, 0, 0}, e->ModelToWorld());
        const EmitterDrawHeader h = e->DrawHeader();
        if (h.output == ParticleOutput::Billboard)
            build(*e, h, k.model, k.id, origin);
        const Emitter2* e2 = e->AsEmitter2();
        if (!e2)
            continue;
        // Trails are billboards whatever their owner's output is: a
        // model-particle emitter can carry one, and its trail still draws
        // quads. An emitter index belongs to one output space or the other,
        // never both, so the synthetic ids cannot collide across the two.
        i32 childIdx = 0;
        for (const auto& t : e2->Trails())
            build(*t, t->DrawHeader(), k.model, TrailEmitterId(k.id, childIdx++), origin);
    }
    // The last emitter may have been one that writes no texcoords.
    if (out.d3)
        out.d3->LevelTo(out.vertices.size());
}

void ParticleService::SetEmissionScaler(f32 s) {
    std::lock_guard<std::mutex> lock(mutex_);
    emissionScaler_ = s;
}

void ParticleService::SetGroundQuery(GroundQuery query) {
    std::lock_guard<std::mutex> lock(mutex_);
    groundQuery_ = std::move(query);
    for (auto& [key, emitter] : emitters_) {
        (void)key;
        if (emitter)
            emitter->SetGroundQuery(groundQuery_);
    }
}

} // namespace whiteout::flakes::renderer::particle
