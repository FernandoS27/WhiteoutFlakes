// ============================================================================
// Diablo III particles — the gates for D3_PARTICLE_DESIGN.md phases P0..P4.
//
// P0  the animated path: the MWC stream, the nine distributions, linear
//     evaluation, the held-random clamp past the last node, the loop
//     sub-range, and the driver.
// P1  the adapter: every `.prt` in the corpus parses, the slot -> channel-id
//     table lands on the path TYPE the engine expects, and the capability
//     mask is derived rather than guessed.
// P2  emission and birth: the accumulator including its fractional carry, and
//     the seven shape distributions as histograms rather than eyeballs.
// P3  motion: one model at a time, with a synthetic single-channel desc, so a
//     wrong sign shows up as a wrong shape and not as "the effect looks odd".
// P4  orientation: roll, free spin, and the reversed sign on channel 16.
//
// The corpus sweep is skipped without `C:/Projects/WhiteoutLib/Corpus/D3`
// (override with WDX_TEST_D3_CORPUS). Skipped is not passed — the sweep prints
// what it covered.
// ============================================================================

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "io/d3/d3_particle_adapter.h"
#include "renderer/particle/d3_channels.h"
#include "renderer/particle/d3_emitter.h"
#include "renderer/particle/d3_emitter_desc.h"
#include "renderer/particle/d3_path.h"
#include "renderer/particle/particle_geometry.h"

#include <whiteout/sno/d3/native/d3_native.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace d3n = ::whiteout::sno::d3::native;
namespace pd3 = ::whiteout::flakes::renderer::particle::d3;
using namespace ::whiteout;
using whiteout::flakes::renderer::particle::Particle2;

namespace {

constexpr f32 kTwoPi = 6.28318530717958647692f;

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_D3_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/D3");
}

std::size_t SweepLimit() {
    if (const char* v = std::getenv("WDX_TEST_D3_LIMIT"); v && *v)
        return static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
    return 3000;
}

/// The extracted corpus groups assets by SNO group, so the `.prt` live one
/// level down in `Particle/`. Falls back to the root for a flat extraction.
std::vector<fs::path> FindPrt(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    fs::path dir = root / "Particle";
    if (!fs::is_directory(dir, ec))
        dir = root;
    if (!fs::is_directory(dir, ec))
        return out;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (it->is_regular_file(ec) && it->path().extension() == ".prt")
            out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<u8> ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return {};
    f.seekg(0, std::ios::end);
    const auto n = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<u8> b(n);
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
    return b;
}

/// A one-node constant path, the shape 99% of shipped channels take.
pd3::Path ConstPath(f32 v) {
    pd3::Path p;
    p.components = 1;
    p.nodes.push_back({{v, 0, 0, 0}, {v, 0, 0, 0}, 0.0f});
    return p;
}

pd3::Path ConstVectorPath(f32 x, f32 y, f32 z) {
    pd3::Path p;
    p.components = 3;
    p.nodes.push_back({{x, y, z, 0}, {x, y, z, 0}, 0.0f});
    return p;
}

/// Two nodes, so the evaluator takes the interpolating branch rather than the
/// constant fast path.
pd3::Path RampPath(f32 a, f32 b) {
    pd3::Path p;
    p.components = 1;
    p.nodes.push_back({{a, 0, 0, 0}, {a, 0, 0, 0}, 0.0f});
    p.nodes.push_back({{b, 0, 0, 0}, {b, 0, 0, 0}, 1.0f});
    return p;
}

pd3::EvalCtx AtTime(f32 t) {
    pd3::EvalCtx c;
    c.timeMode = 1;
    c.time = t;
    c.period = 1.0f;
    return c;
}

} // namespace

// ---------------------------------------------------------------------------
// P0 — the path
// ---------------------------------------------------------------------------

TEST_CASE("d3 particle P0: the MWC stream is the engine's", "[d3][particle][p0]") {
    // The generator is `hi + 1791398085 * lo` in 64 bits, keeping both halves.
    // Transcribed rather than approximated, so a literal check is the whole
    // test: everything downstream is a pure function of this sequence.
    pd3::MwcRng r = pd3::MwcRng::Seed(1);
    const u64 s = 666ull + 1791398085ull * 1ull;
    REQUIRE(r.Next() == static_cast<u32>(s));
    REQUIRE(r.lo == static_cast<u32>(s));
    REQUIRE(r.hi == static_cast<u32>(s >> 32));

    // Seeding is `(channelId * particleSeed, 666)` on EVERY evaluation, which
    // is what makes a channel's draw a pure function of (particle, channel)
    // and independent of evaluation order.
    const pd3::MwcRng a = pd3::MwcRng::ForChannel(3, 12345u);
    const pd3::MwcRng b = pd3::MwcRng::Seed(3u * 12345u);
    REQUIRE(a.lo == b.lo);
    REQUIRE(a.hi == 666u);

    // Uniform, and never outside [0,1).
    pd3::MwcRng u = pd3::MwcRng::Seed(0x9E3779B9u);
    f64 sum = 0.0;
    for (int i = 0; i < 20000; ++i) {
        const f32 v = u.NextUnit();
        REQUIRE(v >= 0.0f);
        REQUIRE(v < 1.0f);
        sum += v;
    }
    REQUIRE(std::fabs(sum / 20000.0 - 0.5) < 0.02);
}

