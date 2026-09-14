// ============================================================================
// SC2 ribbon load plumbing (RIBBON_SERVICE_PLAN.md W2, device-free).
//
// Three kinds of case:
//   * conversion — DescFromSc2Config's load-time rules (drag floor, midTime
//     clamp, spline record 0), pinned synthetically.
//   * surface table — BuildM3SurfaceTable's appended per-RIB_ block, on a
//     synthetic model, because a material only a ribbon references has no
//     geoset row and the whole point is that it gets one anyway.
//   * corpus — every `RIB_` record in the HotS ribbon corpus registers, and
//     the value-range oracles that would catch the emitterShape/ribbonType
//     label swap (SC2_RIBBON_RE.md §1.1): a swapped mapping puts cross-section
//     values 2/3 into cullMethod, which only ever holds 0/1.
//     Pointed DIRECTLY at HotSM3 — generic M3 sweeps fill their limit from
//     the 33k Sc2 files and never reach Heroes.
// ============================================================================

#include "io/m3/m3_model_adapter.h"
#include "renderer/profiles/sc2_heroes/m3_surface_table.h"
#include "renderer/ribbon/ribbon_emitter.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace whiteout;
using namespace whiteout::flakes;
using namespace whiteout::flakes::renderer::ribbon;
using namespace whiteout::flakes::renderer::profiles::sc2_heroes;
namespace wio = whiteout::flakes::io;
namespace fs = std::filesystem;

namespace {

fs::path HotsCorpus() {
    if (const char* v = std::getenv("WDX_TEST_SC2_CORPUS"); v && *v)
        return fs::path(v) / "HotSM3";
    return fs::path("C:/Projects/WhiteoutLib/Corpus/HotSM3");
}

std::vector<u8> ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::vector<u8>(std::istreambuf_iterator<char>(f), {});
}

} // namespace

// ---------------------------------------------------------------------------
// Time-mode simulation invariants (RIBBON_SERVICE_PLAN.md W3). The per-element
// launch state is oracle-gated in sc2_ribbon_oracle_test; here the FRAME LOOP
// is checked for the structural properties RIBBON_SERVICE.md §5.1 promises —
// the headU clock, the divisions density, deathU = birthU + lifetime, and the
// retire walk. These are design-driven (the caller-side headU advance is not
// oracle-pinned), so they gate structure, not bit-parity.
// ---------------------------------------------------------------------------
namespace {

RibbonDesc Sc2TimeDesc(f32 divisions, f32 mass = 1.0f) {
    RibbonDesc d;
    d.family = RibbonDesc::Family::Sc2;
    d.sc2.simTechnique = SimTechnique::GpuOnly;
    d.sc2.cullMethod = CullMethod::Time;
    d.sc2.ribbonType = whiteout::m3::RibbonType::Billboard;
    d.sc2.divisions = divisions;
    d.sc2.mass = mass;
    return d;
}

whiteout::Matrix44f TranslationAt(f32 x, f32 y, f32 z) {
    whiteout::Matrix44f m = whiteout::Matrix44f::identity();
    m.data[3][0] = x;
    m.data[3][1] = y;
    m.data[3][2] = z;
    return m;
}

RibbonState Sc2FrameAt(f32 x, f32 lifetime, bool active = true) {
    RibbonState st;
    st.transform = TranslationAt(x, 0.0f, 0.0f);
    st.visibility = 1.0f;
    st.sc2.speed = 3.0f;
    st.sc2.yawDeg = 20.0f;
    st.sc2.pitchDeg = 10.0f;
    st.sc2.lifetime = lifetime;
    st.sc2.size3 = {2.0f, 2.0f, 2.0f};
    st.sc2.active = active;
    return st;
}

} // namespace

TEST_CASE("sc2 time-mode trail ages monotonically and holds ~divisions segments",
          "[ribbon][sc2_ribbon]") {
    const f32 lifetime = 1.0f;
    const f32 divisions = 10.0f;
    RibbonEmitter e;
    e.SetDesc(Sc2TimeDesc(divisions));

    const f32 dt = 1.0f / 60.0f;
    f32 x = 0.0f;
    for (int i = 0; i < 240; ++i) { // 4 s of travel
        x += 2.0f * dt;
        e.SetState(Sc2FrameAt(x, lifetime));
        e.Update(dt);
    }

    const auto& edges = e.Edges();
    REQUIRE(edges.size() >= 2);
    // divisions segments live over one lifetime; the fractional accumulator and
    // the provisional head leave a small band.
    CHECK(edges.size() >= static_cast<std::size_t>(divisions) - 3);
    CHECK(edges.size() <= static_cast<std::size_t>(divisions) + 3);

    for (std::size_t i = 0; i + 1 < edges.size(); ++i) {
        INFO("segment " << i);
        CHECK(edges[i].birthU < edges[i + 1].birthU);          // monotonic
        CHECK(edges[i].deathU >= edges[i].birthU);
    }
}

TEST_CASE("sc2 time-mode deathU is birthU plus the sampled lifetime",
          "[ribbon][sc2_ribbon]") {
    const f32 lifetime = 0.8f;
    RibbonEmitter e;
    e.SetDesc(Sc2TimeDesc(12.0f));

    f32 x = 0.0f;
    for (int i = 0; i < 120; ++i) {
        x += 0.05f;
        e.SetState(Sc2FrameAt(x, lifetime));
        e.Update(1.0f / 60.0f);
    }
    for (const auto& seg : e.Edges())
        CHECK(seg.deathU - seg.birthU == Catch::Approx(lifetime).epsilon(1e-5));
}

