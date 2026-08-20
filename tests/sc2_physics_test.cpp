// `PHRB`/`PHYJ` -> Snowball, end to end.
//
// The chain is the same length as the WoW one and fails the same two ways:
// WhiteoutLib parses the physics chunks, `sc2_physics.cpp` maps them onto
// Domino's body/shape/joint model per `DOMINO_GLUE.md` §6, and Snowball
// simulates it. A slip anywhere — a body created kinematic that should be
// dynamic, a transposed joint frame, degrees fed to a field that wants radians
// — produces a rig that either explodes or does nothing, and from the renderer
// those look identical.
//
// What is *not* the same as WoW is where the interesting failure lives, so the
// pure cases below go after M3's own hazards rather than repeating WoW's:
//
//   - An M3 bone matrix carries **scale**, and the frame conversion has to
//     divide it out and put it back. Nothing about a finished frame reveals a
//     conversion that does neither.
//   - `simulationType` says how a body is *created*, not what it is. Reading it
//     as the answer leaves every corpus ragdoll kinematic — which renders as a
//     model that simply plays its animation, the single most plausible-looking
//     wrong result in the whole path.
//
// The corpus case is a `*DeathRagdoll`, which is the content this stage exists
// for: every one of its bodies is authored dynamic, so it has no kinematic
// anchor at all and must collapse under gravity rather than hold its pose.

#include "io/m3/m3_model_adapter.h"
#include "renderer/profiles/sc2_heroes/sc2_physics.h"
#include "whiteout/flakes/pose_stage.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/m3/parser.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace flakes = whiteout::flakes;
namespace sc2 = whiteout::flakes::renderer::profiles::sc2_heroes;

using whiteout::Matrix44f;
using whiteout::Vector3f;
using whiteout::f32;
using whiteout::flakes::io::M3ModelAdapter;
using whiteout::flakes::renderer::animation::PoseStageContext;
using whiteout::flakes::renderer::model::FrameState;
using Catch::Approx;

namespace {

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_SC2_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus");
}

/// Zeratul's death ragdoll: 39 bodies, 38 joints, every body authored dynamic.
/// A model with two of each would pass this file while being wrong.
fs::path RagdollPath() {
    return CorpusRoot() / "HotSM3" / "Storm_Hero_Zeratul_Base_DeathRagdoll.m3";
}

/// Valeera alive: 9 kinematic anchors, 11 dynamic segments, 7 joints — five
/// shoulders and two welds. The *other* shape of this content, and the one
/// where the kinematic drive and the anchoring actually matter.
fs::path HangingPath() {
    return CorpusRoot() / "HotSM3" / "Storm_Hero_Valeera_Base.m3";
}

/// Structures whose debris is *keyed*: every body carries a sampled
/// `dynamicState` whose init value is 0, so the model is inert until a sequence
/// turns it on. 591 of the corpus's 4160 physicalised models are like this, and
/// a stage that reads only the authored constant never creates one of them.
///
/// Two, because they key the same channel over different collision geometry —
/// the Hell core's rubble is boxes and spheres, the refinery's is convex hulls,
/// and the hull path is the one whose shape dimensions are all zero because the
/// extent lives in the point cloud instead.
std::vector<fs::path> KeyedPaths() {
    return {CorpusRoot() / "HotSM3" / "Storm_Building_Hell_Core_Death.m3",
            CorpusRoot() / "Sc2M3" / "SnowRefinery_Terran_BaseRefineryDeath.m3"};
}

std::vector<whiteout::u8> ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in)
        return {};
    return std::vector<whiteout::u8>((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
}

bool Finite(const Matrix44f& m) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(m.data[r][c]))
                return false;
    return true;
}

Vector3f Origin(const Matrix44f& m) {
    return {m.data[3][0], m.data[3][1], m.data[3][2]};
}

