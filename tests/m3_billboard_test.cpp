// ============================================================================
// `.m3` billboards (`BBSC`).
//
// StarCraft II resolves these at draw time, per view, from `sub_10290A890` —
// `CBBSolver::Solve` is a stub, which is why the chunk looked inert for so
// long. `CBBSolver::ApplyBillboard` (`0x1027F1F30`) is the real body: it builds
// a 3x3 whose rows are the bone's new local axes, converts it to a quaternion,
// composes the record's correction, and writes the bone's LOCAL rotation so the
// subtree follows.
//
// The seven modes are not seven variations on one idea. Types 0/1/2 lock a
// WORLD axis and aim an axis away from the eye; types 3/5 lock one of the
// bone's OWN current axes and aim an axis at it; type 4 does nothing at all.
// The tests below pin each of those separately, because a sign or a slot swap
// between them is exactly the kind of thing that still looks plausible.
// ============================================================================

#include "io/m3/m3_billboard.h"
#include "io/m3/m3_model_adapter.h"
#include "m3_anim_builders.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <whiteout/models/m3/parser.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace whiteout;
using namespace whiteout::flakes;
using namespace whiteout::flakes::io;
using Catch::Approx;

namespace {

constexpr f32 kRoot2 = 0.70710678f;

Vector3f Row(const Matrix44f& m, i32 r) {
    return {m.data[r][0], m.data[r][1], m.data[r][2]};
}

// The camera used by most cases: level, ten units down +X, looking at the
// origin with world +Z up. So the direction a billboard aims (`bone - eye`) is
// exactly -X, and world up survives into the frame unrotated.
M3CameraFrame LevelCamera() {
    const Vector3f eye{10.0f, 0.0f, 0.0f};
    return M3BuildCameraFrame(Matrix44f::identity(),
                              Matrix44f::look_at_rh(eye, {0, 0, 0}, {0, 0, 1}), eye);
}

m3::BillboardBehavior Record(u16 bone, u8 type, u8 lookAt = 1) {
    m3::BillboardBehavior bb;
    bb.boneIndex = bone;
    bb.billboardType = type;
    bb.cameraLookAt = lookAt;
    bb.up = Quaternion{0, 0, 0, 1};
    bb.forward = Quaternion{0, 0, 0, 1};
    return bb;
}

void RequireOrthonormal(const Matrix44f& m) {
    for (i32 r = 0; r < 3; ++r)
        REQUIRE(Row(m, r).length() == Approx(1.0f).margin(1e-4f));
    REQUIRE(Row(m, 0).dot(Row(m, 1)) == Approx(0.0f).margin(1e-4f));
    REQUIRE(Row(m, 1).dot(Row(m, 2)) == Approx(0.0f).margin(1e-4f));
    REQUIRE(Row(m, 0).dot(Row(m, 2)) == Approx(0.0f).margin(1e-4f));
}

void RequireVec(const Vector3f& got, const Vector3f& want) {
    REQUIRE(got.x == Approx(want.x).margin(1e-4f));
    REQUIRE(got.y == Approx(want.y).margin(1e-4f));
    REQUIRE(got.z == Approx(want.z).margin(1e-4f));
}

} // namespace

TEST_CASE("The camera frame is StarCraft II's camera rows, in model space", "[m3bb]") {
    const Vector3f eye{10.0f, 0.0f, 0.0f};
    const M3CameraFrame cam = LevelCamera();

    RequireVec(cam.position, eye);
    // `look_at_rh` keeps the basis in the view matrix's COLUMNS, and column 2
    // is BACKWARD — reading it as forward points every billboard the wrong way.
    RequireVec(cam.forward, {-1.0f, 0.0f, 0.0f});
    RequireVec(cam.up, {0.0f, 0.0f, 1.0f});
    // The engine's row 0. `right == cross(forward, up)` holds in both bases,
    // which is why no sign fix is needed on the way across.
    RequireVec(cam.axisX, whiteout::cross(cam.forward, cam.up));
}

