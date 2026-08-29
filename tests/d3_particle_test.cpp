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

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "io/d3/d3_particle_adapter.h"
#include "io/d3/d3_sno_cache.h"
#include "io/file_content_provider.h"
#include "renderer/particle/d3_channels.h"
#include "renderer/particle/d3_emit_mesh.h"
#include "renderer/particle/d3_emitter.h"
#include "renderer/particle/d3_emitter_desc.h"
#include "renderer/particle/d3_path.h"
#include "renderer/particle/particle_geometry.h"
#include "renderer/particle/particle_service.h"
#include "renderer/profiles/diablo3/d3_particle_shading.h"

#include <whiteout/sno/d3/native/d3_native.h>

#include <algorithm>
#include <array>
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
// P5 — the material: four stage binds and a UV transform each
// ---------------------------------------------------------------------------

TEST_CASE("d3 particle P5: a material is four stage types, never a layer list",
          "[d3][particle][p5][corpus]") {
    // `Particle_DrawBatch` binds types 1, 19, 12 and 14 in that order and the
    // `.prt` carries entries of a dozen other types beside them. The adapter's
    // job is that filter and that order — get it wrong and a particle draws
    // whichever entry happened to come first in the file.
    const fs::path root = CorpusRoot();
    const std::vector<fs::path> files = FindPrt(root);
    if (files.empty()) {
        WARN("no .prt corpus at " << root.string() << " — set WDX_TEST_D3_CORPUS. SKIPPED.");
        return;
    }
    const std::size_t limit = SweepLimit();
    const std::size_t n = (limit == 0) ? files.size() : std::min(limit, files.size());

    std::map<i32, std::size_t> byType;
    std::map<std::size_t, std::size_t> byCount;
    std::size_t withDiffuse = 0, scaled = 0, scrolling = 0, orderBreaks = 0, repeats = 0;
    std::size_t modes[7] = {};

    for (std::size_t i = 0; i < n; ++i) {
        auto prt = d3n::parseParticle(ReadAll(files[i]));
        REQUIRE(prt.has_value());
        auto desc = whiteout::flakes::io::d3::BuildD3EmitterDesc(*prt, -1);
        const auto& m = desc->d3mat;
        INFO("file " << files[i].filename().string());

        REQUIRE(m.layerCount <= pd3::MaterialDesc::kMaxLayers);
        ++byCount[m.layerCount];

        // The bind order is the filter: 1, 19, 12, 14, and a type appears at
        // most once — the property a `dest[type] = entry` LUT key must have.
        static constexpr i32 kOrder[4] = {1, 19, 12, 14};
        i32 seen = -1;
        std::map<i32, int> once;
        for (u32 L = 0; L < m.layerCount; ++L) {
            const auto& layer = m.layers[L];
            ++byType[layer.rawType];
            i32 rank = -1;
            for (i32 k = 0; k < 4; ++k)
                if (kOrder[k] == layer.rawType)
                    rank = k;
            if (rank <= seen)
                ++orderBreaks;
            seen = rank;
            if (++once[layer.rawType] > 1)
                ++repeats;
            if (layer.rawType == 1)
                ++withDiffuse;
            const int mode = static_cast<int>(layer.uv.mode);
            if (mode >= 0 && mode < 7)
                ++modes[mode];
            if (layer.uv.scale.x != 1.0f || layer.uv.scale.y != 1.0f)
                ++scaled;
            if (layer.uv.animated)
                ++scrolling;
            // Nothing is resolved yet: which texture id a layer takes is a
            // property of the actor it is about to ride, not of the file.
            CHECK(layer.textureId == -1);
        }
    }

    std::printf("[d3 mat] %zu files; layers per file:", n);
    for (const auto& [c, k] : byCount)
        std::printf(" %zu=%zu", c, k);
    std::printf("\n[d3 mat] stage types:");
    for (const auto& [t, k] : byType)
        std::printf(" %d=%zu", t, k);
    std::printf("\n[d3 mat] uv modes: identity=%zu matrix=%zu scaleRotScroll=%zu anim2D=%zu"
                " (scaled %zu, animated %zu)\n",
                modes[0], modes[1], modes[2], modes[3], scaled, scrolling);

    // Order and uniqueness are the whole contract; a single break means the
    // filter is reading the wrong field.
    CHECK(orderBreaks == 0);
    CHECK(repeats == 0);
    // Nothing outside the four is ever carried.
    for (const auto& [t, k] : byType) {
        INFO("stage type " << t << " x" << k);
        CHECK((t == 1 || t == 19 || t == 12 || t == 14));
    }
    // 18,473 of 21,593 shipped files carry a diffuse (85.5%); a sweep that
    // finds almost none is reading `arTextures` at the wrong offset, which is
    // exactly the failure that made this measurement wrong the first time.
    CHECK(withDiffuse * 10 > n * 7);
    // And the UV transforms are not decoration: 9,600 corpus entries scale
    // (3,372 by 0.5, 0.5 — a quarter tile) and 13,996 scroll.
    CHECK(scaled > n / 10);
    CHECK(scrolling > n / 10);
}