TEST_CASE("sc2 inactive stops new segments while the live trail finishes",
          "[ribbon][sc2_ribbon]") {
    const f32 lifetime = 1.0f;
    RibbonEmitter e;
    e.SetDesc(Sc2TimeDesc(10.0f));

    f32 x = 0.0f;
    for (int i = 0; i < 120; ++i) { // fill for 2 s
        x += 0.05f;
        e.SetState(Sc2FrameAt(x, lifetime, true));
        e.Update(1.0f / 60.0f);
    }
    const std::size_t filled = e.Edges().size();
    REQUIRE(filled >= 2);

    // Deactivate: no new segments, but the trail keeps aging out. After a full
    // lifetime the trail must have drained.
    for (int i = 0; i < 90; ++i) { // 1.5 s inactive
        x += 0.05f;
        e.SetState(Sc2FrameAt(x, lifetime, false));
        e.Update(1.0f / 60.0f);
    }
    CHECK(e.Edges().size() < filled);
    CHECK(e.Edges().empty());
}

TEST_CASE("sc2 catch-up pre-populates the trail on the first tick",
          "[ribbon][sc2_ribbon]") {
    RibbonEmitter e;
    e.SetDesc(Sc2TimeDesc(10.0f));
    // One seeding SetState then one Update: without catch-up the trail would
    // hold at most a segment or two; the pre-roll lays the age distribution.
    e.SetState(Sc2FrameAt(0.0f, 1.0f));
    e.Update(1.0f / 60.0f);
    CHECK(e.Edges().size() >= 5);
}

TEST_CASE("sc2 BuildStage emits a strip and one m3-surface draw record",
          "[ribbon][sc2_ribbon]") {
    RibbonDesc d = Sc2TimeDesc(10.0f);
    d.sc2.m3Surface = 4; // pretend the loader resolved a surface
    d.priorityPlane = 7;
    RibbonEmitter e;
    e.SetDesc(d);

    f32 x = 0.0f;
    for (int i = 0; i < 120; ++i) {
        x += 0.05f;
        e.SetState(Sc2FrameAt(x, 1.0f));
        e.Update(1.0f / 60.0f);
    }
    const std::size_t n = e.Edges().size();
    REQUIRE(n >= 2);

    RibbonBuildContext ctx;
    ctx.cameraDir = {0.0f, -1.0f, 0.0f};
    std::vector<renderer::Vertex> verts;
    std::vector<RibbonDrawList> draws;
    const i32 added = e.BuildStage(ctx, verts, draws);

    // One quad (6 verts) per adjacent node pair, over the n frozen edges plus
    // the live head BuildStage synthesises at the emitter — which keeps the
    // leading edge anchored between spawns — so n quads.
    CHECK(added == static_cast<i32>(n * 6));
    CHECK(verts.size() == static_cast<std::size_t>(added));
    REQUIRE(draws.size() == 1);
    CHECK(draws[0].m3Surface == 4);
    CHECK(draws[0].priorityPlane == 7);
    CHECK(draws[0].vertexCount == added);

    for (const auto& v : verts) {
        CHECK(std::isfinite(v.position.x));
        CHECK(std::isfinite(v.position.y));
        CHECK(std::isfinite(v.position.z));
        CHECK(v.uv.y >= 0.0f);   // V = fAge is saturated to [0,1]
        CHECK(v.uv.y <= 1.0f);
    }
}

TEST_CASE("sc2 BuildStage yields nothing for an unbuilt trail",
          "[ribbon][sc2_ribbon]") {
    RibbonEmitter e;
    e.SetDesc(Sc2TimeDesc(10.0f));
    RibbonBuildContext ctx;
    std::vector<renderer::Vertex> verts;
    std::vector<RibbonDrawList> draws;
    CHECK(e.BuildStage(ctx, verts, draws) == 0);
    CHECK(verts.empty());
    CHECK(draws.empty());
}

// ---------------------------------------------------------------------------
// Spline ribbons (RIBBON_SERVICE_PLAN.md W5). A spline is a whole-ribbon shape
// rebuilt from four Bezier control points each frame, not a trail of aged
// segments — so there are no elements, exactly 32 samples, and V = 1 − t. The
// control-point construction is verified against CRibbon_Simulate_Spline.
// ---------------------------------------------------------------------------
namespace {

RibbonDesc Sc2SplineDesc(whiteout::m3::RibbonType ribbonType =
                             whiteout::m3::RibbonType::Billboard) {
    RibbonDesc d;
    d.family = RibbonDesc::Family::Sc2;
    d.sc2.simTechnique = SimTechnique::Spline;
    d.sc2.ribbonType = ribbonType;
    d.sc2.edges = 5;
    d.sc2.hasSpline = true;
    d.sc2.spline.emissionOffset = {0, 0, 0}; // C0 at the emitter origin
    d.sc2.spline.emissionVector = {0, 0, 1}; // start tangent
    d.sc2.spline.endOffset = {0, 0, 0};      // C3 at the node origin
    d.sc2.spline.endTangent = {0, 0, 1};     // end tangent
    return d;
}

RibbonState Sc2SplineFrame(f32 lifetime = 1.0f) {
    RibbonState st;
    st.transform = whiteout::Matrix44f::identity();       // emitter at origin
    st.sc2.splineNodeTransform = TranslationAt(10, 0, 0); // end frame at x=10
    st.visibility = 1.0f;
    st.sc2.lifetime = lifetime;
    st.sc2.size3 = {2, 2, 2};
    st.sc2.velocityBaseFactor = 1.0f;
    st.sc2.velocityEndFactor = 1.0f;
    st.sc2.color3[0] = {1, 0, 0, 1}; // start red
    st.sc2.color3[2] = {0, 0, 1, 1}; // end blue
    return st;
}

} // namespace

