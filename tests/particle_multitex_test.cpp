// Multi-texture particles (M2 `MultiTexture`): the extra texture layers, and
// the routing that keeps their three-UV vertices out of the ordinary particle
// stream while leaving their DRAW in the sorted transparent queue.
//
// A multi-texture emitter is a `CMultiTexParticle` emitter like a refraction
// one — same per-particle UV state, same six random draws at birth — but it
// stays in the transparent pass and only changes shader. That difference is
// the whole point of these cases: refraction leaves the scene, multi-texture
// does not, and the two must not be confused for one another by anything
// downstream. See M2_MULTITEX_DESIGN.md §2.
//
// The corpus half — that shipped models really carry the flag, and that the
// three 5-bit texture indices decode the way the client splits them — is in
// m2_particle_test.cpp, which is where the `.m2` reader is already wired up.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/particle/particle_adapters.h"
#include "renderer/particle/particle_service.h"

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

void Step(Emitter2& e) {
    e.SetVisible(true);
    e.Update(kDt, 1.0f);
}

const MultiTexState& StateOfFirstAlive(const Emitter2& e) {
    return e.MultiTex()[e.Pool().AliveAt(0)];
}

// Both emitters in a service, simulated one step so each has released a
// particle. Returns the service by value-ish through the out param because
// Emitter2 is move-only through unique_ptr.
void AddArmed(ParticleService& svc, i32 id, std::shared_ptr<EmitterDesc> desc) {
    auto e = std::make_unique<Emitter2>();
    e->SetDesc(std::move(desc));
    Arm(*e, 60.0f, 0x51DE0200u + static_cast<u32>(id));
    svc.AddEmitter(7, id, std::move(e));
}

void DriveOnce(ParticleService& svc, std::initializer_list<i32> ids) {
    svc.Simulate(kDt);
    // Visibility is applied per frame by the actor layer, which no test scene
    // runs; drive it directly so every emitter releases a particle.
    for (i32 id : ids) {
        Emitter2* e = svc.GetEmitter(7, ParticleOutput::Billboard, id)->AsEmitter2();
        REQUIRE(e != nullptr);
        e->SetVisible(true);
        e->Update(kDt, 1.0f);
    }
}

} // namespace

TEST_CASE("a multi-texture emitter carries the same per-particle layers refraction does",
          "[particle][multitex]") {
    // The client allocates `CMultiTexParticle` for particle types 2 AND 3, and
    // `CreateParticle(CMultiTexParticle&)` @0x1016a10b0 is one function serving
    // both — so a multi-texture emitter takes the same six random draws at
    // birth. That is not cosmetic: those draws come off the emitter's own
    // stream, so an emitter that failed to take them would spawn every later
    // particle differently.
    auto d = PointDesc();
    d->multiTexture = true;
    d->multiTexScale[0] = 0.25f;
    d->multiTexScale[1] = 0.75f;

    REQUIRE(d->UsesMultiTexLayers());
    REQUIRE_FALSE(d->refraction);

    Emitter2 e;
    e.SetDesc(d);
    Arm(e, 60.0f, 0x51DE0005u);
    Step(e);
    REQUIRE(e.Pool().AliveCount() == 1);
    REQUIRE_FALSE(e.MultiTex().empty());

    const Matrix44f view = Matrix44f::identity();
    std::vector<Vertex> verts;
    std::vector<Vector4f> extra;
    BuildGeometryInput in{};
    in.worldToView = &view;
    in.extraUV = &extra;
    const i32 n = BuildEmitterGeometry(e, in, verts);

    REQUIRE(n == 6);
    REQUIRE(extra.size() == 6);

    const MultiTexState& m = StateOfFirstAlive(e);
    for (usize i = 0; i < 6; ++i) {
        const f32 su = verts[i].uv.x;
        const f32 sv = verts[i].uv.y;
        REQUIRE(extra[i].x == Approx(m.uv[0].x + su * 0.25f));
        REQUIRE(extra[i].y == Approx(m.uv[0].y + sv * 0.25f));
        REQUIRE(extra[i].z == Approx(m.uv[1].x + su * 0.75f));
        REQUIRE(extra[i].w == Approx(m.uv[1].y + sv * 0.75f));
    }
}