TEST_CASE("The camera frame follows the actor's world transform", "[m3bb]") {
    // Yaw the actor 90 degrees about Z. Its own space then sees the camera
    // somewhere else entirely, and a billboard measured in model space has to
    // see it there too or it aims into the world instead of at the eye.
    const Vector3f eye{10.0f, 0.0f, 0.0f};
    Matrix44f world = M3RotationRows(Quaternion{0, 0, kRoot2, kRoot2});
    const M3CameraFrame cam =
        M3BuildCameraFrame(world, Matrix44f::look_at_rh(eye, {0, 0, 0}, {0, 0, 1}), eye);

    RequireVec(cam.position, {0.0f, -10.0f, 0.0f});
    RequireVec(cam.forward, {0.0f, 1.0f, 0.0f});
    RequireVec(cam.up, {0.0f, 0.0f, 1.0f});
}

TEST_CASE("Type 2 turns about world Z and aims local +Y away from the eye", "[m3bb]") {
    Matrix44f basis;
    REQUIRE(M3BillboardBasis(2, true, LevelCamera(), Matrix44f::identity(), basis));

    RequireOrthonormal(basis);
    // World Z is the locked axis: it is copied through untouched, not merely
    // left near vertical.
    RequireVec(Row(basis, 2), {0.0f, 0.0f, 1.0f});
    RequireVec(Row(basis, 1), {-1.0f, 0.0f, 0.0f});
    RequireVec(Row(basis, 0), {0.0f, 1.0f, 0.0f});
}

TEST_CASE("Type 2 stays upright when the camera is not", "[m3bb]") {
    // The whole point of the axis lock. A high camera tilts the aim direction,
    // and the locked row must not follow it.
    const Vector3f eye{10.0f, 0.0f, 40.0f};
    const M3CameraFrame cam = M3BuildCameraFrame(
        Matrix44f::identity(), Matrix44f::look_at_rh(eye, {0, 0, 0}, {0, 0, 1}), eye);

    Matrix44f basis;
    REQUIRE(M3BillboardBasis(2, true, cam, Matrix44f::identity(), basis));
    RequireOrthonormal(basis);
    RequireVec(Row(basis, 2), {0.0f, 0.0f, 1.0f});
    RequireVec(Row(basis, 1), {-1.0f, 0.0f, 0.0f});
}

TEST_CASE("Type 0 locks world X and type 1 locks world Y", "[m3bb]") {
    // Type 1 is the odd one: with Y pinned it is the only mode that aims local
    // +X, because +Y is the axis it may not move.
    const Vector3f eye{0.0f, 0.0f, 10.0f};
    const M3CameraFrame cam = M3BuildCameraFrame(
        Matrix44f::identity(), Matrix44f::look_at_rh(eye, {0, 0, 0}, {0, 1, 0}), eye);

    Matrix44f zero;
    REQUIRE(M3BillboardBasis(0, true, cam, Matrix44f::identity(), zero));
    RequireOrthonormal(zero);
    RequireVec(Row(zero, 0), {1.0f, 0.0f, 0.0f});
    RequireVec(Row(zero, 1), {0.0f, 0.0f, -1.0f});

    Matrix44f one;
    REQUIRE(M3BillboardBasis(1, true, cam, Matrix44f::identity(), one));
    RequireOrthonormal(one);
    RequireVec(Row(one, 1), {0.0f, 1.0f, 0.0f});
    RequireVec(Row(one, 0), {0.0f, 0.0f, -1.0f});
}

TEST_CASE("Type 6 aims local +Y and rolls with the camera's up", "[m3bb]") {
    // A camera up at 45 degrees, so the free mode's answer cannot coincide with
    // the world-Z lock's.
    const Vector3f eye{10.0f, 0.0f, 10.0f};
    const M3CameraFrame cam = M3BuildCameraFrame(
        Matrix44f::identity(), Matrix44f::look_at_rh(eye, {0, 0, 0}, {0, 0, 1}), eye);

    Matrix44f basis;
    REQUIRE(M3BillboardBasis(6, true, cam, Matrix44f::identity(), basis));
    RequireOrthonormal(basis);
    // Local +Y is the aim direction itself — no projection, unlike every locked
    // mode.
    RequireVec(Row(basis, 1), {-kRoot2, 0.0f, -kRoot2});
    // And local +X is perpendicular to the roll hint, which is what keeps a
    // sprite's "up" on screen where the camera's is.
    REQUIRE(Row(basis, 0).dot(cam.up) == Approx(0.0f).margin(1e-4f));
}

TEST_CASE("Type 4 is parsed and never applied", "[m3bb]") {
    // 54 shipped records ask for it and the engine returns immediately.
    Matrix44f basis;
    REQUIRE_FALSE(M3BillboardBasis(4, true, LevelCamera(), Matrix44f::identity(), basis));
}