TEST_CASE("d3 particle P0: all nine distributions stay in range and keep their bias",
          "[d3][particle][p0]") {
    // Values 0..8 only; the switch's default returns 0 for anything else, and
    // the corpus holds nothing outside the range over 798,941 paths.
    REQUIRE(pd3::ApplyDistribution(9, 0.5f) == 0.0f);
    REQUIRE(pd3::ApplyDistribution(-1, 0.5f) == 0.0f);

    for (i32 dist = 0; dist <= 8; ++dist) {
        f64 mean = 0.0;
        const int N = 2001;
        for (int i = 0; i < N; ++i) {
            const f32 u = static_cast<f32>(i) / static_cast<f32>(N);
            const f32 v = pd3::ApplyDistribution(dist, u);
            INFO("dist=" << dist << " u=" << u << " -> " << v);
            REQUIRE(v >= 0.0f);
            REQUIRE(v <= 1.0f);
            mean += v;
        }
        mean /= N;

        // The bias each remap is FOR. 1 and 5 pull toward `start`, 3 and 7
        // toward `end`; the four tent shapes stay centred.
        switch (dist) {
        case 0:
            REQUIRE(std::fabs(mean - 0.5) < 0.02);
            break;
        case 1: // u^2
        case 5: // 1 - sqrt(u)
            REQUIRE(mean < 0.4);
            break;
        case 3: // 1 - u^2
        case 7: // sqrt(u)
            REQUIRE(mean > 0.6);
            break;
        default:
            REQUIRE(std::fabs(mean - 0.5) < 0.06);
            break;
        }
    }

    // Two exact values, so a transcription slip cannot hide inside a mean.
    REQUIRE_THAT(pd3::ApplyDistribution(1, 0.5f),
                 Catch::Matchers::WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(pd3::ApplyDistribution(3, 0.5f),
                 Catch::Matchers::WithinAbs(0.75f, 1e-6f));
    REQUIRE_THAT(pd3::ApplyDistribution(7, 0.25f),
                 Catch::Matchers::WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(pd3::ApplyDistribution(5, 0.25f),
                 Catch::Matchers::WithinAbs(0.5f, 1e-6f));
}

TEST_CASE("d3 particle P0: start..end is a per-particle random range, not a keyframe pair",
          "[d3][particle][p0]") {
    // ONE node whose start and end differ. Every particle gets a different
    // value from it and keeps that value for its whole life — this is the
    // single most load-bearing property of the format, and reading the pair as
    // a keyframe would make every such channel animate instead.
    pd3::Path p;
    p.components = 1;
    p.nodes.push_back({{10.0f, 0, 0, 0}, {20.0f, 0, 0, 0}, 0.0f});

    std::vector<f32> seen;
    for (u32 seed = 1; seed <= 64; ++seed) {
        const f32 a = p.EvalScalar(seed, pd3::kChSize, AtTime(0.0f));
        const f32 b = p.EvalScalar(seed, pd3::kChSize, AtTime(0.9f));
        REQUIRE(a >= 10.0f);
        REQUIRE(a <= 20.0f);
        // Stable across time for one particle: the draw is re-seeded from
        // (channel, particle) every evaluation, so it cannot drift.
        REQUIRE(a == b);
        seen.push_back(a);
    }
    // ...and different across particles.
    std::sort(seen.begin(), seen.end());
    REQUIRE(std::unique(seen.begin(), seen.end()) - seen.begin() > 32);

    // A different channel id on the same particle is a different stream.
    REQUIRE(p.EvalScalar(7u, pd3::kChSize, AtTime(0.0f)) !=
            p.EvalScalar(7u, pd3::kChAlpha, AtTime(0.0f)));
}

TEST_CASE("d3 particle P0: evaluation is linear, and past the last node it holds the RANDOM",
          "[d3][particle][p0]") {
    const pd3::Path ramp = RampPath(0.0f, 100.0f);
    REQUIRE_THAT(ramp.EvalScalar(5u, pd3::kChSize, AtTime(0.0f)),
                 Catch::Matchers::WithinAbs(0.0f, 1e-4f));
    REQUIRE_THAT(ramp.EvalScalar(5u, pd3::kChSize, AtTime(0.25f)),
                 Catch::Matchers::WithinAbs(25.0f, 1e-3f));
    REQUIRE_THAT(ramp.EvalScalar(5u, pd3::kChSize, AtTime(0.5f)),
                 Catch::Matchers::WithinAbs(50.0f, 1e-3f));
    // No easing anywhere: the field the old spec read as a curve type is the
    // node count, so there is nothing in the format that could bend this.
    REQUIRE_THAT(ramp.EvalScalar(5u, pd3::kChSize, AtTime(0.75f)),
                 Catch::Matchers::WithinAbs(75.0f, 1e-3f));

    // Past the last node the result is `lerp(start, end, r)` — a held RANDOM
    // value, not a held interpolated one. With a range on the final node the
    // two differ, which is what makes this checkable.
    pd3::Path tail;
    tail.components = 1;
    tail.nodes.push_back({{0.0f, 0, 0, 0}, {0.0f, 0, 0, 0}, 0.0f});
    tail.nodes.push_back({{1.0f, 0, 0, 0}, {3.0f, 0, 0, 0}, 0.5f});
    const f32 held = tail.EvalScalar(11u, pd3::kChSize, AtTime(0.9f));
    REQUIRE(held >= 1.0f);
    REQUIRE(held <= 3.0f);
    // Still held anywhere past the last node time but inside the loop range.
    // Past `loopEnd` it would WRAP instead, which is a different rule.
    REQUIRE(held == tail.EvalScalar(11u, pd3::kChSize, AtTime(0.99f)));
}

TEST_CASE("d3 particle P0: the loop sub-range wraps normalised time", "[d3][particle][p0]") {
    pd3::Path p = RampPath(0.0f, 1.0f);
    p.loopStart = 0.25f;
    p.loopEnd = 0.75f;

    // Inside the range, time passes through untouched.
    REQUIRE_THAT(p.EvalScalar(1u, pd3::kChSize, AtTime(0.5f)),
                 Catch::Matchers::WithinAbs(0.5f, 1e-3f));
    // Past `loopEnd` it folds back into [loopStart, loopEnd) rather than
    // clamping, so a looping channel keeps cycling for the particle's life.
    const f32 wrapped = p.EvalScalar(1u, pd3::kChSize, AtTime(0.8f));
    REQUIRE(wrapped >= 0.25f);
    REQUIRE(wrapped < 0.75f);
    REQUIRE_THAT(wrapped, Catch::Matchers::WithinAbs(0.3f, 1e-3f));
}

TEST_CASE("d3 particle P0: the driver multiplies, and only for the modes a viewer can supply",
          "[d3][particle][p0]") {
    pd3::Path p = ConstPath(4.0f);

    // Mode 0 is 99.2% of authored paths and applies nothing at all — which is
    // NOT the same as multiplying by [lo, hi].
    p.driver = {0, 0.0f, 0.5f};
    REQUIRE_THAT(p.EvalScalar(1u, pd3::kChSize, AtTime(0.0f)),
                 Catch::Matchers::WithinAbs(4.0f, 1e-5f));

    // Mode 3 reads the normalised distance from the emitter. The path is no
    // longer constant-with-no-driver, so this also exercises the slow branch.
    p.driver = {3, 0.0f, 1.0f};
    pd3::EvalCtx c = AtTime(0.0f);
    c.driver.distNorm = 0.5f;
    REQUIRE_THAT(p.EvalScalar(1u, pd3::kChSize, c), Catch::Matchers::WithinAbs(2.0f, 1e-4f));

    // Every other non-zero mode reaches gameplay state we do not have. The
    // honest answer is "unmodulated", identical to mode 0 — not zero, which
    // would silently blank the channel.
    p.driver = {8, 0.0f, 1.0f};
    REQUIRE_THAT(p.EvalScalar(1u, pd3::kChSize, c), Catch::Matchers::WithinAbs(4.0f, 1e-4f));
}

TEST_CASE("d3 particle P0: a vector channel draws one random PER COMPONENT",
          "[d3][particle][p0]") {
    // One draw reused for all three would make every vector channel diagonal:
    // x, y and z would pick the same point in their ranges. The engine calls
    // DrawRandom three times, so the components are independent.
    pd3::Path p;
    p.components = 3;
    p.nodes.push_back({{0, 0, 0, 0}, {1, 1, 1, 0}, 0.0f});

    int diagonal = 0;
    for (u32 seed = 1; seed <= 64; ++seed) {
        const Vector3f v = p.EvalVector(seed, pd3::kChOffsetA, AtTime(0.0f));
        REQUIRE(v.x >= 0.0f);
        REQUIRE(v.x <= 1.0f);
        if (v.x == v.y && v.y == v.z)
            ++diagonal;
    }
    REQUIRE(diagonal == 0);
}

// ---------------------------------------------------------------------------
// P2 — the shape samplers
// ---------------------------------------------------------------------------

TEST_CASE("d3 particle P2: an extent pair is an area-uniform annulus, not a diameter",
          "[d3][particle][p2]") {
    // r = sqrt(lo^2 + U*(hi^2 - lo^2)). Area-uniform means the MEDIAN radius
    // of a disc is at 1/sqrt(2) of the outer radius, not at the middle.
    pd3::MwcRng rng = pd3::MwcRng::Seed(99);
    std::vector<f32> rs;
    for (int i = 0; i < 20000; ++i)
        rs.push_back(pd3::SampleRadiusInAnnulus(rng, 0.0f, 1.0f));
    std::sort(rs.begin(), rs.end());
    REQUIRE(rs.front() >= 0.0f);
    REQUIRE(rs.back() <= 1.0f);
    REQUIRE_THAT(rs[rs.size() / 2], Catch::Matchers::WithinAbs(0.7071f, 0.02f));

    // An inner radius is respected exactly: nothing lands inside the hole.
    rs.clear();
    for (int i = 0; i < 20000; ++i)
        rs.push_back(pd3::SampleRadiusInAnnulus(rng, 2.0f, 1.0f));
    std::sort(rs.begin(), rs.end());
    REQUIRE(rs.front() >= 2.0f - 1e-4f);
    REQUIRE(rs.back() <= 3.0f + 1e-4f);

    // A zero-width annulus consumes NO draw. The random stream is positional,
    // so a spurious draw here would desynchronise every later channel.
    pd3::MwcRng a = pd3::MwcRng::Seed(7);
    pd3::MwcRng b = pd3::MwcRng::Seed(7);
    (void)pd3::SampleRadiusInAnnulus(a, 5.0f, 0.0f);
    REQUIRE(a.lo == b.lo);
    REQUIRE(a.hi == b.hi);
}

TEST_CASE("d3 particle P2: shape 4 is a full sphere and shape 10 is the +Z half",
          "[d3][particle][p2]") {
    // The ONLY difference between the two samplers is that the elevation draw
    // is [-1,1) for one and [0,1) for the other. That single line is the whole
    // of what separates emitter shapes 4 and 10, so it gets its own gate.
    pd3::MwcRng rng = pd3::MwcRng::Seed(4242);
    int below = 0;
    for (int i = 0; i < 8000; ++i) {
        const Vector3f v = pd3::SamplePointOnSphere(rng, 3.0f);
        REQUIRE_THAT(std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z),
                     Catch::Matchers::WithinAbs(3.0f, 1e-3f));
        if (v.z < 0.0f)
            ++below;
    }
    REQUIRE(below > 3000);
    REQUIRE(below < 5000);

    below = 0;
    for (int i = 0; i < 8000; ++i) {
        const Vector3f v = pd3::SamplePointOnHemisphere(rng, 3.0f);
        REQUIRE_THAT(std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z),
                     Catch::Matchers::WithinAbs(3.0f, 1e-3f));
        if (v.z < -1e-4f)
            ++below;
    }
    REQUIRE(below == 0);
}

