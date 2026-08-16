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

PlayHandle ClipPlaylist::Play(const PlayDesc& desc, i32 nowMs) {
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
    // equal-priority layers resolve in.
    plays_.insert(plays_.begin(), st);
    return st.handle;
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
        handles.push_back(p.handle);
    for (PlayHandle h : handles)
        Stop(h, blendOutMs, nowMs);
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

void ClipPlaylist::RetireCovered() {
    // Newest first. Once a play is settled, at full weight, opaque and driving
    // the whole skeleton, nothing below it can show through — so everything
    // below it that is not persistent is dead weight.
    for (std::size_t i = 0; i < plays_.size(); ++i) {
        const PlayState& p = plays_[i];
        const bool covers = p.phase == Phase::Steady && p.envelope >= 1.0f &&
                            p.desc.weight >= 1.0f && p.desc.mask == ClipMask::FillDefault &&
                            p.desc.rootNode < 0;
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

    // ---- notice a sequence request ----
    //
    // Deliberately here and not in SetActiveSequence: the old cursor compared
    // `rawIdx != prevActiveSequence` inside Advance, so two sets between frames
    // collapsed to one transition. Hosts depend on that.
    // The two surfaces do not fight over the stack. A sequence request owns
    // only the play it created; a host driving the layered API exclusively has
    // no primary play, and its stack is left alone.
    const PlayState* primary = Primary();
    if (requestedSequence_ != acknowledgedSequence_ && (primary || plays_.empty())) {
        const bool hadPrimary = primary != nullptr;
        acknowledgedSequence_ = requestedSequence_;
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
            plays_.insert(plays_.begin(), st);
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
        clips_.push_back(c);

        // With no primary play the newest one reports the time, so a purely
        // layered host still has a meaningful TimeMs().
        if (p.primary || (!hasPrimary && clips_.size() == 1))
            primaryTimeMs_ = w.frameMs;

        ++i;
    }

    RetireCovered();
}

} // namespace whiteout::flakes::renderer::animation
