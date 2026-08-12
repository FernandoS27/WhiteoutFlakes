// The particle sim end to end, with no device and no model file.
//
// This is what the viewer's --particle-diff and --childmodel-check run against
// a real actor: emission, integration, death, the geometry build, and the
// child-model Birth/Death protocol. All of it is pure CPU — ParticleService
// never touches the GPU — so a synthetic EmitterDesc stands in for the MDX and
// the same invariants get checked on a machine with no graphics at all.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/particle/child_model_emitter.h"
#include "renderer/particle/particle_motion.h"
#include "renderer/particle/particle_service.h"
#include "renderer/particle/particle_trace.h"

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace whiteout::flakes::renderer::particle;
using whiteout::flakes::f32;
using whiteout::flakes::i32;
using whiteout::flakes::u32;
using whiteout::flakes::usize;
using whiteout::flakes::Matrix44f;
using whiteout::flakes::renderer::Vertex;
using Catch::Approx;

namespace {

constexpr f32 kDt = 1.0f / 60.0f;
constexpr f32 kRate = 20.0f;   // particles per second
constexpr f32 kLife = 1.0f;    // seconds
constexpr i32 kSteady = 20;    // kRate * kLife

// Fixed, non-axis-aligned view so the billboard basis and tail perpendicular
// are both exercised by the geometry build.
Matrix44f TraceView() {
    return Matrix44f::rotation_x(0.4f) * Matrix44f::rotation_y(0.7f);
}

std::shared_ptr<EmitterDesc> MakeDesc(ParticleOutput output = ParticleOutput::Billboard) {
    auto d = std::make_shared<EmitterDesc>();
    d->shape = std::make_shared<PlaneShape>();
    d->output = output;
    d->lifeSpan = kLife;
    d->sheet.Set(1, 1);
    d->curves.color.SetInterp(Interp::Linear);
    d->curves.color.AddKey(0.0f, {1.0f, 1.0f, 1.0f});
    d->curves.color.AddKey(1.0f, {0.0f, 0.0f, 0.0f});
    d->curves.alpha.SetInterp(Interp::Linear);
    d->curves.alpha.AddKey(0.0f, 1.0f);
    d->curves.alpha.AddKey(1.0f, 0.0f);
    d->curves.size.SetInterp(Interp::Linear);
    d->curves.size.AddKey(0.0f, {8.0f, 8.0f});
    d->curves.size.AddKey(1.0f, {2.0f, 2.0f});
    return d;
}

// The animated half — what actor eval writes every frame from the FrameState.
void Arm(Emitter2& e, u32 seed) {
    e.SetSeed(seed);
    e.SetEmissionRate(kRate);
    e.Spawn().width = 10.0f;
    e.Spawn().height = 6.0f;
    e.Spawn().latitude = 0.5f;
    e.Spawn().speed.base = 3.0f;
    e.Spawn().speed.variance = 0.0f;
}

Emitter2* AddBillboard(ParticleService& svc, ModelId model, i32 id, u32 seed,
                       std::shared_ptr<const EmitterDesc> desc) {
    auto e = std::make_unique<Emitter2>();
    e->SetDesc(std::move(desc));
    Arm(*e, seed);
    Emitter2* raw = e.get();
    svc.AddEmitter(model, ParticleOutput::Billboard, id, std::move(e));
    return raw;
}

// Visibility is a per-frame flag: InternalUpdate clears it at the end of every
// update, because actor eval re-asserts it from the model's FrameState.
void Step(ParticleService& svc, const std::vector<Emitter2*>& live, i32 frames) {
    for (i32 i = 0; i < frames; ++i) {
        for (Emitter2* e : live)
            e->SetVisible(true);
        svc.Simulate(kDt);
    }
}

Trace Run(ParticleService& svc, const std::vector<Emitter2*>& live, i32 frames) {
    Trace t;
    const Matrix44f view = TraceView();
    for (i32 i = 0; i < frames; ++i) {
        for (Emitter2* e : live)
            e->SetVisible(true);
        svc.Simulate(kDt);
        CaptureFrame(svc, view, i, t);
    }
    return t;
}

const TraceEmitter* Find(const TraceFrame& f, ModelId model, i32 id) {
    for (const auto& e : f.emitters)
        if (e.model == model && e.emitterId == id)
            return &e;
    return nullptr;
}

} // namespace