TEST_CASE("d3 particle P2: the circle sampler stays in XY", "[d3][particle][p2]") {
    pd3::MwcRng rng = pd3::MwcRng::Seed(5);
    for (int i = 0; i < 2000; ++i) {
        const Vector3f v = pd3::SamplePointOnCircleXY(rng, 2.5f);
        REQUIRE(v.z == 0.0f);
        REQUIRE_THAT(std::sqrt(v.x * v.x + v.y * v.y),
                     Catch::Matchers::WithinAbs(2.5f, 1e-3f));
    }
}

// ---------------------------------------------------------------------------
// P2 — emission
// ---------------------------------------------------------------------------

namespace {

/// A minimal live emitter: one channel of lifetime, one of rate, nothing else.
std::shared_ptr<pd3::EmitterDesc> MakeDesc(f32 ratePerFrame, f32 lifeFrames) {
    auto d = std::make_shared<pd3::EmitterDesc>();
    d->systemType = 0;
    d->lifetime = 0.0f; // never finishes emitting
    d->emissionPeriod = 10.0f;
    d->maxDistance = 0.0f; // no kill radius, so the count is the accumulator's
    d->channels[pd3::kChEmissionRate] = ConstPath(ratePerFrame);
    d->channels[pd3::kChParticleLife] = ConstPath(lifeFrames);
    d->channels[pd3::kChBirthSize] = ConstPath(1.0f);
    d->DeriveCapabilities();
    return d;
}

} // namespace