TEST_CASE("Types 3 and 5 lock the bone's own axis, not a world one", "[m3bb]") {
    // A bone turned 90 degrees about world Y, so neither of the two axes these
    // modes lock is anywhere near the world's, nor parallel to the aim.
    const Matrix44f bone = M3RotationRows(Quaternion{0.0f, kRoot2, 0.0f, kRoot2});

    Matrix44f three;
    REQUIRE(M3BillboardBasis(3, true, LevelCamera(), bone, three));
    RequireOrthonormal(three);
    // The bone's current X survives in place...
    RequireVec(Row(three, 0), Row(bone, 0));
    // ...and it is local +Z that turns to face the eye. Note the sign: these
    // two modes face the camera where 0/1/2 face away from it.
    REQUIRE(Row(three, 2).dot(Vector3f{1.0f, 0.0f, 0.0f}) > 0.99f);

    Matrix44f five;
    REQUIRE(M3BillboardBasis(5, true, LevelCamera(), bone, five));
    RequireOrthonormal(five);
    // Type 5 takes the bone's own Y and files it under local Z — the same
    // triple as type 3, rotated one slot.
    RequireVec(Row(five, 2), Row(bone, 1));
    REQUIRE(Row(five, 1).dot(Vector3f{1.0f, 0.0f, 0.0f}) > 0.99f);
}

TEST_CASE("A lock axis pointing at the eye leaves the bone alone", "[m3bb]") {
    // `A x d` is the whole construction for types 3 and 5, so a bone whose
    // locked axis already lies along the aim has no basis to build. The engine
    // returns without touching it rather than falling back to something.
    const Matrix44f bone = Matrix44f::identity(); // X is +X, straight at the camera
    Matrix44f basis;
    REQUIRE_FALSE(M3BillboardBasis(3, true, LevelCamera(), bone, basis));
    // Its Y is across the view, though, so type 5 still has an answer.
    REQUIRE(M3BillboardBasis(5, true, LevelCamera(), bone, basis));
}

TEST_CASE("cameraLookAt decides whether bones share one orientation", "[m3bb]") {
    const M3CameraFrame cam = LevelCamera();
    Matrix44f here = Matrix44f::identity();
    Matrix44f there = Matrix44f::translation({0.0f, 20.0f, 0.0f});

    Matrix44f a, b;
    REQUIRE(M3BillboardBasis(6, true, cam, here, a));
    REQUIRE(M3BillboardBasis(6, true, cam, there, b));
    REQUIRE(Row(a, 1).dot(Row(b, 1)) < 0.99f); // each aims at the eye from where it is

    REQUIRE(M3BillboardBasis(6, false, cam, here, a));
    REQUIRE(M3BillboardBasis(6, false, cam, there, b));
    // Clear, and the aim is the camera's view direction — one orientation for
    // every bone in the model, wherever it stands.
    RequireVec(Row(a, 1), cam.forward);
    RequireVec(Row(b, 1), cam.forward);
}

TEST_CASE("Applying a billboard turns the bone in place", "[m3bb]") {
    // Scale 3 on a bone parked away from the origin. The engine only ever
    // replaces the LOCAL ROTATION, so neither the scale nor the position can
    // move — a billboard that resizes or teleports its bone is the classic
    // symptom of rebuilding the matrix instead of re-orienting it.
    Matrix44f bone = Matrix44f::identity();
    for (i32 r = 0; r < 3; ++r)
        bone.data[r][r] = 3.0f;
    bone.data[3][0] = 0.0f;
    bone.data[3][1] = 5.0f;
    bone.data[3][2] = 2.0f;

    REQUIRE(
        M3ApplyBillboard(Record(0, 2), LevelCamera(), nullptr, {3.0f, 3.0f, 3.0f}, nullptr, bone));

    for (i32 r = 0; r < 3; ++r)
        REQUIRE(Row(bone, r).length() == Approx(3.0f).margin(1e-4f));
    RequireVec(Row(bone, 3), {0.0f, 5.0f, 2.0f});
    RequireVec(Row(bone, 2) * (1.0f / 3.0f), {0.0f, 0.0f, 1.0f});
}