TEST_CASE("An emitter settles at the population its rate and lifespan imply") {
    ParticleService svc;
    auto* e = AddBillboard(svc, 1, 0, 1234u, MakeDesc());

    Step(svc, {e}, 240); // four lifespans
    const i32 alive = svc.TotalParticleCount();
    INFO("alive = " << alive);
    REQUIRE(alive > 0);
    REQUIRE(alive >= kSteady - 4);
    // Sync sizes the pool at 1.15x the steady state, and death frees a slot the
    // same frame — an off-by-one in either would show as unbounded growth.
    REQUIRE(alive <= static_cast<i32>(1.15f * kRate * kLife));
}

TEST_CASE("No particle outlives its lifespan") {
    // Death is `age >= lifeSpan` and nothing else. It used to also trigger on
    // running out of curve keys, which killed every particle of a keyless
    // emitter on its first update.
    ParticleService svc;
    auto* e = AddBillboard(svc, 1, 0, 99u, MakeDesc());
    Step(svc, {e}, 300);

    i32 checked = 0;
    svc.ForEachEmitter([&](const EmitterKey&, const Emitter2& em) {
        const ParticlePool& pool = em.Pool();
        for (usize i = 0; i < pool.AliveCount(); ++i) {
            REQUIRE(pool[pool.AliveAt(i)].age < kLife);
            ++checked;
        }
    });
    REQUIRE(checked > 0);
}

TEST_CASE("An invisible emitter emits nothing") {
    ParticleService svc;
    AddBillboard(svc, 1, 0, 7u, MakeDesc());
    for (i32 i = 0; i < 120; ++i)
        svc.Simulate(kDt); // never made visible
    REQUIRE(svc.TotalParticleCount() == 0);
}

TEST_CASE("The emission scaler is per-service") {
    // It used to be a process global, so scaling one viewport's particles
    // silently scaled every other scene's too.
    auto desc = MakeDesc();

    ParticleService full;
    auto* a = AddBillboard(full, 1, 0, 4242u, desc);

    ParticleService halved;
    auto* b = AddBillboard(halved, 1, 0, 4242u, desc);
    halved.SetEmissionScaler(0.5f);

    ParticleService off;
    auto* c = AddBillboard(off, 1, 0, 4242u, desc);
    off.SetEmissionScaler(0.0f);

    Step(full, {a}, 240);
    Step(halved, {b}, 240);
    Step(off, {c}, 240);

    REQUIRE(off.TotalParticleCount() == 0);
    REQUIRE(halved.TotalParticleCount() < full.TotalParticleCount());
    REQUIRE(halved.TotalParticleCount() >= kSteady / 2 - 3);
    REQUIRE(halved.TotalParticleCount() <= kSteady / 2 + 3);
}

TEST_CASE("A squirt is a one-shot burst, independent of visibility") {
    auto desc = MakeDesc();
    desc->emission.squirtAtStart = true;

    ParticleService svc;
    AddBillboard(svc, 1, 0, 5u, desc);

    svc.Simulate(kDt); // no SetVisible — the squirt fires anyway
    REQUIRE(svc.TotalParticleCount() == static_cast<i32>(kRate));

    svc.Simulate(kDt); // and does not re-fire
    REQUIRE(svc.TotalParticleCount() == static_cast<i32>(kRate));
}

