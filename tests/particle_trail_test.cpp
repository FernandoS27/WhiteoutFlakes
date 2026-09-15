// Trail emitters (M2 RPID) and the random-spacing defect they lean on.
//
// A trail emitter is a second model's emitter, adopted by this one, that every
// live particle drives at its own position — the client's "child emitter".
// What is pinned here is the simulation contract in M2_TRAIL_EMITTER_DESIGN.md
// §1 and §3: a trail never emits on its own, it is driven once per live
// particle per sub-step, it spawns on the particle, and its pool is sized as
// the product the client's Sync computes.
//
// Everything is synthetic. Loading the five shipped carriers is the corpus
// sweep's job (m2_particle_test.cpp); what a corpus cannot check is the
// arithmetic, because no reference image of this feature exists.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/particle/particle_adapters.h"
#include "renderer/particle/particle_service.h"

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

using Catch::Approx;
using whiteout::flakes::f32;
using whiteout::flakes::i32;
using whiteout::flakes::Matrix44f;
using whiteout::flakes::u32;
using whiteout::flakes::usize;
using whiteout::flakes::Vector3f;
using whiteout::flakes::renderer::Vertex;
using whiteout::flakes::renderer::core::ParticleBehavior;

using namespace whiteout::flakes::renderer::particle;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

// A point emitter: zero spawn area and zero speed, so a particle is born
// exactly at the emitter's transform and every offset seen in a test is one the
// code under test put there.
std::shared_ptr<EmitterDesc> PointDesc(f32 life = 10.0f) {
    auto d = std::make_shared<EmitterDesc>();
    d->shape = std::make_shared<WowPlaneShape>();
    d->lifeSpan = life;
    d->sheet.Set(1, 1);
    d->curves.color.AddKey(0.0f, Vector3f{1.0f, 1.0f, 1.0f});
    d->curves.alpha.AddKey(0.0f, 1.0f);
    d->curves.size.AddKey(0.0f, whiteout::Vector2f{1.0f, 1.0f});
    return d;
}

Matrix44f TranslationOf(const Vector3f& p) {
    Matrix44f m = Matrix44f::identity();
    m.data[3][0] = p.x;
    m.data[3][1] = p.y;
    m.data[3][2] = p.z;
    return m;
}

// The emitter's world position and its transform are the same thing in the
// client — `EmitNewParticles` reads the spawn point out of the matrix it is
// handed — so a test that moves one has to move the other.
void PlaceAt(Emitter2& e, const Vector3f& p) {
    e.SetModelToWorld(TranslationOf(p));
    e.SetWorldPosition(p);
}

void Arm(Emitter2& e, f32 rate, u32 seed) {
    e.SetBehavior(ParticleBehavior::Wow());
    e.SetSeed(seed);
    e.SetEmissionRate(rate);
    e.SetLifeSpan(e.Desc().lifeSpan);
    e.Spawn().speed.base = 0.0f;
    e.Spawn().speed.variance = 0.0f;
}

void Step(Emitter2& e, f32 dt = kDt, i32 count = 1) {
    for (i32 i = 0; i < count; ++i) {
        e.SetVisible(true);
        e.Update(dt, 1.0f);
    }
}

Vector3f PositionOf(const Emitter2& e, usize i) {
    return e.Pool()[e.Pool().AliveAt(i)].position;
}

// A trail, armed the way the loader arms one: its own desc and seed, then the
// static state it will run on for the rest of its life.
std::unique_ptr<Emitter2> MakeTrail(std::shared_ptr<EmitterDesc> desc, f32 rate, u32 seed) {
    auto t = std::make_unique<Emitter2>();
    t->SetDesc(std::move(desc));
    t->SetBehavior(ParticleBehavior::Wow());
    t->SetSeed(seed);
    whiteout::flakes::renderer::model::FrameState::ParticleFrameState st{};
    st.emissionRate = rate;
    st.lifeSpan = t->Desc().lifeSpan;
    st.visibility = 1.0f;
    st.enabled = true;
    st.hasGravityVector = true;
    t->SetTrailState(st);
    return t;
}

} // namespace

// ---------------------------------------------------------------------------
// The random-spacing defect (M2_TRAIL_EMITTER_DESIGN.md §3b)
// ---------------------------------------------------------------------------

