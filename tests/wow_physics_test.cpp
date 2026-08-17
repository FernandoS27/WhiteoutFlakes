// `.phys` -> Snowball, end to end.
//
// The chain this exercises is long and every link is somebody's guess until it
// runs: WhiteoutLib parses the PFDC payload, `wow_physics.cpp` maps it onto
// Domino's body/shape/joint model per `DOMINO_GLUE.md` §2-§3, and Snowball
// simulates it. A slip anywhere — an inverted body type, a transposed joint
// frame, a shape index read off the wrong array — produces a rig that either
// explodes or does nothing, and both look the same from the renderer.
//
// So the assertions are the ones that separate those two failures from a
// working rig: the dynamic bones must *move*, they must move *finitely*, and
// they must stay near the model rather than departing for the horizon.
//
// Corpus only. Lor'themar is the named fixture because he is dense — 38 bodies,
// 27 joints, ponytail and robes — and because a model with two dynamic bodies
// would pass this suite while being wrong.

#include "whiteout/flakes/pose_stage.h"
#include "renderer/profiles/wow/wow_physics.h"
#include "whiteout/models/m2/parser.h"
#include "whiteout/utils/os_file_system.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace flakes = whiteout::flakes;

using whiteout::Matrix44f;
using whiteout::f32;

namespace {

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_WOW_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/WoW");
}

