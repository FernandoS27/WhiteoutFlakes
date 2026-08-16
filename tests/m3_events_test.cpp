// ============================================================================
// Discrete keyframe events (design B13, plan P-A6).
//
// Two halves, and the split is deliberate.
//
// The *crossing* half tests `event_crossing.h`, which is format-neutral and was
// previously locked in `EventEmitterPool`'s anonymous namespace with no test at
// all — the loop-wrap branch in particular, which is the one that decides
// whether a looping animation keeps firing its events after the first cycle.
//
// The *decode* half tests `.m3`'s side: which SDEV keys become dispatchable
// configs, and — the part that is genuinely surprising — that a config is
// stamped with the sequence it came from, because M3 authors events inside a
// sub-track container and every container's track restarts at zero.
// ============================================================================

#include "io/m3/m3_model_adapter.h"
#include "m3_anim_builders.h"
#include "renderer/effects/event_crossing.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::flakes;
using namespace whiteout::flakes::io;
using whiteout::flakes::renderer::effects::CrossedKeyCount;
using whiteout::flakes::renderer::effects::KeysInHalfOpen;

namespace {

// A sequence window of [0, 1000] with keys at 100, 500 and 900.
const std::vector<u32> kKeys{100, 500, 900};
constexpr i32 kLo = 0;
constexpr i32 kHi = 1000;

// An SDEV key. The trailing NUL is the point: `Ref<CHAR>` keeps the terminator,
// so a name built the obvious way in a test would NOT reproduce what the parser
// hands the decoder, and a decoder bug that only bites on real files would slip
// through green.
m3::Event Ev(const char* name, u16 bone, const char* option = "") {
    m3::Event e;
    e.name = std::string(name) + '\0';
    e.optionString = std::string(option) + '\0';
    e.boneIndex = bone;
    e.eventType = 2;
    return e;
}

} // namespace

// ---------------------------------------------------------------------------
// Crossing (B13)
// ---------------------------------------------------------------------------

TEST_CASE("Crossing is half-open: a key fires on arrival, once", "[m3events]") {
    // (prev, now] — the key at 500 belongs to the frame that reached it.
    REQUIRE(CrossedKeyCount(kKeys, 400, 500, kLo, kHi) == 1);
    // The next frame starts where that one ended and must not re-fire it.
    REQUIRE(CrossedKeyCount(kKeys, 500, 600, kLo, kHi) == 0);
    // A cursor that did not move crosses nothing, however long it sits there.
    REQUIRE(CrossedKeyCount(kKeys, 500, 500, kLo, kHi) == 0);
}

TEST_CASE("A frame spanning several keys reports all of them", "[m3events]") {
    REQUIRE(CrossedKeyCount(kKeys, 0, 1000, kLo, kHi) == 3);
    REQUIRE(CrossedKeyCount(kKeys, 99, 901, kLo, kHi) == 3);
    REQUIRE(CrossedKeyCount(kKeys, 100, 900, kLo, kHi) == 2);
}

TEST_CASE("A loop wrap emits the tail then the head", "[m3events]") {
    // 900 -> 150 across the loop point. Both the tail (nothing after 900) and
    // the head (the key at 100) are crossed.
    REQUIRE(CrossedKeyCount(kKeys, 900, 150, kLo, kHi) == 1);
    // 800 -> 150 crosses 900 in the tail and 100 in the head.
    REQUIRE(CrossedKeyCount(kKeys, 800, 150, kLo, kHi) == 2);
    // Treating the wrap as one interval `(800, 150]` would report zero, which
    // is what "the events stopped after the first cycle" looks like.
    REQUIRE(KeysInHalfOpen(kKeys, 800, 150, kLo, kHi) == 0);
}

TEST_CASE("A key on the window's first millisecond is not swallowed", "[m3events]") {
    // The pool seeds `lastFrame` to windowLo - 1 precisely so this fires. A
    // seed of windowLo would lose any key authored at the very start.
    const std::vector<u32> atZero{0, 400};
    REQUIRE(CrossedKeyCount(atZero, kLo - 1, 10, kLo, kHi) == 1);
    REQUIRE(CrossedKeyCount(atZero, kLo, 10, kLo, kHi) == 0);
}

TEST_CASE("Keys outside the sequence window never fire", "[m3events]") {
    // A shared track with keys belonging to other sequences: the window is what
    // selects them, which is how MDX events work.
    const std::vector<u32> shared{50, 500, 5000};
    REQUIRE(CrossedKeyCount(shared, 0, 1000, kLo, kHi) == 2);
    REQUIRE(CrossedKeyCount(shared, 0, 6000, kLo, kHi) == 2); // 5000 is out of window
    REQUIRE(CrossedKeyCount(shared, 1999, 6000, 2000, 6000) == 1);
}

TEST_CASE("A global-clock track wraps on its own duration", "[m3events]") {
    // Global sequences give the pool a window of [0, dur-1] and a frame of
    // `globalTime % dur`, so the same wrap branch covers them; there is no
    // separate path to get wrong.
    const std::vector<u32> g{0, 250};
    constexpr i32 gLo = 0, gHi = 499;
    REQUIRE(CrossedKeyCount(g, -1, 100, gLo, gHi) == 1);
    // 400 -> 100 wraps but crosses only the key at 0: 250 sits *behind* the
    // cursor at 400 and was consumed last cycle.
    REQUIRE(CrossedKeyCount(g, 400, 100, gLo, gHi) == 1);
    // 400 -> 300 wraps past the end and back over both keys.
    REQUIRE(CrossedKeyCount(g, 400, 300, gLo, gHi) == 2);
}