TEST_CASE("Integrate is semi-implicit Euler with the quadratic position term") {
    SECTION("free fall from rest") {
        Particle2 p;
        MotionParams m;
        m.gravity = {0.0f, 0.0f, -10.0f};
        Integrate(p, m, 0.5f);
        REQUIRE(p.position.z == Approx(-1.25f).margin(1e-6)); // 0.5 * g * dt^2
        REQUIRE(p.velocity.z == Approx(-5.0f).margin(1e-6));  // g * dt
        REQUIRE(p.position.x == 0.0f);
        REQUIRE(p.position.y == 0.0f);
    }
    SECTION("zero drag and wind are exact no-ops, not approximations") {
        Particle2 p;
        p.velocity = {1.0f, 2.0f, 3.0f};
        MotionParams m; // all defaults: no gravity, no drag, no wind
        Integrate(p, m, 0.25f);
        REQUIRE(p.velocity.x == 1.0f);
        REQUIRE(p.velocity.y == 2.0f);
        REQUIRE(p.velocity.z == 3.0f);
        REQUIRE(p.position.x == Approx(0.25f).margin(1e-6));
    }
    SECTION("drag damps velocity, wind adds to it") {
        Particle2 p;
        p.velocity = {4.0f, 0.0f, 0.0f};
        MotionParams m;
        m.drag = 0.5f;
        m.wind = {0.0f, 2.0f, 0.0f};
        Integrate(p, m, 0.5f);
        REQUIRE(p.velocity.x == Approx(4.0f * (1.0f - 0.25f)).margin(1e-6));
        REQUIRE(p.velocity.y == Approx(1.0f).margin(1e-6));
    }
}

TEST_CASE("Gravity set on an emitter reaches its particles") {
    ParticleService svc;
    auto* e = AddBillboard(svc, 1, 0, 31337u, MakeDesc());
    e->Motion().gravity = {0.0f, 0.0f, -100.0f};
    Step(svc, {e}, 60);

    f32 lowest = 0.0f;
    svc.ForEachEmitter([&](const EmitterKey&, const Emitter2& em) {
        const ParticlePool& pool = em.Pool();
        for (usize i = 0; i < pool.AliveCount(); ++i)
            lowest = std::min(lowest, pool[pool.AliveAt(i)].velocity.z);
    });
    // Spawn speed is 3, so anything past -3 can only have come from gravity.
    REQUIRE(lowest < -3.0f);
}

TEST_CASE("Geometry is one quad per alive particle per enabled end") {
    ParticleService svc;
    auto desc = MakeDesc();
    auto* e = AddBillboard(svc, 1, 0, 808u, desc);
    Step(svc, {e}, 120);

    const i32 alive = svc.TotalParticleCount();
    REQUIRE(alive > 0);

    std::vector<Vertex> verts;
    std::vector<EmitterDrawList> draws;
    svc.BuildGeometry(TraceView(), verts, draws);

    REQUIRE(draws.size() == 1u);
    REQUIRE(draws[0].model == 1u);
    REQUIRE(draws[0].emitterId == 0);
    REQUIRE(draws[0].vertexOffset == 0);
    REQUIRE(draws[0].vertexCount == 6 * alive); // head only: two triangles
    REQUIRE(verts.size() == static_cast<usize>(6 * alive));
}

TEST_CASE("A tailed emitter builds a second quad per particle") {
    auto desc = MakeDesc();
    desc->hasTail = true;
    desc->tailLength = 1.0f;

    ParticleService svc;
    auto* e = AddBillboard(svc, 1, 0, 808u, desc);
    Step(svc, {e}, 120);
    const i32 alive = svc.TotalParticleCount();
    REQUIRE(alive > 0);

    std::vector<Vertex> verts;
    std::vector<EmitterDrawList> draws;
    svc.BuildGeometry(TraceView(), verts, draws);
    REQUIRE(draws[0].vertexCount == 12 * alive);
}

