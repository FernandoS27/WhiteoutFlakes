// Spawn shapes and the RNG they draw from. WC3's particle motion is
// reproducible run-to-run only because both are deterministic and consume
// their random numbers in a fixed order — that ordering is observable in the
// particle trace, so it is asserted here rather than left to a visual diff.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/particle/particle_shape.h"
#include "renderer/particle/rnd_seed.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace whiteout::flakes::renderer::particle;
using whiteout::flakes::f32;
using whiteout::flakes::i32;
using whiteout::flakes::u32;
using whiteout::flakes::Vector3f;
using Catch::Approx;

namespace {

f32 Length(const Vector3f& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

SpawnParams MakeParams() {
    SpawnParams p;
    p.width = 10.0f;
    p.height = 6.0f;
    p.latitude = 0.5f;
    p.longitude = 6.2831853f;
    p.speed.base = 3.0f;
    p.speed.variance = 0.0f; // pin the speed so the magnitude is checkable
    return p;
}

} // namespace

TEST_CASE("RndSeed is reproducible for a given seed") {
    RndSeed a(12345u);
    RndSeed b(12345u);
    for (i32 i = 0; i < 64; ++i)
        REQUIRE(CRandom::next_u32(a) == CRandom::next_u32(b));
}

TEST_CASE("Seed zero is remapped away from the xorshift fixed point") {
    // x ^= x << 13 … on a zero state stays zero forever, which would freeze
    // every emitter that happened to hash to 0.
    RndSeed zero(0u);
    REQUIRE(zero.state != 0u);
    REQUIRE(CRandom::next_u32(zero) != 0u);
}

TEST_CASE("Different seeds diverge") {
    RndSeed a(1u);
    RndSeed b(2u);
    bool diverged = false;
    for (i32 i = 0; i < 16 && !diverged; ++i)
        diverged = CRandom::next_u32(a) != CRandom::next_u32(b);
    REQUIRE(diverged);
}

TEST_CASE("real_ and reals_ stay in range") {
    RndSeed rnd(0xC0FFEEu);
    for (i32 i = 0; i < 4096; ++i) {
        const f32 u = CRandom::real_(rnd);
        REQUIRE(u >= 0.0f);
        REQUIRE(u < 1.0f);
    }
    for (i32 i = 0; i < 4096; ++i) {
        const f32 s = CRandom::reals_(rnd);
        REQUIRE(s >= -1.0f);
        REQUIRE(s < 1.0f);
    }
}

TEST_CASE("C3Vector_ draws unit vectors") {
    RndSeed rnd(777u);
    for (i32 i = 0; i < 1024; ++i)
        REQUIRE(Length(CRandom::C3Vector_(rnd)) == Approx(1.0f).margin(1e-4));
}

TEST_CASE("dice_ stays below its bound and handles zero") {
    RndSeed rnd(42u);
    REQUIRE(CRandom::dice_(0u, rnd) == 0u);
    for (i32 i = 0; i < 1024; ++i)
        REQUIRE(CRandom::dice_(6u, rnd) < 6u);
}

TEST_CASE("MixSeed is a pure, stable hash") {
    REQUIRE(MixSeed(7u) == MixSeed(7u));
    REQUIRE(MixSeed(7u) != MixSeed(8u));

    // Per-emitter seeds must depend on both the actor and the emitter index,
    // and not on how many emitters were constructed before them.
    REQUIRE(MixSeed(3u, 1u) == MixSeed(3u, 1u));
    REQUIRE(MixSeed(3u, 1u) != MixSeed(3u, 2u));
    REQUIRE(MixSeed(3u, 1u) != MixSeed(4u, 1u));
}

TEST_CASE("PlaneShape spawns inside its width/height rectangle") {
    const SpawnParams p = MakeParams();
    PlaneShape plane;
    RndSeed rnd(12345u);

    for (i32 i = 0; i < 500; ++i) {
        SpawnSample s;
        plane.Sample(s, p, rnd);
        REQUIRE(std::fabs(s.localPos.x) <= p.width * 0.5f + 1e-4f);
        REQUIRE(std::fabs(s.localPos.y) <= p.height * 0.5f + 1e-4f);
        REQUIRE(s.localPos.z == 0.0f); // the plane is flat in local XY
    }
}

TEST_CASE("ConeShape spawns at the origin") {
    const SpawnParams p = MakeParams();
    ConeShape cone;
    RndSeed rnd(999u);

    for (i32 i = 0; i < 500; ++i) {
        SpawnSample s;
        cone.Sample(s, p, rnd);
        REQUIRE(s.localPos.x == 0.0f);
        REQUIRE(s.localPos.y == 0.0f);
        REQUIRE(s.localPos.z == 0.0f);
    }
}

TEST_CASE("Spawn velocity magnitude equals the drawn speed") {
    const SpawnParams p = MakeParams();
    PlaneShape plane;
    ConeShape cone;
    RndSeed rnd(2024u);

    for (i32 i = 0; i < 500; ++i) {
        SpawnSample s;
        plane.Sample(s, p, rnd);
        REQUIRE(Length(s.localVel) == Approx(p.speed.base).margin(1e-3));
        cone.Sample(s, p, rnd);
        REQUIRE(Length(s.localVel) == Approx(p.speed.base).margin(1e-3));
    }
}

TEST_CASE("Spawn velocity stays inside the latitude cone") {
    SpawnParams p = MakeParams();
    p.latitude = 0.4f;
    ConeShape cone;
    RndSeed rnd(555u);

    // The cone opens around local +Z; cos of the half-angle bounds vz.
    const f32 minZ = p.speed.base * std::cos(p.latitude) - 1e-3f;
    for (i32 i = 0; i < 500; ++i) {
        SpawnSample s;
        cone.Sample(s, p, rnd);
        REQUIRE(s.localVel.z >= minZ);
    }
}

TEST_CASE("Zero latitude and longitude emit straight down local +Z") {
    SpawnParams p = MakeParams();
    p.latitude = 0.0f;
    p.longitude = 0.0f;
    ConeShape cone;
    RndSeed rnd(1u);

    SpawnSample s;
    cone.Sample(s, p, rnd);
    REQUIRE(s.localVel.x == Approx(0.0f).margin(1e-5));
    REQUIRE(s.localVel.y == Approx(0.0f).margin(1e-5));
    REQUIRE(s.localVel.z == Approx(p.speed.base).margin(1e-5));
}

TEST_CASE("Speed variance widens the draw around the base speed") {
    SpawnParams p = MakeParams();
    p.speed.variance = 0.5f;
    ConeShape cone;
    RndSeed rnd(31337u);

    f32 lo = p.speed.base, hi = p.speed.base;
    for (i32 i = 0; i < 512; ++i) {
        SpawnSample s;
        cone.Sample(s, p, rnd);
        const f32 mag = Length(s.localVel);
        // speed = base * (1 + reals_ * variance), so |v| ∈ base·[0.5, 1.5].
        REQUIRE(mag >= p.speed.base * (1.0f - p.speed.variance) - 1e-3f);
        REQUIRE(mag <= p.speed.base * (1.0f + p.speed.variance) + 1e-3f);
        lo = std::min(lo, mag);
        hi = std::max(hi, mag);
    }
    REQUIRE(hi > lo); // the variance is actually applied
}

TEST_CASE("Two emitters on the same seed sample identically") {
    // What makes a fixed scene reproduce its particle motion run to run.
    const SpawnParams p = MakeParams();
    PlaneShape plane;
    RndSeed a(MixSeed(9u, 2u));
    RndSeed b(MixSeed(9u, 2u));

    for (i32 i = 0; i < 256; ++i) {
        SpawnSample sa, sb;
        plane.Sample(sa, p, a);
        plane.Sample(sb, p, b);
        REQUIRE(sa.localPos.x == sb.localPos.x);
        REQUIRE(sa.localPos.y == sb.localPos.y);
        REQUIRE(sa.localVel.x == sb.localVel.x);
        REQUIRE(sa.localVel.y == sb.localVel.y);
        REQUIRE(sa.localVel.z == sb.localVel.z);
    }
}

TEST_CASE("Each spawn consumes a fixed number of random draws") {
    // The shape's draw count is part of the format's observable behaviour: an
    // extra or missing draw shifts every subsequent particle in the emitter.
    const SpawnParams p = MakeParams();

    SECTION("PlaneShape draws 5 (height, width, latitude, longitude, speed)") {
        RndSeed shaped(1234u);
        RndSeed raw(1234u);
        SpawnSample s;
        PlaneShape().Sample(s, p, shaped);
        for (i32 i = 0; i < 5; ++i)
            CRandom::next_u32(raw);
        REQUIRE(shaped.state == raw.state);
    }
    SECTION("ConeShape draws 3 (latitude, longitude, speed)") {
        RndSeed shaped(1234u);
        RndSeed raw(1234u);
        SpawnSample s;
        ConeShape().Sample(s, p, shaped);
        for (i32 i = 0; i < 3; ++i)
            CRandom::next_u32(raw);
        REQUIRE(shaped.state == raw.state);
    }
}

// ---------------------------------------------------------------------------
// WowBoneShape — the bone generator (`emitterType` 4). It is the one shape whose
// spawn area is not its own: it picks a bone out of a per-MODEL table and
// scatters radially in that bone's plane.
// ---------------------------------------------------------------------------

namespace {

using BoneSpawn = ::whiteout::flakes::renderer::model::FrameState::BoneSpawn;

// Two bones along +X, each with a parent, so every draw the shape makes is
// exercised — a parentless bone skips the along-the-segment lerp.
std::vector<BoneSpawn> MakeBoneTable() {
    std::vector<BoneSpawn> table(2);
    table[0].parentPos = {0.0f, 0.0f, 0.0f};
    table[0].pos = {4.0f, 0.0f, 0.0f};
    table[0].axisA = {1.0f, 0.0f, 0.0f};
    table[0].axisB = {0.0f, 0.0f, 1.0f};
    table[0].hasParent = true;

    table[1].parentPos = {0.0f, 10.0f, 0.0f};
    table[1].pos = {0.0f, 14.0f, 0.0f};
    table[1].axisA = {0.0f, 1.0f, 0.0f};
    table[1].axisB = {0.0f, 0.0f, 1.0f};
    table[1].hasParent = true;
    return table;
}

} // namespace

TEST_CASE("WowBoneShape spawns along a bone, scattered inside its radius") {
    const auto table = MakeBoneTable();
    SpawnParams p = MakeParams();
    // For this generator the two area floats are the min and max scatter radius.
    p.width = 0.5f;
    p.height = 2.0f;
    p.zSource = 0.0f;
    p.boneTable = table;

    WowBoneShape bone;
    RndSeed rnd(4711u);
    i32 fromFirst = 0, fromSecond = 0;

    // The scatter is NOT a tube around the bone. `axisA` is the bone's own
    // direction and `axisB` a perpendicular, so the offset sweeps a circle in a
    // plane that CONTAINS the bone — a particle can slide along the bone as
    // easily as away from it, and its distance to the segment can be anything
    // from zero to the radius. What the plane does pin exactly is the third
    // axis: bone 0 lies along X and scatters in XZ, so every spawn from it has
    // y == 0; bone 1 lies along Y and scatters in YZ, so x == 0.
    for (i32 i = 0; i < 2000; ++i) {
        SpawnSample s;
        bone.Sample(s, p, rnd);
        const bool first = (std::fabs(s.localPos.y) < 1e-5f);
        if (first) {
            REQUIRE(s.localPos.y == Approx(0.0f).margin(1e-5f));
            // Along the bone: anywhere on [0, 4], plus at most one radius of
            // scatter in the same direction.
            REQUIRE(s.localPos.x >= -p.height - 1e-3f);
            REQUIRE(s.localPos.x <= 4.0f + p.height + 1e-3f);
            ++fromFirst;
        } else {
            REQUIRE(s.localPos.x == Approx(0.0f).margin(1e-5f));
            REQUIRE(s.localPos.y >= 10.0f - p.height - 1e-3f);
            REQUIRE(s.localPos.y <= 14.0f + p.height + 1e-3f);
            ++fromSecond;
        }
        // Both bones scatter their perpendicular into Z, so the maximum radius
        // bounds it whichever was picked.
        REQUIRE(std::fabs(s.localPos.z) <= p.height + 1e-3f);
    }
    // Both bones get used: picking one and never the other is what a broken
    // `dice_` bound looks like, and it would still pass every bound above.
    INFO("first=" << fromFirst << " second=" << fromSecond);
    REQUIRE(fromFirst > 200);
    REQUIRE(fromSecond > 200);
}

TEST_CASE("WowBoneShape with no table spawns nothing rather than crashing") {
    // An unresolved table is the normal state for every generator that is not a
    // bone one, and for a bone one on a model whose skin used no eligible bone.
    // The client refuses the spawn; the caller here cannot, so it gets a
    // particle at the emitter with no velocity.
    SpawnParams p = MakeParams();
    p.boneTable = {};

    WowBoneShape bone;
    RndSeed rnd(4711u);
    const u32 before = rnd.state;
    SpawnSample s;
    bone.Sample(s, p, rnd);

    REQUIRE(s.localPos.x == 0.0f);
    REQUIRE(s.localPos.y == 0.0f);
    REQUIRE(s.localPos.z == 0.0f);
    REQUIRE(Length(s.localVel) == 0.0f);
    // And it costs nothing: an early return that still drew would desynchronise
    // every later particle on the emitter.
    REQUIRE(rnd.state == before);
}

TEST_CASE("WowBoneShape emits straight up unless zSource aims it") {
    const auto table = MakeBoneTable();
    SpawnParams p = MakeParams();
    p.width = 0.0f;
    p.height = 0.0f; // no scatter, so the spawn sits exactly on the bone
    p.boneTable = table;

    SECTION("no zSource: +Z, because the generator has no angle ranges of its own") {
        p.zSource = 0.0f;
        WowBoneShape bone;
        RndSeed rnd(31337u);
        for (i32 i = 0; i < 200; ++i) {
            SpawnSample s;
            bone.Sample(s, p, rnd);
            REQUIRE(s.localVel.x == Approx(0.0f).margin(1e-5f));
            REQUIRE(s.localVel.y == Approx(0.0f).margin(1e-5f));
            REQUIRE(s.localVel.z == Approx(p.speed.base).margin(1e-4f));
        }
    }

    SECTION("with zSource: away from (0, 0, zSource) through the spawn point") {
        // Positive, and it has to be: the branch is gated on
        // `zSource > kMinZSource`, so a negative source is simply "off" — which
        // is CGeneratorAniProp::MIN_ZSOURCE's whole job.
        p.zSource = 20.0f;
        WowBoneShape bone;
        RndSeed rnd(31337u);
        for (i32 i = 0; i < 200; ++i) {
            SpawnSample s;
            bone.Sample(s, p, rnd);
            REQUIRE(Length(s.localVel) == Approx(p.speed.base).margin(1e-4f));
            const Vector3f aim{s.localPos.x, s.localPos.y, s.localPos.z - p.zSource};
            const f32 len = Length(aim);
            REQUIRE(len > 0.0f);
            REQUIRE(s.localVel.x == Approx(aim.x / len * p.speed.base).margin(1e-4f));
            REQUIRE(s.localVel.y == Approx(aim.y / len * p.speed.base).margin(1e-4f));
            REQUIRE(s.localVel.z == Approx(aim.z / len * p.speed.base).margin(1e-4f));
        }
    }
}

// ---------------------------------------------------------------------------
// The zSource aim, for the three generators that share AimFromZSource with the
// bone one. Previously only WowBoneShape asserted it, which let a sign flip in
// WowPlaneShape's branch pass the whole suite: the aim is the only strongly
// Z-directional spawn path in the format, and the polar/azimuth branch that
// covers the rest of the corpus is Z-symmetric at the common verticalRange of
// pi, so nothing else notices.
//
// The direction is `(pos - (0,0,zSource))` normalised, NOT its negation. From
// CPlaneGenerator::CreateParticle @0x1016c6730, which computes
// `velZ = (pos.z - zSource) * speed/len` — the sphere and spline generators
// reach the same helper.
// ---------------------------------------------------------------------------

namespace {

// The aim the client computes, independent of the shape under test.
Vector3f ExpectedAim(const Vector3f& pos, f32 zSource, f32 speed) {
    const Vector3f d{pos.x, pos.y, pos.z - zSource};
    const f32 len = Length(d);
    return {d.x / len * speed, d.y / len * speed, d.z / len * speed};
}

} // namespace

TEST_CASE("a zSource aim points away from the source, not toward it") {
    SpawnParams p = MakeParams();
    p.zSource = 20.0f;
    // Non-degenerate so the lateral components are non-zero and a sign error on
    // any single axis is visible.
    p.width = 4.0f;
    p.height = 3.0f;

    auto check = [&](const IParticleShape& shape, u32 seed) {
        RndSeed rnd(seed);
        for (i32 i = 0; i < 200; ++i) {
            SpawnSample s;
            shape.Sample(s, p, rnd);
            const Vector3f want = ExpectedAim(s.localPos, p.zSource, p.speed.base);
            REQUIRE(s.localVel.x == Approx(want.x).margin(1e-4f));
            REQUIRE(s.localVel.y == Approx(want.y).margin(1e-4f));
            REQUIRE(s.localVel.z == Approx(want.z).margin(1e-4f));
            REQUIRE(Length(s.localVel) == Approx(p.speed.base).margin(1e-4f));
        }
    };

    SECTION("plane") {
        check(WowPlaneShape(), 4711u);
    }
    SECTION("sphere") {
        check(WowSphereShape(), 4711u);
    }
    SECTION("sphere, hemisphere flag set — zSource still wins") {
        check(WowSphereShape(true), 4711u);
    }
    SECTION("spline") {
        check(WowSplineShape({{0.0f, 0.0f, 0.0f}, {2.0f, 1.0f, 0.5f}, {4.0f, -1.0f, 1.0f}}),
              4711u);
    }
}

TEST_CASE("a large zSource degenerates into a beam along local -Z") {
    // The shipped idiom, and the reason a sign flip here is so visible: 255 is
    // the recurring authored value, and at that distance the lateral terms wash
    // out and the emitter becomes a parallel beam. A plane emitter puts its
    // spawn at z = 0 exactly, so the aim is (x, y, -255) — essentially -Z.
    //
    // Positive speed therefore emits DOWNWARD in emitter-local space. That is
    // the client's behaviour, not a bug: an artist wanting the beam to rise
    // authors a negative emissionSpeed, which the sign-preserving CalcVelocity
    // @0x10169eea0 carries through.
    SpawnParams p = MakeParams();
    p.zSource = 255.0f;
    p.width = 0.5f;
    p.height = 0.5f;

    RndSeed rnd(31337u);
    for (i32 i = 0; i < 200; ++i) {
        SpawnSample s;
        WowPlaneShape().Sample(s, p, rnd);
        REQUIRE(s.localPos.z == 0.0f);
        REQUIRE(s.localVel.z < 0.0f);
        REQUIRE(s.localVel.z == Approx(-p.speed.base).margin(1e-3f));
    }

    SECTION("and a negative speed turns the same beam around") {
        SpawnParams up = p;
        up.speed.base = -3.0f;
        RndSeed r2(31337u);
        for (i32 i = 0; i < 200; ++i) {
            SpawnSample s;
            WowPlaneShape().Sample(s, up, r2);
            REQUIRE(s.localVel.z > 0.0f);
            REQUIRE(s.localVel.z == Approx(3.0f).margin(1e-3f));
        }
    }
}

TEST_CASE("WowBoneShape draws speed before it picks a bone") {
    // Load-bearing ordering, and the one place this generator differs from every
    // other: the client splits the spawn across two functions, and `CalcVelocity`
    // runs at the end of `BaseCreateParticle` — before the derived
    // `CreateParticle` has picked a bone at all. Five draws: speed, bone,
    // along-the-bone, radius, angle.
    const auto table = MakeBoneTable();
    SpawnParams p = MakeParams();
    p.width = 0.5f;
    p.height = 2.0f;
    p.zSource = 0.0f;
    p.boneTable = table;

    RndSeed shaped(1234u);
    RndSeed raw(1234u);
    SpawnSample s;
    WowBoneShape().Sample(s, p, shaped);
    for (i32 i = 0; i < 5; ++i)
        CRandom::next_u32(raw);
    REQUIRE(shaped.state == raw.state);

    SECTION("and the speed really is the first of them") {
        RndSeed a(1234u);
        SpawnParams fast = p;
        fast.speed.base = 9.0f;
        SpawnSample first, second;
        WowBoneShape().Sample(first, p, a);

        RndSeed b(1234u);
        WowBoneShape().Sample(second, fast, b);
        // Same stream, so the same bone, the same point along it and the same
        // scatter — only the speed differs, which is only visible in the
        // velocity. A speed drawn later would have shifted the position too.
        REQUIRE(first.localPos.x == Approx(second.localPos.x));
        REQUIRE(first.localPos.y == Approx(second.localPos.y));
        REQUIRE(first.localPos.z == Approx(second.localPos.z));
        REQUIRE(Length(second.localVel) == Approx(3.0f * Length(first.localVel)));
    }
}

TEST_CASE("a parentless bone spawns at its pivot and skips a draw") {
    // The lerp along the segment only happens when there is a segment. Skipping
    // the draw as well as the lerp is what keeps the stream aligned.
    std::vector<BoneSpawn> table(1);
    table[0].pos = {2.0f, 3.0f, 4.0f};
    table[0].parentPos = table[0].pos;
    table[0].hasParent = false;

    SpawnParams p = MakeParams();
    p.width = 0.0f;
    p.height = 0.0f;
    p.zSource = 0.0f;
    p.boneTable = table;

    RndSeed shaped(77u);
    RndSeed raw(77u);
    SpawnSample s;
    WowBoneShape().Sample(s, p, shaped);
    for (i32 i = 0; i < 4; ++i) // speed, bone, radius, angle — no lerp
        CRandom::next_u32(raw);
    REQUIRE(shaped.state == raw.state);

    REQUIRE(s.localPos.x == Approx(2.0f));
    REQUIRE(s.localPos.y == Approx(3.0f));
    REQUIRE(s.localPos.z == Approx(4.0f));
}
