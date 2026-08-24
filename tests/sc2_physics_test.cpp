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
#include "m3_anim_builders.h"
#include "renderer/profiles/sc2_heroes/sc2_physics.h"
#include "whiteout/flakes/pose_stage.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/m3/parser.h>

#include <algorithm>
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
using whiteout::Quaternion;
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

TEST_CASE("a convex hull's overlay geometry is the hull, at the hull's size",
          "[m3][phys][corpus]") {
    // The two arrays a `PHSH` hull ships are named backwards by the parser (see
    // `HullPoints`), and reading them as named is invisible in every way except
    // the one that matters: the fixtures still build, the rig still steps, and
    // every hull is silently a ~2-unit blob of unit normals. On the overlay that
    // is a cube the size of the whole model around each limb; in the solver it
    // is six colliders that all overlap everything.
    //
    // Guarded here by the two properties that separate the arrays — the point
    // cloud is not unit-length and it closes Euler against the `DMSE` table.
    const fs::path path = CorpusRoot() / "Sc2M3" / "Reaver_Ragdoll_Death_00.m3";
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
    REQUIRE(shapes.size() == 6);

    for (const auto& sh : shapes) {
        REQUIRE(sh.type == static_cast<whiteout::i32>(
                               whiteout::flakes::renderer::model::CollisionShapeType::Hull));
        REQUIRE(sh.hullPoints.size() >= 4);
        REQUIRE_FALSE(sh.hullEdges.empty());
        REQUIRE(sh.hullEdges.size() % 2 == 0);

        Vector3f lo = sh.hullPoints[0], hi = sh.hullPoints[0];
        for (const Vector3f& v : sh.hullPoints) {
            lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
            hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
        }
        // A body part of a Reaver, not a bounding box of the Reaver. The plane
        // table read as points spans very nearly 2 on every axis, because its
        // entries are unit vectors; no real collider on this model reaches 1.
        CHECK(Dist(lo, hi) > 0.05f);
        CHECK(hi.x - lo.x < 1.0f);
        CHECK(hi.y - lo.y < 1.0f);
        CHECK(hi.z - lo.z < 1.0f);

        for (whiteout::u16 idx : sh.hullEdges)
            REQUIRE(idx < sh.hullPoints.size());
    }

    // V - E + F = 2, against the chunk's own three tables. This is what settles
    // which array is which: the count that pairs with the `DMSE` edges to close
    // Euler is the one the wireframe has to be built from, and reading the
    // arrays as named leaves the half-edge table indexing a vertex past the end.
    for (std::size_t i = 0; i < shapes.size(); ++i) {
        const auto& ps = model.rigidBodies[i].rigidBodyShape[0];
        const std::size_t V = shapes[i].hullPoints.size();
        const std::size_t E = shapes[i].hullEdges.size() / 2;
        const std::size_t F = ps.hullVertexPositions.size();
        CHECK(V + F == E + 2);
        CHECK(E == ps.hullHalfEdges.size() / 2);
    }
}

