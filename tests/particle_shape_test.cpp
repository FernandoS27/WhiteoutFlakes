// Spawn shapes and the RNG they draw from. WC3's particle motion is
// reproducible run-to-run only because both are deterministic and consume
// their random numbers in a fixed order — that ordering is observable in the
// particle trace, so it is asserted here rather than left to a visual diff.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/particle/particle_selftest.h"
#include "renderer/particle/particle_shape.h"
#include "renderer/particle/rnd_seed.h"

#include <algorithm>
#include <cmath>
#include <string>

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

TEST_CASE("The viewer's --particle-selftest checks still pass") {
    // Keeps the standalone's built-in check path covered now that the same
    // invariants have proper test cases above.
    std::string report;
    const bool ok = RunParticleSelfTest(report);
    INFO(report);
    REQUIRE(ok);
}