TEST_CASE("A child's billboard is a world orientation, not a local one", "[m3bb]") {
    // The engine writes the bone's LOCAL rotation, so the world basis it wants
    // has to be divided back through the parent's before it is stored and
    // multiplied through again on the way out. Skip the divide and the parent's
    // rotation lands on the bone twice — a billboard that swings with its
    // parent instead of ignoring it.
    const Matrix44f parent = M3RotationRows(Quaternion{0.0f, 0.0f, kRoot2, kRoot2});
    Matrix44f bone = parent;
    REQUIRE(
        M3ApplyBillboard(Record(0, 2), LevelCamera(), nullptr, {1.0f, 1.0f, 1.0f}, &parent, bone));

    // The same answer a parentless bone gets: a billboard does not care what it
    // hangs off.
    RequireVec(Row(bone, 1), {-1.0f, 0.0f, 0.0f});
    RequireVec(Row(bone, 2), {0.0f, 0.0f, 1.0f});
}

TEST_CASE("A mirrored bone stays mirrored", "[m3bb]") {
    // A negative scale component is how a piece of geometry gets flipped, and
    // it lives in the bone's local SCALE — which the engine never touches,
    // because all it writes is a rotation. Rebuilding the world rows from the
    // basis and their lengths silently undoes it and inverts every normal the
    // bone skins: the same class of bug `M2CameraBasis` normalises handedness
    // for. 121 of the corpus's billboarded bones are mirrored this way.
    Matrix44f bone = Matrix44f::identity();
    bone.data[1][1] = -1.0f;
    REQUIRE(
        M3ApplyBillboard(Record(0, 6), LevelCamera(), nullptr, {1.0f, -1.0f, 1.0f}, nullptr, bone));

    REQUIRE(Row(bone, 0).dot(whiteout::cross(Row(bone, 1), Row(bone, 2))) < 0.0f);
    for (i32 r = 0; r < 3; ++r)
        REQUIRE(Row(bone, r).length() == Approx(1.0f).margin(1e-4f));
}

TEST_CASE("A root bone keeps its animated spin, a child does not", "[m3bb]") {
    // The engine tests the parent NODE and only composes the sampled rotation
    // back on when the parent is not a bone. That is what lets a root-level
    // sprite spin in the screen plane while a child's animated rotation is
    // thrown away.
    const Quaternion spin{kRoot2, 0.0f, 0.0f, kRoot2}; // 90 degrees about X

    Matrix44f asChild = Matrix44f::identity();
    REQUIRE(M3ApplyBillboard(Record(0, 2), LevelCamera(), nullptr, {1.0f, 1.0f, 1.0f}, nullptr,
                             asChild));

    Matrix44f asRoot = Matrix44f::identity();
    REQUIRE(
        M3ApplyBillboard(Record(0, 2), LevelCamera(), &spin, {1.0f, 1.0f, 1.0f}, nullptr, asRoot));

    REQUIRE(Row(asChild, 1).dot(Row(asRoot, 1)) < 0.99f);
    // Composed on the LEFT: the spin turns the bone first, then the billboard
    // basis reorients the result.
    const Matrix44f want = M3RotationRows(spin) * asChild;
    for (i32 r = 0; r < 3; ++r)
        RequireVec(Row(asRoot, r), Row(want, r));
}

