// The particle sim end to end, with no device and no model file.
//
// This is what the viewer's --particle-diff and --childmodel-check run against
// a real actor: emission, integration, death, the geometry build, and the
// child-model Birth/Death protocol. All of it is pure CPU — ParticleService
// never touches the GPU — so a synthetic EmitterDesc stands in for the MDX and
// the same invariants get checked on a machine with no graphics at all.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/effects/splat_service.h"
#include "renderer/particle/base/child_model_emitter.h"
#include "renderer/particle/base/model_particle_emitter.h"
#include "renderer/particle/base/particle_motion.h"
#include "renderer/particle/output/particle_trace.h"
#include "renderer/particle/particle_service.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace whiteout::flakes::renderer::particle;
namespace effects = whiteout::flakes::renderer::effects;
using whiteout::flakes::f32;
using whiteout::flakes::i32;
using whiteout::flakes::u32;
using whiteout::flakes::usize;
using whiteout::flakes::Matrix44f;
using whiteout::flakes::Vector3f;
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
    svc.AddEmitter(model, id, std::move(e));
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
    svc.ForEachEmitter([&](const EmitterKey&, const ParticleEmitter& em) {
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

TEST_CASE("IntegrateWc3 is semi-implicit Euler with the quadratic position term") {
    SECTION("free fall from rest") {
        Particle2 p;
        MotionParams m;
        m.gravity = {0.0f, 0.0f, -10.0f};
        IntegrateWc3(p, m, 0.5f);
        REQUIRE(p.position.z == Approx(-1.25f).margin(1e-6)); // 0.5 * g * dt^2
        REQUIRE(p.velocity.z == Approx(-5.0f).margin(1e-6));  // g * dt
        REQUIRE(p.position.x == 0.0f);
        REQUIRE(p.position.y == 0.0f);
    }
    SECTION("with no gravity it is pure kinematics") {
        Particle2 p;
        p.velocity = {1.0f, 2.0f, 3.0f};
        MotionParams m; // all defaults
        IntegrateWc3(p, m, 0.25f);
        REQUIRE(p.velocity.x == 1.0f);
        REQUIRE(p.velocity.y == 2.0f);
        REQUIRE(p.velocity.z == 3.0f);
        REQUIRE(p.position.x == Approx(0.25f).margin(1e-6));
    }
    SECTION("drag and wind belong to the WoW force model, not this one") {
        // MotionParams carries drag and wind because the WoW integrator reads
        // them; the MDX path has neither to animate. This used to apply an
        // approximation of both — in an order WoW does not actually use — so
        // the terms moved to the dialect that was measured. Asserting the
        // absence here is what stops them drifting back into the WC3 path.
        Particle2 p;
        p.velocity = {4.0f, 0.0f, 0.0f};
        MotionParams m;
        m.drag = 0.5f;
        m.wind = {0.0f, 2.0f, 0.0f};
        IntegrateWc3(p, m, 0.5f);
        REQUIRE(p.velocity.x == 4.0f);
        REQUIRE(p.velocity.y == 0.0f);
    }
}

TEST_CASE("Gravity set on an emitter reaches its particles") {
    ParticleService svc;
    auto* e = AddBillboard(svc, 1, 0, 31337u, MakeDesc());
    e->Motion().gravity = {0.0f, 0.0f, -100.0f};
    Step(svc, {e}, 60);

    f32 lowest = 0.0f;
    svc.ForEachEmitter([&](const EmitterKey&, const ParticleEmitter& em) {
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
    svc.BuildGeometry(TraceView(), {verts, draws});

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
    svc.BuildGeometry(TraceView(), {verts, draws});
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
    svc.BuildGeometry(TraceView(), {verts, draws});
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
    desc->childModelPaths = {"units/human/footman/footman.mdx"};

    ParticleService svc;
    u32 nextHandle = 1;
    auto emitter = std::make_unique<ChildModelEmitter>(1u, 0, [&] { return nextHandle++; });
    emitter->SetDesc(desc);
    Arm(*emitter, 616u);
    ChildModelEmitter* raw = emitter.get();
    svc.AddEmitter(1u, 0, std::move(emitter));

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
    svc.AddEmitter(1u, 0, std::move(emitter));

    for (i32 i = 0; i < 120; ++i) {
        raw->SetVisible(true);
        svc.Simulate(kDt);
    }
    REQUIRE(svc.TotalParticleCount() > 0);

    std::vector<Vertex> verts;
    std::vector<EmitterDrawList> draws;
    svc.BuildGeometry(TraceView(), {verts, draws});
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

// ---------------------------------------------------------------------------
// M2 model particles. Same protocol as PE1's — the sim does not know the two
// apart — so what is worth pinning here is only what differs: the orientation,
// the per-frame size, and the twinkle blink.
// ---------------------------------------------------------------------------

namespace {

// A model-particle desc: the same emitter, with a geometry model and a tumble.
std::shared_ptr<EmitterDesc> MakeModelParticleDesc() {
    auto d = MakeDesc(ParticleOutput::ChildModel);
    d->shape = std::make_shared<ConeShape>();
    d->childModelPaths = {"#12345"};
    return d;
}

f32 RowLength(const Matrix44f& m, i32 row) {
    return std::sqrt(m.data[row][0] * m.data[row][0] + m.data[row][1] * m.data[row][1] +
                     m.data[row][2] * m.data[row][2]);
}

// Rotation-only comparison: the placement matrix is scale * rotation *
// translation, so a basis row carries the scale and has to be normalised before
// two orientations can be compared.
Vector3f UnitRow(const Matrix44f& m, i32 row) {
    const f32 len = RowLength(m, row);
    if (len <= 0.0f)
        return {m.data[row][0], m.data[row][1], m.data[row][2]};
    return {m.data[row][0] / len, m.data[row][1] / len, m.data[row][2] / len};
}

ModelParticleEmitter* AddModelParticles(ParticleService& svc,
                                        std::shared_ptr<const EmitterDesc> desc, u32& nextHandle,
                                        i32 id = 0, u32 seed = 4242u) {
    auto e = std::make_unique<ModelParticleEmitter>(1u, id, [&nextHandle] { return nextHandle++; });
    e->SetDesc(std::move(desc));
    Arm(*e, seed);
    ModelParticleEmitter* raw = e.get();
    svc.AddEmitter(1u, id, std::move(e));
    return raw;
}

} // namespace

TEST_CASE("Model particles balance Birth against Death") {
    // The PE1 invariant suite, re-pointed: an unmatched Birth is a leaked actor
    // and an unmatched Death is one destroyed twice, and the M2 emitter reaches
    // that protocol through two more virtual overrides than PE1 does.
    ParticleService svc;
    u32 nextHandle = 1;
    auto* raw = AddModelParticles(svc, MakeModelParticleDesc(), nextHandle);

    std::set<u32> liveHandles;
    std::set<u32> everBorn;
    i32 births = 0, deaths = 0, transforms = 0;
    std::vector<ChildModelEvent> events;

    for (i32 i = 0; i < 300; ++i) {
        raw->SetVisible(true);
        svc.Simulate(kDt);
        svc.DrainChildModelEvents(events);
        for (const ChildModelEvent& ev : events) {
            REQUIRE(ev.childHandle != 0u);
            switch (ev.kind) {
            case ChildModelEvent::Kind::Birth:
                REQUIRE(everBorn.insert(ev.childHandle).second);
                REQUIRE(liveHandles.insert(ev.childHandle).second);
                ++births;
                break;
            case ChildModelEvent::Kind::Death:
                REQUIRE(liveHandles.erase(ev.childHandle) == 1u);
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
    REQUIRE(births > 0);
    REQUIRE(deaths > 0);
    REQUIRE(transforms > 0);
    REQUIRE(births - deaths == static_cast<i32>(liveHandles.size()));
    REQUIRE(static_cast<i32>(liveHandles.size()) == raw->TotalAlive());
}

TEST_CASE("Model particles contribute nothing to the vertex stream") {
    // Same guarantee PE1 has, and worth re-checking rather than inheriting: an
    // M2 model-particle desc carries a full set of appearance curves, and
    // BuildGeometry skips on the output kind alone.
    ParticleService svc;
    u32 nextHandle = 1;
    auto* raw = AddModelParticles(svc, MakeModelParticleDesc(), nextHandle);

    for (i32 i = 0; i < 120; ++i) {
        raw->SetVisible(true);
        svc.Simulate(kDt);
    }
    REQUIRE(svc.TotalParticleCount() > 0);

    std::vector<Vertex> verts;
    std::vector<EmitterDrawList> draws;
    const Matrix44f view = TraceView();
    svc.BuildGeometry(view, {verts, draws});
    REQUIRE(verts.empty());
    REQUIRE(draws.empty());
}

TEST_CASE("a model particle's placement carries the emitter's scale track") {
    ParticleService svc;
    u32 nextHandle = 1;
    auto* raw = AddModelParticles(svc, MakeModelParticleDesc(), nextHandle);

    std::vector<ChildModelEvent> events;
    // Two lifespans, so the sampled ages span the whole 8 -> 2 size ramp.
    f32 largest = 0.0f, smallest = 1e30f;
    bool meanChecked = false;
    for (i32 i = 0; i < 120; ++i) {
        raw->SetVisible(true);
        svc.Simulate(kDt);
        svc.DrainChildModelEvents(events);
        for (const ChildModelEvent& ev : events) {
            if (ev.kind != ChildModelEvent::Kind::Transform)
                continue;
            const f32 sx = RowLength(ev.transform, 0);
            const f32 sy = RowLength(ev.transform, 1);
            const f32 sz = RowLength(ev.transform, 2);
            largest = (std::max)(largest, sx);
            smallest = (std::min)(smallest, sx);
            // The record has no third scale. The client fills the gap with the
            // mean of X and Y rather than with 1, so a square particle stays
            // cubic instead of flattening.
            REQUIRE(sz == Approx((sx + sy) * 0.5f).margin(1e-3f));
            meanChecked = true;
        }
        events.clear();
    }
    // PE1 would report one fixed `childScale` for every particle at every age.
    INFO("scale spread " << smallest << " .. " << largest);
    REQUIRE(meanChecked);
    REQUIRE(largest > smallest);
    REQUIRE(largest <= Approx(8.0f).margin(1e-3f));
    REQUIRE(smallest >= Approx(2.0f).margin(1e-3f));
}

TEST_CASE("a tumbling model particle turns and a still one does not") {
    ParticleService svc;
    u32 nextHandle = 1;

    auto spinning = MakeModelParticleDesc();
    // One axis, one rate: unambiguous, and X is the only pair the client reads
    // as (min, range) — see ModelParticleEmitter for the other two.
    spinning->tumbleBase = {2.0f, 0.0f, 0.0f};

    auto* still = AddModelParticles(svc, MakeModelParticleDesc(), nextHandle, 0);
    auto* turning = AddModelParticles(svc, spinning, nextHandle, 1);

    std::vector<ChildModelEvent> events;
    std::map<u32, Vector3f> bornRow;
    std::map<u32, i32> bornFrom;
    f32 worstStill = 0.0f, bestSpinning = 0.0f;
    for (i32 i = 0; i < 90; ++i) {
        still->SetVisible(true);
        turning->SetVisible(true);
        svc.Simulate(kDt);
        svc.DrainChildModelEvents(events);
        for (const ChildModelEvent& ev : events) {
            if (ev.kind == ChildModelEvent::Kind::Birth) {
                bornRow[ev.childHandle] = UnitRow(ev.transform, 1);
                bornFrom[ev.childHandle] = ev.emitterId;
                continue;
            }
            if (ev.kind != ChildModelEvent::Kind::Transform)
                continue;
            auto it = bornRow.find(ev.childHandle);
            if (it == bornRow.end())
                continue;
            const Vector3f now = UnitRow(ev.transform, 1);
            const f32 dx = now.x - it->second.x;
            const f32 dy = now.y - it->second.y;
            const f32 dz = now.z - it->second.z;
            const f32 drift = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (bornFrom[ev.childHandle] == 0)
                worstStill = (std::max)(worstStill, drift);
            else
                bestSpinning = (std::max)(bestSpinning, drift);
        }
        events.clear();
    }
    INFO("still drift " << worstStill << ", spinning drift " << bestSpinning);
    // A zero tumble box is below the client's own angular-speed floor, so the
    // orientation is left exactly alone rather than integrated by zero.
    REQUIRE(worstStill == Approx(0.0f).margin(1e-6f));
    REQUIRE(bestSpinning > 0.5f);
}

TEST_CASE("the tumble draw reads the range twice on Y and Z") {
    // Reproduced, not corrected. `CreateParticle(CModelParticle&)` @0x1016a08e0
    // multiplies and then adds the SAME field on Y and Z, where X correctly adds
    // the minimum: `min.x + u*range.x` against `range.y*(1 + u)`. That is
    // observable rather than academic — with a {0, max} box the correct reading
    // lets a particle draw an angular speed of nearly zero, and the client's
    // floor is `max` itself, so under the defect NOTHING is ever still.
    auto desc = MakeModelParticleDesc();
    desc->tumbleBase = {0.0f, 0.0f, 0.0f};
    desc->tumbleVary = {0.0f, 1.0f, 0.0f}; // Y only: one defective axis, alone

    ParticleService svc;
    u32 nextHandle = 1;
    auto* raw = AddModelParticles(svc, desc, nextHandle);

    std::vector<ChildModelEvent> events;
    std::map<u32, Vector3f> bornRow;
    std::map<u32, f32> maxDrift;
    std::map<u32, i32> seen;
    for (i32 i = 0; i < 300; ++i) {
        raw->SetVisible(true);
        svc.Simulate(kDt);
        svc.DrainChildModelEvents(events);
        for (const ChildModelEvent& ev : events) {
            if (ev.kind == ChildModelEvent::Kind::Birth) {
                // Row 0, because the rotation is about Y and leaves row 1 fixed.
                bornRow[ev.childHandle] = UnitRow(ev.transform, 0);
                continue;
            }
            if (ev.kind != ChildModelEvent::Kind::Transform)
                continue;
            auto it = bornRow.find(ev.childHandle);
            if (it == bornRow.end())
                continue;
            ++seen[ev.childHandle];
            const Vector3f now = UnitRow(ev.transform, 0);
            const f32 dx = now.x - it->second.x;
            const f32 dy = now.y - it->second.y;
            const f32 dz = now.z - it->second.z;
            const f32 drift = std::sqrt(dx * dx + dy * dy + dz * dz);
            f32& worst = maxDrift[ev.childHandle];
            worst = (std::max)(worst, drift);
        }
        events.clear();
    }

    // Only particles that lived most of a lifespan: a young one has not had time
    // to turn regardless of how fast it is turning.
    i32 counted = 0;
    f32 leastDrift = 1e30f;
    for (const auto& [handle, frames] : seen) {
        if (frames < 50)
            continue;
        ++counted;
        leastDrift = (std::min)(leastDrift, maxDrift[handle]);
    }
    INFO("counted=" << counted << " least drift=" << leastDrift);
    REQUIRE(counted > 4);
    // 1 rad/s is the defect's floor; over 5/6 s that is a chord of ~0.8. The
    // correct reading would put some particle near zero.
    REQUIRE(leastDrift > 0.7f);
}

TEST_CASE("a model-space model particle is oriented exactly like its emitter") {
    // The one claim the two candidate compositions disagreed on, and it is not
    // arguable: a model-space particle that is not tumbling rides its emitter,
    // so the placement basis IS the emitter basis. Getting this backwards
    // (extracting a column-convention quaternion from row-convention basis
    // vectors) applies the emitter's rotation inverted, which renders plausibly
    // and is wrong — it cost one M2 golden and nothing else would have caught it.
    auto desc = MakeModelParticleDesc();
    desc->modelSpace = true;
    desc->tumbleBase = {0.0f, 0.0f, 0.0f};
    desc->tumbleVary = {0.0f, 0.0f, 0.0f};

    ParticleService svc;
    u32 nextHandle = 1;
    auto* raw = AddModelParticles(svc, desc, nextHandle);

    // A quarter turn about Z, in the row-vector form every emitter matrix here
    // uses, plus a translation the orientation must ignore.
    Matrix44f world = Matrix44f::identity();
    world.data[0][0] = 0.0f;  world.data[0][1] = 1.0f;
    world.data[1][0] = -1.0f; world.data[1][1] = 0.0f;
    world.data[3][0] = 17.0f; world.data[3][1] = -4.0f; world.data[3][2] = 9.0f;
    raw->SetModelToWorld(world);

    std::vector<ChildModelEvent> events;
    i32 checked = 0;
    for (i32 i = 0; i < 30; ++i) {
        raw->SetVisible(true);
        raw->SetModelToWorld(world);
        svc.Simulate(kDt);
        svc.DrainChildModelEvents(events);
        for (const ChildModelEvent& ev : events) {
            if (ev.kind != ChildModelEvent::Kind::Transform)
                continue;
            const Vector3f r0 = UnitRow(ev.transform, 0);
            const Vector3f r1 = UnitRow(ev.transform, 1);
            REQUIRE(r0.x == Approx(0.0f).margin(1e-5f));
            REQUIRE(r0.y == Approx(1.0f).margin(1e-5f));
            REQUIRE(r1.x == Approx(-1.0f).margin(1e-5f));
            REQUIRE(r1.y == Approx(0.0f).margin(1e-5f));
            ++checked;
        }
        events.clear();
    }
    REQUIRE(checked > 0);
}

TEST_CASE("a world-space model particle keeps the emitter basis it was born with") {
    // The other half of the same split: a world-space particle is stamped into
    // the world at birth, so turning the emitter afterwards must not turn it.
    auto desc = MakeModelParticleDesc();
    desc->modelSpace = false;
    desc->tumbleBase = {0.0f, 0.0f, 0.0f};
    desc->tumbleVary = {0.0f, 0.0f, 0.0f};

    ParticleService svc;
    u32 nextHandle = 1;
    auto* raw = AddModelParticles(svc, desc, nextHandle);

    std::vector<ChildModelEvent> events;
    std::map<u32, Vector3f> bornRow;
    f32 worstDrift = 0.0f;
    i32 compared = 0;
    for (i32 i = 0; i < 60; ++i) {
        // A different emitter rotation every frame. A world-space particle must
        // ignore all of them after its own birth frame.
        Matrix44f world = Matrix44f::rotation_z(static_cast<f32>(i) * 0.1f);
        raw->SetModelToWorld(world);
        raw->SetVisible(true);
        svc.Simulate(kDt);
        svc.DrainChildModelEvents(events);
        for (const ChildModelEvent& ev : events) {
            if (ev.kind == ChildModelEvent::Kind::Birth) {
                bornRow[ev.childHandle] = UnitRow(ev.transform, 0);
                continue;
            }
            if (ev.kind != ChildModelEvent::Kind::Transform)
                continue;
            auto it = bornRow.find(ev.childHandle);
            if (it == bornRow.end())
                continue;
            const Vector3f now = UnitRow(ev.transform, 0);
            const f32 dx = now.x - it->second.x;
            const f32 dy = now.y - it->second.y;
            const f32 dz = now.z - it->second.z;
            worstDrift = (std::max)(worstDrift, std::sqrt(dx * dx + dy * dy + dz * dz));
            ++compared;
        }
        events.clear();
    }
    INFO("compared=" << compared << " worst drift=" << worstDrift);
    REQUIRE(compared > 10);
    REQUIRE(worstDrift == Approx(0.0f).margin(1e-5f));
}

TEST_CASE("a starved emitter does not bank the emission it could not spawn") {
    // The pool is sized from the animated lifespan, so a zero one sizes it to
    // nothing and every step is dropped. `EmitNewParticles` @0x1016a5c90 drops
    // that step's carry too — its `m_numNew -= 1` sits OUTSIDE the buffer guard
    // — and banking it instead is what made effects walk away from their model:
    // `carried` sets the first spawn's phase along the emitter's path, so a
    // carry of N places that spawn N emission-periods of travel behind the
    // emitter, growing for as long as the pool stays full.
    auto desc = MakeDesc();
    desc->modelSpace = false;

    Emitter2 e;
    e.SetDesc(desc);
    e.SetBehavior(whiteout::flakes::renderer::core::ParticleBehavior::Wow());
    Arm(e, 12345u);

    // A moving emitter, so the path the spawn walks is non-degenerate.
    auto stateAt = [](i32 frame, f32 life) {
        whiteout::flakes::renderer::model::FrameState::ParticleFrameState st{};
        st.emitterId = 0;
        st.emissionRate = 60.0f;
        st.lifeSpan = life;
        st.visibility = 1.0f;
        st.transform = Matrix44f::identity();
        st.worldPosition = {static_cast<f32>(frame) * 5.0f, 0.0f, 0.0f};
        st.transform.data[3][0] = st.worldPosition.x;
        st.modelAlpha = 1.0f;
        st.enabled = true;
        st.unitScale = 1.0f;
        return st;
    };

    // Phase 1: lifespan 0 sizes the pool to nothing, so 120 frames of emission
    // are asked for and none can be served.
    for (i32 i = 0; i < 120; ++i) {
        e.ApplyState(stateAt(i, 0.0f));
        e.SetVisible(true);
        e.Update(kDt, 1.0f);
    }
    REQUIRE(e.Pool().AliveCount() == 0);

    // Phase 2: a real lifespan grows the pool, and the first particles out of
    // it must appear along THIS frame's travel — not 120 frames behind it.
    const f32 travel = 5.0f;
    for (i32 i = 120; i < 130; ++i) {
        e.ApplyState(stateAt(i, kLife));
        e.SetVisible(true);
        e.Update(kDt, 1.0f);

        const auto& pool = e.Pool();
        for (usize k = 0; k < pool.AliveCount(); ++k) {
            const auto& p = pool[pool.AliveAt(k)];
            if (p.age > 2.0f * kDt)
                continue; // only this frame's births carry the spawn offset
            const f32 dx = p.position.x - e.WorldPosition().x;
            // The spawn walks between the previous pose and this one, so the
            // offset can never exceed one frame of travel plus the emitter's
            // own spawn area.
            CHECK(std::fabs(dx) <= travel + 20.0f);
        }
    }
    CHECK(e.Pool().AliveCount() > 0);
}

TEST_CASE("an SPL splat sweeps its sprite cells the way CSplatKey does", "[particle][splat]") {
    // `CSplatKey::Interpolate` @0x141FE5BE0 over the key @0x141FE73F0 builds.
    // The time is already nudged into [0.005, 0.995].
    const auto sweep = [](i32 start, i32 end, i32 repeat) {
        std::vector<i32> cells;
        for (f32 t : {0.005f, 0.255f, 0.505f, 0.755f, 0.995f})
            cells.push_back(effects::detail::SplatCell(start, end, repeat, t));
        return cells;
    };
    // Ascending: every cell of [0, 3], first to last.
    CHECK(sweep(0, 3, 1) == std::vector<i32>{0, 1, 2, 3, 3});
    // Reversed: the key starts one past `start`, so the sweep opens on cell 3
    // and closes on 0. Starting at `start` itself opened on 2.
    CHECK(sweep(3, 0, 1) == std::vector<i32>{3, 2, 1, 0, 0});
    // Two repeats run the range twice.
    CHECK(sweep(0, 1, 2) == std::vector<i32>{0, 1, 0, 1, 1});
    // Clamped to a byte, not to the range.
    CHECK(effects::detail::SplatCell(250, 300, 1, 0.995f) == 255);
    CHECK(effects::detail::SplatCell(-5, -1, 1, 0.005f) == 0);
}

TEST_CASE("a finished SPL splat stays until its ring overwrites it", "[particle][splat]") {
    // `CSplatEmitter::RenderSplat` @0x141FE6990 keeps drawing a finished splat
    // with its end colour while the end alpha byte is above 8, and only
    // `CreateSplat` @0x141FE58D0 removes one, when a full ring of 1000 per
    // blend mode reuses its slot.
    whiteout::flakes::io::SplEntry e;
    e.lifespan = 1.0f;
    e.decay = 1.0f;
    e.endC[3] = 0.5f;
    e.blendMode = 2;
    e.uvDecayEnd = 0;
    const auto drawn = [](const effects::SplatService& s) {
        std::vector<whiteout::flakes::renderer::Vertex> v;
        std::vector<effects::SplatDrawList> lists;
        s.BuildGeometry(v, lists);
        return v;
    };

    effects::SplatService svc;
    svc.SpawnSpl(e, {0, 0, 0}, {1, 0, 0}, {0, 1, 0});
    for (int i = 0; i < 10; ++i)
        svc.Tick(0.5f); // five seconds, well past lifespan + decay
    CHECK(svc.Count() == 1);
    const auto v = drawn(svc);
    REQUIRE(v.size() == 6);
    CHECK(v[0].color.w == 0.5f);

    SECTION("an end at alpha 8 keeps its slot and draws nothing") {
        whiteout::flakes::io::SplEntry faint = e;
        faint.endC[3] = 8.0f / 255.0f;
        effects::SplatService s2;
        s2.SpawnSpl(faint, {0, 0, 0}, {1, 0, 0}, {0, 1, 0});
        s2.Tick(0.5f);
        s2.Tick(0.5f);
        s2.Tick(0.5f);
        s2.Tick(0.5f);
        CHECK(s2.Count() == 1);
        CHECK(drawn(s2).empty());
    }

    SECTION("a full ring drops its oldest, and only its own blend mode's") {
        whiteout::flakes::io::SplEntry other = e;
        other.blendMode = 1;
        svc.SpawnSpl(other, {0, 0, 0}, {1, 0, 0}, {0, 1, 0});
        for (int i = 0; i < 1000; ++i)
            svc.SpawnSpl(e, {0, 0, 0}, {1, 0, 0}, {0, 1, 0});
        CHECK(svc.Count() == 1001);
        // The finished splat was the oldest of blend mode 2 and is gone; the
        // newborns all draw their start colour, alpha 1.
        const auto all = drawn(svc);
        CHECK(all.size() == 1001 * 6);
        CHECK(std::none_of(all.begin(), all.end(),
                           [](const auto& vtx) { return vtx.color.w == 0.5f; }));
    }
}
