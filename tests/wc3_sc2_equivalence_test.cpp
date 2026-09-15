// ============================================================================
// Warcraft III -> StarCraft II equivalence (WC3_TO_SC2_COMPLETION_PLAN.md §3).
//
// Device-free: both renderers' own kernels run on one input, and the Warcraft
// III result, restated through the export's basis change, is what the
// StarCraft II result has to be.
//
// E1, billboards. `MdxBillboardNodeMatrix` is the hierarchy walk's billboard;
// `M3ApplyBillboard` is the `.m3` solver, transcribed from the client. A node's
// axes cross as `X_sc2 = U(Y_wc3)`, `Y_sc2 = -U(X_wc3)`, `Z_sc2 = U(Z_wc3)` with
// `U(x, y, z) = (y, -x, z)` -- the conjugation `toM3` applies to every bone --
// and a record is right when the bone it billboards comes out at that frame
// for every camera. Each row names the candidate it rejects, and the grid is
// checked to separate them: a row whose rejected candidate also passes is a
// grid about nothing.
// ============================================================================

#include "io/m3/m3_billboard.h"
#include "io/mdx_animation.h"
#include "renderer/particle/particle_curve.h"
#include "renderer/particle/particle_motion.h"
#include "renderer/particle/particle_shape.h"
#include "renderer/particle/particle_stages_sc2.h"
#include "renderer/ribbon/ribbon_emitter.h"
#include "renderer/sc2/sc2_rng.h"
#include "renderer/types.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace whiteout;
using namespace whiteout::flakes;
using namespace whiteout::flakes::io;

namespace {

constexpr u32 kBillboarded = 0x8;
constexpr u32 kLockX = 0x10;
constexpr u32 kLockY = 0x20;
constexpr u32 kLockZ = 0x40;
constexpr f32 kLengthScale = 0.01f; // Warcraft III units -> StarCraft II units

Vector3f Row(const Matrix44f& m, i32 r) {
    return {m.data[r][0], m.data[r][1], m.data[r][2]};
}

void SetRow(Matrix44f& m, i32 r, const Vector3f& v) {
    m.data[r][0] = v.x;
    m.data[r][1] = v.y;
    m.data[r][2] = v.z;
}

/// The export's change of basis on a vector: Warcraft III's +X forward, +Y
/// left into StarCraft II's -Y forward, +X left.
Vector3f U(const Vector3f& v) {
    return {v.y, -v.x, v.z};
}

/// A Warcraft III model-space node matrix as the `.m3` bone it becomes.
Matrix44f Restate(const Matrix44f& w) {
    Matrix44f s = Matrix44f::identity();
    SetRow(s, 0, U(Row(w, 1)));
    SetRow(s, 1, U(Row(w, 0)) * -1.0f);
    SetRow(s, 2, U(Row(w, 2)));
    SetRow(s, 3, U(Row(w, 3)) * kLengthScale);
    return s;
}

/// A rotation about a unit axis, as rows (row-vector convention).
Matrix44f AxisAngleRows(Vector3f axis, f32 angle) {
    axis = axis.normalized();
    const f32 h = angle * 0.5f;
    const f32 s = std::sin(h);
    return M3RotationRows(Quaternion{axis.x * s, axis.y * s, axis.z * s, std::cos(h)});
}

struct Camera {
    Vector3f eye;    ///< Warcraft III model space.
    Vector3f target; ///< Same.
};

/// Cameras on a sphere round an off-centre target: yaws and pitches that are
/// neither axis-aligned nor round, so a sign or an axis swap cannot hide.
std::vector<Camera> CameraGrid() {
    std::vector<Camera> out;
    const Vector3f target{10.0f, -7.0f, 50.0f};
    for (const f32 yaw : {0.0f, 0.7f, 1.9f, 2.8f, 3.3f, 4.1f, 5.5f}) {
        for (const f32 pitch : {-0.6f, 0.1f, 0.35f, 0.9f, 1.3f}) {
            const Vector3f dir{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw),
                               std::sin(pitch)};
            out.push_back({target + dir * 400.0f, target});
        }
    }
    return out;
}

/// The StarCraft II camera for a Warcraft III one: same eye and target,
/// restated, world +Z up.
M3CameraFrame Sc2Camera(const Camera& c) {
    const Vector3f eye = U(c.eye) * kLengthScale;
    const Vector3f target = U(c.target) * kLengthScale;
    return M3BuildCameraFrame(Matrix44f::identity(),
                              Matrix44f::look_at_rh(eye, target, {0.0f, 0.0f, 1.0f}), eye);
}

struct Pose {
    std::string name;
    Vector3f pivot;         ///< Warcraft III model space.
    Matrix44f parentRot;    ///< The parent's rotation, rows.
    bool root = true;
};

std::vector<Pose> PoseGrid() {
    const Matrix44f turned = AxisAngleRows({0.3f, 0.5f, 0.8f}, 0.9f);
    return {
        {"root at origin", {0.0f, 0.0f, 0.0f}, Matrix44f::identity(), true},
        {"root off-centre", {37.3f, -12.1f, 55.7f}, Matrix44f::identity(), true},
        {"child, still parent", {37.3f, -12.1f, 55.7f}, Matrix44f::identity(), false},
        {"child, turned parent", {-21.9f, 44.2f, 18.4f}, turned, false},
    };
}

struct Candidate {
    u8 type = 6;
    u8 lookAt = 1;
    Quaternion up{0, 0, 0, 1};
};

struct Verdict {
    u32 cases = 0;
    u32 rowsOff = 0;       ///< A basis row off by more than the tolerance.
    u32 facingOff = 0;     ///< The facing axis alone off.
    u32 nonFinite = 0;     ///< Counted as a mismatch, never skipped.
    f32 worstRow = 0.0f;
    f32 worstRoll = 0.0f;  ///< Radians between the two roll axes, facing aside.
    u32 rowOff[3] = {};    ///< Per basis row.
    u32 rowNegated[3] = {}; ///< Per basis row: exactly the negation instead.
};

f32 RowError(const Vector3f& a, const Vector3f& b) {
    const Vector3f d = a - b;
    return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}

