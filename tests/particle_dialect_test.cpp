// The particle dialect — which client's CParticleEmitter2 a profile runs.
//
// Two jobs, and the first is the load-bearing one:
//
//  1. Pin `ParticleBehavior::Wc3()` field by field. Those defaults ARE the
//     Warcraft III contract: every "this refactor does not move WC3" claim in
//     M2_PARTICLE_PLAN.md rests on a Wc3() emitter taking exactly the branches
//     it took before the dialect existed. Asserting them literally is what
//     turns "bit-identical by construction" into something a later edit to a
//     default cannot quietly break.
//
//  2. Check each profile answers with the dialect it should.
//
// Divergence *behaviour* (what a Wow() emitter actually simulates) is tested
// as it lands, phase by phase — see M2_PARTICLE_PLAN.md.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "core/particle_dialect.h"
#include "profiles/wc3/wc3_profile.h"
#include "renderer/particle/particle2_emitter.h"
#include "renderer/particle/particle_geometry.h"
#include "renderer/render_settings.h"
#include "whiteout/flakes/model_types.h"
#if WDX_ENABLE_M2
#include "profiles/wow/wow_profile.h"
#endif

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using Catch::Approx;
using whiteout::flakes::f32;
using whiteout::flakes::i32;
using whiteout::flakes::u16;
using whiteout::flakes::u32;
using whiteout::flakes::usize;
using whiteout::flakes::Vector3f;
using whiteout::flakes::renderer::RenderSettings;
using whiteout::flakes::renderer::core::ParticleBehavior;
using whiteout::flakes::renderer::core::ParticleDtPolicy;
using whiteout::flakes::renderer::core::ParticleForceModel;
using whiteout::flakes::renderer::profiles::wc3::Wc3HdProfile;
using whiteout::flakes::renderer::profiles::wc3::Wc3SdProfile;

namespace {

using namespace whiteout::flakes::renderer::particle;

std::shared_ptr<EmitterDesc> MakeDesc(f32 life = 1.0f) {
    auto d = std::make_shared<EmitterDesc>();
    d->shape = std::make_shared<PlaneShape>();
    d->lifeSpan = life;
    d->sheet.Set(1, 1);
    return d;
}

// The animated half an actor would push every frame.
void Arm(Emitter2& e, f32 rate = 20.0f, u32 seed = 1234u) {
    e.SetSeed(seed);
    e.SetEmissionRate(rate);
    e.SetLifeSpan(e.Desc().lifeSpan);
    e.Spawn().width = 4.0f;
    e.Spawn().height = 4.0f;
    e.Spawn().latitude = 0.4f;
    e.Spawn().speed.base = 2.0f;
    e.SetVisible(true);
}

// Visibility is cleared at the end of every update, so it has to be re-pushed
// exactly as actor eval does.
void Step(Emitter2& e, f32 dt, i32 count = 1) {
    for (i32 i = 0; i < count; ++i) {
        e.SetVisible(true);
        e.Update(dt, 1.0f);
    }
}

f32 OldestAge(const Emitter2& e) {
    f32 oldest = -1e9f;
    for (usize i = 0; i < e.Pool().AliveCount(); ++i)
        oldest = (std::max)(oldest, e.Pool()[e.Pool().AliveAt(i)].age);
    return oldest;
}

// ---- geometry-builder helpers ----------------------------------------------
//
// The builder cases place particles by hand rather than simulating them: what
// is under test is the quad, and a spawn shape's draws would only make the
// fixture depend on the RNG stream two phases away from here.

using whiteout::flakes::Matrix44f;
using whiteout::flakes::renderer::Vertex;

// Camera at the origin looking down -Z. BasisFromView reads the view matrix's
// columns, so an identity view gives right = +X, up = +Y, forward = -Z, and
// every corner below is readable by inspection.
Matrix44f IdentityView() {
    return Matrix44f::identity();
}

// A desc that draws one flat white unit-square head per particle: constant
// curves, so any offset seen in the output comes from the quad construction.
std::shared_ptr<EmitterDesc> MakeDrawDesc(f32 sizeX = 1.0f, f32 sizeY = 1.0f) {
    auto d = MakeDesc(10.0f);
    d->curves.color.AddKey(0.0f, Vector3f{1.0f, 1.0f, 1.0f});
    d->curves.alpha.AddKey(0.0f, 1.0f);
    d->curves.size.AddKey(0.0f, whiteout::Vector2f{sizeX, sizeY});
    return d;
}

void Place(Emitter2& e, const std::vector<Particle2>& ps) {
    e.Pool().Sync(static_cast<u32>(ps.size()));
    for (const Particle2& p : ps) {
        const u32 idx = e.Pool().PopDead();
        e.Pool().PushAlive(idx);
        e.Pool()[idx] = p;
    }
}

Particle2 At(const Vector3f& pos, const Vector3f& vel = {0, 0, 0}, f32 age = 0.0f,
             u16 seed = 0) {
    Particle2 p;
    p.position = pos;
    p.velocity = vel;
    p.age = age;
    p.SetVarianceAndSeed(0, seed);
    return p;
}

std::vector<Vertex> Build(const Emitter2& e, const Matrix44f& view) {
    std::vector<Vertex> out;
    BuildGeometryInput in;
    in.worldToView = &view;
    BuildEmitterGeometry(e, in, out);
    return out;
}

// Every quad is six vertices; corner 0 is `centre - A + B`.
constexpr usize kVertsPerQuad = 6;

} // namespace

TEST_CASE("wc3 particle dialect is the default-constructed behaviour", "[particle][dialect]") {
    // Not a tautology: it is the invariant that lets every WC3 code path read
    // `behavior_` without a null-or-unset case, and it is what makes a
    // default-constructed Emitter2 a WC3 emitter.
    const ParticleBehavior d{};
    const ParticleBehavior wc3 = ParticleBehavior::Wc3();

    REQUIRE(wc3.dtPolicy == d.dtPolicy);
    REQUIRE(wc3.forceModel == d.forceModel);
    REQUIRE(wc3.birthDrawsVarianceAndSeed == d.birthDrawsVarianceAndSeed);
    REQUIRE(wc3.sortedBuilderDefect == d.sortedBuilderDefect);
}