TEST_CASE("d3 particle P2: the emission accumulator carries its fraction",
          "[d3][particle][p2]") {
    // 0.5 particles per frame at 60 fps is 30/s; over 120 frames of 1/60 s
    // that is exactly 60 particles, and getting there requires the fractional
    // remainder to survive each step rather than being floored away.
    pd3::Emitter e;
    e.SetD3Desc(MakeDesc(0.5f, 600.0f));
    e.SetVisible(true);
    e.SetWorldPosition({0, 0, 0});

    i32 total = 0;
    for (int i = 0; i < 120; ++i) {
        e.Update(1.0f / 60.0f, 1.0f);
        total += e.EmittedLastUpdate();
    }
    REQUIRE(total == 60);
    REQUIRE(static_cast<i32>(e.Pool().AliveCount()) == 60);

    // The scaler is a plain multiplier on the same accumulator.
    pd3::Emitter half;
    half.SetD3Desc(MakeDesc(0.5f, 600.0f));
    half.SetVisible(true);
    half.SetWorldPosition({0, 0, 0});
    i32 scaled = 0;
    for (int i = 0; i < 120; ++i) {
        half.Update(1.0f / 60.0f, 0.5f);
        scaled += half.EmittedLastUpdate();
    }
    REQUIRE(scaled == 30);
}

TEST_CASE("d3 particle P2: a particle dies at its authored lifetime", "[d3][particle][p2]") {
    // 30 frames is half a second. The lifetime channel is a TimePath in frames
    // and the conversion is x1/60 — a unit slip here would make every effect
    // in the game 60 times too long or too short.
    pd3::Emitter e;
    e.SetD3Desc(MakeDesc(60.0f, 30.0f));
    e.SetVisible(true);
    e.SetWorldPosition({0, 0, 0});

    e.Update(1.0f / 60.0f, 1.0f);
    REQUIRE(e.Pool().AliveCount() > 0);
    const u32 idx = e.Pool().AliveAt(0);
    REQUIRE_THAT(e.States()[idx].lifetime, Catch::Matchers::WithinAbs(0.5f, 1e-4f));

    // Stop emitting and run past the lifetime: everything must be gone.
    e.SetVisible(false);
    for (int i = 0; i < 60; ++i)
        e.Update(1.0f / 60.0f, 1.0f);
    REQUIRE(e.Pool().AliveCount() == 0);
}

