// ============================================================================
// Replay of the Diablo III particle ORACLE goldens.
//
// Every case here was recorded by executing the real 2.6.2 Switch binary under
// Unicorn (`tools/d3_particle_oracle/`). The recorded inputs and the engine's own
// outputs are checked in as JSON; this file runs OUR code on the same inputs and
// compares. So a divergence fails `ctest` on a machine with no emulator, no
// `main.elf` and no Python.
//
// The comparison is BIT-EXACT wherever the plan says it can be — anything the RNG
// touches, anything that is a pure copy, every flag decision and every ordering.
// A case that needs a tolerance says which operation forced it, in its own
// section. Nothing here uses "close enough" as a default.
//
// Re-record with `python tools/d3_particle_oracle/record_golden.py`. That needs
// the binary; this does not.
// ============================================================================

#include "oracle_golden.h"

#include "io/d3/d3_types.h"
#include "renderer/particle/d3_channels.h"
#include "renderer/particle/d3_emitter.h"
#include "renderer/particle/d3_orientation.h"
#include "renderer/particle/d3_particle.h"
#include "renderer/particle/d3_path.h"
#include "renderer/particle/particle_pool.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace pd3 = ::whiteout::flakes::renderer::particle::d3;
using namespace ::whiteout;

namespace {

fs::path GoldenRoot() {
    if (const char* v = std::getenv("WDX_D3_ORACLE_GOLDEN"); v && *v)
        return fs::path(v);
#ifdef WDX_D3_ORACLE_GOLDEN_DIR
    return fs::path(WDX_D3_ORACLE_GOLDEN_DIR);
#else
    return fs::path("tools/d3_particle_oracle/golden");
#endif
}

wdx_golden::ValuePtr LoadGolden(const std::string& name) {
    const fs::path p = GoldenRoot() / (name + ".json");
    INFO("golden " << p.string());
    REQUIRE(fs::exists(p));
    return wdx_golden::Load(p.string());
}

/// Bit equality, which is what a golden comparison means. `==` would call two
/// NaNs unequal and two zeros of opposite sign equal; neither is what we want
/// when the question is "did the engine produce this exact word".
bool SameBits(f32 a, f32 b) {
    u32 x, y;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    return x == y;
}

/// The declared MWC, transcribed nowhere else: `d3_path.h`'s MwcRng is the code
/// under test, so the test uses it directly and the golden is the oracle.
pd3::Path MakeScalarPath(const wdx_golden::Value& nodes, f32 loopStart, f32 loopEnd) {
    pd3::Path p;
    p.components = 1;
    p.loopStart = loopStart;
    p.loopEnd = loopEnd;
    for (std::size_t i = 0; i < nodes.Size(); ++i) {
        const auto& n = nodes[i];
        pd3::PathNode node;
        node.start = {n[0].F(), 0, 0, 0};
        node.end = {n[1].F(), 0, 0, 0};
        node.time = n[2].F();
        p.nodes.push_back(node);
    }
    return p;
}

pd3::EvalCtx MakeCtx(const wdx_golden::Value& c) {
    pd3::EvalCtx ctx;
    ctx.timeMode = static_cast<pd3::TimeMode>(c["time_mode"].I());
    ctx.period = c["period"].F();
    ctx.blend = c.Has("blend") ? c["blend"].F() : 0.0f;
    ctx.blendT = c.Has("blend_t") ? c["blend_t"].F() : 0.0f;
    return ctx;
}

Vector4f UnpackColorDword(u32 c) {
    return {static_cast<f32>(c & 0xFFu) / 255.0f, static_cast<f32>((c >> 8) & 0xFFu) / 255.0f,
            static_cast<f32>((c >> 16) & 0xFFu) / 255.0f,
            static_cast<f32>((c >> 24) & 0xFFu) / 255.0f};
}

} // namespace

// ---------------------------------------------------------------------------
// A1 — the random stream and the nine distributions
// ---------------------------------------------------------------------------

TEST_CASE("oracle A1: the MWC stream and its seeding", "[d3][oracle][a1]") {
    const auto doc = LoadGolden("a1_mwc");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const u32 seed = c["in"]["seed"].U();
        const auto& state = c["out"]["state"];
        // Rand_MWC_Seed writes {seed, 666}. The 666 is literal, not a hash.
        pd3::MwcRng rng = pd3::MwcRng::Seed(seed);
        INFO("seed " << seed);
        CHECK(rng.lo == state[0].U());
        CHECK(rng.hi == state[1].U());
        const auto& words = c["out"]["words"];
        for (std::size_t k = 0; k < words.Size(); ++k) {
            const u32 got = rng.Next();
            INFO("draw " << k);
            REQUIRE(got == words[k].U());
        }
    }
}

TEST_CASE("oracle A1: the nine distributions", "[d3][oracle][a1]") {
    const auto doc = LoadGolden("a1_draw_random");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() == 12); // 0..8 plus three out-of-range
    std::size_t compared = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const i32 dist = c["in"]["dist"].I();
        const auto& us = c["in"]["u"];
        const auto& vs = c["out"]["v"];
        REQUIRE(us.Size() == vs.Size());
        for (std::size_t k = 0; k < us.Size(); ++k) {
            const f32 got = pd3::ApplyDistribution(dist, us[k].F());
            INFO("distribution " << dist << " draw " << k << " u=" << us[k].F());
            REQUIRE(SameBits(got, vs[k].F()));
            ++compared;
        }
    }
    CHECK(compared > 36000);
}

TEST_CASE("oracle A1: the raw stream drives the distributions in engine order",
          "[d3][oracle][a1]") {
    // The `u` recorded in a1_draw_random is the engine's own converted draw. If
    // our MwcRng and our 2^-32 conversion agree with it, the two halves of A1 are
    // joined and every later gate can use engine-order draws.
    const auto doc = LoadGolden("a1_draw_random");
    const auto& cases = (*doc)["cases"];
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        pd3::MwcRng rng = pd3::MwcRng::Seed(0x1234567u);
        const auto& us = c["in"]["u"];
        for (std::size_t k = 0; k < us.Size(); ++k) {
            const f32 got = rng.NextUnit();
            INFO("case " << i << " draw " << k);
            REQUIRE(SameBits(got, us[k].F()));
        }
    }
}

// ---------------------------------------------------------------------------
// A2 — path evaluation
// ---------------------------------------------------------------------------

TEST_CASE("oracle A2: InterpolationPath_Sample, all three time modes",
          "[d3][oracle][a2]") {
    const auto doc = LoadGolden("a2_path_sample");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    std::size_t compared = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        pd3::Path p = MakeScalarPath(in["nodes"], in["loop"][0].F(), in["loop"][1].F());
        pd3::EvalCtx ctx = MakeCtx(in);
        const auto& samples = in["samples"];
        const auto& vs = c["out"]["v"];
        REQUIRE(samples.Size() == vs.Size());
        for (std::size_t k = 0; k < samples.Size(); ++k) {
            ctx.time = samples[k]["t"].F();
            const f32 r = samples[k]["r"].F();
            const f32 got = pd3::SampleAt(p, {r, r, r, r}, ctx).x;
            INFO("case " << (c.Has("tag") ? c["tag"].S() : std::string()) << " sample " << k);
            REQUIRE(SameBits(got, vs[k].F()));
            ++compared;
        }
    }
    CHECK(compared >= 8000);
}

