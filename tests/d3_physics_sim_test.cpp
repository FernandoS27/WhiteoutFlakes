// ============================================================================
// D3 rigid bodies through Snowball — the G5 gate.
//
// Separate from `d3_physics_test.cpp` because that one is data-only and is
// registered whether or not physics is compiled in; this one links Snowball and
// is registered beside `wow_physics_test` / `sc2_physics_test` under
// WDX_ENABLE_PHYSICS.
//
// **This area's failure mode, in two profiles already, is a green gate over a
// rig that never simulated.** StarCraft II's was a ragdoll left kinematic
// because `simulationType` lies; the M3 animation arm's first recording was
// three separate breaks hiding behind a bind pose, with PASS, the draw count
// and a non-blank golden healthy throughout. So every case below asserts that
// something *moved*, and by how much, rather than that nothing threw.
//
// D3 adds a fourth way to pass vacuously that neither of the others has: the
// stage is **disarmed by default**. A rig that is never armed runs, claims
// nothing, steps nothing and leaves a perfectly correct pose behind. So the
// disarmed state is asserted explicitly, and every collapse case arms first.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "renderer/animation/anim_math.h"
#include "renderer/profiles/diablo3/d3_collision.h"
#include "renderer/profiles/diablo3/d3_physics.h"
#include "whiteout/flakes/model_types.h"

#include <whiteout/sno/d3/native/d3_native.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace d3n = ::whiteout::sno::d3::native;
namespace d3p = ::whiteout::flakes::renderer::profiles::diablo3;
namespace sb = ::snowball;

using namespace ::whiteout;
using ::whiteout::flakes::renderer::animation::PoseStageContext;
using ::whiteout::flakes::renderer::model::FrameState;

namespace {

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_D3_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/D3");
}

std::vector<u8> ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return {};
    f.seekg(0, std::ios::end);
    const auto n = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<u8> b(n);
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
    return b;
}

Vector3f Origin(const Matrix44f& m) {
    return {m.data[3][0], m.data[3][1], m.data[3][2]};
}

f32 Dist(const Vector3f& a, const Vector3f& b) {
    const f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool Finite(const Matrix44f& m) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(m.data[r][c]))
                return false;
    return true;
}

/// @brief The bind pose as `FrameState::boneWorldMatrices`, from `tTransform0`.
///
/// `tTransform0` is the **model-space** bind pose A — the pose the client's own
/// per-bone table holds when `Physics_CreateActorBoneBodies` reads it — so this
/// composes nothing and is the pose the rig is authored against. Going through
/// the local pose (`tTransform2`) and a parent walk instead would reproduce
/// bind pose *B*, and 8% of bones over a 400-file sample disagree about which
/// of the two they are in.
FrameState BindPose(const d3n::Appearances& app) {
    FrameState fs;
    fs.boneWorldMatrices.reserve(app.arBones.size());
    for (const auto& b : app.arBones) {
        const auto& t = b.tTransform0;
        const Quaternion q{t.qRotation.x, t.qRotation.y, t.qRotation.z, t.qRotation.w};
        const f32 s = t.flScale != 0.0f ? t.flScale : 1.0f;
        fs.boneWorldMatrices.push_back(::whiteout::flakes::renderer::animation::ComposePivotSRT(
            t.vTranslation, q, {s, s, s}, {0.0f, 0.0f, 0.0f}));
    }
    return fs;
}

std::vector<i32> Parents(const d3n::Appearances& app) {
    std::vector<i32> out;
    out.reserve(app.arBones.size());
    for (const auto& b : app.arBones)
        out.push_back(b.nParentIndex);
    return out;
}

std::size_t DynamicBones(const d3n::Appearances& app, i32 lod) {
    std::size_t n = 0;
    for (const auto& b : app.arBones)
        for (const auto& s : b.arCollisionShapes)
            if (s.nLodIndex == lod && s.flScaleX > 0.0f) {
                ++n;
                break;
            }
    return n;
}