f32 Dist(const Vector3f& a, const Vector3f& b) {
    const f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/// A row-vector bone matrix: rotation about +Z, then a per-axis scale, then a
/// translation — composed in the order `M3ComposeLocal` uses.
Matrix44f Bone(f32 yaw, const Vector3f& scale, const Vector3f& pos) {
    const f32 c = std::cos(yaw), s = std::sin(yaw);
    Matrix44f m = Matrix44f::identity();
    m.data[0][0] = c * scale.x;
    m.data[0][1] = s * scale.x;
    m.data[1][0] = -s * scale.y;
    m.data[1][1] = c * scale.y;
    m.data[2][2] = scale.z;
    m.data[3][0] = pos.x;
    m.data[3][1] = pos.y;
    m.data[3][2] = pos.z;
    return m;
}

void CheckRoundTrip(const Matrix44f& in) {
    const Matrix44f out = sc2::Sc2RoundTripBoneFrame(in);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
            CHECK(out.data[r][c] == Approx(in.data[r][c]).margin(1e-4));
}

} // namespace

// ---------------------------------------------------------------------------
// pure — no corpus, no device
// ---------------------------------------------------------------------------

TEST_CASE("the M3 bone-frame conversion is its own inverse", "[m3][phys]") {
    // Identity first: the case where every wrong convention still looks right.
    CheckRoundTrip(Matrix44f::identity());

    // Rotation alone. A transposed extraction returns the conjugate here, which
    // is stable, plausible, and mirrored.
    CheckRoundTrip(Bone(0.7f, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f, 0.0f}));

    // Rotation and translation — the WoW stage's whole domain.
    CheckRoundTrip(Bone(-1.9f, {1.0f, 1.0f, 1.0f}, {3.0f, -2.0f, 11.0f}));

    // **Uniform scale.** A conversion that forgets to divide it out still
    // extracts a valid rotation from a uniformly scaled basis, so this passes
    // for the wrong reason unless the write-back also puts the scale back.
    CheckRoundTrip(Bone(0.4f, {2.5f, 2.5f, 2.5f}, {1.0f, 0.0f, 4.0f}));

    // **Non-uniform scale**, which is the case that separates them: an
    // extraction fed unscaled rows here returns a rotation that is quietly
    // wrong in proportion to how unequal the three axes are.
    CheckRoundTrip(Bone(1.1f, {0.5f, 2.0f, 1.3f}, {-4.0f, 6.0f, 0.5f}));
}

TEST_CASE("a degenerate bone basis does not produce NaN", "[m3][phys]") {
    // A collapsed axis is not a rotation, and the conversion has to answer
    // something rather than divide by zero — one NaN here reaches the solver
    // and takes the whole rig with it.
    const Matrix44f flat = Bone(0.3f, {1.0f, 1.0f, 0.0f}, {2.0f, 2.0f, 2.0f});
    const Matrix44f out = sc2::Sc2RoundTripBoneFrame(flat);
    REQUIRE(Finite(out));
    // The translation survives whatever the basis does.
    CHECK(Origin(out).x == Approx(2.0f));
    CHECK(Origin(out).z == Approx(2.0f));
}

// ---------------------------------------------------------------------------
// corpus
// ---------------------------------------------------------------------------