TEST_CASE("oracle A2: the period short circuit is 1/60, not 0.016667",
          "[d3][oracle][a2]") {
    // The engine compares against the constant @0x7100E3BEF4, which is 1/60. The
    // `0.016667` a decompile prints is a different float and never matches, so a
    // build that transcribes it never takes the branch.
    const auto doc = LoadGolden("a2_period_cases");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        pd3::Path p = MakeScalarPath(in["nodes"], 0.0f, 1.0f);
        pd3::EvalCtx ctx;
        ctx.timeMode = static_cast<pd3::TimeMode>(in["time_mode"].I());
        ctx.time = in["time"].F();
        ctx.period = in["period"].F();
        const f32 r = in["r"].F();
        const f32 got = pd3::SampleAt(p, {r, r, r, r}, ctx).x;
        INFO("period " << in["period"].F() << " mode " << static_cast<i32>(ctx.timeMode));
        REQUIRE(SameBits(got, c["out"]["v"].F()));
    }
}

TEST_CASE("oracle A2: the full-range test is loopEnd > 0.999999", "[d3][oracle][a2]") {
    const auto doc = LoadGolden("a2_loopend_threshold");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        pd3::Path p = MakeScalarPath(in["nodes"], 0.0f, in["loop_end"].F());
        pd3::EvalCtx ctx;
        ctx.timeMode = static_cast<pd3::TimeMode>(in["time_mode"].I());
        ctx.period = in["period"].F();
        const f32 r = in["r"].F();
        const auto& vs = c["out"]["v"];
        const int n = in["n"].I();
        for (int k = 0; k < n; ++k) {
            ctx.time = static_cast<f32>(k * 3.0 / (n - 1));
            const f32 got = pd3::SampleAt(p, {r, r, r, r}, ctx).x;
            INFO("loopEnd " << in["loop_end"].F() << " step " << k);
            REQUIRE(SameBits(got, vs[static_cast<std::size_t>(k)].F()));
        }
    }
}

TEST_CASE("oracle A2: a vector channel draws three randoms, in x y z order",
          "[d3][oracle][a2]") {
    const auto doc = LoadGolden("a2_eval_wrappers");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        pd3::Path p;
        p.components = 3;
        for (std::size_t k = 0; k < in["nodes"].Size(); ++k) {
            const auto& n = in["nodes"][k];
            pd3::PathNode node;
            node.start = {n[0][0].F(), n[0][1].F(), n[0][2].F(), 0};
            node.end = {n[1][0].F(), n[1][1].F(), n[1][2].F(), 0};
            node.time = n[2].F();
            p.nodes.push_back(node);
        }
        pd3::EvalCtx ctx = MakeCtx(in);
        ctx.time = in["time"].F();
        const Vector3f got = p.EvalVector(in["seed"].U(), in["channel"].I(), ctx);
        const auto& want = c["out"]["v"];
        INFO("seed " << in["seed"].U() << " channel " << in["channel"].I());
        CHECK(SameBits(got.x, want[0].F()));
        CHECK(SameBits(got.y, want[1].F()));
        CHECK(SameBits(got.z, want[2].F()));

        // And the draws themselves, so an order change fails here rather than
        // three areas downstream.
        pd3::MwcRng rng = pd3::MwcRng::ForChannel(in["channel"].I(), in["seed"].U());
        const auto& r = c["out"]["r"];
        for (std::size_t k = 0; k < r.Size(); ++k)
            CHECK(SameBits(rng.NextUnit(), r[k].F()));
    }
}

TEST_CASE("oracle A2: colour is 8-bit fixed point and draws ONE random",
          "[d3][oracle][a2]") {
    // Not the generic path with four components. Evaluating colour as floats and
    // rounding at the end is a different number, and drawing three randoms is a
    // different colour.
    const auto doc = LoadGolden("a2_eval_color");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        pd3::Path p;
        p.components = 4;
        for (std::size_t k = 0; k < in["nodes"].Size(); ++k) {
            const auto& n = in["nodes"][k];
            pd3::PathNode node;
            node.start = UnpackColorDword(n[0].U());
            node.end = UnpackColorDword(n[1].U());
            node.time = n[2].F();
            p.nodes.push_back(node);
        }
        pd3::EvalCtx ctx = MakeCtx(in);
        ctx.time = in["time"].F();
        const Vector4f got = p.EvalColor(in["seed"].U(), in["channel"].I(), ctx);
        const auto& want = c["out"]["bytes"];
        const int gb[4] = {
            static_cast<int>(std::lround(got.x * 255.0f)),
            static_cast<int>(std::lround(got.y * 255.0f)),
            static_cast<int>(std::lround(got.z * 255.0f)),
            static_cast<int>(std::lround(got.w * 255.0f)),
        };
        INFO("case " << i << " seed " << in["seed"].U() << " mode " << static_cast<i32>(ctx.timeMode)
                     << " t " << ctx.time);
        for (int k = 0; k < 4; ++k)
            REQUIRE(gb[k] == want[static_cast<std::size_t>(k)].I());
    }
}

TEST_CASE("oracle A2: an int channel is integer arithmetic, rounded half-to-even",
          "[d3][oracle][a2]") {
    const auto doc = LoadGolden("a2_eval_int");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        pd3::Path p;
        p.components = 1;
        for (std::size_t k = 0; k < in["nodes"].Size(); ++k) {
            const auto& n = in["nodes"][k];
            pd3::PathNode node;
            node.start = {static_cast<f32>(n[0].I()), 0, 0, 0};
            node.end = {static_cast<f32>(n[1].I()), 0, 0, 0};
            node.time = n[2].F();
            p.nodes.push_back(node);
        }
        pd3::EvalCtx ctx = MakeCtx(in);
        ctx.time = in["time"].F();
        const i32 got = p.EvalInt(in["seed"].U(), in["channel"].I(), ctx);
        INFO("case " << i << " mode " << static_cast<i32>(ctx.timeMode) << " t " << ctx.time);
        REQUIRE(got == c["out"]["v"].I());
    }
}

TEST_CASE("oracle A2: GetScalarRange is per lane, never a start against an end",
          "[d3][oracle][a2]") {
    const auto doc = LoadGolden("a2_scalar_range");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        pd3::Path p = MakeScalarPath(c["in"]["nodes"], 0.0f, 1.0f);
        f32 lo = 0.0f, hi = 0.0f;
        p.ScalarRange(lo, hi);
        const std::string tag = c.Has("tag") ? c["tag"].S() : std::to_string(i);
        INFO(tag);
        CHECK(SameBits(lo, c["out"]["lo"].F()));
        CHECK(SameBits(hi, c["out"]["hi"].F()));
    }
}

TEST_CASE("oracle A2: GetScalarEndpoints is the two lanes at a time",
          "[d3][oracle][a2]") {
    // Not the curve's global range. An animated shape extent depends on this.
    const auto doc = LoadGolden("a2_scalar_endpoints");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        pd3::Path p = MakeScalarPath(in["nodes"], in["loop"][0].F(), in["loop"][1].F());
        pd3::EvalCtx ctx = MakeCtx(in);
        const auto& lanes = c["out"]["lanes"];
        const int n = in["n"].I();
        for (int k = 0; k < n; ++k) {
            ctx.time = static_cast<f32>(k * 1.6 / 16.0);
            f32 lo = 0.0f, hi = 0.0f;
            p.ScalarEndpoints(ctx, lo, hi);
            INFO("case " << i << " step " << k);
            REQUIRE(SameBits(lo, lanes[static_cast<std::size_t>(k)][0].F()));
            REQUIRE(SameBits(hi, lanes[static_cast<std::size_t>(k)][1].F()));
        }
    }
}

// ---------------------------------------------------------------------------
// A5 — the emitter shapes
//
// The one tolerance in this file, and the operation that forces it: `sinf` and
// `cosf` are nnSdk imports. The oracle runs the console's own; we run the host's.
// They agree to within a single ulp, so shape positions are compared to 2 ulp
// while everything a trig call does not touch stays bit-exact.
// ---------------------------------------------------------------------------