TEST_CASE("random emission spacing spawns at the emitter, not along the path",
          "[particle][trail]") {
    // The client computes a random point between the previous position and this
    // one, writes it into a stack copy of the spawn matrix, and then hands
    // `CreateParticle` the UNMODIFIED matrix (0x1016a6492 vs 0x1016a64ca). So a
    // moving random-spacing emitter piles every spawn on its current position
    // while an evenly-spaced one strings them out behind it.
    auto spaced = PointDesc();
    auto random = PointDesc();
    random->randomEmissionSpacing = true;

    Emitter2 a, b;
    a.SetDesc(spaced);
    b.SetDesc(random);
    Arm(a, 0.0f, 11u);
    Arm(b, 0.0f, 11u);

    // One silent frame first: the emitter's previous position only exists once
    // it has been set twice, and a particle born before the jump would still be
    // sitting at the origin when the assertions run.
    PlaceAt(a, {0, 0, 0});
    PlaceAt(b, {0, 0, 0});
    Step(a);
    Step(b);

    // A big jump, so "at the emitter" and "along the path" cannot be confused.
    a.SetEmissionRate(300.0f);
    b.SetEmissionRate(300.0f);
    PlaceAt(a, {100.0f, 0, 0});
    PlaceAt(b, {100.0f, 0, 0});
    Step(a);
    Step(b);

    REQUIRE(a.TotalAlive() > 2);
    REQUIRE(b.TotalAlive() == a.TotalAlive());

    // Every random-spacing spawn is exactly on the emitter.
    for (usize i = 0; i < b.Pool().AliveCount(); ++i) {
        const Vector3f p = PositionOf(b, i);
        CHECK(p.x == Approx(100.0f).margin(1e-4f));
    }
    // The evenly-spaced one is not: at least one particle sits behind.
    bool sawTrailing = false;
    for (usize i = 0; i < a.Pool().AliveCount(); ++i)
        sawTrailing = sawTrailing || PositionOf(a, i).x < 99.0f;
    CHECK(sawTrailing);
}

TEST_CASE("random emission spacing still consumes its draw", "[particle][trail]") {
    // The position is dead but the draw is not: it advances the stream every
    // later spawn is sequenced against. With the emitter standing still both
    // branches put the particle in the same place, so the ONLY observable
    // difference is that shift — which is exactly what has to survive.
    auto spaced = PointDesc();
    auto random = PointDesc();
    random->randomEmissionSpacing = true;

    Emitter2 a, b;
    a.SetDesc(spaced);
    b.SetDesc(random);
    Arm(a, 300.0f, 77u);
    Arm(b, 300.0f, 77u);
    PlaceAt(a, {0, 0, 0});
    PlaceAt(b, {0, 0, 0});
    Step(a, kDt, 2);
    Step(b, kDt, 2);

    REQUIRE(a.TotalAlive() == b.TotalAlive());
    REQUIRE(a.TotalAlive() > 0);

    bool anyDifferent = false;
    for (usize i = 0; i < a.Pool().AliveCount(); ++i) {
        const auto& pa = a.Pool()[a.Pool().AliveAt(i)];
        const auto& pb = b.Pool()[b.Pool().AliveAt(i)];
        // Same place — the emitter never moved.
        CHECK(pb.position.x == Approx(pa.position.x).margin(1e-5f));
        anyDifferent = anyDifferent || pa.RenderSeed() != pb.RenderSeed();
    }
    CHECK(anyDifferent);
}

// ---------------------------------------------------------------------------
// Adoption
// ---------------------------------------------------------------------------

TEST_CASE("a parent adopts at most four trails", "[particle][trail]") {
    Emitter2 parent;
    parent.SetDesc(PointDesc());
    for (i32 i = 0; i < 6; ++i)
        parent.AddTrail(MakeTrail(PointDesc(), 10.0f, static_cast<u32>(i)));
    CHECK(parent.Trails().size() == Emitter2::kMaxTrails);
}

TEST_CASE("a trail stays enabled without anything animating it", "[particle][trail]") {
    // Its record's model is never placed, so no track ever re-asserts the flag
    // that an ordinary emitter has re-pushed every frame. The client solves it
    // by ORing the enable bit once at adoption and never clearing it.
    Emitter2 parent;
    parent.SetDesc(PointDesc());
    parent.AddTrail(MakeTrail(PointDesc(), 10.0f, 1u));
    Arm(parent, 0.0f, 5u);

    REQUIRE(parent.Trails()[0]->Visible());
    Step(parent, kDt, 3);
    CHECK(parent.Trails()[0]->Visible());
}

// ---------------------------------------------------------------------------
// The drive
// ---------------------------------------------------------------------------

TEST_CASE("a trail emits nothing on its own", "[particle][trail]") {
    // Its only source of particles is the per-particle drive: the client
    // recurses with suppressEmit set, and that is the only path into a trail's
    // update. A parent with no particles therefore leaves its trail empty no
    // matter how high the trail's own rate is.
    Emitter2 parent;
    parent.SetDesc(PointDesc());
    parent.AddTrail(MakeTrail(PointDesc(), 500.0f, 1u));
    Arm(parent, 0.0f, 5u); // parent emits nothing

    PlaceAt(parent, {0, 0, 0});
    Step(parent, kDt, 30);

    CHECK(parent.TotalAlive() == 0);
    CHECK(parent.Trails()[0]->TotalAlive() == 0);
}