/// Runs @p c on @p flags over every camera and pose. The facing axis is local
/// +Y in StarCraft II (-X in Warcraft III), compared on its own for the free
/// billboard whose roll the two engines choose differently.
Verdict Run(u32 flags, const Candidate& c, bool includeTurnedParent) {
    constexpr f32 kTol = 2e-4f;
    Verdict v;
    for (const Pose& pose : PoseGrid()) {
        if (!includeTurnedParent && pose.name == "child, turned parent")
            continue;
        Matrix44f stack = pose.parentRot;
        SetRow(stack, 3, pose.pivot);
        for (const Camera& cam : CameraGrid()) {
            const Matrix44f wc3 = MdxBillboardNodeMatrix(stack, flags, cam.eye, {0, 0, 0});
            const Matrix44f want = Restate(wc3);

            m3::BillboardBehavior record;
            record.billboardType = c.type;
            record.cameraLookAt = c.lookAt;
            record.up = c.up;
            record.forward = Quaternion{0, 0, 0, 1};
            Matrix44f bone = Restate(stack);
            Matrix44f parentWorld = Restate(pose.parentRot);
            const Quaternion identity{0, 0, 0, 1};
            const bool applied = M3ApplyBillboard(record, Sc2Camera(cam),
                                                  pose.root ? &identity : nullptr, {1, 1, 1},
                                                  pose.root ? nullptr : &parentWorld, bone);
            ++v.cases;
            bool finite = true;
            for (i32 r = 0; r < 3; ++r) {
                for (i32 k = 0; k < 3; ++k) {
                    finite = finite && std::isfinite(bone.data[r][k]) &&
                             std::isfinite(want.data[r][k]);
                }
            }
            if (!applied || !finite) {
                ++v.nonFinite;
                ++v.rowsOff;
                ++v.facingOff;
                continue;
            }
            f32 worst = 0.0f;
            for (i32 r = 0; r < 3; ++r) {
                const f32 e = RowError(Row(bone, r), Row(want, r));
                worst = std::max(worst, e);
                v.rowOff[r] += e > kTol ? 1u : 0u;
                v.rowNegated[r] += RowError(Row(bone, r), Row(want, r) * -1.0f) <= kTol ? 1u : 0u;
            }
            v.worstRow = std::max(v.worstRow, worst);
            if (worst > kTol)
                ++v.rowsOff;
            if (RowError(Row(bone, 1), Row(want, 1)) > kTol)
                ++v.facingOff;
            const f32 cosRoll = std::clamp(Row(bone, 0).dot(Row(want, 0)), -1.0f, 1.0f);
            v.worstRoll = std::max(v.worstRoll, std::acos(cosRoll));
        }
    }
    return v;
}

} // namespace

TEST_CASE("E1 a free billboard faces the eye the way Warcraft III does",
          "[wc3_sc2][equivalence][billboard]") {
    const Verdict chosen = Run(kBillboarded, Candidate{6, 1}, true);
    INFO("cases " << chosen.cases << ", worst roll " << chosen.worstRoll << " rad");
    CHECK(chosen.nonFinite == 0u);
    CHECK(chosen.facingOff == 0u);
    // The roll is the declared difference (§4.2): Warcraft III keeps the
    // node's own Y, StarCraft II the camera's up. It must at least exist on
    // this grid, or the grid never pitched the camera off an axis.
    CHECK(chosen.worstRoll > 0.05f);

    // Rejected: the view direction instead of the eye (Blizzard's lookAt 0).
    // An off-centre bone sees the eye from a different angle than the camera
    // looks, so the facing parts company.
    const Verdict viewDirection = Run(kBillboarded, Candidate{6, 0}, true);
    CHECK(viewDirection.facingOff > 0u);
}

TEST_CASE("E1 a billboard locked to Z is a bone locked to model Z",
          "[wc3_sc2][equivalence][billboard]") {
    // Warcraft III locks WORLD up whatever turns above the node, so the turned
    // parent is inside the claim.
    const Verdict chosen = Run(kLockZ, Candidate{2, 1}, true);
    INFO("worst row " << chosen.worstRow);
    CHECK(chosen.nonFinite == 0u);
    CHECK(chosen.rowsOff == 0u);

    const Verdict viewDirection = Run(kLockZ, Candidate{2, 0}, true);
    CHECK(viewDirection.rowsOff > 0u);
}

TEST_CASE("E1 a billboard locked to Y is a bone locked to model X",
          "[wc3_sc2][equivalence][billboard]") {
    // Warcraft III's Y is StarCraft II's X: type 0, no pre-rotation.
    const Verdict chosen = Run(kLockY, Candidate{0, 1}, false);
    INFO("worst row " << chosen.worstRow);
    CHECK(chosen.nonFinite == 0u);
    CHECK(chosen.rowsOff == 0u);

    // Rejected: type 1 by the axis's name (Blizzard's LockY -> 1).
    CHECK(Run(kLockY, Candidate{1, 1}, false).rowsOff > 0u);

    // Warcraft III locks the node's OWN current Y; the bone locks the model's.
    // Under a turned parent they differ, and the export says so.
    CHECK(Run(kLockY, Candidate{0, 1}, true).rowsOff > 0u);
}

TEST_CASE("E1 a billboard locked to X is a bone locked to model Y, mirrored in X",
          "[wc3_sc2][equivalence][billboard]") {
    // Warcraft III's X is StarCraft II's -Y, so the lock is type 1. But the
    // frame the Warcraft III walk builds for a lock on X is LEFT-handed --
    // `yp = xp x zp` makes `xp x yp == -zp` -- and no rotation, which is all a
    // bone can hold, reproduces a reflection. The closest a bone gets keeps
    // the two axes that face and stand (Y and Z) and turns the locked one
    // around: type 1 with no pre-rotation. Every other pre-rotation
    // negates a facing or a standing axis instead, and type 0 is wrong outright.
    const Verdict chosen = Run(kLockX, Candidate{1, 1}, false);
    INFO("worst row " << chosen.worstRow);
    CHECK(chosen.nonFinite == 0u);
    CHECK(chosen.rowOff[1] == 0u);
    CHECK(chosen.rowOff[2] == 0u);
    CHECK(chosen.rowNegated[0] == chosen.cases);

    // Rejected: the half turn about Z, which negates the facing axis (the quad
    // turns its back on the eye), and type 0 by the axis's name.
    const Verdict halfTurn = Run(kLockX, Candidate{1, 1, Quaternion{0, 0, 1, 0}}, false);
    CHECK(halfTurn.rowNegated[1] == halfTurn.cases);
    CHECK(Run(kLockX, Candidate{0, 1}, false).rowOff[1] > 0u);
}