TEST_CASE("wc3 particle dialect pins the mdx contract", "[particle][dialect]") {
    const ParticleBehavior b = ParticleBehavior::Wc3();

    // Time stepping: one step, clamped to the half-second the MDX path has
    // always clamped to.
    REQUIRE(b.dtPolicy == ParticleDtPolicy::ClampToMax);
    REQUIRE(b.maxStepSeconds == 0.5f);

    // Spawn: one draw per birth, one lifespan for the whole emitter, no
    // per-frame rate jitter, no path interpolation, no distance LOD, no
    // inherited emitter motion. Every one of these would shift the random
    // stream, which is why they are behaviour rather than data.
    REQUIRE_FALSE(b.birthDrawsVarianceAndSeed);
    REQUIRE_FALSE(b.perParticleLifespan);
    REQUIRE_FALSE(b.rateJitterPerFrame);
    REQUIRE_FALSE(b.emitAlongPath);
    REQUIRE_FALSE(b.lodEmissionScale);
    REQUIRE_FALSE(b.inheritEmitterVelocity);

    // Motion: gravity and nothing else.
    REQUIRE(b.forceModel == ParticleForceModel::Wc3Gravity);
    REQUIRE_FALSE(b.implosionKill);
    REQUIRE_FALSE(b.followPosition);

    // Appearance: randomness resolved at spawn, model visibility gates rather
    // than scales, and the sorted draw order is the correct one.
    REQUIRE_FALSE(b.renderRandomsFromSeed);
    REQUIRE_FALSE(b.modelAlphaScalesParticles);
    REQUIRE_FALSE(b.sortedBuilderDefect);
}

TEST_CASE("wow particle dialect selects every measured divergence", "[particle][dialect]") {
    const ParticleBehavior b = ParticleBehavior::Wow();

    REQUIRE(b.dtPolicy == ParticleDtPolicy::FixedSubSteps);
    REQUIRE(b.subStepSeconds == 0.1f);
    REQUIRE(b.forceModel == ParticleForceModel::WowForces);

    REQUIRE(b.birthDrawsVarianceAndSeed);
    REQUIRE(b.perParticleLifespan);
    REQUIRE(b.rateJitterPerFrame);
    REQUIRE(b.emitAlongPath);
    REQUIRE(b.lodEmissionScale);
    REQUIRE(b.inheritEmitterVelocity);
    REQUIRE(b.implosionKill);
    REQUIRE(b.followPosition);
    REQUIRE(b.renderRandomsFromSeed);
    REQUIRE(b.modelAlphaScalesParticles);

    // The shipped defect: reproduced deliberately. If this ever reads false,
    // someone "fixed" a bug that exists in the client and the renderer now
    // draws a different particle set than WoW does.
    REQUIRE(b.sortedBuilderDefect);
}

TEST_CASE("a fresh emitter is a wc3 emitter", "[particle][dialect]") {
    // The registration sites set the behaviour explicitly, but an emitter
    // constructed without one must still simulate — and must simulate WC3,
    // since that is what every existing caller and baseline expects.
    whiteout::flakes::renderer::particle::Emitter2 em;
    REQUIRE(em.Behavior().dtPolicy == ParticleDtPolicy::ClampToMax);
    REQUIRE(em.Behavior().forceModel == ParticleForceModel::Wc3Gravity);
    REQUIRE_FALSE(em.Behavior().sortedBuilderDefect);
}

// ---------------------------------------------------------------------------
// Time stepping
// ---------------------------------------------------------------------------

TEST_CASE("wow steps a long frame as fixed sub-steps plus a remainder", "[particle][dialect]") {
    // 0.83s at a 0.1s sub-step: 8 whole steps + a 0.03 remainder = the whole
    // frame, because the lifespan is long enough not to cap it.
    Emitter2 em;
    em.SetDesc(MakeDesc(10.0f));
    em.SetBehavior(ParticleBehavior::Wow());
    Arm(em);

    Step(em, 0.05f); // seed a particle with a short first frame
    REQUIRE(em.TotalAlive() > 0);
    // Stop emitting: newborns are created BEFORE the ageing pass of the same
    // step, so with emission live the "oldest" particle is a newborn and the
    // measurement is of the wrong thing entirely.
    em.SetEmissionRate(0.0f);
    const f32 before = OldestAge(em);

    Step(em, 0.83f);
    REQUIRE(OldestAge(em) - before == Approx(0.83f).margin(1e-3));
}

TEST_CASE("wow drops the time a short lifespan cannot cover", "[particle][dialect]") {
    // lifespan 0.55 => floor(0.55/0.1) = 5 sub-steps, and dt 0.83 asks for 8.
    // The client takes min(5, 8) fixed steps plus the 0.03 remainder and simply
    // discards the rest: 0.53s of the 0.83s frame is simulated. Nothing alive
    // would have outlived the difference, which is why it is not a bug.
    // The cap makes the simulated time land just under the lifespan by
    // construction, so a particle aged from zero dies right at the boundary and
    // cannot be measured. Lifespan variation gives some particles a longer
    // effective life than the cap value, and one of those is the witness.
    auto d = MakeDesc(0.55f);
    d->lifespanVariation = 0.8f;

    Emitter2 em;
    em.SetDesc(d);
    em.SetBehavior(ParticleBehavior::Wow());
    Arm(em, 300.0f);

    Step(em, 0.02f);
    REQUIRE(em.TotalAlive() > 0);
    em.SetEmissionRate(0.0f);

    // Longest-lived particle in the pool; pool indices are stable for as long
    // as a particle is alive, so this is a handle on that exact particle.
    u32 witness = em.Pool().AliveAt(0);
    for (usize i = 1; i < em.Pool().AliveCount(); ++i) {
        const u32 idx = em.Pool().AliveAt(i);
        if (em.EffectiveLifeSpan(em.Pool()[idx]) > em.EffectiveLifeSpan(em.Pool()[witness]))
            witness = idx;
    }
    REQUIRE(em.EffectiveLifeSpan(em.Pool()[witness]) > 0.7f);
    const f32 before = em.Pool()[witness].age;

    Step(em, 0.83f);

    // 5 fixed steps (floor(0.55/0.1)) instead of the 8 the frame asked for,
    // plus the 0.03 remainder: 0.53s simulated, 0.30s discarded.
    REQUIRE(em.Pool()[witness].age - before == Approx(0.53f).margin(1e-3));
}

TEST_CASE("wc3 clamps a long frame instead of sub-stepping", "[particle][dialect]") {
    Emitter2 em;
    em.SetDesc(MakeDesc(10.0f));
    em.SetBehavior(ParticleBehavior::Wc3());
    Arm(em);

    Step(em, 0.05f);
    REQUIRE(em.TotalAlive() > 0);
    em.SetEmissionRate(0.0f);
    const f32 before = OldestAge(em);

    Step(em, 2.0f); // clamped to maxStepSeconds
    REQUIRE(OldestAge(em) - before == Approx(0.5f).margin(1e-4));
}

// ---------------------------------------------------------------------------
// Spawn
// ---------------------------------------------------------------------------