TEST_CASE("a trail spawns on the particle driving it", "[particle][trail]") {
    // The client stamps the particle's position into the translation row of the
    // matrix it hands the trail, so a trail particle is born at the driving
    // particle plus whatever its own shape offsets it by — nothing, here.
    Emitter2 parent;
    parent.SetDesc(PointDesc());
    parent.AddTrail(MakeTrail(PointDesc(), 200.0f, 1u));
    Arm(parent, 60.0f, 5u);

    PlaceAt(parent, {7.0f, -3.0f, 2.0f});
    Step(parent, kDt, 4);

    REQUIRE(parent.TotalAlive() > 0);
    const Emitter2& trail = *parent.Trails()[0];
    REQUIRE(trail.TotalAlive() > 0);

    // Every trail particle sits on one of the parent's particles. With a point
    // emitter and no motion they are all at the same place, which is the
    // strongest form of the claim: the trail is where the particles are.
    for (usize i = 0; i < trail.Pool().AliveCount(); ++i) {
        const Vector3f p = trail.Pool()[trail.Pool().AliveAt(i)].position;
        CHECK(p.x == Approx(7.0f).margin(1e-3f));
        CHECK(p.y == Approx(-3.0f).margin(1e-3f));
        CHECK(p.z == Approx(2.0f).margin(1e-3f));
    }
}

TEST_CASE("a trail is driven once per live particle", "[particle][trail]") {
    // One trail emitter serves every particle of its parent, so its emission
    // accumulates once per drive: twice the particles, twice the trail.
    auto run = [](f32 parentRate) {
        Emitter2 parent;
        parent.SetDesc(PointDesc());
        parent.AddTrail(MakeTrail(PointDesc(), 30.0f, 1u));
        Arm(parent, parentRate, 5u);
        PlaceAt(parent, {0, 0, 0});
        Step(parent, kDt, 20);
        return std::pair<i32, i32>{parent.TotalAlive(), parent.Trails()[0]->TotalAlive()};
    };

    const auto few = run(60.0f);
    const auto many = run(120.0f);

    REQUIRE(few.first > 0);
    REQUIRE(many.first > few.first);
    REQUIRE(few.second > 0);
    // Not exactly 2x: emission is an accumulator with a per-drive floor, and
    // the pool is capped. Strictly more is the invariant that matters.
    CHECK(many.second > few.second);
}

TEST_CASE("a trail pool is sized as the client's product", "[particle][trail]") {
    // Sync @0x10169f860: the parent's own capacity times the trail's steady
    // state, clamped to 4096, and never shrinking afterwards.
    Emitter2 parent;
    parent.SetDesc(PointDesc(2.0f));
    parent.AddTrail(MakeTrail(PointDesc(1.0f), 10.0f, 1u));
    Arm(parent, 20.0f, 5u);
    parent.SetLifeSpan(2.0f);

    Step(parent, kDt);

    const u32 parentCap = static_cast<u32>(1.15f * 20.0f * 2.0f);
    const u32 trailOwn = static_cast<u32>(1.15f * 10.0f * 1.0f);
    CHECK(parent.Pool().Capacity() == parentCap);
    CHECK(parent.Trails()[0]->Pool().Capacity() == parentCap * trailOwn);
}

TEST_CASE("a trail pool clamps at four thousand slots", "[particle][trail]") {
    Emitter2 parent;
    parent.SetDesc(PointDesc(4.0f));
    parent.AddTrail(MakeTrail(PointDesc(4.0f), 400.0f, 1u));
    Arm(parent, 400.0f, 5u);
    parent.SetLifeSpan(4.0f);

    Step(parent, kDt);

    CHECK(parent.Trails()[0]->Pool().Capacity() == 4096u);
}

// ---------------------------------------------------------------------------
// The service
// ---------------------------------------------------------------------------

TEST_CASE("the service draws and counts a trail", "[particle][trail]") {
    // A trail holds no key in the emitter map — its owner does — so every
    // service-level walk has to recurse or the trail is invisible to the
    // renderer, the counters and the trace alike.
    ParticleService svc;
    auto owned = std::make_unique<Emitter2>();
    owned->SetDesc(PointDesc());
    owned->AddTrail(MakeTrail(PointDesc(), 200.0f, 1u));
    Arm(*owned, 60.0f, 5u);
    PlaceAt(*owned, {0, 0, 0});
    Emitter2* parent = owned.get();
    svc.AddEmitter(1u, 0, std::move(owned));

    for (i32 i = 0; i < 4; ++i) {
        parent->SetVisible(true);
        svc.Simulate(kDt);
    }

    REQUIRE(parent->Trails()[0]->TotalAlive() > 0);
    CHECK(svc.EmitterCount() == 2);
    CHECK(svc.TotalParticleCount() == parent->TotalAlive() + parent->Trails()[0]->TotalAlive());

    std::vector<Vertex> verts;
    std::vector<EmitterDrawList> lists;
    svc.BuildGeometry(Matrix44f::identity(), {verts, lists});

    REQUIRE(lists.size() == 2);
    CHECK(lists[0].emitterId == 0);
    CHECK(lists[1].emitterId == TrailEmitterId(0, 0));
    // A trail sorts on its owner's origin so the two never separate in the
    // transparent pass.
    CHECK(lists[1].worldOrigin.x == Approx(lists[0].worldOrigin.x));
}