// ============================================================================
// E2, particles. Warcraft III's spawn shape and integrator against StarCraft
// II's samplers and closed form, over many draws: two games' random streams
// never land the same particle, so each row compares a DISTRIBUTION, and each
// names the candidate it rejects. The export's frame is the claim under test
// throughout (§2.4): Warcraft III spawns in `Rz(90) * node`, so its spawn +X is
// the node's +Y, which is StarCraft II's bone +X.
// ============================================================================

namespace {

namespace part = whiteout::flakes::renderer::particle;
namespace sc2r = whiteout::flakes::renderer::sc2;

constexpr f32 kPiF = 3.14159265358979323846f;
constexpr int kDraws = 20000;

/// Kolmogorov-Smirnov distance between two samples.
f32 Ks(std::vector<f32> a, std::vector<f32> b) {
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    std::size_t i = 0, j = 0;
    f32 worst = 0.0f;
    while (i < a.size() && j < b.size()) {
        const f32 x = std::min(a[i], b[j]);
        while (i < a.size() && a[i] <= x)
            ++i;
        while (j < b.size() && b[j] <= x)
            ++j;
        const f32 fa = static_cast<f32>(i) / static_cast<f32>(a.size());
        const f32 fb = static_cast<f32>(j) / static_cast<f32>(b.size());
        worst = std::max(worst, std::fabs(fa - fb));
    }
    return worst;
}

f32 PolarOf(const Vector3f& v) {
    const f32 len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return len > 0.0f ? std::acos(std::clamp(v.z / len, -1.0f, 1.0f)) : 0.0f;
}

/// Warcraft III's PE2 spawn: the plane shape, in its spawn frame, carried
/// through `Rz(90)` into node space and restated as StarCraft II bone space.
part::SpawnSample Wc3Spawn(part::RndSeed& rnd, f32 latDeg, bool line, f32 width, f32 length) {
    part::SpawnParams params;
    params.width = width;
    params.height = length;
    params.latitude = latDeg * kPiF / 180.0f;
    params.longitude = line ? 0.0f : 2.0f * kPiF;
    params.speed.base = 1.0f;
    params.speed.variance = 0.0f;
    part::SpawnSample s;
    part::PlaneShape{}.Sample(s, params, rnd);
    const Matrix44f frame = Matrix44f::rotation_z(1.5707963267948966f);
    s.localPos = U(whiteout::transform_point(s.localPos, frame));
    s.localVel = U(whiteout::transform_normal(s.localVel, frame));
    return s;
}

std::vector<f32> Wc3Polars(u32 seed, f32 latDeg) {
    part::RndSeed rnd(seed);
    std::vector<f32> out;
    for (int i = 0; i < kDraws; ++i)
        out.push_back(PolarOf(Wc3Spawn(rnd, latDeg, false, 0.0f, 0.0f).localVel));
    return out;
}

// The spawn position and mesh normal the velocity types drawn here never read.
constexpr Vector3f kAtOrigin{0.0f, 0.0f, 0.0f};
constexpr Vector3f kUp{0.0f, 0.0f, 1.0f};

std::vector<f32> Sc2Polars(u32 seed, f32 horizontal, f32 vertical) {
    sc2r::Rng rng(seed, seed * 2654435761u);
    part::Sc2SpawnVelInputs in;
    in.velocityType = 0;
    in.spawnHorizontal = horizontal;
    in.spawnVertical = vertical;
    in.speed = 1.0f;
    in.speedRandom = 1.0f;
    std::vector<f32> out;
    for (int i = 0; i < kDraws; ++i)
        out.push_back(PolarOf(part::Sc2SampleSpawnVelocity(rng, in, kAtOrigin, kUp)));
    return out;
}

} // namespace

TEST_CASE("E2 the cone: a square spread of lat/sqrt(2) a side is the round cone's nearest",
          "[wc3_sc2][equivalence][particles]") {
    for (const f32 lat : {10.0f, 30.0f, 47.0f, 75.0f}) {
        const f32 rad = lat * kPiF / 180.0f;
        const std::vector<f32> wc3 = Wc3Polars(0x1234u, lat);
        const f32 floor = Ks(wc3, Wc3Polars(0x9876u, lat));
        const f32 chosen = Ks(wc3, Sc2Polars(7u, rad / std::sqrt(2.0f), rad / std::sqrt(2.0f)));
        const f32 full = Ks(wc3, Sc2Polars(7u, rad, rad));
        const f32 half = Ks(wc3, Sc2Polars(7u, rad * 0.5f, rad * 0.5f));
        INFO("lat " << lat << ": noise " << floor << ", lat/sqrt2 " << chosen << ", lat " << full
                    << ", lat/2 " << half);
        CHECK(chosen < full);
        CHECK(chosen < half);
        // The grid separates the candidates: each loser is further off than
        // two seeds of Warcraft III itself are from each other, by a margin.
        CHECK(full - chosen > 4.0f * floor);
        CHECK(half - chosen > 4.0f * floor);
    }
}

