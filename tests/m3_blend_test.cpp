// ============================================================================
// `.m3` layered blending, driven through the real M3ModelAdapter::Evaluate.
//
// Everything here is a behaviour of the StarCraft II runtime that a
// straightforward implementation gets wrong quietly:
//
//   B2  a transparent layer with no track abstains; an opaque one forces the
//       property's default at full weight
//   B3  one contribution per logical play, claimed by the first layer past the
//       B2 filter — including an opaque default-fill (verified at the stamp
//       site in M3Anim_BlendVec3_Weighted)
//   B4  a weight budget spent highest-priority-first, combined
//       lowest-priority-first through a smoothstep
//
// Going through Evaluate rather than a private sampler keeps the test on the
// path the renderer actually calls, and makes the assertions read as poses.
// ============================================================================

#include "io/m3/m3_model_adapter.h"
#include "m3_anim_builders.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace whiteout;
using namespace whiteout::flakes;
using namespace whiteout::flakes::io;
using Catch::Approx;

namespace {

// A bone's world-space translation, which for these fixtures is its sampled
// position composed down the chain.
Vector3f Translation(const renderer::model::FrameState& fs, std::size_t bone) {
    REQUIRE(bone < fs.boneWorldMatrices.size());
    const auto& m = fs.boneWorldMatrices[bone];
    return {m.data[3][0], m.data[3][1], m.data[3][2]};
}

renderer::model::FrameState EvalAt(M3ModelAdapter& a, std::vector<ClipRef> clips) {
    PoseRequest req;
    req.clips = clips;
    return a.Evaluate(req);
}

// Defaults to non-looping so a fixture can be sampled at its own end key.
// With loop set, `elapsed == duration` wraps to 0 — see the wrap cases below.
ClipRef Clip(i32 seq, i32 elapsed, f32 weight = 1.0f, bool loop = false) {
    ClipRef c;
    c.sequence = seq;
    c.timeMs = elapsed;
    c.elapsedMs = elapsed;
    c.weight = weight;
    c.loop = loop;
    return c;
}

} // namespace

TEST_CASE("Bind pose when no clip is playing", "[m3blend]") {
    M3ModelAdapter a(m3fix::SplitBodyFixture());
    const auto fs = EvalAt(a, {});
    REQUIRE(fs.boneWorldMatrices.size() == 2);
    // Every channel falls back to its AnimRef init value.
    REQUIRE(Translation(fs, 0).x == Approx(0.0f));
    REQUIRE(Translation(fs, 1).y == Approx(0.0f));
}

TEST_CASE("Split body: the transparent layer wins where it has a track", "[m3blend]") {
    // The fixture keys bone 1 from both containers:
    //   lower (priority 1, opaque)      -> (0,20,0) at t=1000
    //   upper (priority 2, concurrent)  -> (0,0,30) at t=1000
    // Highest priority first means the upper layer contributes, and B3 stops
    // the lower one from contributing again for the same play.
    M3ModelAdapter a(m3fix::SplitBodyFixture());
    const auto fs = EvalAt(a, {Clip(0, 1000)});

    const Vector3f chest = Translation(fs, 1);
    // Bone 1 is parented to bone 0, so its world translation carries the
    // root's contribution too.
    const Vector3f root = Translation(fs, 0);
    REQUIRE(root.x == Approx(10.0f));   // lower layer drove the root
    REQUIRE(chest.z - root.z == Approx(30.0f)); // upper layer drove the chest
    REQUIRE(chest.y - root.y == Approx(0.0f));  // not the lower layer's (0,20,0)
}

TEST_CASE("Split body: a transparent layer abstains where it has no track", "[m3blend]") {
    // The upper container has no track for the root bone's position, and it is
    // concurrent — so it must abstain and let the lower container through
    // rather than forcing the default.
    M3ModelAdapter a(m3fix::SplitBodyFixture());
    const auto fs = EvalAt(a, {Clip(0, 1000)});
    REQUIRE(Translation(fs, 0).x == Approx(10.0f)); // not 0, which is the default
}