// ---------------------------------------------------------------------------
// P3 — motion, one model at a time
// ---------------------------------------------------------------------------

namespace {

/// Emit exactly one particle into @p e, then leave it stepping without
/// emitting again. Fills a caller-owned emitter rather than returning one:
/// Emitter2 declares a virtual destructor, so it has no move constructor and
/// its unique_ptr trail list makes it non-copyable.
void SpawnOne(pd3::Emitter& e, std::shared_ptr<pd3::EmitterDesc> d,
              const Vector3f& at = {0, 0, 0}) {
    e.SetD3Desc(std::move(d));
    e.SetVisible(true);
    e.SetWorldPosition(at);
    e.SetWorldPosition(at); // seed prev == current, so no sub-frame spread
    e.Update(1.0f / 60.0f, 1.0f);
    e.SetVisible(false);
}

const Particle2& Only(const pd3::Emitter& e) {
    REQUIRE(e.Pool().AliveCount() == 1);
    return e.Pool()[e.Pool().AliveAt(0)];
}

} // namespace

TEST_CASE("d3 particle P3: the radial model pushes along a straight ray", "[d3][particle][p3]") {
    auto d = MakeDesc(1.0f, 600.0f);
    // Born on a ring so the radial direction is defined; a particle exactly at
    // the origin has no direction to push along and must not move.
    d->shape = pd3::Shape::Cylinder;
    d->shapeExtent0 = ConstPath(1.0f);
    d->shapeExtent1 = ConstPath(0.0f);
    d->channels[pd3::kChRadialSpeed] = ConstPath(1.0f); // x60 -> 60 units/s
    d->DeriveCapabilities();
    REQUIRE(d->Cap(pd3::kCapRadial));

    pd3::Emitter e;
    SpawnOne(e, d);
    const Vector3f start = Only(e).position;
    const f32 r0 = std::sqrt(start.x * start.x + start.y * start.y);
    // Born on the unit ring, then advanced by the REMAINDER of the emitting
    // frame — 60 u/s for 1/60 s — before anyone looks at it. A newborn that
    // had not been stepped would read 1.0 here, which is the bug this pins.
    REQUIRE_THAT(r0, Catch::Matchers::WithinAbs(2.0f, 1e-3f));

    const Vector3f dir{start.x / r0, start.y / r0, 0.0f};
    for (int i = 0; i < 60; ++i)
        e.Update(1.0f / 60.0f, 1.0f);

    const Vector3f end = Only(e).position;
    const f32 r1 = std::sqrt(end.x * end.x + end.y * end.y);
    // One second at 60 u/s, from radius 1. The first (birth) step counts too.
    REQUIRE(r1 > 55.0f);
    REQUIRE(r1 < 65.0f);
    // Straight: it never left the ray it started on.
    REQUIRE_THAT(end.x / r1, Catch::Matchers::WithinAbs(dir.x, 1e-3f));
    REQUIRE_THAT(end.y / r1, Catch::Matchers::WithinAbs(dir.y, 1e-3f));
}

TEST_CASE("d3 particle P3: the orbit model traces a circle of the authored radius",
          "[d3][particle][p3]") {
    auto d = MakeDesc(1.0f, 600.0f);
    d->shape = pd3::Shape::Cylinder;
    d->shapeExtent0 = ConstPath(4.0f);
    d->shapeExtent1 = ConstPath(0.0f);
    d->channels[pd3::kChOrbitAxis] = ConstVectorPath(0, 0, 1);
    d->channels[pd3::kChOrbitAngSpeed] = ConstPath(kTwoPi / 60.0f); // x60 -> 1 rev/s
    d->DeriveCapabilities();
    REQUIRE(d->Cap(pd3::kCapOrbit));

    pd3::Emitter e;
    SpawnOne(e, d);
    const Vector3f start = Only(e).position;

    f32 minR = 1e9f, maxR = 0.0f;
    for (int i = 0; i < 60; ++i) {
        e.Update(1.0f / 60.0f, 1.0f);
        const Vector3f p = Only(e).position;
        const f32 r = std::sqrt(p.x * p.x + p.y * p.y);
        minR = std::fmin(minR, r);
        maxR = std::fmax(maxR, r);
    }
    // The radius is preserved — that is what makes it an orbit and not a
    // spiral, and a sign error in the rotation would show up here first.
    REQUIRE_THAT(minR, Catch::Matchers::WithinAbs(4.0f, 0.05f));
    REQUIRE_THAT(maxR, Catch::Matchers::WithinAbs(4.0f, 0.05f));

    // One revolution per second brings it back where it started.
    const Vector3f end = Only(e).position;
    REQUIRE(std::fabs(end.x - start.x) < 0.5f);
    REQUIRE(std::fabs(end.y - start.y) < 0.5f);
}