struct Rig {
    std::shared_ptr<const d3n::Appearances> app;
    std::vector<u8> bytes;
};

Rig LoadRig(const char* file) {
    Rig r;
    r.bytes = ReadAll(CorpusRoot() / "Appearances" / file);
    if (r.bytes.empty())
        return r;
    if (auto parsed = d3n::parseAppearances(r.bytes))
        r.app = std::make_shared<d3n::Appearances>(std::move(*parsed));
    return r;
}

} // namespace

// ---------------------------------------------------------------------------

TEST_CASE("the D3 bone-frame conversion is its own inverse", "[d3][phys]") {
    // The one error a finished frame cannot show you: transposing one of the
    // two boundary conversions hands every body its **conjugate** rotation,
    // which is stable, physically plausible, and wrong only in orientation.
    // Unit, and deliberately so: `ComposePivotSRT` bakes whatever norm it is
    // given into the basis, and the conversion normalises — so a non-unit
    // literal here measures the test's own arithmetic, not the round trip's.
    auto unit = [](Quaternion q) {
        const f32 n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        return Quaternion{q.x / n, q.y / n, q.z / n, q.w / n};
    };
    const Quaternion qs[] = {
        unit({0.0f, 0.0f, 0.0f, 1.0f}),
        unit({0.2f, -0.5f, 0.3f, 0.788f}),
        unit({-0.7071068f, 0.0f, 0.0f, 0.7071068f}),
        unit({0.35f, 0.35f, 0.35f, 0.7842f}),
    };
    const Vector3f ts[] = {{0.0f, 0.0f, 0.0f}, {3.5f, -2.25f, 11.0f}, {-100.0f, 0.5f, 0.0f}};
    // Uniform on purpose: a D3 bone's scale is a single float all the way down
    // the hierarchy (`Skeleton_ComposeWorldPose`), which is why this conversion
    // carries one and not three.
    const f32 ss[] = {1.0f, 0.35f, 4.0f};

    for (const auto& q : qs) {
        for (const auto& t : ts) {
            for (const f32 s : ss) {
                const Matrix44f world =
                    ::whiteout::flakes::renderer::animation::ComposePivotSRT(t, q, {s, s, s},
                                                                            {0.0f, 0.0f, 0.0f});
                const Matrix44f back = d3p::D3RoundTripBoneFrame(world);
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        CHECK(std::fabs(back.data[r][c] - world.data[r][c]) < 1e-4f);
            }
        }
    }
}

TEST_CASE("a degenerate D3 bone basis does not produce NaN", "[d3][phys]") {
    Matrix44f m = Matrix44f::identity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            m.data[r][c] = 0.0f;
    m.data[3][0] = 1.0f;
    CHECK(Finite(d3p::D3RoundTripBoneFrame(m)));
}