TEST_CASE("E2 a line emitter's fan is the cone with no vertical spread",
          "[wc3_sc2][equivalence][particles]") {
    const f32 lat = 35.0f;
    const f32 rad = lat * kPiF / 180.0f;
    part::RndSeed rnd(0xBEEFu);
    sc2r::Rng rng(11u, 0x01020304u);
    part::Sc2SpawnVelInputs in;
    in.spawnHorizontal = rad;
    in.spawnVertical = 0.0f;
    in.speed = 1.0f;
    in.speedRandom = 1.0f;
    std::vector<f32> wc3Fan, sc2Fan;
    f32 wc3Off = 0.0f, sc2Off = 0.0f;
    for (int i = 0; i < kDraws; ++i) {
        const Vector3f w = Wc3Spawn(rnd, lat, true, 0.0f, 0.0f).localVel;
        const Vector3f s = part::Sc2SampleSpawnVelocity(rng, in, kAtOrigin, kUp);
        // The fan lies in the X-Z plane of the bone in BOTH -- the frame claim.
        wc3Off = std::max(wc3Off, std::fabs(w.y));
        sc2Off = std::max(sc2Off, std::fabs(s.y));
        wc3Fan.push_back(std::atan2(w.x, w.z));
        sc2Fan.push_back(std::atan2(-s.x, s.z));
    }
    CHECK(wc3Off < 1e-5f);
    CHECK(sc2Off < 1e-5f);
    const f32 ks = Ks(wc3Fan, sc2Fan);
    INFO("fan KS " << ks);
    CHECK(ks < 0.02f);
    // Rejected: Blizzard's half-width.
    sc2r::Rng rng2(11u, 0x01020304u);
    in.spawnHorizontal = rad * 0.5f;
    std::vector<f32> halfFan;
    for (int i = 0; i < kDraws; ++i) {
        const Vector3f s = part::Sc2SampleSpawnVelocity(rng2, in, kAtOrigin, kUp);
        halfFan.push_back(std::atan2(-s.x, s.z));
    }
    CHECK(Ks(wc3Fan, halfFan) > 0.2f);
}

TEST_CASE("E2 the spawn plane lays its width across the bone's X", "[wc3_sc2][equivalence][particles]") {
    const f32 width = 40.0f, length = 20.0f;
    part::RndSeed rnd(0xC0FFEEu);
    sc2r::Rng rng(5u, 0x0A0B0C0Du);
    part::Sc2SpawnPosInputs in;
    in.shape = part::Sc2SpawnShape::Plane;
    in.shapeOuter = {width * kLengthScale, length * kLengthScale, 0.0f};
    Vector3f wMin{1e9f, 1e9f, 1e9f}, wMax{-1e9f, -1e9f, -1e9f};
    Vector3f sMin = wMin, sMax = wMax;
    const auto grow = [](Vector3f& lo, Vector3f& hi, const Vector3f& p) {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    };
    u32 nonFinite = 0;
    for (int i = 0; i < kDraws; ++i) {
        const Vector3f w = Wc3Spawn(rnd, 0.0f, false, width, length).localPos * kLengthScale;
        const Vector3f s = part::Sc2SampleSpawnPosition(rng, in);
        nonFinite += (std::isfinite(w.x) && std::isfinite(s.x)) ? 0u : 1u;
        grow(wMin, wMax, w);
        grow(sMin, sMax, s);
    }
    CHECK(nonFinite == 0u);
    // Extents, not finiteness: the width is across X, the length along Y.
    CHECK(wMax.x - wMin.x == Catch::Approx(sMax.x - sMin.x).epsilon(0.05));
    CHECK(wMax.y - wMin.y == Catch::Approx(sMax.y - sMin.y).epsilon(0.05));
    CHECK(sMax.x - sMin.x == Catch::Approx(width * kLengthScale).epsilon(0.05));
    CHECK(sMax.y - sMin.y == Catch::Approx(length * kLengthScale).epsilon(0.05));
    // Rejected: width and length swapped -- what a wrong spawn frame gives.
    CHECK(std::fabs((wMax.x - wMin.x) - (sMax.y - sMin.y)) > 0.1f);
}

TEST_CASE("E2 a particle falls as far under the crossed gravity", "[wc3_sc2][equivalence][particles]") {
    const f32 gravity = 50.0f; // Warcraft III units / s^2
    part::Particle2 p;
    p.position = {0, 0, 0};
    p.velocity = {0, 0, 30.0f};
    part::MotionParams m;
    m.gravity = {0.0f, 0.0f, -gravity};
    for (int i = 0; i < 90; ++i)
        part::IntegrateWc3(p, m, 1.0f / 60.0f);

    const auto fall = [&](f32 parGravity) {
        part::Sc2AnalyticInputs in;
        in.velocity0 = {0.0f, 0.0f, 30.0f * kLengthScale};
        in.birthTime = 0.0f;
        in.deathTime = 10.0f;
        in.systemTime = 1.5f;
        // The drag floor, as the builder writes it for an authored 0.
        in.drag = 0.01f;
        in.invDrag = 100.0f;
        // The lane as the vertex builder writes it: `worldGravityScale *
        // gravity`, the scale 1 in a game (R1). The shader negates it into a
        // helper that subtracts, so a negative `PAR_.gravity` falls.
        in.gravityZ = parGravity;
        return part::Sc2StepAnalytic(in).position.z / kLengthScale;
    };
    const f32 chosen = fall(-gravity * kLengthScale);
    INFO("wc3 " << p.position.z << ", crossed " << chosen);
    CHECK(chosen == Catch::Approx(p.position.z).epsilon(0.02));
    // Rejected: Blizzard's -0.02 g, twice the fall.
    CHECK(std::fabs(fall(-2.0f * gravity * kLengthScale) - p.position.z) > 5.0f);
}