TEST_CASE("The correction quaternion belongs to the type, not to every record", "[m3bb]") {
    // Types 0/1/2 always take the first quaternion; type 6 takes the SECOND,
    // and only on a bone with a real parent. Swapping which is read leaves
    // 4437 shipped type-6 records rotated by a stranger's correction.
    const Quaternion tip{kRoot2, 0.0f, 0.0f, kRoot2};

    m3::BillboardBehavior locked = Record(0, 2);
    locked.up = tip;
    Matrix44f a = Matrix44f::identity(), plain = Matrix44f::identity();
    REQUIRE(M3ApplyBillboard(locked, LevelCamera(), nullptr, {1.0f, 1.0f, 1.0f}, nullptr, a));
    REQUIRE(
        M3ApplyBillboard(Record(0, 2), LevelCamera(), nullptr, {1.0f, 1.0f, 1.0f}, nullptr, plain));
    REQUIRE(Row(a, 1).dot(Row(plain, 1)) < 0.99f);

    m3::BillboardBehavior free = Record(0, 6);
    free.up = tip; // read by nothing at this type
    Matrix44f b = Matrix44f::identity(), freePlain = Matrix44f::identity();
    REQUIRE(M3ApplyBillboard(free, LevelCamera(), nullptr, {1.0f, 1.0f, 1.0f}, nullptr, b));
    REQUIRE(M3ApplyBillboard(Record(0, 6), LevelCamera(), nullptr, {1.0f, 1.0f, 1.0f}, nullptr,
                             freePlain));
    for (i32 r = 0; r < 3; ++r)
        RequireVec(Row(b, r), Row(freePlain, r));

    // ...and the second one is skipped on a root bone, where the sampled
    // rotation takes its place.
    m3::BillboardBehavior freeCorrected = Record(0, 6);
    freeCorrected.forward = tip;
    const Quaternion identity{0.0f, 0.0f, 0.0f, 1.0f};
    Matrix44f root = Matrix44f::identity(), child = Matrix44f::identity();
    REQUIRE(M3ApplyBillboard(freeCorrected, LevelCamera(), &identity, {1.0f, 1.0f, 1.0f}, nullptr,
                             root));
    REQUIRE(M3ApplyBillboard(freeCorrected, LevelCamera(), nullptr, {1.0f, 1.0f, 1.0f}, nullptr,
                             child));
    REQUIRE(Row(root, 1).dot(Row(child, 1)) < 0.99f);
}

TEST_CASE("Evaluate billboards the bone and carries its children", "[m3bb]") {
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    mb.StaticBone("child", 0, Vector3f{0.0f, 4.0f, 0.0f});
    m3::Model model = mb.Build();
    model.billboardBehaviors.push_back(Record(0, 2));
    M3ModelAdapter a(std::move(model));

    const Vector3f eye{10.0f, 0.0f, 0.0f};
    PoseRequest req;
    req.cameraPos = eye;
    req.view = Matrix44f::look_at_rh(eye, {0, 0, 0}, {0, 0, 1});
    const auto fs = a.Evaluate(req);

    REQUIRE(fs.boneWorldMatrices.size() == 2);
    RequireVec(Row(fs.boneWorldMatrices[0], 1), {-1.0f, 0.0f, 0.0f});
    // The child sits four units up its parent's +Y; once the parent turns to
    // face the eye that offset has to turn with it. A billboard applied as a
    // late per-bone fix-up would leave the child at (0, 4, 0).
    RequireVec(Row(fs.boneWorldMatrices[1], 3), {-4.0f, 0.0f, 0.0f});
}

TEST_CASE("A model with no BBSC chunk is untouched", "[m3bb]") {
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    mb.StaticBone("child", 0, Vector3f{0.0f, 4.0f, 0.0f});
    M3ModelAdapter a(mb.Build());

    const Vector3f eye{10.0f, 0.0f, 0.0f};
    PoseRequest req;
    req.cameraPos = eye;
    req.view = Matrix44f::look_at_rh(eye, {0, 0, 0}, {0, 0, 1});
    const auto fs = a.Evaluate(req);

    RequireVec(Row(fs.boneWorldMatrices[0], 1), {0.0f, 1.0f, 0.0f});
    RequireVec(Row(fs.boneWorldMatrices[1], 3), {0.0f, 4.0f, 0.0f});
}

// ---------------------------------------------------------------------------
// Corpus sweep
//
// The cases above pin the arithmetic against the decompile; this one asks
// whether it survives contact with what shipped. Each model is evaluated
// twice — once as parsed, once with the `BBSC` list emptied — so the two runs
// differ in exactly one thing, and then the billboarded bones are held to what
// a billboard is not allowed to do whichever of the seven modes it picked.
//
// The mirroring check is the one worth having. A basis assembled with a cross
// product the wrong way round is still orthonormal, still looks plausible in a
// still frame, and inverts every skinned normal underneath it — the failure
// `M2CameraBasis` has to normalise handedness for.
// ---------------------------------------------------------------------------

namespace {

namespace fs = std::filesystem;

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_SC2_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus");
}

std::size_t PerCorpusLimit() {
    if (const char* v = std::getenv("WDX_TEST_M3_BB_LIMIT"); v && *v)
        return static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
    return 150;
}

