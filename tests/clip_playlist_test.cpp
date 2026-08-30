// ============================================================================
// ClipPlaylist — the shared playback layer.
//
// The load-bearing test here is LegacyAdvanceParity. Warcraft III and World of
// Warcraft are gated byte-identical across the refactor that introduced this
// class, and the only thing standing between "the playlist works" and "the
// goldens moved by one frame" is that the hard-cut path reproduces the old
// `Actor::Advance` arithmetic exactly. So the old algorithm is transcribed
// verbatim below as `LegacyCursor` and the two are run side by side over a
// table of sequence shapes and tick patterns.
//
// LegacyCursor is a copy, deliberately. Calling the real code would only prove
// it equals itself.
// ============================================================================

#include "renderer/animation/clip_playlist.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

using namespace whiteout::flakes;
using namespace whiteout::flakes::renderer::animation;
using Catch::Approx;

namespace {

SequenceInfo Seq(const char* name, int startMs, int endMs, bool nonLooping = false) {
    SequenceInfo s;
    s.name = name;
    s.startMs = startMs;
    s.endMs = endMs;
    s.nonLooping = nonLooping;
    return s;
}

// ---------------------------------------------------------------------------
// The pre-refactor Actor::Advance, transcribed. Do not "improve" this.
// ---------------------------------------------------------------------------
struct LegacyCursor {
    int actorTimeMs = 0;
    int sequenceStartTimeMs = 0;
    int prevActiveSequence = -1;
    int sequenceCycle = 0;
    int timeMs = 0;

    void Advance(float dtSec, float playbackSpeed, int rawIdx,
                 const std::vector<SequenceInfo>& seqs, bool ignoreNonLooping) {
        const int dtMs = (dtSec > 0.0f) ? (int)(dtSec * playbackSpeed * 1000.0f + 0.5f) : 0;
        actorTimeMs += dtMs;
        const int now = actorTimeMs;
        if (seqs.empty())
            return;

        const int n = (int)seqs.size();
        const int boundedIdx = ((rawIdx % n) + n) % n;
        if (rawIdx != prevActiveSequence) {
            sequenceStartTimeMs = now;
            prevActiveSequence = rawIdx;
            ++sequenceCycle;
        }

        const auto& seq = seqs[boundedIdx];
        const int duration = seq.endMs - seq.startMs;
        int elapsed = now - sequenceStartTimeMs;
        if (elapsed < 0)
            elapsed = 0;

        int frameMs;
        if (duration <= 0) {
            frameMs = seq.startMs;
        } else if (seq.nonLooping && !ignoreNonLooping) {
            frameMs = seq.startMs + (std::min)(elapsed, duration);
        } else {
            if (elapsed >= duration) {
                const int cycles = elapsed / duration;
                sequenceStartTimeMs += cycles * duration;
                sequenceCycle += cycles;
                elapsed -= cycles * duration;
            }
            frameMs = seq.startMs + elapsed;
        }
        timeMs = frameMs;
    }
};

// The playlist side of the same loop: the actor clock stays outside the class,
// exactly as Actor::Advance keeps it.
struct PlaylistDriver {
    ClipPlaylist pl;
    int actorTimeMs = 0;

    void Advance(float dtSec, float playbackSpeed, int rawIdx,
                 const std::vector<SequenceInfo>& seqs, bool ignoreNonLooping) {
        const int dtMs = (dtSec > 0.0f) ? (int)(dtSec * playbackSpeed * 1000.0f + 0.5f) : 0;
        actorTimeMs += dtMs;
        pl.SetActiveSequence(rawIdx);
        pl.Advance(actorTimeMs, seqs, ignoreNonLooping);
    }
};

} // namespace

TEST_CASE("WindowSequence reproduces the legacy windowing", "[clip_playlist]") {
    SECTION("zero-length sequence pins to its start") {
        const auto w = WindowSequence(400, 400, false, 999, false);
        REQUIRE(w.frameMs == 400);
        REQUIRE(w.cycles == 0);
        REQUIRE_FALSE(w.ended);
    }
    SECTION("negative elapsed clamps to the start rather than reading backwards") {
        REQUIRE(WindowSequence(0, 1000, false, -5000, false).frameMs == 0);
    }
    SECTION("non-looping clamps and reports the end") {
        REQUIRE(WindowSequence(100, 600, true, 200, false).frameMs == 300);
        const auto past = WindowSequence(100, 600, true, 900, false);
        REQUIRE(past.frameMs == 600);
        REQUIRE(past.ended);
    }
    SECTION("forceLoop overrides the non-looping flag") {
        const auto w = WindowSequence(0, 500, true, 1200, true);
        REQUIRE(w.frameMs == 200);
        REQUIRE(w.cycles == 2);
        REQUIRE_FALSE(w.ended);
    }
    SECTION("looping rolls forward by whole durations") {
        const auto w = WindowSequence(1000, 1500, false, 1250, false);
        REQUIRE(w.cycles == 2);
        REQUIRE(w.startShift == 1000);
        REQUIRE(w.frameMs == 1250); // 1000 + (1250 - 1000)
    }
}

