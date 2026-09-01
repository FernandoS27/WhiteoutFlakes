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
#include "renderer/particle/d3_orientation.h"
#include "renderer/particle/d3_emitter_desc.h"
#include "renderer/particle/d3_path.h"
#include "renderer/particle/particle_geometry.h"
#include "renderer/particle/particle_service.h"
#include "renderer/profiles/diablo3/d3_particle_shading.h"
#include "renderer/profiles/diablo3/d3_surface_table.h"

#include <whiteout/sno/d3/native/d3_native.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
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
///
/// The count channel is set EXPLICITLY to zero. `ParticleSystem_TickEmitter`
/// defaults its target population to 1 when the count path is absent, so an
/// emitter with no count channel emits one particle on its first frame no matter
/// what its rate says — which is the engine's behaviour and is exactly what a
/// test of the accumulator must not have mixed in.
std::shared_ptr<pd3::EmitterDesc> MakeDesc(f32 ratePerFrame, f32 lifeFrames) {
    auto d = std::make_shared<pd3::EmitterDesc>();
    d->systemType = 0;
    d->lifetime = 0.0f; // never finishes emitting
    d->emissionPeriod = 10.0f;
    d->maxDistance = 0.0f; // no kill radius, so the count is the accumulator's
    d->channels[pd3::kChEmissionRate] = ConstPath(ratePerFrame);
    d->channels[pd3::kChTargetCount] = ConstPath(0.0f);
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

    // And the default the engine actually uses when no count path exists: a
    // target of ONE, so an emitter with no rate at all still shows a particle.
    auto d = MakeDesc(0.0f, 600.0f);
    d->channels[pd3::kChTargetCount] = pd3::Path{};
    d->DeriveCapabilities();
    pd3::Emitter bare;
    bare.SetD3Desc(d);
    bare.SetVisible(true);
    bare.SetWorldPosition({0, 0, 0});
    for (int i = 0; i < 30; ++i)
        bare.Update(1.0f / 60.0f, 1.0f);
    REQUIRE(static_cast<i32>(bare.Pool().AliveCount()) == 1);
}

TEST_CASE("d3 particle P3: tmPreSimulate makes a system already running on its first frame",
          "[d3][particle][p3]") {
    // `ParticleSystem_Spawn` @0x71000ADF84 runs TickEmitter(forceEmit) plus
    // UpdateAndCull for `tmPreSimulate / 60` seconds at a fixed 1/60 step
    // before the system is drawn once. 2,532 shipped files ask for it and they
    // are the ambient set, so ignoring it starts every torch fire empty.
    auto d = MakeDesc(1.0f, 600.0f); // one particle per frame, 10 s lifetimes
    d->preSimulate = 0.5f;           // 30 frames

    pd3::Emitter warm;
    warm.SetD3Desc(d);
    warm.SetVisible(true);
    warm.SetWorldPosition({0, 0, 0});
    warm.Update(1.0f / 60.0f, 1.0f);
    // 30 catch-up steps plus the real one.
    CHECK(static_cast<i32>(warm.Pool().AliveCount()) == 31);
    CHECK(warm.SystemAge() == Catch::Approx(31.0f / 60.0f).margin(1e-4f));

    // It happens ONCE. A second frame adds one particle, not another 30.
    warm.Update(1.0f / 60.0f, 1.0f);
    CHECK(static_cast<i32>(warm.Pool().AliveCount()) == 32);

    // And a system that does not ask for it is untouched.
    auto cold = MakeDesc(1.0f, 600.0f);
    pd3::Emitter e;
    e.SetD3Desc(cold);
    e.SetVisible(true);
    e.SetWorldPosition({0, 0, 0});
    e.Update(1.0f / 60.0f, 1.0f);
    CHECK(static_cast<i32>(e.Pool().AliveCount()) == 1);
}