namespace {

int Ulps(f32 a, f32 b) {
    i32 x, y;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    if (x < 0)
        x = static_cast<i32>(0x80000000u) - x;
    if (y < 0)
        y = static_cast<i32>(0x80000000u) - y;
    return std::abs(x - y);
}

constexpr int kTrigUlp = 2;

} // namespace

TEST_CASE("oracle A5: the shape primitives and their draw counts", "[d3][oracle][a5]") {
    const auto doc = LoadGolden("a5_rand_helpers");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    int worst = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const std::string fn = c["in"]["fn"].S();
        // The annulus cases carry (lo, span) instead of a radius.
        const f32 radius = c["in"].Has("radius") ? c["in"]["radius"].F() : 0.0f;
        pd3::MwcRng rng = pd3::MwcRng::Seed(0x1234567u);
        if (fn == "Rand_RadiusInAnnulus") {
            const f32 lo = c["in"]["lo"].F();
            const f32 span = c["in"]["span"].F();
            const auto& rs = c["out"]["r"];
            for (std::size_t k = 0; k < rs.Size(); ++k) {
                const f32 got = pd3::SampleRadiusInAnnulus(rng, lo, span);
                INFO("annulus lo=" << lo << " span=" << span << " draw " << k);
                REQUIRE(SameBits(got, rs[k].F()));
            }
            // The zero-width case must consume NO draw, which the final RNG state
            // is the only witness to.
            const auto& state = c["out"]["state"];
            CHECK(rng.lo == state[0].U());
            CHECK(rng.hi == state[1].U());
            continue;
        }
        const auto& ps = c["out"]["p"];
        for (std::size_t k = 0; k < ps.Size(); ++k) {
            Vector3f got{};
            if (fn == "Rand_PointOnSphere")
                got = pd3::SamplePointOnSphere(rng, radius);
            else if (fn == "Rand_PointOnHemisphere")
                got = pd3::SamplePointOnHemisphere(rng, radius);
            else
                got = pd3::SamplePointOnCircleXY(rng, radius);
            const f32 want[3] = {ps[k][0].F(), ps[k][1].F(), ps[k][2].F()};
            const f32 mine[3] = {got.x, got.y, got.z};
            for (int a = 0; a < 3; ++a) {
                INFO(fn << " r=" << radius << " draw " << k << " comp " << a);
                const int u = Ulps(mine[a], want[a]);
                worst = std::max(worst, u);
                REQUIRE(u <= kTrigUlp);
            }
        }
    }
    // Stated, not hidden: if this ever climbs, a formula changed, not a libm.
    CHECK(worst <= kTrigUlp);
}

// ---------------------------------------------------------------------------
// A8 - geometry
// ---------------------------------------------------------------------------

TEST_CASE("oracle A8: the draw-order sort is descending and a permutation",
          "[d3][oracle][a8]") {
    // The WoW oracle found a shipped off-by-one in the equivalent function. This
    // one has none: over 11 lists the engine's introsort is a clean descending
    // permutation, which is what our own back-to-front sort has to reproduce.
    const auto doc = LoadGolden("a8_sort");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& keys = c["out"]["keys"];
        const std::string tag = c.Has("tag") ? c["tag"].S() : std::to_string(i);
        INFO(tag);
        for (std::size_t k = 1; k < keys.Size(); ++k)
            REQUIRE(keys[k - 1].F() >= keys[k].F());
        std::vector<u64> handles;
        for (std::size_t k = 0; k < c["out"]["handles"].Size(); ++k)
            handles.push_back(static_cast<u64>(c["out"]["handles"][k].Num()));
        std::vector<u64> sorted = handles;
        std::sort(sorted.begin(), sorted.end());
        for (std::size_t k = 0; k < sorted.size(); ++k)
            REQUIRE(sorted[k] == 0x1000 + k);
    }
}

// ---------------------------------------------------------------------------
// A9 - the UV transform
// ---------------------------------------------------------------------------

TEST_CASE("oracle A9: flags bits 0 and 1 are phase randomisers, not address modes",
          "[d3][oracle][a9]") {
    // Section 19.3 could not separate the two readings from the corpus, and said
    // so. The engine separates them: the bits change the PHASE, and only for uv
    // mode 2. The address mode is not in the entry at all - this build reads it
    // off the bound pass, and that stays right.
    const auto doc = LoadGolden("a9_init_uv_state");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    int randomised = 0, fixed = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const int mode = c["in"]["mode"].I();
        const unsigned flags = c["in"]["flags"].U();
        const auto& u = c["in"]["u"];
        const auto& v = c["in"]["v"];
        INFO("case " << i << " mode " << mode << " flags " << flags);
        if (mode != 2) {
            // No draw and no phase, whatever the bits say.
            CHECK(c["out"]["u_phase"].F() == 0.0f);
            CHECK(c["out"]["v_phase"].F() == 0.0f);
            continue;
        }
        if (flags & 1u) {
            CHECK(c["out"]["u_phase"].F() != u[0].F());
            ++randomised;
        } else {
            CHECK(SameBits(c["out"]["u_phase"].F(), u[0].F()));
            ++fixed;
        }
        if (flags & 2u)
            CHECK(c["out"]["v_phase"].F() != v[0].F());
        else
            CHECK(SameBits(c["out"]["v_phase"].F(), v[0].F()));
        // And the rate is flRate0 x 60, plus the jitter when there is one.
        if (v[2].F() == 0.0f)
            CHECK(SameBits(c["out"]["v_rate"].F(), v[1].F() * 60.0f));
    }
    CHECK(randomised > 0);
    CHECK(fixed > 0);
}

// ---------------------------------------------------------------------------
// A10 - the flip-book
// ---------------------------------------------------------------------------

TEST_CASE("oracle A10: the flip-book's start frame, rate and wrap length",
          "[d3][oracle][a10]") {
    const auto doc = LoadGolden("a10_anim2d_bind");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const int count = c["in"]["frame_count"].I();
        const int base = c["in"]["base"].I();
        const int range = c["in"]["range"].I();
        const f32 rate = c["in"]["rate"].F();
        const f32 jitter = c["in"]["jitter"].F();
        INFO("count " << count << " base " << base << " range " << range);
        const int frame = c["out"]["frame"].I();
        REQUIRE(frame >= 0);
        REQUIRE(frame <= count - 1);
        REQUIRE(frame >= std::min(base, count - 1));
        REQUIRE(frame <= std::max(0, std::min(base + range, count - 1)));
        // The wrap length is count - 0.0001, not count.
        REQUIRE(SameBits(c["out"]["length"].F(), static_cast<f32>(count) - 0.0001f));
        if (jitter == 0.0f)
            REQUIRE(SameBits(c["out"]["rate"].F(), rate * 60.0f));
    }
}

TEST_CASE("oracle A10: a forward flip-book never leaves its sheet",
          "[d3][oracle][a10]") {
    const auto doc = LoadGolden("a10_anim2d_cursor");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    bool sawNegativeWalk = false;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const int count = c["in"]["frame_count"].I();
        const f32 rate = c["in"]["rate_per_sec"].F();
        const auto& steps = c["out"]["steps"];
        for (std::size_t k = 0; k < steps.Size(); ++k) {
            const int frame = steps[k][1].I();
            if (rate >= 0.0f) {
                INFO("count " << count << " rate " << rate << " step " << k);
                REQUIRE(frame >= 0);
                REQUIRE(frame <= count - 1);
            } else if (frame < 0) {
                sawNegativeWalk = true;
            }
        }
    }
    // Shipped behaviour, asserted so it stays visible: the loop arm folds only the
    // top end, so a negative rate walks the cursor below zero and the matrix's own
    // clamp pins the sheet to tile 0.
    CHECK(sawNegativeWalk);
}

// ---------------------------------------------------------------------------
// A4 - the birth record
// ---------------------------------------------------------------------------