TEST_CASE("Hard-cut playback matches the legacy cursor exactly", "[clip_playlist]") {
    struct Case {
        const char* what;
        std::vector<SequenceInfo> seqs;
        float speed;
        bool ignoreNonLooping;
    };

    const std::vector<Case> cases = {
        {"looping", {Seq("Stand", 0, 1000)}, 1.0f, false},
        {"looping, offset window", {Seq("Walk", 1300, 2100)}, 1.0f, false},
        {"non-looping clamps", {Seq("Death", 0, 900, true)}, 1.0f, false},
        {"non-looping forced to loop", {Seq("Death", 0, 900, true)}, 1.0f, true},
        {"zero duration", {Seq("Pose", 500, 500)}, 1.0f, false},
        {"double speed", {Seq("Stand", 0, 1000)}, 2.0f, false},
        {"quarter speed", {Seq("Stand", 0, 1000)}, 0.25f, false},
        {"multi-sequence",
         {Seq("Stand", 0, 1000), Seq("Walk", 1000, 1700), Seq("Death", 1700, 2400, true)},
         1.0f,
         false},
    };

    // Tick pattern mixes a steady 60 Hz with stalls and a zero dt, because the
    // rounding in dtMs is where a rewrite would drift.
    const std::vector<float> dts = {0.0f,    1.0f / 60, 1.0f / 60, 1.0f / 60, 0.25f,
                                    1.0f / 60, 0.0f,    2.5f,      1.0f / 30, 1.0f / 60};

    for (const auto& c : cases) {
        DYNAMIC_SECTION(c.what) {
            LegacyCursor legacy;
            PlaylistDriver actual;

            // Sequence switches at fixed points, including a re-set of the same
            // index (which must not restart) and an out-of-range index.
            const std::vector<int> indices = {0, 0, 1, 1, 1, 2, 2, 0, 5, -1};

            for (int rep = 0; rep < 6; ++rep) {
                for (size_t i = 0; i < dts.size(); ++i) {
                    const int idx = indices[i] % (int)std::max<size_t>(1, c.seqs.size() + 2);
                    legacy.Advance(dts[i], c.speed, idx, c.seqs, c.ignoreNonLooping);
                    actual.Advance(dts[i], c.speed, idx, c.seqs, c.ignoreNonLooping);

                    INFO("rep " << rep << " tick " << i << " idx " << idx);
                    REQUIRE(actual.actorTimeMs == legacy.actorTimeMs);
                    REQUIRE(actual.pl.PrimaryTimeMs() == legacy.timeMs);
                    REQUIRE(actual.pl.SequenceCycle() == legacy.sequenceCycle);
                }
            }
        }
    }
}

TEST_CASE("Hard cut produces exactly one clip", "[clip_playlist]") {
    const std::vector<SequenceInfo> seqs = {Seq("Stand", 0, 1000), Seq("Walk", 1000, 1800)};
    ClipPlaylist pl;
    pl.SetActiveSequence(0);
    pl.Advance(0, seqs, false);
    REQUIRE(pl.Clips().size() == 1);

    pl.SetActiveSequence(1);
    pl.Advance(100, seqs, false);
    REQUIRE(pl.Clips().size() == 1);
    REQUIRE(pl.Clips()[0].sequence == 1);
    REQUIRE(pl.Clips()[0].weight == Approx(1.0f));
    // The switch restarts the clock, so the new play is at its own frame 0.
    REQUIRE(pl.Clips()[0].timeMs == 1000);
}

TEST_CASE("Empty sequence table leaves the reported time alone", "[clip_playlist]") {
    ClipPlaylist pl;
    const std::vector<SequenceInfo> none;
    pl.SetActiveSequence(3);
    pl.Advance(1234, none, false);
    REQUIRE(pl.Clips().empty());
    REQUIRE(pl.PrimaryTimeMs() == 0);
}

