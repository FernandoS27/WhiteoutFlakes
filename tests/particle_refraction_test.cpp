// Refraction particles (M2 `Refraction`): the two extra texture layers, and
// the routing that keeps such an emitter out of the transparent scene.
//
// A refraction emitter is a `CMultiTexParticle` emitter that draws into a
// distortion buffer rather than into colour. Two halves are pinned here, both
// device-free:
//
//   * the per-particle multi-texture state — a UV origin and a scroll rate per
//     layer, drawn at birth and wrapped every step (M2_REFRACTION_DESIGN.md §2);
//   * the split in ParticleService::BuildGeometry, which is what stops a
//     distortion mask being painted into the scene as if it were colour.
//
// The corpus half — that shipped models really carry the flag, and that the
// record's fixed-point fields decode the way the client reads them — is in
// m2_particle_test.cpp, which is where the `.m2` reader is already wired up.

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
using whiteout::flakes::Vector2f;
using whiteout::flakes::Vector3f;
using whiteout::flakes::Vector4f;
using whiteout::flakes::renderer::Vertex;
using whiteout::flakes::renderer::core::ParticleBehavior;

using namespace whiteout::flakes::renderer::particle;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;

// A point emitter with one sprite cell: a particle is born at the emitter's
// transform and its quad spans the whole texture, so every UV in a test is one
// the code under test put there.
std::shared_ptr<EmitterDesc> PointDesc(f32 life = 10.0f) {
    auto d = std::make_shared<EmitterDesc>();
    d->shape = std::make_shared<WowPlaneShape>();
    d->lifeSpan = life;
    d->sheet.Set(1, 1);
    d->curves.color.AddKey(0.0f, Vector3f{1.0f, 1.0f, 1.0f});
    d->curves.alpha.AddKey(0.0f, 1.0f);
    d->curves.size.AddKey(0.0f, Vector2f{1.0f, 1.0f});
    return d;
}

void Arm(Emitter2& e, f32 rate, u32 seed) {
    e.SetBehavior(ParticleBehavior::Wow());
    e.SetSeed(seed);
    e.SetEmissionRate(rate);
    e.SetLifeSpan(e.Desc().lifeSpan);
    e.Spawn().speed.base = 0.0f;
    e.Spawn().speed.variance = 0.0f;
    e.SetModelToWorld(Matrix44f::identity());
    e.SetWorldPosition({0, 0, 0});
}

void Step(Emitter2& e, f32 dt = kDt, i32 count = 1) {
    for (i32 i = 0; i < count; ++i) {
        e.SetVisible(true);
        e.Update(dt, 1.0f);
    }
}

const MultiTexState& StateOfFirstAlive(const Emitter2& e) {
    return e.MultiTex()[e.Pool().AliveAt(0)];
}

} // namespace

TEST_CASE("a refraction emitter seeds two texture layers at birth", "[particle][refraction]") {
    auto d = PointDesc();
    d->refraction = true;
    d->multiTexScale[0] = 0.25f;
    d->multiTexScale[1] = 0.5f;
    // Zero range means every particle's scroll rate is exactly the centre, so
    // the seeding can be asserted without reasoning about the RNG.
    d->multiTexScrollMid[0] = {0.5f, -0.25f};
    d->multiTexScrollMid[1] = {-1.5f, 2.0f};

    Emitter2 e;
    e.SetDesc(d);
    Arm(e, 60.0f, 0x51DE0001u);
    Step(e);

    REQUIRE(e.Pool().AliveCount() >= 1);
    // Index-parallel with the pool, not with the alive list.
    REQUIRE(e.MultiTex().size() == e.Pool().Capacity());

    const MultiTexState& m = StateOfFirstAlive(e);
    for (usize layer = 0; layer < 2; ++layer) {
        // The UV origin is a pair of [0,1) draws.
        REQUIRE(m.uv[layer].x >= 0.0f);
        REQUIRE(m.uv[layer].x < 1.0f);
        REQUIRE(m.uv[layer].y >= 0.0f);
        REQUIRE(m.uv[layer].y < 1.0f);
    }
    REQUIRE(m.scroll[0].x == Approx(0.5f));
    REQUIRE(m.scroll[0].y == Approx(-0.25f));
    REQUIRE(m.scroll[1].x == Approx(-1.5f));
    REQUIRE(m.scroll[1].y == Approx(2.0f));
}