TEST_CASE("wow birth ages can be negative", "[particle][dialect]") {
    // `fmod(frand * dt, life)` keeps the sign of the draw, so a particle can be
    // born slightly "before" the frame; WC3's `ufrand * dt` never can. Ageing
    // happens later in the same step, so the observable is the age AFTER that:
    // WC3 births land in [dt, 2*dt) and WoW's can land below dt. A clamp in the
    // birth draw would be a silent divergence from the client.
    constexpr f32 kDt = 0.05f;
    Emitter2 em;
    em.SetDesc(MakeDesc(2.0f));
    em.SetBehavior(ParticleBehavior::Wow());
    Arm(em, 200.0f);

    em.SetVisible(true);
    em.Update(kDt, 1.0f);

    bool sawBelowDt = false;
    for (usize i = 0; i < em.Pool().AliveCount(); ++i)
        if (em.Pool()[em.Pool().AliveAt(i)].age < kDt)
            sawBelowDt = true;
    REQUIRE(sawBelowDt);
}

TEST_CASE("wc3 birth ages are never negative", "[particle][dialect]") {
    constexpr f32 kDt = 0.05f;
    Emitter2 em;
    em.SetDesc(MakeDesc(2.0f));
    em.SetBehavior(ParticleBehavior::Wc3());
    Arm(em, 200.0f);

    em.SetVisible(true);
    em.Update(kDt, 1.0f);

    REQUIRE(em.Pool().AliveCount() > 0u);
    for (usize i = 0; i < em.Pool().AliveCount(); ++i) {
        const f32 age = em.Pool()[em.Pool().AliveAt(i)].age;
        REQUIRE(age >= kDt); // i.e. the birth age itself was >= 0
        REQUIRE(age < 2.0f * kDt);
    }
}

TEST_CASE("wow gives each particle its own lifespan", "[particle][dialect]") {
    auto d = MakeDesc(1.0f);
    d->lifespanVariation = 0.5f;
    Emitter2 em;
    em.SetDesc(d);
    em.SetBehavior(ParticleBehavior::Wow());
    Arm(em, 100.0f);

    Step(em, 0.05f);
    REQUIRE(em.Pool().AliveCount() > 2u);

    // Two particles born the same frame should disagree about when they die.
    f32 first = em.EffectiveLifeSpan(em.Pool()[em.Pool().AliveAt(0)]);
    bool differs = false;
    for (usize i = 1; i < em.Pool().AliveCount(); ++i)
        if (em.EffectiveLifeSpan(em.Pool()[em.Pool().AliveAt(i)]) != first)
            differs = true;
    REQUIRE(differs);
}

TEST_CASE("wow re-reads the animated lifespan for particles already alive",
          "[particle][dialect]") {
    // The client recomputes the effective lifespan every frame from the CURRENT
    // track value, so shortening the track retro-actively shortens particles
    // in flight. WC3 has no such track.
    auto d = MakeDesc(4.0f);
    d->lifespanVariation = 0.0f;
    Emitter2 em;
    em.SetDesc(d);
    em.SetBehavior(ParticleBehavior::Wow());
    Arm(em);
    em.SetLifeSpan(4.0f);

    Step(em, 0.05f);
    REQUIRE(em.TotalAlive() > 0);
    const Particle2& p = em.Pool()[em.Pool().AliveAt(0)];
    REQUIRE(em.EffectiveLifeSpan(p) == Approx(4.0f));

    em.SetLifeSpan(0.5f);
    REQUIRE(em.EffectiveLifeSpan(p) == Approx(0.5f));
}

TEST_CASE("wow re-jitters the emission rate every frame, wc3 does not",
          "[particle][dialect]") {
    // The jitter DRAW is the divergence, not its magnitude: it consumes a
    // random the WC3 path never takes, shifting every subsequent number. With
    // the variation at zero the rate is identical, so any difference in the
    // resulting spawns is the stream shift and nothing else.
    auto d = MakeDesc(2.0f);
    d->emissionRateVariation = 0.0f;

    ParticleBehavior noJitter = ParticleBehavior::Wow();
    noJitter.rateJitterPerFrame = false;

    Emitter2 a, b;
    a.SetDesc(d);
    a.SetBehavior(ParticleBehavior::Wow());
    b.SetDesc(d);
    b.SetBehavior(noJitter);
    Arm(a, 60.0f, 777u);
    Arm(b, 60.0f, 777u);

    Step(a, 1.0f / 60.0f, 3);
    Step(b, 1.0f / 60.0f, 3);

    REQUIRE(a.Pool().AliveCount() == b.Pool().AliveCount());
    REQUIRE(a.Pool().AliveCount() > 0u);
    bool anyDifferent = false;
    for (usize i = 0; i < a.Pool().AliveCount(); ++i) {
        const Particle2& pa = a.Pool()[a.Pool().AliveAt(i)];
        const Particle2& pb = b.Pool()[b.Pool().AliveAt(i)];
        if (pa.position.x != pb.position.x || pa.position.y != pb.position.y)
            anyDifferent = true;
    }
    REQUIRE(anyDifferent);
}

TEST_CASE("wow spawns along the emitter's path", "[particle][dialect]") {
    // Moving the emitter a long way in one frame should spread that frame's
    // spawns across the segment it travelled, not stack them at the end of it.
    auto d = MakeDesc(2.0f);
    Emitter2 em;
    em.SetDesc(d);
    em.SetBehavior(ParticleBehavior::Wow());
    Arm(em, 400.0f);
    em.Spawn().width = 0.0f; // isolate the path spread from the plane spread
    em.Spawn().height = 0.0f;
    em.Spawn().speed.base = 0.0f;

    em.SetWorldPosition({0.0f, 0.0f, 0.0f});
    Step(em, 0.05f);
    em.SetWorldPosition({1000.0f, 0.0f, 0.0f});
    Step(em, 0.05f);

    f32 lo = 1e9f, hi = -1e9f;
    for (usize i = 0; i < em.Pool().AliveCount(); ++i) {
        const f32 x = em.Pool()[em.Pool().AliveAt(i)].position.x;
        lo = (std::min)(lo, x);
        hi = (std::max)(hi, x);
    }
    REQUIRE(hi - lo > 100.0f);
}

TEST_CASE("wc3 spawns every particle at the emitter", "[particle][dialect]") {
    auto d = MakeDesc(2.0f);
    Emitter2 em;
    em.SetDesc(d);
    em.SetBehavior(ParticleBehavior::Wc3());
    Arm(em, 400.0f);
    em.Spawn().width = 0.0f;
    em.Spawn().height = 0.0f;
    em.Spawn().speed.base = 0.0f;

    em.SetWorldPosition({0.0f, 0.0f, 0.0f});
    Step(em, 0.05f);
    em.SetWorldPosition({1000.0f, 0.0f, 0.0f});
    Step(em, 0.05f);

    // The MDX path ignores emitter travel entirely: modelToWorld is identity
    // here, so every particle sits at the origin.
    for (usize i = 0; i < em.Pool().AliveCount(); ++i)
        REQUIRE(em.Pool()[em.Pool().AliveAt(i)].position.x == Approx(0.0f).margin(1e-4));
}