TEST_CASE("elapsedMs is unwrapped while timeMs wraps", "[clip_playlist]") {
    const std::vector<SequenceInfo> seqs = {Seq("Stand", 0, 1000)};
    ClipPlaylist pl;
    pl.SetActiveSequence(0);
    pl.Advance(0, seqs, false);

    // 2500 ms into a 1000 ms sequence.
    pl.Advance(2500, seqs, false);
    REQUIRE(pl.Clips().size() == 1);
    REQUIRE(pl.Clips()[0].timeMs == 500);
    REQUIRE(pl.Clips()[0].elapsedMs == 2500);
    REQUIRE(pl.SequenceCycle() == 3); // 1 for the initial switch + 2 wraps

    // The window start rolls forward as the play loops; the elapsed origin must
    // not, or a track wrapped by its own duration would alias to the sequence's
    // cycle. This second tick is the one that catches it.
    pl.Advance(3200, seqs, false);
    REQUIRE(pl.Clips()[0].timeMs == 200);
    REQUIRE(pl.Clips()[0].elapsedMs == 3200);

    // What the M3 sampler will actually do with it: a 700 ms track inside a
    // 1000 ms sequence wraps on its own period, landing somewhere the
    // sequence-windowed time never reports.
    REQUIRE(pl.Clips()[0].elapsedMs % 700 == 400);
}

TEST_CASE("Cross-fade overlaps two plays", "[clip_playlist]") {
    const std::vector<SequenceInfo> seqs = {Seq("Stand", 0, 1000), Seq("Walk", 0, 800)};
    ClipPlaylist pl;
    TransitionPolicy p;
    p.crossFade = true;
    p.blendInMs = 200;
    p.blendOutMs = 200;
    pl.SetTransitionPolicy(p);

    pl.SetActiveSequence(0);
    pl.Advance(0, seqs, false);
    REQUIRE(pl.Clips().size() == 1);

    pl.SetActiveSequence(1);
    pl.Advance(1000, seqs, false);
    REQUIRE(pl.Clips().size() == 2);

    SECTION("midpoint splits the weight") {
        pl.Advance(1100, seqs, false);
        REQUIRE(pl.Clips().size() == 2);
        // Newest first: the incoming play is ramping up, the outgoing down.
        REQUIRE(pl.Clips()[0].sequence == 1);
        REQUIRE(pl.Clips()[0].weight == Approx(0.5f));
        REQUIRE(pl.Clips()[1].sequence == 0);
        REQUIRE(pl.Clips()[1].weight == Approx(0.5f));
    }

    SECTION("the outgoing play is gone once the fade completes") {
        pl.Advance(1200, seqs, false);
        pl.Advance(1201, seqs, false);
        REQUIRE(pl.Clips().size() == 1);
        REQUIRE(pl.Clips()[0].sequence == 1);
        REQUIRE(pl.Clips()[0].weight == Approx(1.0f));
    }
}

TEST_CASE("Blend envelopes", "[clip_playlist]") {
    const std::vector<SequenceInfo> seqs = {Seq("Stand", 0, 1000)};

    SECTION("blend-in ramps then settles at exactly 1") {
        ClipPlaylist pl;
        PlayDesc d;
        d.sequence = 0;
        d.blendInMs = 400;
        pl.Play(d, 0);
        pl.SetActiveSequence(0);

        pl.Advance(100, seqs, false);
        REQUIRE(pl.Clips().back().weight == Approx(0.25f));
        pl.Advance(300, seqs, false);
        REQUIRE(pl.Clips().back().weight == Approx(0.75f));
        pl.Advance(400, seqs, false);
        REQUIRE(pl.Clips().back().weight == Approx(1.0f));
        pl.Advance(900, seqs, false);
        REQUIRE(pl.Clips().back().weight == Approx(1.0f)); // does not overshoot
    }

    SECTION("blend-out ramps down from the weight it had") {
        ClipPlaylist pl;
        PlayDesc d;
        d.sequence = 0;
        const PlayHandle h = pl.Play(d, 0);
        pl.Advance(0, seqs, false);

        pl.Stop(h, 200, 0);
        pl.Advance(50, seqs, false);
        REQUIRE(pl.Clips().back().weight == Approx(0.75f));
        pl.Advance(150, seqs, false);
        REQUIRE(pl.Clips().back().weight == Approx(0.25f));
        pl.Advance(200, seqs, false);
        REQUIRE(pl.Clips().empty());
    }

    SECTION("stopping mid-blend-in fades from where it got to") {
        ClipPlaylist pl;
        PlayDesc d;
        d.sequence = 0;
        d.blendInMs = 400;
        const PlayHandle h = pl.Play(d, 0);
        pl.Advance(200, seqs, false);
        REQUIRE(pl.Clips().back().weight == Approx(0.5f));

        pl.Stop(h, 100, 200);
        pl.Advance(250, seqs, false);
        REQUIRE(pl.Clips().back().weight == Approx(0.25f)); // half of 0.5
    }

    SECTION("a zero-length blend-out removes the play at once") {
        ClipPlaylist pl;
        PlayDesc d;
        d.sequence = 0;
        const PlayHandle h = pl.Play(d, 0);
        pl.Advance(0, seqs, false);
        pl.Stop(h, 0, 0);
        pl.Advance(1, seqs, false);
        REQUIRE(pl.Clips().empty());
    }
}