TEST_CASE("d3 particle P5: a scrolling layer's affine moves with the clock",
          "[d3][particle][p5]") {
    // The host evaluates each layer against the emitter's own age and uploads
    // six coefficients; a build that evaluated once at load would ship a still
    // frame of every flame in the game.
    whiteout::flakes::io::D3UvXform x;
    x.mode = whiteout::flakes::io::D3UvMode::ScaleRotateScroll;
    x.scale = {0.5f, 0.5f};
    x.scrollPerSec = {0.25f, 0.0f};
    x.animated = true;

    f32 a0[6], a1[6];
    whiteout::flakes::io::D3UvAffine(x, 0.0f, a0);
    whiteout::flakes::io::D3UvAffine(x, 2.0f, a1);

    // The scale is on the linear part and the scroll on the translation, so
    // the two are separable — which is what lets one CB row carry both.
    CHECK(a0[0] == Catch::Approx(0.5f));
    CHECK(a0[4] == Catch::Approx(0.5f));
    CHECK(a1[0] == Catch::Approx(a0[0]));
    CHECK(a1[2] - a0[2] == Catch::Approx(0.5f));
    CHECK(a1[5] == Catch::Approx(a0[5]));
}

TEST_CASE("D3 install: a particle's ShaderMap resolves to Billboard.fx",
          "[d3][particle][p5][install]") {
    // The half of the material a corpus tree structurally cannot see: an
    // extracted `.prt` names its ShaderMap by SNO id and there is no CoreTOC
    // beside it to find the `.shm` by. Skipped without an install, and skipped
    // is not passed — the printout says what it covered.
    using ::whiteout::flakes::ProductId;
    namespace pdia = whiteout::flakes::renderer::profiles::diablo3;

    const fs::path root = CorpusRoot();
    const std::vector<fs::path> files = FindPrt(root);
    if (files.empty()) {
        WARN("no .prt corpus at " << root.string() << " — SKIPPED.");
        return;
    }
    whiteout::flakes::io::FileContentProvider provider;
    if (const char* r = std::getenv("WDX_TEST_D3_INSTALL"); r && *r)
        provider.SetInstallPath(r);
    provider.SetGame(ProductId::D3);
    if (provider.GamePath(ProductId::D3).empty()) {
        WARN("no Diablo III install (set WDX_TEST_D3_INSTALL). SKIPPED, not passed.");
        return;
    }
    whiteout::flakes::io::D3SnoCache cache(&provider);

    std::size_t bound = 400;
    if (const char* v = std::getenv("WDX_TEST_D3_MAT_LIMIT"); v && *v)
        bound = static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
    const std::size_t n = (bound == 0) ? files.size() : std::min(bound, files.size());

    std::map<std::string, std::size_t> effects;
    std::map<std::pair<u32, u32>, std::size_t> blends;
    std::size_t resolved = 0, writesDepth = 0, gained = 0, alphaTested = 0;
    // Per stage type: how the pass's combine block routes it.
    std::map<i32, std::array<std::size_t, 4>> routing; // [both, colourOnly, alphaOnly, neither]

    for (std::size_t i = 0; i < n; ++i) {
        auto prt = d3n::parseParticle(ReadAll(files[i]));
        REQUIRE(prt.has_value());
        auto desc = whiteout::flakes::io::d3::BuildD3EmitterDesc(*prt, -1);
        pdia::D3ResolveParticleMaterial(*prt, &cache, desc->d3mat);
        const auto& m = desc->d3mat;
        if (!m.passResolved)
            continue;
        ++resolved;
        ++effects[m.effectFile];
        ++blends[{m.blendSrc, m.blendDst}];
        if (m.depthWrite)
            ++writesDepth;
        if (m.colorGain != 1.0f || m.alphaGain != 1.0f)
            ++gained;
        if (m.alphaTest > 0.0f)
            ++alphaTested;
        for (u32 L = 0; L < m.layerCount; ++L) {
            const auto& layer = m.layers[L];
            const std::size_t slot = layer.samplesColor ? (layer.samplesAlpha ? 0u : 1u)
                                                        : (layer.samplesAlpha ? 2u : 3u);
            ++routing[layer.rawType][slot];
        }
    }

    std::printf("[d3 mat] %zu of %zu resolved a pass; effect files:", resolved, n);
    for (const auto& [e, k] : effects)
        std::printf(" %s=%zu", e.c_str(), k);
    std::printf("\n[d3 mat] blend (src,dst):");
    for (const auto& [b, k] : blends)
        std::printf(" (%u,%u)=%zu", b.first, b.second, k);
    std::printf("\n[d3 mat] %zu write depth, %zu carry a combine gain, %zu alpha-test\n",
                writesDepth, gained, alphaTested);
    std::printf("[d3 mat] stage routing (type: both / colour / alpha / neither):");
    for (const auto& [t, r] : routing)
        std::printf(" %d:%zu/%zu/%zu/%zu", t, r[0], r[1], r[2], r[3]);
    std::printf("\n");

    if (resolved == 0) {
        WARN("no ShaderMap resolved through this install — SKIPPED, not passed.");
        return;
    }
    // Every shipped particle pass is a billboard family, and that is what makes
    // io/d3/d3_types.h's `Legacy.fx` stage-block decoder the authority for the
    // combine chain: 19 of the corpus's 20 `SoftBillboard.fx` shaders bind
    // `ps_legacy` too, and their stage list is the same one with type 39 — the
    // scene-colour copy, an engine render target no material owns — pushed in
    // front for the soft-particle depth fade. That fade is not reproduced; the
    // four binds behind it are identical. A THIRD family here would mean the
    // tag chain picked the wrong shader.
    CHECK((effects["Billboard.fx"] + effects["SoftBillboard.fx"]) * 100 > resolved * 99);
    // Depth write is the pass's own answer and it is almost always no: 6 of
    // 18,420 shipped particles ask for it. Defaulting it ON would punch a hole
    // in everything behind every effect in the game, which is why this is a
    // bound and not a constant.
    CHECK(writesDepth * 100 < resolved);
    // And the gains are not a rounding term: 7,134 of 18,420 carry one, up to
    // x4 on the colour and x32 on the alpha.
    CHECK(gained > resolved / 4);

    // The combine block routes each stage, and it says something: type 19 is
    // `alphaMap2Sampler` in the shipped programs and a real fraction of passes
    // bind it for the alpha alone. Reported as counts and asserted as a
    // presence, because the exact split is a property of the install.
    CHECK(routing[19][2] > 0);
    // The DIFFUSE, though, is never dropped from BOTH channels — which is why
    // the gate is on "does this stage sample the channel" and not on "does it
    // modulate it". The modulate test drops type 1's colour on 26 of the
    // corpus's 243 billboard passes, and a particle then draws as a plain
    // untextured quad.
    CHECK(routing[1][3] == 0);
}