TEST_CASE("An opaque layer with no track forces the property default", "[m3blend]") {
    // Same shape as the split-body fixture, but the high-priority container is
    // NOT concurrent. It has no track for the root, so instead of abstaining it
    // contributes the AnimRef's init value at full weight — snapping the bone
    // back toward bind pose. That asymmetry is the whole feature.
    m3fix::ModelBuilder mb;
    mb.Bone("root", -1, m3fix::Ref<Vector3f>(700, {7, 7, 7}),
            m3fix::ConstRef(Quaternion{0, 0, 0, 1}), m3fix::ConstRef(Vector3f{1, 1, 1}));

    m3fix::StcBuilder low("low", 1, /*concurrent*/ false);
    low.Vec3(700, m3fix::Block<Vector3f>({0, 1000}, {{0, 0, 0}, {99, 0, 0}}));
    m3fix::StcBuilder high("high", 2, /*concurrent*/ false);
    high.Vec3(701, m3fix::Block<Vector3f>({0, 1000}, {{0, 0, 0}, {1, 0, 0}})); // unrelated id

    const u32 lo = mb.AddStc(low.Build());
    const u32 hi = mb.AddStc(high.Build());
    mb.Sequence("Attack", 0, 1000, {lo, hi});

    M3ModelAdapter a(mb.Build());
    const auto fs = EvalAt(a, {Clip(0, 1000)});
    // The init value (7,7,7), not the lower layer's 99.
    REQUIRE(Translation(fs, 0).x == Approx(7.0f));
}

TEST_CASE("One contribution per play, even across many layers", "[m3blend]") {
    // Three containers all driving the same property from one play. Only the
    // highest-priority one may contribute; if the rule were missing the value
    // would be a blend of all three.
    m3fix::ModelBuilder mb;
    mb.Bone("root", -1, m3fix::Ref<Vector3f>(900, {0, 0, 0}),
            m3fix::ConstRef(Quaternion{0, 0, 0, 1}), m3fix::ConstRef(Vector3f{1, 1, 1}));

    std::vector<u32> group;
    const f32 xs[3] = {10.0f, 20.0f, 40.0f};
    for (int i = 0; i < 3; ++i) {
        m3fix::StcBuilder s("s", static_cast<u16>(i + 1), true);
        s.Vec3(900, m3fix::Block<Vector3f>({0, 1000}, {{0, 0, 0}, {xs[i], 0, 0}}));
        group.push_back(mb.AddStc(s.Build()));
    }
    mb.Sequence("Multi", 0, 1000, group);

    M3ModelAdapter a(mb.Build());
    const auto fs = EvalAt(a, {Clip(0, 1000)});
    // Priority 3 is highest -> 40. A mean of the three would be ~23.
    REQUIRE(Translation(fs, 0).x == Approx(40.0f));
}

TEST_CASE("Two plays blend through the smoothstep chain", "[m3blend]") {
    // One container per sequence, so each play contributes exactly once.
    //   play 0 (newest, weight 0.4) -> x = 100
    //   play 1 (weight 0.6)         -> x = 0
    // Budget order is list order; the combine runs lowest-priority-first, so
    // the base is play 1's value and play 0 is mixed in with
    // smoothstep(0.4 / (0.6 + 0.4)) = 0.352.
    m3fix::ModelBuilder mb;
    mb.Bone("root", -1, m3fix::Ref<Vector3f>(900, {0, 0, 0}),
            m3fix::ConstRef(Quaternion{0, 0, 0, 1}), m3fix::ConstRef(Vector3f{1, 1, 1}));

    m3fix::StcBuilder a0("a", 5, true);
    a0.Vec3(900, m3fix::Block<Vector3f>({0, 1000}, {{100, 0, 0}, {100, 0, 0}}));
    m3fix::StcBuilder b0("b", 5, true);
    b0.Vec3(900, m3fix::Block<Vector3f>({0, 1000}, {{0, 0, 0}, {0, 0, 0}}));

    const u32 ia = mb.AddStc(a0.Build());
    const u32 ib = mb.AddStc(b0.Build());
    mb.Sequence("A", 0, 1000, {ia});
    mb.Sequence("B", 0, 1000, {ib});

    M3ModelAdapter adapter(mb.Build());
    const auto fs = EvalAt(adapter, {Clip(0, 500, 0.4f), Clip(1, 500, 0.6f)});

    // base = 0 (play 1), then lerp toward 100 by 0.352.
    REQUIRE(Translation(fs, 0).x == Approx(35.2f).margin(1e-3));
}