TEST_CASE("Non-looping plays retire themselves under cross-fade", "[clip_playlist]") {
    const std::vector<SequenceInfo> seqs = {Seq("Death", 0, 500, true)};
    ClipPlaylist pl;
    TransitionPolicy p;
    p.crossFade = true;
    p.blendOutMs = 100;
    pl.SetTransitionPolicy(p);

    PlayDesc d;
    d.sequence = 0;
    pl.Play(d, 0);
    pl.Advance(0, seqs, false);
    REQUIRE(pl.Clips().size() == 1);

    pl.Advance(500, seqs, false); // reaches the end, starts fading
    REQUIRE(pl.Clips().size() == 1);
    pl.Advance(601, seqs, false);
    REQUIRE(pl.Clips().empty());
}

TEST_CASE("A hard-cut source holds the last frame of a non-looping sequence",
          "[clip_playlist]") {
    // Warcraft III behaviour: nothing replaces the pose, so fading out of it
    // would leave the model in bind pose.
    const std::vector<SequenceInfo> seqs = {Seq("Death", 0, 500, true)};
    ClipPlaylist pl;
    pl.SetActiveSequence(0);
    pl.Advance(0, seqs, false);
    pl.Advance(5000, seqs, false);
    REQUIRE(pl.Clips().size() == 1);
    REQUIRE(pl.Clips()[0].timeMs == 500);
    REQUIRE(pl.Clips()[0].weight == Approx(1.0f));
}

TEST_CASE("Covered plays are culled", "[clip_playlist]") {
    const std::vector<SequenceInfo> seqs = {Seq("Stand", 0, 1000), Seq("Walk", 0, 800)};

    SECTION("a settled full-weight opaque play removes what is under it") {
        ClipPlaylist pl;
        PlayDesc under;
        under.sequence = 0;
        pl.Play(under, 0);
        PlayDesc over;
        over.sequence = 1;
        pl.Play(over, 0);
        pl.Advance(0, seqs, false);
        REQUIRE(pl.PlayCount() == 1);
        REQUIRE(pl.Clips()[0].sequence == 1);
    }

    SECTION("a persistent play survives being covered") {
        ClipPlaylist pl;
        PlayDesc under;
        under.sequence = 0;
        under.persistent = true;
        pl.Play(under, 0);
        PlayDesc over;
        over.sequence = 1;
        pl.Play(over, 0);
        pl.Advance(0, seqs, false);
        REQUIRE(pl.PlayCount() == 2);
    }

    SECTION("a partial-weight play covers nothing") {
        ClipPlaylist pl;
        PlayDesc under;
        under.sequence = 0;
        pl.Play(under, 0);
        PlayDesc over;
        over.sequence = 1;
        over.weight = 0.9f;
        pl.Play(over, 0);
        pl.Advance(0, seqs, false);
        REQUIRE(pl.PlayCount() == 2);
    }

    SECTION("an abstaining play covers nothing") {
        ClipPlaylist pl;
        PlayDesc under;
        under.sequence = 0;
        pl.Play(under, 0);
        PlayDesc over;
        over.sequence = 1;
        over.mask = ClipMask::Abstain;
        pl.Play(over, 0);
        pl.Advance(0, seqs, false);
        REQUIRE(pl.PlayCount() == 2);
    }

    SECTION("a subtree-scoped play covers nothing") {
        ClipPlaylist pl;
        PlayDesc under;
        under.sequence = 0;
        pl.Play(under, 0);
        PlayDesc over;
        over.sequence = 1;
        over.rootNode = 7;
        pl.Play(over, 0);
        pl.Advance(0, seqs, false);
        REQUIRE(pl.PlayCount() == 2);
    }

    SECTION("a play still blending in covers nothing") {
        ClipPlaylist pl;
        PlayDesc under;
        under.sequence = 0;
        pl.Play(under, 0);
        PlayDesc over;
        over.sequence = 1;
        over.blendInMs = 500;
        pl.Play(over, 0);
        pl.Advance(100, seqs, false);
        REQUIRE(pl.PlayCount() == 2);
    }
}