TEST_CASE("a D3 character rig collapses when armed", "[d3][phys][corpus]") {
    // The lod-1 rig, which is what the client builds: seven shapes on a
    // skeleton, every one of them dynamic. `Physics_CreateActorBoneBodies` is
    // called with lod **1** at its one call site, and the same builder at lod 0
    // would give the full collider set instead.
    const Rig rig = LoadRig("skeleton_oneHand.app");
    if (!rig.app)
        SKIP("no corpus model skeleton_oneHand.app under " + CorpusRoot().string());

    // The premises. Without these the rest passes vacuously.
    REQUIRE_FALSE(rig.app->arBones.empty());
    REQUIRE(DynamicBones(*rig.app, d3p::kD3BoneBodyLod) > 0);

    auto control = std::make_shared<d3p::D3PhysicsControl>();
    auto stage = d3p::CreateD3PhysicsStage(*rig.app, rig.bytes, nullptr, control);
    REQUIRE(stage);
    // Everything this rig needs is in the appearance and on the ground plane.
    REQUIRE_FALSE(stage->NeedsHostInputs());

    const FrameState bind = BindPose(*rig.app);
    const std::vector<i32> parents = Parents(*rig.app);
    PoseStageContext ctx;
    ctx.nodeParents = parents;
    ctx.frameDtMs = 16;

    // ---- disarmed -----------------------------------------------------------
    //
    // The fourth way to pass vacuously, and the one only D3 has. A stage that
    // is never armed leaves a perfectly correct pose behind, so "the pose is
    // right" is not evidence of anything until it has been armed.
    {
        FrameState fs = bind;
        for (int frame = 0; frame < 30; ++frame)
            stage->Run(fs, ctx);
        CHECK(stage->Claims().empty());
        for (std::size_t i = 0; i < fs.boneWorldMatrices.size(); ++i)
            CHECK(Dist(Origin(fs.boneWorldMatrices[i]), Origin(bind.boneWorldMatrices[i])) < 1e-6f);
    }

    // ---- armed --------------------------------------------------------------
    control->simulating = true;

    FrameState fs = bind;
    // The first armed Run builds the scene from the pose and seeds it, and must
    // not move anything: a rig that jumps on frame one is seeding from the
    // wrong space, which no later frame can distinguish from a solver fault.
    stage->Run(fs, ctx);
    for (std::size_t i = 0; i < fs.boneWorldMatrices.size(); ++i)
        CHECK(Dist(Origin(fs.boneWorldMatrices[i]), Origin(bind.boneWorldMatrices[i])) < 1e-5f);
    // And it must have taken the bones over, or nothing it computes reaches the
    // palette however well the solver runs.
    REQUIRE_FALSE(stage->Claims().empty());

    // One step, then the rest. The two are compared at the end: a gate whose
    // failure mode is its own null hypothesis is not a gate, and "settled
    // differs from one step" is the cheapest statement that cannot be true of
    // a rig that never stepped.
    FrameState oneStep = fs;
    stage->Run(oneStep, ctx);

    for (int frame = 0; frame < 240; ++frame)
        stage->Run(fs, ctx);

    f32 worst = 0.0f, farthest = 0.0f, drop = 0.0f, fromOneStep = 0.0f;
    for (std::size_t i = 0; i < fs.boneWorldMatrices.size(); ++i) {
        REQUIRE(Finite(fs.boneWorldMatrices[i]));
        const Vector3f now = Origin(fs.boneWorldMatrices[i]);
        const Vector3f was = Origin(bind.boneWorldMatrices[i]);
        worst = (std::max)(worst, Dist(now, was));
        drop = (std::min)(drop, now.z - was.z);
        farthest = (std::max)(farthest, Dist(now, {0.0f, 0.0f, 0.0f}));
        fromOneStep = (std::max)(fromOneStep, Dist(now, Origin(oneStep.boneWorldMatrices[i])));
    }

    // **The drop is the assertion that matters.** Displacement alone is what a
    // rig that merely jitters produces too, and a rig whose bodies all came out
    // Static produces neither — which is exactly the shape of the mistake here,
    // D3's body type being decided at build from one float per shape.
    CHECK(worst > 0.5f);
    CHECK(drop < -0.5f);
    // Nothing may have left, and nothing may have sunk through the floor — a
    // body whose fixtures came out massless does exactly that while everything
    // around it looks healthy.
    CHECK(farthest < 200.0f);
    CHECK(drop > -40.0f);
    // Progressive: settled is not one step.
    CHECK(fromOneStep > 0.01f);
}