TEST_CASE("sc2 spline builds a 32-sample strip spanning its control hull",
          "[ribbon][sc2_ribbon]") {
    RibbonDesc d = Sc2SplineDesc();
    d.sc2.m3Surface = 2;
    RibbonEmitter e;
    e.SetDesc(d);
    e.SetState(Sc2SplineFrame());
    e.Update(1.0f / 60.0f);

    // A spline emits no per-segment elements — the strip is regenerated whole.
    CHECK(e.Edges().empty());

    RibbonBuildContext ctx;
    ctx.cameraDir = {0.0f, -1.0f, 0.0f};
    std::vector<renderer::Vertex> verts;
    std::vector<RibbonDrawList> draws;
    const i32 added = e.BuildStage(ctx, verts, draws);

    // 32 samples, billboard section → (32 − 1) quads × 6 verts.
    CHECK(added == 31 * 6);
    REQUIRE(draws.size() == 1);
    CHECK(draws[0].m3Surface == 2);

    f32 minX = 1e9f, maxX = -1e9f, vHead = -1.0f, vTail = -1.0f;
    for (const auto& v : verts) {
        CHECK(std::isfinite(v.position.x));
        CHECK(std::isfinite(v.position.z));
        CHECK(v.uv.y >= -1e-4f);
        CHECK(v.uv.y <= 1.0f + 1e-4f);
        minX = (std::min)(minX, v.position.x);
        maxX = (std::max)(maxX, v.position.x);
    }
    vHead = verts.front().uv.y; // node 0 (t=0): V = 1 − 0
    vTail = verts.back().uv.y;  // node 31 (t=1): V = 1 − 1
    // C0 sits at x≈0, C3 at x≈10, so the strip spans the hull.
    CHECK(minX < 1.0f);
    CHECK(maxX > 9.0f);
    // V = 1 − t: full at the start control point, zero at the end.
    CHECK(vHead == Catch::Approx(1.0f).margin(1e-4f));
    CHECK(vTail == Catch::Approx(0.0f).margin(1e-4f));
}

TEST_CASE("sc2 star cross-section has 2x the ring vertices of a cylinder",
          "[ribbon][sc2_ribbon]") {
    // BuildCrossSection type 3: a star ring holds 2·edges points (one inner + one
    // outer per authored edge), vs a cylinder's edges. Same 32 spline samples and
    // edge count → the star strip has exactly twice the vertices.
    RibbonBuildContext ctx;
    ctx.cameraDir = {0.0f, -1.0f, 0.0f};
    const auto build = [&](whiteout::m3::RibbonType ribbonType) {
        RibbonEmitter e;
        e.SetDesc(Sc2SplineDesc(ribbonType)); // edges = 5
        e.SetState(Sc2SplineFrame());
        e.Update(1.0f / 60.0f);
        std::vector<renderer::Vertex> verts;
        std::vector<RibbonDrawList> draws;
        return e.BuildStage(ctx, verts, draws);
    };
    const i32 cyl = build(whiteout::m3::RibbonType::Cylinder);  // 31 rungs × 5 edges × 6
    const i32 star = build(whiteout::m3::RibbonType::Star);     // 31 rungs × 2·5 edges × 6
    CHECK(cyl == 31 * 5 * 6);
    CHECK(star == 31 * 10 * 6);
    CHECK(star == 2 * cyl);
}

TEST_CASE("sc2 spline rebuilds every frame and never accumulates elements",
          "[ribbon][sc2_ribbon]") {
    RibbonEmitter e;
    e.SetDesc(Sc2SplineDesc(whiteout::m3::RibbonType::Cylinder));
    RibbonBuildContext ctx;

    i32 lastCount = -1;
    for (int i = 0; i < 300; ++i) {
        e.SetState(Sc2SplineFrame());
        e.Update(1.0f / 60.0f);
        std::vector<renderer::Vertex> verts;
        std::vector<RibbonDrawList> draws;
        const i32 added = e.BuildStage(ctx, verts, draws);
        CHECK(e.Edges().empty());          // no segment accumulation, ever
        if (lastCount >= 0)
            CHECK(added == lastCount);     // steady vertex count frame to frame
        lastCount = added;
    }
    CHECK(lastCount > 0);
}

// ---------------------------------------------------------------------------
// Overlay waves, inherit-velocity, legacy Euler, noise (RIBBON_SERVICE_PLAN.md
// W6/W4). Structural — the head kernel's wave/inherit math is oracle-adjacent
// (Sc2WriteHead's W3 subset is bit-gated; the wave paths are inert at type 0),
// so these gate that the feature MOVES the right thing, not bit-parity.
// ---------------------------------------------------------------------------