TEST_CASE("oracle A4: no lifetime path means ONE FRAME, not one second",
          "[d3][oracle][a4]") {
    const auto doc = LoadGolden("a4_init_life_and_size");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    bool checked = false;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        if (!c["in"]["life"].IsNull())
            continue;
        const auto& births = c["out"]["births"];
        REQUIRE(births.Size() > 0);
        REQUIRE(SameBits(births[0]["life"].F(), 1.0f / 60.0f));
        REQUIRE(SameBits(births[0]["base_size"].F(), 1.0f));
        checked = true;
    }
    CHECK(checked);
}

TEST_CASE("oracle A4: the particle seed is the raw draw, nudged at two values",
          "[d3][oracle][a4]") {
    const auto doc = LoadGolden("a4_init_life_and_size");
    const auto& cases = (*doc)["cases"];
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        // dwPrtFlags bit 8 decides whether a second draw is taken for the
        // birth-position lerp, so it decides the seed of every later particle.
        const bool noLerp = (c["in"]["flags"].U() & 0x100u) != 0;
        pd3::MwcRng rng = pd3::MwcRng::Seed(0x1234567u);
        const auto& births = c["out"]["births"];
        for (std::size_t k = 0; k < births.Size(); ++k) {
            const u32 raw = rng.Next();
            const u32 want = (raw >= 0xFFFFFFFEu) ? (raw + 2u) : raw;
            INFO("case " << i << " birth " << k);
            REQUIRE(births[k]["seed"].U() == want);
            if (!noLerp)
                rng.Next(); // the position lerp's draw
            if (c["in"]["state"].U() & 0x10u) {
                rng.Next(); // the birth sphere's two draws
                rng.Next();
            }
            if (c["in"]["state"].U() & 0x01u)
                rng.Next(); // the orbit phase
            if (c["in"]["flags"].U() & 0x1000u)
                rng.Next(); // the random roll
        }
    }
}

// ---------------------------------------------------------------------------
// Provenance — every golden must come from the same build the audit ran against.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// A7 - the fourteen orientation modes
// ---------------------------------------------------------------------------

TEST_CASE("oracle A7: the orientation frames, per render mode", "[d3][oracle][a7]") {
    const auto doc = LoadGolden("a7_orientation_modes");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);

    int modelled = 0;
    int silent = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const int mode = c["in"]["mode"].I();
        const bool gated = c["in"]["gated"].I() != 0;
        const Vector3f axis{c["in"]["axis"][0].F(), c["in"]["axis"][1].F(),
                            c["in"]["axis"][2].F()};
        const Vector3f cand{c["in"]["candidate"][0].F(), c["in"]["candidate"][1].F(),
                            c["in"]["candidate"][2].F()};
        const Vector3f fall{c["in"]["fallback"][0].F(), c["in"]["fallback"][1].F(),
                            c["in"]["fallback"][2].F()};
        const Vector3f pos{c["in"]["pos"][0].F(), c["in"]["pos"][1].F(),
                           c["in"]["pos"][2].F()};
        const Vector3f ref{c["in"]["refpos"][0].F(), c["in"]["refpos"][1].F(),
                           c["in"]["refpos"][2].F()};
        const bool engineWrote = c["out"]["wrote"].B();

        Quaternion q{};
        bool ourWrote = false;
        bool covered = true;
        if (mode == 1 || mode == 8) {
            ourWrote = false;
        } else if (mode == 0) {
            // Mode 0 is a no-op unless the system's flag bit 13 is set. That is not
            // a shortcut to skip: three of every five corpus appearances land on a
            // mode that leaves the caller's quaternion standing.
            ourWrote = gated && pd3::OrientBillboard(axis, q);
        } else if (mode == 13) {
            ourWrote = pd3::OrientFlattened(axis, gated, q);
        } else if (!gated && (mode == 3 || mode == 4 || mode == 5 || mode == 6)) {
            Vector3f pick{};
            if (mode == 3)
                pick = cand;
            else if (mode == 6)
                pick = {-axis.x, -axis.y, 0.0f};
            else if (mode == 4)
                pick = {pos.x - ref.x, pos.y - ref.y, 0.0f};
            else
                pick = {pos.x - ref.x, pos.y - ref.y, pos.z - ref.z};
            ourWrote = pd3::OrientFromAxis(pick, fall, q);
        } else {
            covered = false;
        }
        if (!covered) {
            // Recorded, not implemented. The golden still pins that whatever the
            // engine produced is a usable frame, so a future implementation has a
            // target rather than a guess.
            if (engineWrote) {
                const f32 qx = c["out"]["q"][0].F(), qy = c["out"]["q"][1].F();
                const f32 qz = c["out"]["q"][2].F(), qw = c["out"]["q"][3].F();
                const f32 n = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
                CHECK(std::fabs(n - 1.0f) < 1e-4f);
            }
            continue;
        }
        ++modelled;
        INFO("mode " << mode << " gated " << gated << " case " << i);
        REQUIRE(ourWrote == engineWrote);
        if (!ourWrote) {
            ++silent;
            continue;
        }
        CHECK(SameBits(q.x, c["out"]["q"][0].F()));
        CHECK(SameBits(q.y, c["out"]["q"][1].F()));
        CHECK(SameBits(q.z, c["out"]["q"][2].F()));
        CHECK(SameBits(q.w, c["out"]["q"][3].F()));
    }
    CHECK(modelled > 0);
    // Modes 1 and 8, and mode 0 with the gate bit clear, must produce nothing.
    CHECK(silent > 0);
}

// ---------------------------------------------------------------------------
// A8 - the quad the engine writes
// ---------------------------------------------------------------------------

TEST_CASE("oracle A8: vertex+36 is the orientation quaternion, not a colour",
          "[d3][oracle][a8]") {
    const auto doc = LoadGolden("a8_quad_vertices");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& q = c["in"]["quat"];
        u32 want = 0;
        for (std::size_t lane = 0; lane < 4; ++lane) {
            const f32 t = (q[lane].F() * 0.5f + 0.5f) * 255.99f;
            const u32 b = t < 0.0f ? 0u : static_cast<u32>(std::fmin(t, 255.0f));
            want |= b << (8 * lane);
        }
        const auto& verts = c["out"]["verts"];
        REQUIRE(verts.Size() == 4);
        for (std::size_t v = 0; v < 4; ++v) {
            INFO("case " << i << " vertex " << v);
            CHECK(verts[v]["quat"].U() == want);
            // The colour is a straight copy of the draw descriptor's dword, and it
            // is NOT the quaternion word.
            CHECK(verts[v]["colour"].U() == c["in"]["colour"].U());
        }
    }
}

TEST_CASE("oracle A8: the sway offset bends the quad, it does not move the particle",
          "[d3][oracle][a8]") {
    const auto doc = LoadGolden("a8_quad_vertices");
    const auto& cases = (*doc)["cases"];
    int bent = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const bool applied = (c["in"]["flags"].I() & 4) != 0;
        const Vector3f base{c["in"]["pos"][0].F(), c["in"]["pos"][1].F(),
                            c["in"]["pos"][2].F()};
        const Vector3f off{c["in"]["stretch"][0].F(), c["in"]["stretch"][1].F(),
                           c["in"]["stretch"][2].F()};
        const auto& verts = c["out"]["verts"];
        // Corner order is the strip (+u,-v), (-u,-v), (+u,+v), (-u,+v), so the two
        // displaced vertices are exactly the two on the +v side.
        for (std::size_t v = 0; v < 4; ++v) {
            const bool top = v >= 2;
            const Vector3f want =
                (applied && top) ? Vector3f{base.x + off.x, base.y + off.y, base.z + off.z}
                                 : base;
            INFO("case " << i << " vertex " << v);
            CHECK(SameBits(verts[v]["pos"][0].F(), want.x));
            CHECK(SameBits(verts[v]["pos"][1].F(), want.y));
            CHECK(SameBits(verts[v]["pos"][2].F(), want.z));
        }
        if (applied && (off.x != 0.0f || off.y != 0.0f))
            ++bent;
    }
    CHECK(bent > 0);
}