TEST_CASE("one draw scales both axes of a layer's scroll rate", "[particle][refraction]") {
    // The client takes THREE draws per layer — u, v, then a single symmetric
    // scalar that scales the whole rate range. A layer's u and v rates are
    // therefore perfectly correlated, which is visible as the ratio below.
    auto d = PointDesc();
    d->refraction = true;
    d->multiTexScrollMid[0] = {0.0f, 0.0f};
    d->multiTexScrollRange[0] = {2.0f, 5.0f};

    Emitter2 e;
    e.SetDesc(d);
    Arm(e, 240.0f, 0x51DE0002u);
    Step(e);

    REQUIRE(e.Pool().AliveCount() >= 2);
    usize checked = 0;
    for (usize i = 0; i < e.Pool().AliveCount(); ++i) {
        const MultiTexState& m = e.MultiTex()[e.Pool().AliveAt(i)];
        REQUIRE(std::fabs(m.scroll[0].x) <= 2.0f + 1e-5f);
        REQUIRE(std::fabs(m.scroll[0].y) <= 5.0f + 1e-5f);
        if (std::fabs(m.scroll[0].x) > 1e-4f) {
            REQUIRE(m.scroll[0].y / 5.0f == Approx(m.scroll[0].x / 2.0f).margin(1e-5f));
            ++checked;
        }
    }
    REQUIRE(checked > 0);
}

TEST_CASE("layers scroll and wrap back into [0,1)", "[particle][refraction]") {
    auto d = PointDesc();
    d->refraction = true;
    // One layer runs far forward per step, the other far backward: the wrap is
    // `x - floor(x)`, not fmod, so both stay inside [0,1) rather than the
    // negative one walking off toward -1.
    d->multiTexScrollMid[0] = {37.0f, 0.0f};
    d->multiTexScrollMid[1] = {-41.0f, 0.0f};

    Emitter2 e;
    e.SetDesc(d);
    Arm(e, 60.0f, 0x51DE0003u);
    Step(e);
    REQUIRE(e.Pool().AliveCount() >= 1);

    for (i32 i = 0; i < 40; ++i) {
        Step(e);
        for (usize k = 0; k < e.Pool().AliveCount(); ++k) {
            const MultiTexState& m = e.MultiTex()[e.Pool().AliveAt(k)];
            for (usize layer = 0; layer < 2; ++layer) {
                REQUIRE(m.uv[layer].x >= 0.0f);
                REQUIRE(m.uv[layer].x < 1.0f);
            }
        }
    }
}

TEST_CASE("an ordinary emitter carries no multi-texture state", "[particle][refraction]") {
    auto d = PointDesc();
    Emitter2 e;
    e.SetDesc(d);
    Arm(e, 60.0f, 0x51DE0004u);
    Step(e, kDt, 5);
    REQUIRE(e.Pool().AliveCount() >= 1);
    REQUIRE(e.MultiTex().empty());
}