TEST_CASE("sc2 wave sampler matches M3_SampleAnimValue", "[ribbon][sc2_ribbon]") {
    constexpr f32 kPi = 3.14159265f;
    // 0 off; 1 sin·amp; 2 cos·amp; 4 square (±amp about frac 0.5).
    CHECK(sc2::SampleWave(0, 1.23f, 5.0f) == 0.0f);
    CHECK(sc2::SampleWave(1, 0.0f, 5.0f) == Catch::Approx(0.0f).margin(1e-5));
    CHECK(sc2::SampleWave(1, kPi * 0.5f, 5.0f) == Catch::Approx(5.0f).margin(1e-4));
    CHECK(sc2::SampleWave(2, 0.0f, 5.0f) == Catch::Approx(5.0f).margin(1e-4));
    CHECK(sc2::SampleWave(4, 0.25f, 3.0f) == Catch::Approx(3.0f));  // frac < 0.5
    CHECK(sc2::SampleWave(4, 0.75f, 3.0f) == Catch::Approx(-3.0f)); // frac > 0.5
    // 3 sawtooth: amp·(2·fmod(phase,1)−1), a bipolar ramp per unit period
    // (M3_SampleAnimValue case 3, K=1.0/C=−1.0 recovered from a clean disasm).
    CHECK(sc2::SampleWave(3, 0.0f, 4.0f) == Catch::Approx(-4.0f)); // ramp start
    CHECK(sc2::SampleWave(3, 0.5f, 4.0f) == Catch::Approx(0.0f));  // mid
    CHECK(sc2::SampleWave(3, 0.75f, 4.0f) == Catch::Approx(2.0f));
    CHECK(sc2::SampleWave(3, 1.25f, 4.0f) == Catch::Approx(-2.0f)); // wraps
    // 5 is our deterministic stand-in for retail's RNG: same phase → same value,
    // always inside [−amp, amp].
    const f32 r1 = sc2::SampleWave(5, 2.5f, 4.0f);
    CHECK(sc2::SampleWave(5, 2.5f, 4.0f) == r1);
    CHECK(std::abs(r1) <= 4.0f);
}

TEST_CASE("sc2 yaw overlay wave wobbles the emitted trail", "[ribbon][sc2_ribbon]") {
    // A stationary emitter lays segments with a constant launch direction — so
    // any spread in segment velocity comes from the overlay wave alone.
    const auto spread = [](RibbonEmitter& e) {
        f32 lo = 1e9f, hi = -1e9f;
        for (const auto& s : e.Edges()) {
            lo = (std::min)(lo, s.velocity.x);
            hi = (std::max)(hi, s.velocity.x);
        }
        return hi - lo;
    };

    RibbonEmitter base;
    base.SetDesc(Sc2TimeDesc(12.0f));
    RibbonDesc waved = Sc2TimeDesc(12.0f);
    waved.sc2.waveTypes[0] = 1; // yaw sin wave
    RibbonEmitter wav;
    wav.SetDesc(waved);

    for (int i = 0; i < 120; ++i) {
        RibbonState st = Sc2FrameAt(0.0f, 1.0f); // stationary
        base.SetState(st);
        base.Update(1.0f / 60.0f);
        st.sc2.waveAmp[0] = 40.0f;  // ±40° of yaw swing
        st.sc2.waveFreq[0] = 6.28f; // ~one cycle per second of birthU
        wav.SetState(st);
        wav.Update(1.0f / 60.0f);
    }
    // Constant yaw → identical launch velocity across the trail; the wave makes
    // the segments fan out.
    CHECK(spread(base) == Catch::Approx(0.0f).margin(1e-4));
    CHECK(spread(wav) > 0.5f);
}

TEST_CASE("sc2 inherit-velocity adds the emitter's motion to the launch",
          "[ribbon][sc2_ribbon]") {
    // World-space trail; move the emitter along +x so its smoothed velocity is
    // +x. With inherit (flags & 0x10) the segments launch faster in +x.
    const auto run = [](bool inherit) {
        RibbonDesc d = Sc2TimeDesc(12.0f);
        d.sc2.additionalFlags = whiteout::m3::RibbonAdditionalFlag::WorldSpace;
        if (inherit)
            d.sc2.flags |= whiteout::m3::RibbonFlag::InheritParentVelocity;
        RibbonEmitter e;
        e.SetDesc(d);
        f32 x = 0.0f;
        for (int i = 0; i < 120; ++i) {
            x += 5.0f / 60.0f; // 5 units/s along +x
            RibbonState st = Sc2FrameAt(x, 1.0f);
            st.sc2.parentVelocityScale = 1.0f;
            e.SetState(st);
            e.Update(1.0f / 60.0f);
        }
        f32 sum = 0.0f;
        for (const auto& s : e.Edges())
            sum += s.velocity.x;
        const std::size_t count = e.Edges().empty() ? 1 : e.Edges().size();
        return sum / static_cast<f32>(count);
    };
    // The inherited ~+5 units/s in x lifts the mean launch velocity.x well past
    // the no-inherit baseline.
    CHECK(run(true) > run(false) + 2.0f);
}