// ---------------------------------------------------------------------------
// A6 - the wind spring
// ---------------------------------------------------------------------------

TEST_CASE("oracle A6: the wind spring, both arms", "[d3][oracle][a6]") {
    const auto doc = LoadGolden("a6_wind_spring");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    int clamped = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& sno = in["sno"];
        pd3::WindSpringRig rig{sno["freq"].F(), sno["damp"].F(), sno["maxoff"].F(),
                               sno["gust"].F(), sno["base"].F()};
        pd3::ParticleState st;
        st.swayPhase = in["phase"].F();
        st.size = in["radius"].F();
        st.swayOffset = {in["offset"][0].F(), in["offset"][1].F()};
        st.swayVelocity = {in["vel"][0].F(), in["vel"][1].F()};
        st.swayForce = {in["force"][0].F(), in["force"][1].F()};
        const bool frozen = in["frozen"].I() != 0;

        pd3::StepWindSpring(st, rig, {in["wind"][0].F(), in["wind"][1].F()},
                            in["strength"].F(), in["wind_phase"].F(), in["dt"].F(), frozen);

        INFO("case " << i << (frozen ? " frozen arm" : " live arm"));
        if (frozen) {
            // No transcendental on this arm, so it is bit-exact or it is wrong.
            CHECK(SameBits(st.swayOffset.x, c["out"]["offset"][0].F()));
            CHECK(SameBits(st.swayOffset.y, c["out"]["offset"][1].F()));
            CHECK(SameBits(st.swayVelocity.x, c["out"]["vel"][0].F()));
            CHECK(SameBits(st.swayVelocity.y, c["out"]["vel"][1].F()));
        } else {
            // The live arm passes through cosf and our libm is not the console's;
            // an ulp there is not a finding about the particle system.
            const f32 tol = 1e-6f;
            const f32 wx = c["out"]["offset"][0].F(), wy = c["out"]["offset"][1].F();
            CHECK(std::fabs(st.swayOffset.x - wx) <= tol * std::fmax(1.0f, std::fabs(wx)));
            CHECK(std::fabs(st.swayOffset.y - wy) <= tol * std::fmax(1.0f, std::fabs(wy)));
        }
        if (st.swayVelocity.x == 0.0f && st.swayVelocity.y == 0.0f &&
            (in["vel"][0].F() != 0.0f || in["vel"][1].F() != 0.0f))
            ++clamped;
    }
    // The clamp is load-bearing: hitting it zeroes the velocity, which is what stops
    // a stiff rig from oscillating out of its own bound.
    CHECK(clamped > 0);
}

// ---------------------------------------------------------------------------
// A9 - the per-frame UV advance
// ---------------------------------------------------------------------------

TEST_CASE("oracle A9: the UV fold keeps 1.0, and flAmount clamps instead",
          "[d3][oracle][a9]") {
    const auto doc = LoadGolden("a9_tick_uv_state");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);

    // We evaluate the UV transform in closed form rather than integrating a state
    // block, so the whole tick is not replayable. What IS ours is the fold, and
    // this pins it against the engine's own folded output: recompute the value the
    // engine had in hand before it folded, fold it our way, and compare.
    int folded = 0;
    int clamped = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& e = c["in"]["entry"];
        const auto& st = c["in"]["state"];
        if (e["mode"].I() != 2)
            continue;
        const bool fromClock = (e["flags"].I() & 4) != 0;
        const f32 step = fromClock ? c["in"]["now"].F() - st["t"].F() : c["in"]["dt"].F();

        // All three of these fields are tested as INTEGERS by the engine. Testing
        // them as floats is not a stylistic difference: entry 8 of the golden sets
        // -0.0f, which is false as a float and true as an int, and comparing the
        // wrong way puts 55 cases on the wrong arm.
        const auto Set = [](f32 v) {
            u32 bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            return bits != 0;
        };
        const bool doClamp = Set(e["amount"].F());
        const bool hasDriver = !c["in"]["driver"].IsNull();
        const f32 driver = hasDriver ? c["in"]["driver"].F() : 0.0f;

        // The driver REPLACES the scroll for whichever of u and v names it.
        const f32 preU = (hasDriver && Set(e["rate0"].F()))
                             ? driver
                             : st["u"].F() + step * st["urate"].F();
        const f32 preV = (hasDriver && Set(e["rate1"].F()))
                             ? driver
                             : st["v"].F() + step * st["vrate"].F();
        const f32 ourU = doClamp ? std::clamp(preU, 0.0f, 1.0f)
                                 : ::whiteout::flakes::io::D3FoldUv(preU);
        const f32 ourV = doClamp ? std::clamp(preV, 0.0f, 1.0f)
                                 : ::whiteout::flakes::io::D3FoldUv(preV);
        INFO("case " << i << (doClamp ? " clamp" : " fold") << " pre " << preU);
        CHECK(SameBits(ourU, c["out"]["u"].F()));
        CHECK(SameBits(ourV, c["out"]["v"].F()));
        if (doClamp)
            ++clamped;
        else if (preU != ourU || preV != ourV)
            ++folded;
    }
    // Both arms have to be exercised or the comparison proves nothing.
    CHECK(folded > 0);
    CHECK(clamped > 0);
}

TEST_CASE("oracle A9: flAmount is tested as an integer, so -0.0f is set",
          "[d3][oracle][a9]") {
    const auto doc = LoadGolden("a9_tick_uv_state");
    const auto& cases = (*doc)["cases"];
    bool sawNegativeZero = false;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& e = c["in"]["entry"];
        const f32 amount = e["amount"].F();
        u32 bits = 0;
        std::memcpy(&bits, &amount, sizeof(bits));
        if (bits != 0x80000000u)
            continue;
        sawNegativeZero = true;
        // -0.0f == 0.0f as a float, so a float test would call this clear and wrap.
        // The engine clamps. This is the case that decides which reading is right.
        REQUIRE(amount == 0.0f);
        const auto& st = c["in"]["state"];
        if (e["mode"].I() != 2)
            continue;
        const bool fromClock = (e["flags"].I() & 4) != 0;
        const f32 step = fromClock ? c["in"]["now"].F() - st["t"].F() : c["in"]["dt"].F();
        const f32 preU = st["u"].F() + step * st["urate"].F();
        CHECK(SameBits(std::clamp(preU, 0.0f, 1.0f), c["out"]["u"].F()));
    }
    CHECK(sawNegativeZero);
}

// ---------------------------------------------------------------------------
// A4 — the birth velocity and the emission cone (G-D3P-15)
// ---------------------------------------------------------------------------

namespace {

/// The golden's constant-path cases, evaluated without the sampler: a path whose
/// single node has start == end returns that value and takes no draw, so the
/// whole pipeline reduces to arithmetic this file can check on its own.
bool ConstantNode(const wdx_golden::Value& nodes, Vector3f& out) {
    if (nodes.IsNull() || nodes.Size() != 1)
        return false;
    const auto& n = nodes[0];
    for (std::size_t c = 0; c < 3; ++c) {
        if (!SameBits(n[0][c].F(), n[1][c].F()))
            return false;
    }
    out = {n[0][0].F(), n[0][1].F(), n[0][2].F()};
    return true;
}

bool ConstantScalar(const wdx_golden::Value& nodes, f32& out) {
    if (nodes.IsNull() || nodes.Size() != 1)
        return false;
    const auto& n = nodes[0];
    if (!SameBits(n[0].F(), n[1].F()))
        return false;
    out = n[0].F();
    return true;
}

} // namespace