TEST_CASE("An emitter with neither head nor tail builds nothing") {
    auto desc = MakeDesc();
    desc->hasHead = false;

    ParticleService svc;
    auto* e = AddBillboard(svc, 1, 0, 808u, desc);
    Step(svc, {e}, 120);
    REQUIRE(svc.TotalParticleCount() > 0); // the sim still runs

    std::vector<Vertex> verts;
    std::vector<EmitterDrawList> draws;
    svc.BuildGeometry(TraceView(), verts, draws);
    REQUIRE(verts.empty());
    REQUIRE(draws.empty()); // a zero-vertex emitter contributes no draw call
}

TEST_CASE("The whole sim reproduces bit-exactly from a fixed seed") {
    // The property the recorded --particle-diff baselines rest on: same seeds,
    // same dt, same trace, down to the vertex-stream hash.
    auto desc = MakeDesc();

    ParticleService one;
    auto* a1 = AddBillboard(one, 1, 0, 2024u, desc);
    auto* b1 = AddBillboard(one, 1, 1, 77u, desc);

    ParticleService two;
    auto* a2 = AddBillboard(two, 1, 0, 2024u, desc);
    auto* b2 = AddBillboard(two, 1, 1, 77u, desc);

    const Trace first = Run(one, {a1, b1}, 90);
    const Trace second = Run(two, {a2, b2}, 90);

    std::string report;
    REQUIRE(CompareTraces(first, second, CompareTolerance{}, report));
    REQUIRE(report == "identical");
    REQUIRE(first.frames.size() == 90u);
    REQUIRE(first.frames.back().emitters.size() == 2u);
}

TEST_CASE("A different seed produces a different trace") {
    auto desc = MakeDesc();

    ParticleService one;
    auto* a = AddBillboard(one, 1, 0, 2024u, desc);
    ParticleService two;
    auto* b = AddBillboard(two, 1, 0, 2025u, desc);

    const Trace first = Run(one, {a}, 90);
    const Trace second = Run(two, {b}, 90);

    std::string report;
    REQUIRE_FALSE(CompareTraces(first, second, CompareTolerance{}, report));
}

TEST_CASE("One emitter's random stream is not perturbed by another's") {
    // Emitters draw from their own RndSeed, and pool compaction draws from a
    // second stream again — otherwise adding an emitter to a model, or one
    // emitter happening to empty its pool, would shift every other emitter's
    // particles and invalidate the baseline.
    auto desc = MakeDesc();

    ParticleService alone;
    auto* solo = AddBillboard(alone, 1, 0, 4242u, desc);

    ParticleService together;
    auto* same = AddBillboard(together, 1, 0, 4242u, desc);
    auto* other = AddBillboard(together, 1, 1, 909u, desc);

    const Trace soloTrace = Run(alone, {solo}, 90);
    const Trace pairTrace = Run(together, {same, other}, 90);

    REQUIRE(soloTrace.frames.size() == pairTrace.frames.size());
    for (usize f = 0; f < soloTrace.frames.size(); ++f) {
        const TraceEmitter* x = Find(soloTrace.frames[f], 1, 0);
        const TraceEmitter* y = Find(pairTrace.frames[f], 1, 0);
        REQUIRE(x != nullptr);
        REQUIRE(y != nullptr);
        REQUIRE(x->particles.size() == y->particles.size());
        REQUIRE(x->vertexHash == y->vertexHash); // its slice, not the whole stream
        for (usize p = 0; p < x->particles.size(); ++p) {
            REQUIRE(x->particles[p].position.x == y->particles[p].position.x);
            REQUIRE(x->particles[p].position.z == y->particles[p].position.z);
            REQUIRE(x->particles[p].age == y->particles[p].age);
        }
    }
}