// Read the chunk table only. Nine models in ten carry no `BBSC` and parsing
// them to find that out would spend the whole budget on models that test
// nothing. Tags are stored reversed on disk, so `BBSC` is written `CSBB`.
bool HasBillboardChunk(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    char magic[4] = {};
    u32 indexOffset = 0, indexCount = 0;
    f.read(magic, 4);
    f.read(reinterpret_cast<char*>(&indexOffset), 4);
    f.read(reinterpret_cast<char*>(&indexCount), 4);
    if (!f || std::memcmp(magic, "43DM", 4) != 0 || indexCount == 0 || indexCount > 4096)
        return false;
    f.seekg(static_cast<std::streamoff>(indexOffset), std::ios::beg);
    std::vector<char> table(static_cast<std::size_t>(indexCount) * 16);
    f.read(table.data(), static_cast<std::streamsize>(table.size()));
    if (!f)
        return false;
    for (u32 i = 0; i < indexCount; ++i)
        if (std::memcmp(table.data() + 16 * i, "CSBB", 4) == 0)
            return true;
    return false;
}

std::vector<fs::path> FindBillboardModels(const fs::path& dir, std::size_t limit) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (!it->is_regular_file(ec) || it->path().extension() != ".m3")
            continue;
        if (!HasBillboardChunk(it->path()))
            continue;
        out.push_back(it->path());
        if (out.size() >= limit)
            break;
    }
    return out;
}

// The worst normalised off-diagonal of the 3x3 — zero for any pure rotation
// and scale, non-zero once the rows stop being square to each other.
f32 Skew(const Matrix44f& m) {
    const Vector3f r0 = Row(m, 0), r1 = Row(m, 1), r2 = Row(m, 2);
    const f32 l0 = r0.length(), l1 = r1.length(), l2 = r2.length();
    if (l0 <= 1e-6f || l1 <= 1e-6f || l2 <= 1e-6f)
        return 0.0f;
    return std::max({std::fabs(r0.dot(r1) / (l0 * l1)), std::fabs(r1.dot(r2) / (l1 * l2)),
                     std::fabs(r0.dot(r2) / (l0 * l2))});
}

// Rotation, uniform scale, translation and nothing else. Composing through a
// frame like this preserves both row lengths and squareness; composing through
// a non-uniformly scaled one does not, in the engine as much as here, so the
// two claims that depend on it are only made where the parent qualifies.
bool Conformal(const Matrix44f& m) {
    const f32 l0 = Row(m, 0).length(), l1 = Row(m, 1).length(), l2 = Row(m, 2).length();
    if (l0 <= 1e-6f || l1 <= 1e-6f || l2 <= 1e-6f)
        return false;
    if (std::fabs(l0 - l1) > 1e-3f * l0 || std::fabs(l0 - l2) > 1e-3f * l0)
        return false;
    return Skew(m) <= 1e-3f;
}

int Handedness(const Matrix44f& m) {
    const f32 d = Row(m, 0).dot(whiteout::cross(Row(m, 1), Row(m, 2)));
    return d > 0.0f ? 1 : (d < 0.0f ? -1 : 0);
}

bool Finite(const Matrix44f& m) {
    for (i32 r = 0; r < 4; ++r)
        for (i32 c = 0; c < 4; ++c)
            if (!std::isfinite(m.data[r][c]))
                return false;
    return true;
}

} // namespace