// ---------------------------------------------------------------------------
// Motion
// ---------------------------------------------------------------------------

TEST_CASE("the wow force model applies wind, gravity and drag in the client's order",
          "[particle][dialect]") {
    MotionParams m;
    m.gravity = {0.0f, 0.0f, -10.0f};
    m.wind = {2.0f, 0.0f, 0.0f};
    m.drag = 0.5f;

    Particle2 p;
    p.velocity = {1.0f, 0.0f, 0.0f};
    const ParticleForces f = CalculateForcesWow(m, 0.5f);
    REQUIRE(MoveParticleWow(p, f, 0.5f, false, {0, 0, 0}));

    // wind first: vx = 1 + 2*0.5 = 2; displacement uses THAT, before gravity:
    // x = 2*0.5 = 1. Then drag last: vx = 2 * (1 - min(0.5*0.5, 1)) = 1.5.
    REQUIRE(p.position.x == Approx(1.0f).margin(1e-5));
    REQUIRE(p.velocity.x == Approx(1.5f).margin(1e-5));
    // gravity contributes both a velocity delta and its own position term.
    REQUIRE(p.position.z == Approx(-1.25f).margin(1e-5));
    REQUIRE(p.velocity.z == Approx(-5.0f * 0.75f).margin(1e-5));
}

TEST_CASE("wow drag is clamped so a long step cannot reverse a particle",
          "[particle][dialect]") {
    MotionParams m;
    m.drag = 10.0f;
    Particle2 p;
    p.velocity = {5.0f, 0.0f, 0.0f};
    // drag*dt = 5, unclamped that would give v *= -4 and fling it backwards.
    const ParticleForces f = CalculateForcesWow(m, 0.5f);
    REQUIRE(f.drag == Approx(1.0f));
    MoveParticleWow(p, f, 0.5f, false, {0, 0, 0});
    REQUIRE(p.velocity.x == Approx(0.0f).margin(1e-6));
}

TEST_CASE("implosion and follow are per-emitter, not dialect-wide", "[particle][dialect]") {
    // The dialect says WoW HAS an implosion filter; the desc says whether this
    // emitter asked for one. Conflating the two kills nearly every particle on
    // every WoW emitter, since outward motion is the normal case.
    auto d = MakeDesc(2.0f);
    REQUIRE_FALSE(d->implosionFilter);

    Emitter2 em;
    em.SetDesc(d);
    em.SetBehavior(ParticleBehavior::Wow());
    Arm(em, 60.0f);
    Step(em, 0.05f, 4);
    REQUIRE(em.TotalAlive() > 0);
}

TEST_CASE("the implosion filter kills only outbound particles", "[particle][dialect]") {
    MotionParams m;
    Particle2 outbound;
    outbound.position = {1.0f, 0.0f, 0.0f};
    outbound.velocity = {1.0f, 0.0f, 0.0f};
    Particle2 inbound;
    inbound.position = {1.0f, 0.0f, 0.0f};
    inbound.velocity = {-1.0f, 0.0f, 0.0f};

    const ParticleForces f = CalculateForcesWow(m, 0.1f);
    REQUIRE_FALSE(MoveParticleWow(outbound, f, 0.1f, true, {0, 0, 0}));
    REQUIRE(MoveParticleWow(inbound, f, 0.1f, true, {0, 0, 0}));
}

TEST_CASE("a following emitter drags its older particles along", "[particle][dialect]") {
    // The pull is only on particles older than 2*dt: a newborn is already where
    // the emitter is, so moving it again would double-count the travel.
    auto d = MakeDesc(4.0f);
    d->followPosition = true;
    d->followBias = 1.0f; // constant factor 1 — the whole delta, no speed ramp
    d->followSlope = 0.0f;

    Emitter2 em;
    em.SetDesc(d);
    em.SetBehavior(ParticleBehavior::Wow());
    Arm(em, 40.0f);
    // A stationary point emitter with no launch speed: every particle sits at
    // the origin, so any x it ends up with came from the follow term and
    // nothing else.
    em.Spawn().speed.base = 0.0f;
    em.Spawn().width = 0.0f;
    em.Spawn().height = 0.0f;
    em.SetWorldPosition({0, 0, 0});
    Step(em, 0.05f, 3);
    REQUIRE(em.TotalAlive() > 0);

    f32 before = 0.0f;
    for (usize i = 0; i < em.Pool().AliveCount(); ++i)
        before = (std::max)(before, em.Pool()[em.Pool().AliveAt(i)].position.x);
    REQUIRE(before == Approx(0.0f).margin(1e-4f));

    // Silence emission first: WoW also spawns ALONG the path, so newborns would
    // land at the far end of the move and the measurement would pass without
    // any existing particle having followed anything.
    em.SetEmissionRate(0.0f);
    em.SetWorldPosition({10.0f, 0, 0});
    Step(em, 0.05f);

    f32 after = 0.0f;
    for (usize i = 0; i < em.Pool().AliveCount(); ++i)
        after = (std::max)(after, em.Pool()[em.Pool().AliveAt(i)].position.x);
    CHECK(after == Approx(before + 10.0f).margin(1e-3f));
}

TEST_CASE("the follow factor is a clamped line in the emitter's own speed",
          "[particle][dialect]") {
    // Zero slope and zero bias is the degenerate record: the feature is flagged
    // on but the line is flat at zero, so nothing follows. This is the state
    // SolveFollowLine produces from two identical sample speeds.
    auto d = MakeDesc(4.0f);
    d->followPosition = true;
    d->followBias = 0.0f;
    d->followSlope = 0.0f;

    Emitter2 em;
    em.SetDesc(d);
    em.SetBehavior(ParticleBehavior::Wow());
    Arm(em, 40.0f);
    em.Spawn().speed.base = 0.0f;
    em.Spawn().width = 0.0f; // a point emitter, so x is only ever the follow term
    em.Spawn().height = 0.0f;
    em.SetWorldPosition({0, 0, 0});
    Step(em, 0.05f, 3);
    REQUIRE(em.TotalAlive() > 0);

    em.SetEmissionRate(0.0f);
    em.SetWorldPosition({10.0f, 0, 0});
    Step(em, 0.05f);
    for (usize i = 0; i < em.Pool().AliveCount(); ++i)
        CHECK(em.Pool()[em.Pool().AliveAt(i)].position.x == Approx(0.0f).margin(1e-4f));
}