TEST_CASE("Child-model particles balance Birth against Death") {
    // The --childmodel-check invariant, at the level where it is actually
    // decided. FrameTicker turns these events into SpawnChild / DestroyActor,
    // so an unmatched Birth is a leaked actor and an unmatched Death is one
    // destroyed twice.
    auto desc = MakeDesc(ParticleOutput::ChildModel);
    desc->shape = std::make_shared<ConeShape>();
    desc->childModelPath = "units/human/footman/footman.mdx";

    ParticleService svc;
    u32 nextHandle = 1;
    auto emitter = std::make_unique<ChildModelEmitter>(1u, 0, [&] { return nextHandle++; });
    emitter->SetDesc(desc);
    Arm(*emitter, 616u);
    ChildModelEmitter* raw = emitter.get();
    svc.AddEmitter(1u, ParticleOutput::ChildModel, 0, std::move(emitter));

    std::set<u32> liveHandles;
    std::set<u32> everBorn;
    i32 births = 0, deaths = 0, transforms = 0;
    std::vector<ChildModelEvent> events;

    for (i32 i = 0; i < 300; ++i) { // five lifespans — long enough to die
        raw->SetVisible(true);
        svc.Simulate(kDt);
        svc.DrainChildModelEvents(events);
        for (const ChildModelEvent& ev : events) {
            REQUIRE(ev.owner == 1u);
            REQUIRE(ev.emitterId == 0);
            REQUIRE(ev.childHandle != 0u);
            switch (ev.kind) {
            case ChildModelEvent::Kind::Birth:
                REQUIRE(everBorn.insert(ev.childHandle).second); // never reissued
                REQUIRE(liveHandles.insert(ev.childHandle).second);
                ++births;
                break;
            case ChildModelEvent::Kind::Death:
                REQUIRE(liveHandles.erase(ev.childHandle) == 1u); // never orphaned
                ++deaths;
                break;
            case ChildModelEvent::Kind::Transform:
                REQUIRE(liveHandles.count(ev.childHandle) == 1u);
                ++transforms;
                break;
            }
        }
        events.clear();
    }

    INFO("births=" << births << " deaths=" << deaths << " transforms=" << transforms);
    REQUIRE(births > 0);  // nothing spawning would make the rest vacuous
    REQUIRE(deaths > 0);  // and nothing dying would too
    REQUIRE(births - deaths == static_cast<i32>(liveHandles.size()));
    REQUIRE(static_cast<i32>(liveHandles.size()) == raw->TotalAlive());
    REQUIRE(transforms > 0);
}

TEST_CASE("Child-model emitters contribute nothing to the vertex stream") {
    auto desc = MakeDesc(ParticleOutput::ChildModel);

    ParticleService svc;
    u32 nextHandle = 1;
    auto emitter = std::make_unique<ChildModelEmitter>(1u, 0, [&] { return nextHandle++; });
    emitter->SetDesc(desc);
    Arm(*emitter, 616u);
    ChildModelEmitter* raw = emitter.get();
    svc.AddEmitter(1u, ParticleOutput::ChildModel, 0, std::move(emitter));

    for (i32 i = 0; i < 120; ++i) {
        raw->SetVisible(true);
        svc.Simulate(kDt);
    }
    REQUIRE(svc.TotalParticleCount() > 0);

    std::vector<Vertex> verts;
    std::vector<EmitterDrawList> draws;
    svc.BuildGeometry(TraceView(), verts, draws);
    REQUIRE(verts.empty());
    REQUIRE(draws.empty());
}

TEST_CASE("Removing a model drops exactly its emitters") {
    auto desc = MakeDesc();
    ParticleService svc;
    auto* a = AddBillboard(svc, 1, 0, 1u, desc);
    auto* b = AddBillboard(svc, 2, 0, 2u, desc);
    Step(svc, {a, b}, 60);

    REQUIRE(svc.EmitterCount() == 2);
    REQUIRE(svc.HasEmittersForModel(1));

    svc.RemoveModel(1);
    REQUIRE(svc.EmitterCount() == 1);
    REQUIRE_FALSE(svc.HasEmittersForModel(1));
    REQUIRE(svc.HasEmittersForModel(2));
    REQUIRE(svc.GetEmitter(2, ParticleOutput::Billboard, 0) == b);
    REQUIRE(svc.GetEmitter(1, ParticleOutput::Billboard, 0) == nullptr);
}