TEST_CASE("The weight budget stops the walk", "[m3blend]") {
    // Two full-weight plays: the first exhausts the budget, so the second
    // never contributes and the result is exactly the first play's value.
    m3fix::ModelBuilder mb;
    mb.Bone("root", -1, m3fix::Ref<Vector3f>(900, {0, 0, 0}),
            m3fix::ConstRef(Quaternion{0, 0, 0, 1}), m3fix::ConstRef(Vector3f{1, 1, 1}));

    m3fix::StcBuilder a0("a", 5, true);
    a0.Vec3(900, m3fix::Block<Vector3f>({0, 1000}, {{100, 0, 0}, {100, 0, 0}}));
    m3fix::StcBuilder b0("b", 5, true);
    b0.Vec3(900, m3fix::Block<Vector3f>({0, 1000}, {{0, 0, 0}, {0, 0, 0}}));
    const u32 ia = mb.AddStc(a0.Build());
    const u32 ib = mb.AddStc(b0.Build());
    mb.Sequence("A", 0, 1000, {ia});
    mb.Sequence("B", 0, 1000, {ib});

    M3ModelAdapter adapter(mb.Build());
    const auto fs = EvalAt(adapter, {Clip(0, 500, 1.0f), Clip(1, 500, 1.0f)});
    REQUIRE(Translation(fs, 0).x == Approx(100.0f));
}

TEST_CASE("A track shorter than its sequence wraps on its own duration", "[m3blend]") {
    // The behaviour ClipRef::elapsedMs exists for. The track is 400 ms inside a
    // 1000 ms sequence; at 500 ms elapsed it must have wrapped to 100 ms, which
    // is a quarter of the way up the ramp.
    m3fix::ModelBuilder mb;
    mb.Bone("root", -1, m3fix::Ref<Vector3f>(900, {0, 0, 0}),
            m3fix::ConstRef(Quaternion{0, 0, 0, 1}), m3fix::ConstRef(Vector3f{1, 1, 1}));
    m3fix::StcBuilder s("s", 1, false);
    s.Vec3(900, m3fix::Block<Vector3f>({0, 400}, {{0, 0, 0}, {40, 0, 0}}));
    const u32 i = mb.AddStc(s.Build());
    mb.Sequence("Long", 0, 1000, {i});

    M3ModelAdapter a(mb.Build());
    REQUIRE(Translation(EvalAt(a, {Clip(0, 500, 1.0f, true)}), 0).x == Approx(10.0f));
    REQUIRE(Translation(EvalAt(a, {Clip(0, 900, 1.0f, true)}), 0).x == Approx(10.0f));
    REQUIRE(Translation(EvalAt(a, {Clip(0, 200, 1.0f, true)}), 0).x == Approx(20.0f));

    SECTION("landing exactly on the period wraps to the start") {
        // 400 % 400 == 0. Faithful to `frame %= track->duration`, and worth
        // pinning because it looks like an off-by-one the first time it is
        // seen: the last key of a looping track is never held.
        REQUIRE(Translation(EvalAt(a, {Clip(0, 400, 1.0f, true)}), 0).x == Approx(0.0f));
        REQUIRE(Translation(EvalAt(a, {Clip(0, 800, 1.0f, true)}), 0).x == Approx(0.0f));
    }
    SECTION("the same time on a non-looping clip holds the last key") {
        REQUIRE(Translation(EvalAt(a, {Clip(0, 400)}), 0).x == Approx(40.0f));
        REQUIRE(Translation(EvalAt(a, {Clip(0, 5000)}), 0).x == Approx(40.0f));
    }
}