TEST_CASE("distance fades a wow emitter's rate and never a wc3 one",
          "[particle][dialect]") {
    // The falloff floors at 0.25, so a far emitter still emits a quarter of its
    // rate rather than stopping — and lodIgnoreDistance opts out entirely.
    auto plain = MakeDesc(2.0f);
    auto ignoring = MakeDesc(2.0f);
    ignoring->lodIgnoreDistance = true;

    auto populationAt = [](std::shared_ptr<EmitterDesc> d, f32 dist,
                           const ParticleBehavior& b) {
        Emitter2 em;
        em.SetDesc(d);
        em.SetBehavior(b);
        Arm(em, 100.0f, 77u);
        em.SetViewDistance(dist);
        Step(em, 0.05f, 4);
        return em.TotalAlive();
    };

    const i32 near = populationAt(plain, 0.0f, ParticleBehavior::Wow());
    const i32 far = populationAt(plain, 500.0f, ParticleBehavior::Wow());
    CHECK(far < near);
    CHECK(populationAt(ignoring, 500.0f, ParticleBehavior::Wow()) == near);

    // WC3 has no such falloff: distance is not an input to its emission at all.
    CHECK(populationAt(plain, 500.0f, ParticleBehavior::Wc3()) ==
          populationAt(plain, 0.0f, ParticleBehavior::Wc3()));
}

// ---------------------------------------------------------------------------
// The shared half really is shared
// ---------------------------------------------------------------------------

TEST_CASE("both dialects share the emission accumulator and the pool",
          "[particle][dialect]") {
    // Same rate, same dt, same sub-step size: the fractional accumulator and the
    // alive/dead recycling are common code, so the population must agree even
    // though almost everything about the particles differs.
    auto d = MakeDesc(1.0f);

    Emitter2 wc3, wow;
    wc3.SetDesc(d);
    wc3.SetBehavior(ParticleBehavior::Wc3());
    wow.SetDesc(d);
    wow.SetBehavior(ParticleBehavior::Wow());
    Arm(wc3, 30.0f, 55u);
    Arm(wow, 30.0f, 55u);

    // dt below the sub-step size, so WoW takes one step exactly like WC3.
    Step(wc3, 0.05f, 10);
    Step(wow, 0.05f, 10);
    REQUIRE(wc3.Pool().AliveCount() == wow.Pool().AliveCount());
}

TEST_CASE("wc3 still advances the curve cursor through the aux word",
          "[particle][dialect]") {
    // The four bytes WoW spends on variance and seed are WC3's curve hint, and
    // the WoW path must not touch them for a WC3 emitter.
    auto d = MakeDesc(1.0f);
    d->curves.color.AddKey(0.0f, {1, 1, 1});
    d->curves.color.AddKey(0.5f, {0.5f, 0.5f, 0.5f});
    d->curves.color.AddKey(1.0f, {0, 0, 0});

    Emitter2 em;
    em.SetDesc(d);
    em.SetBehavior(ParticleBehavior::Wc3());
    Arm(em, 20.0f);

    Step(em, 0.05f);
    REQUIRE(em.TotalAlive() > 0);
    REQUIRE(em.Pool()[em.Pool().AliveAt(0)].Cursor() == 0u);

    Step(em, 0.05f, 13); // past the 0.5 key
    REQUIRE(em.TotalAlive() > 0);
    REQUIRE(em.Pool()[em.Pool().AliveAt(0)].Cursor() == 1u);
}

TEST_CASE("wow keeps its variance and seed intact for the particle's whole life",
          "[particle][dialect]") {
    auto d = MakeDesc(2.0f);
    d->lifespanVariation = 0.3f;
    d->curves.color.AddKey(0.0f, {1, 1, 1});
    d->curves.color.AddKey(0.5f, {0.5f, 0.5f, 0.5f});
    d->curves.color.AddKey(1.0f, {0, 0, 0});

    Emitter2 em;
    em.SetDesc(d);
    em.SetBehavior(ParticleBehavior::Wow());
    Arm(em, 20.0f);

    Step(em, 0.05f);
    REQUIRE(em.TotalAlive() > 0);
    const u32 auxAtBirth = em.Pool()[em.Pool().AliveAt(0)].aux;

    Step(em, 0.05f, 12);
    REQUIRE(em.TotalAlive() > 0);
    REQUIRE(em.Pool()[em.Pool().AliveAt(0)].aux == auxAtBirth);
}

TEST_CASE("a wc3 quad is exactly the six vertices it always was",
          "[particle][dialect][geometry]") {
    // The in-test half of the L2 lock. `particle-diff.ps1` compares the real
    // corpus stream; this pins the shape of one quad so a WC3 regression shows
    // up in the device-free suite too, where CI can see it.
    auto d = MakeDrawDesc(2.0f, 2.0f);
    Emitter2 em;
    em.SetDesc(d);
    em.SetBehavior(ParticleBehavior::Wc3());
    Place(em, {At({0, 0, -5})});

    const Matrix44f view = IdentityView();
    const auto v = Build(em, view);
    REQUIRE(v.size() == kVertsPerQuad);

    // right = +X and up = +Y, so the corners are (-2,+2), (-2,-2), (+2,+2),
    // (+2,-2) about the particle, emitted 0,1,2,3,2,1.
    REQUIRE(v[0].position.x == Approx(-2.0f));
    REQUIRE(v[0].position.y == Approx(2.0f));
    REQUIRE(v[1].position.x == Approx(-2.0f));
    REQUIRE(v[1].position.y == Approx(-2.0f));
    REQUIRE(v[2].position.x == Approx(2.0f));
    REQUIRE(v[2].position.y == Approx(2.0f));
    REQUIRE(v[3].position.x == Approx(2.0f));
    REQUIRE(v[3].position.y == Approx(-2.0f));
    REQUIRE(v[4].position.x == Approx(v[2].position.x));
    REQUIRE(v[5].position.x == Approx(v[1].position.x));
    for (const Vertex& vx : v)
        REQUIRE(vx.position.z == Approx(-5.0f));
}