TEST_CASE("d3 particle P3: an authored OFFSET curve produces momentum, not a teleport",
          "[d3][particle][p3]") {
    // The differentiation idiom: every (offset, rate) pair combines as
    // `rate*60 + (now - prev)/dt`. So a ramping offset channel makes a
    // particle KEEP moving after the ramp ends, which is the whole reason the
    // engine caches a previous sample per channel.
    auto d = MakeDesc(1.0f, 600.0f);
    pd3::Path ramp;
    ramp.components = 3;
    ramp.nodes.push_back({{0, 0, 0, 0}, {0, 0, 0, 0}, 0.0f});
    ramp.nodes.push_back({{0, 0, 10, 0}, {0, 0, 10, 0}, 0.1f});
    d->channels[pd3::kChOffsetA] = ramp;
    d->DeriveCapabilities();
    REQUIRE(d->Cap(pd3::kCapTripleA));

    pd3::Emitter e;
    SpawnOne(e, d);
    // Run past the end of the ramp (0.1 of a 10 s life = 1 s).
    for (int i = 0; i < 90; ++i)
        e.Update(1.0f / 60.0f, 1.0f);
    const f32 zA = Only(e).position.z;
    for (int i = 0; i < 30; ++i)
        e.Update(1.0f / 60.0f, 1.0f);
    const f32 zB = Only(e).position.z;

    // It travelled during the ramp...
    REQUIRE(zA > 5.0f);
    // ...and stopped once the curve went flat, because the derivative did.
    REQUIRE_THAT(zB, Catch::Matchers::WithinAbs(zA, 0.01f));
}

TEST_CASE("d3 particle P3: triple B is emitter-local, triple A is world", "[d3][particle][p3]") {
    // The spec's long-standing open question. Rotating the emitter must move
    // triple B's displacement and leave triple A's alone.
    auto make = [](i32 offsetChannel, i32 velChannel) {
        auto d = MakeDesc(1.0f, 600.0f);
        (void)offsetChannel;
        d->channels[velChannel] = ConstVectorPath(1.0f / 60.0f, 0, 0); // x60 -> 1 u/s in +X
        d->DeriveCapabilities();
        return d;
    };

    const Quaternion yaw90 = Quaternion::from_axis_angle({0, 0, 1}, kTwoPi * 0.25f);

    pd3::Emitter world;
    world.SetD3Desc(make(pd3::kChOffsetA, pd3::kChVelocityA));
    world.SetEmitterOrientation(yaw90);
    world.SetVisible(true);
    world.SetWorldPosition({0, 0, 0});
    world.SetWorldPosition({0, 0, 0});
    world.Update(1.0f / 60.0f, 1.0f);
    world.SetVisible(false);
    for (int i = 0; i < 60; ++i)
        world.Update(1.0f / 60.0f, 1.0f);

    pd3::Emitter local;
    local.SetD3Desc(make(pd3::kChOffsetB, pd3::kChVelocityB));
    local.SetEmitterOrientation(yaw90);
    local.SetVisible(true);
    local.SetWorldPosition({0, 0, 0});
    local.SetWorldPosition({0, 0, 0});
    local.Update(1.0f / 60.0f, 1.0f);
    local.SetVisible(false);
    for (int i = 0; i < 60; ++i)
        local.Update(1.0f / 60.0f, 1.0f);

    const Vector3f w = Only(world).position;
    const Vector3f l = Only(local).position;

    // World space: +X stays +X however the emitter is turned.
    REQUIRE(w.x > 0.9f);
    REQUIRE(std::fabs(w.y) < 0.05f);
    // Emitter-local: the birth quaternion turned +X into +Y.
    REQUIRE(std::fabs(l.x) < 0.05f);
    REQUIRE(l.y > 0.9f);
}

// ---------------------------------------------------------------------------
// P4 — orientation
// ---------------------------------------------------------------------------

TEST_CASE("d3 particle P4: roll accumulates and wraps into [0, 2pi]", "[d3][particle][p4]") {
    auto d = MakeDesc(1.0f, 600.0f);
    d->channels[pd3::kChRollRate] = ConstPath(kTwoPi / 60.0f); // x60 -> 1 rev/s
    d->DeriveCapabilities();
    REQUIRE(d->Cap(pd3::kCapRoll));

    pd3::Emitter e;
    SpawnOne(e, d);
    for (int i = 0; i < 30; ++i)
        e.Update(1.0f / 60.0f, 1.0f);
    const u32 idx = e.Pool().AliveAt(0);
    // Half a second at one revolution per second, plus the birth step.
    REQUIRE(e.States()[idx].rollAngle > 3.0f);
    REQUIRE(e.States()[idx].rollAngle <= kTwoPi);

    for (int i = 0; i < 300; ++i)
        e.Update(1.0f / 60.0f, 1.0f);
    // Still wrapped after five more revolutions rather than growing without
    // bound — the engine clamps to 8pi and then folds.
    REQUIRE(e.States()[idx].rollAngle >= 0.0f);
    REQUIRE(e.States()[idx].rollAngle <= kTwoPi);
}