TEST_CASE("a StarCraft II death ragdoll collapses under gravity", "[m3][phys][corpus]") {
    const fs::path path = RagdollPath();
    if (!fs::exists(path))
        SKIP("no corpus model at " + path.string());

    const std::vector<whiteout::u8> bytes = ReadAll(path);
    REQUIRE_FALSE(bytes.empty());
    whiteout::m3::Model model;
    {
        whiteout::m3::Parser parser;
        model = parser.parse(bytes);
    }
    REQUIRE_FALSE(model.bones.empty());

    // The premise. If this model ever loses its physics chunks the rest would
    // pass vacuously, so both are hard requirements rather than skips.
    REQUIRE_FALSE(model.rigidBodies.empty());
    REQUIRE_FALSE(model.physicsJoints.empty());

    // Every body authored dynamic is what makes this a *ragdoll* rather than a
    // rig with hanging parts: there is no kinematic anchor holding it up.
    std::size_t dynamic = 0;
    for (const auto& rb : model.rigidBodies) {
        if (rb.dynamicState.initValue != 0)
            ++dynamic;
    }
    CHECK(dynamic == model.rigidBodies.size());

    // Only four joint types exist; a fifth would mean the mapping has a hole.
    for (const auto& pj : model.physicsJoints)
        CHECK(pj.jointType <= 3u);

    // The adapter takes a copy so `model` outlives the stage below, which holds
    // a reference to it.
    const M3ModelAdapter adapter(model);
    flakes::renderer::animation::PoseStageList stages;
    adapter.CreatePoseStages(stages);
    REQUIRE_FALSE(stages.empty());

    // Built directly rather than taken off the list. `CreatePoseStages` appends
    // the *cloth* stage after the bodies (`sc2_cloth.h`), and half this corpus
    // is Heroes content carrying both, so `stages.back()` stopped meaning
    // "physics" — silently, because a cloth stage runs happily on a model whose
    // bodies were never stepped.
    auto stage = sc2::CreateSc2PhysicsStage(model);
    REQUIRE(stage);
    // A ragdoll is fully described by its model: no ground query, no aim target.
    REQUIRE_FALSE(stage->NeedsHostInputs());

    flakes::PoseRequest req;
    const FrameState bind = adapter.Evaluate(req);
    REQUIRE_FALSE(bind.boneWorldMatrices.empty());

    const auto skeleton = const_cast<M3ModelAdapter&>(adapter).GetSkeleton();
    PoseStageContext ctx;
    ctx.nodeParents = skeleton.nodeParents;
    ctx.frameDtMs = 16;

    // The first Run builds the scene from the pose and seeds it, and must not
    // move anything: a rig that jumps on frame one is seeding from the wrong
    // space, which no later frame can distinguish from a solver fault.
    FrameState fs = bind;
    stage->Run(fs, ctx);
    for (std::size_t i = 0; i < fs.boneWorldMatrices.size(); ++i)
        CHECK(Dist(Origin(fs.boneWorldMatrices[i]), Origin(bind.boneWorldMatrices[i])) < 1e-5f);

    for (int frame = 0; frame < 240; ++frame)
        stage->Run(fs, ctx);

    // Three questions, and they are the three that separate a working rig from
    // the two failures that look alike: did it move, did it stay finite, and is
    // it still in the neighbourhood of the model it belongs to.
    f32 worst = 0.0f;
    f32 farthest = 0.0f;
    f32 drop = 0.0f;
    for (std::size_t i = 0; i < fs.boneWorldMatrices.size(); ++i) {
        REQUIRE(Finite(fs.boneWorldMatrices[i]));
        const Vector3f now = Origin(fs.boneWorldMatrices[i]);
        const Vector3f was = Origin(bind.boneWorldMatrices[i]);
        worst = std::max(worst, Dist(now, was));
        drop = std::min(drop, now.z - was.z);
        farthest = std::max(farthest, Dist(now, {0.0f, 0.0f, 0.0f}));
    }
    // Four seconds of an 8.8 m/s^2 fall onto the plane at z=0, from a standing
    // hero about two units tall. **The drop is the assertion that matters**:
    // displacement alone is also what a rig that merely jitters produces, and a
    // rig left kinematic tracks its animation and produces neither.
    CHECK(worst > 1.0f);
    CHECK(drop < -1.0f);
    // Nothing may have left, and nothing may have sunk through the floor —
    // a body whose fixtures came out massless does exactly that while every
    // joint around it holds perfectly.
    CHECK(farthest < 100.0f);
    CHECK(drop > -20.0f);

    // The claims are the dynamic bones, and they are what the host echoes back
    // as overrides. An empty list from a model that moved means the write-back
    // and the claim list disagree about which bones physics owns.
    CHECK_FALSE(stage->Claims().empty());
}