TEST_CASE("A step track holds its left key", "[m3blend]") {
    m3fix::ModelBuilder mb;
    // Flag bit 4 is what steps a track: `interpolate = !(animRef->flags & 0x10)`
    // in M3Anim_BlendQuat_Weighted / _BlendF32_Weighted, and nothing else feeds
    // that decision.
    mb.Bone("root", -1, m3fix::Ref<Vector3f>(900, {0, 0, 0}, /*interpType*/ 1, /*flags*/ 0x11),
            m3fix::ConstRef(Quaternion{0, 0, 0, 1}), m3fix::ConstRef(Vector3f{1, 1, 1}));
    m3fix::StcBuilder s("s", 1, false);
    s.Vec3(900, m3fix::Block<Vector3f>({0, 1000}, {{0, 0, 0}, {50, 0, 0}}));
    const u32 i = mb.AddStc(s.Build());
    mb.Sequence("Step", 0, 1000, {i});

    M3ModelAdapter a(mb.Build());
    REQUIRE(Translation(EvalAt(a, {Clip(0, 500)}), 0).x == Approx(0.0f));
    REQUIRE(Translation(EvalAt(a, {Clip(0, 1000)}), 0).x == Approx(50.0f));
}

TEST_CASE("interpType 0 still interpolates - it is not an interp type", "[m3blend]") {
    // The `.m3a` case, and the reason Jaina juddered. A model with no sequences
    // of its own ships every bone AnimRef with that u16 zeroed, because the
    // exporter had no track to number; the tracks arrive later from an attached
    // file. At runtime the loader overwrites the field with the property's row
    // in the flattened track table (0xFFFF = unbound), so it never reaches the
    // interpolation decision — only `flags & 0x10` does. Treating a zero there
    // as "step" turned 75.5% of bone SRT refs on `.m3a`-driven models into step
    // tracks, which at 30 fps keys against a 60 fps present reads as a tremor.
    m3fix::ModelBuilder mb;
    mb.Bone("root", -1, m3fix::Ref<Vector3f>(900, {0, 0, 0}, /*interpType*/ 0, /*flags*/ 0),
            m3fix::ConstRef(Quaternion{0, 0, 0, 1}), m3fix::ConstRef(Vector3f{1, 1, 1}));
    m3fix::StcBuilder s("s", 1, false);
    s.Vec3(900, m3fix::Block<Vector3f>({0, 1000}, {{0, 0, 0}, {50, 0, 0}}));
    const u32 i = mb.AddStc(s.Build());
    mb.Sequence("Lerp", 0, 1000, {i});

    M3ModelAdapter a(mb.Build());
    REQUIRE(Translation(EvalAt(a, {Clip(0, 500)}), 0).x == Approx(25.0f));
    REQUIRE(Translation(EvalAt(a, {Clip(0, 250)}), 0).x == Approx(12.5f));
}

TEST_CASE("Bone composition carries the parent transform", "[m3blend]") {
    // Full inheritance, unconditionally — the Inherit* bone flags are
    // authoring metadata the runtime never reads (verified in
    // M3Anim_EvaluateBoneTransform / M3_UpdateNodeTransform).
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1, {5, 0, 0});
    mb.StaticBone("child", 0, {0, 3, 0});
    M3ModelAdapter a(mb.Build());
    const auto fs = EvalAt(a, {});

    REQUIRE(Translation(fs, 0).x == Approx(5.0f));
    const Vector3f c = Translation(fs, 1);
    REQUIRE(c.x == Approx(5.0f));
    REQUIRE(c.y == Approx(3.0f));
}