fs::path LorthemarPath() {
    return CorpusRoot() / "creature" / "lorthemar" / "lorthemar.m2";
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

whiteout::Vector3f Origin(const Matrix44f& m) {
    return {m.data[3][0], m.data[3][1], m.data[3][2]};
}

f32 Dist(const whiteout::Vector3f& a, const whiteout::Vector3f& b) {
    const f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

TEST_CASE("Lor'themar's .phys drives his ponytail and robes", "[m2][phys][corpus]") {
    const fs::path path = LorthemarPath();
    if (!fs::exists(path))
        SKIP("no corpus model at " + path.string());

    // The parser, not the renderer adapter: `M2ModelAdapter::Load` also wants
    // `.skin` profiles and a CASC to resolve them against, and physics needs
    // neither. Bones and `.physics` are all this exercises.
    whiteout::utils::OsFileSystem vfs(path.parent_path().string());
    whiteout::m2::Parser parser;
    const whiteout::m2::Model model = parser.parse(vfs, path.string());
    REQUIRE_FALSE(model.bones.empty());

    // The premise. If this model ever loses its inline physics the rest of the
    // test would pass vacuously by skipping, so it is a hard requirement.
    REQUIRE(model.physics.has_value());
    const whiteout::m2::PhysicsData& phys = *model.physics;
    CHECK(phys.version == 6);
    CHECK(phys.bodies.size() > 8);
    CHECK_FALSE(phys.joints.empty());

    std::size_t dynamicBodies = 0;
    std::size_t kinematicBodies = 0;
    for (const auto& b : phys.bodies) {
        if (b.type == whiteout::m2::PhysicsBodyType::Dynamic)
            ++dynamicBodies;
        else
            ++kinematicBodies;
    }
    // Both halves must exist: all-dynamic means nothing anchors the rig to the
    // skeleton, all-kinematic means nothing ever simulates.
    CHECK(dynamicBodies > 0);
    CHECK(kinematicBodies > 0);

    auto stage = flakes::renderer::profiles::wow::CreateWowPhysicsStage(model);
    REQUIRE(stage != nullptr);

    // **The bind pose is an all-identity palette, and that is the whole point.**
    // `boneWorldMatrices` is a skinning palette, not a table of bone transforms:
    // `M2ModelAdapter::Evaluate` brackets each local SRT with `T(-pivot) ...
    // T(+pivot)`, so an unanimated bone gets identity and the bind frame lives
    // in the pivot alone.
    //
    // This test used to seed the translation row with each pivot, which is the
    // shape the *stage* wrongly assumed and no adapter ever produces. It made
    // the suite agree with the bug: physics settled into a plausible-looking
    // rig here while the viewer hung a tangle off the model origin, because
    // every body really was seeded at (0,0,0).
    flakes::renderer::model::FrameState fs;
    fs.boneWorldMatrices.assign(model.bones.size(), Matrix44f::identity());

    flakes::renderer::animation::PoseStageContext ctx;
    ctx.frameDtMs = 16;

    // First call seeds and returns; everything after it simulates.
    stage->Run(fs, ctx);

    // A bone's posed position is its pivot pushed through its palette entry —
    // `pivot * palette`, row-vector — not the palette's translation row, which
    // is the residual after the pivot has been bracketed back out.
    auto sample = [&]() {
        std::vector<whiteout::Vector3f> out;
        for (const auto& c : stage->Claims()) {
            const auto i = static_cast<std::size_t>(c.node);
            if (i >= fs.boneWorldMatrices.size()) {
                out.push_back({});
                continue;
            }
            const Matrix44f& m = fs.boneWorldMatrices[i];
            const auto& p = model.bones[i].pivot;
            out.push_back({p.x * m.data[0][0] + p.y * m.data[1][0] + p.z * m.data[2][0] +
                               m.data[3][0],
                           p.x * m.data[0][1] + p.y * m.data[1][1] + p.z * m.data[2][1] +
                               m.data[3][1],
                           p.x * m.data[0][2] + p.y * m.data[1][2] + p.z * m.data[2][2] +
                               m.data[3][2]});
        }
        return out;
    };
    /// Largest distance between any two simulated bones — the rig's extent.
    auto spread = [](const std::vector<whiteout::Vector3f>& p) {
        f32 w = 0.0f;
        for (std::size_t i = 0; i < p.size(); ++i)
            for (std::size_t j = i + 1; j < p.size(); ++j)
                w = std::max(w, Dist(p[i], p[j]));
        return w;
    };

    for (int frame = 0; frame < 60; ++frame)
        stage->Run(fs, ctx);
    const std::vector<whiteout::Vector3f> early = sample();

    for (int frame = 0; frame < 240; ++frame)
        stage->Run(fs, ctx);
    const std::vector<whiteout::Vector3f> mid = sample();

    for (int frame = 0; frame < 300; ++frame)
        stage->Run(fs, ctx);
    const std::vector<whiteout::Vector3f> late = sample();

    REQUIRE(early.size() == dynamicBodies);

    for (std::size_t i = 0; i < late.size(); ++i) {
        INFO("bone " << stage->Claims()[i].node);
        CHECK(std::isfinite(late[i].x));
        CHECK(std::isfinite(late[i].y));
        CHECK(std::isfinite(late[i].z));
    }

    // **The collapse guard.** Every joint family except distance and spherical
    // pins B's origin at `localAnchorB` read in *A's* frame. Hand it the anchor
    // from B's frame instead — the symmetric-looking assignment, and the wrong
    // one — and every link pins its child onto its parent. The rig concertinas
    // into a point while each joint holds perfectly, so nothing is unstable,
    // nothing is NaN, and the only visible symptom is the skinned mesh
    // stretching. This is the assertion that catches it.
    const f32 spreadEarly = spread(early);
    const f32 spreadLate = spread(late);
    INFO("spread " << spreadEarly << " -> " << spreadLate);
    CHECK(spreadEarly > 0.1f);
    CHECK(spreadLate > 0.5f * spreadEarly);

    // **...and the rig has to be the size of the skeleton it came from.** The
    // collapse guard above only compares the rig against itself, so it passes
    // just as happily on a rig that seeded every body onto the model origin and
    // then let the joints push them apart — which is exactly what reading the
    // palette's translation row as a bone position produces, since that row is
    // zero for every bone at bind. Anchoring the extent to the pivots is what
    // separates "wrong place" from "wrong size".
    std::vector<whiteout::Vector3f> pivots;
    for (const auto& c : stage->Claims())
        pivots.push_back(model.bones[static_cast<std::size_t>(c.node)].pivot);
    const f32 spreadPivots = spread(pivots);
    CHECK(spreadPivots > 0.5f);

    // Every simulated bone stays near the bone it *is*. Cloth swings — the
    // ponytail tip is released sticking out and settles about 0.9 down, which
    // is real physics and not an error — but no bone should travel further than
    // the whole rig is wide. That bound is self-calibrating and still an order
    // of magnitude below the failure it guards: taking a bone's frame from the
    // palette's translation row instead of from `pivot * palette` seeds every
    // body on the model origin, and the rig then hangs off the character's
    // navel, metres from its own pivots.
    f32 wander = 0.0f;
    for (std::size_t i = 0; i < late.size(); ++i)
        wander = std::max(wander, Dist(late[i], pivots[i]));
    INFO("pivot spread " << spreadPivots << ", worst wander from bind " << wander);
    CHECK(wander < spreadPivots);

    // ...and the divergence guard: a rig that is merely wrong settles somewhere
    // wrong, while an unstable one grows without bound. Between 5s and 10s of
    // simulation a hanging chain with nothing driving it should have stopped
    // moving.
    f32 drift = 0.0f;
    for (std::size_t i = 0; i < late.size(); ++i)
        drift = std::max(drift, Dist(late[i], mid[i]));
    INFO("drift between 5s and 10s: " << drift);
    CHECK(drift < 0.05f);
}

TEST_CASE("Lor'themar's cloth survives a moving skeleton", "[m2][phys][corpus]") {
    // **Everything above runs on a pose that never changes**, and a whole class of bug is
    // invisible there. The kinematic drive turns the difference between the current and animated
    // pose into a velocity, so at rest that velocity is zero and any error in how it is derived
    // multiplies by nothing. The angular half was wrong for exactly this reason: the quaternion
    // difference was converted to an angular velocity with the cross-product term negated, which
    // is a perfect no-op standing still and spins every collider backwards once a clip plays.
    //
    // So this drives the skeleton. Not a real sequence -- the parser has the animation data but
    // evaluating it needs the adapter, its skins and a CASC -- but a swing about the model's X
    // axis at a plausible rate, which is what the drive actually has to cope with.
    const fs::path path = LorthemarPath();
    if (!fs::exists(path))
        SKIP("no corpus model at " + path.string());

    whiteout::utils::OsFileSystem vfs(path.parent_path().string());
    whiteout::m2::Parser parser;
    const whiteout::m2::Model model = parser.parse(vfs, path.string());
    REQUIRE(model.physics.has_value());

    auto stage = flakes::renderer::profiles::wow::CreateWowPhysicsStage(model);
    REQUIRE(stage != nullptr);

    flakes::renderer::model::FrameState fs;
    fs.boneWorldMatrices.assign(model.bones.size(), Matrix44f::identity());
    flakes::renderer::animation::PoseStageContext ctx;
    ctx.frameDtMs = 16;
    stage->Run(fs, ctx);

    auto bonePos = [&](std::size_t i) {
        const Matrix44f& m = fs.boneWorldMatrices[i];
        const auto& p = model.bones[i].pivot;
        return whiteout::Vector3f{
            p.x * m.data[0][0] + p.y * m.data[1][0] + p.z * m.data[2][0] + m.data[3][0],
            p.x * m.data[0][1] + p.y * m.data[1][1] + p.z * m.data[2][1] + m.data[3][1],
            p.x * m.data[0][2] + p.y * m.data[1][2] + p.z * m.data[2][2] + m.data[3][2]};
    };

    f32 worst = 0.0f;
    for (int frame = 0; frame < 600; ++frame) {
        // A +-25 degree swing about X at roughly one cycle a second, applied about each bone's
        // own pivot so the palette stays a well-formed skinning matrix.
        const f32 angle = 0.44f * std::sin(static_cast<f32>(frame) * 0.1f);
        const f32 c = std::cos(angle), s = std::sin(angle);
        for (std::size_t i = 0; i < model.bones.size(); ++i) {
            Matrix44f m = Matrix44f::identity();
            m.data[1][1] = c;  m.data[1][2] = s;
            m.data[2][1] = -s; m.data[2][2] = c;
            const auto& p = model.bones[i].pivot;
            // T(-pivot) * R * T(+pivot), row-vector — the same bracketing the adapter uses.
            m.data[3][0] = p.x - (p.x * m.data[0][0] + p.y * m.data[1][0] + p.z * m.data[2][0]);
            m.data[3][1] = p.y - (p.x * m.data[0][1] + p.y * m.data[1][1] + p.z * m.data[2][1]);
            m.data[3][2] = p.z - (p.x * m.data[0][2] + p.y * m.data[1][2] + p.z * m.data[2][2]);
            fs.boneWorldMatrices[i] = m;
        }
        stage->Run(fs, ctx);

        // The cloth is driven, so it lags and overshoots — but it is attached, and a bone that
        // has left the neighbourhood of its own pivot has come off the skeleton.
        for (const auto& claim : stage->Claims()) {
            const auto i = static_cast<std::size_t>(claim.node);
            const whiteout::Vector3f now = bonePos(i);
            REQUIRE(std::isfinite(now.x));
            REQUIRE(std::isfinite(now.y));
            REQUIRE(std::isfinite(now.z));
            worst = std::max(worst, Dist(now, model.bones[i].pivot));
        }
    }
    INFO("worst excursion from bind while animating: " << worst);
    CHECK(worst < 4.0f);
}

TEST_CASE("The pose a bone is seeded with is the pose it comes back with", "[m2][phys][corpus]") {
    // **The orientation round-trip, which nothing else here can see.** The stage reads a bone's
    // frame out of the palette and writes the simulated one back, and those two conversions have
    // to be exact inverses. Get one of them transposed and every simulated bone renders as its
    // own conjugate rotation — silent in position, total in orientation, and *stable*, so the rig
    // still hangs correctly, still settles, still collides, and every assertion above passes.
    //
    // A rotation and its transpose are both orthonormal with determinant +1, so no property of a
    // single matrix catches this. Only comparing against the pose that went in does. The pose is
    // deliberately asymmetric: a rotation about a single axis is its own conjugate about the
    // negated axis, and a symmetric one would round-trip either way.
    const fs::path path = LorthemarPath();
    if (!fs::exists(path))
        SKIP("no corpus model at " + path.string());

    whiteout::utils::OsFileSystem vfs(path.parent_path().string());
    whiteout::m2::Parser parser;
    const whiteout::m2::Model model = parser.parse(vfs, path.string());
    REQUIRE(model.physics.has_value());

    auto stage = flakes::renderer::profiles::wow::CreateWowPhysicsStage(model);
    REQUIRE(stage != nullptr);

    // Yaw 40 degrees, then pitch 25 — a composition with no zero off-diagonal terms.
    const f32 cy = std::cos(0.7f), sy = std::sin(0.7f);
    const f32 cp = std::cos(0.44f), sp = std::sin(0.44f);
    const Matrix44f yaw = [&] {
        Matrix44f m = Matrix44f::identity();
        m.data[0][0] = cy; m.data[0][1] = sy; m.data[1][0] = -sy; m.data[1][1] = cy;
        return m;
    }();
    Matrix44f rot = yaw;
    for (int r = 0; r < 3; ++r) {
        const f32 y = rot.data[r][1], z = rot.data[r][2];
        rot.data[r][1] = y * cp + z * sp;
        rot.data[r][2] = -y * sp + z * cp;
    }

    flakes::renderer::model::FrameState fs;
    fs.boneWorldMatrices.assign(model.bones.size(), rot);
    for (std::size_t i = 0; i < model.bones.size(); ++i) {
        // T(-pivot) * R * T(+pivot), so the palette stays a well-formed skinning matrix.
        const auto& p = model.bones[i].pivot;
        fs.boneWorldMatrices[i].data[3][0] =
            p.x - (p.x * rot.data[0][0] + p.y * rot.data[1][0] + p.z * rot.data[2][0]);
        fs.boneWorldMatrices[i].data[3][1] =
            p.y - (p.x * rot.data[0][1] + p.y * rot.data[1][1] + p.z * rot.data[2][1]);
        fs.boneWorldMatrices[i].data[3][2] =
            p.z - (p.x * rot.data[0][2] + p.y * rot.data[1][2] + p.z * rot.data[2][2]);
    }

    f32 worst = 0.0f;
    for (const auto& claim : stage->Claims()) {
        const auto b = static_cast<std::size_t>(claim.node);
        const Matrix44f& in = fs.boneWorldMatrices[b];
        const Matrix44f out =
            flakes::renderer::profiles::wow::RoundTripBoneFrame(in, model.bones[b].pivot);
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c)
                worst = std::max(worst, std::abs(out.data[r][c] - in.data[r][c]));
    }
    INFO("worst palette deviation across the round trip: " << worst);
    CHECK(worst < 1e-4f);
}

TEST_CASE("Joint angles in .phys are degrees and must be converted", "[m2][phys][corpus]") {
    // **A units bug that a clamp swallows.** `dmShoulderJoint::Create` clamps the cone to
    // [10, 170] degrees *expressed in radians*, and Snowball reproduces that clamp — so a cone
    // handed over in degrees does not assert, produce a NaN, or go out of range. It saturates
    // at 170 degrees. Every joint silently becomes a free ball joint while a debug dump of the
    // def still reads like a configured cone, and the cloth folds through itself and the body.
    //
    // WhiteoutLib's `ShoulderJoint::coneAngle` doc says the loader converts on the way in; it
    // does not, `binary_parse_visitor/phys.cpp` reads the float straight through. So this pins
    // the units at the source rather than trusting that comment.
    const fs::path path = LorthemarPath();
    if (!fs::exists(path))
        SKIP("no corpus model at " + path.string());

    whiteout::utils::OsFileSystem vfs(path.parent_path().string());
    whiteout::m2::Parser parser;
    const whiteout::m2::Model model = parser.parse(vfs, path.string());
    REQUIRE(model.physics.has_value());
    const auto& shoulders = model.physics->shoulderJoints;
    REQUIRE_FALSE(shoulders.empty());

    constexpr f32 kPi = 3.14159265f;
    constexpr f32 kMaxCone = 170.0f * kPi / 180.0f;  // dmShoulderJoint's own upper clamp
    bool anyBeyondPi = false;
    for (const auto& sj : shoulders) {
        // Degrees, positively: a cone wider than a half turn is not a cone. Lor'themar's are
        // 20/35/45/60, and the corpus holds nothing else.
        INFO("cone " << sj.coneAngle);
        CHECK(sj.coneAngle > 1.0f);
        CHECK(sj.coneAngle <= 180.0f);
        anyBeyondPi = anyBeyondPi || sj.coneAngle > kPi;

        // ...and once converted it has to land strictly inside the clamp. Landing *on* the
        // clamp is the failure: that is what an unconverted value does.
        const f32 radians = sj.coneAngle * kPi / 180.0f;
        CHECK(radians < kMaxCone * 0.99f);
    }
    // If every cone were under pi the file would be ambiguous and this test would prove nothing.
    CHECK(anyBeyondPi);
}

TEST_CASE("A model with no .phys produces no physics stage", "[m2][phys]") {
    // Any corpus model without inline physics would do; a WC3-era `.m2` has
    // none, and neither does a freshly default-constructed one.
    whiteout::m2::Model bare;
    bare.bones.resize(4);
    // Not an inert stage: an empty one still costs a virtual call and a claim
    // copy per actor per frame, and every WC3 model in the scene would pay it.
    CHECK(flakes::renderer::profiles::wow::CreateWowPhysicsStage(bare) == nullptr);
}