TEST_CASE("a live model's chains hang off their kinematic anchors", "[m3][phys][corpus]") {
    // The complement of the ragdoll, and the case that actually exercises the
    // drive: here most bodies are **kinematic**, the animation moves them, and
    // the dynamic ones hang off them through joints. The ragdoll above cannot
    // catch a broken `DriveKinematic` because it has no kinematic body at all.
    const fs::path path = HangingPath();
    if (!fs::exists(path))
        SKIP("no corpus model at " + path.string());

    const std::vector<whiteout::u8> bytes = ReadAll(path);
    REQUIRE_FALSE(bytes.empty());
    whiteout::m3::Model model;
    {
        whiteout::m3::Parser parser;
        model = parser.parse(bytes);
    }
    REQUIRE_FALSE(model.rigidBodies.empty());
    REQUIRE_FALSE(model.physicsJoints.empty());

    std::size_t dynamic = 0, kinematic = 0;
    for (const auto& rb : model.rigidBodies)
        (rb.dynamicState.initValue != 0 ? dynamic : kinematic) += 1;
    // Both halves must exist: all-dynamic is the ragdoll case, all-kinematic
    // means nothing ever simulates.
    REQUIRE(dynamic > 0);
    REQUIRE(kinematic > 0);

    // Copy, not move: the stage below holds a reference to `model`.
    const M3ModelAdapter adapter(model);
    auto stage = sc2::CreateSc2PhysicsStage(model);
    REQUIRE(stage);

    const auto sequences = adapter.GetSequences();
    REQUIRE_FALSE(sequences.empty());

    const auto skeleton = const_cast<M3ModelAdapter&>(adapter).GetSkeleton();
    PoseStageContext ctx;
    ctx.nodeParents = skeleton.nodeParents;
    ctx.frameDtMs = 16;

    whiteout::flakes::ClipRef clip;
    clip.sequence = 0;
    clip.loop = true;

    FrameState bind;
    FrameState fs;
    f32 worst = 0.0f;
    for (int frame = 0; frame < 300; ++frame) {
        clip.timeMs = 16 * frame;
        clip.elapsedMs = clip.timeMs;
        // Re-evaluated every frame, exactly as the frame ticker does it: the
        // animation writes every bone and the stage overwrites the ones it
        // owns, so a drive that reads the wrong space diverges instead of
        // tracking.
        fs = adapter.Evaluate(flakes::PoseRequest::OneClip(clip));
        if (frame == 0)
            bind = fs;
        stage->Run(fs, ctx);
        for (const auto& m : fs.boneWorldMatrices)
            REQUIRE(Finite(m));
    }

    // **Sag, not collapse.** Measured against the *animated* pose of the same
    // frame rather than against bind, because the animation itself has moved on
    // by now: what physics owes here is a small departure from where the
    // sampler alone would have put each bone.
    const FrameState animated = adapter.Evaluate(flakes::PoseRequest::OneClip(clip));
    for (std::size_t i = 0; i < fs.boneWorldMatrices.size(); ++i)
        worst = std::max(
            worst, Dist(Origin(fs.boneWorldMatrices[i]), Origin(animated.boneWorldMatrices[i])));

    // Both bounds matter, and they fail for opposite reasons. Too large means a
    // chain came off its anchor and fell — which is what a body wrongly read as
    // dynamic does, since it is jointed to nothing and simply drops. Too small
    // means physics did nothing at all and the stage is an expensive copy.
    CHECK(worst > 0.01f);
    CHECK(worst < 2.0f);
    CHECK_FALSE(stage->Claims().empty());
    (void)bind;
}