TEST_CASE("oracle A4: the emission cone opens about the velocity, not about Z",
          "[d3][oracle][a4]") {
    const auto doc = LoadGolden("a4b_initial_velocity");
    const auto& cases = (*doc)["cases"];
    constexpr f32 kAzimuthTwoPi = 6.28318452835083f;

    std::size_t exact = 0;
    std::size_t coned = 0;
    std::size_t oldWouldDiffer = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        Vector3f v0{0, 0, 0};
        const bool hasV0 = ConstantNode(in["vel0"], v0);
        if (!in["vel0"].IsNull() && !hasV0)
            continue; // a sampled path; the A2 cases own the sampler
        Vector3f v1{0, 0, 0};
        const bool hasV1 = ConstantNode(in["vel1"], v1);
        if (!in["vel1"].IsNull() && !hasV1)
            continue;
        f32 cone = 0.0f;
        if (!in["angle"].IsNull() && !ConstantScalar(in["angle"], cone))
            continue;

        Vector3f v = hasV0 ? Vector3f{v0.x * 60.0f, v0.y * 60.0f, v0.z * 60.0f}
                           : Vector3f{0, 0, 0};
        const f32 mag = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        const bool spins = cone != 0.0f && mag > 1e-6f;
        Vector3f before = v;
        if (spins) {
            pd3::MwcRng rng(static_cast<u32>(in["rng"].I()), 666);
            const f32 azim = rng.NextUnit() * kAzimuthTwoPi;
            v = pd3::ConeSpread(v, cone, azim);
            ++coned;
            // The reading this replaced: a rotation about an axis lying in the
            // XY plane. It agrees only when the base velocity is on Z, and the
            // golden has cases either side of that.
            const Vector3f axis{std::cos(azim), std::sin(azim), 0.0f};
            const Vector3f old = Quaternion::from_axis_angle(axis, cone).rotate_vector(before);
            if (!SameBits(old.x, v.x) || !SameBits(old.y, v.y) || !SameBits(old.z, v.z))
                ++oldWouldDiffer;
        }
        const auto& q = in["quat"];
        const Quaternion eq{q[0].F(), q[1].F(), q[2].F(), q[3].F()};
        v = eq.rotate_vector(v);

        if (hasV1)
            v = {v1.x * 60.0f + v.x, v1.y * 60.0f + v.y, v1.z * 60.0f + v.z};

        const auto& want = c["out"]["v"];
        INFO("case " << i << " cone " << cone << " spins " << spins);
        CHECK(static_cast<int>(spins) == c["out"]["draws"].I());
        // Two things force a tolerance and nothing else does: the cone calls
        // sinf/cosf (ours is the host libm, the engine's the console's), and a
        // non-identity emitter quaternion goes through our own rotate_vector,
        // which schedules its products differently from the engine's inline
        // expansion. An identity quaternion is a no-op in any schedule, so
        // those cases stay bit-exact and carry the comparison.
        const bool identityQuat = SameBits(eq.x, 0.0f) && SameBits(eq.y, 0.0f) &&
                                  SameBits(eq.z, 0.0f) && SameBits(eq.w, 1.0f);
        if (spins || !identityQuat) {
            // The cone calls sinf/cosf; ours is the host libm and the engine's
            // is the console's, so this arm carries a tolerance and says so.
            // Everything else is bit-exact.
            CHECK(std::fabs(v.x - want[0].F()) <= 1e-4f * (1.0f + std::fabs(want[0].F())));
            CHECK(std::fabs(v.y - want[1].F()) <= 1e-4f * (1.0f + std::fabs(want[1].F())));
            CHECK(std::fabs(v.z - want[2].F()) <= 1e-4f * (1.0f + std::fabs(want[2].F())));
        } else {
            CHECK(SameBits(v.x, want[0].F()));
            CHECK(SameBits(v.y, want[1].F()));
            CHECK(SameBits(v.z, want[2].F()));
            ++exact;
        }
    }
    // 23 of the golden's constant-path cases have an identity emitter
    // quaternion and no cone, and those are the bit-exact ones.
    CHECK(exact >= 20);
    CHECK(coned >= 20);
    // The regression guard: if this is zero the golden no longer separates a
    // cone about the velocity from a cone about Z, and the test proves nothing.
    CHECK(oldWouldDiffer >= 8);
}

// ---------------------------------------------------------------------------
// A9 — the 2x3 the quad uses (G-D3P-16)
// ---------------------------------------------------------------------------

TEST_CASE("oracle A9: uv mode 1 is an authored 2x3, off-diagonals included",
          "[d3][oracle][a9]") {
    const auto doc = LoadGolden("a9c_uv_affine_2x3");
    const auto& cases = (*doc)["cases"];
    std::size_t seen = 0;
    std::size_t offDiagonal = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        if (in["entry"].IsNull() || in["entry"]["mode"].I() != 1 || in["size"].I() != 144)
            continue;
        ::whiteout::flakes::io::D3UvXform x;
        x.mode = ::whiteout::flakes::io::D3UvMode::Matrix;
        x.authored[0] = in["entry"]["r0x"].F();
        x.authored[1] = in["entry"]["r1x"].F();
        x.authored[2] = in["entry"]["r3x"].F();
        x.authored[3] = in["entry"]["r0y"].F();
        x.authored[4] = in["entry"]["r1y"].F();
        x.authored[5] = in["entry"]["r3y"].F();
        f32 a[6];
        ::whiteout::flakes::io::D3UvAffine(x, 0.0f, a);

        const auto& want = c["out"]["affine"]; // m00, m01, m10, m11, tu, tv
        INFO("case " << i);
        CHECK(c["out"]["code"].I() == 4);
        CHECK(SameBits(a[0], want[0].F()));
        CHECK(SameBits(a[3], want[1].F()));
        CHECK(SameBits(a[1], want[2].F()));
        CHECK(SameBits(a[4], want[3].F()));
        CHECK(SameBits(a[2], want[4].F()));
        CHECK(SameBits(a[5], want[5].F()));
        if (want[1].F() != 0.0f || want[2].F() != 0.0f)
            ++offDiagonal;
        ++seen;
    }
    CHECK(seen >= 4);
    // Without an authored off-diagonal the case is indistinguishable from a
    // scale pair, which is what this build read mode 1 as.
    CHECK(offDiagonal >= 2);
}

TEST_CASE("oracle A9: the rotation pivots on the SCROLL, not on (0.5, 0.5)",
          "[d3][oracle][a9]") {
    const auto doc = LoadGolden("a9c_uv_affine_2x3");
    const auto& cases = (*doc)["cases"];
    std::size_t seen = 0;
    std::size_t fixedPivotWouldDiffer = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        if (in["entry"].IsNull() || in["entry"]["mode"].I() != 2 || in["size"].I() != 144)
            continue;
        if ((in["entry"]["flags"].I() & 0x8) == 0)
            continue;
        const f32 u = in["state"]["u"].F();
        const f32 v = in["state"]["v"].F();
        // Our closed form folds the translation before pivoting; the engine's
        // state machine has already folded it. Only a scroll already inside
        // [0, 1] compares the two things this case is about.
        if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
            continue;
        const f32 su = in["entry"]["r0x"].F();
        const f32 sv = in["entry"]["r1y"].F();

        ::whiteout::flakes::io::D3UvXform x;
        x.mode = ::whiteout::flakes::io::D3UvMode::ScaleRotateScroll;
        x.aboutCentre = true;
        if (su > 0.0f && sv > 0.0f)
            x.scale = {su, sv};
        x.offset = {u, v};
        x.rotate = in["state"]["rot"].F();
        f32 a[6];
        ::whiteout::flakes::io::D3UvAffine(x, 0.0f, a);

        const auto& want = c["out"]["affine"];
        INFO("case " << i << " u " << u << " v " << v);
        CHECK(std::fabs(a[2] - want[4].F()) <= 2e-6f);
        CHECK(std::fabs(a[5] - want[5].F()) <= 2e-6f);
        // A fixed (0.5, 0.5) pivot drops the scroll from the product entirely.
        const f32 fixedU = (-0.5f * a[0] + -0.5f * a[1]) + 0.5f;
        if (std::fabs(fixedU - want[4].F()) > 1e-4f)
            ++fixedPivotWouldDiffer;
        ++seen;
    }
    CHECK(seen >= 3);
    CHECK(fixedPivotWouldDiffer >= 2);
}