TEST_CASE("the overlay's body kind follows the solver, not the file", "[m3][phys][corpus]") {
    // `bodyKind` is what the file says at rest and the shape list is built once
    // per template, so the colour a body is drawn in can only track "which of
    // these is the solver allowed to move" if the *current* type is published
    // per frame. It is: the stage resolves the sampled channel — inherit chains
    // and all — back into `physicsBodyDynamic`, and every shape names the body
    // it belongs to.
    //
    // The keyed models are the case that matters. Their bodies are authored off
    // and a Death sequence turns them on, which is exactly the transition an
    // overlay reading the authored constant can never show.
    int ran = 0;
    for (const fs::path& path : KeyedPaths()) {
        if (!fs::exists(path))
            continue;
        ++ran;
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
        for (const auto& sh : shapes) {
            REQUIRE(sh.bodyIndex >= 0);
            REQUIRE(static_cast<std::size_t>(sh.bodyIndex) < model.rigidBodies.size());
        }

        auto stage = sc2::CreateSc2PhysicsStage(model);
        REQUIRE(stage);
        const auto skeleton = adapter.GetSkeleton();
        PoseStageContext ctx;
        ctx.nodeParents = skeleton.nodeParents;
        ctx.frameDtMs = 16;

        // At rest: nothing is dynamic yet, and the published state has to say so
        // rather than repeating the channel it was handed.
        FrameState fs = adapter.Evaluate(flakes::PoseRequest());
        stage->Run(fs, ctx); // builds
        stage->Run(fs, ctx);
        REQUIRE(fs.physicsBodyDynamic.size() == model.rigidBodies.size());
        std::size_t liveAtRest = 0;
        for (auto d : fs.physicsBodyDynamic)
            liveAtRest += d ? 1 : 0;
        CHECK(liveAtRest == 0);

        // Held past the switch, the same way `a keyed dynamicState turns bodies
        // dynamic mid-sequence` finds it.
        bool sawDynamic = false;
        const auto seqs = adapter.GetSequences();
        REQUIRE_FALSE(seqs.empty());
        for (std::size_t seq = 0; seq < seqs.size() && !sawDynamic; ++seq) {
            const whiteout::i32 span = std::max(1, seqs[seq].endMs - seqs[seq].startMs);
            for (whiteout::i32 t = 0; t <= span && !sawDynamic; t += 33) {
                flakes::ClipRef live;
                live.sequence = static_cast<whiteout::i32>(seq);
                live.timeMs = t;
                live.elapsedMs = t;
                fs = adapter.Evaluate(flakes::PoseRequest::OneClip(live));
                stage->Run(fs, ctx);
                for (auto d : fs.physicsBodyDynamic)
                    if (d != 0) {
                        sawDynamic = true;
                        break;
                    }
            }
        }
        INFO(path.filename().string());
        CHECK(sawDynamic);
    }
    if (ran == 0)
        SKIP("no keyed corpus model under " + CorpusRoot().string());
}

TEST_CASE("a hull's overlay placement is the fixture's, in the fixture's order", "[m3][phys]") {
    // `MakePolytope` takes points that are **already** in the shape frame and
    // scales the lot about the origin, so the shape matrix's row scale lands on
    // its translation too. Placing the wireframe the other way round — scale the
    // points, then rotate and translate — draws a hull the right size in the
    // wrong place, by exactly the shape offset times one minus the scale. That
    // is invisible on the identity shape matrices most bodies ship and wrong on
    // every collider that is actually offset from its bone.
    const f32 yaw = 0.6f;
    const f32 c = std::cos(yaw), sn = std::sin(yaw);
    const Vector3f rowScale{2.0f, 0.5f, 1.5f};
    const Vector3f offset{3.0f, -1.0f, 4.0f};

    whiteout::m3::PhysicsShape ps;
    ps.shapeType = whiteout::m3::PhysicsShapeType::ConvexHull;
    ps.transform = Matrix44f::identity();
    ps.transform.data[0][0] = c * rowScale.x;
    ps.transform.data[0][1] = sn * rowScale.x;
    ps.transform.data[1][0] = -sn * rowScale.y;
    ps.transform.data[1][1] = c * rowScale.y;
    ps.transform.data[2][2] = rowScale.z;
    ps.transform.data[3][0] = offset.x;
    ps.transform.data[3][1] = offset.y;
    ps.transform.data[3][2] = offset.z;
    // The point cloud lives in the array the parser calls `hullFaceNormals`.
    ps.hullFaceNormals = {
        {0.1f, 0.0f, 0.0f}, {0.0f, 0.2f, 0.0f}, {0.0f, 0.0f, 0.3f}, {-0.1f, -0.2f, -0.3f}};

    whiteout::m3::RigidBody rb;
    rb.parentBoneIndex = 0;
    rb.simulationType = 1;
    rb.dynamicState = whiteout::m3::AnimRef<whiteout::u32>{};
    rb.dynamicState.initValue = 1;
    rb.rigidBodyShape.push_back(ps);

    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    whiteout::m3::Model model = mb.Build();
    model.rigidBodies.push_back(rb);

    const sc2::Sc2CollisionShapes built = sc2::Sc2BuildCollisionShapes(model);
    REQUIRE(built.shapes.size() == 1);
    const auto& sh = built.shapes[0];
    CHECK(sh.type ==
          static_cast<whiteout::i32>(whiteout::flakes::renderer::model::CollisionShapeType::Hull));
    CHECK(sh.bodyIndex == 0);
    CHECK(sh.bodyKind == static_cast<whiteout::i32>(
                             whiteout::flakes::renderer::model::CollisionBodyKind::Dynamic));
    REQUIRE(sh.hullPoints.size() == ps.hullFaceNormals.size());

    // An identity bone, so the whole placement is the shape frame and nothing
    // else can absorb an ordering slip.
    std::vector<Matrix44f> boneWorld{Matrix44f::identity()};
    std::vector<Matrix44f> placed;
    sc2::Sc2PlaceCollisionShapes(built.bones, built.locals, built.anisotropic, boneWorld, placed);
    REQUIRE(placed.size() == 1);

    const f32 uniform = std::max({rowScale.x, rowScale.y, rowScale.z});
    for (std::size_t i = 0; i < sh.hullPoints.size(); ++i) {
        const Vector3f& p = sh.hullPoints[i];
        // (basis * p + t) * uniform — MakePolytope's own expression.
        const Vector3f want{(p.x * c - p.y * sn + offset.x) * uniform,
                            (p.x * sn + p.y * c + offset.y) * uniform, (p.z + offset.z) * uniform};
        const Vector3f got = whiteout::transform_point(p, placed[0]);
        CHECK(got.x == Approx(want.x).margin(1e-4));
        CHECK(got.y == Approx(want.y).margin(1e-4));
        CHECK(got.z == Approx(want.z).margin(1e-4));
    }
}

