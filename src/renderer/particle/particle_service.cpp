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

bool ParticleService::HasRefractionEmitters() const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [k, e] : emitters_) {
        if (e->Desc().refraction)
            return true;
        for (const auto& t : e->Trails())
            if (t->Desc().refraction)
                return true;
    }
    return false;
}

void ParticleService::BuildGeometry(const Matrix44f& worldToView, std::vector<Vertex>& outVertices,
                                    std::vector<EmitterDrawList>& outDrawLists,
                                    MultiTexGeometry* refraction,
                                    MultiTexGeometry* multiTex, D3VertexStream* d3Uv) const {
    std::lock_guard<std::mutex> lock(mutex_);

    BuildGeometryInput in{};
    in.worldToView = &worldToView;
    in.fogEnabled = fogEnabled_;
    in.fogSampler = fogSampler_;
    if (d3Uv) {
        in.d3Uv01 = &d3Uv->uv01;
        in.d3Uv23 = &d3Uv->uv23;
        in.d3Color1 = &d3Uv->color1;
    }

    // The two side streams put their VERTICES somewhere other than
    // `outVertices`, so they must not also append to arrays kept parallel with
    // it. No shipped emitter is both Diablo III and multi-texture — the flags
    // are M2's and the D3 adapter never sets them — but the invariant is worth
    // holding structurally rather than by that argument.
    BuildGeometryInput refractIn = in;
    refractIn.d3Uv01 = nullptr;
    refractIn.d3Uv23 = nullptr;
    refractIn.d3Color1 = nullptr;
    if (refraction)
        refractIn.extraUV = &refraction->extraUV;

    BuildGeometryInput multiTexIn = in;
    multiTexIn.d3Uv01 = nullptr;
    multiTexIn.d3Uv23 = nullptr;
    multiTexIn.d3Color1 = nullptr;
    if (multiTex)
        multiTexIn.extraUV = &multiTex->extraUV;

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
    auto build = [&](const Emitter2& e, ModelId model, i32 id, const Vector3f& origin) {
        if (e.Desc().refraction) {
            if (!refraction)
                return;
            const i32 offset = (i32)refraction->vertices.size();
            const i32 vcount = e.BuildGeometry(refractIn, refraction->vertices);
            if (vcount > 0)
                refraction->draws.push_back({model, id, offset, vcount, e.PriorityPlane(),
                                             e.Material(), origin, e.MaterialTimeSec()});
            return;
        }
        if (e.Desc().multiTexture && multiTex) {
            const i32 offset = (i32)multiTex->vertices.size();
            const i32 vcount = e.BuildGeometry(multiTexIn, multiTex->vertices);
            if (vcount > 0)
                outDrawLists.push_back({model, id, offset, vcount, e.PriorityPlane(),
                                        e.Material(), origin, e.MaterialTimeSec()});
            return;
        }
        // Levelled BEFORE the build, not after: a D3 emitter appends its
        // texcoords as it appends its vertices, so the two arrays have to
        // already be the same length or its first quad's coordinates land under
        // whatever an earlier emitter's quads occupy.
        if (in.d3Uv01) {
            in.d3Uv01->resize(outVertices.size());
            in.d3Uv23->resize(outVertices.size());
            in.d3Color1->resize(outVertices.size(), 1.0f);
        }
        const i32 offset = (i32)outVertices.size();
        const i32 vcount = e.BuildGeometry(in, outVertices);
        if (vcount > 0) {
            EmitterDrawList dl{model,           id,           offset, vcount, e.PriorityPlane(),
                               e.Material(), origin, e.MaterialTimeSec()};
            // Nothing downstream can shade three layers off this stream, so the
            // draw must not claim it carries them.
            dl.material.multiTexture = false;
            outDrawLists.push_back(dl);
        }
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
    // The last emitter may have been one that writes no texcoords.
    if (d3Uv) {
        d3Uv->uv01.resize(outVertices.size());
        d3Uv->uv23.resize(outVertices.size());
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