TEST_CASE("d3 particle P4: channel 16's difference is REVERSED", "[d3][particle][p4]") {
    // `w += (prev - now)/dt` where every other pair in the system is
    // `now - prev`. Re-checked at instruction level (`FSUB S1, prev, now` at
    // 0x71000BF124), so a rising angle curve must spin the particle
    // NEGATIVELY about its axis. Reproduced, not repaired.
    auto d = MakeDesc(1.0f, 600.0f);
    d->channels[pd3::kChSpinAxis] = ConstVectorPath(0, 0, 1);
    pd3::Path rising;
    rising.components = 1;
    rising.nodes.push_back({{0.0f, 0, 0, 0}, {0.0f, 0, 0, 0}, 0.0f});
    rising.nodes.push_back({{1.0f, 0, 0, 0}, {1.0f, 0, 0, 0}, 1.0f});
    d->channels[pd3::kChSpinAngle] = rising;
    d->DeriveCapabilities();
    REQUIRE(d->Cap(pd3::kCapSpin));

    pd3::Emitter e;
    SpawnOne(e, d);
    for (int i = 0; i < 60; ++i)
        e.Update(1.0f / 60.0f, 1.0f);

    const u32 idx = e.Pool().AliveAt(0);
    const Quaternion q = e.States()[idx].orientation;
    // A rotation about +Z with a negative angle has a negative z component.
    REQUIRE(q.z < 0.0f);
}

// ---------------------------------------------------------------------------
// P1 — the adapter, over the corpus
// ---------------------------------------------------------------------------

TEST_CASE("d3 particle P1: every corpus .prt parses into a desc", "[d3][particle][p1][corpus]") {
    const fs::path root = CorpusRoot();
    const std::vector<fs::path> files = FindPrt(root);
    if (files.empty()) {
        WARN("no .prt corpus at " << root.string() << " — set WDX_TEST_D3_CORPUS. SKIPPED.");
        return;
    }

    const std::size_t limit = SweepLimit();
    const std::size_t n = (limit == 0) ? files.size() : std::min(limit, files.size());

    std::map<i32, std::size_t> systemTypes;
    std::map<i32, std::size_t> shapes;
    std::map<u32, std::size_t> capBits;
    std::size_t parsed = 0, withRate = 0, distributionsSeen = 0;
    std::map<i32, std::size_t> distributions;

    for (std::size_t i = 0; i < n; ++i) {
        const std::vector<u8> bytes = ReadAll(files[i]);
        auto prt = d3n::parseParticle(bytes);
        INFO("file " << files[i].filename().string());
        REQUIRE(prt.has_value());
        ++parsed;

        auto desc = whiteout::flakes::io::d3::BuildD3EmitterDesc(*prt, -1);
        REQUIRE(desc != nullptr);

        ++systemTypes[desc->systemType];
        ++shapes[static_cast<i32>(desc->shape)];
        for (u32 b = 0; b < 16; ++b)
            if (desc->caps & (1u << b))
                ++capBits[1u << b];
        if (desc->Has(pd3::kChEmissionRate))
            ++withRate;

        // Nothing outside 0..8 exists in the format, and anything that did
        // would silently evaluate to `r = 0` rather than failing.
        for (const pd3::Path& p : desc->channels) {
            if (p.nodes.empty())
                continue;
            REQUIRE(p.distribution >= 0);
            REQUIRE(p.distribution <= 8);
            ++distributions[p.distribution];
            ++distributionsSeen;
            // Node times are normalised and monotone; the evaluator's linear
            // scan depends on both.
            f32 prev = -1.0f;
            for (const pd3::PathNode& node : p.nodes) {
                REQUIRE(node.time >= prev);
                prev = node.time;
            }
        }
    }

    std::printf("[d3-prt] parsed %zu of %zu files (limit %zu)\n", parsed, files.size(), limit);
    std::printf("[d3-prt] eSystemType:");
    for (const auto& [v, c] : systemTypes)
        std::printf(" %d=%zu", v, c);
    std::printf("\n[d3-prt] eEmitterShape:");
    for (const auto& [v, c] : shapes)
        std::printf(" %d=%zu", v, c);
    std::printf("\n[d3-prt] capability bits:");
    for (const auto& [v, c] : capBits)
        std::printf(" 0x%04X=%zu", v, c);
    std::printf("\n[d3-prt] distributions over %zu live channels:", distributionsSeen);
    for (const auto& [v, c] : distributions)
        std::printf(" %d=%zu", v, c);
    std::printf("\n[d3-prt] with an emission-rate channel: %zu\n", withRate);

    // The shipped set is closed, and a value outside it means the slot table
    // is being read at the wrong offset.
    for (const auto& [v, c] : systemTypes) {
        INFO("eSystemType " << v << " x" << c);
        REQUIRE(v >= 0);
        REQUIRE(v <= 10);
    }
    for (const auto& [v, c] : shapes) {
        INFO("eEmitterShape " << v << " x" << c);
        REQUIRE(v >= 1);
        REQUIRE(v <= 11);
        REQUIRE(v != 2);
        REQUIRE(v != 3);
    }
}