TEST_CASE("the wow sorted builder reproduces its use-after-pop",
          "[particle][dialect][geometry]") {
    // The shipped defect: for N depth-sorted particles the emitted order is
    // ranks 2,3,...,N,N. Asserted as required behaviour — repairing it would
    // diverge from every depth-sorted emitter in the client.
    auto d = MakeDrawDesc();
    d->sortZ = true;

    // Distinct depths, and an x that tags which particle each quad came from.
    const std::vector<Particle2> ps = {At({10, 0, -1}), At({20, 0, -2}), At({30, 0, -3}),
                                       At({40, 0, -4})};
    const Matrix44f view = IdentityView();

    Emitter2 wow;
    wow.SetDesc(d);
    wow.SetBehavior(ParticleBehavior::Wow());
    Place(wow, ps);
    const auto vw = Build(wow, view);
    REQUIRE(vw.size() == 4 * kVertsPerQuad);

    // Corner 0 sits one unit left of the particle, so +1 recovers the tag.
    std::vector<f32> emitted;
    for (usize q = 0; q < 4; ++q)
        emitted.push_back(vw[q * kVertsPerQuad].position.x + 1.0f);
    // Farthest first is 40, 30, 20, 10 — what comes out is 30, 20, 10, 10.
    REQUIRE(emitted[0] == Approx(30.0f));
    REQUIRE(emitted[1] == Approx(20.0f));
    REQUIRE(emitted[2] == Approx(10.0f));
    REQUIRE(emitted[3] == Approx(10.0f));

    Emitter2 wc3;
    wc3.SetDesc(d);
    wc3.SetBehavior(ParticleBehavior::Wc3());
    Place(wc3, ps);
    const auto vc3 = Build(wc3, view);
    REQUIRE(vc3.size() == 4 * kVertsPerQuad);
    // Reforged dequeues by value and is correct; nothing is dropped or doubled.
    REQUIRE(vc3[0 * kVertsPerQuad].position.x + 1.0f == Approx(40.0f));
    REQUIRE(vc3[1 * kVertsPerQuad].position.x + 1.0f == Approx(30.0f));
    REQUIRE(vc3[2 * kVertsPerQuad].position.x + 1.0f == Approx(20.0f));
    REQUIRE(vc3[3 * kVertsPerQuad].position.x + 1.0f == Approx(10.0f));
}

TEST_CASE("a single sorted wow particle is untouched by the defect",
          "[particle][dialect][geometry]") {
    // pop() skips the reorder below two elements, so N == 1 still draws itself.
    auto d = MakeDrawDesc();
    d->sortZ = true;
    Emitter2 em;
    em.SetDesc(d);
    em.SetBehavior(ParticleBehavior::Wow());
    Place(em, {At({7, 0, -1})});
    const Matrix44f view = IdentityView();
    const auto v = Build(em, view);
    REQUIRE(v.size() == kVertsPerQuad);
    REQUIRE(v[0].position.x + 1.0f == Approx(7.0f));
}

TEST_CASE("wow render randomness is a pure function of seed and age",
          "[particle][dialect][geometry]") {
    // The seed-stability property: everything random about how a particle LOOKS
    // is re-derived per frame from its own seed, so building the same frame
    // twice has to be identical bit for bit — including the size jitter and the
    // spin, which are the two that actually draw.
    auto d = MakeDrawDesc();
    d->sizeVariation = {0.5f, 0.5f};
    d->unscaledSizeVariation = true;
    d->baseSpin = 0.3f;
    d->baseSpinVariation = 0.9f;
    d->spinSpeed = 1.1f;
    d->spinSpeedVariation = 0.7f;

    Emitter2 em;
    em.SetDesc(d);
    em.SetBehavior(ParticleBehavior::Wow());
    std::vector<Particle2> ps;
    for (u16 s = 0; s < 8; ++s)
        ps.push_back(At({static_cast<f32>(s), 0, -3}, {0, 0, 0}, 0.25f, s));
    Place(em, ps);

    const Matrix44f view = IdentityView();
    const auto a = Build(em, view);
    const auto b = Build(em, view);
    REQUIRE(a.size() == 8 * kVertsPerQuad);
    REQUIRE(a.size() == b.size());
    for (usize i = 0; i < a.size(); ++i) {
        REQUIRE(a[i].position.x == b[i].position.x);
        REQUIRE(a[i].position.y == b[i].position.y);
        REQUIRE(a[i].uv.x == b[i].uv.x);
    }

    // And genuinely varying: without this the property above would hold just as
    // well for randomness that is a constant.
    std::vector<f32> offsets;
    for (usize q = 0; q < 8; ++q)
        offsets.push_back(a[q * kVertsPerQuad].position.x - static_cast<f32>(q));
    std::sort(offsets.begin(), offsets.end());
    REQUIRE(offsets.back() - offsets.front() > 0.01f);
}

TEST_CASE("twinkle blinks particles out and pulses the rest",
          "[particle][dialect][geometry]") {
    const Matrix44f view = IdentityView();

    {   // Percent 0: every table entry is at least 0, so nothing survives.
        auto d = MakeDrawDesc();
        d->twinklePercent = 0.0f;
        Emitter2 em;
        em.SetDesc(d);
        em.SetBehavior(ParticleBehavior::Wow());
        std::vector<Particle2> ps;
        for (u16 s = 0; s < 64; ++s)
            ps.push_back(At({0, 0, -3}, {0, 0, 0}, 0.0f, s));
        Place(em, ps);
        REQUIRE(Build(em, view).empty());
    }
    {   // Percent 1 with no size range is the inert default: nothing culled.
        auto d = MakeDrawDesc();
        Emitter2 em;
        em.SetDesc(d);
        em.SetBehavior(ParticleBehavior::Wow());
        std::vector<Particle2> ps;
        for (u16 s = 0; s < 64; ++s)
            ps.push_back(At({0, 0, -3}, {0, 0, 0}, 0.0f, s));
        Place(em, ps);
        REQUIRE(Build(em, view).size() == 64 * kVertsPerQuad);
    }
    {   // Half the time, indexed by the particle's own seed. The table is fixed
        // rather than rand()-seeded like the client's, which is what makes this
        // assertable at all — see the note in particle_geometry.cpp.
        auto d = MakeDrawDesc();
        d->twinklePercent = 0.5f;
        Emitter2 em;
        em.SetDesc(d);
        em.SetBehavior(ParticleBehavior::Wow());
        std::vector<Particle2> ps;
        for (u16 s = 0; s < 128; ++s)
            ps.push_back(At({0, 0, -3}, {0, 0, 0}, 0.0f, s));
        Place(em, ps);
        const usize quads = Build(em, view).size() / kVertsPerQuad;
        REQUIRE(quads > 40);
        REQUIRE(quads < 88);
    }
    {   // Size range {2,7}: the multiplier is uniform in [2,7], so the smallest
        // quad drawn is at least twice the track size and the largest at most
        // seven times it.
        auto d = MakeDrawDesc();
        d->twinkleBase = 2.0f;
        d->twinkleVary = 5.0f;
        Emitter2 em;
        em.SetDesc(d);
        em.SetBehavior(ParticleBehavior::Wow());
        std::vector<Particle2> ps;
        for (u16 s = 0; s < 128; ++s)
            ps.push_back(At({0, 0, -3}, {0, 0, 0}, 0.0f, s));
        Place(em, ps);
        const auto v = Build(em, view);
        REQUIRE(v.size() == 128 * kVertsPerQuad);
        f32 lo = 1e9f, hi = -1e9f;
        for (usize q = 0; q < 128; ++q) {
            const f32 half = -v[q * kVertsPerQuad].position.x;
            lo = (std::min)(lo, half);
            hi = (std::max)(hi, half);
        }
        REQUIRE(lo >= Approx(2.0f).margin(1e-4));
        REQUIRE(hi <= Approx(7.0f).margin(1e-4));
        REQUIRE(hi - lo > 1.0f);
    }
}