TEST_CASE("A concurrent sequence covers nothing", "[clip_playlist]") {
    // The cull's fourth condition, and the one the host cannot state: a
    // StarCraft II sequence whose sub-track containers all run concurrent only
    // drives the properties it keys, so however settled and full-weight it is,
    // what is under it still shows through. Judging coverage from `desc.mask`
    // alone retired the primary play the instant a shield-only layer settled.
    std::vector<SequenceInfo> seqs = {Seq("Stand", 0, 1000), Seq("Cover", 0, 33)};
    seqs[1].concurrent = true;

    ClipPlaylist pl;
    PlayDesc under;
    under.sequence = 0;
    pl.Play(under, 0);
    PlayDesc over;
    over.sequence = 1;
    pl.Play(over, 0);
    pl.Advance(0, seqs, false);
    REQUIRE(pl.PlayCount() == 2);

    // ...and the opaque case is unchanged, which is what keeps every existing
    // `layer=` baseline where it was.
    seqs[1].concurrent = false;
    ClipPlaylist pl2;
    pl2.Play(under, 0);
    pl2.Play(over, 0);
    pl2.Advance(0, seqs, false);
    REQUIRE(pl2.PlayCount() == 1);
}

TEST_CASE("Global loops play themselves", "[clip_playlist]") {
    std::vector<SequenceInfo> seqs = {Seq("Stand", 0, 1000), Seq("GLstand", 0, 500),
                                      Seq("GLbirth", 0, 700)};
    seqs[1].alwaysPlays = true;
    seqs[1].concurrent = true;
    seqs[2].alwaysPlays = true;
    seqs[2].concurrent = true;

    SECTION("the set is reconciled, not replayed") {
        ClipPlaylist pl;
        pl.SetGlobalSequences({1, 2});
        pl.Advance(0, seqs, false);
        // Three: the two globals, plus the sequence-0 play the untouched
        // `SetActiveSequence` default asks for once the stack is only globals.
        REQUIRE(pl.PlayCount() == 3);

        // Advance again: nothing restarts, because a live global that is still
        // in the set keeps its play (and therefore its clock).
        pl.Advance(100, seqs, false);
        REQUIRE(pl.PlayCount() == 3);
        for (const auto& c : pl.Clips())
            REQUIRE(c.elapsedMs == 100);

        // Dropping one leaves the other's clock alone rather than rebuilding
        // both — the light does not blink when an `.m3a` brings new sequences.
        pl.SetGlobalSequences({2});
        pl.Advance(200, seqs, false);
        REQUIRE(pl.PlayCount() == 2);
        bool sawGlobal = false;
        for (const auto& c : pl.Clips()) {
            REQUIRE(c.sequence != 1);
            REQUIRE(c.elapsedMs == 200);
            sawGlobal = sawGlobal || c.sequence == 2;
        }
        REQUIRE(sawGlobal);
    }

    SECTION("a global loop does not make the stack look host-driven") {
        // The regression this guards: a sequence request is only honoured when
        // the host is not already layering, and reading a model's own global
        // loops as "the host is layering" left the dropdown inert on every
        // `.m3` that has one.
        ClipPlaylist pl;
        pl.SetGlobalSequences({1});
        pl.Advance(0, seqs, false);
        pl.SetActiveSequence(0);
        pl.Advance(10, seqs, false);
        REQUIRE(pl.PlayCount() == 2);
        REQUIRE(pl.Clips()[1].sequence == 0); // the request, under the overlay
    }

    SECTION("a concurrent global stays above the sequence the host switches to") {
        // The regression this guards: the global was played once and every
        // later request went in *above* it, so a model's rotors turned until
        // the first switch and then stopped — an opaque full-body play above
        // an overlay spends the whole weight budget on default-fills before
        // the overlay is ever reached.
        ClipPlaylist pl;
        pl.SetGlobalSequences({1});
        pl.SetActiveSequence(0);
        pl.Advance(0, seqs, false);
        REQUIRE(pl.Clips()[0].sequence == 1);

        pl.SetActiveSequence(2);
        pl.Advance(10, seqs, false);
        REQUIRE(pl.PlayCount() == 2);
        REQUIRE(pl.Clips()[0].sequence == 1);
        REQUIRE(pl.Clips()[1].sequence == 2);

        // ... and a host layer goes under it too.
        PlayDesc d;
        d.sequence = 0;
        d.persistent = true;
        pl.Play(d, 10);
        pl.Advance(20, seqs, false);
        REQUIRE(pl.Clips()[0].sequence == 1);
    }

    SECTION("a global that is not concurrent stays at the bottom") {
        // It is not an overlay — it keys the whole skeleton, so it is the
        // model's own animation and an ordinary play is entitled to bury it.
        // 61 of the 621 flagged sequences in the StarCraft II corpus.
        std::vector<SequenceInfo> opaque = seqs;
        opaque[1].concurrent = false;
        ClipPlaylist pl;
        pl.SetGlobalSequences({1});
        pl.SetActiveSequence(0);
        pl.Advance(0, opaque, false);
        REQUIRE(pl.PlayCount() == 2);
        REQUIRE(pl.Clips()[0].sequence == 0);
        REQUIRE(pl.Clips()[1].sequence == 1);
    }

    SECTION("StopAll leaves them running") {
        // They belong to the model, not to whoever asked for the last play.
        ClipPlaylist pl;
        pl.SetGlobalSequences({1});
        PlayDesc d;
        d.sequence = 0;
        pl.Play(d, 0);
        pl.Advance(0, seqs, false);
        REQUIRE(pl.PlayCount() == 2);
        pl.StopAll(0, 0);
        pl.Advance(10, seqs, false);
        REQUIRE(pl.PlayCount() == 1);
        REQUIRE(pl.Clips()[0].sequence == 1);
    }
}