TEST_CASE("d3 particle P1: a corpus asset simulates without producing NaN",
          "[d3][particle][p1][corpus]") {
    const fs::path root = CorpusRoot();
    const std::vector<fs::path> files = FindPrt(root);
    if (files.empty()) {
        WARN("no .prt corpus at " << root.string() << " — SKIPPED.");
        return;
    }

    // A bounded sample, deterministically the first N in sorted order. This is
    // the end-to-end gate: parse, adapt, emit, simulate, and build geometry,
    // asserting only that nothing produces a non-finite number — which is
    // exactly what a bad unit conversion or a divide by a zero lifetime does.
    const std::size_t n = std::min<std::size_t>(400, files.size());
    std::size_t simulated = 0, everEmitted = 0;
    std::size_t verts = 0;

    Matrix44f view = Matrix44f::identity();
    whiteout::flakes::renderer::particle::BuildGeometryInput in{};
    in.worldToView = &view;

    for (std::size_t i = 0; i < n; ++i) {
        const std::vector<u8> bytes = ReadAll(files[i]);
        auto prt = d3n::parseParticle(bytes);
        if (!prt)
            continue;
        auto desc = whiteout::flakes::io::d3::BuildD3EmitterDesc(*prt, -1);

        pd3::Emitter e;
        e.SetD3Desc(desc);
        e.SetVisible(true);
        e.SetWorldPosition({0, 0, 0});
        e.SetWorldPosition({0, 0, 0});

        for (int f = 0; f < 60; ++f)
            e.Update(1.0f / 60.0f, 1.0f);
        ++simulated;
        if (e.Pool().AliveCount() > 0)
            ++everEmitted;

        INFO("file " << files[i].filename().string());
        for (std::size_t k = 0; k < e.Pool().AliveCount(); ++k) {
            const u32 idx = e.Pool().AliveAt(k);
            const Particle2& p = e.Pool()[idx];
            REQUIRE(std::isfinite(p.position.x));
            REQUIRE(std::isfinite(p.position.y));
            REQUIRE(std::isfinite(p.position.z));
            const pd3::ParticleState& st = e.States()[idx];
            REQUIRE(std::isfinite(st.size));
            REQUIRE(st.size > 0.0f);
            REQUIRE(std::isfinite(st.color.w));
        }

        std::vector<whiteout::flakes::renderer::Vertex> out;
        const i32 vc = e.BuildGeometry(in, out);
        REQUIRE(vc == static_cast<i32>(out.size()));
        verts += out.size();
        for (const auto& v : out) {
            REQUIRE(std::isfinite(v.position.x));
            REQUIRE(std::isfinite(v.color.x));
        }
    }

    std::printf("[d3-prt] simulated %zu assets, %zu produced particles, %zu vertices\n",
                simulated, everEmitted, verts);
    REQUIRE(simulated > 0);
    // If NOTHING in a 400-asset sample ever emits, the emission path is broken
    // in a way every other assertion above would sail straight past.
    REQUIRE(everEmitted > simulated / 4);
}

// ---------------------------------------------------------------------------
// P5 — reachability: does a `.prt` reach a model at all?
// ---------------------------------------------------------------------------

TEST_CASE("d3 particle P5: appearances carry bone-attached particle systems",
          "[d3][particle][p5][corpus]") {
    // `BoneStructure::snoParticle` is the one route a `.prt` takes into a
    // Diablo III model, and the loader hook is built on it. If no shipped
    // appearance used it the hook would be dead code that still compiles, so
    // this counts rather than assumes.
    const fs::path root = CorpusRoot();
    fs::path dir = root / "Appearances";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        WARN("no .app corpus at " << dir.string() << " — SKIPPED.");
        return;
    }

    std::vector<fs::path> apps;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (it->is_regular_file(ec) && it->path().extension() == ".app")
            apps.push_back(it->path());
    }
    std::sort(apps.begin(), apps.end());
    if (apps.empty()) {
        WARN("no .app files under " << dir.string() << " — SKIPPED.");
        return;
    }

    // `.app` parsing is expensive (p95 8.3 MB), so this is deliberately a
    // bounded, deterministic sample and says so. WDX_TEST_D3_APP_LIMIT widens
    // it; 0 means the whole set.
    std::size_t bound = 300;
    if (const char* v = std::getenv("WDX_TEST_D3_APP_LIMIT"); v && *v)
        bound = static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
    const std::size_t n = (bound == 0) ? apps.size() : std::min(bound, apps.size());
    std::size_t parsed = 0, withParticle = 0, boneEmitters = 0, distinctPrt = 0;
    std::map<i32, std::size_t> prtIds;

    for (std::size_t i = 0; i < n; ++i) {
        auto app = d3n::parseAppearances(ReadAll(apps[i]));
        if (!app)
            continue;
        ++parsed;
        std::size_t here = 0;
        for (const auto& b : app->arBones) {
            if (b.snoParticle.valid()) {
                ++here;
                ++prtIds[b.snoParticle.id];
            }
        }
        boneEmitters += here;
        if (here)
            ++withParticle;
    }
    distinctPrt = prtIds.size();

    std::printf("[d3-prt] %zu of %zu appearances parsed (of %zu on disk); "
                "%zu carry bone particles, %zu attachments, %zu distinct .prt\n",
                parsed, n, apps.size(), withParticle, boneEmitters, distinctPrt);
    REQUIRE(parsed > 0);
}