TEST_CASE("a D3 breakable collapses at lod 0", "[d3][phys][corpus]") {
    // The complement, and the rig the *other* 2,367 models carry: the same
    // builder over the full lod-0 collider set. Worth its own case because the
    // two lods reach different shapes — a character proxy is spheres and
    // capsules, a breakable is mostly cooked polytopes — and the polytope path
    // is the one that can silently produce no fixture at all.
    const Rig rig = LoadRig("x1_Fortress_Soul_Grinder.app");
    if (!rig.app)
        SKIP("no corpus model x1_Fortress_Soul_Grinder.app under " + CorpusRoot().string());
    REQUIRE(DynamicBones(*rig.app, 0) > 0);

    auto control = std::make_shared<d3p::D3PhysicsControl>();
    auto stage = d3p::CreateD3PhysicsStage(*rig.app, rig.bytes, nullptr, control,
                                            d3p::D3RigMode::BoneBodies, 0);
    REQUIRE(stage);
    control->simulating = true;

    const FrameState bind = BindPose(*rig.app);
    const std::vector<i32> parents = Parents(*rig.app);
    PoseStageContext ctx;
    ctx.nodeParents = parents;
    ctx.frameDtMs = 16;

    FrameState fs = bind;
    stage->Run(fs, ctx);
    REQUIRE_FALSE(stage->Claims().empty());
    // The 64-body cap is the client's, and it counts bodies that survived:
    // a body the bridge destroys for having no fixture never reaches the
    // counter. This rig authors 55 dynamic bones, so it is near it either way.
    CHECK(stage->Claims().size() <= d3p::kD3MaxBodies);

    for (int frame = 0; frame < 240; ++frame)
        stage->Run(fs, ctx);

    f32 worst = 0.0f, drop = 0.0f;
    for (std::size_t i = 0; i < fs.boneWorldMatrices.size(); ++i) {
        REQUIRE(Finite(fs.boneWorldMatrices[i]));
        const Vector3f now = Origin(fs.boneWorldMatrices[i]);
        const Vector3f was = Origin(bind.boneWorldMatrices[i]);
        worst = (std::max)(worst, Dist(now, was));
        drop = (std::min)(drop, now.z - was.z);
    }
    CHECK(worst > 0.5f);
    CHECK(drop < -0.5f);
}

TEST_CASE("releasing and re-arming a D3 rig collapses again from the animation",
          "[d3][phys][corpus]") {
    // Releasing hands the bones back to the sampler, so re-arming has to
    // **re-seed** rather than resume — the client gets this for free by
    // destroying the whole physics object and building a new one. Resuming
    // instead is invisible for one collapse and wrong for every one after it:
    // the rig picks up from where it fell rather than from where the model is.
    const Rig rig = LoadRig("skeleton_oneHand.app");
    if (!rig.app)
        SKIP("no corpus model skeleton_oneHand.app under " + CorpusRoot().string());

    auto control = std::make_shared<d3p::D3PhysicsControl>();
    auto stage = d3p::CreateD3PhysicsStage(*rig.app, rig.bytes, nullptr, control);
    REQUIRE(stage);

    const FrameState bind = BindPose(*rig.app);
    const std::vector<i32> parents = Parents(*rig.app);
    PoseStageContext ctx;
    ctx.nodeParents = parents;
    ctx.frameDtMs = 16;

    control->simulating = true;
    FrameState fs = bind;
    for (int frame = 0; frame < 120; ++frame)
        stage->Run(fs, ctx);
    REQUIRE_FALSE(stage->Claims().empty());

    // Released: no claims, and the sampler's pose passes through untouched.
    control->simulating = false;
    fs = bind;
    stage->Run(fs, ctx);
    CHECK(stage->Claims().empty());
    for (std::size_t i = 0; i < fs.boneWorldMatrices.size(); ++i)
        CHECK(Dist(Origin(fs.boneWorldMatrices[i]), Origin(bind.boneWorldMatrices[i])) < 1e-6f);

    // Re-armed: the seeding frame must put the rig back on the *animated* pose,
    // not leave it where the first collapse ended. That is the whole assertion
    // — a resumed rig would still be lying on the floor here.
    control->simulating = true;
    fs = bind;
    stage->Run(fs, ctx);
    for (std::size_t i = 0; i < fs.boneWorldMatrices.size(); ++i)
        CHECK(Dist(Origin(fs.boneWorldMatrices[i]), Origin(bind.boneWorldMatrices[i])) < 1e-5f);

    // And it collapses again rather than sitting there.
    for (int frame = 0; frame < 240; ++frame)
        stage->Run(fs, ctx);
    f32 drop = 0.0f;
    for (std::size_t i = 0; i < fs.boneWorldMatrices.size(); ++i) {
        REQUIRE(Finite(fs.boneWorldMatrices[i]));
        drop = (std::min)(drop, Origin(fs.boneWorldMatrices[i]).z -
                                    Origin(bind.boneWorldMatrices[i]).z);
    }
    CHECK(drop < -0.5f);
}