TEST_CASE("a rig's placed colliders sit inside the model they wrap", "[m3][phys][corpus]") {
    // The end-to-end shape of the overlay bug, and the one check that needs no
    // knowledge of which array is which: a collider lives *inside* the limb it
    // drives, so every wireframe point, once placed by the shape list's own
    // transforms, has to land inside the model's own vertex bounds. A hull
    // built from the plane table instead spans very nearly 2 on every axis
    // whatever the limb's size, which on a model barely more than a unit tall
    // puts most of the wireframe outside the mesh entirely.
    //
    // The tolerance is a hair rather than zero because a collider is allowed to
    // graze the silhouette; it is not allowed to be twice the model.
    for (const fs::path& path :
         {CorpusRoot() / "Sc2M3" / "Reaver_Ragdoll_Death_00.m3", RagdollPath()}) {
        if (!fs::exists(path))
            continue;
        const std::vector<whiteout::u8> bytes = ReadAll(path);
        REQUIRE_FALSE(bytes.empty());
        whiteout::m3::Model model;
        {
            whiteout::m3::Parser parser;
            model = parser.parse(bytes);
        }
        M3ModelAdapter adapter(model);

        Vector3f lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (const auto& mesh : adapter.GetMeshes())
            for (const Vector3f& v : mesh.positions) {
                lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
                hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
            }
        REQUIRE(hi.z > lo.z);

        const auto shapes = adapter.GetCollisionShapes();
        REQUIRE_FALSE(shapes.empty());
        const FrameState fs = adapter.Evaluate(flakes::PoseRequest());
        REQUIRE(fs.collisionTransforms.size() == shapes.size());

        const f32 tol = 0.05f * Dist(lo, hi);
        f32 worst = 0.0f;
        for (std::size_t i = 0; i < shapes.size(); ++i)
            for (const Vector3f& p : shapes[i].hullPoints) {
                const Vector3f w = whiteout::transform_point(p, fs.collisionTransforms[i]);
                worst = std::max({worst, lo.x - w.x, w.x - hi.x, lo.y - w.y, w.y - hi.y, lo.z - w.z,
                                  w.z - hi.z});
            }
        INFO(path.filename().string() << ": worst point " << worst << " outside the mesh box");
        CHECK(worst < tol);
    }
}