TEST_CASE("a keyed dynamicState turns an animated body into a simulated one",
          "[m3][phys][corpus]") {
    // The third shape of this content, and the one that needs the sampler: a
    // body whose type is *animated*. Neither case above reaches it — both are
    // decided by the authored constant and never change — and the failure it
    // guards is silent, because a model that never simulates renders exactly
    // like one that is not supposed to.
    std::size_t ran = 0;
    for (const fs::path& path : KeyedPaths()) {
    if (!fs::exists(path))
        continue;
    ++ran;
    INFO("model " << path.filename().string());

    const std::vector<whiteout::u8> bytes = ReadAll(path);
    REQUIRE_FALSE(bytes.empty());
    whiteout::m3::Model model;
    {
        whiteout::m3::Parser parser;
        model = parser.parse(bytes);
    }
    REQUIRE_FALSE(model.rigidBodies.empty());

    // The premise, and the thing that makes this model the interesting one:
    // every body asks to be sampled and every one of them defaults to *off*.
    // The gate is the AnimRef's flag bit 1 — `animId` is non-zero on all of
    // them and says nothing about whether they are keyed.
    for (const auto& rb : model.rigidBodies) {
        REQUIRE((rb.dynamicState.flags & 0x2u) != 0u);
        REQUIRE(rb.dynamicState.initValue == 0u);
    }
    const std::size_t bodyCount = model.rigidBodies.size();

    const M3ModelAdapter adapter(model);
    // Read as a constant this model is inert, and the stage would be dropped as
    // useless before any of the rest could run.
    auto stage = sc2::CreateSc2PhysicsStage(model);
    REQUIRE(stage);

    const auto skeleton = const_cast<M3ModelAdapter&>(adapter).GetSkeleton();
    PoseStageContext ctx;
    ctx.nodeParents = skeleton.nodeParents;
    ctx.frameDtMs = 16;

    // Off first. With no clip playing every channel falls back to its init
    // value, so nothing may claim a bone and nothing may move.
    FrameState idle = adapter.Evaluate(flakes::PoseRequest());
    REQUIRE(idle.physicsBodyDynamic.size() == bodyCount);
    for (whiteout::u8 v : idle.physicsBodyDynamic)
        CHECK(v == 0);
    const FrameState atRest = idle;
    for (int frame = 0; frame < 120; ++frame)
        stage->Run(idle, ctx);
    CHECK(stage->Claims().empty());
    for (std::size_t i = 0; i < idle.boneWorldMatrices.size(); ++i)
        CHECK(Dist(Origin(idle.boneWorldMatrices[i]), Origin(atRest.boneWorldMatrices[i])) <
              1e-4f);

    // ...and on. Which sequence and when is the file's business, so it is
    // searched for rather than hard-coded: what is being asserted is that the
    // channel is reachable at all, not where its keys happen to sit.
    const auto sequences = adapter.GetSequences();
    REQUIRE_FALSE(sequences.empty());
    flakes::ClipRef live;
    bool found = false;
    for (std::size_t s = 0; s < sequences.size() && !found; ++s) {
        const whiteout::i32 span = std::max(1, sequences[s].endMs - sequences[s].startMs);
        for (whiteout::i32 t = 0; t <= span && !found; t += 33) {
            flakes::ClipRef probe;
            probe.sequence = static_cast<whiteout::i32>(s);
            probe.timeMs = t;
            probe.elapsedMs = t;
            const FrameState fsProbe = adapter.Evaluate(flakes::PoseRequest::OneClip(probe));
            for (whiteout::u8 v : fsProbe.physicsBodyDynamic) {
                if (v != 0) {
                    live = probe;
                    found = true;
                    break;
                }
            }
        }
    }
    INFO("keyed on at sequence " << live.sequence << " t=" << live.timeMs << "ms");
    REQUIRE(found);

    // Held there — the keys are a step channel, so parking on a frame past the
    // switch keeps every body dynamic — and the debris must now fall. This
    // model has no joints at all: its bodies are loose rubble, and rubble that
    // stays put is a body whose type never changed.
    FrameState fs;
    for (int frame = 0; frame < 240; ++frame) {
        fs = adapter.Evaluate(flakes::PoseRequest::OneClip(live));
        stage->Run(fs, ctx);
        for (const auto& m : fs.boneWorldMatrices)
            REQUIRE(Finite(m));
    }
    CHECK_FALSE(stage->Claims().empty());

    const FrameState animated = adapter.Evaluate(flakes::PoseRequest::OneClip(live));
    f32 worst = 0.0f;
    for (std::size_t i = 0; i < fs.boneWorldMatrices.size(); ++i)
        worst = std::max(
            worst, Dist(Origin(fs.boneWorldMatrices[i]), Origin(animated.boneWorldMatrices[i])));
    CHECK(worst > 0.05f);
    }
    if (ran == 0)
        SKIP("no keyed corpus model under " + CorpusRoot().string());
}