TEST_CASE("d3 particle P3: tLifetimeRandom scales the SYSTEM lifetime, and only mode 10",
          "[d3][particle][p3]") {
    // `ParticleSystem_Spawn` @0x71000AC670 evaluates the InterpolationScalar at
    // Particle+32 and multiplies the result into the one stored lifetime, so a
    // driver moves the expiry and the emitter clock together. Mode 0 -- 21,492
    // of 21,593 files -- makes `InterpolationScalar_Evaluate` return "not
    // applied" and leaves 1.0.
    auto make = [](i32 mode, f32 lo, f32 hi) {
        auto d = MakeDesc(0.0f, 600.0f);
        d->lifetime = 1.0f;
        d->prtFlags = 0; // not persistent, so the expiry test runs
        d->lifetimeRandom = {mode, lo, hi};
        return d;
    };
    auto ageToFinish = [](const std::shared_ptr<pd3::EmitterDesc>& d) {
        pd3::Emitter e;
        e.SetD3Desc(d);
        e.SetVisible(true);
        e.SetWorldPosition({0, 0, 0});
        for (int i = 0; i < 240 && !e.EmissionFinished(); ++i)
            e.Update(1.0f / 60.0f, 1.0f);
        return e.SystemAge();
    };

    // Mode 0 ignores lo/hi entirely -- a range that would otherwise halve the
    // system must not touch it.
    CHECK(ageToFinish(make(0, 0.0f, 0.5f)) == Catch::Approx(1.0f).margin(0.02f));
    // Mode 10 applies. The draw is taken at its midpoint here, so a 0.5..1.5
    // system runs its authored length and a 1.0..1.5 one runs a quarter longer.
    CHECK(ageToFinish(make(10, 0.5f, 1.5f)) == Catch::Approx(1.0f).margin(0.02f));
    CHECK(ageToFinish(make(10, 1.0f, 1.5f)) == Catch::Approx(1.25f).margin(0.02f));
    CHECK(ageToFinish(make(10, 0.5f, 0.5f)) == Catch::Approx(0.5f).margin(0.02f));
    // The four other authored modes read actor and game state a viewer has
    // none of, and behave as mode 0 rather than guessing at a value.
    for (i32 mode : {1, 2, 4, 8})
        CHECK(ageToFinish(make(mode, 0.0f, 0.5f)) == Catch::Approx(1.0f).margin(0.02f));
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
    std::size_t esAbsent = 0, esOne = 0, esLive = 0, esShrink = 0, esGrow = 0, esHalf = 0;
    std::size_t hrOne = 0, hrLive = 0, hrSquat = 0, hrTall = 0;
    std::size_t a6One = 0, a6Const = 0, a6Live = 0;
    std::size_t withLayers = 0, slotOrderMatches = 0, slotOrderDiffers = 0;
    std::map<i32, std::size_t> distributions;
    std::map<i32, std::size_t> lifeDriver;
    std::map<i32, std::size_t> renderModes;
    std::size_t frameGated = 0;

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
        {
            f32 lo = 0.0f, hi = 0.0f;
            desc->Channel(pd3::kChEffectScale).ScalarRange(lo, hi);
            if (!desc->Has(pd3::kChEffectScale))
                ++esAbsent;
            else if (lo == 1.0f && hi == 1.0f)
                ++esOne;
            else {
                ++esLive;
                if (hi < 1.0f)
                    ++esShrink;
                if (hi > 1.0f)
                    ++esGrow;
                if (hi < 0.5f)
                    ++esHalf;
            }
        }
        {
            // Channel 2's blast radius: it scales the quad's HEIGHT alone, so a
            // file whose range leaves 1.0 draws a sprite the width does not
            // describe.
            f32 lo = 0.0f, hi = 0.0f;
            desc->Channel(pd3::kChHeightRatio).ScalarRange(lo, hi);
            if (!desc->Has(pd3::kChHeightRatio) || (lo == 1.0f && hi == 1.0f))
                ++hrOne;
            else {
                ++hrLive;
                if (lo < 1.0f)
                    ++hrSquat;
                if (hi > 1.0f)
                    ++hrTall;
            }
        }
        {
            f32 lo = 0.0f, hi = 0.0f;
            desc->Channel(pd3::kChAlpha).ScalarRange(lo, hi);
            if (!desc->Has(pd3::kChAlpha) || (lo == 1.0f && hi == 1.0f))
                ++a6One;
            else if (lo == hi)
                ++a6Const;
            else
                ++a6Live;
        }
        ++lifeDriver[desc->lifetimeRandom.mode];
        ++renderModes[desc->renderMode];
        // What gates `Particle_BuildOrientationBasis` into its second column
        // order is bit 13 of the system's RUNTIME word at sys+8, and
        // `ParticleSystem_Spawn` @0x71000AC504 sets that for eSystemType 1, 3
        // and 4 — the child-actor types. So the gated arm orients a spawned
        // MODEL and never a quad, which is why `BuildQuadFrame` carries only
        // the ungated arms.
        if (desc->systemType == 1 || desc->systemType == 3 || desc->systemType == 4)
            ++frameGated;

        // Slot order. `Particle_BindDrawTextures` @0x71000B7620 asks for the
        // four types in the fixed order 1, 19, 12, 14 and parks each in ITS OWN
        // slot, leaving gaps null (G-D3P-23). This build binds in the entry
        // array's order instead, so the two agree only when the `.prt` happens
        // to list them the same way -- and this is the census that says whether
        // "happens to" means every file or merely most.
        {
            static const i32 kEngineOrder[4] = {1, 19, 12, 14};
            u32 slot = 0;
            bool same = true;
            for (u32 L = 0; L < desc->d3mat.layerCount; ++L) {
                while (slot < 4 && kEngineOrder[slot] != desc->d3mat.layers[L].rawType)
                    ++slot;
                if (slot >= 4) {
                    same = false;
                    break;
                }
                if (slot != L)
                    same = false;
                ++slot;
            }
            if (desc->d3mat.layerCount > 0) {
                ++withLayers;
                if (same)
                    ++slotOrderMatches;
                else
                    ++slotOrderDiffers;
            }
        }

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
    std::printf("[d3-prt] arAlphaPath (ch6): one=%zu const=%zu animated=%zu\n",
                a6One, a6Const, a6Live);
    std::printf("[d3-prt] arSize2Path (quad height ratio): one=%zu live=%zu squat=%zu tall=%zu\n",
                hrOne, hrLive, hrSquat, hrTall);
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
    std::printf("[d3-prt] nRenderMode:");
    for (const auto& [v, c] : renderModes)
        std::printf(" %d=%zu", v, c);
    std::printf("; %zu take the gated column order\n", frameGated);
    std::printf("[d3-prt] tLifetimeRandom nMode:");
    for (const auto& [v, c] : lifeDriver)
        std::printf(" %d=%zu", v, c);
    std::printf("\n");
    std::printf("[d3-prt] %zu carry texture layers; %zu list them in the engine\'s "
                "slot order (1, 19, 12, 14) and %zu do not\n",
                withLayers, slotOrderMatches, slotOrderDiffers);

    // `tLifetimeRandom` SCALES the system lifetime once at spawn
    // (`ParticleSystem_Spawn` @0x71000AC670), and mode 0 means the evaluate
    // leaves 1.0 standing. Only mode 10 -- a plain uniform -- is answerable
    // without an actor, and it is the largest of the five that are authored, so
    // ignoring the field entirely was wrong for 47 files and defensible for 54.
    if (limit == 0) {
        CHECK(lifeDriver[0] == 21492);
        CHECK(lifeDriver[10] == 47);
        CHECK(lifeDriver[1] == 44);
        CHECK(lifeDriver[4] == 7);
        CHECK(lifeDriver[2] == 1);
        CHECK(lifeDriver[8] == 2);

        // The orientation frame, censused. 8,476 files -- 39.3% -- ask for a
        // frame that is not the caller's; modes 0 (ungated), 1 and 8 are the
        // 13,117 that write nothing. `BuildQuadFrame` answers all but 7, 9
        // and 10.
        CHECK(renderModes[0] == 9181);
        CHECK(renderModes[1] == 3917);
        CHECK(renderModes[2] == 2124);
        CHECK(renderModes[3] == 77);
        CHECK(renderModes[4] == 351);
        CHECK(renderModes[5] == 145);
        CHECK(renderModes[6] == 176);
        CHECK(renderModes[7] == 2312);
        CHECK(renderModes[8] == 19);
        CHECK(renderModes[9] == 24);
        CHECK(renderModes[10] == 398);
        CHECK(renderModes[11] == 115);
        CHECK(renderModes[12] == 1428);
        CHECK(renderModes[13] == 1326);
        CHECK(renderModes.size() == 14);
        // The gated column order belongs to the child-actor types alone, and
        // this is the same 4,795 the emit clamp counts.
        CHECK(frameGated == 4795);
        // `arEffectScalePath` is stored inline on every file like the other
        // thirty-nine, so "absent" means an all-default node and never a null.
        // It multiplies the particle's OPACITY and only ever attenuates: no
        // shipped file's range reaches above 1. That ceiling is the corpus-side
        // evidence for what it is -- a size term would have no reason to stop at
        // exactly 1.0 on all 14,012 of them.
        CHECK(esAbsent == 0);
        CHECK(esOne == 7581);
        CHECK(esLive == 14012);
        CHECK(esGrow == 0);
        CHECK(esShrink == 7660);
        CHECK(esHalf == 3435);

        // `arSize2Path` is the quad's HEIGHT ratio and it is authored on 55% of
        // the corpus, in both directions -- so leaving it unread stretched or
        // squashed 11,930 files, and reading it as a second size would have
        // moved their width too. Unlike `arEffectScalePath` it has no ceiling
        // at 1.0, which is the other half of why the two are not the same kind
        // of term.
        CHECK(hrOne == 9663);
        CHECK(hrLive == 11930);
        CHECK(hrSquat == 7279);
        CHECK(hrTall == 10446);

        // Channel 6 is the whole of COLOR1, and COLOR1's only reader is the
        // erosion tail. 265 files author it -- 1.2% -- but 253 of those ANIMATE
        // it, which is why it has to ride the vertex and cannot be a per-draw
        // constant. See the D3-install sweep for the other half: 163 of the
        // corpus's 168 dissolve systems are in this set.
        CHECK(a6One == 21328);
        CHECK(a6Const == 12);
        CHECK(a6Live == 253);
    }

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
            REQUIRE(std::isfinite(st.dissolve));
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
    std::size_t clamped = 0, added = 0, vcolDropped = 0, vcolLast = 0;
    // no ShaderMap / map missing / no valid entry / Shaders missing / no pass
    std::array<std::size_t, 5> unresolved{};
    std::map<i32, std::size_t> noMapByType, drawnByType;
    std::size_t noMapNoLayers = 0;
    // Per stage type: how the pass's combine block routes it.
    std::map<i32, std::array<std::size_t, 4>> routing; // [both, colourOnly, alphaOnly, neither]
    // Which pixel program each system actually runs, and how many of its four
    // textures the flow shaders among them read as distortion.
    std::map<std::string, std::size_t> programs;
    std::map<int, std::size_t> flowMaps;
    std::size_t blendAdd = 0, softFade = 0, uvReroute = 0, erosion = 0;
    // The intersection that says whether COLOR1 is live: only `ps_legacy`'s
    // erosion tail reads it, and only channel 6 fills it.
    std::size_t erosionLiveA6 = 0, liveA6 = 0;
    std::size_t phantom = 0, phantomGain = 0;
    std::size_t phantomDeclared = 0;
    std::map<i32, std::size_t> phantomType;
    std::map<std::pair<u8, u8>, std::size_t> phantomOp;
    // The UV half. `MatTex_BuildUvAffine2x3` reads the anim triples for uv mode
    // 2 ALONE, and takes the quad's base rectangle off stage 0's SHEET whatever
    // mode stage 0 is -- so what has to be counted is per (mode, stage), plus
    // how many sheets exist at all and how many a material carries.
    std::map<std::pair<i32, i32>, std::size_t> uvModeByType; // (rawType, mode)
    std::map<i32, std::size_t> sheetByType;   // rawType -> layers with a frame table
    std::map<i32, std::size_t> sheetFrames;   // frame count histogram
    std::map<std::size_t, std::size_t> sheetsPerMaterial;
    std::size_t stage0Sheet = 0, stage0SheetNotAnim2D = 0, atlasScaleSet = 0;
    std::size_t stage0Tiled = 0, stage0TiledNotAnim2D = 0, lateSheetTiled = 0;
    std::map<u32, std::size_t> leadSkip;
    std::size_t partialSingle = 0, notContiguous = 0, wantsMoreFrames = 0;
    std::size_t deadAnim = 0, liveFlip = 0;

    for (std::size_t i = 0; i < n; ++i) {
        auto prt = d3n::parseParticle(ReadAll(files[i]));
        REQUIRE(prt.has_value());
        auto desc = whiteout::flakes::io::d3::BuildD3EmitterDesc(*prt, -1);
        pdia::D3ResolveParticleMaterial(*prt, &cache, desc->d3mat);
        const auto& m = desc->d3mat;
        {
            std::size_t sheets = 0;
            for (u32 L = 0; L < m.layerCount; ++L) {
                const auto& lay = m.layers[L];
                ++uvModeByType[{lay.rawType, static_cast<i32>(lay.uv.mode)}];
                if (lay.atlas) {
                    ++sheets;
                    ++sheetByType[lay.rawType];
                    ++sheetFrames[static_cast<i32>(lay.atlas->frames.size())];
                    ++leadSkip[lay.atlas->leadSkip];
                    const auto& fr = lay.atlas->frames;
                    // A one-frame sheet that does NOT cover the whole texture,
                    // and a multi-frame one whose tiles do not butt up against
                    // each other, are both what a table read one record off
                    // would look like.
                    if (fr.size() == 1 &&
                        (fr[0].x != 0.0f || fr[0].y != 0.0f || fr[0].z < 0.999f ||
                         fr[0].w < 0.999f))
                        ++partialSingle;
                    for (std::size_t q = 1; q < fr.size(); ++q) {
                        const bool joins = std::fabs(fr[q].x - fr[q - 1].z) < 1e-4f ||
                                           std::fabs(fr[q].y - fr[q - 1].w) < 1e-4f;
                        if (!joins) {
                            ++notContiguous;
                            break;
                        }
                    }
                    // The entry asks for a start frame the sheet cannot supply.
                    if (lay.uv.mode == flakes::io::D3UvMode::Anim2D &&
                        lay.atlasFrameBase + lay.atlasFrameRange >=
                            static_cast<i32>(fr.size()))
                        ++wantsMoreFrames;
                    // A sheet with ONE frame covering the whole texture is
                    // what almost every `.tex` carries and is a no-op: the base
                    // rectangle it builds is the unit square. Only a sheet that
                    // really tiles moves anything, so count that apart.
                    const bool tiled = lay.atlas->frames.size() > 1 ||
                                       lay.atlas->TileSize().x < 0.999f ||
                                       lay.atlas->TileSize().y < 0.999f;
                    if (lay.rawType == 1) {
                        ++stage0Sheet;
                        if (lay.uv.mode != flakes::io::D3UvMode::Anim2D)
                            ++stage0SheetNotAnim2D;
                        if (tiled) {
                            ++stage0Tiled;
                            if (lay.uv.mode != flakes::io::D3UvMode::Anim2D)
                                ++stage0TiledNotAnim2D;
                        }
                    } else if (tiled && lay.uv.mode == flakes::io::D3UvMode::Anim2D) {
                        ++lateSheetTiled;
                    }
                }
                if (lay.uv.mode == flakes::io::D3UvMode::Anim2D) {
                    if (lay.uv.atlasScale)
                        ++atlasScaleSet;
                    if (lay.atlas && std::fabs(lay.atlasRate) >= 1e-6f)
                        ++liveFlip;
                }
                // Animation data on a mode the engine never reads it for. This
                // build applied it until 2026-08-31 and it is what slid a
                // mode-0 ALPHA MASK off its own sprite.
                if (lay.uv.mode != flakes::io::D3UvMode::ScaleRotateScroll &&
                    lay.uv.mode != flakes::io::D3UvMode::Matrix &&
                    (lay.uv.scrollPerSec.x != 0.0f || lay.uv.scrollPerSec.y != 0.0f ||
                     lay.uv.rotatePerSec != 0.0f || lay.uv.offset.x != 0.0f ||
                     lay.uv.offset.y != 0.0f || lay.uv.rotate != 0.0f))
                    ++deadAnim;
            }
            ++sheetsPerMaterial[sheets];
        }
        if (!m.passResolved) {
            // Why, exactly. "Unresolved" is not one failure: the material can
            // name no ShaderMap at all, name one the install does not hold,
            // hold a map with no valid Shaders entry, or reach a Shaders asset
            // with no render pass. Each leaves the layer on the constructed
            // default — blendEnable with (SRCALPHA, ONE) — and each is a
            // different bug, so they are counted apart.
            const auto& mat = prt->tMaterial;
            if (!mat.snoShaderMap.valid()) {
                ++unresolved[0];
                ++noMapByType[prt->eSystemType];
                if (m.layerCount == 0)
                    ++noMapNoLayers;
            }
            else if (!cache.ShaderMap(mat.snoShaderMap.id))
                ++unresolved[1];
            else {
                const auto map = cache.ShaderMap(mat.snoShaderMap.id);
                bool anyEntry = false;
                i32 firstShader = -1;
                for (const auto& e : map->arShaders)
                    if (e.snoShader.valid()) {
                        anyEntry = true;
                        firstShader = e.snoShader.id;
                        break;
                    }
                if (!anyEntry)
                    ++unresolved[2];
                else if (!cache.Shaders(firstShader))
                    ++unresolved[3];
                else
                    ++unresolved[4];
            }
            continue;
        }
        ++resolved;
        ++drawnByType[prt->eSystemType];
        ++effects[m.effectFile];
        ++blends[{m.blendSrc, m.blendDst}];
        if (m.depthWrite)
            ++writesDepth;
        if (m.alphaTest > 0.0f)
            ++alphaTested;
        bool anyGain = false;
        for (u32 L = 0; L < m.layerCount; ++L) {
            const auto& layer = m.layers[L];
            const bool cOn = layer.colorOp != flakes::io::kD3StageSkip;
            const bool aOn = layer.alphaOp != flakes::io::kD3StageSkip;
            ++routing[layer.rawType][cOn ? (aOn ? 0u : 1u) : (aOn ? 2u : 3u)];
            anyGain = anyGain || layer.colorGain != 1.0f || layer.alphaGain != 1.0f;
            if (layer.colorClamp || layer.alphaClamp)
                ++clamped;
            if (layer.colorOp == flakes::io::kD3StageAdd ||
                layer.alphaOp == flakes::io::kD3StageAdd)
                ++added;
        }
        if (anyGain)
            ++gained;
        if (!m.colorVcolFirst || !m.alphaVcolFirst)
            ++vcolDropped;
        if (m.colorVcolLast || m.alphaVcolLast)
            ++vcolLast;
        ++programs[m.pixelEntry];
        ++flowMaps[m.flowFirst < 0 ? 0 : m.flowLast - m.flowFirst + 1];
        if (m.program == flakes::io::D3ParticleProgram::BlendAdd)
            ++blendAdd;
        if (m.softFade)
            ++softFade;
        if (m.erosion)
            ++erosion;
        {
            f32 lo = 0.0f, hi = 0.0f;
            desc->Channel(pd3::kChAlpha).ScalarRange(lo, hi);
            const bool live = desc->Has(pd3::kChAlpha) && !(lo == 1.0f && hi == 1.0f);
            if (live)
                ++liveA6;
            if (live && m.erosion)
                ++erosionLiveA6;
        }
        // A content stage the PASS declares that the material binds no texture
        // for -- the reverse of the 3,242 case above. The original still runs
        // that chain step, against a unit nothing bound; this build skips it,
        // which drops the stage's gain and its clamp with it. Type 0 is not a
        // stage at all: the five BLENDADD passes pad their block with it (see
        // D3PassState::stageHole) and no layer can ever carry it.
        {
            const auto ps = pdia::D3PassStateFor(prt->tMaterial, &cache);
            bool any = false, gainy = false, declaredToo = false;
            for (u32 k = 0; k < ps.combineCount; ++k) {
                if (ps.combines[k].type == 0)
                    continue;
                bool bound = false;
                for (u32 li = 0; li < m.layerCount; ++li)
                    bound = bound || m.layers[li].rawType == ps.combines[k].type;
                if (bound)
                    continue;
                any = true;
                declaredToo = declaredToo ||
                              (ps.declaredTypes & flakes::io::D3TypeBit(ps.combines[k].type)) != 0;
                ++phantomType[ps.combines[k].type];
                ++phantomOp[{ps.combines[k].colorOp, ps.combines[k].alphaOp}];
                if (ps.combines[k].colorGain != 1.0f || ps.combines[k].alphaGain != 1.0f ||
                    ps.combines[k].colorClamp || ps.combines[k].alphaClamp)
                    gainy = true;
            }
            if (any)
                ++phantom;
            if (declaredToo)
                ++phantomDeclared;
            if (gainy) ++phantomGain;
        }
        for (u32 i = 0; i < m.layerCount; ++i) {
            // Its own positional set, against the one the pass routed it to.
            u32 own = 0;
            for (u32 k = 0; k < pd3::MaterialDesc::kMaxLayers; ++k)
                if (m.layers[i].rawType == flakes::io::kD3TexcoordSetType[k])
                    own = k;
            if (m.layers[i].uvSet != own) {
                ++uvReroute;
                break;
            }
        }
    }

    std::printf("[d3 mat] no-ShaderMap by eSystemType:");
    for (const auto& [t, k] : noMapByType)
        std::printf(" %d=%zu", t, k);
    std::printf(" | %zu of them declare no texture layer\n", noMapNoLayers);
    std::printf("[d3 mat] resolved by eSystemType:");
    for (const auto& [t, k] : drawnByType)
        std::printf(" %d=%zu", t, k);
    std::printf("\n");
    std::printf("[d3 mat] unresolved: %zu no ShaderMap, %zu map missing, %zu no valid entry, "
                "%zu Shaders missing, %zu no render pass\n",
                unresolved[0], unresolved[1], unresolved[2], unresolved[3], unresolved[4]);
    std::printf("[d3 mat] %zu of %zu resolved a pass; effect files:", resolved, n);
    for (const auto& [e, k] : effects)
        std::printf(" %s=%zu", e.c_str(), k);
    std::printf("\n[d3 mat] blend (src,dst):");
    for (const auto& [b, k] : blends)
        std::printf(" (%u,%u)=%zu", b.first, b.second, k);
    std::printf("\n[d3 mat] %zu write depth, %zu carry a combine gain, %zu alpha-test\n",
                writesDepth, gained, alphaTested);
    std::printf("[d3 mat] %zu layers clamp mid-chain, %zu add; %zu materials drop the "
                "vertex colour, %zu take it last\n",
                clamped, added, vcolDropped, vcolLast);
    std::printf("[d3 mat] pixel programs:");
    for (const auto& [pe, k] : programs)
        std::printf(" %s=%zu", pe.c_str(), k);
    std::printf("\n[d3 mat] flow maps per system:");
    for (const auto& [f, k] : flowMaps)
        std::printf(" %d=%zu", f, k);
    std::printf(" | %zu blend-add, %zu soft depth fade, %zu sample at another layer's uv, %zu dissolve\n",
                blendAdd, softFade, uvReroute, erosion);
    // COLOR1 is channel 6 and nothing but the erosion tail reads it, so this
    // pair is the whole of how live the second colour is.
    std::printf("[d3 mat] COLOR1: %zu systems animate channel 6, %zu of those run the erosion tail\n",
                liveA6, erosionLiveA6);
    if (bound == 0) {
        // The exponent the erosion tail raises the combined alpha to is
        // `10 * COLOR1.a`, and COLOR1.a is channel 6. Assuming a flat 10 was
        // wrong for 163 of the 168 systems that run the tail at all -- 97% --
        // which is why the value now rides the vertex.
        CHECK(erosion == 168);
        CHECK(liveA6 == 262);
        CHECK(erosionLiveA6 == 163);
    }
    std::printf("[d3 mat] uv modes by stage type:");
    for (const auto& um : uvModeByType)
        std::printf(" %d/m%d=%zu", um.first.first, um.first.second, um.second);
    std::printf("\n[d3 mat] sheets: %zu layers carry a frame table (by type:", stage0Sheet);
    for (const auto& sk : sheetByType)
        std::printf(" %d=%zu", sk.first, sk.second);
    std::printf("); %zu of the type-1 ones are NOT uv mode 3;"
                " %zu mode-3 entries set the tile SCALE; %zu flip-books actually advance\n",
                stage0SheetNotAnim2D, atlasScaleSet, liveFlip);
    // The population the base-rectangle rule actually moves. A one-frame sheet
    // covering the whole texture is the near-universal case and gives the unit
    // square, so only a sheet that really tiles reshapes a quad.
    std::printf("[d3 mat] type-1 sheets that really TILE: %zu, of which %zu are not uv mode 3"
                " (those are the quads the base rectangle reshapes); %zu tiling flip-books sit"
                " on a later stage and get no base rectangle\n",
                stage0Tiled, stage0TiledNotAnim2D, lateSheetTiled);
    std::printf("[d3 mat] sheets per material:");
    for (const auto& sm : sheetsPerMaterial)
        std::printf(" %zu=%zu", sm.first, sm.second);
    std::printf(" | frames per sheet:");
    for (const auto& sf : sheetFrames)
        std::printf(" %d=%zu", sf.first, sf.second);
    std::printf("\n[d3 mat] frame-table lead skip:");
    for (const auto& ls : leadSkip)
        std::printf(" %u=%zu", ls.first, ls.second);
    std::printf(" | %zu one-frame sheets do not cover their texture, %zu multi-frame sheets do"
                " not tile, %zu entries name a start frame past the end\n",
                partialSingle, notContiguous, wantsMoreFrames);
    std::printf("[d3 mat] %zu layers carry scroll/rotation on a uv mode that never reads it\n",
                deadAnim);
    std::printf("[d3 mat] phantom stages: %zu materials (%zu on a stage the pass also "
                "DECLARES), %zu carry a gain or clamp; by type:",
                phantom, phantomDeclared, phantomGain);
    for (const auto& tk : phantomType) std::printf(" %d=%zu", tk.first, tk.second);
    std::printf(" | (colourOp,alphaOp):");
    for (const auto& op : phantomOp)
        std::printf(" (%u,%u)=%zu", op.first.first, op.first.second, op.second);
    std::printf("\n");
    std::printf("[d3 mat] stage routing (type: both / colour / alpha / neither):");
    for (const auto& [t, r] : routing)
        std::printf(" %d:%zu/%zu/%zu/%zu", t, r[0], r[1], r[2], r[3]);
    std::printf("\n");

    if (resolved == 0) {
        WARN("no ShaderMap resolved through this install — SKIPPED, not passed.");
        return;
    }
    // The chain never fails part-way. Over the whole corpus every unresolved
    // material names no ShaderMap at all: no missing `.shm`, no empty entry
    // list, no missing `.shd`, no passless `Shaders`. That is what makes an
    // unresolved pass a property of the DATA rather than of this install, and
    // it is what rules the lookup chain out as the cause of the untextured
    // white squares — see D3_PARTICLE_AUDIT.md section 10.3.
    CHECK(unresolved[1] == 0);
    CHECK(unresolved[2] == 0);
    CHECK(unresolved[3] == 0);
    CHECK(unresolved[4] == 0);
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
    // And the systems that name none are not arbitrary: they are overwhelmingly
    // the child-actor spawners, which draw no billboard and need no material.
    // Asserted as a majority rather than a count, because the split is a
    // property of the install.
    if (unresolved[0] > 0)
        CHECK(noMapByType[1] * 2 > unresolved[0]);

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
    // the transform rather than a quad's half-extent. Read as a column length,
    // because the rotation half is no longer identity: this desc leaves
    // `renderMode` at 0, whose gated arm turns the child to face the camera.
    {
        const auto& m = events[0].transform;
        const f32 sx = std::sqrt(m.data[0][0] * m.data[0][0] + m.data[0][1] * m.data[0][1] +
                                 m.data[0][2] * m.data[0][2]);
        CHECK(sx == Catch::Approx(2.0f));
    }

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

// ---------------------------------------------------------------------------
// The three the viewer surfaced after §18: the timing model, the sampler
// address modes and the flip-book
// ---------------------------------------------------------------------------

TEST_CASE("d3 particle: dwPrtFlags bit 0 makes the system persistent",
          "[d3][particle]") {
    // `tmLifetime` is 60 frames on 6,884 shipped files. Read as an expiry for
    // every system it stops a third of the game's effects after exactly one
    // second; the engine only runs that test when bit 0 is CLEAR, and when it
    // is set the same quotient wraps instead.
    const auto build = [](u32 flags) {
        auto d = std::make_shared<pd3::EmitterDesc>();
        d->systemType = 0;
        d->prtFlags = flags;
        d->shape = pd3::Shape::Point;
        d->lifetime = 1.0f;        // 60 frames, the modal value
        d->emissionPeriod = 0.5f;  // the wind-down, which must not be the period
        d->maxDistance = 0.0f;
        d->channels[pd3::kChEmissionRate] = ConstPath(1.0f);
        d->channels[pd3::kChParticleLife] = ConstPath(30.0f);
        d->channels[pd3::kChBirthSize] = ConstPath(1.0f);
        d->DeriveCapabilities();
        return d;
    };

    const auto run = [](const std::shared_ptr<pd3::EmitterDesc>& d, i32 frames) {
        pd3::Emitter e;
        e.SetD3Desc(d);
        e.SetVisible(true);
        i32 emitted = 0;
        for (i32 i = 0; i < frames; ++i) {
            e.Update(1.0f / 60.0f, 1.0f);
            if (i >= 180) // after three lifetimes
                emitted += e.EmittedLastUpdate();
        }
        return emitted;
    };

    CHECK(run(build(0u), 300) == 0);      // one-shot: silent after its second
    CHECK(run(build(0x1u), 300) > 0);     // persistent: still going at five
}

TEST_CASE("d3 particle: the emitter clock's period is tmLifetime", "[d3][particle]") {
    // Not tmEmissionPeriod. The engine divides the elapsed time by the value it
    // took from SNO+20 in both branches of its timing switch, and normalising
    // against SNO+24 instead runs every emitter channel at 1/2 to 1/6 of its
    // authored length.
    auto d = std::make_shared<pd3::EmitterDesc>();
    d->systemType = 0;
    d->shape = pd3::Shape::Point;
    d->lifetime = 2.0f;
    d->emissionPeriod = 0.5f;
    d->maxDistance = 0.0f;
    // A ramp from 0 to 60 over the path's normalised time, so the emitted
    // count reads the clock back out.
    pd3::Path ramp;
    ramp.components = 1;
    ramp.nodes.push_back({{0, 0, 0, 0}, {0, 0, 0, 0}, 0.0f});
    ramp.nodes.push_back({{1, 0, 0, 0}, {1, 0, 0, 0}, 1.0f});
    d->channels[pd3::kChEmissionRate] = ramp;
    d->channels[pd3::kChParticleLife] = ConstPath(600.0f);
    d->channels[pd3::kChBirthSize] = ConstPath(1.0f);
    d->DeriveCapabilities();

    pd3::Emitter e;
    e.SetD3Desc(d);
    e.SetVisible(true);
    // Half of `lifetime`, so the ramp is at 0.5 and the rate is 30/s.
    for (i32 i = 0; i < 60; ++i)
        e.Update(1.0f / 60.0f, 1.0f);
    const std::size_t atHalf = e.Pool().AliveCount();
    // Against `emissionPeriod` the ramp would have saturated long ago and the
    // population would be the full-rate one; against `lifetime` it is half.
    CHECK(atHalf > 5);
    CHECK(atHalf < 25);
}

TEST_CASE("d3 particle: dwPrtFlags bit 10 picks the particle time mode",
          "[d3][particle]") {
    // Mode 0 plays a channel's curve once across the particle's life; mode 1
    // wraps it into the path's own loop sub-range. 18,415 of 21,593 files set
    // the bit, so mode 0 is the rule and mode 1 the exception — and a colour or
    // alpha channel with a short loop range is visibly different under each.
    pd3::Path ramp;
    ramp.components = 1;
    ramp.nodes.push_back({{0, 0, 0, 0}, {0, 0, 0, 0}, 0.0f});
    ramp.nodes.push_back({{1, 0, 0, 0}, {1, 0, 0, 0}, 1.0f});
    ramp.loopStart = 0.0f;
    ramp.loopEnd = 0.5f; // the curve is done at half life

    // The OLDEST live particle, and its own normalised age — the population is
    // continuous, so "the first alive one" is an arbitrary age and the answer
    // has to be read against the particle actually sampled.
    const auto oldest = [&](u32 flags, f32& outT) {
        auto d = std::make_shared<pd3::EmitterDesc>();
        d->systemType = 0;
        d->prtFlags = flags;
        d->shape = pd3::Shape::Point;
        d->lifetime = 0.0f;
        d->maxDistance = 0.0f;
        d->channels[pd3::kChEmissionRate] = ConstPath(1.0f);
        d->channels[pd3::kChParticleLife] = ConstPath(120.0f);
        d->channels[pd3::kChBirthSize] = ConstPath(1.0f);
        d->channels[pd3::kChAlpha] = ramp;
        d->DeriveCapabilities();

        pd3::Emitter e;
        e.SetD3Desc(d);
        e.SetVisible(true);
        for (i32 i = 0; i < 90; ++i)
            e.Update(1.0f / 60.0f, 1.0f);
        REQUIRE(e.Pool().AliveCount() > 0);
        u32 best = e.Pool().AliveAt(0);
        for (std::size_t i = 1; i < e.Pool().AliveCount(); ++i) {
            const u32 idx = e.Pool().AliveAt(i);
            if (e.Pool()[idx].age > e.Pool()[best].age)
                best = idx;
        }
        outT = e.Pool()[best].age / e.States()[best].lifetime;
        // Channel 6, which lives on `dissolve` rather than the colour's alpha.
        return e.States()[best].dissolve;
    };

    f32 t0 = 0.0f, t1 = 0.0f;
    const f32 unwrapped = oldest(0x400u, t0);
    const f32 wrapped = oldest(0u, t1);
    // Same seed stream, so the same particle: the two runs differ only in how
    // its age is turned into a curve position.
    REQUIRE(t0 == Catch::Approx(t1));
    REQUIRE(t0 > 0.5f); // past `loopEnd`, which is where the two disagree

    // Mode 0 reads the curve at the raw quotient. Mode 1 folds it back into
    // [loopStart, loopEnd] first, so it is exactly one loop-span behind.
    CHECK(unwrapped == Catch::Approx(t0).margin(0.02));
    CHECK(wrapped == Catch::Approx(t0 - 0.5f).margin(0.02));
}

TEST_CASE("d3 particle: a flip-book layer walks its sheet per particle",
          "[d3][particle]") {
    // A four-tile sheet, the shape 512x128 ships. The atlas replaces the
    // layer's UV rectangle, and which tile is a property of the PARTICLE — the
    // engine seeds one player per particle per stage precisely so a system's
    // particles do not all show the same frame.
    auto atlas = std::make_shared<whiteout::flakes::io::D3TextureAtlas>();
    atlas->width = 512;
    atlas->height = 128;
    for (i32 k = 0; k < 4; ++k) {
        const f32 u0 = static_cast<f32>(k) * 0.25f;
        atlas->frames.push_back({u0, 0.0f, u0 + 0.25f, 1.0f});
    }
    CHECK(atlas->TileSize().x == Catch::Approx(0.25f));
    CHECK(atlas->TileSize().y == Catch::Approx(1.0f));

    auto d = std::make_shared<pd3::EmitterDesc>();
    d->systemType = 0;
    d->prtFlags = 0x1u; // persistent, so the population survives the run
    d->shape = pd3::Shape::Point;
    d->lifetime = 0.0f;
    d->maxDistance = 0.0f;
    d->channels[pd3::kChEmissionRate] = ConstPath(1.0f);
    d->channels[pd3::kChParticleLife] = ConstPath(600.0f);
    d->channels[pd3::kChBirthSize] = ConstPath(1.0f);
    d->DeriveCapabilities();
    d->d3mat.layerCount = 1;
    d->d3mat.setLayer[0] = 0;
    d->d3mat.layers[0].rawType = 1;
    d->d3mat.layers[0].uv.mode = whiteout::flakes::io::D3UvMode::Anim2D;
    d->d3mat.layers[0].atlas = atlas;
    d->d3mat.layers[0].atlasFrameRange = 3; // any of the four
    d->d3mat.layers[0].atlasRate = 0.0f;    // a still frame per particle

    pd3::Emitter e;
    e.SetD3Desc(d);
    e.SetVisible(true);
    for (i32 i = 0; i < 240; ++i)
        e.Update(1.0f / 60.0f, 1.0f);
    REQUIRE(e.Pool().AliveCount() > 20);

    // Every particle sits on a tile boundary, and the population is spread over
    // more than one tile — the whole point of the per-particle draw.
    std::set<i32> tiles;
    for (std::size_t i = 0; i < e.Pool().AliveCount(); ++i) {
        const f32 c = e.States()[e.Pool().AliveAt(i)].uv[0].cursor;
        const i32 k = static_cast<i32>(c);
        CHECK(k >= 0);
        CHECK(k <= 3);
        tiles.insert(k);
    }
    CHECK(tiles.size() >= 3);

    // And the rectangle reaches the geometry BAKED: the quad's own uv stays
    // 0..1 while set 0's coordinate spans one tile, `[k*0.25, k*0.25+0.25]`.
    // A non-square tile also makes a non-square quad — 0.25 of a 512-wide sheet
    // is 128px against a 128px-tall one, so this sheet is square and the aspect
    // is 1.
    Matrix44f view = Matrix44f::identity();
    whiteout::flakes::renderer::particle::BuildGeometryInput in{};
    in.worldToView = &view;
    std::vector<Vector4f> uv01, uv23;
    in.d3Uv01 = &uv01;
    in.d3Uv23 = &uv23;
    std::vector<whiteout::flakes::renderer::Vertex> out;
    REQUIRE(e.BuildGeometry(in, out) > 0);
    REQUIRE(uv01.size() == out.size());
    std::set<i32> originTiles;
    for (std::size_t i = 0; i < out.size(); ++i) {
        CHECK(out[i].uv.x >= 0.0f);
        CHECK(out[i].uv.x <= 1.0f);
        // The u of a corner is `origin + 0.25 * quadU`, so both corners of a
        // tile land on a multiple of 0.25 and the tile is which one.
        const f32 u = uv01[i].x;
        const i32 k = static_cast<i32>(std::lround(u * 4.0f));
        CHECK(u == Catch::Approx(static_cast<f32>(k) * 0.25f).margin(1e-5));
        CHECK(uv01[i].y == Catch::Approx(out[i].uv.y).margin(1e-5));
        originTiles.insert(k - static_cast<i32>(std::lround(out[i].uv.x)));
    }
    CHECK(originTiles.size() >= 3);

    // A rate walks the same particle forward, and a loop wraps it rather than
    // running off the end.
    auto moving = std::make_shared<pd3::EmitterDesc>(*d);
    moving->d3mat.setLayer[0] = 0;
    moving->d3mat.layers[0].atlasRate = 8.0f;
    moving->d3mat.layers[0].atlasFrameRange = 0; // everyone starts on tile 0
    moving->d3mat.layers[0].atlasLoops = true;
    pd3::Emitter m;
    m.SetD3Desc(moving);
    m.SetVisible(true);
    m.Update(1.0f / 60.0f, 1.0f);
    REQUIRE(m.Pool().AliveCount() > 0);
    const u32 first = m.Pool().AliveAt(0);
    const f32 t0 = m.States()[first].uv[0].cursor;
    for (i32 i = 0; i < 30; ++i)
        m.Update(1.0f / 60.0f, 1.0f);
    const f32 t1 = m.States()[first].uv[0].cursor;
    CHECK(t1 > t0);
    CHECK(t1 < 4.0f);
    // Long enough to have wrapped several times, and still in range.
    for (i32 i = 0; i < 600; ++i)
        m.Update(1.0f / 60.0f, 1.0f);
    CHECK(m.States()[first].uv[0].cursor >= 0.0f);
    CHECK(m.States()[first].uv[0].cursor < 4.0f);
}

TEST_CASE("d3 particle: each texture stage runs its OWN uv state",
          "[d3][particle]") {
    // `MatTex_InitUvState` runs once per STAGE from ParticleSystem_EmitParticle
    // and `MatTex_TickUvStateEntry` once per stage per frame, so two stages of
    // one material walk independently — 505 shipped materials carry two mode-3
    // stages and 5 carry three, and `Mace_norm_unique_05_sparkles` runs its two
    // 16-tile sheets at 24 and 26 fps off the same texture.
    //
    // And a mode-2 stage's phase belongs to the particle: 12,361 of the corpus's
    // 13,897 mode-2 entries draw it at random, which is the difference between a
    // puff of different tiles and one flat rectangle.
    auto sheet = std::make_shared<whiteout::flakes::io::D3TextureAtlas>();
    sheet->width = 512;
    sheet->height = 128;
    for (i32 k = 0; k < 4; ++k)
        sheet->frames.push_back({static_cast<f32>(k) * 0.25f, 0.0f,
                                 static_cast<f32>(k + 1) * 0.25f, 1.0f});

    auto d = std::make_shared<pd3::EmitterDesc>();
    d->systemType = 0;
    d->prtFlags = 0x1u;
    d->shape = pd3::Shape::Point;
    d->lifetime = 0.0f;
    d->maxDistance = 0.0f;
    d->channels[pd3::kChEmissionRate] = ConstPath(1.0f);
    d->channels[pd3::kChParticleLife] = ConstPath(600.0f);
    d->channels[pd3::kChBirthSize] = ConstPath(1.0f);
    d->DeriveCapabilities();

    // Set 0 is a flip-book at 6 fps; set 1 is a second one at 30. Both start on
    // frame 0 so only the RATES can separate them.
    d->d3mat.layerCount = 2;
    d->d3mat.setLayer[0] = 0;
    d->d3mat.setLayer[1] = 1;
    for (u32 i = 0; i < 2; ++i) {
        auto& L = d->d3mat.layers[i];
        L.rawType = (i == 0) ? 1 : 19;
        L.uv.mode = whiteout::flakes::io::D3UvMode::Anim2D;
        L.atlas = sheet;
        L.atlasFrameRange = 0;
        L.atlasRate = (i == 0) ? 6.0f : 30.0f;
    }

    pd3::Emitter e;
    e.SetD3Desc(d);
    e.SetVisible(true);
    for (i32 i = 0; i < 12; ++i)
        e.Update(1.0f / 60.0f, 1.0f);
    REQUIRE(e.Pool().AliveCount() > 0);
    {
        const auto& st = e.States()[e.Pool().AliveAt(0)];
        // The second stage has walked five times as far. A build with one state
        // per particle reports the same number twice.
        CHECK(st.uv[1].cursor > st.uv[0].cursor + 0.5f);
    }

    // A mode-2 stage with the V phase randomised: the population must SPREAD.
    // Sharing one emitter-clock phase puts every particle on one coordinate.
    auto r = std::make_shared<pd3::EmitterDesc>(*d);
    r->d3mat.layers[1].atlas = nullptr;
    r->d3mat.layers[1].uv.mode = whiteout::flakes::io::D3UvMode::ScaleRotateScroll;
    r->d3mat.layers[1].uv.scale = {1.0f, 0.25f};
    r->d3mat.layers[1].uv.randomPhaseV = true;
    pd3::Emitter q;
    q.SetD3Desc(r);
    q.SetVisible(true);
    for (i32 i = 0; i < 240; ++i)
        q.Update(1.0f / 60.0f, 1.0f);
    REQUIRE(q.Pool().AliveCount() > 20);
    std::set<i32> bands;
    for (std::size_t i = 0; i < q.Pool().AliveCount(); ++i) {
        const f32 v = q.States()[q.Pool().AliveAt(i)].uv[1].v;
        CHECK(v >= 0.0f);
        CHECK(v <= 1.0f);
        bands.insert(static_cast<i32>(v * 8.0f));
    }
    CHECK(bands.size() >= 4);
}

TEST_CASE("d3 particle: the emitter effect scale multiplies the particle opacity",
          "[d3][particle]") {
    // `arEffectScalePath` (channel 35). `ParticleSystem_TickEmitter` @0x71000AEF64
    // samples it once per tick into sys+0x12C off the EMITTER's seed, and every
    // branch of channel 5 in `ParticleSystem_UpdateParticles` multiplies by it:
    // the evaluated path @0x71000BEE28, the cached constant @0x71000BEDE4, and
    // the absent-path 1.0 that falls through the same instruction. So a system
    // with no channel 5 at all still attenuates.
    //
    // The product is an OPACITY, not a size. `Particle_PrepareDrawFrame`
    // @0x71000BCC48 quantises particle+0xD0 to a byte and skips the quad
    // entirely when it comes out zero; the quad's extent is particle+0xD4, which
    // neither channel touches. Both shipped shapes agree: channel 5 ramps
    // 0 -> 1 -> 0 across a particle's life, and channel 35's range never exceeds
    // 1.0 on any of the 21,593 files.
    auto make = [](f32 effectScale, bool withScaleChannel) {
        auto d = std::make_shared<pd3::EmitterDesc>();
        d->systemType = 0;
        d->prtFlags = 0x1u;
        d->shape = pd3::Shape::Point;
        d->lifetime = 0.0f;
        d->maxDistance = 0.0f;
        d->channels[pd3::kChEmissionRate] = ConstPath(1.0f);
        d->channels[pd3::kChParticleLife] = ConstPath(600.0f);
        d->channels[pd3::kChBirthSize] = ConstPath(2.0f);
        if (withScaleChannel)
            d->channels[pd3::kChScale] = ConstPath(3.0f);
        if (effectScale != 1.0f)
            d->channels[pd3::kChEffectScale] = ConstPath(effectScale);
        d->DeriveCapabilities();
        pd3::Emitter e;
        e.SetD3Desc(d);
        e.SetVisible(true);
        for (i32 i = 0; i < 4; ++i)
            e.Update(1.0f / 60.0f, 1.0f);
        REQUIRE(e.Pool().AliveCount() > 0);
        const auto& st = e.States()[e.Pool().AliveAt(0)];
        return std::pair<f32, f32>{st.size, st.opacity};
    };

    const auto [size1, op1] = make(1.0f, true);
    const auto [sizeH, opH] = make(0.4f, true);
    CHECK(op1 == Catch::Approx(3.0f));
    CHECK(opH == Catch::Approx(1.2f));
    // The SIZE is the other emitter term's (channel 34) and must not move with
    // the opacity -- the defect this pair of checks exists to catch.
    CHECK(sizeH == Catch::Approx(size1));

    // No channel 5: the engine multiplies its 1.0 anyway.
    const auto [sizeN, opN] = make(0.25f, false);
    CHECK(opN == Catch::Approx(0.25f));
    CHECK(sizeN == Catch::Approx(size1));
}

TEST_CASE("d3 particle: channel 6 rides COLOR1, not the vertex alpha", "[d3][particle]") {
    // `arAlphaPath` never touches COLOR0. `ParticleSystem_UpdateParticles`
    // quantises it and replicates the byte into all four lanes of the SECOND
    // dword (`MOV W9,#0x1010101` @0x71000BEF20 -> particle+0xEC), which
    // `Particle_PrepareDrawFrame` @0x71000BCDC0 hands on as drawDesc[5];
    // COLOR0's own alpha byte is overwritten by the `ch5 * ch35` opacity
    // @0x71000BCCAC. `Billboard.fx__ps_legacy`'s erosion tail is the only
    // consumer, `alpha = min(1, pow(alpha, 10 * COLOR1.a))`.
    auto build = [](f32 ch6, f32 ch5) {
        auto d = std::make_shared<pd3::EmitterDesc>();
        d->systemType = 0;
        d->prtFlags = 0x1u;
        d->shape = pd3::Shape::Point;
        d->lifetime = 0.0f;
        d->maxDistance = 0.0f;
        d->channels[pd3::kChEmissionRate] = ConstPath(1.0f);
        d->channels[pd3::kChParticleLife] = ConstPath(600.0f);
        d->channels[pd3::kChBirthSize] = ConstPath(2.0f);
        d->channels[pd3::kChAlpha] = ConstPath(ch6);
        d->channels[pd3::kChScale] = ConstPath(ch5);
        d->DeriveCapabilities();
        pd3::Emitter e;
        e.SetD3Desc(d);
        e.SetVisible(true);
        e.Update(1.0f / 60.0f, 1.0f);
        REQUIRE(e.Pool().AliveCount() >= 1);
        Matrix44f view = Matrix44f::identity();
        whiteout::flakes::renderer::particle::BuildGeometryInput in{};
        in.worldToView = &view;
        std::vector<Vector4f> uv01, uv23;
        std::vector<f32> color1;
        in.d3Uv01 = &uv01;
        in.d3Uv23 = &uv23;
        in.d3Color1 = &color1;
        std::vector<whiteout::flakes::renderer::Vertex> out;
        REQUIRE(e.BuildGeometry(in, out) >= 6);
        // Every side array stays the same length as the shared stream, which is
        // what lets a draw's vertexOffset index all three.
        REQUIRE(color1.size() == out.size());
        REQUIRE(uv01.size() == out.size());
        return std::pair<f32, f32>{out[0].color.w, color1[0]};
    };

    // ch6 = 0.5 must NOT dim the vertex alpha; the alpha is the opacity byte,
    // and ch5 = 0.5 quantises to ceil(0.5 * 255) / 255.
    const auto [alphaHalf6, c1Half6] = build(0.5f, 1.0f);
    CHECK(alphaHalf6 == Catch::Approx(1.0f));
    CHECK(c1Half6 == Catch::Approx(0.5f));

    const auto [alphaHalf5, c1Half5] = build(1.0f, 0.5f);
    CHECK(alphaHalf5 == Catch::Approx(128.0f / 255.0f));
    CHECK(c1Half5 == Catch::Approx(1.0f));

    // And they do not mix: the pair that would look identical if ch6 were still
    // folded into the alpha.
    const auto [alphaBoth, c1Both] = build(0.5f, 0.5f);
    CHECK(alphaBoth == Catch::Approx(alphaHalf5));
    CHECK(c1Both == Catch::Approx(0.5f));
}

TEST_CASE("d3 particle: channel 2 scales the quad's height and not its width",
          "[d3][particle]") {
    // `arSize2Path` -> `particle+0xF0` (default 1.0 @0x71000BEF98), forwarded as
    // drawDesc[2] @0x71000BCD68. `Particle_WriteQuadVertices` halves the WIDTH
    // out of drawDesc[1] alone and builds the vertical half-extent as
    // `(aspect * halfWidth) * drawDesc[2]` @0x71000BC4E4 -- one axis, not both.
    auto extents = [](f32 ratio) {
        auto d = std::make_shared<pd3::EmitterDesc>();
        d->systemType = 0;
        d->prtFlags = 0x1u;
        d->shape = pd3::Shape::Point;
        d->lifetime = 0.0f;
        d->maxDistance = 0.0f;
        d->channels[pd3::kChEmissionRate] = ConstPath(1.0f);
        d->channels[pd3::kChParticleLife] = ConstPath(600.0f);
        d->channels[pd3::kChBirthSize] = ConstPath(2.0f);
        if (ratio != 1.0f)
            d->channels[pd3::kChHeightRatio] = ConstPath(ratio);
        d->DeriveCapabilities();
        pd3::Emitter e;
        e.SetD3Desc(d);
        e.SetVisible(true);
        e.Update(1.0f / 60.0f, 1.0f);
        REQUIRE(e.Pool().AliveCount() >= 1);
        Matrix44f view = Matrix44f::identity();
        whiteout::flakes::renderer::particle::BuildGeometryInput in{};
        in.worldToView = &view;
        std::vector<whiteout::flakes::renderer::Vertex> out;
        REQUIRE(e.BuildGeometry(in, out) >= 6);
        Vector3f lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (std::size_t i = 0; i < 6; ++i)
            for (i32 c = 0; c < 3; ++c) {
                lo.data[c] = (std::min)(lo.data[c], out[i].position.data[c]);
                hi.data[c] = (std::max)(hi.data[c], out[i].position.data[c]);
            }
        std::array<f32, 3> span{hi.x - lo.x, hi.y - lo.y, hi.z - lo.z};
        std::sort(span.begin(), span.end(), [](f32 a, f32 b) { return a > b; });
        return span;
    };

    const std::array<f32, 3> square = extents(1.0f);
    const std::array<f32, 3> squat = extents(0.25f);
    REQUIRE(square[0] > 0.0f);
    // No sheet, so the aspect is 1 and an unscaled quad comes out square.
    CHECK(square[1] == Catch::Approx(square[0]));
    // The width survives and only the other axis is quartered. Reading channel 2
    // as a second SIZE would have moved both.
    CHECK(squat[0] == Catch::Approx(square[0]));
    CHECK(squat[1] == Catch::Approx(square[0] * 0.25f));
}

TEST_CASE("d3 particle: a corpus flip-book resolves against the real install",
          "[d3][particle][corpus]") {
    // The join this phase added, end to end: a uv mode 3 entry names a `.tex`
    // whose own frame table is the sheet's subdivision. Skipped when no D3
    // install is reachable, because the `.tex` is not in the extracted corpus.
    const fs::path root = CorpusRoot();
    const fs::path dir = fs::is_directory(root / "Particle") ? (root / "Particle") : root;
    const fs::path f = dir / "Axe_norm_unique_04_zappyRandom.prt";
    std::error_code ec;
    if (!fs::is_regular_file(f, ec)) {
        WARN("d3 atlas: the named corpus file is absent; nothing checked");
        return;
    }
    auto prt = d3n::parseParticle(ReadAll(f));
    REQUIRE(prt);
    auto d = whiteout::flakes::io::d3::BuildD3EmitterDesc(*prt, -1);
    REQUIRE(d->d3mat.layerCount >= 1);
    // The playback params, which are read out of tAnim4/tAnim5 and are not a
    // UV animation at all.
    CHECK(d->d3mat.layers[0].uv.mode == whiteout::flakes::io::D3UvMode::Anim2D);
    CHECK(d->d3mat.layers[0].atlasRate == Catch::Approx(8.0f));
    CHECK(d->d3mat.layers[0].atlasFrameBase == 0);
    CHECK(d->d3mat.layers[0].atlasFrameRange == 7);

    using ::whiteout::flakes::ProductId;
    whiteout::flakes::io::FileContentProvider provider;
    if (const char* r = std::getenv("WDX_TEST_D3_INSTALL"); r && *r)
        provider.SetInstallPath(r);
    provider.SetGame(ProductId::D3);
    if (provider.GamePath(ProductId::D3).empty()) {
        WARN("d3 atlas: no Diablo III install (set WDX_TEST_D3_INSTALL); SKIPPED.");
        return;
    }
    whiteout::flakes::io::D3SnoCache cache(&provider);
    whiteout::flakes::renderer::profiles::diablo3::D3ResolveParticleMaterial(*prt, &cache,
                                                                            d->d3mat);
    if (!d->d3mat.layers[0].atlas) {
        WARN("d3 atlas: the sheet did not resolve; the frame table was not checked");
        return;
    }
    // Eight tiles across a 1024x256 sheet: the LAST one is what a build that
    // takes the table's junk leading slot for a frame loses.
    const auto& a = *d->d3mat.layers[0].atlas;
    CHECK(a.width == 1024u);
    CHECK(a.height == 256u);
    REQUIRE(a.frames.size() == 8u);
    for (std::size_t k = 0; k < a.frames.size(); ++k) {
        CHECK(a.frames[k].x == Catch::Approx(static_cast<f32>(k) * 0.125f).margin(1e-5));
        CHECK(a.frames[k].z == Catch::Approx(static_cast<f32>(k + 1) * 0.125f).margin(1e-5));
        CHECK(a.frames[k].y == Catch::Approx(0.0f).margin(1e-5));
        CHECK(a.frames[k].w == Catch::Approx(1.0f).margin(1e-5));
    }
    std::printf("[d3 atlas] %zu frames, tile %.3f x %.3f\n", a.frames.size(), a.TileSize().x,
                a.TileSize().y);
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

// ---------------------------------------------------------------------------
// P6: the orientation frame. `Particle_BuildOrientationBasis` @0x71000BAB30.
// ---------------------------------------------------------------------------

TEST_CASE("d3 particle P6: the orientation frame is a right/up pair per render mode",
          "[d3][particle][p6]") {
    using Catch::Approx;
    using pd3::BuildQuadFrame;
    using pd3::QuadFrame;

    auto dot = [](const Vector3f& a, const Vector3f& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    };
    auto unit = [&](const Vector3f& v) { return std::sqrt(dot(v, v)); };

    // A camera looking along +Y, and a particle moving along +Z -- the shape of
    // every lightning bolt in the corpus.
    const Vector3f camF{0.0f, 1.0f, 0.0f};
    const Vector3f up{0.0f, 0.0f, 1.0f};
    const Vector3f none{0.0f, 0.0f, 0.0f};

    SECTION("modes that write nothing leave the caller's basis") {
        QuadFrame f;
        // 0 is gated (and only ever for a child-actor system, which draws no
        // quad) and 1 and 8 return immediately. All three must decline rather
        // than invent a frame; the other eleven modes all build one.
        for (i32 mode : {0, 1, 8})
            CHECK_FALSE(BuildQuadFrame(mode, {camF, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}}, f));
    }

    SECTION("mode 13 stands the quad up and yaws it at the camera") {
        QuadFrame f;
        REQUIRE(BuildQuadFrame(13, {camF}, f));
        // This is the whole bug: the quad's up is WORLD Z, not world Y. A
        // ground-plane basis -- right (1,0,0), up (0,1,0) -- lays every bolt
        // flat, which is what this build drew.
        CHECK(f.up.x == Approx(0.0f));
        CHECK(f.up.y == Approx(0.0f));
        CHECK(f.up.z == Approx(1.0f));
        CHECK(f.right.z == Approx(0.0f));
        CHECK(unit(f.right) == Approx(1.0f));
        // Facing the camera means the quad's own right is across the view.
        CHECK(dot(f.right, camF) == Approx(0.0f).margin(1e-6));
        // ...and it tracks: turn the camera 90 degrees and the quad turns with it.
        QuadFrame g;
        REQUIRE(BuildQuadFrame(13, {{1.0f, 0.0f, 0.0f}}, g));
        CHECK(dot(g.right, f.right) == Approx(0.0f).margin(1e-6));
        CHECK(g.up.z == Approx(1.0f));
        // A camera looking straight down has no XY to flatten; the engine bails.
        CHECK_FALSE(BuildQuadFrame(13, {{0.0f, 0.0f, -1.0f}}, g));
    }

    SECTION("mode 6 reaches mode 13's frame by the other route") {
        // The evidence for the column convention: mode 6 runs the negated
        // camera direction through `Particle_SelectOrientationAxis`'s column
        // order (-c, b, n) and mode 13 runs the flattened one through its own
        // (c, b, u). They agree vector for vector only if a column order of
        // (right, up, normal) is the right reading.
        for (const Vector3f& cam :
             {Vector3f{0, 1, 0}, Vector3f{1, 0, 0}, Vector3f{0.6f, -0.8f, 0.3f}}) {
            QuadFrame a, b;
            REQUIRE(BuildQuadFrame(6, {cam}, a));
            REQUIRE(BuildQuadFrame(13, {cam}, b));
            CHECK(a.right.x == Approx(b.right.x));
            CHECK(a.right.y == Approx(b.right.y));
            CHECK(a.right.z == Approx(b.right.z).margin(1e-6));
            CHECK(a.up.x == Approx(b.up.x).margin(1e-6));
            CHECK(a.up.y == Approx(b.up.y).margin(1e-6));
            CHECK(a.up.z == Approx(b.up.z));
        }
    }

    SECTION("modes 2 and 12 stretch the quad along the particle's own axis") {
        const Vector3f vel{0.0f, 0.0f, 0.5f};
        QuadFrame f2, f12;
        REQUIRE(BuildQuadFrame(2, {camF, vel, {0, 0, 1}}, f2));
        // Up IS the direction of travel, normalised -- a bolt runs along itself.
        CHECK(f2.up.z == Approx(1.0f));
        CHECK(unit(f2.up) == Approx(1.0f));
        CHECK(dot(f2.right, f2.up) == Approx(0.0f).margin(1e-6));
        // Mode 2 resolves the spin about that axis against the CAMERA...
        CHECK(dot(f2.right, camF) == Approx(0.0f).margin(1e-6));

        // ...and mode 12 against world up, which for a vertical bolt is the
        // degenerate case the engine walks into and we decline.
        CHECK_FALSE(BuildQuadFrame(12, {camF, vel, {0, 0, 1}}, f12));
        const Vector3f lateral{0.3f, 0.4f, 0.0f};
        REQUIRE(BuildQuadFrame(12, {camF, lateral, {0.6f, 0.8f, 0.0f}}, f12));
        CHECK(f12.up.x == Approx(0.6f));
        CHECK(f12.up.y == Approx(0.8f));
        CHECK(dot(f12.right, up) == Approx(0.0f).margin(1e-6));
        CHECK(dot(f12.right, f12.up) == Approx(0.0f).margin(1e-6));
    }

    SECTION("modes 3 to 5 stand the quad ACROSS its axis") {
        QuadFrame f;
        // Mode 4 is the direction from the system to the particle, flattened --
        // an outward-facing wall, which is what an expanding ring of beams is.
        REQUIRE(BuildQuadFrame(4, {camF, none, {1, 0, 0}, {3.0f, 0.0f, 9.0f}}, f));
        CHECK(std::fabs(f.up.z) == Approx(1.0f).margin(1e-6));  // stands upright
        CHECK(dot(f.right, Vector3f{1, 0, 0}) == Approx(0.0f).margin(1e-6));
        // Mode 5 keeps the Z, so the same offset tilts the wall back.
        QuadFrame g;
        REQUIRE(BuildQuadFrame(5, {camF, none, {1, 0, 0}, {3.0f, 0.0f, 9.0f}}, g));
        CHECK(std::fabs(g.up.z) != Approx(1.0f));
    }

    SECTION("mode 7 is the emitter's own frame, permuted") {
        QuadFrame f;
        // Columns (q*Y, q*Z, q*X). An unrotated emitter therefore stands the
        // quad in the world YZ plane facing +X -- vertical, not flat.
        REQUIRE(BuildQuadFrame(7, {camF}, f));
        CHECK(f.right.y == Approx(1.0f));
        CHECK(f.up.z == Approx(1.0f));
        CHECK(dot(f.right, f.up) == Approx(0.0f).margin(1e-6));
        // It ignores the camera entirely: turn the camera and nothing moves.
        QuadFrame g;
        REQUIRE(BuildQuadFrame(7, {{1.0f, 0.0f, 0.0f}}, g));
        CHECK(g.right.y == Approx(f.right.y));
        CHECK(g.up.z == Approx(f.up.z));
        // Turn the EMITTER a quarter turn about Z and the frame goes with it.
        pd3::FrameInput in;
        in.camForward = camF;
        in.emitterQuat = Quaternion::from_axis_angle({0.0f, 0.0f, 1.0f}, 1.57079633f);
        QuadFrame h;
        REQUIRE(BuildQuadFrame(7, in, h));
        CHECK(h.right.x == Approx(-1.0f).margin(1e-5));  // q * worldY
        CHECK(h.up.z == Approx(1.0f).margin(1e-5));      // q * worldUp, unmoved
    }

    SECTION("modes 9 and 10 lie flat and follow the ground") {
        pd3::FrameInput in;
        in.camForward = camF;
        // The OTHER permutation: columns (R*X, R*Y, R*Z), so an identity R is a
        // quad lying flat. That is the basis this build used to hand mode 13,
        // which is vertical -- the two were swapped.
        for (i32 mode : {9, 10}) {
            QuadFrame f;
            REQUIRE(BuildQuadFrame(mode, in, f));
            CHECK(f.right.x == Approx(1.0f));
            CHECK(f.up.y == Approx(1.0f));
            CHECK(f.up.z == Approx(0.0f).margin(1e-6));
        }
        // On a slope the quad tilts with it: the frame's normal is the ground
        // normal, which is what "ground-conforming" means.
        in.groundNormal = {0.0f, 0.6f, 0.8f};
        QuadFrame s;
        REQUIRE(BuildQuadFrame(9, in, s));
        const Vector3f n{s.right.y * s.up.z - s.right.z * s.up.y,
                         s.right.z * s.up.x - s.right.x * s.up.z,
                         s.right.x * s.up.y - s.right.y * s.up.x};
        CHECK(n.x == Approx(in.groundNormal.x).margin(1e-5));
        CHECK(n.y == Approx(in.groundNormal.y).margin(1e-5));
        CHECK(n.z == Approx(in.groundNormal.z).margin(1e-5));
        CHECK(unit(s.right) == Approx(1.0f));
        CHECK(dot(s.right, s.up) == Approx(0.0f).margin(1e-5));
        // The flat grid is world up, which is also the engine's raycast-miss
        // value, so a viewer with no terrain gets the flat case above.
        in.groundNormal = {0.0f, 0.0f, 1.0f};
        QuadFrame g;
        REQUIRE(BuildQuadFrame(10, in, g));
        CHECK(g.up.y == Approx(1.0f));
    }

    SECTION("a particle that has stopped keeps the direction it had") {
        // `SelectFrameAxis`: the raw step, else the last unit vector, else world
        // X. The engine skips the normalise on a step below the epsilon rather
        // than zeroing it, so a stalled bolt does not snap to world X.
        QuadFrame f;
        REQUIRE(BuildQuadFrame(12, {camF, none, {0.6f, 0.8f, 0.0f}}, f));
        CHECK(f.up.x == Approx(0.6f));
        CHECK(f.up.y == Approx(0.8f));
        // One that never moved at all does.
        QuadFrame g;
        REQUIRE(BuildQuadFrame(12, {camF}, g));
        CHECK(g.up.x == Approx(1.0f));
        CHECK(g.up.y == Approx(0.0f));
    }
}

// ---------------------------------------------------------------------------
// P7: the GATED arm of the same function -- the orientation a spawned child
// actor is born holding. `ParticleSystem_EmitParticle` @0x71000B1A00 runs the
// switch at emit and hands the result straight to `Actor_SpawnFromSno`.
// ---------------------------------------------------------------------------

TEST_CASE("d3 particle P7: the gated arm orients a spawned child actor",
          "[d3][particle][p7]") {
    using Catch::Approx;
    using pd3::BuildChildOrientation;
    using pd3::BuildQuadFrame;
    using pd3::QuadFrame;

    // The rotation's columns, which is what the quaternion is: column 0 is
    // where the model's own +X ends up.
    auto col = [](const Quaternion& q, int i) {
        const Vector3f e[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        return q.rotate_vector(e[i]);
    };
    auto same = [](const Vector3f& a, const Vector3f& b) {
        return std::abs(a.x - b.x) < 1e-5f && std::abs(a.y - b.y) < 1e-5f &&
               std::abs(a.z - b.z) < 1e-5f;
    };

    // A camera looking along +Y (so it sits at -Y) and a particle moving +Y.
    const Vector3f camF{0.0f, 1.0f, 0.0f};
    const Quaternion spun = Quaternion::from_axis_angle({0.0f, 0.0f, 1.0f}, 0.7f);

    SECTION("modes 1 and 8 leave the emitter quaternion standing") {
        // 3,917 + 19 files, and 1,116 of them spawn child actors -- the
        // second-largest population there is. The engine writes nothing into
        // the slot, and what is already in it is `sys+0x64`, the system's own
        // quaternion, copied there at @0x71000B1EC0.
        Quaternion q = spun;
        for (i32 mode : {1, 8}) {
            CHECK_FALSE(BuildChildOrientation(mode, {camF}, q));
            CHECK(q.x == Approx(spun.x));
            CHECK(q.w == Approx(spun.w));
        }
    }

    SECTION("mode 7 is the emitter quaternion verbatim") {
        // `if (gated) { *a3 = *a6; return; }` -- four floats, no arithmetic.
        // 2,143 child-actor systems, the largest share of the 4,795.
        pd3::FrameInput in;
        in.camForward = camF;
        in.emitterQuat = spun;
        Quaternion q = Quaternion::identity();
        REQUIRE(BuildChildOrientation(7, in, q));
        CHECK(q.x == Approx(spun.x));
        CHECK(q.y == Approx(spun.y));
        CHECK(q.z == Approx(spun.z));
        CHECK(q.w == Approx(spun.w));
    }

    SECTION("mode 3's gated arm is its ungated frame, cyclically shifted") {
        // The two arms of one mode, cross-checked against each other. Ungated
        // reads (right, up, normal); gated reads the same three vectors as
        // (normal, right, up). Getting that permutation wrong is a 120-degree
        // relabel, which is exactly what this catches.
        pd3::FrameInput in;
        in.camForward = camF;
        in.axis = {0.0f, 1.0f, 0.0f};
        in.axisUnit = in.axis;

        QuadFrame f;
        REQUIRE(BuildQuadFrame(3, in, f));
        const Vector3f normal{f.right.y * f.up.z - f.right.z * f.up.y,
                              f.right.z * f.up.x - f.right.x * f.up.z,
                              f.right.x * f.up.y - f.right.y * f.up.x};

        Quaternion q = Quaternion::identity();
        REQUIRE(BuildChildOrientation(3, in, q));
        CHECK(same(col(q, 0), normal));
        CHECK(same(col(q, 1), f.right));
        CHECK(same(col(q, 2), f.up));
    }

    SECTION("a velocity-aligned child faces the way it is going") {
        // Modes 2, 3 and 12 -- 428 child-actor systems. Column 0 is the axis,
        // which for a Z-up +X-forward model is its facing, and column 2 stays
        // world up for as long as the axis is horizontal.
        pd3::FrameInput in;
        in.camForward = camF;
        in.axis = {0.0f, 2.5f, 0.0f}; // unnormalised on purpose
        in.axisUnit = in.axis;
        for (i32 mode : {2, 3, 12}) {
            Quaternion q = Quaternion::identity();
            REQUIRE(BuildChildOrientation(mode, in, q));
            CHECK(same(col(q, 0), {0.0f, 1.0f, 0.0f}));
            CHECK(same(col(q, 2), {0.0f, 0.0f, 1.0f}));
        }
        // Straight up has no horizontal perpendicular, so the engine writes
        // nothing and the emitter's quaternion stands.
        pd3::FrameInput vert = in;
        vert.axis = {0.0f, 0.0f, 1.0f};
        vert.axisUnit = vert.axis;
        Quaternion q = Quaternion::identity();
        CHECK_FALSE(BuildChildOrientation(12, vert, q));
    }

    SECTION("modes 0 and 13 turn the child to face the camera") {
        // 907 + 187 systems. Mode 0 takes the view direction as it is and mode
        // 13 flattens it, so with a horizontal camera the two must agree --
        // the same cross-check that settled the column convention for quads.
        Quaternion a = Quaternion::identity(), b = Quaternion::identity();
        REQUIRE(BuildChildOrientation(0, {camF}, a));
        REQUIRE(BuildChildOrientation(13, {camF}, b));
        for (int i = 0; i < 3; ++i)
            CHECK(same(col(a, i), col(b, i)));
        // Facing the camera means +X points back down the view direction.
        CHECK(same(col(a, 0), {0.0f, -1.0f, 0.0f}));
        CHECK(same(col(a, 2), {0.0f, 0.0f, 1.0f}));

        // Tilt the camera down and mode 13 keeps the child upright while mode
        // 0 leans it -- that is the whole difference between the two.
        pd3::FrameInput tilted;
        tilted.camForward = {0.0f, 0.707107f, -0.707107f};
        Quaternion c = Quaternion::identity(), d = Quaternion::identity();
        REQUIRE(BuildChildOrientation(0, tilted, c));
        REQUIRE(BuildChildOrientation(13, tilted, d));
        CHECK(same(col(d, 2), {0.0f, 0.0f, 1.0f}));
        CHECK_FALSE(same(col(c, 2), {0.0f, 0.0f, 1.0f}));
    }

    SECTION("gated modes 2 and 11 drop the camera the ungated ones cross with") {
        // `Particle_OrientationBasisHelper` @0x71000BBF10 opens with
        // `if (gated) { Particle_QuaternionFromAxes(...); return; }` and never
        // reaches the cross with the view direction.
        pd3::FrameInput in;
        in.axis = {0.3f, 0.9f, 0.1f};
        in.axisUnit = in.axis;
        in.fromSystem = {1.0f, 2.0f, 0.5f};
        for (i32 mode : {2, 11}) {
            pd3::FrameInput a = in, b = in;
            a.camForward = {0.0f, 1.0f, 0.0f};
            b.camForward = {1.0f, 0.0f, 0.0f};
            Quaternion qa = Quaternion::identity(), qb = Quaternion::identity();
            REQUIRE(BuildChildOrientation(mode, a, qa));
            REQUIRE(BuildChildOrientation(mode, b, qb));
            CHECK(qa.x == Approx(qb.x));
            CHECK(qa.y == Approx(qb.y));
            CHECK(qa.z == Approx(qb.z));
            CHECK(qa.w == Approx(qb.w));
        }
        // The ungated arm of the same mode does not: it is a billboard.
        QuadFrame fa, fb;
        pd3::FrameInput a = in, b = in;
        a.camForward = {0.0f, 1.0f, 0.0f};
        b.camForward = {1.0f, 0.0f, 0.0f};
        REQUIRE(BuildQuadFrame(2, a, fa));
        REQUIRE(BuildQuadFrame(2, b, fb));
        CHECK_FALSE(same(fa.right, fb.right));
    }

    SECTION("modes 9 and 10 stand the child on the ground normal") {
        pd3::FrameInput in;
        in.camForward = camF;
        // Flat ground and an unrotated emitter compose to nothing at all.
        Quaternion q = Quaternion::identity();
        REQUIRE(BuildChildOrientation(9, in, q));
        for (int i = 0; i < 3; ++i) {
            const Vector3f e[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
            CHECK(same(col(q, i), e[i]));
        }
        // On a slope the child's own up lands on the terrain normal, which is
        // what "ground-conforming" means for a model rather than a quad.
        in.groundNormal = {0.0f, 0.6f, 0.8f};
        Quaternion s = Quaternion::identity();
        REQUIRE(BuildChildOrientation(10, in, s));
        CHECK(same(col(s, 2), in.groundNormal));
    }
}

// ---------------------------------------------------------------------------
// The DISTORTION phase, particle side.
//
// A geoset surface resolves TWO surfaces when its `Shaders` declares both a
// body pass and a phase-3 one (`D3Surface::distortion`). A particle material
// resolves ONE `D3PassState`, through `D3ScenePassIndex`, and there is a real
// question behind that: an emitter whose asset declared both phases would have
// its distortion half silently dropped, because the scene pass wins the pick
// and `MaterialDesc::distortion` then reads false.
//
// This measures whether that case is in the shipped data. It is not: every
// particle material carrying phase 3 carries NOTHING ELSE, so the one pass the
// emitter resolves IS the distortion pass and no second material is owed. The
// gate is that count staying zero -- if a later corpus turns one up, the
// emitter needs the two-material treatment the geoset side already has.
// ---------------------------------------------------------------------------
TEST_CASE("D3 install: a distortion particle declares phase 3 and nothing else",
          "[d3][particle][material][install]") {
    using ::whiteout::flakes::ProductId;
    namespace wio = whiteout::flakes::io;

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

    // The whole tree by default: 21,593 `.prt`, and the population being
    // counted is a handful of them. A limit here would report zero for the
    // reason M3's sweeps did -- see the corpus notes in the M3 tests.
    std::size_t bound = 0;
    if (const char* v = std::getenv("WDX_TEST_D3_MAT_LIMIT"); v && *v)
        bound = static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
    const std::size_t n = (bound == 0) ? files.size() : std::min(bound, files.size());

    std::size_t resolved = 0, withDistortion = 0, distortionOnly = 0, mixed = 0;
    std::map<std::string, std::size_t> programs;
    std::vector<std::string> mixedNames;
    for (std::size_t i = 0; i < n; ++i) {
        auto prt = d3n::parseParticle(ReadAll(files[i]));
        if (!prt)
            continue;
        const auto shaders = wio::D3ResolveShaders(prt->tMaterial, &cache);
        if (!shaders || shaders->arRenderPasses.empty())
            continue;
        ++resolved;
        const i32 di = wio::D3DistortionPassIndex(*shaders);
        if (di < 0)
            continue;
        ++withDistortion;
        std::size_t phase3 = 0;
        for (const auto& pass : shaders->arRenderPasses)
            if (pass.dwUnknown00 == wio::kD3RenderPhaseDistortion)
                ++phase3;
        if (phase3 == shaders->arRenderPasses.size()) {
            ++distortionOnly;
        } else {
            ++mixed;
            if (mixedNames.size() < 8)
                mixedNames.push_back(files[i].filename().string());
        }
        const auto& p = shaders->arRenderPasses[static_cast<std::size_t>(di)];
        ++programs[p.szEffectFile + "::" + p.szPixelShaderEntry];
        // The pick the emitter actually makes. For a phase-3-only asset the
        // scene index and the distortion index are the same pass, which is why
        // `MaterialDesc::distortion` comes out true without a second material.
        const auto st =
            whiteout::flakes::renderer::profiles::diablo3::D3PassStateFor(prt->tMaterial, &cache);
        CHECK(st.resolved);
        CHECK(st.distortion == (phase3 == shaders->arRenderPasses.size()));
    }

    std::printf("[d3-distort/prt] %zu of %zu .prt resolve a Shaders; %zu carry phase 3 "
                "(%zu phase-3 ONLY, %zu mixed)\n",
                resolved, n, withDistortion, distortionOnly, mixed);
    for (const auto& [name, count] : programs)
        std::printf("[d3-distort/prt]   %-40s %zu\n", name.c_str(), count);
    for (const auto& name : mixedNames)
        std::printf("[d3-distort/prt]   MIXED: %s\n", name.c_str());

    if (resolved == 0) {
        WARN("no particle material resolved. SKIPPED, not passed.");
        return;
    }
    // Vacuity: a run finding no distortion emitter at all proves nothing.
    CHECK(withDistortion > 0);
    // The gate. Non-zero means an emitter is losing its distortion half.
    CHECK(mixed == 0);
}
