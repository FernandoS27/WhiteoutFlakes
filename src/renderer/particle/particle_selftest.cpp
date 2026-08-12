// Unit checks for the pure pieces of the particle refactor — curves, cell
// tracks and spawn shapes are ordinary functions, so they get ordinary tests.
// Driven by the standalone's --particle-selftest flag (the repo has no test
// framework and this needs no new dependency).

#include "renderer/particle/particle_selftest.h"

#include "renderer/particle/particle_curve.h"
#include "renderer/particle/particle_shape.h"

#include <cmath>
#include <sstream>

namespace whiteout::flakes::renderer::particle {

namespace {

struct Checker {
    std::ostringstream fails;
    i32 ran = 0;
    i32 failed = 0;

    void Check(bool ok, const char* what) {
        ++ran;
        if (!ok) {
            ++failed;
            fails << "  FAIL: " << what << "\n";
        }
    }
    void CheckNear(f32 a, f32 b, f32 eps, const char* what) {
        ++ran;
        if (!(std::fabs(a - b) <= eps)) {
            ++failed;
            fails << "  FAIL: " << what << " (" << a << " vs " << b << ")\n";
        }
    }
};

ParticleCurve<f32> MakeRamp(Interp mode) {
    ParticleCurve<f32> c;
    c.SetInterp(mode);
    c.AddKey(0.0f, 0.0f);
    c.AddKey(0.5f, 10.0f);
    c.AddKey(1.0f, 4.0f);
    return c;
}

void TestCurveEndpoints(Checker& k) {
    const Interp modes[] = {Interp::Step, Interp::Linear, Interp::Hermite, Interp::Bezier,
                            Interp::CatmullRom};
    for (Interp m : modes) {
        auto c = MakeRamp(m);
        // Unbiased curves must reproduce their first and last key exactly —
        // otherwise a particle's colour pops at birth or death.
        k.CheckNear(c.Evaluate(0.0f), 0.0f, 1e-6f, "curve Evaluate(0) == first key");
        k.CheckNear(c.Evaluate(1.0f), 4.0f, 1e-6f, "curve Evaluate(1) == last key");
    }
}

void TestCurveHintIndependence(Checker& k) {
    auto c = MakeRamp(Interp::Linear);
    bool same = true;
    // Sweep including the exact key times, which is where a mismatched
    // forward/backward comparison used to make the hint observable.
    for (i32 i = 0; i <= 200; ++i) {
        const f32 t = static_cast<f32>(i) / 200.0f;
        const f32 ref = c.Evaluate(t, 0);
        for (u32 hint = 0; hint < 5; ++hint)
            if (c.Evaluate(t, hint) != ref)
                same = false;
    }
    k.Check(same, "curve evaluation is independent of the cursor hint");
}

void TestCurveDegenerate(Checker& k) {
    ParticleCurve<f32> empty;
    k.CheckNear(empty.Evaluate(0.5f), 0.0f, 1e-6f, "empty curve evaluates to zero");

    ParticleCurve<f32> one;
    one.AddKey(0.3f, 7.0f);
    k.CheckNear(one.Evaluate(0.0f), 7.0f, 1e-6f, "single-key curve is constant");
    k.CheckNear(one.Evaluate(1.0f), 7.0f, 1e-6f, "single-key curve is constant at t=1");
}

void TestCurveMonotonic(Checker& k) {
    ParticleCurve<f32> c;
    c.SetInterp(Interp::Linear);
    c.AddKey(0.0f, 0.0f);
    c.AddKey(1.0f, 1.0f);
    bool monotonic = true;
    f32 prev = -1.0f;
    for (i32 i = 0; i <= 100; ++i) {
        const f32 v = c.Evaluate(static_cast<f32>(i) / 100.0f);
        if (v < prev - 1e-6f)
            monotonic = false;
        prev = v;
    }
    k.Check(monotonic, "linear curve over a rising ramp is monotonic");
}

void TestBias(Checker& k) {
    // WC3 samples each segment over [bias, 1-bias] rather than [0, 1].
    k.CheckNear(ApplyBias(0.0f, 0.005f), 0.005f, 1e-6f, "bias insets the segment start");
    k.CheckNear(ApplyBias(1.0f, 0.005f), 0.995f, 1e-6f, "bias insets the segment end");
    k.CheckNear(ApplyBias(0.5f, 0.005f), 0.5f, 1e-6f, "bias leaves the midpoint alone");
    k.CheckNear(ApplyBias(0.25f, 0.0f), 0.25f, 1e-6f, "zero bias is the identity");
}

void TestCellTrack(Checker& k) {
    CellAnimTrack track;
    track.AddSegment(0.5f, 0, 3, 1);
    track.AddSegment(1.0f, 4, 7, 1);

    bool inRange = true;
    for (i32 i = 0; i <= 100; ++i) {
        const f32 t = static_cast<f32>(i) / 100.0f;
        const i32 cell = track.Evaluate(t);
        if (cell < 0 || cell > 7)
            inRange = false;
    }
    k.Check(inRange, "cell track stays within its declared cell range");
    k.Check(track.Evaluate(0.0f) >= 0, "cell track start is non-negative");
}

void TestShapes(Checker& k) {
    RndSeed rnd(12345u);

    SpawnParams p;
    p.width = 10.0f;
    p.height = 6.0f;
    p.latitude = 0.5f;
    p.longitude = 6.2831853f;
    p.speed.base = 3.0f;
    p.speed.variance = 0.0f; // pin the speed so magnitude is checkable

    PlaneShape plane;
    bool inBounds = true;
    bool speedOk = true;
    for (i32 i = 0; i < 500; ++i) {
        SpawnSample s;
        plane.Sample(s, p, rnd);
        if (std::fabs(s.localPos.x) > p.width * 0.5f + 1e-4f ||
            std::fabs(s.localPos.y) > p.height * 0.5f + 1e-4f || s.localPos.z != 0.0f)
            inBounds = false;
        const f32 mag = std::sqrt(s.localVel.x * s.localVel.x + s.localVel.y * s.localVel.y +
                                  s.localVel.z * s.localVel.z);
        if (std::fabs(mag - p.speed.base) > 1e-3f)
            speedOk = false;
    }
    k.Check(inBounds, "plane shape samples inside its width/height rectangle");
    k.Check(speedOk, "plane shape velocity magnitude equals the drawn speed");

    ConeShape cone;
    bool atOrigin = true;
    bool coneSpeedOk = true;
    for (i32 i = 0; i < 500; ++i) {
        SpawnSample s;
        cone.Sample(s, p, rnd);
        if (s.localPos.x != 0.0f || s.localPos.y != 0.0f || s.localPos.z != 0.0f)
            atOrigin = false;
        const f32 mag = std::sqrt(s.localVel.x * s.localVel.x + s.localVel.y * s.localVel.y +
                                  s.localVel.z * s.localVel.z);
        if (std::fabs(mag - p.speed.base) > 1e-3f)
            coneSpeedOk = false;
    }
    k.Check(atOrigin, "cone shape spawns at the origin");
    k.Check(coneSpeedOk, "cone shape velocity magnitude equals the drawn speed");
}

} // namespace

bool RunParticleSelfTest(std::string& report) {
    Checker k;
    TestCurveEndpoints(k);
    TestCurveHintIndependence(k);
    TestCurveDegenerate(k);
    TestCurveMonotonic(k);
    TestBias(k);
    TestCellTrack(k);
    TestShapes(k);

    std::ostringstream os;
    os << k.ran - k.failed << "/" << k.ran << " checks passed";
    if (k.failed > 0)
        os << "\n" << k.fails.str();
    report = os.str();
    return k.failed == 0;
}

} // namespace whiteout::flakes::renderer::particle