// ---------------------------------------------------------------------------
// P6/P7 — the systems whose particles ARE models (eSystemType 1, 3, 4)
// ---------------------------------------------------------------------------

TEST_CASE("d3 particle P6: eSystemType 1 is a child-actor system, not a ribbon",
          "[d3][particle][p6][corpus]") {
    // `ParticleSystem_EmitParticle` opens on `(type - 3) < 2 || type == 1` and
    // that branch spawns an ACTOR; the 176-byte segment record the first RE
    // pass read as a ribbon belongs to type 9. The corpus is the independent
    // check: if the branch really is "these three types", then those three
    // types and nothing else should carry the `snoActor` the branch reads.
    const fs::path root = CorpusRoot();
    const std::vector<fs::path> files = FindPrt(root);
    if (files.empty()) {
        WARN("no .prt corpus at " << root.string() << " — SKIPPED.");
        return;
    }
    const std::size_t limit = SweepLimit();
    const std::size_t n = (limit == 0) ? files.size() : std::min(limit, files.size());

    std::map<i32, std::size_t> byType;
    std::map<i32, std::size_t> withActor;
    std::size_t spawners = 0, strays = 0;
    std::vector<std::string> strayNames;

    for (std::size_t i = 0; i < n; ++i) {
        auto prt = d3n::parseParticle(ReadAll(files[i]));
        REQUIRE(prt.has_value());
        auto desc = whiteout::flakes::io::d3::BuildD3EmitterDesc(*prt, -1);
        const bool actorType =
            desc->systemType == 1 || desc->systemType == 3 || desc->systemType == 4;
        ++byType[desc->systemType];
        if (desc->snoActor >= 0) {
            ++withActor[desc->systemType];
            if (!actorType) {
                ++strays;
                if (strayNames.size() < 4)
                    strayNames.push_back(files[i].filename().string());
            }
        }
        if (desc->SpawnsChildActors())
            ++spawners;
        // The branch is on the TYPE, so a type that takes it must have an actor
        // to spawn — otherwise the engine returns before it does anything.
        CHECK(desc->SpawnsChildActors() == (actorType && desc->snoActor >= 0));
    }

    std::printf("[d3 child] %zu files; snoActor by type:", n);
    for (const auto& [t, k] : withActor)
        std::printf(" %d=%zu/%zu", t, k, byType[t]);
    std::printf("\n[d3 child] %zu spawn actors; %zu of another type carry one", spawners, strays);
    for (const auto& sn : strayNames)
        std::printf(" (%s)", sn.c_str());
    std::printf("\n");

    // Every file of the three types has one — 4,795 of 4,795 over the whole
    // corpus. A single miss would mean the branch is not keyed on the type.
    for (const i32 t : {1, 3, 4})
        CHECK(withActor[t] == byType[t]);
    // And almost nothing else does: exactly one type-0 file
    // (`banner_treasureGoblin_glow.prt`) sets the field, where it is dead data
    // because the type-0 branch never reads it.
    CHECK(strays <= 1);
    // 4,790 of the corpus's 21,593 are type 1 alone — 22%, and the largest
    // single group after the plain billboard.
    if (n == files.size())
        CHECK(spawners > files.size() / 6);
}