TEST_CASE("a scaled bone carries its shape offsets out with it", "[m3][phys]") {
    // **The body scale multiplies the shape frame's translation, not only its
    // dimensions** (`DOMINO_GLUE.md` §6.2, kinds 0-3). `MakeBox` gets this for
    // free — it scales corners that already carry the offset — so the box path
    // was right and the three kinds that place themselves by hand were not:
    // their collider stayed at the *unscaled* offset while the overlay, the
    // skinned mesh and the box beside them all moved. 676 of the corpus's 5587
    // sphere/capsule/cylinder shapes sit on a scaled bone, up to 1.9 units out.
    //
    // Read off a settle height rather than off the fixture, because the fixture
    // is what is under test: a sphere hung `h` below its bone on a bone scaled
    // `s` comes to rest with the bone at `radius + s*h`, and at `radius + h` if
    // the offset never got scaled. The two are far apart by construction.
    const f32 kBoneScale = 3.0f;
    const f32 kDrop = 0.5f;   // shape offset below the bone, in bone-local units
    const f32 kRadius = 0.25f; // shapeDimensions.x

    auto settle = [&](whiteout::m3::PhysicsShapeType type) {
        whiteout::m3::PhysicsShape ps;
        ps.shapeType = type;
        ps.transform = Matrix44f::identity();
        ps.transform.data[3][2] = -kDrop;
        ps.shapeDimensions = {kRadius, 0.0f, 0.0f}; // a capsule of zero length is a sphere

        whiteout::m3::RigidBody rb;
        rb.parentBoneIndex = 0;
        rb.simulationType = 1;
        rb.density = 1.0f;
        rb.friction = 0.5f;
        rb.restitution = 0.0f;
        rb.dynamicState = whiteout::m3::AnimRef<whiteout::u32>{};
        rb.dynamicState.initValue = 1;
        rb.rigidBodyShape.push_back(ps);

        m3fix::ModelBuilder mb;
        mb.Bone("root", -1, m3fix::ConstRef(Vector3f{0.0f, 0.0f, 5.0f}),
                m3fix::ConstRef(Quaternion{0, 0, 0, 1}),
                m3fix::ConstRef(Vector3f{kBoneScale, kBoneScale, kBoneScale}),
                whiteout::m3::BoneFlag::None);
        whiteout::m3::Model model = mb.Build();
        model.rigidBodies.push_back(rb);

        M3ModelAdapter adapter(model);
        auto stage = sc2::CreateSc2PhysicsStage(model);
        REQUIRE(stage);
        const auto skeleton = adapter.GetSkeleton();
        PoseStageContext ctx;
        ctx.nodeParents = skeleton.nodeParents;
        ctx.frameDtMs = 16;

        FrameState fs;
        for (int frame = 0; frame < 900; ++frame) {
            fs = adapter.Evaluate(flakes::PoseRequest());
            stage->Run(fs, ctx);
        }
        REQUIRE_FALSE(fs.boneWorldMatrices.empty());
        REQUIRE(Finite(fs.boneWorldMatrices[0]));
        return Origin(fs.boneWorldMatrices[0]).z;
    };

    const f32 radius = kRadius * kBoneScale;
    const f32 want = radius + kDrop * kBoneScale; // 0.75 + 1.5
    const f32 unscaledOffset = radius + kDrop;    // 0.75 + 0.5, the defect

    // A margin, not an exactness: the solver rests a shape on a contact margin,
    // and 0.05 is far tighter than the 1.0 that separates the two answers.
    CHECK(settle(whiteout::m3::PhysicsShapeType::Sphere) == Approx(want).margin(0.05));
    CHECK(settle(whiteout::m3::PhysicsShapeType::Capsule) == Approx(want).margin(0.05));
    CHECK(std::fabs(want - unscaledOffset) > 0.5f);
}