TEST_CASE("the builder emits one extra UV pair per vertex", "[particle][refraction]") {
    auto d = PointDesc();
    d->refraction = true;
    d->multiTexScale[0] = 0.25f;
    d->multiTexScale[1] = 0.75f;

    Emitter2 e;
    e.SetDesc(d);
    Arm(e, 60.0f, 0x51DE0005u);
    Step(e);
    REQUIRE(e.Pool().AliveCount() == 1);

    const Matrix44f view = Matrix44f::identity();
    std::vector<Vertex> verts;
    std::vector<Vector4f> extra;
    BuildGeometryInput in{};
    in.worldToView = &view;
    in.extraUV = &extra;
    const i32 n = BuildEmitterGeometry(e, in, verts);

    // One head quad: six vertices, and an extra pair for every one of them.
    REQUIRE(n == 6);
    REQUIRE(verts.size() == 6);
    REQUIRE(extra.size() == 6);

    // Each layer's UV is the particle's own origin plus the quad's 0/1 corner
    // times that layer's scale — the same corner the sprite cell uses, which
    // with a 1x1 sheet is the vertex's own UV.
    const MultiTexState& m = StateOfFirstAlive(e);
    for (usize i = 0; i < 6; ++i) {
        const f32 su = verts[i].uv.x;
        const f32 sv = verts[i].uv.y;
        REQUIRE((su == 0.0f || su == 1.0f));
        REQUIRE((sv == 0.0f || sv == 1.0f));
        REQUIRE(extra[i].x == Approx(m.uv[0].x + su * 0.25f));
        REQUIRE(extra[i].y == Approx(m.uv[0].y + sv * 0.25f));
        REQUIRE(extra[i].z == Approx(m.uv[1].x + su * 0.75f));
        REQUIRE(extra[i].w == Approx(m.uv[1].y + sv * 0.75f));
    }
}

TEST_CASE("the service routes refraction emitters out of the scene", "[particle][refraction]") {
    ParticleService svc;

    auto plain = PointDesc();
    auto refract = PointDesc();
    refract->refraction = true;
    refract->multiTexScale[0] = 0.5f;

    for (auto [id, desc] : {std::pair<i32, std::shared_ptr<EmitterDesc>>{0, plain},
                            std::pair<i32, std::shared_ptr<EmitterDesc>>{1, refract}}) {
        auto e = std::make_unique<Emitter2>();
        e->SetDesc(desc);
        Arm(*e, 60.0f, 0x51DE0100u + static_cast<u32>(id));
        svc.AddEmitter(7, ParticleOutput::Billboard, id, std::move(e));
    }

    REQUIRE(svc.HasRefractionEmitters());
    svc.Simulate(kDt);
    // Visibility is applied per frame by the actor layer, which no test scene
    // runs; drive it directly so both emitters release particles.
    svc.ForEachEmitter([](const EmitterKey&, const Emitter2&) {});
    for (i32 id : {0, 1}) {
        Emitter2* e = svc.GetEmitter(7, ParticleOutput::Billboard, id);
        REQUIRE(e != nullptr);
        e->SetVisible(true);
        e->Update(kDt, 1.0f);
    }

    const Matrix44f view = Matrix44f::identity();

    SECTION("with a sink, the two go to different lists") {
        std::vector<Vertex> verts;
        std::vector<EmitterDrawList> draws;
        MultiTexGeometry refractGeo;
        svc.BuildGeometry(view, verts, draws, &refractGeo);

        REQUIRE(draws.size() == 1);
        REQUIRE(draws[0].emitterId == 0);
        REQUIRE(refractGeo.draws.size() == 1);
        REQUIRE(refractGeo.draws[0].emitterId == 1);
        // The refraction emitter's vertices are NOT in the scene stream.
        REQUIRE(verts.size() == static_cast<usize>(draws[0].vertexCount));
        REQUIRE(refractGeo.vertices.size() == refractGeo.extraUV.size());
        REQUIRE(refractGeo.vertices.size() ==
                static_cast<usize>(refractGeo.draws[0].vertexCount));
    }

    SECTION("without a sink, it is dropped rather than drawn as colour") {
        std::vector<Vertex> verts;
        std::vector<EmitterDrawList> draws;
        svc.BuildGeometry(view, verts, draws);

        REQUIRE(draws.size() == 1);
        REQUIRE(draws[0].emitterId == 0);
        REQUIRE(verts.size() == static_cast<usize>(draws[0].vertexCount));
    }
}