TEST_CASE("d3 particle P7: a child-actor system emits models and pools nothing",
          "[d3][particle][p7]") {
    using whiteout::flakes::renderer::particle::ChildModelEvent;
    using whiteout::flakes::renderer::particle::ParticleOutput;

    // The shipped shape: type 1, one actor, a target count of 1 and no emission
    // rate at all — 4,189 of 4,795 files author exactly this, which is what
    // makes the count target mean "one model" rather than "one per frame".
    auto d = std::make_shared<pd3::EmitterDesc>();
    d->systemType = 1;
    d->snoActor = 4242;
    d->emissionPeriod = 1.0f;
    d->lifetime = 1.0f;
    d->channels[pd3::kChTargetCount] = ConstPath(1.0f);
    d->channels[pd3::kChParticleLife] = ConstPath(1.0f);
    d->channels[pd3::kChBirthSize] = ConstPath(2.0f);
    d->DeriveCapabilities();
    REQUIRE(d->SpawnsChildActors());

    pd3::Emitter em;
    em.SetD3Desc(d);
    em.SetVisible(true);
    // It declares the child-model space, so the geometry builder is never asked
    // for it — a system that spawns models draws nothing of its own.
    CHECK(em.Desc().output == ParticleOutput::ChildModel);

    u32 next = 100;
    em.SetChildOwner(7, 3, [&next] { return next++; });

    std::vector<ChildModelEvent> events;
    em.Update(1.0f / 60.0f, 1.0f);
    em.CollectOutputEvents(events);

    // One model, and NO particle: the engine's type-1 branch builds its record
    // on the stack and never touches the pool.
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == ChildModelEvent::Kind::Birth);
    CHECK(events[0].owner == 7u);
    CHECK(events[0].emitterId == 3);
    CHECK(events[0].childHandle == 100u);
    CHECK(em.ChildCount() == 1);
    CHECK(em.Pool().AliveCount() == 0);
    // The birth size is a SCALE on the spawned actor, which is why it reaches
    // the transform rather than a quad's half-extent.
    CHECK(events[0].transform.data[0][0] == Catch::Approx(2.0f));

    // The count target counts CHILDREN (`sys+408 + sys+376`), so a system that
    // has reached it stops. Without that the emitter spawns one model a frame
    // forever, which is the failure this test exists for.
    events.clear();
    for (int i = 0; i < 30; ++i)
        em.Update(1.0f / 60.0f, 1.0f);
    em.CollectOutputEvents(events);
    CHECK(events.empty());
    CHECK(em.ChildCount() == 1);

    // A re-trigger takes its children with it. The engine forgets them — a
    // spawned ACD outlives the system — but a looping viewer would then stack
    // one model per lap; see Emitter::Restart.
    em.Restart();
    events.clear();
    em.CollectOutputEvents(events);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == ChildModelEvent::Kind::Death);
    CHECK(events[0].childHandle == 100u);
    CHECK(em.ChildCount() == 0);

    events.clear();
    em.Update(1.0f / 60.0f, 1.0f);
    em.CollectOutputEvents(events);
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == ChildModelEvent::Kind::Birth);
    CHECK(events[0].childHandle == 101u);
}