TEST_CASE("sc2 local-space ribbons still inherit the emitter's motion (DRIFT-2)",
          "[ribbon][sc2_ribbon]") {
    // The binary gates inherit on flags & 0x10 ALONE — a LOCAL ribbon (no
    // additionalFlags & 8) whose emitter moves accumulates a world smoothedDir
    // that adds to the raw local launch velocity. The old code required world
    // space and dropped inherit entirely for local ribbons.
    const auto run = [](bool inherit) {
        RibbonDesc d = Sc2TimeDesc(12.0f);
        d.sc2.additionalFlags = whiteout::m3::RibbonAdditionalFlag::None; // local-space
        if (inherit)
            d.sc2.flags |= whiteout::m3::RibbonFlag::InheritParentVelocity;
        RibbonEmitter e;
        e.SetDesc(d);
        f32 x = 0.0f;
        for (int i = 0; i < 120; ++i) {
            x += 5.0f / 60.0f; // emitter slides +x at 5 u/s
            RibbonState st = Sc2FrameAt(x, 1.0f);
            st.sc2.parentVelocityScale = 1.0f;
            e.SetState(st);
            e.Update(1.0f / 60.0f);
        }
        f32 sum = 0.0f;
        for (const auto& s : e.Edges())
            sum += s.velocity.x;
        const std::size_t count = e.Edges().empty() ? 1 : e.Edges().size();
        return sum / static_cast<f32>(count);
    };
    CHECK(run(true) > run(false) + 2.0f);
}

TEST_CASE("sc2 stationary floor runs on the post-transform velocity (DRIFT-1)",
          "[ribbon][sc2_ribbon]") {
    // World-space tech-0 ribbon whose LOCAL launch speed is below the 1e-4²
    // gate, but the emitter transform scales it 10x. The binary floors the WORLD
    // velocity (after transform + inherit), so the 10x scale lifts |v|² past the
    // gate and the true small velocity survives — magnified, NOT clamped to
    // direction·1e-4. The old code floored in local, pre-transform space.
    RibbonDesc d = Sc2TimeDesc(12.0f);
    d.sc2.additionalFlags = whiteout::m3::RibbonAdditionalFlag::WorldSpace; // GpuOnly floors
    RibbonEmitter e;
    e.SetDesc(d);
    constexpr f32 scale = 10.0f, baseSpeed = 0.005f; // 0.005² = 2.5e-5 < 1e-4
    for (int i = 0; i < 30; ++i) {
        RibbonState st = Sc2FrameAt(static_cast<f32>(i), 1.0f);
        st.transform = TranslationAt(static_cast<f32>(i), 0.0f, 0.0f);
        st.transform.data[0][0] = scale;
        st.transform.data[1][1] = scale;
        st.transform.data[2][2] = scale;
        st.sc2.speed = baseSpeed;
        e.SetState(st);
        e.Update(1.0f / 60.0f);
    }
    REQUIRE(!e.Edges().empty());
    const auto& v = e.Edges().back().velocity; // head segment
    const f32 mag = std::sqrt((v.x * v.x + v.y * v.y) + v.z * v.z);
    // World magnitude ≈ scale·baseSpeed (0.05), not the floored scale·1e-4 (1e-3).
    CHECK(mag == Catch::Approx(scale * baseSpeed).margin(0.005f));
    CHECK(mag > 0.02f);
}

TEST_CASE("sc2 UseLengthAndTime (flags & 0x1000) maxes length-V with time-V (DRIFT-3)",
          "[ribbon][sc2_ribbon]") {
    // A slow world-space length-mode legacy trail: the arc is a tiny fraction of
    // maxLength (small length-V), but the segments are old (large time-V). The
    // binary maxes the two when flags & 0x1000 is set (Simulate_Type4 phase 2);
    // the old code ignored the flag and used the length term only, so the 0x1000
    // build lifts V toward the age fraction.
    const auto maxV = [](bool useLenAndTime) {
        RibbonDesc d = Sc2TimeDesc(100.0f);
        d.sc2.simTechnique = SimTechnique::Legacy;
        d.sc2.cullMethod = CullMethod::Length;
        // world-space: the arc IS the emitter path
        d.sc2.additionalFlags = whiteout::m3::RibbonAdditionalFlag::WorldSpace;
        if (useLenAndTime)
            d.sc2.flags |= whiteout::m3::RibbonFlag::UseLengthAndTime;
        RibbonEmitter e;
        e.SetDesc(d);
        f32 x = 0.0f;
        for (int i = 0; i < 90; ++i) { // 1.5 s
            x += 0.5f / 60.0f;         // 0.5 u/s: short arc vs maxLength 10
            RibbonState st = Sc2FrameAt(x, 1.0f);
            st.sc2.speed = 0.5f;
            st.sc2.maxLength = 10.0f;
            e.SetState(st);
            e.Update(1.0f / 60.0f);
        }
        RibbonBuildContext ctx;
        ctx.cameraDir = {0.0f, -1.0f, 0.0f};
        std::vector<renderer::Vertex> verts;
        std::vector<RibbonDrawList> draws;
        e.BuildStage(ctx, verts, draws);
        f32 mx = 0.0f;
        for (const auto& v : verts)
            mx = (std::max)(mx, v.uv.y);
        return mx;
    };
    CHECK(maxV(true) > maxV(false) + 0.2f);
}