TEST_CASE("a multi-texture emitter keeps its draw in the scene but its vertices apart",
          "[particle][multitex]") {
    ParticleService svc;

    auto plain = PointDesc();
    auto multi = PointDesc();
    multi->multiTexture = true;
    multi->multiTexScale[0] = 0.5f;
    multi->material.multiTexture = true;
    multi->material.textureId = 3;
    multi->material.textureId2 = 4;
    multi->material.textureId3 = 5;

    AddArmed(svc, 0, plain);
    AddArmed(svc, 1, multi);
    // Unlike refraction, this is NOT a pass of its own.
    REQUIRE_FALSE(svc.HasRefractionEmitters());
    DriveOnce(svc, {0, 1});

    const Matrix44f view = Matrix44f::identity();

    SECTION("with a sink, both draws sort together and only the vertices split") {
        std::vector<Vertex> verts;
        std::vector<EmitterDrawList> draws;
        MultiTexGeometry multiGeo;
        svc.BuildGeometry(view, {verts, draws, nullptr, &multiGeo});

        // Two draws in the ONE sorted list — that is the difference from
        // refraction, which would have moved its draw out entirely.
        REQUIRE(draws.size() == 2);
        REQUIRE(multiGeo.draws.empty());

        const EmitterDrawList* plainDraw = nullptr;
        const EmitterDrawList* multiDraw = nullptr;
        for (const auto& dl : draws)
            (dl.emitterId == 0 ? plainDraw : multiDraw) = &dl;
        REQUIRE(plainDraw != nullptr);
        REQUIRE(multiDraw != nullptr);

        // The bit the dispatcher reads to know which buffer the offsets index.
        CHECK_FALSE(plainDraw->material.multiTexture);
        CHECK(multiDraw->material.multiTexture);
        CHECK(multiDraw->material.textureId2 == 4);
        CHECK(multiDraw->material.textureId3 == 5);

        // The ordinary stream holds only the plain emitter's vertices; the
        // three-UV stream holds only the multi-texture one's, with an extra UV
        // pair for every vertex.
        CHECK(verts.size() == static_cast<usize>(plainDraw->vertexCount));
        CHECK(multiGeo.vertices.size() == static_cast<usize>(multiDraw->vertexCount));
        CHECK(multiGeo.extraUV.size() == multiGeo.vertices.size());
        CHECK(multiDraw->vertexOffset == 0);
    }

    SECTION("without a sink it falls back to one layer, and says so") {
        std::vector<Vertex> verts;
        std::vector<EmitterDrawList> draws;
        svc.BuildGeometry(view, {verts, draws});

        // Still drawn — a multi-texture emitter is ordinary colour, so an
        // approximate particle beats a missing one.
        REQUIRE(draws.size() == 2);
        usize total = 0;
        for (const auto& dl : draws) {
            // ...but the draw must not claim a stream that was never built,
            // or the dispatcher would read another emitter's vertices.
            CHECK_FALSE(dl.material.multiTexture);
            total += static_cast<usize>(dl.vertexCount);
        }
        CHECK(verts.size() == total);
    }
}

TEST_CASE("refraction and multi-texture emitters take separate streams",
          "[particle][multitex]") {
    ParticleService svc;

    auto refract = PointDesc();
    refract->refraction = true;
    auto multi = PointDesc();
    multi->multiTexture = true;
    multi->material.multiTexture = true;

    AddArmed(svc, 0, refract);
    AddArmed(svc, 1, multi);
    REQUIRE(svc.HasRefractionEmitters());
    DriveOnce(svc, {0, 1});

    const Matrix44f view = Matrix44f::identity();
    std::vector<Vertex> verts;
    std::vector<EmitterDrawList> draws;
    MultiTexGeometry refractGeo;
    MultiTexGeometry multiGeo;
    svc.BuildGeometry(view, {verts, draws, &refractGeo, &multiGeo});

    // The refraction emitter leaves the scene; the multi-texture one does not.
    REQUIRE(refractGeo.draws.size() == 1);
    CHECK(refractGeo.draws[0].emitterId == 0);
    REQUIRE(draws.size() == 1);
    CHECK(draws[0].emitterId == 1);
    CHECK(draws[0].material.multiTexture);

    // Nothing lands in the ordinary vertex stream at all, and neither
    // three-UV stream carries the other's vertices.
    CHECK(verts.empty());
    CHECK(refractGeo.vertices.size() == refractGeo.extraUV.size());
    CHECK(multiGeo.vertices.size() == multiGeo.extraUV.size());
    CHECK(multiGeo.vertices.size() == static_cast<usize>(draws[0].vertexCount));
}