TEST_CASE("Scale scales the rotation rows, not the translation", "[m3blend]") {
    m3fix::ModelBuilder mb;
    mb.Bone("root", -1, m3fix::ConstRef(Vector3f{1, 2, 3}),
            m3fix::ConstRef(Quaternion{0, 0, 0, 1}), m3fix::ConstRef(Vector3f{2, 4, 8}));
    M3ModelAdapter a(mb.Build());
    const auto fs = EvalAt(a, {});
    const auto& m = fs.boneWorldMatrices[0];

    REQUIRE(m.data[0][0] == Approx(2.0f));
    REQUIRE(m.data[1][1] == Approx(4.0f));
    REQUIRE(m.data[2][2] == Approx(8.0f));
    // The translation row is the location verbatim.
    REQUIRE(m.data[3][0] == Approx(1.0f));
    REQUIRE(m.data[3][1] == Approx(2.0f));
    REQUIRE(m.data[3][2] == Approx(3.0f));
}

TEST_CASE("elapsedMs is the clock, not timeMs", "[m3blend]") {
    // The contract that broke in the renderer, expressed at the seam it broke
    // at. `FrameTicker::EvaluateActorTreeRec` synthesises its own ClipRef from
    // the actor's windowed time; it filled `timeMs` and left `elapsedMs` at its
    // default, so every M3 layer sampled t=0 on every frame. The model still
    // loaded, skinned, drew the right number of draws and traced identically
    // frame to frame — a frozen pose is invisible to every other check the
    // gates make.
    //
    // Nothing about the failure is M3-specific in shape: any sampler reading
    // the unwrapped elapsed rather than the windowed time has the same silent
    // mode available to it.
    m3fix::ModelBuilder mb;
    mb.Bone("mover", -1, m3fix::Ref<Vector3f>(700, {0, 0, 0}),
            m3fix::ConstRef(Quaternion{0, 0, 0, 1}), m3fix::ConstRef(Vector3f{1, 1, 1}));
    m3fix::StcBuilder stc("all", 1, /*concurrent*/ false);
    stc.Vec3(700, m3fix::Block<Vector3f>({0, 1000}, {{0, 0, 0}, {10, 0, 0}}));
    mb.Sequence("Walk", 0, 1000, {mb.AddStc(stc.Build())});
    M3ModelAdapter a(mb.Build());

    // Mid-track, not at the end key: `elapsed == duration` wraps to 0 on a
    // looping clip, which would make both halves of this read 0 for entirely
    // different reasons.
    ClipRef stuck;
    stuck.sequence = 0;
    stuck.timeMs = 500;  // advancing...
    stuck.elapsedMs = 0; // ...but the clock the sampler reads never moved
    REQUIRE(Translation(EvalAt(a, {stuck}), 0).x == Approx(0.0f));

    ClipRef live = stuck;
    live.elapsedMs = 500;
    REQUIRE(Translation(EvalAt(a, {live}), 0).x == Approx(5.0f));
}

TEST_CASE("An out-of-range clip sequence wraps rather than dropping the layer",
          "[m3blend]") {
    // Hosts pass a raw, unbounded index and rely on the wrap — `ClipPlaylist`
    // bounds it on the way into its own clips, but `FrameTicker` passes the
    // host's value through untouched. A dropped layer is bind pose, silently.
    m3fix::ModelBuilder mb;
    mb.Bone("mover", -1, m3fix::Ref<Vector3f>(700, {0, 0, 0}),
            m3fix::ConstRef(Quaternion{0, 0, 0, 1}), m3fix::ConstRef(Vector3f{1, 1, 1}));
    m3fix::StcBuilder stc("all", 1, /*concurrent*/ false);
    stc.Vec3(700, m3fix::Block<Vector3f>({0, 1000}, {{0, 0, 0}, {10, 0, 0}}));
    mb.Sequence("Walk", 0, 1000, {mb.AddStc(stc.Build())});
    M3ModelAdapter a(mb.Build());

    ClipRef c = Clip(/*seq=*/0, /*elapsed=*/1000);
    const f32 inRange = Translation(EvalAt(a, {c}), 0).x;
    REQUIRE(inRange == Approx(10.0f));

    for (i32 raw : {1, -1, 7}) {
        c.sequence = raw;
        INFO("raw sequence " << raw);
        REQUIRE(Translation(EvalAt(a, {c}), 0).x == Approx(inRange));
    }
}