// ---------------------------------------------------------------------------
// `.m3` decode
// ---------------------------------------------------------------------------

TEST_CASE("Evt_Sound becomes a dispatchable config; the other two do not",
          "[m3events]") {
    // The entire shipped vocabulary, measured over 3607 corpus models: 6873
    // Evt_SeqEnd, 188 Evt_Simulate, 31 Evt_Sound, and nothing else.
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    mb.StaticBone("muzzle", 0);

    m3fix::StcBuilder stc("all", 1, false);
    stc.Event(900, m3fix::Block<m3::Event>(
                       {0, 500, 900},
                       {Ev("Evt_Sound", 1, "Terran_ExplosionLarge"), Ev("Evt_Simulate", 1),
                        Ev("Evt_SeqEnd", 0xFFFF)}));
    mb.Sequence("Death", 0, 1000, {mb.AddStc(stc.Build())});

    M3ModelAdapter a(mb.Build());
    const auto evts = a.GetEventObjects();

    REQUIRE(evts.size() == 1);
    REQUIRE(evts[0].kind == renderer::model::EventObjectConfig::Kind::SND);
    REQUIRE(evts[0].id == "Terran_ExplosionLarge");
    REQUIRE(evts[0].nodeIndex == 1);
    REQUIRE(evts[0].eventTrackTimes == std::vector<u32>{0});
}

TEST_CASE("A config is stamped with the sequence its container drives", "[m3events]") {
    // The reason `sequenceIndex` exists. Both containers key their sound at
    // t=200; without the stamp, playing Walk would fire Attack's cue too,
    // because both tracks start at zero.
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);

    m3fix::StcBuilder walk("walk", 1, false);
    walk.Event(900, m3fix::Block<m3::Event>({200}, {Ev("Evt_Sound", 0, "Footstep")}));
    m3fix::StcBuilder attack("attack", 1, false);
    attack.Event(900, m3fix::Block<m3::Event>({200}, {Ev("Evt_Sound", 0, "Swing")}));

    const u32 w = mb.AddStc(walk.Build());
    const u32 t = mb.AddStc(attack.Build());
    mb.Sequence("Walk", 0, 1000, {w});
    mb.Sequence("Attack", 0, 1000, {t});

    M3ModelAdapter a(mb.Build());
    auto evts = a.GetEventObjects();
    REQUIRE(evts.size() == 2);
    std::sort(evts.begin(), evts.end(),
              [](const auto& x, const auto& y) { return x.id < y.id; });

    REQUIRE(evts[0].id == "Footstep");
    REQUIRE(evts[0].sequenceIndex == 0);
    REQUIRE(evts[1].id == "Swing");
    REQUIRE(evts[1].sequenceIndex == 1);
}

TEST_CASE("Distinct payloads become distinct configs; repeats share one",
          "[m3events]") {
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    m3fix::StcBuilder stc("all", 1, false);
    stc.Event(900, m3fix::Block<m3::Event>({100, 300, 700},
                                           {Ev("Evt_Sound", 0, "Step"),
                                            Ev("Evt_Sound", 0, "Clang"),
                                            Ev("Evt_Sound", 0, "Step")}));
    mb.Sequence("Walk", 0, 1000, {mb.AddStc(stc.Build())});

    M3ModelAdapter a(mb.Build());
    auto evts = a.GetEventObjects();
    REQUIRE(evts.size() == 2);
    std::sort(evts.begin(), evts.end(),
              [](const auto& x, const auto& y) { return x.id < y.id; });

    REQUIRE(evts[0].id == "Clang");
    REQUIRE(evts[0].eventTrackTimes == std::vector<u32>{300});
    // One config, two firing times — not two configs.
    REQUIRE(evts[1].id == "Step");
    REQUIRE(evts[1].eventTrackTimes == std::vector<u32>{100, 700});
}

TEST_CASE("An unrecognised event name is ignored, not dispatched", "[m3events]") {
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    m3fix::StcBuilder stc("all", 1, false);
    stc.Event(900, m3fix::Block<m3::Event>({100}, {Ev("Evt_SomethingNew", 0, "x")}));
    mb.Sequence("Walk", 0, 1000, {mb.AddStc(stc.Build())});

    M3ModelAdapter a(mb.Build());
    REQUIRE(a.GetEventObjects().empty());
}

TEST_CASE("The name comparison survives the trailing NUL", "[m3events]") {
    // `Ref<CHAR>` keeps the terminator, so `name.size()` is one past the text
    // and `name == "Evt_Sound"` is false for a real parsed key. The decoder
    // round-trips through c_str(); this pins that, because the failure mode is
    // silent — the strings print identically and every event just vanishes.
    m3::Event e = Ev("Evt_Sound", 0, "Cue");
    REQUIRE(e.name.size() == std::string("Evt_Sound").size() + 1);
    REQUIRE_FALSE(e.name == "Evt_Sound");

    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    m3fix::StcBuilder stc("all", 1, false);
    stc.Event(900, m3fix::Block<m3::Event>({100}, {e}));
    mb.Sequence("Walk", 0, 1000, {mb.AddStc(stc.Build())});

    M3ModelAdapter a(mb.Build());
    REQUIRE(a.GetEventObjects().size() == 1);
}