TEST_CASE("the physics bodies draw where the solver put them", "[m3][phys][corpus]") {
    // The Collisions overlay is the only view that separates "the bodies are in
    // the wrong place" from "the bodies are right and the skinning is wrong",
    // and it can only do that if it is drawn from the *simulated* pose. The
    // source fills these before the stage runs, from the animated one — so a
    // shape list that never gets overwritten trails the rig it is meant to
    // explain by exactly the amount under investigation.
    const fs::path path = RagdollPath();
    if (!fs::exists(path))
        SKIP("no corpus model at " + path.string());

    const std::vector<whiteout::u8> bytes = ReadAll(path);
    REQUIRE_FALSE(bytes.empty());
    whiteout::m3::Model model;
    {
        whiteout::m3::Parser parser;
        model = parser.parse(bytes);
    }
    M3ModelAdapter adapter(model);

    const auto shapes = adapter.GetCollisionShapes();
    REQUIRE_FALSE(shapes.empty());
    // A death ragdoll is dynamic throughout, which is what its overlay colour
    // has to say — a rig drawn entirely in the kinematic colour is the picture
    // of the bug this whole stage exists to avoid.
    for (const auto& s : shapes) {
        CHECK(s.bodyKind ==
              static_cast<whiteout::i32>(
                  whiteout::flakes::renderer::model::CollisionBodyKind::Dynamic));
    }

    auto stage = sc2::CreateSc2PhysicsStage(model);
    REQUIRE(stage);

    const auto skeleton = adapter.GetSkeleton();
    PoseStageContext ctx;
    ctx.nodeParents = skeleton.nodeParents;
    ctx.frameDtMs = 16;

    FrameState fs = adapter.Evaluate(flakes::PoseRequest());
    // Filled by the source, so a model with no stage still draws its proxies.
    REQUIRE(fs.collisionTransforms.size() == shapes.size());
    const std::vector<Matrix44f> before = fs.collisionTransforms;

    for (int frame = 0; frame < 240; ++frame)
        stage->Run(fs, ctx);

    REQUIRE(fs.collisionTransforms.size() == shapes.size());
    f32 moved = 0.0f;
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        REQUIRE(Finite(fs.collisionTransforms[i]));
        moved = std::max(moved, Dist(Origin(fs.collisionTransforms[i]), Origin(before[i])));
    }
    // The rig fell; the shapes have to have fallen with it. Left to the source
    // alone this is zero, because nothing re-evaluates between frames here.
    CHECK(moved > 1.0f);
}

TEST_CASE("physics chunks across the corpus build and step without incident",
          "[m3][phys][corpus]") {
    // The scene is built on the first `Run`, not in the constructor, so
    // `m3_solver_test`'s sweep over `CreatePoseStages` reaches none of the
    // shape, joint or body construction here. This is the sweep that does.
    //
    // It asserts almost nothing about the *result* — the two cases above own
    // that — and everything about not falling over: real chunks carry bone
    // indices past the end, shape arrays too short for their counts, joints
    // naming bones with no body, and hulls with three vertices.
    const fs::path root = CorpusRoot();
    std::size_t built = 0, withJoints = 0, checked = 0;
    std::error_code ec;

    for (const char* corpus : {"HotSM3", "Sc2M3"}) {
        const fs::path dir = root / corpus;
        if (!fs::is_directory(dir, ec))
            continue;
        for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec || built >= 60)
                break;
            if (!it->is_regular_file(ec) || it->path().extension() != ".m3")
                continue;

            const std::vector<whiteout::u8> bytes = ReadAll(it->path());
            if (bytes.empty())
                continue;
            whiteout::m3::Model model;
            try {
                whiteout::m3::Parser parser;
                model = parser.parse(bytes);
            } catch (...) {
                continue;
            }
            if (model.rigidBodies.empty())
                continue;
            const bool jointed = !model.physicsJoints.empty();

            const M3ModelAdapter adapter(std::move(model));
            flakes::renderer::animation::PoseStageList stages;
            adapter.CreatePoseStages(stages);
            if (stages.empty())
                continue;

            const auto skeleton = const_cast<M3ModelAdapter&>(adapter).GetSkeleton();
            PoseStageContext ctx;
            ctx.nodeParents = skeleton.nodeParents;
            ctx.frameDtMs = 16;

            flakes::PoseRequest req;
            FrameState fs = adapter.Evaluate(req);
            if (fs.boneWorldMatrices.empty())
                continue;

            ++built;
            withJoints += jointed ? 1 : 0;
            // Enough frames for the accumulator to fill and every body to be
            // integrated, contacts and all.
            for (int frame = 0; frame < 30; ++frame)
                for (auto& stage : stages)
                    stage->Run(fs, ctx);

            for (const auto& m : fs.boneWorldMatrices) {
                REQUIRE(Finite(m));
                ++checked;
            }
        }
    }

    if (built == 0) {
        WARN("no .m3 with physics under " << root.string() << " — skipping");
        return;
    }
    INFO("stepped " << built << " models (" << withJoints << " jointed), " << checked
                    << " bone matrices checked");
    CHECK(built > 0);
}
