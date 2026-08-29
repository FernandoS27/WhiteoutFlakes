#include "renderer/effects/d3_attachment_pool.h"

#include "io/d3/d3_effect_resolver.h"
#include "io/d3/d3_model_adapter.h"
#include "io/d3/d3_particle_adapter.h"
#include "renderer/effects/event_crossing.h"
#include "renderer/model/model_instance.h"
#include "renderer/particle/d3_emitter.h"
#include "renderer/particle/particle_service.h"
#include "renderer/profiles/diablo3/d3_particle_shading.h"

#include <utility>

namespace whiteout::flakes::renderer::effects {

using namespace ::whiteout::flakes::renderer::model;

void D3AttachmentPool::Bind(std::shared_ptr<io::D3ModelAdapter> adapter, io::D3SnoCache* cache,
                            i32 firstEmitterId, std::function<u32()> allocHandle) {
    adapter_ = std::move(adapter);
    cache_ = cache;
    allocHandle_ = std::move(allocHandle);
    bySequence_.clear();
    pending_.clear();
    nextEmitterId_ = firstEmitterId;
    prevSeq_ = -1;
    prevTimeMs_ = 0;
}

std::vector<D3AttachmentPool::Entry>* D3AttachmentPool::Resolve(i32 seq) {
    if (auto it = bySequence_.find(seq); it != bySequence_.end())
        return &it->second;
    if (!adapter_ || !cache_)
        return nullptr;

    std::vector<Entry> entries;
    const auto& app = adapter_->SourceAppearance();
    // Read now rather than cached at bind: a wardrobe change moves the look on
    // the adapter in place, and select mode 10 asks for the current one.
    std::string_view look;
    if (adapter_->LookIndex() < adapter_->Looks().size())
        look = adapter_->Looks()[adapter_->LookIndex()];
    for (const auto& att : adapter_->ClipAttachments(seq)) {
        if (!att.event)
            continue;
        // One attachment can be several effects: a group 14 payload is a
        // library and select mode 2 — 4,888 of 6,426 files — plays all of it.
        std::vector<io::d3::ResolvedEffect> fx;
        io::d3::ExpandD3TriggerEvent(*att.event, *cache_, look, fx);
        for (const auto& f : fx) {
            Entry e;
            e.times.push_back(static_cast<u32>(att.timeMs < 0 ? 0 : att.timeMs));
            const auto attach = io::d3::ResolveD3Attach(app, f.hardpoint);
            e.bone = attach.bone;
            e.offset = attach.offset;
            if (f.kind == io::d3::ResolvedEffect::Kind::Actor)
                e.snoActor = f.sno;
            else
                e.snoParticle = f.sno;
            entries.push_back(std::move(e));
        }
    }
    return &bySequence_.emplace(seq, std::move(entries)).first->second;
}

void D3AttachmentPool::Tick(Actor& actor, i32 activeSeq, i32 localTimeMs, i32 seqStartMs,
                            i32 seqEndMs, particle::ParticleService* particles) {
    if (!adapter_)
        return;

    // A sequence switch rewinds the cursor to just before the window, so an
    // attachment sitting on the clip's very first millisecond still fires —
    // 4,283 of the 7,570 particle attachments are at frame 0.
    if (activeSeq != prevSeq_) {
        prevSeq_ = activeSeq;
        prevTimeMs_ = seqStartMs - 1;
    }

    auto* list = Resolve(activeSeq);
    if (!list || list->empty()) {
        prevTimeMs_ = localTimeMs;
        return;
    }

    for (usize i = 0; i < list->size(); ++i) {
        Entry& e = (*list)[i];
        if (e.dead)
            continue;
        if (CrossedKeyCount(e.times, prevTimeMs_, localTimeMs, seqStartMs, seqEndMs) == 0)
            continue;

        if (e.snoParticle >= 0) {
            if (!particles)
                continue;
            if (e.emitterId < 0) {
                auto prt = cache_ ? cache_->Particle(e.snoParticle) : nullptr;
                if (!prt) {
                    e.dead = true;
                    continue;
                }
                auto desc = io::d3::BuildD3EmitterDesc(*prt, e.snoParticle);
                profiles::diablo3::D3ResolveParticleMaterial(*prt, cache_, desc->d3mat);
                profiles::diablo3::D3BindParticleTextures(actor, desc);
                auto em = std::make_unique<particle::d3::Emitter>();
                em->SetD3Desc(std::move(desc));
                em->SetAttachBone(e.bone);
                em->SetAttachOffset(e.offset);
                e.emitterId = nextEmitterId_++;
                e.output = em->Desc().output;
                if (allocHandle_)
                    em->SetChildOwner(actor.handle, e.emitterId, allocHandle_);
                particles->AddEmitter(actor.handle, e.output, e.emitterId, std::move(em));
                continue; // Freshly built: already at age zero.
            }
            auto* em = particles->GetEmitter(actor.handle, e.output, e.emitterId);
            if (auto* d3 = dynamic_cast<particle::d3::Emitter*>(em))
                d3->Restart();
            continue;
        }

        if (e.snoActor >= 0) {
            PendingChild p;
            p.snoActor = e.snoActor;
            p.bone = e.bone;
            p.offset = e.offset;
            p.existing = e.childHandle;
            p.sequence = activeSeq;
            p.entry = static_cast<i32>(i);
            pending_.push_back(p);
        }
    }

    prevTimeMs_ = localTimeMs;
}

std::vector<D3AttachmentPool::PendingChild> D3AttachmentPool::TakePending() {
    std::vector<PendingChild> out;
    out.swap(pending_);
    return out;
}

void D3AttachmentPool::NoteChildSpawned(i32 sequence, i32 entry, u32 handle) {
    auto it = bySequence_.find(sequence);
    if (it == bySequence_.end() || entry < 0 || entry >= static_cast<i32>(it->second.size()))
        return;
    Entry& e = it->second[static_cast<usize>(entry)];
    e.childHandle = handle;
    e.dead = (handle == 0);
}

u32 D3AttachmentPool::ChildHandle(i32 sequence, i32 entry) const {
    auto it = bySequence_.find(sequence);
    if (it == bySequence_.end() || entry < 0 || entry >= static_cast<i32>(it->second.size()))
        return 0;
    return it->second[static_cast<usize>(entry)].childHandle;
}

} // namespace whiteout::flakes::renderer::effects