TEST_CASE("Retune leaves the clock alone", "[clip_playlist]") {
    // A host dragging a blend weight must not rewind the animation, which is
    // the whole reason this is not stop-and-replay.
    const std::vector<SequenceInfo> seqs = {Seq("Stand", 0, 1000)};
    ClipPlaylist pl;
    PlayDesc d;
    d.sequence = 0;
    d.persistent = true;
    const PlayHandle h = pl.Play(d, 0);
    pl.Advance(400, seqs, false);
    REQUIRE(pl.Clips()[0].elapsedMs == 400);

    REQUIRE(pl.Retune(h, 0.25f, 1.0f, true));
    pl.Advance(500, seqs, false);
    REQUIRE(pl.Clips()[0].elapsedMs == 500);
    REQUIRE(pl.Clips()[0].weight == Approx(0.25f));

    REQUIRE_FALSE(pl.Retune(h + 99, 1.0f, 1.0f, true));
}

TEST_CASE("A clip carries its sub-track selection", "[clip_playlist]") {
    const std::vector<SequenceInfo> seqs = {Seq("Cover", 0, 33)};
    ClipPlaylist pl;
    PlayDesc d;
    d.sequence = 0;
    d.subtrack = 1;
    d.persistent = true;
    pl.Play(d, 0);
    pl.Advance(0, seqs, false);
    REQUIRE(pl.Clips()[0].subtrack == 1);
}

TEST_CASE("Scrubbing re-bases so the next advance recomputes the same frame",
          "[clip_playlist]") {
    const std::vector<SequenceInfo> seqs = {Seq("Walk", 1000, 2000)};
    ClipPlaylist pl;
    pl.SetActiveSequence(0);
    pl.Advance(0, seqs, false);

    pl.SetPrimaryTimeMs(1600, 0, seqs);
    REQUIRE(pl.PrimaryTimeMs() == 1600);
    // Without the re-base this would snap back to 1000 on the next tick.
    pl.Advance(0, seqs, false);
    REQUIRE(pl.PrimaryTimeMs() == 1600);
    pl.Advance(100, seqs, false);
    REQUIRE(pl.PrimaryTimeMs() == 1700);
}

TEST_CASE("Newest play is reported first", "[clip_playlist]") {
    const std::vector<SequenceInfo> seqs = {Seq("A", 0, 1000), Seq("B", 0, 1000),
                                            Seq("C", 0, 1000)};
    ClipPlaylist pl;
    for (int i = 0; i < 3; ++i) {
        PlayDesc d;
        d.sequence = i;
        d.weight = 0.5f; // partial, so nothing culls anything
        pl.Play(d, 0);
    }
    pl.Advance(0, seqs, false);
    REQUIRE(pl.Clips().size() == 3);
    REQUIRE(pl.Clips()[0].sequence == 2);
    REQUIRE(pl.Clips()[1].sequence == 1);
    REQUIRE(pl.Clips()[2].sequence == 0);
}