TEST_CASE("a non-uniformly scaled bone stretches the colliders that can stretch", "[m3][phys]") {
    // StarCraft II reduces a bone's three scale components to their minimum and
    // splats it (`M3Physics_CreateRigidBody`, `0x102946d0d`), so its own runtime
    // simulates a stretched physics bone at its thinnest axis. That is a
    // consequence of `dmFixture` carrying one `m_scaleOrRadius` and cooking its
    // polytopes offline — the per-axis part is baked into the cook — and not of
    // anything the format cannot express.
    //
    // Snowball builds its polytopes from points handed to it, so the three
    // polytope kinds take the bone's scale per axis and the collider tracks the
    // limb. A sphere and a capsule still cannot: there is no ellipsoid in the
    // engine, so they keep the client's `min` and this test pins that too — the
    // divergence has to stay confined to the kinds that had a choice.
    constexpr f32 kTall = 3.0f;   // bone scale is (1, 1, 3)
    constexpr f32 kHalfZ = 0.3f;  // shapeDimensions.z, a half extent

    auto settle = [&](whiteout::m3::PhysicsShapeType type, FrameState& fsOut) {
        whiteout::m3::PhysicsShape ps;
        ps.shapeType = type;
        ps.transform = Matrix44f::identity();
        ps.shapeDimensions = {kHalfZ, kHalfZ, kHalfZ};

        whiteout::m3::RigidBody rb;
        rb.parentBoneIndex = 0;
        rb.simulationType = 1;
        rb.density = 1.0f;
        rb.friction = 0.5f;
        rb.restitution = 0.0f;
        rb.dynamicState = whiteout::m3::AnimRef<whiteout::u32>{};
        rb.dynamicState.initValue = 1;
        rb.rigidBodyShape.push_back(ps);

        m3fix::ModelBuilder mb;
        mb.Bone("root", -1, m3fix::ConstRef(Vector3f{0.0f, 0.0f, 5.0f}),
                m3fix::ConstRef(Quaternion{0, 0, 0, 1}),
                m3fix::ConstRef(Vector3f{1.0f, 1.0f, kTall}), whiteout::m3::BoneFlag::None);
        whiteout::m3::Model model = mb.Build();
        model.rigidBodies.push_back(rb);

        M3ModelAdapter adapter(model);
        auto stage = sc2::CreateSc2PhysicsStage(model);
        REQUIRE(stage);
        const auto skeleton = adapter.GetSkeleton();
        PoseStageContext ctx;
        ctx.nodeParents = skeleton.nodeParents;
        ctx.frameDtMs = 16;
        for (int frame = 0; frame < 900; ++frame) {
            fsOut = adapter.Evaluate(flakes::PoseRequest());
            stage->Run(fsOut, ctx);
        }
        REQUIRE_FALSE(fsOut.boneWorldMatrices.empty());
        REQUIRE(Finite(fsOut.boneWorldMatrices[0]));
        return Origin(fsOut.boneWorldMatrices[0]).z;
    };

    FrameState boxFs;
    const f32 boxRest = settle(whiteout::m3::PhysicsShapeType::Box, boxFs);
    // The box is a polytope, so its half-height is the bone's *own* z scale.
    // Collapsed to min(1, 1, 3) = 1 it would rest at 0.3, a third of this.
    CHECK(boxRest == Approx(kHalfZ * kTall).margin(0.05));

    FrameState sphereFs;
    const f32 sphereRest = settle(whiteout::m3::PhysicsShapeType::Sphere, sphereFs);
    // One radius, so `min` — and unchanged by the bone being three times taller.
    CHECK(sphereRest == Approx(kHalfZ).margin(0.05));

    // And the overlay draws what the solver settled on, which is the whole point
    // of the flag: a wireframe collapsed to min beside a box simulated per axis
    // is the same wrong picture the other way round.
    const auto shapes = [] {
        whiteout::m3::PhysicsShape ps;
        ps.shapeType = whiteout::m3::PhysicsShapeType::Box;
        ps.transform = Matrix44f::identity();
        ps.shapeDimensions = {kHalfZ, kHalfZ, kHalfZ};
        whiteout::m3::RigidBody rb;
        rb.parentBoneIndex = 0;
        rb.simulationType = 1;
        rb.dynamicState = whiteout::m3::AnimRef<whiteout::u32>{};
        rb.dynamicState.initValue = 1;
        rb.rigidBodyShape.push_back(ps);
        m3fix::ModelBuilder mb;
        mb.Bone("root", -1, m3fix::ConstRef(Vector3f{0.0f, 0.0f, 5.0f}),
                m3fix::ConstRef(Quaternion{0, 0, 0, 1}),
                m3fix::ConstRef(Vector3f{1.0f, 1.0f, kTall}), whiteout::m3::BoneFlag::None);
        whiteout::m3::Model model = mb.Build();
        model.rigidBodies.push_back(rb);
        return sc2::Sc2BuildCollisionShapes(model);
    }();
    REQUIRE(shapes.shapes.size() == 1);
    CHECK(shapes.anisotropic.size() == 1);
    CHECK(shapes.anisotropic[0] == 1);
    REQUIRE(boxFs.collisionTransforms.size() == 1);

    f32 lowest = 1e9f, highest = -1e9f;
    const Vector3f mn = shapes.shapes[0].vertices[0], mx = shapes.shapes[0].vertices[1];
    const Vector3f corners[8] = {{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z},
                                 {mn.x, mx.y, mn.z}, {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z},
                                 {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
    for (const Vector3f& v : corners) {
        const f32 z = whiteout::transform_point(v, boxFs.collisionTransforms[0]).z;
        lowest = std::min(lowest, z);
        highest = std::max(highest, z);
    }
    // Resting on the ground, and as tall as the stretched box the solver holds.
    CHECK(lowest == Approx(0.0f).margin(0.05));
    CHECK(highest - lowest == Approx(2.0f * kHalfZ * kTall).margin(0.05));
}

TEST_CASE("every PHSH kind draws as its own kind, at its own scale", "[m3][phys]") {
    // Five kinds of fixture want five kinds of wireframe, and two of them used
    // to share one: an M3 capsule and an M3 cylinder both came back as
    // `Cylinder`, so a capsule was drawn flat-capped. That hides a whole radius
    // of reach at either end — the exact margin that says whether a collider
    // covers the limb it drives — and it is invisible in the picture, because a
    // flat-capped tube is a perfectly plausible thing to be looking at.
    //
    // The second half of the case is the scale rule, on a bone scaled (1, 1, 3).
    // A sphere and a capsule can only be *drawn* under a uniform placement,
    // there being no ellipsoid to draw otherwise; a box, a cylinder and a hull
    // have to take all three axes or the wireframe contradicts the fixture
    // sitting inside it. One rig asserts both, because the two are the same
    // decision made in two places.
    namespace m3 = whiteout::m3;
    using Kind = whiteout::flakes::renderer::model::CollisionShapeType;
    constexpr f32 kTall = 3.0f;

    auto authored = [](m3::PhysicsShapeType type, const Vector3f& dims) {
        m3::PhysicsShape ps;
        ps.shapeType = type;
        ps.transform = Matrix44f::identity();
        ps.shapeDimensions = dims;
        return ps;
    };

    m3::RigidBody rb;
    rb.parentBoneIndex = 0;
    rb.simulationType = 1;
    rb.density = 1.0f;
    rb.rigidBodyShape.push_back(authored(m3::PhysicsShapeType::Box, {0.3f, 0.2f, 0.1f}));
    rb.rigidBodyShape.push_back(authored(m3::PhysicsShapeType::Sphere, {0.25f, 0.0f, 0.0f}));
    rb.rigidBodyShape.push_back(authored(m3::PhysicsShapeType::Capsule, {0.1f, 0.6f, 0.0f}));
    rb.rigidBodyShape.push_back(authored(m3::PhysicsShapeType::Cylinder, {0.2f, 0.5f, 0.0f}));
    {
        m3::PhysicsShape hull = authored(m3::PhysicsShapeType::ConvexHull, {0.0f, 0.0f, 0.0f});
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2)
                for (int sz = -1; sz <= 1; sz += 2)
                    hull.hullFaceNormals.push_back(
                        {0.2f * (f32)sx, 0.2f * (f32)sy, 0.2f * (f32)sz});
        rb.rigidBodyShape.push_back(hull);
    }
    // Last, and deliberately: a capsule of zero length is simulated as a sphere,
    // so it has to be *drawn* as one. The overlay shows the fixture, not the file.
    rb.rigidBodyShape.push_back(authored(m3::PhysicsShapeType::Capsule, {0.15f, 0.0f, 0.0f}));
    // And a box with a zero extent, which the engine simulates as a thin slab
    // (`sb::kMinBoxHalfExtent`). A wireframe drawn at the authored zero is a flat
    // quad over a collider that has thickness.
    rb.rigidBodyShape.push_back(authored(m3::PhysicsShapeType::Box, {0.3f, 0.2f, 0.0f}));

    m3fix::ModelBuilder mb;
    mb.Bone("root", -1, m3fix::ConstRef(Vector3f{0.0f, 0.0f, 5.0f}),
            m3fix::ConstRef(Quaternion{0, 0, 0, 1}),
            m3fix::ConstRef(Vector3f{1.0f, 1.0f, kTall}), whiteout::m3::BoneFlag::None);
    m3::Model model = mb.Build();
    model.rigidBodies.push_back(rb);

    const auto built = sc2::Sc2BuildCollisionShapes(model);
    REQUIRE(built.shapes.size() == 7);
    REQUIRE(built.anisotropic.size() == 7);

    const Kind want[7] = {Kind::Box,      Kind::Sphere, Kind::Capsule, Kind::Cylinder,
                          Kind::Hull,     Kind::Sphere, Kind::Box};
    const whiteout::u8 perAxis[7] = {1, 0, 0, 1, 1, 0, 1};
    for (std::size_t i = 0; i < 7; ++i) {
        INFO("shape " << i);
        CHECK(built.shapes[i].type == static_cast<whiteout::i32>(want[i]));
        CHECK(built.anisotropic[i] == perAxis[i]);
    }

    // Each kind's own geometry, in the fields that kind's drawing reads.
    CHECK(built.shapes[0].vertices[1].x == Approx(0.3f));
    CHECK(built.shapes[0].vertices[1].z == Approx(0.1f));
    CHECK(built.shapes[1].radius == Approx(0.25f));
    CHECK(built.shapes[2].radius == Approx(0.1f));
    // Cap centres, so 0.6 apart and reaching 0.1 further at each end.
    CHECK(Dist(built.shapes[2].vertices[0], built.shapes[2].vertices[1]) == Approx(0.6f));
    CHECK(built.shapes[3].radius == Approx(0.2f));
    CHECK(Dist(built.shapes[3].vertices[0], built.shapes[3].vertices[1]) == Approx(0.5f));
    CHECK(built.shapes[4].hullPoints.size() == 8);
    CHECK(built.shapes[5].radius == Approx(0.15f));
    CHECK(built.shapes[6].vertices[1].z > 0.0f);

    // And the placement each one lands under.
    M3ModelAdapter adapter(model);
    const auto shapes = adapter.GetCollisionShapes();
    REQUIRE(shapes.size() == 7);
    const FrameState fs = adapter.Evaluate(flakes::PoseRequest());
    REQUIRE(fs.collisionTransforms.size() == 7);

    auto rowLen = [](const Matrix44f& m, int r) {
        return std::sqrt(m.data[r][0] * m.data[r][0] + m.data[r][1] * m.data[r][1] +
                         m.data[r][2] * m.data[r][2]);
    };
    for (std::size_t i = 0; i < 7; ++i) {
        const Matrix44f& m = fs.collisionTransforms[i];
        const f32 x = rowLen(m, 0), y = rowLen(m, 1), z = rowLen(m, 2);
        INFO("shape " << i << " placed rows " << x << "," << y << "," << z);
        if (perAxis[i] != 0) {
            // The bone's 3 on z reaches the collider, so the wireframe grows with it.
            CHECK(z == Approx(kTall * x).margin(0.01));
        } else {
            // Every axis equal, which is the only way a drawn circle stays a circle.
            CHECK(y == Approx(x).margin(0.001));
            CHECK(z == Approx(x).margin(0.001));
        }
    }
}