TEST_CASE("d3 particle P7: with no handle allocator a child system is inert",
          "[d3][particle][p7]") {
    // The emitter reports births as data and the actor layer owns the spawn, so
    // an emitter nobody wired up must produce nothing rather than counting
    // emissions it never made — otherwise its target count fills with ghosts
    // and it goes quiet for good.
    auto d = std::make_shared<pd3::EmitterDesc>();
    d->systemType = 1;
    d->snoActor = 1;
    d->emissionPeriod = 1.0f;
    d->channels[pd3::kChTargetCount] = ConstPath(4.0f);
    d->DeriveCapabilities();

    pd3::Emitter em;
    em.SetD3Desc(d);
    em.SetVisible(true);
    for (int i = 0; i < 10; ++i)
        em.Update(1.0f / 60.0f, 1.0f);
    CHECK(em.ChildCount() == 0);
    CHECK(em.Pool().AliveCount() == 0);
}

// ---------------------------------------------------------------------------
// P5 — reachability: does a `.prt` reach a model at all?
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// The three defects a viewer surfaced after P7 landed
// ---------------------------------------------------------------------------

TEST_CASE("d3 particle: the colour dword is 0xAABBGGRR, red in the LOW byte",
          "[d3][particle][corpus]") {
    // Two shipped files name their own colour, which is as close to a labelled
    // sample as a reverse-engineered format gets. Read the other way round the
    // orange one comes out cyan and the blue one salmon, and every fire in the
    // game draws as its own complement.
    const fs::path root = CorpusRoot();
    const fs::path dir = fs::is_directory(root / "Particle") ? (root / "Particle") : root;
    struct Named {
        const char* file;
        bool warm; // is the RED channel the dominant one?
    };
    const Named kNamed[] = {
        {"a1dun_cave_TorchGlow_Orange.prt", true},
        {"a1dun_jail_Embers_blue.prt", false},
    };

    std::size_t checked = 0;
    for (const Named& n : kNamed) {
        const fs::path f = dir / n.file;
        std::error_code ec;
        if (!fs::is_regular_file(f, ec))
            continue;
        auto prt = d3n::parseParticle(ReadAll(f));
        REQUIRE(prt);
        auto d = whiteout::flakes::io::d3::BuildD3EmitterDesc(*prt, -1);
        REQUIRE(d->Has(pd3::kChColor));

        pd3::EvalCtx ctx;
        ctx.timeMode = 1;
        ctx.period = 1.0f;
        // Every stop of the path, not just the first: an ember fades, and one
        // sample could sit on a grey node that says nothing either way.
        bool sawDominant = false;
        for (i32 step = 0; step <= 8; ++step) {
            ctx.time = static_cast<f32>(step) / 8.0f;
            const Vector4f c = d->Channel(pd3::kChColor).Eval(1u, pd3::kChColor, ctx);
            if (std::fabs(c.x - c.z) < 0.02f)
                continue; // a grey stop carries no evidence
            sawDominant = true;
            CHECK((c.x > c.z) == n.warm);
        }
        CHECK(sawDominant);
        ++checked;
    }
    if (checked == 0)
        WARN("d3 colour order: neither named corpus file present; nothing checked");
    std::printf("[d3 colour] %zu of 2 named files checked\n", checked);
}