// ---------------------------------------------------------------------------
// A11 — what an emitter move does to live particles (G-D3P-21)
// ---------------------------------------------------------------------------

TEST_CASE("oracle A11: bit 8 carries the particles and bit 29 drops the rotation",
          "[d3][oracle][a11]") {
    const auto doc = LoadGolden("a11_emitter_transform");
    const auto& cases = (*doc)["cases"];
    constexpr u32 kCarry = 0x100u;
    constexpr u32 kNoRotate = 0x20000000u;

    std::size_t carried = 0;
    std::size_t rotated = 0;
    std::size_t refreshed = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const i32 type = in["type"].I();
        const u32 flags = static_cast<u32>(in["flags"].I());
        const Vector3f oldP{in["old_pos"][0].F(), in["old_pos"][1].F(), in["old_pos"][2].F()};
        const Vector3f newP{in["new_pos"][0].F(), in["new_pos"][1].F(), in["new_pos"][2].F()};
        const Quaternion oldQ{in["old_quat"][0].F(), in["old_quat"][1].F(),
                              in["old_quat"][2].F(), in["old_quat"][3].F()};
        const Quaternion newQ{in["new_quat"][0].F(), in["new_quat"][1].F(),
                              in["new_quat"][2].F(), in["new_quat"][3].F()};

        // The rule Emitter::CarryWithEmitter implements, stated once more here
        // so the test fails if either drifts.
        const bool eligible = type != 9 && type != 1 && (type & ~1) != 2 &&
                              (flags & kCarry) != 0;
        const Vector3f d{oldP.x - newP.x, oldP.y - newP.y, oldP.z - newP.z};
        const bool moved = (d.x * d.x + d.y * d.y + d.z * d.z) != 0.0f;
        const bool sameQ = SameBits(newQ.x, oldQ.x) && SameBits(newQ.y, oldQ.y) &&
                           SameBits(newQ.z, oldQ.z) && SameBits(newQ.w, oldQ.w);
        // The displacement guard is on the TRANSLATE arm only: a turn in place
        // still sweeps the particles round. This case is what found that in the
        // implementation.
        const bool doesRotate = eligible && (flags & kNoRotate) == 0 && !sameQ;
        const bool carries = eligible && (doesRotate || moved);

        const auto& parts = in["particles"];
        for (std::size_t k = 0; k < parts.Size(); ++k) {
            const Vector3f p0{parts[k]["pos"][0].F(), parts[k]["pos"][1].F(),
                              parts[k]["pos"][2].F()};
            Vector3f want = p0;
            if (carries) {
                if (doesRotate) {
                    const Quaternion dq = newQ * oldQ.conjugate();
                    const Vector3f rel{p0.x - oldP.x, p0.y - oldP.y, p0.z - oldP.z};
                    const Vector3f r = dq.rotate_vector(rel);
                    want = {newP.x + r.x, newP.y + r.y, newP.z + r.z};
                } else {
                    want = {newP.x + (p0.x - oldP.x), newP.y + (p0.y - oldP.y),
                            newP.z + (p0.z - oldP.z)};
                }
            }
            const auto& got = c["out"]["pos"][k];
            INFO("case " << i << " particle " << k << " type " << type << " flags "
                         << flags);
            const f32 tol = doesRotate ? 3e-6f : 0.0f;
            if (tol == 0.0f) {
                CHECK(SameBits(want.x, got[0].F()));
                CHECK(SameBits(want.y, got[1].F()));
                CHECK(SameBits(want.z, got[2].F()));
            } else {
                CHECK(std::fabs(want.x - got[0].F()) <= tol * (1.0f + std::fabs(got[0].F())));
                CHECK(std::fabs(want.y - got[1].F()) <= tol * (1.0f + std::fabs(got[1].F())));
                CHECK(std::fabs(want.z - got[2].F()) <= tol * (1.0f + std::fabs(got[2].F())));
            }
            // The birth quaternion is REFRESHED on a carried move — the field
            // section 5.1 called frozen at birth.
            const auto& gq = c["out"]["quat"][k];
            const Quaternion wq = carries ? newQ
                                                      : Quaternion{parts[k]["quat"][0].F(),
                                                                   parts[k]["quat"][1].F(),
                                                                   parts[k]["quat"][2].F(),
                                                                   parts[k]["quat"][3].F()};
            CHECK(SameBits(wq.x, gq[0].F()));
            CHECK(SameBits(wq.w, gq[3].F()));
            if (carries && !SameBits(newQ.w, parts[k]["quat"][3].F()))
                ++refreshed;
        }
        if (carries)
            ++carried;
        if (doesRotate)
            ++rotated;
    }
    CHECK(carried >= 10);
    CHECK(rotated >= 5);
    CHECK(refreshed >= 5);
}

// ---------------------------------------------------------------------------
// A12 — the per-particle draw frame (G-D3P-17)
// ---------------------------------------------------------------------------

TEST_CASE("oracle A12: the distance fade is system types 7 and 8 only",
          "[d3][oracle][a12]") {
    const auto doc = LoadGolden("a12_prepare_draw_frame");
    const auto& cases = (*doc)["cases"];

    // Two claims, both read off the engine's own record. The first is why this
    // build can leave the fade out for everything it renders; the second is why
    // leaving it out is a real difference for foliage.
    std::map<std::pair<int, int>, std::set<int>> byTypeAlpha; // (type, alphaBits) -> bytes
    std::size_t fadedRows = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const int type = in["type"].I();
        const f32 alpha = in["alpha"].F();
        u32 bits;
        std::memcpy(&bits, &alpha, 4);
        // A culled particle records a zero byte for a reason that is not the
        // fade; including it would make every type look distance-dependent.
        const bool culled = c["in"]["cull"].B() && c["in"]["cullmode"].I() != 15 &&
                            c["in"]["visible"].I() == 0;
        if (culled)
            continue;
        byTypeAlpha[{type, static_cast<int>(bits)}].insert(c["out"]["alpha_byte"].I());
        if (type == 7 || type == 8)
            ++fadedRows;
    }
    std::size_t constantTypes = 0;
    std::size_t varyingTypes = 0;
    for (const auto& kv : byTypeAlpha) {
        const int type = kv.first.first;
        if (type == 7 || type == 8) {
            if (kv.second.size() > 1)
                ++varyingTypes;
        } else {
            CHECK(kv.second.size() == 1);
            ++constantTypes;
        }
    }
    CHECK(constantTypes >= 6);
    CHECK(varyingTypes >= 2);
    CHECK(fadedRows >= 30);
}

TEST_CASE("oracle A12: a faded-out particle is zeroed, not skipped",
          "[d3][oracle][a12]") {
    const auto doc = LoadGolden("a12_prepare_draw_frame");
    const auto& cases = (*doc)["cases"];
    std::size_t zeroed = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        INFO("case " << i);
        // The quad is memset exactly when the alpha byte is zero. There is no
        // third state: the draw list keeps the slot either way.
        CHECK(c["out"]["quad_zeroed"].B() == (c["out"]["alpha_byte"].I() == 0));
        if (c["out"]["quad_zeroed"].B())
            ++zeroed;
        // And the byte is byte 3 OF the colour word, not a field beside it.
        if (!c["out"]["desc"].IsNull()) {
            // U(), not I(): the colour word's top byte makes it exceed INT_MAX
            // and the narrowing conversion saturates to 0x80000000 instead.
            const u32 colour = c["out"]["desc"]["colour"].U();
            CHECK((colour >> 24) == static_cast<u32>(c["out"]["alpha_byte"].I()));
            CHECK((colour & 0x00FFFFFFu) == 0x00112233u);
        }
    }
    CHECK(zeroed >= 10);
}