TEST_CASE("E2 a sprite sheet runs its cells over the same ages, up to StarCraft II's rounding",
          "[wc3_sc2][equivalence][particles]") {
    // A 4x4 sheet: life 0..7 over the first 40% of the life, decay 8..15 after.
    part::CellAnimTrack wc3;
    wc3.SetBias(0.005f);
    wc3.AddSegment(0.4f, 0, 7, 1);
    wc3.AddSegment(1.0f, 8, 15, 1);

    const auto sc2Cell = [](f32 age, const std::array<f32, 3>& frames, f32 mid) {
        part::Sc2QuadInput v;
        part::Sc2QuadBatch b;
        b.flipbookMidKeyTime = mid;
        b.flipbookColumns = 4.0f;
        b.flipbookFrames = frames;
        b.cellSize = {0.25f, 0.25f};
        part::Sc2QuadFlags fl;
        fl.flipbookUv = true;
        const i16 corner[2] = {-1, 1};
        const Vector2f uv = part::Sc2ParticleUv(v, corner, age, b, fl);
        const int x = static_cast<int>(std::floor(uv.x / 0.25f + 1e-3f));
        const int y = static_cast<int>(std::floor(uv.y / 0.25f + 1e-3f));
        return y * 4 + x;
    };
    const auto mismatch = [&](const std::array<f32, 3>& frames) {
        int off = 0, n = 0;
        for (f32 age = 0.0005f; age < 1.0f; age += 0.001f, ++n)
            off += sc2Cell(age, frames, 0.4f) != wc3.Evaluate(age) ? 1 : 0;
        return static_cast<f32>(off) / static_cast<f32>(n);
    };
    // StarCraft II has one run's stop where Warcraft III has a life end AND a
    // decay start, and rounds a cell where Warcraft III floors it, so part of
    // every run is half a cell early whatever the triple. Candidates: the life
    // end as the stop, the decay start as the stop, and Blizzard's one-low.
    const f32 lifeEnd = mismatch({0.0f, 7.0f, 15.0f});
    const f32 decayStart = mismatch({0.0f, 8.0f, 15.0f});
    const f32 oneLow = mismatch({0.0f, 6.0f, 14.0f});
    INFO("mismatch life end " << lifeEnd << ", decay start " << decayStart << ", one low "
                              << oneLow);
    // Chosen: the decay start as the stop, so the end run begins where
    // Warcraft III's decay does.
    CHECK(decayStart < lifeEnd);
    CHECK(decayStart < oneLow);
    CHECK(decayStart < 0.5f);
}

TEST_CASE("E2 a tail is as long as Warcraft III's velocity times its time",
          "[wc3_sc2][equivalence][particles]") {
    const f32 scale = 25.0f;    // the Warcraft III mid scale (half extent)
    const f32 tailTime = 0.5f;  // seconds
    const f32 speedWc3 = 300.0f;
    const auto length = [&](f32 tailLength) {
        part::Sc2QuadInput v;
        // The size lanes: the key (full width 2 s L) halved, as u16 * 1/256.
        const f32 half = scale * kLengthScale;
        v.size = {half * 256.0f, half * 256.0f, half * 256.0f, 256.0f};
        v.velocity = {speedWc3 * kLengthScale, 0.0f, 0.0f};
        v.instanceVec = {tailLength, 0.0f, 0.0f};
        v.deathTime = 1.0f;
        part::Sc2QuadBatch b;
        part::Sc2QuadCamera cam;
        cam.direction = {0.0f, 1.0f, 0.0f};
        part::Sc2QuadFlags fl;
        fl.instanceType = 10; // Trail
        const part::Sc2QuadResult q = part::Sc2ExpandQuad(v, b, cam, fl);
        f32 lo = 1e9f, hi = -1e9f;
        for (const auto& c : q.corner) {
            lo = std::min(lo, c.position.x);
            hi = std::max(hi, c.position.x);
        }
        return (hi - lo) / kLengthScale;
    };
    const f32 key = 2.0f * scale * kLengthScale;
    const f32 chosen = length(tailTime / key);
    INFO("wc3 " << speedWc3 * tailTime << ", crossed " << chosen);
    CHECK(chosen == Catch::Approx(speedWc3 * tailTime).epsilon(0.01));
    // Rejected: Blizzard's doubled tail.
    CHECK(std::fabs(length(2.0f * tailTime / key) - speedWc3 * tailTime) > 50.0f);
}

TEST_CASE("E2 a cone past 120 degrees is nearest StarCraft II's random sphere",
          "[wc3_sc2][equivalence][particles]") {
    // Warcraft III's polar angle is uniform over [0, lat], which is not uniform
    // on the sphere; past a third of a turn the square spread folds over itself
    // and the uniform sphere (velocity type 3) is the closer shape. 20% of
    // shipped emitters are 90 or wider, 411 of them exactly 180 (C0.5).
    const auto sphere = [](u32 seed) {
        sc2r::Rng rng(seed, seed * 2654435761u);
        part::Sc2SpawnVelInputs in;
        in.velocityType = 3;
        in.speed = 1.0f;
        in.speedRandom = 1.0f;
        std::vector<f32> out;
        for (int i = 0; i < kDraws; ++i)
            out.push_back(PolarOf(part::Sc2SampleSpawnVelocity(rng, in, kAtOrigin, kUp)));
        return out;
    };
    for (const f32 lat : {90.0f, 150.0f, 180.0f}) {
        const f32 rad = lat * kPiF / 180.0f;
        const std::vector<f32> wc3 = Wc3Polars(0x1234u, lat);
        const f32 side = std::min(rad / std::sqrt(2.0f), kPiF);
        const f32 cone = Ks(wc3, Sc2Polars(7u, side, side));
        const f32 random = Ks(wc3, sphere(7u));
        INFO("lat " << lat << ": cone " << cone << ", sphere " << random);
        if (lat <= 120.0f)
            CHECK(cone < random);
        else
            CHECK(random < cone);
    }
}

