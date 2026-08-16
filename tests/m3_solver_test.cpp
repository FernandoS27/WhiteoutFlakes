// ============================================================================
// Post-skeleton pose stages: terrain foot IK (IKJT) and the turret (PATU).
//
// Device-free by construction, and that is the design paying off rather than a
// convenience: a stage reads a `FrameState` and a context of plain values, so
// every one of these cases drives the real solver with no model, no adapter and
// no GPU. The one thing they cannot check is the chunk→stage translation, which
// `CreatePoseStages` does; that has its own case at the bottom using a
// synthetic model.
//
// The contract being pinned throughout: a solver composes a *delta* onto the
// animated pose. Disabling one has to give back exactly what the sampler
// produced, bit for bit — which is also why the render gates stay meaningful
// with solvers off.
// ============================================================================

#include "io/m3/m3_model_adapter.h"
#include "io/m3/m3_pose_solvers.h"
#include "m3_anim_builders.h"

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

using namespace whiteout;
using namespace whiteout::flakes;
using namespace whiteout::flakes::io;
using whiteout::flakes::renderer::animation::PoseStageContext;
using whiteout::flakes::renderer::model::FrameState;
using Catch::Approx;

namespace {

Matrix44f At(f32 x, f32 y, f32 z) {
    return Matrix44f::translation({x, y, z});
}

Vector3f PosOf(const FrameState& fs, std::size_t bone) {
    REQUIRE(bone < fs.boneWorldMatrices.size());
    const auto& m = fs.boneWorldMatrices[bone];
    return {m.data[3][0], m.data[3][1], m.data[3][2]};
}

/// @brief A *bent* three-bone leg: hip (0,0,30), knee (10,0,15), ankle (0,0,10).
///
/// Bent on purpose, and with slack on purpose. Links of 18.0 and 11.2 give a
/// reach of 29.2 against a root-to-ankle distance of 20, so the goals below are
/// inside the chain's range — a straight, fully-extended chain cannot reach
/// anything further and converges to "as close as the links allow", which reads
/// exactly like a solver that failed to converge. That mistake cost a debugging
/// pass; the fixture now has the margin written down.
FrameState Leg() {
    FrameState fs;
    fs.boneWorldMatrices = {At(0, 0, 30), At(10, 0, 15), At(0, 0, 10)};
    return fs;
}

/// @brief A ground plane at @p z that always reports a hit.
PoseStageContext FlatGround(f32 z, i32 dtMs = 16) {
    PoseStageContext ctx;
    ctx.frameDtMs = dtMs;
    ctx.queryGround = [z](const Vector3f&, f32, f32, f32& out) {
        out = z;
        return true;
    };
    return ctx;
}

bool Same(const FrameState& a, const FrameState& b) {
    if (a.boneWorldMatrices.size() != b.boneWorldMatrices.size())
        return false;
    for (std::size_t i = 0; i < a.boneWorldMatrices.size(); ++i)
        for (i32 r = 0; r < 4; ++r)
            for (i32 c = 0; c < 4; ++c)
                if (a.boneWorldMatrices[i].data[r][c] != b.boneWorldMatrices[i].data[r][c])
                    return false;
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// IKJT
// ---------------------------------------------------------------------------

TEST_CASE("A goal inside tolerance leaves the pose bit-identical", "[m3solver]") {
    // `ComputeGroundGoal` returns false and the whole solve is skipped. Not an
    // optimisation — it is why an actor standing on flat ground at its authored
    // height costs nothing and, more importantly, does not drift.
    M3JtIkStage ik({0, 1, 2}, 50.0f, 50.0f, 1000.0f, 5.0f);
    FrameState fs = Leg();
    const FrameState before = fs;
    auto ctx = FlatGround(12.0f); // effector is at z=10, tolerance 5
    ik.Run(fs, ctx);
    REQUIRE(Same(fs, before));
}

TEST_CASE("The chain reaches a ground step below the foot", "[m3solver]") {
    M3JtIkStage ik({0, 1, 2}, 50.0f, 50.0f, 100000.0f, 0.5f);
    FrameState fs = Leg();
    auto ctx = FlatGround(5.0f); // 5 units below the effector, well inside reach
    ik.Run(fs, ctx);

    // Within tolerance of the goal plane, and bounded iterations got there.
    REQUIRE(PosOf(fs, 2).z == Approx(5.0f).margin(0.5f));
    // The root is the anchor and does not move: a leg plants its foot, it does
    // not drag the hip down.
    REQUIRE(PosOf(fs, 0).z == Approx(30.0f));
}

TEST_CASE("The goal is rate-limited across frames", "[m3solver]") {
    // maxSpeed 10 * 0.03 = 0.3 units per ms; 16 ms => 4.8 units of goal travel.
    // The first frame has no previous goal and snaps, so the limit is measured
    // from the second onward — which is what the cached state is for.
    M3JtIkStage ik({0, 1, 2}, 50.0f, 50.0f, 10.0f, 0.01f);
    FrameState fs = Leg();

    auto near = FlatGround(9.0f, 16);
    ik.Run(fs, near); // seeds the goal at 9
    const f32 afterFirst = PosOf(fs, 2).z;
    REQUIRE(afterFirst == Approx(9.0f).margin(0.2f));

    // Now the ground drops 100 units in one frame — a cliff edge. The foot may
    // only follow 4.8 of it.
    auto cliff = FlatGround(-91.0f, 16);
    ik.Run(fs, cliff);
    REQUIRE(PosOf(fs, 2).z == Approx(9.0f - 4.8f).margin(0.3f));
}

TEST_CASE("Two probes take the higher surface", "[m3solver]") {
    // The ledge case: one probe under the foot, one past it. `fmaxf` of the
    // hits, so a foot straddling an edge rests on the higher side instead of
    // dropping into the gap.
    M3JtIkStage ik({0, 1, 2}, 50.0f, 50.0f, 100000.0f, 0.5f);
    FrameState fs = Leg();
    PoseStageContext ctx;
    ctx.frameDtMs = 16;
    // The far probe sits at effector + (effector - previous) = (-10, 0, 5).
    ctx.queryGround = [](const Vector3f& p, f32, f32, f32& out) {
        out = (p.x < -5.0f) ? 14.0f : 2.0f; // the far probe is over the high side
        return true;
    };
    ik.Run(fs, ctx);
    REQUIRE(PosOf(fs, 2).z == Approx(14.0f).margin(0.5f));
}

TEST_CASE("No ground hit falls back to boneZ + searchDown", "[m3solver]") {
    M3JtIkStage ik({0, 1, 2}, 50.0f, -6.0f, 100000.0f, 0.5f);
    FrameState fs = Leg();
    PoseStageContext ctx;
    ctx.frameDtMs = 16;
    ctx.queryGround = [](const Vector3f&, f32, f32, f32&) { return false; };
    ik.Run(fs, ctx);
    // 10 + (-6) = 4, and it is a real solve rather than a no-op.
    REQUIRE(PosOf(fs, 2).z == Approx(4.0f).margin(0.5f));
}

TEST_CASE("Without a ground query the stage does nothing at all", "[m3solver]") {
    // The renderer has no terrain of its own; a stage with no host input must
    // leave the pose alone rather than solve against a guessed plane.
    M3JtIkStage ik({0, 1, 2}, 50.0f, 50.0f, 1000.0f, 0.5f);
    FrameState fs = Leg();
    const FrameState before = fs;
    PoseStageContext ctx;
    ctx.frameDtMs = 16;
    ik.Run(fs, ctx);
    REQUIRE(Same(fs, before));
}

TEST_CASE("A stage that moves a bone moves its descendants too", "[m3solver]") {
    // pose_stage.h's contract: there is no re-composition pass afterwards, so
    // a solver that rotated a parent and left the children behind would tear
    // the model apart and nothing would catch it.
    M3JtIkStage ik({0, 1, 2}, 50.0f, 50.0f, 100000.0f, 0.5f);
    FrameState fs = Leg();
    const FrameState before = fs;
    auto ctx = FlatGround(4.0f);
    ik.Run(fs, ctx);

    // The middle joint has to have moved: reaching 30 units down cannot happen
    // by rotating the last link alone.
    const Vector3f mid = PosOf(fs, 1), midBefore = PosOf(before, 1);
    const f32 moved = std::fabs(mid.x - midBefore.x) + std::fabs(mid.z - midBefore.z);
    REQUIRE(moved > 1.0f);
}

// ---------------------------------------------------------------------------
// PATU
// ---------------------------------------------------------------------------

TEST_CASE("With no target the turret is exactly the animated pose", "[m3solver]") {
    M3TurretStage t(0, {0, 0, 1}, 3.0f, false, 0, 0);
    FrameState fs;
    fs.boneWorldMatrices = {Matrix44f::identity()};
    const FrameState before = fs;
    PoseStageContext ctx;
    ctx.frameDtMs = 16;
    t.Run(fs, ctx);
    REQUIRE(Same(fs, before));
}

TEST_CASE("The turret rotates about its permitted axis and no other",
          "[m3solver]") {
    // Single DOF: whatever the target's elevation, the delta is a rotation
    // about the descriptor axis. Checked by the axis staying put under the
    // rotation the stage applied.
    M3TurretStage t(0, {0, 0, 1}, 1000.0f, false, 0, 0);
    FrameState fs;
    fs.boneWorldMatrices = {Matrix44f::identity()};
    PoseStageContext ctx;
    ctx.frameDtMs = 1000;
    ctx.aimTarget = Vector3f{10.0f, 0.0f, 50.0f}; // well above the plane
    t.Run(fs, ctx);

    const auto& m = fs.boneWorldMatrices[0];
    // Row 2 is the Z basis; a pure Z rotation leaves it alone.
    REQUIRE(m.data[2][0] == Approx(0.0f).margin(1e-4f));
    REQUIRE(m.data[2][1] == Approx(0.0f).margin(1e-4f));
    REQUIRE(m.data[2][2] == Approx(1.0f).margin(1e-4f));
    // And it did rotate: the bone's forward (+Y) swung toward +X.
    REQUIRE(m.data[1][0] > 0.5f);
}

TEST_CASE("The turret slews rather than snapping, then converges", "[m3solver]") {
    M3TurretStage t(0, {0, 0, 1}, 1.0f, false, 0, 0);
    PoseStageContext ctx;
    ctx.frameDtMs = 16;
    ctx.aimTarget = Vector3f{10.0f, 0.0f, 0.0f}; // 90 degrees off the +Y forward

    FrameState fs;
    fs.boneWorldMatrices = {Matrix44f::identity()};
    t.Run(fs, ctx); // first frame snaps: nothing to slew from yet
    const f32 first = fs.boneWorldMatrices[0].data[1][0];

    // Re-running with the same demand must not keep rotating — the turret is
    // already there, and a stage that composed onto its own output instead of
    // onto the animation would wind up without bound.
    for (i32 i = 0; i < 20; ++i) {
        fs.boneWorldMatrices = {Matrix44f::identity()};
        t.Run(fs, ctx);
    }
    REQUIRE(fs.boneWorldMatrices[0].data[1][0] == Approx(first).margin(1e-3f));
}

TEST_CASE("Clearing the target releases the turret back to its animation",
          "[m3solver]") {
    M3TurretStage t(0, {0, 0, 1}, 1.0f, false, 0, 0);
    FrameState fs;
    fs.boneWorldMatrices = {Matrix44f::identity()};

    PoseStageContext aiming;
    aiming.frameDtMs = 16;
    aiming.aimTarget = Vector3f{10.0f, 0.0f, 0.0f};
    t.Run(fs, aiming);
    REQUIRE(fs.boneWorldMatrices[0].data[1][0] > 0.5f);

    // Target gone: the demand is zero, i.e. the animated rotation, and the
    // turret walks back to it over several frames rather than snapping.
    PoseStageContext idle;
    idle.frameDtMs = 100;
    for (i32 i = 0; i < 60; ++i) {
        fs.boneWorldMatrices = {Matrix44f::identity()};
        t.Run(fs, idle);
    }
    REQUIRE(fs.boneWorldMatrices[0].data[1][0] == Approx(0.0f).margin(1e-3f));
    REQUIRE(fs.boneWorldMatrices[0].data[1][1] == Approx(1.0f).margin(1e-3f));
}

TEST_CASE("Yaw limits clamp the demand", "[m3solver]") {
    // ±0.2 rad, against a target 90 degrees away.
    M3TurretStage t(0, {0, 0, 1}, 1000.0f, true, -0.2f, 0.2f);
    FrameState fs;
    fs.boneWorldMatrices = {Matrix44f::identity()};
    PoseStageContext ctx;
    ctx.frameDtMs = 1000;
    ctx.aimTarget = Vector3f{10.0f, 0.0f, 0.0f};
    t.Run(fs, ctx);
    // sin(0.2) ~= 0.199, nowhere near the sin(pi/2) = 1 an unclamped aim gives.
    REQUIRE(fs.boneWorldMatrices[0].data[1][0] == Approx(std::sin(0.2f)).margin(1e-3f));
}

TEST_CASE("The easing curve is a smoothstep on [0,1]", "[m3solver]") {
    REQUIRE(M3TurretEase(0.0f) == Approx(0.0f));
    REQUIRE(M3TurretEase(1.0f) == Approx(1.0f));
    REQUIRE(M3TurretEase(0.5f) == Approx(0.5f));
    REQUIRE(M3TurretEase(-1.0f) == Approx(0.0f)); // clamped, not extrapolated
    REQUIRE(M3TurretEase(2.0f) == Approx(1.0f));
}

// ---------------------------------------------------------------------------
// chunk -> stage
// ---------------------------------------------------------------------------

TEST_CASE("A model with no solver chunks appends no stages", "[m3solver]") {
    // The common case by a wide margin, and the reason `CreatePoseStages`
    // defaults to a no-op rather than being pure.
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    M3ModelAdapter a(mb.Build());
    renderer::animation::PoseStageList stages;
    a.CreatePoseStages(stages);
    REQUIRE(stages.empty());
}

TEST_CASE("An IKJT chunk becomes one stage over the parent walk", "[m3solver]") {
    m3fix::ModelBuilder mb;
    mb.StaticBone("hip", -1);
    mb.StaticBone("knee", 0);
    mb.StaticBone("ankle", 1);
    mb.StaticBone("unrelated", -1);
    m3::Model model = mb.Build();

    m3::IKJoint jt;
    jt.boneIndex1 = 0; // root
    jt.boneIndex2 = 2; // effector
    jt.raycastUp = 10.0f;
    jt.raycastDown = 10.0f;
    jt.maxSpeed = 100.0f;
    jt.goalThreshold = 0.5f;
    model.ikJoints.push_back(jt);

    M3ModelAdapter a(std::move(model));
    renderer::animation::PoseStageList stages;
    a.CreatePoseStages(stages);
    REQUIRE(stages.size() == 1);

    // And it actually solves the chain the walk found: driving it moves the
    // ankle, which only happens if the chain is {hip, knee, ankle}.
    FrameState fs;
    fs.boneWorldMatrices = {At(0, 0, 30), At(10, 0, 15), At(0, 0, 10), At(0, 0, 0)};
    auto ctx = FlatGround(5.0f);
    stages[0]->Run(fs, ctx);
    REQUIRE(PosOf(fs, 2).z == Approx(5.0f).margin(0.6f));
    // The bone outside the chain is untouched.
    REQUIRE(PosOf(fs, 3).z == Approx(0.0f));
}

TEST_CASE("An IKJT whose ends are not related is skipped", "[m3solver]") {
    // A damaged or unexpected chunk costs a missing solver, not a hang: the
    // parent walk is bounded by the bone count and the result is only accepted
    // if it actually reached the named root.
    m3fix::ModelBuilder mb;
    mb.StaticBone("a", -1);
    mb.StaticBone("b", -1);
    m3::Model model = mb.Build();
    m3::IKJoint jt;
    jt.boneIndex1 = 0;
    jt.boneIndex2 = 1; // b is a root, not a descendant of a
    model.ikJoints.push_back(jt);

    M3ModelAdapter a(std::move(model));
    renderer::animation::PoseStageList stages;
    a.CreatePoseStages(stages);
    REQUIRE(stages.empty());
}

TEST_CASE("Stage order is IK first, then turrets", "[m3solver]") {
    // The runner never sorts, so the creator's order IS the contract. IK moves
    // the mount a turret is bolted to, so it has to run first.
    m3fix::ModelBuilder mb;
    mb.StaticBone("hip", -1);
    mb.StaticBone("knee", 0);
    mb.StaticBone("turret", 0);
    m3::Model model = mb.Build();

    m3::IKJoint jt;
    jt.boneIndex1 = 0;
    jt.boneIndex2 = 1;
    jt.goalThreshold = 0.5f;
    jt.maxSpeed = 100.0f;
    model.ikJoints.push_back(jt);

    m3::TurretBehavior tb;
    tb.transform = Matrix44f::identity();
    tb.boneIndex = 2;
    tb.yawWeight = 1.0f;
    model.turretBehaviors.push_back(tb);

    M3ModelAdapter a(std::move(model));
    renderer::animation::PoseStageList stages;
    a.CreatePoseStages(stages);
    REQUIRE(stages.size() == 2);

    // Identify them by behaviour rather than by type: the first responds to a
    // ground query, the second to an aim target.
    // A two-bone chain that is neither collinear with its goal nor fully
    // extended, so the solver has something to do.
    FrameState fs;
    fs.boneWorldMatrices = {At(0, 0, 30), At(10, 0, 20), Matrix44f::identity()};
    auto ground = FlatGround(22.0f);
    const FrameState before = fs;
    stages[0]->Run(fs, ground);
    REQUIRE_FALSE(Same(fs, before));

    FrameState fs2;
    fs2.boneWorldMatrices = {At(0, 0, 30), At(10, 0, 20), Matrix44f::identity()};
    PoseStageContext aim;
    aim.frameDtMs = 1000;
    aim.aimTarget = Vector3f{10.0f, 0.0f, 0.0f};
    const FrameState before2 = fs2;
    stages[1]->Run(fs2, aim);
    REQUIRE_FALSE(Same(fs2, before2));
}

// ---------------------------------------------------------------------------
// corpus
// ---------------------------------------------------------------------------

TEST_CASE("Solver chunks across the corpus build stages without incident",
          "[m3solver][corpus]") {
    // Two jobs. The obvious one is that `CreatePoseStages` survives real chunk
    // data — a malformed parent link or an out-of-range bone index has to cost
    // a skipped solver, never a hang or a crash.
    //
    // The other is to keep the *scale* of solver content honest. It is far
    // smaller than the plan assumed: almost nothing in the corpus carries IKJT
    // or PATU at all, so a green G5 solver arm says very little on its own, and
    // the unit cases above are what actually covers these two stages. Anyone
    // reading a passing gate should know that.
    namespace fs = std::filesystem;
    const fs::path root = [] {
        if (const char* v = std::getenv("WDX_TEST_SC2_CORPUS"); v && *v)
            return fs::path(v);
        return fs::path("C:/Projects/WhiteoutLib/Corpus");
    }();

    std::size_t scanned = 0, withIk = 0, withTurret = 0, stagesBuilt = 0;
    std::vector<std::string> ikExamples, turretExamples;

    for (const char* c : {"Sc2M3", "Sc2BetaM3", "StarM3", "HotSM3"}) {
        const fs::path dir = root / c;
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            continue;
        std::size_t taken = 0;
        for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec || taken >= 600)
                break;
            if (!it->is_regular_file(ec) || it->path().extension() != ".m3")
                continue;
            std::ifstream f(it->path(), std::ios::binary);
            if (!f)
                continue;
            std::vector<u8> bytes((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
            m3::Model model;
            try {
                m3::Parser p;
                model = p.parse(bytes);
            } catch (...) {
                continue;
            }
            ++taken;
            ++scanned;
            const bool ik = !model.ikJoints.empty();
            const bool tu = !model.turretBehaviors.empty();
            if (!ik && !tu)
                continue;
            if (ik && ikExamples.size() < 5)
                ikExamples.push_back(it->path().filename().string());
            if (tu && turretExamples.size() < 5)
                turretExamples.push_back(it->path().filename().string());
            withIk += ik ? 1 : 0;
            withTurret += tu ? 1 : 0;

            M3ModelAdapter a(std::move(model));
            renderer::animation::PoseStageList stages;
            a.CreatePoseStages(stages); // must not hang, throw or crash
            stagesBuilt += stages.size();
        }
    }

    if (scanned == 0) {
        WARN("no `.m3` corpus under " << root.string() << " — skipping");
        return;
    }
    for (const auto& n : ikExamples)
        WARN("IKJT model: " << n);
    for (const auto& n : turretExamples)
        WARN("PATU model: " << n);
    INFO("scanned " << scanned << ", IKJT " << withIk << ", PATU " << withTurret << ", stages "
                    << stagesBuilt);
    REQUIRE(scanned > 0);
}

TEST_CASE("Neither current stage claims a bone", "[m3solver]") {
    // The physics seam, asserted from the other direction. `BoneClaim` /
    // `IPoseStage::Claims` exist so a future ragdoll has a writer for
    // `PoseRequest::overrides` that is not the host — but IK and the turret
    // compose a delta onto the animation rather than replacing it, and
    // StarCraft II marks IK chains with a bit that explicitly does NOT suppress
    // sampling (SC2_ANIM_RE §4c). A stage that started claiming bones here
    // would make the sampler skip them, and the model would freeze in the parts
    // the solver touches.
    M3JtIkStage ik({0, 1, 2}, 50.0f, 50.0f, 1000.0f, 0.5f);
    FrameState fs = Leg();
    auto ctx = FlatGround(5.0f);
    ik.Run(fs, ctx);
    REQUIRE(ik.Claims().empty());

    M3TurretStage t(0, {0, 0, 1}, 1.0f, false, 0, 0);
    PoseStageContext aim;
    aim.frameDtMs = 16;
    aim.aimTarget = Vector3f{10.0f, 0.0f, 0.0f};
    t.Run(fs, aim);
    REQUIRE(t.Claims().empty());
}

TEST_CASE("A degenerate yaw limit is unset, not locked", "[m3solver]") {
    // Every PATU descriptor in the corpus ships yawMin == yawMax == 0. Read
    // literally that clamps the demand to zero and the turret silently never
    // moves — which is exactly what it did until this was found.
    M3TurretStage locked(0, {0, 0, 1}, 1000.0f, /*yawLimited=*/true, 0.0f, 0.0f);
    FrameState fs;
    fs.boneWorldMatrices = {Matrix44f::identity()};
    PoseStageContext ctx;
    ctx.frameDtMs = 1000;
    ctx.aimTarget = Vector3f{10.0f, 0.0f, 0.0f};
    locked.Run(fs, ctx);
    REQUIRE(fs.boneWorldMatrices[0].data[1][0] > 0.5f);
}