TEST_CASE("StopAll fades every play", "[clip_playlist]") {
    const std::vector<SequenceInfo> seqs = {Seq("A", 0, 1000), Seq("B", 0, 1000)};
    ClipPlaylist pl;
    for (int i = 0; i < 2; ++i) {
        PlayDesc d;
        d.sequence = i;
        d.weight = 0.5f;
        pl.Play(d, 0);
    }
    pl.Advance(0, seqs, false);
    REQUIRE(pl.PlayCount() == 2);

    pl.StopAll(100, 0);
    pl.Advance(50, seqs, false);
    REQUIRE(pl.Clips().size() == 2);
    for (const auto& c : pl.Clips())
        REQUIRE(c.weight == Approx(0.25f)); // 0.5 desc weight * 0.5 envelope
    pl.Advance(101, seqs, false);
    REQUIRE(pl.Clips().empty());
}

TEST_CASE("Per-play speed scales that play's clock only", "[clip_playlist]") {
    const std::vector<SequenceInfo> seqs = {Seq("Stand", 0, 1000)};
    ClipPlaylist pl;
    PlayDesc slow;
    slow.sequence = 0;
    slow.speed = 0.5f;
    slow.weight = 0.5f;
    pl.Play(slow, 0);
    PlayDesc fast;
    fast.sequence = 0;
    fast.speed = 2.0f;
    fast.weight = 0.5f;
    pl.Play(fast, 0);

    pl.Advance(200, seqs, false);
    REQUIRE(pl.Clips().size() == 2);
    REQUIRE(pl.Clips()[0].timeMs == 400); // fast, newest first
    REQUIRE(pl.Clips()[1].timeMs == 100); // slow
}

// The animation export rewinds `Actor::cursor.actorTimeMs` to zero and exports
// the sequence that is already on screen. Both halves of that are invisible to
// the playlist on their own — a play's start stamp is left in the future by the
// rewind, and a request for the live sequence is a no-op by design — so every
// exported frame came out as the sequence's first frame.
TEST_CASE("Restart re-bases a rewound clock", "[clip_playlist]") {
    const std::vector<SequenceInfo> seqs = {Seq("Stand", 0, 1000), Seq("Walk", 1000, 2000)};
    ClipPlaylist pl;
    pl.SetActiveSequence(0);
    // Nine seconds of viewing: the window start has rolled forward eight times.
    for (int t = 0; t <= 9000; t += 100)
        pl.Advance(t, seqs, false);
    REQUIRE(pl.PrimaryTimeMs() == 0);

    pl.SetActiveSequence(0); // the export asks for the sequence already playing
    pl.Restart(0);
    pl.Advance(0, seqs, false);
    REQUIRE(pl.PrimaryTimeMs() == 0);
    pl.Advance(33, seqs, false);
    REQUIRE(pl.PrimaryTimeMs() == 33); // 0 without the restart, for every frame
    pl.Advance(66, seqs, false);
    REQUIRE(pl.PrimaryTimeMs() == 66);

    // Restoring the viewer: back onto the old clock, then scrub to the frame it
    // was showing.
    pl.Restart(9000);
    pl.Advance(9000, seqs, false);
    pl.SetPrimaryTimeMs(400, 9000, seqs);
    pl.Advance(9000, seqs, false);
    REQUIRE(pl.PrimaryTimeMs() == 400);
    pl.Advance(9100, seqs, false);
    REQUIRE(pl.PrimaryTimeMs() == 500);
}

// A restart is a hard cut even where a plain sequence switch cross-fades: the
// clock has moved, so there is no previous pose to fade out of.
// ---------------------------------------------------------------------------
// What the animation exporter's clip queue relies on.
//
// The export runner switches clips by bumping the RAW requested index by a
// whole multiple of the sequence count, and holds a pose by scrubbing every
// frame while the clock keeps running. Neither mechanism is reachable from
// export_recipe_test — that one is device-free and the schedule is a pure
// function — so the two contracts they depend on are pinned here, where the
// byte-identity contract for this class already lives.