// ---- Sub-track selection ---------------------------------------------------
//
// One StarCraft II sequence is several sub-track containers, and the game
// normally starts them all. Naming one is how a host borrows a single prop or
// limb out of a sequence it does not otherwise want: the Marine's combat
// shield is `Cover_Shield`, one of `Cover`'s two containers, and holding it
// alone is what leaves the shield up while the unit stands and walks.
//
// `SplitBodyFixture` is the same shape: `Attack` spans an opaque `lower`
// (priority 1, drives both bones) and a concurrent `upper` (priority 2, drives
// the chest only). The containers are reported priority-first, so index 0 is
// `upper` and index 1 is `lower`.

TEST_CASE("Sub-tracks are reported priority-first", "[m3blend]") {
    M3ModelAdapter a(m3fix::SplitBodyFixture());
    const auto subs = a.SubtracksOf(0);
    REQUIRE(subs.size() == 2);
    REQUIRE(subs[0].name == "upper");
    REQUIRE(subs[0].priority == 2);
    REQUIRE(subs[0].concurrent);
    REQUIRE(subs[0].trackCount == 1);
    REQUIRE(subs[1].name == "lower");
    REQUIRE(subs[1].priority == 1);
    REQUIRE_FALSE(subs[1].concurrent);
    REQUIRE(subs[1].trackCount == 2);
    REQUIRE(a.SubtracksOf(7).empty());
}

TEST_CASE("A clip naming no sub-track plays every container", "[m3blend]") {
    M3ModelAdapter a(m3fix::SplitBodyFixture());
    const auto fs = EvalAt(a, {Clip(0, 1000)});
    // Root from `lower` (the only container that keys it); chest from `upper`,
    // which outranks `lower` on the one property both drive.
    REQUIRE(Translation(fs, 0).x == Approx(10.0f));
    REQUIRE(Translation(fs, 1).z == Approx(30.0f));
}

TEST_CASE("subtrack names one container and drops the rest", "[m3blend]") {
    M3ModelAdapter a(m3fix::SplitBodyFixture());

    SECTION("the concurrent one leaves everything it does not key alone") {
        ClipRef c = Clip(0, 1000);
        c.subtrack = 0; // upper
        const auto fs = EvalAt(a, {c});
        // `upper` has no track for the root and abstains, so the root falls
        // back to its AnimRef init rather than to `lower`'s 10.
        REQUIRE(Translation(fs, 0).x == Approx(0.0f));
        REQUIRE(Translation(fs, 1).z == Approx(30.0f));
    }

    SECTION("the opaque one drives what it keys and nothing else runs") {
        ClipRef c = Clip(0, 1000);
        c.subtrack = 1; // lower
        const auto fs = EvalAt(a, {c});
        REQUIRE(Translation(fs, 0).x == Approx(10.0f));
        // `upper` is not playing, so the chest takes `lower`'s key for it.
        REQUIRE(Translation(fs, 1).y == Approx(20.0f));
        REQUIRE(Translation(fs, 1).z == Approx(0.0f));
    }

    SECTION("an out-of-range index plays nothing rather than everything") {
        // Deliberate: a stale index (kept across an `.m3a` detach) silently
        // widening back to the whole sequence would look like the layer works.
        ClipRef c = Clip(0, 1000);
        c.subtrack = 5;
        const auto fs = EvalAt(a, {c});
        REQUIRE(Translation(fs, 0).x == Approx(0.0f));
        REQUIRE(Translation(fs, 1).z == Approx(0.0f));
    }
}

