#include "clip_playlist.h"

#include <algorithm>

namespace whiteout::flakes::renderer::animation {

SequenceWindow WindowSequence(i32 startMs, i32 endMs, bool nonLooping, i32 elapsed,
                              bool forceLoop) {
    SequenceWindow w;
    if (elapsed < 0)
        elapsed = 0;

    const i32 duration = endMs - startMs;
    if (duration <= 0) {
        w.frameMs = startMs;
        return w;
    }
    if (nonLooping && !forceLoop) {
        w.ended = elapsed >= duration;
        w.frameMs = startMs + (std::min)(elapsed, duration);
        return w;
    }
    // Roll the start forward by whole durations so elapsed stays in
    // [0, duration). Each roll is a fresh cycle — consumers key off the count
    // to re-fire per loop.
    if (elapsed >= duration) {
        w.cycles = elapsed / duration;
        w.startShift = w.cycles * duration;
        elapsed -= w.startShift;
    }
    w.frameMs = startMs + elapsed;
    return w;
}

namespace {

f32 EnvelopeBlendIn(i32 nowMs, i32 phaseStartMs, i32 durationMs, f32 startWeight) {
    if (durationMs <= 0)
        return 1.0f;
    // Additive against the weight the play was already at, matching SC2 — a
    // play re-started mid-blend-out resumes from where it faded to rather than
    // snapping to zero.
    return (f32)(nowMs - phaseStartMs) / (f32)durationMs + startWeight;
}

f32 EnvelopeBlendOut(i32 nowMs, i32 phaseStartMs, i32 durationMs, f32 startWeight) {
    if (durationMs <= 0)
        return 0.0f;
    const f32 t = (f32)(nowMs - phaseStartMs) / (f32)durationMs;
    return (1.0f - t) * startWeight;
}

} // namespace

std::size_t ClipPlaylist::OverlayCount() const {
    std::size_t n = 0;
    while (n < plays_.size() && plays_[n].overlay)
        ++n;
    return n;
}

PlayHandle ClipPlaylist::Play(const PlayDesc& desc, i32 nowMs) {
    return PlayAt(desc, nowMs, OverlayCount());
}

PlayHandle ClipPlaylist::PlayAt(const PlayDesc& desc, i32 nowMs, std::size_t at) {
    PlayState st;
    st.desc = desc;
    st.handle = nextHandle_++;
    st.startTimeMs = nowMs;
    st.originTimeMs = nowMs;
    if (desc.blendInMs > 0) {
        st.phase = Phase::BlendIn;
        st.phaseStartMs = nowMs;
        st.phaseStartWeight = 0.0f;
        st.envelope = 0.0f;
    } else {
        st.phase = Phase::Steady;
        st.envelope = 1.0f;
    }
    // Newest first, which is the order the clip span reports and the order
    // equal-priority layers resolve in — below the global-loop overlays, which
    // hold the top of the stack whatever the host does.
    plays_.insert(plays_.begin() + (std::ptrdiff_t)(std::min)(at, plays_.size()), st);
    return st.handle;
}

void ClipPlaylist::SetGlobalSequences(std::vector<i32> sequences) {
    if (sequences == globalSeqs_)
        return;
    globalSeqs_ = std::move(sequences);
    globalsDirty_ = true;
}

void ClipPlaylist::ReconcileGlobals(i32 nowMs, std::span<const SequenceInfo> seqs) {
    // Stop what is no longer global, and note what already runs. Handles are
    // not reused across a rebuild, so a sequence that stayed global keeps its
    // clock rather than restarting — the dish does not jump when an `.m3a`
    // brings new sequences in beside it.
    std::vector<i32> live;
    for (std::size_t i = plays_.size(); i-- > 0;) {
        PlayState& p = plays_[i];
        if (!p.global)
            continue;
        const bool keep =
            std::find(globalSeqs_.begin(), globalSeqs_.end(), p.desc.sequence) != globalSeqs_.end();
        if (keep)
            live.push_back(p.desc.sequence);
        else
            plays_.erase(plays_.begin() + (std::ptrdiff_t)i);
    }
    const i32 n = (i32)seqs.size();
    for (i32 seq : globalSeqs_) {
        if (std::find(live.begin(), live.end(), seq) != live.end())
            continue;
        // Concurrent means "keys only what it owns", which is the whole reason
        // a global loop can run over a walk cycle without flattening it — and
        // also the reason it has to be sampled first. The 10% that are not
        // (61 of 621 flagged across the StarCraft II corpus) are models whose
        // global loop *is* their animation, and those stay at the bottom.
        const i32 idx = n > 0 ? ((seq % n) + n) % n : -1;
        const bool overlay = idx >= 0 && seqs[idx].concurrent;
        PlayDesc d;
        d.sequence = seq;
        // Persistent because the covered-play cull would otherwise drop it the
        // moment a full-body play settled on top, which is the one case these
        // exist to survive. StarCraft II spells the same thing as play flag
        // 0x12: bit 1 exempts the player from the cull, bit 4 puts it on the
        // world clock instead of its own (`M3Anim_InsertPlayer` args 6 and 10).
        d.persistent = true;
        // Abstain is the host-side spelling of the same thing the container's
        // `runsConcurrent` says to the M3 adapter, which reads that and not
        // this. Here it is only the cull's signal.
        d.mask = overlay ? ClipMask::Abstain : ClipMask::FillDefault;
        // No blend-in: the engine passes 0, and a fade would show as the light
        // ramping up every time the sequence table is rebuilt.
        d.blendInMs = 0;
        const PlayHandle h = PlayAt(d, nowMs, overlay ? 0 : plays_.size());
        for (auto& p : plays_) {
            if (p.handle != h)
                continue;
            p.global = true;
            p.overlay = overlay;
        }
    }
    globalsDirty_ = false;
}

void ClipPlaylist::Stop(PlayHandle h, i32 blendOutMs, i32 nowMs) {
    for (auto& p : plays_) {
        if (p.handle != h)
            continue;
        if (p.phase == Phase::BlendOut)
            return;
        const i32 dur = blendOutMs >= 0
                            ? blendOutMs
                            : (p.desc.blendOutMs >= 0 ? p.desc.blendOutMs : policy_.blendOutMs);
        p.phase = Phase::BlendOut;
        p.phaseStartMs = nowMs;
        p.phaseStartWeight = p.envelope;
        p.primary = false;
        if (dur <= 0)
            p.envelope = 0.0f;
        p.desc.blendOutMs = dur;
        return;
    }
}

void ClipPlaylist::StopAll(i32 blendOutMs, i32 nowMs) {
    // Copy the handles first: Stop() walks the vector, and stopping every play
    // in one pass over a container it also mutates is the kind of thing that
    // only breaks once the layered API is actually used.
    std::vector<PlayHandle> handles;
    handles.reserve(plays_.size());
    for (const auto& p : plays_)
        if (!p.global) // model-owned; only SetGlobalSequences retires these
            handles.push_back(p.handle);
    for (PlayHandle h : handles)
        Stop(h, blendOutMs, nowMs);
}

bool ClipPlaylist::Retune(PlayHandle h, f32 weight, f32 speed, bool loop) {
    for (auto& p : plays_) {
        if (p.handle != h)
            continue;
        p.desc.weight = weight;
        p.desc.speed = speed;
        p.desc.loop = loop;
        return true;
    }
    return false;
}

ClipPlaylist::PlayState* ClipPlaylist::Primary() {
    for (auto& p : plays_)
        if (p.primary)
            return &p;
    return nullptr;
}

const ClipPlaylist::PlayState* ClipPlaylist::Primary() const {
    return const_cast<ClipPlaylist*>(this)->Primary();
}

void ClipPlaylist::SetPrimaryTimeMs(i32 frameMs, i32 nowMs, std::span<const SequenceInfo> seqs) {
    PlayState* p = Primary();
    if (!p || seqs.empty())
        return;
    const i32 n = (i32)seqs.size();
    const i32 idx = ((p->desc.sequence % n) + n) % n;
    // Re-base rather than write the time directly: the next Advance recomputes
    // from the start stamp, so a scrub that only set the output would be
    // overwritten one frame later.
    const i32 newStart = nowMs - (frameMs - seqs[idx].startMs);
    p->originTimeMs += newStart - p->startTimeMs;
    p->startTimeMs = newStart;
    primaryTimeMs_ = frameMs;
}

void ClipPlaylist::Restart(i32 nowMs) {
    // Every stamp a play holds is in the old clock domain: the window start,
    // the unwrapped origin and the envelope's phase start. Move all three, or a
    // layered play comes back with a negative blend envelope.
    for (auto& p : plays_) {
        p.startTimeMs = nowMs;
        p.originTimeMs = nowMs;
        p.phaseStartMs = nowMs;
        p.cycles = 0;
    }
    restartPending_ = true;
}

void ClipPlaylist::RetireCovered(std::span<const SequenceInfo> seqs) {
    // Newest first. Once a play is settled, at full weight, opaque and driving
    // the whole skeleton, nothing below it can show through — so everything
    // below it that is not persistent is dead weight.
    //
    // `SequenceInfo::concurrent` is the fourth condition and the one that is
    // not the host's to state. A StarCraft II sequence whose sub-tracks all
    // run concurrent only drives the properties it keys — the Marine's `Cover`
    // is a shield and nothing else — so it buries nothing however settled and
    // full-weight it is. Judging that from `desc.mask` alone (the host's word
    // for the same idea) retired the primary play the moment any such layer
    // settled, which reads as the model snapping to the layer's pose.
    const i32 n = (i32)seqs.size();
    for (std::size_t i = 0; i < plays_.size(); ++i) {
        const PlayState& p = plays_[i];
        const i32 idx = n > 0 ? ((p.desc.sequence % n) + n) % n : -1;
        const bool covers = !p.global && p.phase == Phase::Steady && p.envelope >= 1.0f &&
                            p.desc.weight >= 1.0f && p.desc.mask == ClipMask::FillDefault &&
                            p.desc.rootNode < 0 && (idx < 0 || !seqs[idx].concurrent);
        if (!covers)
            continue;
        plays_.erase(std::remove_if(plays_.begin() + (std::ptrdiff_t)i + 1, plays_.end(),
                                    [](const PlayState& q) { return !q.desc.persistent; }),
                     plays_.end());
        return;
    }
}

void ClipPlaylist::Advance(i32 nowMs, std::span<const SequenceInfo> seqs, bool forceLoop) {
    if (seqs.empty()) {
        // Matches the old cursor exactly: with no sequence table there is
        // nothing to window and the reported time is left alone.
        clips_.clear();
        return;
    }
    const i32 n = (i32)seqs.size();

    if (globalsDirty_)
        ReconcileGlobals(nowMs, seqs);

    // ---- notice a sequence request ----
    //
    // Deliberately here and not in SetActiveSequence: the old cursor compared
    // `rawIdx != prevActiveSequence` inside Advance, so two sets between frames
    // collapsed to one transition. Hosts depend on that.
    // The two surfaces do not fight over the stack. A sequence request owns
    // only the play it created; a host driving the layered API exclusively has
    // no primary play, and its stack is left alone.
    //
    // "Driving the layered API" means a play the *host* asked for, so the test
    // discounts the ones this class started itself: an `.m3` with global loops
    // has a non-empty stack before the host has said anything, and reading that
    // as "the host is layering" left every such model stuck on sequence 0 with
    // the dropdown inert. Only global plays are discounted — a host-started
    // play still suppresses the request, persistent or not, which is what an
    // embedder driving `ActorView::Play` alone relies on.
    const PlayState* primary = Primary();
    const bool onlyGlobals =
        std::none_of(plays_.begin(), plays_.end(), [](const PlayState& p) { return !p.global; });
    //
    // A pending @ref Restart is unconditional — it neither waits for the index
    // to change nor asks whether the host is layering, because it is the host
    // saying so outright.
    const bool noticed = restartPending_ ||
                         (requestedSequence_ != acknowledgedSequence_ && (primary || onlyGlobals));
    if (noticed) {
        const bool hadPrimary = primary != nullptr && !restartPending_;
        acknowledgedSequence_ = requestedSequence_;
        restartPending_ = false;
        ++sequenceCycle_;

        if (policy_.crossFade && hadPrimary) {
            for (auto& p : plays_)
                if (p.primary)
                    Stop(p.handle, policy_.blendOutMs, nowMs);
            PlayDesc d;
            d.sequence = requestedSequence_;
            d.blendInMs = policy_.blendInMs;
            d.blendOutMs = policy_.blendOutMs;
            const PlayHandle h = Play(d, nowMs);
            for (auto& p : plays_)
                if (p.handle == h)
                    p.primary = true;
        } else {
            // Hard cut. The primary play is replaced and restarted at `now` —
            // the old `sequenceStartTimeMs = now` in a different container.
            std::erase_if(plays_, [](const PlayState& p) { return p.primary; });
            PlayState st;
            st.desc.sequence = requestedSequence_;
            st.handle = nextHandle_++;
            st.startTimeMs = nowMs;
            st.originTimeMs = nowMs;
            st.primary = true;
            plays_.insert(plays_.begin() + (std::ptrdiff_t)OverlayCount(), st);
        }
    }

    // ---- advance each play ----
    clips_.clear();
    clips_.reserve(plays_.size());
    const bool hasPrimary = Primary() != nullptr;

    for (std::size_t i = 0; i < plays_.size();) {
        PlayState& p = plays_[i];
        const i32 idx = ((p.desc.sequence % n) + n) % n;
        const SequenceInfo& seq = seqs[idx];

        i32 elapsed = nowMs - p.startTimeMs;
        i32 unwrapped = nowMs - p.originTimeMs;
        if (p.desc.speed != 1.0f) {
            elapsed = (i32)((f32)elapsed * p.desc.speed);
            unwrapped = (i32)((f32)unwrapped * p.desc.speed);
        }

        const bool nonLooping = seq.nonLooping || !p.desc.loop;
        const SequenceWindow w =
            WindowSequence(seq.startMs, seq.endMs, nonLooping, elapsed, forceLoop);

        p.startTimeMs += w.startShift;
        p.cycles += w.cycles;
        if (p.primary)
            sequenceCycle_ += w.cycles;

        // A non-looping play that reached its end retires itself, on its own
        // blend-out if it has one and the policy's otherwise.
        if (w.ended && p.phase != Phase::BlendOut) {
            const i32 dur = p.desc.blendOutMs >= 0 ? p.desc.blendOutMs : policy_.blendOutMs;
            // Hard-cut sources hold the last frame forever rather than fading
            // out of a pose nothing replaces — the old behaviour.
            if (policy_.crossFade) {
                p.phase = Phase::BlendOut;
                p.phaseStartMs = nowMs;
                p.phaseStartWeight = p.envelope;
                p.desc.blendOutMs = dur;
                if (dur <= 0)
                    p.envelope = 0.0f;
            }
        }

        switch (p.phase) {
        case Phase::BlendIn:
            p.envelope = EnvelopeBlendIn(nowMs, p.phaseStartMs, p.desc.blendInMs, p.phaseStartWeight);
            if (p.envelope >= 1.0f) {
                p.envelope = 1.0f;
                p.phase = Phase::Steady;
            }
            break;
        case Phase::BlendOut:
            p.envelope =
                EnvelopeBlendOut(nowMs, p.phaseStartMs, p.desc.blendOutMs, p.phaseStartWeight);
            break;
        case Phase::Steady:
            p.envelope = 1.0f;
            break;
        }

        if (p.phase == Phase::BlendOut && p.envelope <= 0.0f) {
            plays_.erase(plays_.begin() + (std::ptrdiff_t)i);
            continue;
        }

        ClipRef c;
        c.sequence = idx;
        c.timeMs = w.frameMs;
        c.elapsedMs = unwrapped < 0 ? 0 : unwrapped;
        c.weight = p.desc.weight * p.envelope;
        c.speed = p.desc.speed;
        c.loop = !nonLooping;
        c.mask = p.desc.mask;
        c.rootNode = p.desc.rootNode;
        c.subtrack = p.desc.subtrack;
        clips_.push_back(c);

        // With no primary play the newest one reports the time, so a purely
        // layered host still has a meaningful TimeMs().
        if (p.primary || (!hasPrimary && clips_.size() == 1))
            primaryTimeMs_ = w.frameMs;

        ++i;
    }

    RetireCovered(seqs);
}

} // namespace whiteout::flakes::renderer::animation