TEST_CASE("A raw-index bump restarts the primary and leaves globals alone",
          "[clip_playlist]") {
    std::vector<SequenceInfo> seqs = {Seq("Stand", 0, 1000), Seq("Walk", 1000, 2000),
                                      Seq("GLstand", 2000, 2500)};
    seqs[2].alwaysPlays = true;
    seqs[2].concurrent = true;

    ClipPlaylist pl;
    pl.SetGlobalSequences({2});
    pl.SetActiveSequence(0);
    pl.Advance(0, seqs, false);

    // Let both run for a while.
    pl.Advance(400, seqs, false);
    const auto globalAt = [&] {
        for (const auto& c : pl.Clips())
            if (c.sequence == 2)
                return c.elapsedMs;
        return -1;
    };
    REQUIRE(globalAt() == 400);
    REQUIRE(pl.PrimaryTimeMs() == 400);

    SECTION("a bump onto the SAME bounded sequence is still noticed") {
        // The export's repeat case: the queue plays Stand twice in a row, so
        // the bounded sequence does not change and only the raw value can say
        // "start it again". Advance bounds with ((raw % n) + n) % n but
        // compares the raw values.
        const i32 cycleBefore = pl.SequenceCycle();
        pl.SetActiveSequence(0 + static_cast<i32>(seqs.size())); // still bounds to 0
        pl.Advance(400, seqs, false);
        REQUIRE(pl.PrimaryTimeMs() == 0);        // the play restarted
        REQUIRE(pl.SequenceCycle() == cycleBefore + 1); // single-shot emitters re-fire
        REQUIRE(globalAt() == 400);              // …and the global loop did not
    }

    SECTION("a bump onto a different sequence cuts to it") {
        pl.SetActiveSequence(1 + 2 * static_cast<i32>(seqs.size()));
        pl.Advance(400, seqs, false);
        REQUIRE(pl.PrimaryTimeMs() == seqs[1].startMs);
        REQUIRE(globalAt() == 400);
    }

    SECTION("Restart, by contrast, re-bases EVERY play") {
        // Which is why the export uses it only on frame 0, where the actor
        // clock genuinely rewound. Using it per clip would visibly restart an
        // `.m3`'s always-playing overlay at every boundary.
        pl.Restart(400);
        pl.Advance(400, seqs, false);
        REQUIRE(globalAt() == 0);
    }
}

TEST_CASE("A repeated scrub pins the pose while the clock advances",
          "[clip_playlist]") {
    // The export's hold. `playbackSpeed = 0` is NOT this: FrameTicker threads
    // the actor clock down the tree, so the children and the PE1 emitters
    // would freeze with their ancestor. The clock has to keep moving.
    const std::vector<SequenceInfo> seqs = {Seq("Attack", 2000, 2500)};
    ClipPlaylist pl;
    pl.SetActiveSequence(0);
    pl.Advance(0, seqs, false);

    // One short of the end, which is what the schedule emits: a scrub is
    // windowed exactly like playback, so 2500 on a 500 ms looping clip wraps
    // to 2000 — the FIRST frame — and the hold would freeze the wrong pose.
    const i32 holdFrameMs = 2499;
    for (i32 nowMs = 100; nowMs <= 2000; nowMs += 100) {
        pl.SetPrimaryTimeMs(holdFrameMs, nowMs, seqs);
        pl.Advance(nowMs, seqs, false);
        REQUIRE(pl.PrimaryTimeMs() == holdFrameMs);
    }
    // The play is still there and still pinned twenty holds later — the scrub
    // re-bases the start stamp rather than writing an output the next Advance
    // would overwrite.
    REQUIRE(pl.PlayCount() == 1);
}

TEST_CASE("Restart hard-cuts under a cross-fade policy", "[clip_playlist]") {
    const std::vector<SequenceInfo> seqs = {Seq("Stand", 0, 1000), Seq("Walk", 0, 800)};
    ClipPlaylist pl;
    TransitionPolicy policy;
    policy.crossFade = true;
    policy.blendInMs = 200;
    policy.blendOutMs = 200;
    pl.SetTransitionPolicy(policy);

    pl.SetActiveSequence(0);
    pl.Advance(0, seqs, false);
    pl.Advance(2500, seqs, false);

    pl.SetActiveSequence(1);
    pl.Restart(0);
    pl.Advance(0, seqs, false);
    REQUIRE(pl.Clips().size() == 1);
    REQUIRE(pl.Clips()[0].sequence == 1);
    REQUIRE(pl.Clips()[0].weight == Approx(1.0f));
    REQUIRE(pl.Clips()[0].timeMs == 0);
}

// Global loops are re-based too. They keep their own clock across a sequence
// switch, which after a rewind would leave them stuck on their first frame for
// the whole export.
TEST_CASE("Restart re-bases global loops", "[clip_playlist]") {
    std::vector<SequenceInfo> seqs = {Seq("Stand", 0, 1000), Seq("Glow", 0, 500)};
    seqs[1].alwaysPlays = true;
    ClipPlaylist pl;
    pl.SetGlobalSequences({1});
    pl.SetActiveSequence(0);
    for (int t = 0; t <= 4000; t += 100)
        pl.Advance(t, seqs, false);

    pl.Restart(0);
    pl.Advance(0, seqs, false);
    pl.Advance(120, seqs, false);
    bool sawGlow = false;
    for (const auto& c : pl.Clips()) {
        if (c.sequence != 1)
            continue;
        sawGlow = true;
        REQUIRE(c.timeMs == 120);
    }
    REQUIRE(sawGlow);
}