TEST_CASE("A sub-track layer leaves the play under it visible", "[m3blend]") {
    // The shield case end to end: a concurrent container held over a full-body
    // play drives its own property and lets the rest through. Two clips, so
    // this also pins that the per-play dedup counts the filtered layer as its
    // own play rather than merging it into the one below.
    M3ModelAdapter a(m3fix::SplitBodyFixture());
    ClipRef overlay = Clip(0, 1000);
    overlay.subtrack = 0; // upper only
    ClipRef body = Clip(0, 1000);
    body.subtrack = 1; // lower only
    const auto fs = EvalAt(a, {overlay, body});
    REQUIRE(Translation(fs, 0).x == Approx(10.0f)); // from `lower`
    REQUIRE(Translation(fs, 1).z == Approx(30.0f)); // from `upper`
}

// ---- Global loops ----------------------------------------------------------

TEST_CASE("Global loops are recognised", "[m3blend]") {
    SECTION("the AlwaysGlobal flag, which is the engine's only rule") {
        m3fix::ModelBuilder mb;
        mb.StaticBone("root", -1);
        m3fix::StcBuilder s("Stand_full", 0, /*runsConcurrent*/ false);
        s.Float(900, m3fix::Block<f32>({0, 100}, {0.0f, 1.0f}));
        const u32 i = mb.AddStc(s.Build());
        mb.Sequence("Stand", 0, 100, {i});
        mb.Sequence("Flagged", 0, 100, {i}, m3::SequenceFlag::AlwaysGlobal);
        M3ModelAdapter a(mb.Build());
        const auto seqs = a.GetSequences();
        REQUIRE(seqs.size() == 2);
        REQUIRE_FALSE(seqs[0].alwaysPlays);
        REQUIRE(seqs[1].alwaysPlays);
        // Neither is an overlay: the container is opaque, so playing either
        // buries what is under it.
        REQUIRE_FALSE(seqs[0].concurrent);
        REQUIRE_FALSE(seqs[1].concurrent);
    }

    SECTION("the GL naming convention, but only when every container abstains") {
        // 89% of the corpus's `GL*` sequences never set the flag — in the game
        // the unit's actor data starts them, and a model viewer has none. The
        // all-concurrent condition is what makes taking the name safe.
        m3fix::ModelBuilder mb;
        mb.StaticBone("root", -1);
        m3fix::StcBuilder conc("GLstand_light", 0, /*runsConcurrent*/ true);
        conc.Float(900, m3fix::Block<f32>({0, 100}, {0.0f, 1.0f}));
        m3fix::StcBuilder opaque("GLstand_full", 0, /*runsConcurrent*/ false);
        opaque.Float(901, m3fix::Block<f32>({0, 100}, {0.0f, 1.0f}));
        const u32 c = mb.AddStc(conc.Build());
        const u32 o = mb.AddStc(opaque.Build());
        mb.Sequence("GLstand", 0, 100, {c});
        mb.Sequence("GLstand A", 0, 100, {c, o});
        mb.Sequence("Stand", 0, 100, {c});
        M3ModelAdapter a(mb.Build());
        const auto seqs = a.GetSequences();
        REQUIRE(seqs[0].alwaysPlays);       // GL-named, all containers abstain
        REQUIRE(seqs[0].concurrent);
        REQUIRE_FALSE(seqs[1].alwaysPlays); // GL-named but one container buries
        REQUIRE_FALSE(seqs[1].concurrent);
        REQUIRE_FALSE(seqs[2].alwaysPlays); // concurrent, but not GL-named
        REQUIRE(seqs[2].concurrent);
    }
}
