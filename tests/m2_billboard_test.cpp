// ============================================================================
// M2 bone billboards — the four flags, and the property that defines them.
//
// Pure tables: a hand-built `wm2::Model` through the real adapter. No device,
// no corpus, always runs.
//
// The client billboards with no camera vector at all, because it composes its
// whole bone palette in VIEW space — a root bone's parent is `model x view`, so
// overwriting a bone's basis with one that is constant in view space *is* the
// billboard (`CM2Model::AnimateMT`, the `flags & 0x78` block; the root's parent
// is `this+408`, which `CM2Model::Animate` fills with `(this+344) x m_view`).
// Ours is a model-space palette, so the same construction has to be conjugated
// through the camera basis. Every check below is therefore written on the
// VIEW-space basis, which is the thing the client actually pins down, rather
// than on the model-space matrix, which is an artefact of where we compose.
// ============================================================================

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "io/m2/m2_model_adapter.h"
#include "whiteout/flakes/pose_request.h"

#include <cmath>
#include <utility>
#include <vector>

using Catch::Approx;
using whiteout::Matrix44f;
using whiteout::Vector3f;
using whiteout::f32;
using whiteout::u32;
namespace wm2 = whiteout::m2;
namespace io = whiteout::flakes::io;

namespace {

constexpr u32 kSpherical = 0x008;
constexpr u32 kLockX = 0x010;
constexpr u32 kLockY = 0x020;
constexpr u32 kLockZ = 0x040;
constexpr u32 kTransformed = 0x200;

// The packing M2DecodeQuatComponent inverts: `raw * (2/65535) - 1`.
wm2::CompatQuaternion PackQuat(f32 x, f32 y, f32 z, f32 w) {
    auto enc = [](f32 v) {
        return static_cast<whiteout::u16>((v + 1.0f) * 0.5f * 65535.0f + 0.5f);
    };
    return {enc(x), enc(y), enc(z), enc(w)};
}

// Root, then a bone that may billboard, then a plain child of it. The child is
// how propagation is observed: the client writes the billboard back into the
// palette, so everything below a billboarded bone rides along.
wm2::Model MakeModel(u32 billboardBone1Flags, const Vector3f& pivot1 = {0.0f, 0.0f, 0.0f}) {
    wm2::Model m;
    wm2::Sequence seq;
    seq.duration = 1000;
    m.sequences.push_back(seq);

    wm2::Bone root;
    root.parentBoneId = -1;
    root.pivot = {0.0f, 0.0f, 0.0f};
    m.bones.push_back(root);

    wm2::Bone mid;
    mid.parentBoneId = 0;
    mid.flags = billboardBone1Flags;
    mid.pivot = pivot1;
    m.bones.push_back(mid);

    wm2::Bone leaf;
    leaf.parentBoneId = 1;
    leaf.pivot = pivot1;
    m.bones.push_back(leaf);
    return m;
}

std::vector<Matrix44f> Pose(wm2::Model model, const Matrix44f& view, const Matrix44f& world,
                            whiteout::i32 sequence = -1) {
    io::M2ModelAdapter adapter(std::move(model));
    whiteout::flakes::ClipRef clip{};
    clip.sequence = sequence;
    const whiteout::flakes::ClipRef clips[] = {clip};
    whiteout::flakes::PoseRequest req{};
    req.clips = clips;
    req.view = view;
    req.world = world;
    return adapter.Evaluate(req).boneWorldMatrices;
}

Matrix44f ViewFrom(const Vector3f& eye) {
    return Matrix44f::look_at_rh(eye, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
}

Vector3f Row(const Matrix44f& m, int r) {
    return {m.data[r][0], m.data[r][1], m.data[r][2]};
}

// The bone's axes as the camera sees them — the space the client's billboard is
// actually constant in.
Matrix44f ViewSpace(const Matrix44f& bone, const Matrix44f& world, const Matrix44f& view) {
    return bone * (world * view);
}

f32 Det3(const Matrix44f& m) {
    return Row(m, 0).dot(whiteout::cross(Row(m, 1), Row(m, 2)));
}

void RequireRowsMatch(const Matrix44f& a, const Matrix44f& b, f32 eps = 1e-4f) {
    for (int r = 0; r < 3; ++r) {
        INFO("row " << r);
        CHECK(Row(a, r).x == Approx(Row(b, r).x).margin(eps));
        CHECK(Row(a, r).y == Approx(Row(b, r).y).margin(eps));
        CHECK(Row(a, r).z == Approx(Row(b, r).z).margin(eps));
    }
}

} // namespace

TEST_CASE("a spherical billboard holds the same basis from every camera", "[m2][billboard]") {
    // The defining property, and the only one worth asserting for `0x08`: the
    // bone's screen-space orientation must not depend on where the camera is.
    const Matrix44f world = Matrix44f::identity();
    const Matrix44f v1 = ViewFrom({10.0f, 0.0f, 0.0f});
    const Matrix44f v2 = ViewFrom({-3.0f, 7.0f, 4.0f});

    const auto a = Pose(MakeModel(kSpherical), v1, world);
    const auto b = Pose(MakeModel(kSpherical), v2, world);
    REQUIRE(a.size() == 3);

    RequireRowsMatch(ViewSpace(a[1], world, v1), ViewSpace(b[1], world, v2));

    // And the same for the plain child hanging off it: the billboard is written
    // back into the palette, so the subtree inherits it.
    RequireRowsMatch(ViewSpace(a[2], world, v1), ViewSpace(b[2], world, v2));
}

TEST_CASE("an unbillboarded bone is not screen-invariant", "[m2][billboard]") {
    // The control. Without this the test above passes just as well on a bone
    // whose basis happens to be identity in both views.
    const Matrix44f world = Matrix44f::identity();
    const Matrix44f v1 = ViewFrom({10.0f, 0.0f, 0.0f});
    const Matrix44f v2 = ViewFrom({-3.0f, 7.0f, 4.0f});

    const auto a = Pose(MakeModel(0), v1, world);
    const auto b = Pose(MakeModel(0), v2, world);
    const Matrix44f va = ViewSpace(a[1], world, v1);
    const Matrix44f vb = ViewSpace(b[1], world, v2);

    bool differs = false;
    for (int r = 0; r < 3 && !differs; ++r)
        differs = (Row(va, r) - Row(vb, r)).length() > 1e-3f;
    CHECK(differs);
}

TEST_CASE("a cylindrical billboard keeps its locked axis and swings the rest", "[m2][billboard]") {
    // Cylindrical is not screen-invariant and must not be: the locked axis
    // survives untouched, and the other two turn inside the screen plane so the
    // third ends up nearest the eye. Both halves are checked in view space,
    // where "the screen plane" is just z == 0.
    const Matrix44f world = Matrix44f::identity();
    const Matrix44f view = ViewFrom({6.0f, -2.0f, 3.0f});

    struct Case {
        u32 flag;
        int locked;  // the row the flag pins
        int free;    // the row driven into the screen plane
    };
    const Case cases[] = {{kLockX, 0, 1}, {kLockY, 1, 0}, {kLockZ, 2, 1}};

    for (const Case& c : cases) {
        INFO("flag " << c.flag);
        const auto plain = Pose(MakeModel(0), view, world);
        const auto bent = Pose(MakeModel(c.flag), view, world);

        const Matrix44f before = ViewSpace(plain[1], world, view);
        const Matrix44f after = ViewSpace(bent[1], world, view);

        // The locked axis points exactly where it did.
        const Vector3f wasLocked = Row(before, c.locked).normalized();
        const Vector3f isLocked = Row(after, c.locked).normalized();
        CHECK((wasLocked - isLocked).length() == Approx(0.0f).margin(1e-4f));

        // The free axis lies in the screen plane, and stays perpendicular.
        CHECK(Row(after, c.free).z == Approx(0.0f).margin(1e-4f));
        CHECK(Row(after, c.free).dot(Row(after, c.locked)) == Approx(0.0f).margin(1e-4f));

        // All three still form a frame.
        for (int r = 0; r < 3; ++r)
            CHECK(Row(after, r).length() == Approx(1.0f).margin(1e-3f));
    }
}

TEST_CASE("billboarding rotates a bone in place and keeps its scale", "[m2][billboard]") {
    // Both halves of the client's fix-up. It restores the row lengths from the
    // composed matrix and rebuilds the translation from the pre-billboard pivot
    // position, so a billboard is a rotation and nothing else.
    const Vector3f pivot{2.0f, -5.0f, 1.5f};
    const Matrix44f world = Matrix44f::identity();
    const Matrix44f view = ViewFrom({4.0f, 4.0f, 4.0f});

    wm2::Model scaled = MakeModel(kSpherical, pivot);
    // A sampled root, so the billboarded child inherits a real scale to preserve.
    scaled.bones[0].flags = kTransformed;
    scaled.bones[0].scale.interpolationType = wm2::InterpolationType::Linear;
    scaled.bones[0].scale.timestamps = {{0u}};
    scaled.bones[0].scale.values = {{Vector3f{3.0f, 3.0f, 3.0f}}};

    wm2::Model plainModel = scaled;
    plainModel.bones[1].flags = 0;

    const auto bent = Pose(scaled, view, world, 0);
    const auto plain = Pose(plainModel, view, world, 0);

    // Scale: the row lengths of the view-space basis are what the client
    // measures and restores, so that is where they have to match.
    const Matrix44f before = ViewSpace(plain[1], world, view);
    const Matrix44f after = ViewSpace(bent[1], world, view);
    for (int r = 0; r < 3; ++r) {
        INFO("row " << r);
        CHECK(Row(after, r).length() == Approx(Row(before, r).length()).epsilon(1e-4f));
        CHECK(Row(after, r).length() == Approx(3.0f).epsilon(1e-4f));
    }

    // In place: the pivot lands where it did.
    const Vector3f wasAt = whiteout::transform_point(pivot, plain[1]);
    const Vector3f isAt = whiteout::transform_point(pivot, bent[1]);
    CHECK((wasAt - isAt).length() == Approx(0.0f).margin(1e-3f));
}

TEST_CASE("a bone that is not Transformed still billboards", "[m2][billboard]") {
    // 14774 of the corpus's 18799 billboarded bones never set `Transformed`, so
    // if the billboard rode on the sampled branch four in five would do nothing.
    // The client's `flags & 0x78` block sits outside that test.
    const Matrix44f world = Matrix44f::identity();
    const Matrix44f v1 = ViewFrom({9.0f, 1.0f, 0.0f});
    const Matrix44f v2 = ViewFrom({0.0f, -6.0f, 5.0f});

    wm2::Model m = MakeModel(kSpherical);
    REQUIRE((m.bones[1].flags & kTransformed) == 0u);

    RequireRowsMatch(ViewSpace(Pose(m, v1, world)[1], world, v1),
                     ViewSpace(Pose(m, v2, world)[1], world, v2));
}

TEST_CASE("a bone setting two billboard bits does not billboard", "[m2][billboard]") {
    // The client dispatches on `flags & 0x78` through a switch with no
    // `default`, so 0x18 and 0x48 fall through it untouched. Six corpus bones
    // do that (two 0x18 in igc_jaina2/3, four 0x48 in drakonid2primalist_*),
    // and they are inert in the client rather than picking a winner.
    const Matrix44f world = Matrix44f::identity();
    const Matrix44f view = ViewFrom({5.0f, 5.0f, 2.0f});

    const auto none = Pose(MakeModel(0), view, world);
    for (u32 combo : {kSpherical | kLockX, kSpherical | kLockZ, kLockX | kLockZ}) {
        INFO("flags " << combo);
        RequireRowsMatch(Pose(MakeModel(combo), view, world)[1], none[1]);
    }
}

TEST_CASE("a spherical billboard spins with the bone's own rotation", "[m2][billboard]") {
    // The one part of the client's spherical case that is not a constant: the
    // basis is built from the bone's LOCAL rotation with its columns permuted,
    // not from a literal. That is what lets an animated billboard spin in the
    // screen plane; an unsampled bone's identity local transform constant-folds
    // to the fixed basis the client keeps in a literal.
    const Matrix44f world = Matrix44f::identity();
    const Matrix44f view = ViewFrom({7.0f, 0.0f, 2.0f});

    wm2::Model spun = MakeModel(kSpherical);
    spun.bones[1].flags |= kTransformed;
    spun.bones[1].rotation.interpolationType = wm2::InterpolationType::Linear;
    spun.bones[1].rotation.timestamps = {{0u}};
    // 90 degrees about Z.
    spun.bones[1].rotation.values = {{PackQuat(0.0f, 0.0f, 0.70710678f, 0.70710678f)}};

    const Matrix44f still = ViewSpace(Pose(MakeModel(kSpherical), view, world)[1], world, view);
    const Matrix44f turned = ViewSpace(Pose(spun, view, world, 0)[1], world, view);

    bool differs = false;
    for (int r = 0; r < 3 && !differs; ++r)
        differs = (Row(still, r) - Row(turned, r)).length() > 1e-3f;
    CHECK(differs);

    // Still a frame, though — the spin is applied inside the billboard basis,
    // not on top of it.
    for (int r = 0; r < 3; ++r)
        CHECK(Row(turned, r).length() == Approx(1.0f).margin(1e-3f));
}

TEST_CASE("billboarding never mirrors a bone", "[m2][billboard]") {
    // The invariant a billboard cannot break, and the one that catches getting
    // the view's handedness wrong. The client's fixed basis is rows
    // `(0,0,-1) (1,0,0) (0,1,0)` — determinant -1 — so the view basis it is
    // written against is left-handed too, and the two cancel. Handing those
    // constants a right-handed basis instead inverts the frame, which flips the
    // bone's skinned normals and blows the shading out to white.
    const Matrix44f world = Matrix44f::identity();
    const Matrix44f view = ViewFrom({5.0f, -3.0f, 4.0f});

    const f32 plain = Det3(Pose(MakeModel(0), view, world)[1]);
    REQUIRE(plain > 0.0f);

    for (u32 flag : {kSpherical, kLockX, kLockY, kLockZ}) {
        INFO("flag " << flag);
        const auto bent = Pose(MakeModel(flag), view, world);
        CHECK(Det3(bent[1]) > 0.0f);
        // The child rides the billboard, so it must not be mirrored either.
        CHECK(Det3(bent[2]) > 0.0f);
    }
}

TEST_CASE("a billboard is unaffected by the actor's world scale", "[m2][billboard]") {
    // A WoW actor is drawn at 40x through `ScaledWorldTransform`, and that
    // scale reaches the pose as `PoseRequest::world`. It must not reach the
    // camera basis: the bone's row lengths are measured through that basis and
    // mapped back through its inverse, so a scale left in it is applied twice —
    // 1600x on a 40x actor, which is one sprite covering the whole viewport.
    // Every other case here poses at identity, which is precisely why none of
    // them saw it.
    const Matrix44f view = ViewFrom({120.0f, 60.0f, 40.0f});

    for (u32 flag : {kSpherical, kLockX, kLockY, kLockZ}) {
        INFO("flag " << flag);
        const auto unit = Pose(MakeModel(flag), view, Matrix44f::identity());
        const auto big =
            Pose(MakeModel(flag), view, Matrix44f::scaling({40.0f, 40.0f, 40.0f}));
        // Identical, not merely proportional: the palette is model-space and
        // the world transform is applied downstream of it.
        RequireRowsMatch(big[1], unit[1]);
        RequireRowsMatch(big[2], unit[2]);
    }
}