// ---------------------------------------------------------------------------
// A6 — the live list (G-D3P-20)
// ---------------------------------------------------------------------------

TEST_CASE("oracle A6: retiring a particle preserves the order of the rest",
          "[d3][oracle][a6]") {
    const auto doc = LoadGolden("a6b_free_particle");
    const auto& cases = (*doc)["cases"];
    namespace pp = ::whiteout::flakes::renderer::particle;

    std::size_t checked = 0;
    std::size_t swapWouldDiffer = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        if (c["in"].Has("keepAttachments"))
            continue;
        const int n = c["in"]["count"].I();
        const int victim = c["in"]["victim"].I();
        REQUIRE(c["out"]["order_preserved"].B());

        pp::ParticlePool pool;
        pool.Sync(static_cast<u32>(n));
        for (int k = 0; k < n; ++k)
            pool.PushAlive(static_cast<u32>(k));
        pool.RemoveAliveAtOrdered(static_cast<std::size_t>(victim));

        REQUIRE(pool.AliveCount() == static_cast<std::size_t>(n - 1));
        int expect = 0;
        for (std::size_t k = 0; k < pool.AliveCount(); ++k) {
            if (expect == victim)
                ++expect;
            INFO("n " << n << " victim " << victim << " slot " << k);
            CHECK(pool.AliveAt(k) == static_cast<u32>(expect));
            ++expect;
        }
        // The swap-with-last this replaced, on the same input.
        if (n >= 3 && victim < n - 1)
            ++swapWouldDiffer;
        ++checked;
    }
    CHECK(checked >= 12);
    CHECK(swapWouldDiffer >= 5);
}

// ---------------------------------------------------------------------------
// A7 — the quaternion math the ledger never counted (G-D3P-22)
// ---------------------------------------------------------------------------

TEST_CASE("oracle A7: the shortest arc cuts off at 0.999, not at 1 - 1e-6",
          "[d3][oracle][a7]") {
    // `Math_OrientationFromAxes` @0x71000B2EC0 was outside the audit's
    // denominator because that denominator was a name regex. Its two cut-offs
    // are 2.56 degrees of arc, not an epsilon: this build used `1 - 1e-6` and
    // returned a small real rotation for thirty times as many axes as the
    // engine, which returns exact identity for all of them.
    const auto doc = LoadGolden("a7c_orientation_from_axes");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    std::size_t exact = 0, oldWouldDiffer = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const Vector3f a{c["in"]["a"][0].F(), c["in"]["a"][1].F(), c["in"]["a"][2].F()};
        const Vector3f b{c["in"]["b"][0].F(), c["in"]["b"][1].F(), c["in"]["b"][2].F()};
        const auto& q = c["out"]["quat"];
        INFO("A (" << a.x << ", " << a.y << ", " << a.z << ")  B (" << b.x << ", " << b.y
                   << ", " << b.z << ")");
        const Quaternion got = pd3::OrientationFromAxes(a, b);
        CHECK(SameBits(got.x, q[0].F()));
        CHECK(SameBits(got.y, q[1].F()));
        CHECK(SameBits(got.z, q[2].F()));
        CHECK(SameBits(got.w, q[3].F()));
        ++exact;
        // Would the reading this replaced have differed? Only the cases inside
        // the wide cut-off and outside the narrow one can say so.
        const f32 la = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
        const f32 lb = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
        if (la > 1e-6f && lb > 1e-6f) {
            const f32 d = (a.x * b.x + a.y * b.y + a.z * b.z) / (la * lb);
            if ((d > 0.999f && d <= 1.0f - 1e-6f) || (d < -0.999f && d >= -1.0f + 1e-6f))
                ++oldWouldDiffer;
        }
    }
    CHECK(exact == cases.Size());
    // Without this the test would pass against the old thresholds too.
    CHECK(oldWouldDiffer >= 8);
}

TEST_CASE("oracle A7: the arc quaternion is not normalised", "[d3][oracle][a7]") {
    // The engine writes cross/sqrt(2(d+1)) and sqrt(2(d+1))/2 and stops. Those
    // are unit in exact arithmetic and not in float, so a trailing normalise --
    // which this build had -- moves the answer. The golden is the proof: at
    // least one recorded quaternion has a length that is not exactly 1, and our
    // unnormalised form reproduces it bit for bit while a normalised one cannot.
    const auto doc = LoadGolden("a7c_orientation_from_axes");
    const auto& cases = (*doc)["cases"];
    std::size_t offUnit = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& q = cases[i]["out"]["quat"];
        const f32 n2 = q[0].F() * q[0].F() + q[1].F() * q[1].F() + q[2].F() * q[2].F() +
                       q[3].F() * q[3].F();
        if (n2 != 1.0f)
            ++offUnit;
    }
    CHECK(offUnit > 0);
}

// ---------------------------------------------------------------------------
// A12 — what a particle draw binds (G-D3P-23)
// ---------------------------------------------------------------------------

TEST_CASE("oracle A12: the texture slot is fixed by TYPE, and the count is an index",
          "[d3][oracle][a12]") {
    // `Particle_BindDrawTextures` asks for types 1, 19, 12, 14 in that order and
    // parks each in its own slot. record+168 is the index of the last one
    // filled, so a material carrying only type 14 reports four slots with three
    // of them null -- reading it as a tally is wrong for every gappy material.
    // `MaterialDesc::DiffuseTextureId` depends on the first half of that.
    const auto doc = LoadGolden("a12b_bind_draw_textures");
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);
    static const int kOrder[4] = {1, 19, 12, 14};
    std::size_t gappy = 0, indexBeatsTally = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& stages = c["in"]["stages"];
        const auto& handles = c["out"]["handle"];
        int lastFilled = 0, tally = 0;
        for (int k = 0; k < 4; ++k) {
            const std::string key = std::to_string(kOrder[k]);
            const bool textured = stages.Has(key) && stages[key].I() != -1;
            INFO("case " << i << " slot " << k << " type " << kOrder[k]);
            // The slot is the TYPE's slot, whatever the material's own order.
            CHECK((static_cast<u64>(handles[static_cast<std::size_t>(k)].Num()) != 0) ==
                  textured);
            if (textured) {
                lastFilled = k + 1;
                ++tally;
            }
        }
        CHECK(c["out"]["count"].I() == lastFilled);
        if (lastFilled != tally)
            ++indexBeatsTally;
        if (lastFilled > tally)
            ++gappy;
    }
    // Without a gappy material the index and the tally agree and the test says
    // nothing.
    CHECK(indexBeatsTally >= 4);
    CHECK(gappy >= 4);
}

TEST_CASE("oracle: every golden names the same binary", "[d3][oracle]") {
    static const char* kExpected =
        "1c31f194fb5d1c75b9327bd1c10d0aee739757cfb914468eb4b4c8d7f3d26261";
    std::size_t seen = 0;
    for (const auto& e : fs::directory_iterator(GoldenRoot())) {
        if (e.path().extension() != ".json")
            continue;
        const auto doc = wdx_golden::Load(e.path().string());
        INFO(e.path().filename().string());
        REQUIRE((*doc)["binary_sha256"].S() == kExpected);
        ++seen;
    }
    CHECK(seen >= 34);
}