TEST_CASE("sc2 legacy (tech 4) integrates gravity and drag", "[ribbon][sc2_ribbon]") {
    RibbonDesc d = Sc2TimeDesc(12.0f);
    d.sc2.simTechnique = SimTechnique::Legacy;
    d.sc2.additionalFlags = whiteout::m3::RibbonAdditionalFlag::WorldSpace;
    d.sc2.gravity3 = {0, 0, -10.0f};
    d.sc2.drag = 1.0f;
    d.sc2.mass = 1.0f;
    RibbonEmitter e;
    e.SetDesc(d);

    f32 x = 0.0f;
    for (int i = 0; i < 120; ++i) { // 2 s
        x += 1.0f / 60.0f;
        RibbonState st = Sc2FrameAt(x, 2.0f);
        st.sc2.speed = 0.5f;
        e.SetState(st);
        e.Update(1.0f / 60.0f);
    }
    REQUIRE(e.Edges().size() >= 3);
    // The oldest segment (front) has integrated gravity longest, so it has
    // fallen below its birth height and below the youngest segment.
    const auto& oldest = e.Edges().front();
    const auto& youngest = e.Edges().back();
    CHECK(oldest.pos.z < oldest.birthPos.z - 0.1f);
    CHECK(oldest.pos.z < youngest.pos.z);
    // Drag keeps the fall from free-falling away: |v_z| stays bounded near the
    // terminal velocity (gravity / (drag·invMass) = 10), not 10·age.
    CHECK(std::abs(oldest.velocity.z) < 12.0f);
}

TEST_CASE("sc2 legacy (tech 4) segments collide with the ground grid",
          "[ribbon][sc2_ribbon]") {
    // A plane at z = 0, the grid the viewer stands in for the map colliders.
    const auto planeQuery = [](const Vector3f&, f32, f32, f32& outZ) {
        outZ = 0.0f;
        return true;
    };
    // A world-space legacy ribbon born 5 units up, falling under gravity. The
    // terrain-collision flag (0x2) is what forces tech 4 and gates the collide.
    RibbonDesc d = Sc2TimeDesc(12.0f);
    d.sc2.simTechnique = SimTechnique::Legacy;
    d.sc2.additionalFlags = whiteout::m3::RibbonAdditionalFlag::WorldSpace;
    d.sc2.flags = whiteout::m3::RibbonFlag::CollideTerrain;
    d.sc2.gravity3 = {0, 0, -10.0f};
    d.sc2.friction = 1.0f;
    d.sc2.bounce = 0.0f;

    const auto run = [&](bool withQuery, whiteout::m3::RibbonFlag flags) {
        RibbonDesc dd = d;
        dd.sc2.flags = flags;
        RibbonEmitter e;
        e.SetDesc(dd);
        for (int i = 0; i < 120; ++i) { // 2 s
            RibbonState st = Sc2FrameAt(0.0f, 2.0f);
            st.transform = TranslationAt(0.0f, 0.0f, 5.0f);
            st.sc2.speed = 0.5f;
            if (withQuery)
                st.groundQuery = planeQuery;
            e.SetState(st);
            e.Update(1.0f / 60.0f);
        }
        return e.Edges().front().pos.z; // oldest segment, fallen longest
    };

    // With the grid query the oldest segment settles on the surface (contact at
    // the collide radius, 0.03) instead of falling through.
    CHECK(run(true, whiteout::m3::RibbonFlag::CollideTerrain) == Catch::Approx(0.03f).margin(0.1f));
    // No query, and flag clear with a query: both fall well past the ground.
    CHECK(run(false, whiteout::m3::RibbonFlag::CollideTerrain) < -1.0f);
    CHECK(run(true, whiteout::m3::RibbonFlag::None) < -1.0f);

    // Local-space ribbon (no 0x8): born at the origin in local space, it collides
    // in scene space through the emitter transform and maps the result back, so
    // its scene-space height still settles on the grid.
    {
        RibbonDesc dl = d;
        dl.sc2.additionalFlags = whiteout::m3::RibbonAdditionalFlag::None; // local
        RibbonEmitter e;
        e.SetDesc(dl);
        const Matrix44f xf = TranslationAt(0.0f, 0.0f, 5.0f);
        for (int i = 0; i < 120; ++i) {
            RibbonState st = Sc2FrameAt(0.0f, 2.0f);
            st.transform = xf;
            st.sc2.speed = 0.5f;
            st.groundQuery = planeQuery;
            e.SetState(st);
            e.Update(1.0f / 60.0f);
        }
        const Vector3f sceneZ = whiteout::transform_point(e.Edges().front().pos, xf);
        CHECK(sceneZ.z == Catch::Approx(0.03f).margin(0.1f));
    }
}