TEST_CASE("E2 a spawned model stands unturned at its size, as Warcraft III stamps it",
          "[wc3_sc2][equivalence][particles]") {
    // Warcraft III places a PREM's model at the particle, turned no way and
    // at scale 1 (`ChildModelEmitter::TransformFor`). The crossing writes a
    // world-space model particle facing world -Y with a size key of 1 (C8.1).
    const auto pose = [](u32 instanceType, const Vector3f& angle) {
        part::Sc2ModelPoseInputs in;
        in.position = {1.5f, -2.0f, 0.75f};
        in.velocity = {3.0f, 1.0f, -4.0f};
        in.birthTime = 0.0f;
        in.deathTime = 2.0f;
        in.emitterTime = 0.7f;
        in.additionalFlags = 0x8u;
        in.rotationFlags = 0x6u;
        in.instanceType = instanceType;
        in.instanceAngle = angle;
        in.sizeKeys = {1.0f, 1.0f, 1.0f};
        // `rotationFlags & 4`, which the crossing's rests set: the element's own
        // keys, stamped at spawn as size x 256.
        in.elementSize = {256, 256, 256};
        in.colorKeys = {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu};
        in.midTime = {0.996f, 0.996f, 0.996f, 0.996f};
        // A camera looking down and across, so a camera-facing pose is not the
        // identity by accident.
        in.camera = {Vector3f{0.8f, 0.6f, 0.0f}, Vector3f{-0.42f, 0.56f, -0.71f},
                     Vector3f{-0.42f, 0.56f, 0.71f}};
        return part::Sc2ModelParticlePose(in);
    };
    const auto turn = [](const std::array<f32, 4>& q) {
        return std::acos(std::min(std::fabs(q[3]), 1.0f)) * 2.0f;
    };
    const part::Sc2ModelPose chosen = pose(3, {0.0f, -1.0f, 0.0f});
    INFO("chosen turn " << turn(chosen.rotation) << ", scale " << chosen.scale.x);
    CHECK(turn(chosen.rotation) < 2e-3f);
    CHECK(chosen.scale.x == Catch::Approx(1.0f));
    CHECK(chosen.scale.z == Catch::Approx(1.0f));
    CHECK(chosen.position.x == Catch::Approx(1.5f));
    // Rejected: the rest's +Z angle, and a camera-facing model.
    CHECK(turn(pose(3, {0.0f, 0.0f, 1.0f}).rotation) > 1.0f);
    CHECK(turn(pose(0, {0.0f, -1.0f, 0.0f}).rotation) > 0.5f);
}

// ============================================================================
// E3, ribbons. Warcraft III's edge emitter against StarCraft II's segment
// emitter as the crossing writes its `RIB_` (§4.5), on one moving, turning
// node. StarCraft II's strip is read node by node and compared with Warcraft
// III's strip at the same age: the width axis, the width, the centre (which
// carries the path and the fall) and, off centre, the two edges. Positions are
// Warcraft III units in the export's basis: the StarCraft II emitter runs on
// the restated bone times the world's x100, as the adapter hands it over.
// ============================================================================

namespace {

namespace rib = whiteout::flakes::renderer::ribbon;
namespace fx = whiteout::flakes::renderer::effects;

constexpr f32 kFrame = 1.0f / 60.0f;

/// What the crossing writes for one RIBB, and the candidates a row rejects.
struct RibbonCase {
    f32 above = 20.0f, below = 20.0f;
    f32 rate = 30.0f, lifespan = 0.6f;
    f32 gravity = 0.0f;        ///< Warcraft III units / s^2.
    f32 gravityFactor = -2.0f; ///< RIB_ gravity = factor * g * L.
    u32 flags = 0xC800u;
    bool offsetBone = true;      ///< The helper bone at (hA - hB) / 2 across the width.
    f32 divisionsPerRate = 0.0f; ///< 0: rate x lifetime; otherwise rate x this.
    bool floorLifetime = true;
    Matrix44f boneTurn = Matrix44f::identity(); ///< A rotation on the bone, bone-local rows.
};

/// A node that circles in a tilted plane and spins about a tilted axis.
struct NodePath {
    Vector3f axis{0.3f, 0.5f, 0.8f};
    f32 angle0 = 0.9f;
    f32 spin = 0.7f; ///< Radians a second.
    Vector3f centre{40.0f, -25.0f, 120.0f};
    f32 radius = 150.0f;
    f32 angularSpeed = 1.3f;

