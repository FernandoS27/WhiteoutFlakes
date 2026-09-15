// Lifetime curves and cell-animation tracks. These are what decide a
// particle's colour/alpha/size over its life, so an endpoint or hint
// regression here is visible on every emitter in the game.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/particle/base/particle_curve.h"

#include <cmath>
#include <vector>

using namespace whiteout::flakes::renderer::particle;
using whiteout::flakes::f32;
using whiteout::flakes::i32;
using whiteout::flakes::u32;
using Catch::Approx;

namespace {

// Rises to a peak at the midpoint then falls — non-monotonic on purpose so
// interpolation modes are distinguishable.
ParticleCurve<f32> MakeRamp(Interp mode) {
    ParticleCurve<f32> c;
    c.SetInterp(mode);
    c.AddKey(0.0f, 0.0f);
    c.AddKey(0.5f, 10.0f);
    c.AddKey(1.0f, 4.0f);
    return c;
}

constexpr Interp kAllModes[] = {Interp::Step, Interp::Linear, Interp::Hermite, Interp::Bezier,
                                Interp::CatmullRom};

} // namespace

TEST_CASE("An unbiased curve reproduces its first and last key exactly") {
    // Anything else pops the particle's colour at birth or death.
    for (Interp mode : kAllModes) {
        const auto c = MakeRamp(mode);
        REQUIRE(c.Evaluate(0.0f) == Approx(0.0f).margin(1e-6));
        REQUIRE(c.Evaluate(1.0f) == Approx(4.0f).margin(1e-6));
    }
}

TEST_CASE("Evaluation clamps outside the key range") {
    for (Interp mode : kAllModes) {
        const auto c = MakeRamp(mode);
        REQUIRE(c.Evaluate(-5.0f) == Approx(0.0f).margin(1e-6));
        REQUIRE(c.Evaluate(37.0f) == Approx(4.0f).margin(1e-6));
    }
}

TEST_CASE("Evaluation is independent of the cursor hint") {
    // The hint is a search optimisation only. A mismatched forward/backward
    // comparison in FindSegment used to make it observable exactly on the key
    // times, so sweep across them.
    for (Interp mode : kAllModes) {
        const auto c = MakeRamp(mode);
        for (i32 i = 0; i <= 200; ++i) {
            const f32 t = static_cast<f32>(i) / 200.0f;
            const f32 ref = c.Evaluate(t, 0);
            for (u32 hint = 0; hint < 5; ++hint)
                REQUIRE(c.Evaluate(t, hint) == ref);
        }
    }
}

TEST_CASE("A sample exactly on a key resolves to the earlier segment") {
    // Matters most for cell animation, where the two sides of a boundary
    // select very different sprite cells.
    const std::vector<CurveKey<f32>> keys{
        {0.0f, 0.0f, 0.0f, 0.0f}, {0.5f, 1.0f, 0.0f, 0.0f}, {1.0f, 2.0f, 0.0f, 0.0f}};
    for (u32 hint = 0; hint < 4; ++hint)
        REQUIRE(FindSegment(keys, 0.5f, hint) == 0u);

    // Observable through Step, which returns the segment's left key.
    REQUIRE(MakeRamp(Interp::Step).Evaluate(0.5f) == Approx(0.0f).margin(1e-6));
}

TEST_CASE("Linear interpolation hits the arithmetic midpoint") {
    auto c = MakeRamp(Interp::Linear);
    REQUIRE(c.Evaluate(0.25f) == Approx(5.0f).margin(1e-5));
    REQUIRE(c.Evaluate(0.75f) == Approx(7.0f).margin(1e-5));
}

TEST_CASE("Step interpolation holds the left key across the segment") {
    auto c = MakeRamp(Interp::Step);
    REQUIRE(c.Evaluate(0.25f) == Approx(0.0f).margin(1e-6));
    REQUIRE(c.Evaluate(0.49f) == Approx(0.0f).margin(1e-6));
    REQUIRE(c.Evaluate(0.75f) == Approx(10.0f).margin(1e-6));
}

TEST_CASE("A rising linear ramp is monotonic") {
    ParticleCurve<f32> c;
    c.SetInterp(Interp::Linear);
    c.AddKey(0.0f, 0.0f);
    c.AddKey(1.0f, 1.0f);

    f32 prev = -1.0f;
    for (i32 i = 0; i <= 100; ++i) {
        const f32 v = c.Evaluate(static_cast<f32>(i) / 100.0f);
        REQUIRE(v >= prev - 1e-6f);
        prev = v;
    }
}