TEST_CASE("sc2 noise displaces the built strip and stays deterministic",
          "[ribbon][sc2_ribbon]") {
    const auto build = [](bool noise, std::vector<renderer::Vertex>& verts) {
        RibbonDesc d = Sc2TimeDesc(12.0f);
        d.sc2.simTechnique = SimTechnique::Legacy; // noise forces legacy
        if (noise) {
            d.sc2.noiseAmplitude = 2.0f;
            d.sc2.noiseFrequency = 3.0f;
            d.sc2.noiseCoherence = 1.0f;
            d.sc2.noiseEdge = 0.3f;
        }
        RibbonEmitter e;
        e.SetDesc(d);
        f32 x = 0.0f;
        for (int i = 0; i < 120; ++i) {
            x += 2.0f / 60.0f;
            e.SetState(Sc2FrameAt(x, 1.0f));
            e.Update(1.0f / 60.0f);
        }
        RibbonBuildContext ctx;
        std::vector<RibbonDrawList> draws;
        e.BuildStage(ctx, verts, draws);
    };

    std::vector<renderer::Vertex> plain, noisy1, noisy2;
    build(false, plain);
    build(true, noisy1);
    build(true, noisy2);
    REQUIRE(!plain.empty());
    REQUIRE(noisy1.size() == noisy2.size());

    // Noise moves vertices off the clean trail...
    f32 maxDelta = 0.0f;
    const std::size_t n = (std::min)(plain.size(), noisy1.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto& a = plain[i].position;
        const auto& b = noisy1[i].position;
        maxDelta = (std::max)(maxDelta,
                              std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z));
    }
    CHECK(maxDelta > 0.05f);
    // ...but a fixed timeline reproduces the same stream bit-for-bit.
    bool identical = true;
    for (std::size_t i = 0; i < noisy1.size(); ++i) {
        const auto& a = noisy1[i].position;
        const auto& b = noisy2[i].position;
        identical = identical && a.x == b.x && a.y == b.y && a.z == b.z;
    }
    CHECK(identical);
}

TEST_CASE("sc2 spline velocity wave reshapes the control hull",
          "[ribbon][sc2_ribbon]") {
    const auto buildTail = [](bool wave) {
        RibbonDesc d = Sc2SplineDesc();
        d.sc2.spline.velocityNormFactor = 1.0f;
        if (wave)
            d.sc2.spline.waveTypes[2] = 2; // velocity cos wave (nonzero at phase 0)
        RibbonEmitter e;
        e.SetDesc(d);
        RibbonState st = Sc2SplineFrame();
        st.sc2.splineWaveAmp[2] = 5.0f;
        st.sc2.splineWaveFreq[2] = 0.0f; // constant offset for a stable check
        e.SetState(st);
        e.Update(1.0f / 60.0f);
        RibbonBuildContext ctx;
        std::vector<renderer::Vertex> verts;
        std::vector<RibbonDrawList> draws;
        e.BuildStage(ctx, verts, draws);
        f32 minZ = 1e9f, maxZ = -1e9f;
        for (const auto& v : verts) {
            minZ = (std::min)(minZ, v.position.z);
            maxZ = (std::max)(maxZ, v.position.z);
        }
        return maxZ - minZ; // the Bezier's bulge along the tangent axis (+z)
    };
    // The end-tangent (+z) length grows by velWave·normFactor, so the hull bows
    // further along z than the wave-free spline.
    CHECK(buildTail(true) > buildTail(false) + 0.5f);
}

// ---------------------------------------------------------------------------
// Conversion
// ---------------------------------------------------------------------------

TEST_CASE("sc2 desc conversion applies the engine's load-time rules",
          "[ribbon][sc2_ribbon]") {
    Sc2RibbonEmitterConfig cfg;
    cfg.drag = 0.001f;      // below the engine's floor
    cfg.midTime[0] = 1.0f;  // would divide by zero in the VS second piece
    cfg.midTime[1] = 0.25f; // untouched
    cfg.ribbonType = 3;
    cfg.edges = 7;
    cfg.innerRadius = 0.25f;

    const RibbonDesc d = DescFromSc2Config(cfg);
    CHECK(d.family == RibbonDesc::Family::Sc2);
    CHECK(d.sc2.drag == 0.01f);
    CHECK(d.sc2.midTime[0] == 0.996f);
    CHECK(d.sc2.midTime[1] == 0.25f);
    CHECK(int(d.sc2.ribbonType) == 3);
    CHECK(d.sc2.edges == 7);
    CHECK_FALSE(d.sc2.hasSpline);
    CHECK(d.sc2.m3Surface == -1); // the loader stamps it, not the conversion
}

TEST_CASE("sc2 desc keeps SRIB record 0 alone", "[ribbon][sc2_ribbon]") {
    // splineData = splineRibbons.ptr[0]: extra records resolve but are never
    // read (RE §3.4), so the desc carries exactly the first.
    Sc2RibbonEmitterConfig cfg;
    Sc2SplineRibbonConfig s0, s1;
    s0.boneIndex = 4;
    s0.emissionOffset = {1, 2, 3};
    s1.boneIndex = 9;
    cfg.splines = {s0, s1};

    const RibbonDesc d = DescFromSc2Config(cfg);
    REQUIRE(d.sc2.hasSpline);
    CHECK(d.sc2.spline.boneIndex == 4);
    CHECK(d.sc2.spline.emissionOffset.x == 1.0f);
    CHECK(int(d.sc2.simTechnique) == 1);
}

// ---------------------------------------------------------------------------
// Surface table
// ---------------------------------------------------------------------------