TEST_CASE("d3 particle: the unit scale converts sizes and paths, not positions",
          "[d3][particle]") {
    // The emitter POSITION arrives from a world matrix that already carries
    // WorldScale; every size, extent and speed inside the `.prt` is raw. Run
    // the same system at both scales and the geometry must differ by exactly
    // that factor about the emitter — no more, and not zero.
    const auto build = [] {
        auto d = std::make_shared<pd3::EmitterDesc>();
        d->systemType = 0;
        d->shape = pd3::Shape::SphereShell;
        d->shapeExtent0 = ConstPath(2.0f);
        d->emissionPeriod = 4.0f;
        d->lifetime = 4.0f;
        d->maxDistance = 100.0f; // the kill radius is authored too
        d->channels[pd3::kChEmissionRate] = ConstPath(1.0f);
        d->channels[pd3::kChParticleLife] = ConstPath(120.0f);
        d->channels[pd3::kChBirthSize] = ConstPath(3.0f);
        d->channels[pd3::kChRadialSpeed] = ConstPath(0.5f);
        d->DeriveCapabilities();
        return d;
    };

    const Vector3f kOrigin{10.0f, -20.0f, 30.0f};
    Matrix44f view = Matrix44f::identity();
    whiteout::flakes::renderer::particle::BuildGeometryInput in{};
    in.worldToView = &view;

    const auto run = [&](f32 unit, Vector3f& lo, Vector3f& hi, std::size_t& alive) {
        pd3::Emitter e;
        e.SetD3Desc(build());
        e.SetVisible(true);
        e.SetUnitScale(unit);
        e.SetWorldPosition(kOrigin);
        for (i32 i = 0; i < 60; ++i)
            e.Update(1.0f / 60.0f, 1.0f);
        alive = e.Pool().AliveCount();
        std::vector<whiteout::flakes::renderer::Vertex> out;
        REQUIRE(e.BuildGeometry(in, out) > 0);
        lo = hi = out[0].position;
        for (const auto& v : out) {
            lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y),
                  std::min(lo.z, v.position.z)};
            hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y),
                  std::max(hi.z, v.position.z)};
        }
    };

    Vector3f lo1, hi1, lo17, hi17;
    std::size_t alive1 = 0, alive17 = 0;
    run(1.0f, lo1, hi1, alive1);
    run(17.0f, lo17, hi17, alive17);

    // Same simulation, same random stream: the kill radius scales with the
    // positions it is compared against, so the population is identical. Leave
    // it unscaled and the system culls itself the moment the fix lands.
    CHECK(alive1 == alive17);
    CHECK(alive1 > 0);

    const f32 kUnit = 17.0f;
    const Vector3f d1{hi1.x - lo1.x, hi1.y - lo1.y, hi1.z - lo1.z};
    const Vector3f d17{hi17.x - lo17.x, hi17.y - lo17.y, hi17.z - lo17.z};
    REQUIRE(d1.x > 0.01f);
    CHECK(d17.x == Catch::Approx(d1.x * kUnit).epsilon(1e-4));
    CHECK(d17.y == Catch::Approx(d1.y * kUnit).epsilon(1e-4));
    CHECK(d17.z == Catch::Approx(d1.z * kUnit).epsilon(1e-4));

    // And it grows about the EMITTER, which does not move: the position came
    // off a matrix that already applied the scale, so scaling it again would
    // fling every effect away from the bone it rides.
    CHECK(((lo17.x + hi17.x) * 0.5f - kOrigin.x) ==
          Catch::Approx(((lo1.x + hi1.x) * 0.5f - kOrigin.x) * kUnit).margin(0.05f));
}