TEST_CASE("shipped billboards turn a bone without breaking it", "[m3bb][corpus]") {
    const fs::path root = CorpusRoot();
    const std::size_t limit = PerCorpusLimit();
    std::vector<fs::path> files;
    for (const char* c : {"Sc2M3", "HotSM3"}) {
        const auto found = FindBillboardModels(root / c, limit);
        files.insert(files.end(), found.begin(), found.end());
    }
    if (files.empty()) {
        WARN("no .m3 corpus with BBSC under " << root.string() << " - skipping");
        return;
    }

    // Off-axis on all three, so no mode can be right by accident.
    const Vector3f eye{140.0f, -90.0f, 60.0f};
    PoseRequest req;
    req.cameraPos = eye;
    req.view = Matrix44f::look_at_rh(eye, {0, 0, 0}, {0, 0, 1});

    std::size_t models = 0, records = 0, turned = 0, comparable = 0;
    std::size_t nonFinite = 0, mirrored = 0, sheared = 0, moved = 0, resized = 0;
    std::size_t byType[8] = {};

    for (const auto& path : files) {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            continue;
        std::vector<u8> bytes((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
        if (bytes.empty())
            continue;
        m3::Model model;
        try {
            m3::Parser parser;
            model = parser.parse(bytes);
        } catch (...) {
            continue;
        }
        if (model.billboardBehaviors.empty() || model.bones.empty())
            continue;
        ++models;

        m3::Model plainModel = model;
        plainModel.billboardBehaviors.clear();

        M3ModelAdapter billboarded(std::move(model));
        M3ModelAdapter plain(std::move(plainModel));
        const auto& src = billboarded.SourceModel();
        const auto got = billboarded.Evaluate(req);
        const auto want = plain.Evaluate(req);
        if (got.boneWorldMatrices.size() != want.boneWorldMatrices.size())
            continue;

        // A bone under another billboard legitimately moves, so the "did not
        // move" claim below is only made where nothing above interfered.
        std::vector<u8> gated(src.bones.size(), 0);
        for (const auto& bb : src.billboardBehaviors)
            if (bb.boneIndex < gated.size())
                gated[bb.boneIndex] = 1;
        std::vector<u8> tainted(src.bones.size(), 0);
        for (std::size_t i = 0; i < src.bones.size(); ++i) {
            const u16 par = src.bones[i].parentIndex;
            if (par != 0xFFFFu && par < i)
                tainted[i] = static_cast<u8>(tainted[par] || gated[par]);
        }

        for (const auto& bb : src.billboardBehaviors) {
            if (bb.boneIndex >= got.boneWorldMatrices.size())
                continue;
            ++records;
            byType[bb.billboardType < 8 ? bb.billboardType : 7]++;
            const Matrix44f& m = got.boneWorldMatrices[bb.boneIndex];
            const Matrix44f& w = want.boneWorldMatrices[bb.boneIndex];
            if (!Finite(m)) {
                ++nonFinite;
                continue;
            }
            const Vector3f r0 = Row(m, 0), r1 = Row(m, 1), r2 = Row(m, 2);
            const f32 l0 = r0.length(), l1 = r1.length(), l2 = r2.length();
            if (l0 <= 1e-6f || l1 <= 1e-6f || l2 <= 1e-6f)
                continue; // a bone scaled to nothing has no basis to judge
            // Handedness is a sign, so it survives any scale and is asked of
            // every bone: what a billboard may never do is turn a mirrored bone
            // the right way round, which would invert its skinned normals.
            if (Handedness(m) != Handedness(w))
                ++mirrored;
            const f32 want1 = Row(w, 1).length();
            if (want1 > 1e-6f && r1.dot(Row(w, 1)) < 0.999f * l1 * want1)
                ++turned;
            if (tainted[bb.boneIndex])
                continue;
            if ((Row(m, 3) - Row(w, 3)).length() > 1e-3f * std::max(1.0f, Row(w, 3).length()))
                ++moved;

            // Squareness and row lengths are only comparable when the parent
            // frame is a rotation plus a uniform scale. Under a non-uniformly
            // scaled parent the engine's own re-compose changes both — it
            // rewrites the local rotation and multiplies the parent's full
            // matrix back in — so asserting they held would be asserting
            // against the engine.
            const u16 par = src.bones[bb.boneIndex].parentIndex;
            if (par != 0xFFFFu && par < bb.boneIndex && !Conformal(want.boneWorldMatrices[par]))
                continue;
            ++comparable;
            if (Skew(m) > std::max(Skew(w), 1e-3f))
                ++sheared;
            for (i32 k = 0; k < 3; ++k) {
                const f32 a = Row(m, k).length(), b = Row(w, k).length();
                if (std::fabs(a - b) > 1e-3f * std::max(1.0f, b))
                    ++resized;
            }
        }
    }

    WARN("billboard sweep: " << models << " models, " << records << " records, " << turned
                             << " reoriented; types 0/1/2/3/4/5/6 = " << byType[0] << "/"
                             << byType[1] << "/" << byType[2] << "/" << byType[3] << "/"
                             << byType[4] << "/" << byType[5] << "/" << byType[6]);

    REQUIRE(records > 100);
    REQUIRE(comparable > 100);
    // The two modes that carry the corpus have to be in the sample, or this is
    // asserting about a rounding error.
    REQUIRE(byType[6] > 0);
    REQUIRE(byType[2] > 0);
    // ...and it has to have actually done something.
    REQUIRE(turned > 0);

    CHECK(nonFinite == 0);
    CHECK(mirrored == 0);
    CHECK(sheared == 0);
    CHECK(moved == 0);
    CHECK(resized == 0);
}