TEST_CASE("spin rotates the wow quad, and a base spin alone does not",
          "[particle][dialect][geometry]") {
    const Matrix44f view = IdentityView();
    {   // Quarter turn after one second: right becomes up, up becomes -right.
        auto d = MakeDrawDesc();
        d->spinSpeed = 1.5707963f;
        Emitter2 em;
        em.SetDesc(d);
        em.SetBehavior(ParticleBehavior::Wow());
        Place(em, {At({0, 0, -3}, {0, 0, 0}, 1.0f)});
        const auto v = Build(em, view);
        REQUIRE(v.size() == kVertsPerQuad);
        // A = up*1, B = -right*1, so corner 0 = -A + B = (-1, -1).
        REQUIRE(v[0].position.x == Approx(-1.0f).margin(1e-5));
        REQUIRE(v[0].position.y == Approx(-1.0f).margin(1e-5));
    }
    {   // The client tests only the two SPEED terms before rotating, so a base
        // spin with no speed draws square. Reproduced, not repaired.
        auto d = MakeDrawDesc();
        d->baseSpin = 1.5707963f;
        Emitter2 em;
        em.SetDesc(d);
        em.SetBehavior(ParticleBehavior::Wow());
        Place(em, {At({0, 0, -3}, {0, 0, 0}, 1.0f)});
        const auto v = Build(em, view);
        REQUIRE(v[0].position.x == Approx(-1.0f));
        REQUIRE(v[0].position.y == Approx(1.0f));
    }
    {   // OffsetHeadBySpin moves the whole quad along its own spun up-axis.
        auto d = MakeDrawDesc();
        d->spinSpeed = 1.5707963f;
        d->offsetHeadBySpin = true;
        Emitter2 em;
        em.SetDesc(d);
        em.SetBehavior(ParticleBehavior::Wow());
        Place(em, {At({0, 0, -3}, {0, 0, 0}, 1.0f)});
        const auto v = Build(em, view);
        // Centre moved by B = (-1, 0), so corner 0 lands one further left.
        REQUIRE(v[0].position.x == Approx(-2.0f).margin(1e-5));
        REQUIRE(v[0].position.y == Approx(-1.0f).margin(1e-5));
    }
    {   // NegateSpinRandom flips the angle for odd seeds, so a pool of mixed
        // seeds does not rotate as one body.
        auto spun = MakeDrawDesc();
        spun->spinSpeed = 0.7f;
        auto negated = std::make_shared<EmitterDesc>(*spun);
        negated->negateSpinRandom = true;

        auto cornerY = [&](const std::shared_ptr<EmitterDesc>& desc, u16 seed) {
            Emitter2 em;
            em.SetDesc(desc);
            em.SetBehavior(ParticleBehavior::Wow());
            Place(em, {At({0, 0, -3}, {0, 0, 0}, 1.0f, seed)});
            return Build(em, view)[0].position.y;
        };

        // Without the flag every particle takes the same angle.
        REQUIRE(cornerY(spun, 2) == Approx(cornerY(spun, 3)));
        // With it, the odd-seeded one turns the other way and the even-seeded
        // one is left alone.
        REQUIRE(cornerY(negated, 2) == Approx(cornerY(spun, 2)));
        REQUIRE(cornerY(negated, 3) != Approx(cornerY(spun, 3)));
        // Corner 0's y is cos - sin, so negating the angle only flips the sine
        // term: the two land either side of cos, not either side of zero.
        REQUIRE(cornerY(negated, 2) + cornerY(negated, 3) ==
                Approx(2.0f * std::cos(0.7f)).margin(1e-5));
    }
}

TEST_CASE("wow quad orientation modes pick different bases",
          "[particle][dialect][geometry]") {
    const Matrix44f view = IdentityView();
    {   // Velocity-orient lays A along the reversed velocity as the camera sees
        // it: a particle moving +Y gets A pointing -Y.
        auto d = MakeDrawDesc();
        d->velocityOrient = true;
        Emitter2 em;
        em.SetDesc(d);
        em.SetBehavior(ParticleBehavior::Wow());
        Place(em, {At({0, 0, -3}, {0, 5, 0})});
        const auto v = Build(em, view);
        // A = (0,-1,0), B = (1,0,0) → corner 0 = -A + B = (1, 1).
        REQUIRE(v[0].position.x == Approx(1.0f).margin(1e-5));
        REQUIRE(v[0].position.y == Approx(1.0f).margin(1e-5));
    }
    {   // Velocity straight at the camera has no on-screen direction, so the
        // quad collapses rather than picking an arbitrary one.
        auto d = MakeDrawDesc();
        d->velocityOrient = true;
        Emitter2 em;
        em.SetDesc(d);
        em.SetBehavior(ParticleBehavior::Wow());
        Place(em, {At({0, 0, -3}, {0, 0, 5})});
        const auto v = Build(em, view);
        REQUIRE(v[0].position.x == Approx(0.0f).margin(1e-5));
        REQUIRE(v[0].position.y == Approx(0.0f).margin(1e-5));
    }
    {   // XYQuad lies flat in the world XY plane and ignores the camera, which
        // is the whole point of it — so a rolled camera does not move it.
        auto d = MakeDrawDesc();
        d->xyQuads = true;
        Emitter2 em;
        em.SetDesc(d);
        em.SetBehavior(ParticleBehavior::Wow());
        Place(em, {At({0, 0, -3})});

        Matrix44f rolled = Matrix44f::identity();
        rolled.data[0][0] = 0.0f;
        rolled.data[0][1] = 1.0f;
        rolled.data[1][0] = -1.0f;
        rolled.data[1][1] = 0.0f;
        const auto v = Build(em, rolled);
        REQUIRE(v[0].position.x == Approx(-1.0f));
        REQUIRE(v[0].position.y == Approx(1.0f));
        REQUIRE(v[0].position.z == Approx(-3.0f));
    }
}