TEST_CASE("a rig with no dynamic bone gets no D3 stage", "[d3][phys][corpus]") {
    // `CreateD3PhysicsStage` returns nullptr rather than an inert stage, and
    // the predicate is the same one the build uses — so this is a check that
    // the cheap pre-check and the expensive build agree, not a style rule.
    // At lod 1, the overwhelming majority of the corpus has no rig at all.
    const Rig rig = LoadRig("x1_Fortress_Soul_Grinder.app");
    if (!rig.app)
        SKIP("no corpus model under " + CorpusRoot().string());
    REQUIRE(DynamicBones(*rig.app, d3p::kD3BoneBodyLod) == 0);
    CHECK(d3p::CreateD3PhysicsStage(*rig.app, rig.bytes, nullptr,
                                    std::make_shared<d3p::D3PhysicsControl>()) == nullptr);
}

// ---------------------------------------------------------------------------
// PH3 — joints and the anchored rig
// ---------------------------------------------------------------------------

namespace {

d3n::PRTransform Frame(Quaternion q, Vector3f t) {
    d3n::PRTransform f;
    f.qRotation = {q.x, q.y, q.z, q.w};
    f.vTranslation = t;
    return f;
}

/// A constraint with everything authored, so a case can change one field.
d3n::ConstraintParameters MakeConstraint(i32 type, i32 flags) {
    d3n::ConstraintParameters c;
    c.eConstraintType = type;
    c.dwFlags = flags;
    c.tFrameB = Frame({0.0f, 0.0f, 0.7071068f, 0.7071068f}, {1.0f, 2.0f, 3.0f});
    c.tFrameC = Frame({0.7071068f, 0.0f, 0.0f, 0.7071068f}, {-4.0f, 5.0f, -6.0f});
    // A frame nothing in the joint path reads. Deliberately distinctive: if it
    // ever shows up in a def, the -56 shift was applied to the wrong field.
    c.tFrameA = Frame({0.0f, 1.0f, 0.0f, 0.0f}, {99.0f, 99.0f, 99.0f});
    c.flLimitLower = -0.785398f;
    c.flLimitUpper = 0.785398f;
    c.flConeAngle = 0.785398f;
    c.flTwistLower = -0.698132f;
    c.flTwistUpper = 0.698132f;
    return c;
}

sb::Vec4 Q(const Quaternion& q) {
    return sb::Vec4{q.x, q.y, q.z, q.w};
}

bool NearQ(const sb::Vec4& a, const sb::Vec4& b) {
    return std::fabs(a.x - b.x) < 1e-5f && std::fabs(a.y - b.y) < 1e-5f &&
           std::fabs(a.z - b.z) < 1e-5f && std::fabs(a.w - b.w) < 1e-5f;
}

bool NearV(const sb::Vec4& a, Vector3f b) {
    return std::fabs(a.x - b.x) < 1e-5f && std::fabs(a.y - b.y) < 1e-5f &&
           std::fabs(a.z - b.z) < 1e-5f;
}

/// Rotate a vector by a quaternion, the long way, so the test does not lean on
/// the same helper the code under test uses.
Vector3f Rotate(const sb::Vec4& q, Vector3f v) {
    const f32 x = q.x, y = q.y, z = q.z, w = q.w;
    const Vector3f u{x, y, z};
    const f32 dot = u.x * v.x + u.y * v.y + u.z * v.z;
    const Vector3f cross{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    return {2.0f * dot * u.x + (w * w - (x * x + y * y + z * z)) * v.x + 2.0f * w * cross.x,
            2.0f * dot * u.y + (w * w - (x * x + y * y + z * z)) * v.y + 2.0f * w * cross.y,
            2.0f * dot * u.z + (w * w - (x * x + y * y + z * z)) * v.z + 2.0f * w * cross.z};
}

} // namespace

TEST_CASE("the D3 joint type map has a hole at 4", "[d3][phys]") {
    // A finding, not a gap in the read: the client's `switch` has cases for
    // 0..3 and its `default:` falls through to the allocation-failure path. It
    // is 4,475 of the corpus's 14,570 constraints, so a stage that treated an
    // unknown type as "make a spherical" would over-constrain nearly a third of
    // every rig — silently, and in a way that looks like a stiff ragdoll rather
    // than a bug.
    sb::JointDef def;
    CHECK(d3p::D3MakeJointDef(MakeConstraint(0, 0), 0, 1, def));
    CHECK(def.kind == sb::JointKind::Revolute);
    CHECK(d3p::D3MakeJointDef(MakeConstraint(1, 0), 0, 1, def));
    CHECK(def.kind == sb::JointKind::Shoulder);
    CHECK(d3p::D3MakeJointDef(MakeConstraint(2, 0), 0, 1, def));
    CHECK(def.kind == sb::JointKind::Spherical);
    CHECK(d3p::D3MakeJointDef(MakeConstraint(3, 0), 0, 1, def));
    CHECK(def.kind == sb::JointKind::Weld);

    CHECK_FALSE(d3p::D3MakeJointDef(MakeConstraint(4, 0), 0, 1, def));
    for (i32 t = 5; t < 12; ++t)
        CHECK_FALSE(d3p::D3MakeJointDef(MakeConstraint(t, 0), 0, 1, def));
}

TEST_CASE("a D3 revolute frame is pre-rotated by the cyclic axis permutation", "[d3][phys]") {
    // **The one-day mistake.** Both `q x c` branches build `c` from the lane
    // constants at 0x7100E45110/20/30 with ±0.5, and a frame missing that
    // pre-rotation gives a hinge about the wrong axis — which still hangs,
    // still collides and still settles, so no amount of watching a ragdoll
    // finds it.
    const auto c = MakeConstraint(0, 0);
    sb::JointDef def;
    REQUIRE(d3p::D3MakeJointDef(c, 0, 1, def));

    const sb::Vec4 qB = Q({c.tFrameB.qRotation.x, c.tFrameB.qRotation.y, c.tFrameB.qRotation.z,
                           c.tFrameB.qRotation.w});
    const sb::Vec4 qC = Q({c.tFrameC.qRotation.x, c.tFrameC.qRotation.y, c.tFrameC.qRotation.z,
                           c.tFrameC.qRotation.w});
    const sb::Vec4 rev{0.5f, 0.5f, 0.5f, 0.5f};

    CHECK(NearQ(def.localFrameA, sb::QuatMultiply(qB, rev)));
    CHECK(NearQ(def.localFrameB, sb::QuatMultiply(qC, rev)));
    // The hinge reference is the anchor-A slot read as a quaternion. A zero
    // there leaves the whole angular half silently inert.
    CHECK(NearQ(def.localAnchorA, def.localFrameA));
    // …and it is NOT the raw frame, which is what a missing pre-rotation gives.
    CHECK_FALSE(NearQ(def.localFrameA, qB));

    // What `c` actually is: x -> y -> z -> x. That is the whole reason it
    // exists — D3 authors the constrained axis on a different axis than Domino
    // reads it from.
    CHECK(NearV(sb::Vec4{Rotate(rev, {1.0f, 0.0f, 0.0f}).x, Rotate(rev, {1.0f, 0.0f, 0.0f}).y,
                         Rotate(rev, {1.0f, 0.0f, 0.0f}).z, 0.0f},
                {0.0f, 1.0f, 0.0f}));
    CHECK(NearV(sb::Vec4{Rotate(rev, {0.0f, 1.0f, 0.0f}).x, Rotate(rev, {0.0f, 1.0f, 0.0f}).y,
                         Rotate(rev, {0.0f, 1.0f, 0.0f}).z, 0.0f},
                {0.0f, 0.0f, 1.0f}));
    CHECK(NearV(sb::Vec4{Rotate(rev, {0.0f, 0.0f, 1.0f}).x, Rotate(rev, {0.0f, 0.0f, 1.0f}).y,
                         Rotate(rev, {0.0f, 0.0f, 1.0f}).z, 0.0f},
                {1.0f, 0.0f, 0.0f}));
}

TEST_CASE("a D3 shoulder takes the conjugate, and spherical and weld take neither",
          "[d3][phys]") {
    const auto c = MakeConstraint(1, 0);
    const sb::Vec4 qB{c.tFrameB.qRotation.x, c.tFrameB.qRotation.y, c.tFrameB.qRotation.z,
                      c.tFrameB.qRotation.w};
    const sb::Vec4 qC{c.tFrameC.qRotation.x, c.tFrameC.qRotation.y, c.tFrameC.qRotation.z,
                      c.tFrameC.qRotation.w};
    const sb::Vec4 rev{0.5f, 0.5f, 0.5f, 0.5f};
    const sb::Vec4 sho{-0.5f, -0.5f, -0.5f, 0.5f};

    sb::JointDef def;
    REQUIRE(d3p::D3MakeJointDef(c, 0, 1, def));
    CHECK(NearQ(def.localFrameA, sb::QuatMultiply(qB, sho)));
    CHECK(NearQ(def.localFrameB, sb::QuatMultiply(qC, sho)));
    // The two are genuinely different pre-rotations, not the same one written
    // twice — which a copy-paste between the branches would give.
    CHECK_FALSE(NearQ(sb::QuatMultiply(qB, sho), sb::QuatMultiply(qB, rev)));
    CHECK(def.coneAngle == c.flConeAngle);
    CHECK(def.lowerTwist == c.flTwistLower);
    CHECK(def.upperTwist == c.flTwistUpper);
    // **Unconditional**, unlike the revolute's. `v63 = 1` with no flag test.
    CHECK(def.enableTwistLimit);

    // Spherical is the one family that keeps the symmetric two-anchor rule, and
    // it discards both rotations outright.
    REQUIRE(d3p::D3MakeJointDef(MakeConstraint(2, 0), 0, 1, def));
    CHECK(NearV(def.localAnchorA, c.tFrameB.vTranslation));
    CHECK(NearV(def.localAnchorB, c.tFrameC.vTranslation));

    // Weld names no axis, so its frames are raw.
    REQUIRE(d3p::D3MakeJointDef(MakeConstraint(3, 0), 0, 1, def));
    CHECK(NearQ(def.localFrameA, qB));
    CHECK(NearQ(def.localFrameB, qC));
    CHECK(NearQ(def.localAnchorA, qB));
    CHECK(NearV(def.localAnchorB, c.tFrameB.vTranslation));
}

TEST_CASE("D3 joint flags: collide is bit 0, the revolute's twist limit is bit 1",
          "[d3][phys]") {
    sb::JointDef def;
    REQUIRE(d3p::D3MakeJointDef(MakeConstraint(0, 0), 0, 1, def));
    CHECK_FALSE(def.collideConnected);
    CHECK_FALSE(def.enableTwistLimit);

    REQUIRE(d3p::D3MakeJointDef(MakeConstraint(0, 1), 0, 1, def));
    CHECK(def.collideConnected);
    CHECK_FALSE(def.enableTwistLimit);

    REQUIRE(d3p::D3MakeJointDef(MakeConstraint(0, 2), 0, 1, def));
    CHECK_FALSE(def.collideConnected);
    CHECK(def.enableTwistLimit);
    CHECK(def.lowerTwist == -0.785398f);
    CHECK(def.upperTwist == 0.785398f);

    // The shipped bitfield: 12, 13, 14, 15, 30, 31 — bits 1..3 on all 14,570,
    // so the twist limit is on for every shipped revolute and bit 0 decides
    // collision on 95 of them.
    REQUIRE(d3p::D3MakeJointDef(MakeConstraint(0, 15), 0, 1, def));
    CHECK(def.collideConnected);
    CHECK(def.enableTwistLimit);
}

TEST_CASE("a D3 anchored rig hangs from its anchors instead of falling",
          "[d3][phys][corpus]") {
    // `sub_71003E07B0`'s rig, and the finding that turned this phase around: a
    // `dwFlags` bit-0 shape marks a **kinematic anchor**, not a ragdoll member.
    // The builder creates every body Dynamic and then calls
    // `dmBody_SetType(body, 1)` on exactly those bones.
    //
    // A rope bridge rather than the Soul Grinder, and the difference is a
    // finding: **`dwFlags` bit 4 on a bone's first constraint is the only way a
    // chain STARTS.** The other route into the rig needs the ancestor to be
    // Dynamic, and the level directly below an anchor never is — its ancestor
    // is the anchor. So a model with 53 anchors and no bit-4 constraint (the
    // Soul Grinder) builds 53 kinematic bodies and nothing that moves. 181
    // models have both; this is the largest.
    const Rig rig = LoadRig("a1dun_caves_Neph_WaterBridge_A_Short.app");
    if (!rig.app)
        SKIP("no corpus model a1dun_caves_Neph_WaterBridge_A_Short.app under " +
             CorpusRoot().string());
    REQUIRE(d3p::D3HasRagdollAnchor(*rig.app));

    auto control = std::make_shared<d3p::D3PhysicsControl>();
    auto stage = d3p::CreateD3PhysicsStage(*rig.app, rig.bytes, nullptr, control,
                                           d3p::D3RigMode::Ragdoll);
    REQUIRE(stage);
    control->simulating = true;

    const FrameState bind = BindPose(*rig.app);
    const std::vector<i32> parents = Parents(*rig.app);
    PoseStageContext ctx;
    ctx.nodeParents = parents;
    ctx.frameDtMs = 16;

    FrameState fs = bind;
    stage->Run(fs, ctx);
    REQUIRE_FALSE(stage->Claims().empty());
    for (int frame = 0; frame < 240; ++frame)
        stage->Run(fs, ctx);

    f32 worst = 0.0f, drop = 0.0f;
    std::size_t held = 0;
    for (std::size_t i = 0; i < fs.boneWorldMatrices.size(); ++i) {
        REQUIRE(Finite(fs.boneWorldMatrices[i]));
        const f32 d = Dist(Origin(fs.boneWorldMatrices[i]), Origin(bind.boneWorldMatrices[i]));
        worst = (std::max)(worst, d);
        drop = (std::min)(drop, Origin(fs.boneWorldMatrices[i]).z -
                                    Origin(bind.boneWorldMatrices[i]).z);
        if (d < 1e-4f)
            ++held;
    }

    // **The anchors held.** They are kinematic and the pose driving them never
    // changes, so they must sit exactly where the bind pose put them. A rig
    // whose anchors came out Dynamic — the reading this phase started with —
    // would have none of these.
    CHECK(held > 0);
    // Something hung off them and moved, or the anchors are the whole rig.
    CHECK(worst > 0.05f);
    // **And nothing free-fell.** A jointed part is held near its anchor; an
    // unjointed one accelerates at 32.2 units/s² and is 100+ units down inside
    // four seconds. This is what the joints buy, and it is the assertion that
    // fails when they are not built.
    CHECK(drop > -20.0f);
    CHECK(worst < 50.0f);
}