TEST_CASE("d3 particle: mesh shapes emit off the surface, not from the emitter",
          "[d3][particle]") {
    // Shapes 6, 7 and 11 are 1,454 shipped files and the fallback for them is
    // the POINT case — which puts a whole-body fire in one spot at the model
    // origin. A bound surface has to replace the emitter position outright.
    auto mesh = std::make_shared<pd3::EmitMesh>();
    // Two triangles, far from the emitter and from each other, the second six
    // times the area of the first.
    mesh->rest = {{100, 0, 0}, {101, 0, 0}, {100, 1, 0},
                  {-100, 0, 0}, {-100, 3, 0}, {-102, 0, 0}};
    mesh->tris = {0, 1, 2, 3, 4, 5};
    mesh->areaCdf = {0.5f, 0.5f + 3.0f};
    mesh->subs.push_back({0, 2});
    REQUIRE_FALSE(mesh->Empty());

    auto d = std::make_shared<pd3::EmitterDesc>();
    d->systemType = 0;
    d->shape = pd3::Shape::MeshRandom;
    d->emissionPeriod = 10.0f;
    d->lifetime = 10.0f;
    // No kill radius: a surface point is nowhere near the emitter by design,
    // and the default 10 would cull every one of them on the frame it was born.
    d->maxDistance = 0.0f;
    d->channels[pd3::kChEmissionRate] = ConstPath(4.0f);
    d->channels[pd3::kChParticleLife] = ConstPath(600.0f);
    d->channels[pd3::kChBirthSize] = ConstPath(1.0f);
    d->DeriveCapabilities();
    REQUIRE(d->SamplesModelSurface());

    pd3::Emitter e;
    e.SetD3Desc(d);
    e.SetVisible(true);
    e.SetWorldPosition({0, 0, 500});
    e.SetEmitMesh(mesh);
    REQUIRE(e.HasEmitMesh());
    // No pose and no inverse binds: an unskinned model samples its rest mesh,
    // which is 63% of the corpus.
    e.SetEmitMeshPose({}, {}, Matrix44f::identity());

    for (i32 i = 0; i < 120; ++i)
        e.Update(1.0f / 60.0f, 1.0f);
    REQUIRE(e.Pool().AliveCount() > 40);

    std::size_t onFirst = 0, onSecond = 0;
    for (std::size_t i = 0; i < e.Pool().AliveCount(); ++i) {
        const Vector3f p = e.Pool()[e.Pool().AliveAt(i)].position;
        // Nowhere near the emitter, and flat on the triangles own plane.
        CHECK(std::fabs(p.z) < 1e-4f);
        if (p.x > 0.0f) {
            ++onFirst;
            CHECK(p.x >= Catch::Approx(100.0f).margin(1e-4));
            CHECK(p.x <= Catch::Approx(101.0f).margin(1e-4));
        } else {
            ++onSecond;
            CHECK(p.x <= Catch::Approx(-100.0f).margin(1e-4));
        }
    }
    // Area-uniform, not triangle-uniform: the second triangle is 6x the first
    // and must take most of the births. Loose, because this is one seeded
    // stream and not a limit.
    CHECK(onSecond > onFirst * 2);

    // Shape 11 walks its triangles in order instead, so with two triangles it
    // strictly alternates — the property a random draw cannot have.
    auto seqDesc = std::make_shared<pd3::EmitterDesc>(*d);
    seqDesc->shape = pd3::Shape::MeshSequential;
    pd3::Emitter seq;
    seq.SetD3Desc(seqDesc);
    seq.SetVisible(true);
    seq.SetEmitMesh(mesh);
    seq.SetEmitMeshPose({}, {}, Matrix44f::identity());
    for (i32 i = 0; i < 8; ++i)
        seq.Update(1.0f / 60.0f, 1.0f);
    REQUIRE(seq.Pool().AliveCount() >= 4);
    std::vector<bool> side;
    for (std::size_t i = 0; i < seq.Pool().AliveCount(); ++i)
        side.push_back(seq.Pool()[seq.Pool().AliveAt(i)].position.x > 0.0f);
    for (std::size_t i = 1; i < side.size(); ++i)
        CHECK(side[i] != side[i - 1]);

    // And the model matrix is what takes the point to renderer units, so a
    // scaled model puts its fire on its own skin rather than inside it.
    pd3::Emitter scaled;
    scaled.SetD3Desc(d);
    scaled.SetVisible(true);
    scaled.SetEmitMesh(mesh);
    scaled.SetEmitMeshPose({}, {}, Matrix44f::scaling({17.0f, 17.0f, 17.0f}));
    for (i32 i = 0; i < 30; ++i)
        scaled.Update(1.0f / 60.0f, 1.0f);
    REQUIRE(scaled.Pool().AliveCount() > 0);
    for (std::size_t i = 0; i < scaled.Pool().AliveCount(); ++i) {
        const f32 x = std::fabs(scaled.Pool()[scaled.Pool().AliveAt(i)].position.x);
        CHECK(x > 17.0f * 99.0f);
    }
}

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