TEST_CASE("Degenerate curves are well-defined") {
    SECTION("no keys evaluates to a zeroed value") {
        const ParticleCurve<f32> empty;
        REQUIRE(empty.Empty());
        REQUIRE(empty.SegmentCount() == 0u);
        REQUIRE(empty.Evaluate(0.5f) == Approx(0.0f).margin(1e-6));
    }
    SECTION("one key is a constant, whatever its time") {
        ParticleCurve<f32> one;
        one.AddKey(0.3f, 7.0f);
        REQUIRE(one.SegmentCount() == 0u);
        REQUIRE(one.Evaluate(0.0f) == Approx(7.0f).margin(1e-6));
        REQUIRE(one.Evaluate(0.3f) == Approx(7.0f).margin(1e-6));
        REQUIRE(one.Evaluate(1.0f) == Approx(7.0f).margin(1e-6));
    }
    SECTION("a zero-width segment does not divide by zero") {
        ParticleCurve<f32> flat;
        flat.AddKey(0.5f, 1.0f);
        flat.AddKey(0.5f, 2.0f);
        const f32 v = flat.Evaluate(0.5f);
        REQUIRE(std::isfinite(v));
    }
}

TEST_CASE("Vector curves interpolate component-wise") {
    ParticleCurve<whiteout::flakes::Vector3f> c;
    c.SetInterp(Interp::Linear);
    c.AddKey(0.0f, {1.0f, 0.0f, 0.0f});
    c.AddKey(1.0f, {0.0f, 1.0f, 2.0f});

    const auto mid = c.Evaluate(0.5f);
    REQUIRE(mid.x == Approx(0.5f).margin(1e-5));
    REQUIRE(mid.y == Approx(0.5f).margin(1e-5));
    REQUIRE(mid.z == Approx(1.0f).margin(1e-5));
}

TEST_CASE("ApplyBias reproduces WC3's segment inset") {
    // WC3 samples each segment over [bias, 1-bias] rather than [0, 1].
    REQUIRE(ApplyBias(0.0f, 0.005f) == Approx(0.005f).margin(1e-6));
    REQUIRE(ApplyBias(1.0f, 0.005f) == Approx(0.995f).margin(1e-6));
    REQUIRE(ApplyBias(0.5f, 0.005f) == Approx(0.5f).margin(1e-6));

    SECTION("zero bias is the identity") {
        REQUIRE(ApplyBias(0.25f, 0.0f) == Approx(0.25f).margin(1e-6));
    }
    SECTION("out-of-range input is clamped first") {
        REQUIRE(ApplyBias(-1.0f, 0.0f) == Approx(0.0f).margin(1e-6));
        REQUIRE(ApplyBias(2.0f, 0.0f) == Approx(1.0f).margin(1e-6));
    }
}

TEST_CASE("A cell track stays inside its declared cell range") {
    CellAnimTrack track;
    track.AddSegment(0.5f, 0, 3, 1);
    track.AddSegment(1.0f, 4, 7, 1);

    for (i32 i = 0; i <= 100; ++i) {
        const f32 t = static_cast<f32>(i) / 100.0f;
        const i32 cell = track.Evaluate(t);
        REQUIRE(cell >= 0);
        REQUIRE(cell <= 7);
    }

    REQUIRE(track.Evaluate(0.0f) == 0);
    REQUIRE(track.Evaluate(1.0f) == 7); // clamped, not stepped past `end`
}

TEST_CASE("A cell track sweeps its segment in order") {
    CellAnimTrack track;
    track.AddSegment(1.0f, 0, 3, 1);

    REQUIRE(track.Evaluate(0.0f) == 0);
    REQUIRE(track.Evaluate(0.3f) == 1);
    REQUIRE(track.Evaluate(0.6f) == 2);
    REQUIRE(track.Evaluate(0.9f) == 3);
}

TEST_CASE("A descending cell track counts down") {
    CellAnimTrack track;
    track.AddSegment(1.0f, 3, 0, 1);

    REQUIRE(track.Evaluate(0.0f) == 3);
    REQUIRE(track.Evaluate(0.99f) == 0);
    for (i32 i = 0; i <= 100; ++i) {
        const i32 cell = track.Evaluate(static_cast<f32>(i) / 100.0f);
        REQUIRE(cell >= 0);
        REQUIRE(cell <= 3);
    }
}

TEST_CASE("Cell track evaluation is independent of the cursor hint") {
    CellAnimTrack track;
    track.AddSegment(0.25f, 0, 3, 1);
    track.AddSegment(0.75f, 4, 7, 2);
    track.AddSegment(1.0f, 8, 9, 1);

    for (i32 i = 0; i <= 200; ++i) {
        const f32 t = static_cast<f32>(i) / 200.0f;
        const i32 ref = track.Evaluate(t, 0);
        for (u32 hint = 0; hint < 5; ++hint)
            REQUIRE(track.Evaluate(t, hint) == ref);
    }
}

TEST_CASE("An empty cell track evaluates to cell zero") {
    const CellAnimTrack track;
    REQUIRE(track.Empty());
    REQUIRE(track.Evaluate(0.5f) == 0);
}