    Matrix44f At(f32 t) const {
        Matrix44f m = AxisAngleRows(axis, angle0 + spin * t);
        const f32 a = angularSpeed * t;
        SetRow(m, 3,
               centre + Vector3f{radius * std::cos(a), radius * std::sin(a) * 0.8f,
                                 radius * std::sin(a) * 0.3f + 20.0f * std::sin(2.0f * a)});
        return m;
    }
};

/// The bone as the adapter hands it over: the restated node times the world's
/// x100, so translations come back in Warcraft III units.
Matrix44f Sc2World(const Matrix44f& wc3Node, const Matrix44f& boneTurn) {
    Matrix44f bone = boneTurn * Restate(wc3Node);
    for (i32 r = 0; r < 4; ++r)
        SetRow(bone, r, Row(bone, r) * (1.0f / kLengthScale));
    return bone;
}

struct StripNode {
    Vector3f top, bot;
    f32 age = 0.0f;
};

struct RibbonRun {
    std::vector<StripNode> wc3; ///< Oldest first, restated.
    std::vector<StripNode> sc2; ///< Oldest first.
    std::size_t sc2Segments = 0;
    std::size_t wc3Edges = 0;
    f32 sc2Lifetime = 0.0f;
};

RibbonRun RunRibbon(const NodePath& path, const RibbonCase& rc, f32 seconds) {
    constexpr f32 L = kLengthScale;
    fx::RibbonEmitterConfig wcfg;
    wcfg.emission = rc.rate;
    wcfg.life = rc.lifespan;
    wcfg.gravity = rc.gravity;
    rib::RibbonDesc wdesc = rib::DescFromWc3Config(wcfg);
    wdesc.behavior = rib::RibbonBehavior::Wc3(); // behaviour rides on the desc
    rib::RibbonEmitter wc3(wdesc);

    // §4.5 rows 1, 4, 5, 8 and 13, as `RibbonCrossing` writes them.
    const f32 lifetime = rc.floorLifetime ? std::max(rc.lifespan, 0.25f) : rc.lifespan;
    fx::Sc2RibbonEmitterConfig scfg;
    scfg.ribbonType = 1;
    scfg.cullMethod = 0;
    scfg.flags = rc.flags;
    scfg.additionalFlags = 0x8u;
    scfg.divisions = rc.rate * (rc.divisionsPerRate > 0.0f ? rc.divisionsPerRate : lifetime);
    scfg.drag = 0.0f;
    scfg.mass = 1.0f;
    scfg.gravity3 = {0.0f, 0.0f, rc.gravityFactor * rc.gravity * L};
    for (f32& m : scfg.midTime)
        m = 0.996f;
    scfg.lifetimeInit = lifetime;
    rib::RibbonEmitter sc2(rib::DescFromSc2Config(scfg));
    const f32 size = 2.0f * (rc.above + rc.below) * L;
    // Row 2: the helper node sits across the width, in Warcraft III node space
    // (its +Y), and is restated like any other node.
    Matrix44f helper = Matrix44f::identity();
    if (rc.offsetBone)
        SetRow(helper, 3, Vector3f{0.0f, (rc.above - rc.below) * 0.5f, 0.0f});

    const int frames = static_cast<int>(seconds / kFrame);
    for (int f = 0; f < frames; ++f) {
        const Matrix44f node = path.At(static_cast<f32>(f) * kFrame);
        rib::RibbonState ws;
        ws.transform = node;
        ws.above = rc.above;
        ws.below = rc.below;
        wc3.SetState(ws);
        wc3.Update(kFrame);

        rib::RibbonState ss;
        ss.transform = Sc2World(helper * node, rc.boneTurn);
        ss.unitScale = 1.0f / L;
        ss.sc2.lifetime = lifetime;
        ss.sc2.size3 = {size, size, size};
        sc2.SetState(ss);
        sc2.Update(kFrame);
    }

    RibbonRun run;
    for (const rib::RibbonElement& e : wc3.Edges())
        run.wc3.push_back({U(e.top), U(e.bot), e.age});
    run.wc3Edges = wc3.Edges().size();
    run.sc2Segments = sc2.Edges().size();
    run.sc2Lifetime = lifetime;

    std::vector<renderer::Vertex> verts;
    std::vector<rib::RibbonDrawList> draws;
    sc2.BuildStage(rib::RibbonBuildContext{}, verts, draws);
    // Quads of (a.top, a.bot, b.top, a.bot, b.bot, b.top), V the age fraction.
    const std::size_t quads = verts.size() / 6;
    for (std::size_t q = 0; q < quads; ++q) {
        const renderer::Vertex* v = &verts[q * 6];
        run.sc2.push_back({v[0].position, v[1].position, v[0].uv.y * lifetime});
        if (q + 1 == quads)
            run.sc2.push_back({v[5].position, v[4].position, v[5].uv.y * lifetime});
    }
    return run;
}

struct StripVerdict {
    u32 compared = 0;
    u32 nonFinite = 0;
    f32 worstAxis = 0.0f;   ///< Radians between the two width axes.
    f32 worstWidth = 0.0f;  ///< |width ratio - 1|.
    f32 worstCentre = 0.0f; ///< Units.
    f32 worstEdges = 0.0f;  ///< Units, the two edges as an unordered pair.
};

f32 Length(const Vector3f& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

/// Warcraft III's strip at @p age, lerped between the two edges that bracket
/// it. False past either end.
bool Wc3At(const std::vector<StripNode>& strip, f32 age, StripNode& out) {
    for (std::size_t i = 0; i + 1 < strip.size(); ++i) {
        const StripNode& older = strip[i];
        const StripNode& newer = strip[i + 1];
        if (age <= older.age && age >= newer.age && older.age > newer.age) {
            const f32 t = (older.age - age) / (older.age - newer.age);
            out.top = older.top + (newer.top - older.top) * t;
            out.bot = older.bot + (newer.bot - older.bot) * t;
            out.age = age;
            return true;
        }
    }
    return false;
}

StripVerdict Compare(const RibbonRun& run) {
    StripVerdict v;
    for (const StripNode& s : run.sc2) {
        // The live head and the saturated tail carry no age to join on.
        if (s.age < 0.02f || s.age > 0.9f * run.sc2Lifetime)
            continue;
        StripNode w;
        if (!Wc3At(run.wc3, s.age, w))
            continue;
        const Vector3f sw = s.top - s.bot;
        const Vector3f ww = w.top - w.bot;
        const f32 sl = Length(sw), wl = Length(ww);
        const f32 cosAxis = std::fabs(sw.x * ww.x + sw.y * ww.y + sw.z * ww.z) / (sl * wl);
        const f32 axis = std::acos(std::min(cosAxis, 1.0f));
        const f32 centre = Length((s.top + s.bot) * 0.5f - (w.top + w.bot) * 0.5f);
        const f32 edges = 0.5f * std::min(Length(s.top - w.top) + Length(s.bot - w.bot),
                                          Length(s.top - w.bot) + Length(s.bot - w.top));
        if (!std::isfinite(axis) || !std::isfinite(centre) || !std::isfinite(sl)) {
            ++v.nonFinite;
            continue;
        }
        ++v.compared;
        v.worstAxis = std::max(v.worstAxis, axis);
        v.worstWidth = std::max(v.worstWidth, std::fabs(sl / wl - 1.0f));
        v.worstCentre = std::max(v.worstCentre, centre);
        v.worstEdges = std::max(v.worstEdges, edges);
    }
    return v;
}

f32 OldestAge(const std::vector<StripNode>& strip) {
    f32 oldest = 0.0f;
    for (const StripNode& n : strip)
        oldest = std::max(oldest, n.age);
    return oldest;
}

} // namespace

TEST_CASE("E3 a crossed ribbon lays its width along the bone's X, the node's Y",
          "[wc3_sc2][equivalence][ribbons]") {
    // Row 14: the ribbon rides its node's own bone, unturned. Warcraft III lays
    // each edge along node +Y; the planar strip's width is tangent x up with up
    // the bone's +Y (techniques 0/1), or the element's up, the bone's -X (2-4):
    // the bone's X either way, and bone +X is node +Y.
    const NodePath path;
    const RibbonCase rc;
    const StripVerdict chosen = Compare(RunRibbon(path, rc, 3.0f));
    INFO("compared " << chosen.compared << ": axis " << chosen.worstAxis << " rad, width "
                     << chosen.worstWidth << ", centre " << chosen.worstCentre);
    CHECK(chosen.nonFinite == 0u);
    CHECK(chosen.compared >= 12u);
    CHECK(chosen.worstAxis < 0.05f);
    CHECK(chosen.worstWidth < 0.02f);
    CHECK(chosen.worstCentre < 2.0f);

    // Rejected: Blizzard's bone, turned +90 degrees about Z (226 of 370).
    RibbonCase turned = rc;
    turned.boneTurn = AxisAngleRows({0.0f, 0.0f, 1.0f}, 1.5707963f);
    const StripVerdict blizzard = Compare(RunRibbon(path, turned, 3.0f));
    INFO("turned: axis " << blizzard.worstAxis);
    CHECK(blizzard.worstAxis > 1.0f);
}

TEST_CASE("E3 a ribbon falls as far under the crossed gravity, and keeps its width only with "
          "accurate tangents",
          "[wc3_sc2][equivalence][ribbons]") {
    // Row 8 (R3): Warcraft III drops an edge g t^2 and RIB_ gravity is a raw
    // acceleration, so -2 g L. Under gravity the GPU technique's width is
    // tangent x up with the tangent turning down the fall, right only while the
    // bone's Z stands upright; accurate tangents (0x2000, technique 3) take the
    // element's up, the bone's X at birth, as Warcraft III's baked edge does.
    NodePath path;
    path.axis = {1.0f, 0.25f, 0.1f};
    path.angle0 = 1.1f;
    path.spin = 0.4f;
    RibbonCase rc;
    rc.gravity = 120.0f;
    rc.flags = 0xC800u | 0x2000u;
    const StripVerdict chosen = Compare(RunRibbon(path, rc, 3.0f));
    INFO("chosen: compared " << chosen.compared << ", axis " << chosen.worstAxis << ", width "
                             << chosen.worstWidth << ", centre " << chosen.worstCentre);
    CHECK(chosen.nonFinite == 0u);
    CHECK(chosen.compared >= 12u);
    CHECK(chosen.worstAxis < 0.05f);
    CHECK(chosen.worstWidth < 0.02f);
    CHECK(chosen.worstCentre < 2.0f);

    // Rejected: the GPU technique, whose width turns with the fall.
    RibbonCase gpu = rc;
    gpu.flags = 0xC800u;
    const StripVerdict gpuOnly = Compare(RunRibbon(path, gpu, 3.0f));
    INFO("technique 0: axis " << gpuOnly.worstAxis << ", centre " << gpuOnly.worstCentre);
    CHECK(gpuOnly.worstAxis > 0.3f);
    CHECK(gpuOnly.worstCentre < 2.0f);

    // Rejected: half the gravity, and Blizzard's -10 g.
    RibbonCase half = rc;
    half.gravityFactor = -1.0f;
    const StripVerdict halfFall = Compare(RunRibbon(path, half, 3.0f));
    RibbonCase tenfold = rc;
    tenfold.gravityFactor = -10.0f / kLengthScale;
    const StripVerdict blizzard = Compare(RunRibbon(path, tenfold, 3.0f));
    INFO("half: centre " << halfFall.worstCentre << "; -10 g: centre " << blizzard.worstCentre);
    CHECK(halfFall.worstCentre > 10.0f);
    CHECK(blizzard.worstCentre > 100.0f);
}

TEST_CASE("E3 an off-centre ribbon's edges land where Warcraft III's do on the offset bone",
          "[wc3_sc2][equivalence][ribbons]") {
    // Row 2: StarCraft II centres the strip on its bone; a helper bone half the
    // difference of the two heights across the width puts both edges back.
    const NodePath path;
    RibbonCase rc;
    rc.above = 35.0f;
    rc.below = 5.0f;
    const StripVerdict chosen = Compare(RunRibbon(path, rc, 3.0f));
    INFO("offset: compared " << chosen.compared << ", edges " << chosen.worstEdges << ", width "
                             << chosen.worstWidth);
    CHECK(chosen.nonFinite == 0u);
    CHECK(chosen.compared >= 12u);
    CHECK(chosen.worstEdges < 2.0f);
    CHECK(chosen.worstWidth < 0.02f);

    // Rejected: the strip left on the node's own bone.
    RibbonCase centred = rc;
    centred.offsetBone = false;
    const StripVerdict none = Compare(RunRibbon(path, centred, 3.0f));
    INFO("no offset: edges " << none.worstEdges);
    CHECK(none.worstEdges > 10.0f);
}

TEST_CASE("E3 a ribbon lays its edges at the authored rate and lives its floored life",
          "[wc3_sc2][equivalence][ribbons]") {
    // Rows 4-5: StarCraft II lays divisions / lifetime segments a second, so
    // divisions = rate x lifetime keeps Warcraft III's rate, and Warcraft III
    // floors a lifespan at a quarter second. Warcraft III also keeps a head edge
    // every frame (§6), so its own count runs ahead of the rate by the frame rate.
    const NodePath path;
    RibbonCase rc;
    rc.rate = 40.0f;
    rc.lifespan = 0.1f;
    const RibbonRun run = RunRibbon(path, rc, 2.0f);
    const f32 kept = rc.rate * 0.25f;
    INFO("segments " << run.sc2Segments << " for " << kept << "; Warcraft III edges "
                     << run.wc3Edges);
    CHECK(std::fabs(static_cast<f32>(run.sc2Segments) - kept) <= 2.0f);
    const f32 wc3Oldest = run.wc3.empty() ? 0.0f : run.wc3.front().age;
    const f32 sc2Oldest = OldestAge(run.sc2);
    INFO("oldest: wc3 " << wc3Oldest << ", sc2 " << sc2Oldest);
    CHECK(wc3Oldest == Catch::Approx(0.25f).margin(2.0f * kFrame));
    CHECK(sc2Oldest == Catch::Approx(0.25f).margin(2.0f * kFrame));

    // Rejected: divisions copied from the rate (Blizzard's), and the raw life.
    RibbonCase copied = rc;
    copied.divisionsPerRate = 1.0f;
    CHECK(std::fabs(static_cast<f32>(RunRibbon(path, copied, 2.0f).sc2Segments) - kept) > 10.0f);
    RibbonCase raw = rc;
    raw.floorLifetime = false;
    CHECK(std::fabs(OldestAge(RunRibbon(path, raw, 2.0f).sc2) - wc3Oldest) > 0.1f);
}