TEST_CASE("surface table appends one row per RIB_ material ref",
          "[ribbon][sc2_ribbon]") {
    m3::Model model;

    m3::StandardMaterial geoMat;
    geoMat.name = "geoset";
    geoMat.priority = 3;
    m3::StandardMaterial ribMat;
    ribMat.name = "trail";
    ribMat.blendMode = m3::BlendMode::Add;
    ribMat.priority = 7;
    m3::TextureLayer diffuse;
    diffuse.texturePath = "trail.dds";
    diffuse.color.initValue = {255, 255, 255, 255};
    diffuse.rgbMultiply.initValue = 1.0f;
    ribMat.diffuseLayer = diffuse;
    model.standardMaterials = {geoMat, ribMat};

    m3::MaterialMap m0, m1;
    m0.materialType = m3::MaterialType::Standard;
    m0.materialIndex = 0;
    m1.materialType = m3::MaterialType::Standard;
    m1.materialIndex = 1;
    model.materialMaps = {m0, m1};

    m3::MeshDivision div;
    m3::Region region;
    region.vertexCount = 3;
    region.indexCount = 3;
    div.regions.push_back(region);
    m3::Batch batch;
    batch.regionIndex = 0;
    batch.materialIndex = 0;
    div.batches.push_back(batch);
    model.divisions = {div};

    // MATM 1 is referenced by the ribbon ONLY — no geoset names it, so
    // without the appended block it would have no surface at all.
    m3::RibbonEmitter rib;
    rib.materialIndex = 1;
    model.ribbonEmitters = {rib};

    const std::size_t regions[] = {0};
    const u32 materials[] = {0};
    auto table = BuildM3SurfaceTable(model, regions, materials);

    REQUIRE(table->Count() == 2);
    REQUIRE(table->RibbonSurfaceBase() == 1);
    const M3Surface* s = table->Surface(1);
    REQUIRE(s != nullptr);
    CHECK(s->valid);
    CHECK(s->blendMode == m3::BlendMode::Add);
    CHECK(s->priority == 7);
}

TEST_CASE("a model with no ribbons appends nothing", "[ribbon][sc2_ribbon]") {
    m3::Model model;
    m3::StandardMaterial mat;
    model.standardMaterials = {mat};
    m3::MaterialMap m0;
    m0.materialType = m3::MaterialType::Standard;
    m0.materialIndex = 0;
    model.materialMaps = {m0};
    m3::MeshDivision div;
    m3::Region region;
    div.regions.push_back(region);
    model.divisions = {div};

    const std::size_t regions[] = {0};
    const u32 materials[] = {0};
    auto table = BuildM3SurfaceTable(model, regions, materials);
    CHECK(table->Count() == 1);
    CHECK(table->RibbonSurfaceBase() == -1);
}

// ---------------------------------------------------------------------------
// Corpus
// ---------------------------------------------------------------------------

TEST_CASE("every HotS ribbon corpus RIB_ registers with sane statics",
          "[ribbon][sc2_ribbon][corpus]") {
    const fs::path root = HotsCorpus();
    if (!fs::exists(root))
        SKIP("no corpus at " + root.string());

    // Carriers are found by CONTENT, not by filename: the corpus's
    // `*Ribbon*.m3` files are cocoon/trail MESHES with no RIB_ chunk at all,
    // while the emitters live on hero and missile models. The byte scan for
    // the reversed chunk tag is the cheap pre-filter; the parse decides.
    std::size_t limit = 64;
    if (const char* v = std::getenv("WDX_TEST_M3_LIMIT"); v && *v)
        limit = static_cast<std::size_t>(std::strtoul(v, nullptr, 10));

    std::size_t models = 0, records = 0, tubes = 0, lengthMode = 0, splines = 0;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (models >= limit)
            break;
        if (!entry.is_regular_file() || entry.path().extension() != ".m3")
            continue;

        const auto bytes = ReadAll(entry.path());
        static constexpr char kTag[] = {'_', 'B', 'I', 'R'};
        if (std::search(bytes.begin(), bytes.end(), std::begin(kTag), std::end(kTag)) ==
            bytes.end())
            continue;

        const std::string name = entry.path().filename().string();
        auto adapter = wio::M3ModelAdapter::Load(
            ContentRef::FromPath(entry.path().string()), bytes);
        if (!adapter)
            continue;
        if (adapter->SourceModel().ribbonEmitters.empty())
            continue;
        ++models;

        const auto configs = adapter->GetSc2RibbonConfigs();
        // Registration parity: one config per parsed RIB_ record, in order.
        REQUIRE(configs.size() == adapter->SourceModel().ribbonEmitters.size());

        for (const auto& c : configs) {
            ++records;
            INFO(name);
            // The label-swap detector: cross-sections use 0..3, the cull
            // method only 0/1. A swapped mapping fails here on the first
            // tube-sectioned ribbon.
            CHECK(int(c.ribbonType) <= 3);
            CHECK(int(c.cullMethod) <= 1);
            CHECK(c.divisions > 0.0f);
            CHECK(c.edges >= 1);
            CHECK(c.edges <= 64);
            CHECK(c.mass > 0.0f);
            CHECK(int(c.lodReduce) <= 3);
            CHECK(int(c.lodCut) <= 3);
            tubes += (c.ribbonType >= 2) ? 1u : 0u;
            lengthMode += c.cullMethod;
            splines += c.splines.size();

            // And the desc conversion holds for every shipped record.
            const RibbonDesc d = DescFromSc2Config(c);
            CHECK(d.sc2.drag >= 0.01f);
            CHECK(d.sc2.midTime[0] <= 0.996f);
            CHECK(int(d.sc2.simTechnique) <= 4);
        }
    }
    if (models == 0)
        SKIP("no RIB_-carrying .m3 under " + root.string());
    INFO("models=" << models << " records=" << records << " tubes=" << tubes
                   << " lengthMode=" << lengthMode << " sribs=" << splines);
    CHECK(records > 0);
}