TEST_CASE("a wow tail stretches along the velocity and clamps to age",
          "[particle][dialect][geometry]") {
    const Matrix44f view = IdentityView();
    auto d = MakeDrawDesc();
    d->hasHead = false;
    d->hasTail = true;
    d->tailLength = 2.0f;

    {
        Emitter2 em;
        em.SetDesc(d);
        em.SetBehavior(ParticleBehavior::Wow());
        Place(em, {At({0, 0, -3}, {0, 5, 0}, 1.0f)});
        const auto v = Build(em, view);
        REQUIRE(v.size() == kVertsPerQuad);
        // Tail vector is -velocity * 2 = (0,-10,0); the far pair sits there.
        REQUIRE(v[2].position.y == Approx(-10.0f).margin(1e-4));
        REQUIRE(v[0].position.y == Approx(0.0f).margin(1e-4));
    }
    {
        auto clamped = std::make_shared<EmitterDesc>(*d);
        clamped->clampTailToAge = true;
        Emitter2 em;
        em.SetDesc(clamped);
        em.SetBehavior(ParticleBehavior::Wow());
        Place(em, {At({0, 0, -3}, {0, 5, 0}, 0.5f)});
        const auto v = Build(em, view);
        // Half a second old, so the tail is half a second long, not two.
        REQUIRE(v[2].position.y == Approx(-2.5f).margin(1e-4));
    }
}

TEST_CASE("wow cells mask into the sheet and take the emitter's base cell",
          "[particle][dialect][geometry]") {
    const Matrix44f view = IdentityView();
    {   // A 4x4 sheet has 16 cells; cell 20 wraps to 4, which is column 0 of
        // row 1 — the mask is the client's, not a clamp.
        auto d = MakeDrawDesc();
        d->sheet.Set(4, 4);
        d->curves.headCells.AddSegment(1.0f, 20, 20, 1);
        Emitter2 em;
        em.SetDesc(d);
        em.SetBehavior(ParticleBehavior::Wow());
        Place(em, {At({0, 0, -3})});
        const auto v = Build(em, view);
        REQUIRE(v[0].uv.x == Approx(0.0f));
        REQUIRE(v[0].uv.y == Approx(0.25f));
    }
    {   // RandFlipbookStart draws the emitter's offset once, from its own seed,
        // so two actors of one model flip their sheets out of phase.
        auto d = MakeDrawDesc();
        d->sheet.Set(4, 4);
        d->randFlipbookStart = true;
        d->curves.headCells.AddSegment(1.0f, 0, 0, 1);

        std::vector<i32> distinct;
        for (u32 s = 0; s < 12; ++s) {
            Emitter2 em;
            em.SetDesc(d);
            em.SetBehavior(ParticleBehavior::Wow());
            em.SetSeed(s);
            REQUIRE(em.BaseCell() < 16u);
            distinct.push_back(static_cast<i32>(em.BaseCell()));
        }
        std::sort(distinct.begin(), distinct.end());
        distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
        REQUIRE(distinct.size() > 1u);
    }
    {   // Without the flag there is no draw and no offset at all.
        auto d = MakeDrawDesc();
        d->sheet.Set(4, 4);
        Emitter2 em;
        em.SetDesc(d);
        em.SetBehavior(ParticleBehavior::Wow());
        em.SetSeed(7u);
        REQUIRE(em.BaseCell() == 0u);
    }
    {   // ChooseRandomTexture only fires when the head track is empty, and it
        // stays inside the sheet.
        auto d = MakeDrawDesc();
        d->sheet.Set(2, 2);
        d->chooseRandomTexture = true;
        Emitter2 em;
        em.SetDesc(d);
        em.SetBehavior(ParticleBehavior::Wow());
        std::vector<Particle2> ps;
        for (u16 s = 0; s < 32; ++s)
            ps.push_back(At({0, 0, -3}, {0, 0, 0}, 0.0f, s));
        Place(em, ps);
        const auto v = Build(em, view);
        std::vector<f32> us;
        for (usize q = 0; q < 32; ++q)
            us.push_back(v[q * kVertsPerQuad].uv.x);
        REQUIRE(*std::max_element(us.begin(), us.end()) <= Approx(0.5f));
        REQUIRE(*std::min_element(us.begin(), us.end()) == Approx(0.0f));
    }
}

TEST_CASE("wow particle size converts out of model units",
          "[particle][dialect][geometry]") {
    // The trap this closes: a `.m2` authors its sizes in yards while the actor
    // is scaled into renderer units by WorldScale (100 under the wow profile).
    // Drawing the raw track value makes every particle a hundredth of the size
    // it should be, which reads as "the particles are not drawing at all".
    using whiteout::flakes::renderer::model::FrameState;
    FrameState::ParticleFrameState st{};
    st.transform = Matrix44f::identity();
    st.unitScale = 100.0f;
    st.visibility = 1.0f;
    st.modelAlpha = 1.0f;

    auto d = MakeDrawDesc();
    Emitter2 em;
    em.SetDesc(d);
    em.SetBehavior(ParticleBehavior::Wow());
    em.ApplyState(st);
    REQUIRE(em.UnitScale() == Approx(100.0f));

    Place(em, {At({0, 0, -3})});
    const Matrix44f view = IdentityView();
    const auto v = Build(em, view);
    REQUIRE(v[0].position.x == Approx(-100.0f));
    REQUIRE(v[0].position.y == Approx(100.0f));
}

TEST_CASE("model alpha multiplies wow particle alpha and gates nothing",
          "[particle][dialect][geometry]") {
    using whiteout::flakes::renderer::model::FrameState;
    FrameState::ParticleFrameState st{};
    st.transform = Matrix44f::identity();
    st.unitScale = 1.0f;
    st.visibility = 1.0f;
    st.modelAlpha = 0.25f;

    auto d = MakeDrawDesc();
    Emitter2 em;
    em.SetDesc(d);
    em.SetBehavior(ParticleBehavior::Wow());
    em.ApplyState(st);
    Place(em, {At({0, 0, -3})});
    const Matrix44f view = IdentityView();
    const auto v = Build(em, view);
    // Still drawn — WC3 would have gated the emitter off; WoW dims it instead.
    REQUIRE(v.size() == kVertsPerQuad);
    REQUIRE(v[0].color.w == Approx(0.25f).margin(0.01));
}

TEST_CASE("profiles report their particle dialect", "[particle][dialect]") {
    RenderSettings settings;
    Wc3SdProfile sd(settings);
    Wc3HdProfile hd(settings);

    REQUIRE(sd.Particles().forceModel == ParticleForceModel::Wc3Gravity);
    REQUIRE_FALSE(sd.Particles().birthDrawsVarianceAndSeed);
    REQUIRE(hd.Particles().forceModel == ParticleForceModel::Wc3Gravity);
    REQUIRE_FALSE(hd.Particles().birthDrawsVarianceAndSeed);

#if WDX_ENABLE_M2
    whiteout::flakes::renderer::profiles::wow::WowProfile wow(settings);
    REQUIRE(wow.Particles().forceModel == ParticleForceModel::WowForces);
    REQUIRE(wow.Particles().birthDrawsVarianceAndSeed);
    REQUIRE(wow.Particles().sortedBuilderDefect);
#endif
}
