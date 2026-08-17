// Ribbon simulation — the shared core and the two dialects that sit on it.
//
// Warcraft III and World of Warcraft run the same ribbon code, evolved. That
// claim is not stylistic: WoW 6.0.1.18179's `InitInterpDeltas` @0x100e7c0b0 /
// `InterpEdge` @0x100e7c340 evaluate the same blend the MDX path here already
// ran, and `Update` @0x100e7dab0 emits the same `floor(endTime-1)+1` edges at
// the same parameter. So the interesting tests are of two kinds:
//
//   * WC3 regression — the MDX numbers, pinned. A ribbon's shape is a curve
//     nobody eyeballs correctly, and the WC3 path had no coverage at all
//     before the simulation moved into a shared service. These cases fail if
//     the refactor moved a single vertex.
//   * dialect divergence — the handful of places the two genuinely differ,
//     each asserted against what the disassembly showed.
//
// The third kind is the one that would be a mistake to leave out: that the
// SHARED half really is shared. If WC3 and WoW ever stop agreeing on the
// interpolation curve, that is a bug in this file's premise, not a feature.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "renderer/ribbon/ribbon_emitter.h"
#include "renderer/ribbon/ribbon_service.h"

#include <cmath>

using namespace whiteout::flakes::renderer::ribbon;
using whiteout::flakes::f32;
using whiteout::flakes::i32;
using whiteout::flakes::Matrix44f;
using whiteout::flakes::Vector3f;
using Catch::Approx;

namespace {

RibbonDesc MakeDesc() {
    RibbonDesc d;
    d.edgesPerSecond = 10.0f;
    d.edgeLifespan = 1.0f;
    d.gravity = 0.0f;
    d.rows = 1;
    d.cols = 1;
    return d;
}

RibbonState StateAt(const Vector3f& p, f32 above = 10.0f, f32 below = 10.0f) {
    RibbonState st;
    st.transform = Matrix44f::translation(p);
    st.above = above;
    st.below = below;
    st.alpha = 1.0f;
    st.color = {1, 1, 1};
    st.visibility = 1.0f;
    st.slot = 0;
    return st;
}

// Walk the emitter along +X at a fixed step, one Update per step.
void Sweep(RibbonEmitter& em, i32 steps, f32 dt, f32 dx, i32 slot = 0) {
    for (i32 i = 0; i < steps; ++i) {
        RibbonState st = StateAt({dx * static_cast<f32>(i + 1), 0, 0});
        st.slot = slot;
        em.SetState(st);
        em.Update(dt);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// WC3 regression — the MDX path's own numbers
// ---------------------------------------------------------------------------

TEST_CASE("wc3 ribbon seeds prev and curr from the first state", "[ribbon]") {
    RibbonEmitter em(MakeDesc(), RibbonBehavior::Wc3());
    REQUIRE_FALSE(em.PositionSeeded());
    em.SetState(StateAt({0, 0, 0}));
    REQUIRE(em.PositionSeeded());

    // Nothing is emitted before the first Update, and an unseeded emitter
    // cannot emit at all — the client's `posSet` gate.
    REQUIRE(em.Edges().empty());
}

TEST_CASE("wc3 ribbon emits a head every frame plus the rate's edges", "[ribbon]") {
    RibbonEmitter em(MakeDesc(), RibbonBehavior::Wc3());
    em.SetState(StateAt({0, 0, 0}));

    // 10 edges/sec at dt = 1/60 buys 0.1667 of an edge per frame, so the
    // emission loop contributes nothing for the first five frames. The MDX
    // path still appends a head each frame and KEEPS it, which is why the
    // count tracks the frame rate rather than the emission rate.
    for (i32 i = 0; i < 5; ++i) {
        em.SetState(StateAt({0.1f * static_cast<f32>(i + 1), 0, 0}));
        em.Update(1.0f / 60.0f);
    }
    REQUIRE(em.Edges().size() == 5);

    // Sixth frame: the carry crosses 1.0, so one interpolated edge lands too.
    em.SetState(StateAt({0.6f, 0, 0}));
    em.Update(1.0f / 60.0f);
    REQUIRE(em.Edges().size() == 7);
}

TEST_CASE("wc3 ribbon retires edges past the lifespan", "[ribbon]") {
    RibbonDesc d = MakeDesc();
    d.edgeLifespan = 0.5f;
    RibbonEmitter em(d, RibbonBehavior::Wc3());
    em.SetState(StateAt({0, 0, 0}));

    Sweep(em, 20, 0.1f, 1.0f);
    for (const auto& e : em.Edges())
        REQUIRE(e.age < 0.5f);
    const std::size_t afterTwoSeconds = em.Edges().size();

    // The property worth pinning is that the trail is BOUNDED by the lifespan
    // rather than by how long the emitter has run: twice the runtime, same
    // length. Asserting a particular count instead would pin the frame rate.
    Sweep(em, 20, 0.1f, 1.0f);
    REQUIRE(em.Edges().size() == afterTwoSeconds);
}

TEST_CASE("wc3 ribbon restarts the trail when dt exceeds the lifespan", "[ribbon]") {
    RibbonDesc d = MakeDesc();
    d.edgeLifespan = 0.3f;
    RibbonEmitter em(d, RibbonBehavior::Wc3());
    em.SetState(StateAt({0, 0, 0}));
    Sweep(em, 10, 1.0f / 60.0f, 0.1f);
    REQUIRE(em.Edges().size() > 1);

    // A hitch longer than the lifespan: the MDX path throws the whole trail
    // away and re-seeds at the current pose rather than retiring edge by edge.
    em.SetState(StateAt({5, 0, 0}));
    em.Update(0.4f);
    REQUIRE(em.Edges().empty());
}

TEST_CASE("wc3 ribbon uses the 0.25s lifespan floor", "[ribbon]") {
    RibbonDesc d = MakeDesc();
    d.edgeLifespan = 0.05f;
    RibbonEmitter em(d, RibbonBehavior::Wc3());
    // Below the floor the MDX path simulates and UV-maps at 0.25s, which is
    // what stops a very short-lived ribbon collapsing to a degenerate strip.
    REQUIRE(em.SimLifespan() == Approx(0.25f));

    RibbonEmitter wow(d, RibbonBehavior::Wow());
    // WoW applies the same 0.25 only where InitEdges sizes the ring; the
    // simulation and the UV mapping use the raw lifespan. Measured, not
    // assumed: gate_ribbon_edges.py, 250/250 against the binary.
    REQUIRE(wow.SimLifespan() == Approx(0.05f));
}

TEST_CASE("wc3 ribbon gravity integrates as g*t^2, not 0.5*g*t^2", "[ribbon]") {
    RibbonDesc d = MakeDesc();
    d.gravity = 10.0f;
    d.edgeLifespan = 100.0f;
    d.edgesPerSecond = 1.0f;
    RibbonEmitter em(d, RibbonBehavior::Wc3());
    em.SetState(StateAt({0, 0, 0}, 0.0f, 0.0f));

    // One edge, then let it fall for a known total time in uneven steps: the
    // per-step form is g*dt^2 + 2*g*age*dt, which telescopes to g*t^2 only if
    // the age term is present. A half-gravity integrator lands at half this.
    em.SetState(StateAt({1, 0, 0}, 0.0f, 0.0f));
    em.Update(1.0f);
    REQUIRE(em.Edges().size() >= 1);

    const f32 startZ = em.Edges()[0].top.z;
    const f32 startAge = em.Edges()[0].age;
    for (i32 i = 0; i < 4; ++i) {
        em.SetState(StateAt({2.0f + static_cast<f32>(i), 0, 0}, 0.0f, 0.0f));
        em.Update(0.25f);
    }
    const f32 endAge = em.Edges()[0].age;
    const f32 fell = startZ - em.Edges()[0].top.z; // WC3 subtracts from z
    REQUIRE(fell == Approx(d.gravity * (endAge * endAge - startAge * startAge)).epsilon(1e-4));
}

TEST_CASE("wc3 ribbon builds two triangles per edge pair", "[ribbon]") {
    RibbonEmitter em(MakeDesc(), RibbonBehavior::Wc3());
    em.SetState(StateAt({0, 0, 0}));
    Sweep(em, 4, 1.0f / 60.0f, 0.1f);

    std::vector<whiteout::flakes::renderer::Vertex> verts;
    const i32 n = em.BuildStrip(verts);
    REQUIRE(n == static_cast<i32>(em.Edges().size() - 1) * 6);
    REQUIRE(n == em.VertexCount());
    REQUIRE(static_cast<i32>(verts.size()) == n);

    // The strip is a ribbon, not a fan: every quad's first and fourth vertices
    // are the same lower corner, which is what makes the two triangles share
    // an edge instead of overlapping.
    REQUIRE(verts[1].position.x == Approx(verts[3].position.x));
    REQUIRE(verts[1].position.z == Approx(verts[3].position.z));
}

TEST_CASE("wc3 ribbon maps u from age over the lifespan", "[ribbon]") {
    RibbonDesc d = MakeDesc();
    d.cols = 2;
    d.rows = 2;
    RibbonEmitter em(d, RibbonBehavior::Wc3());
    RibbonState st = StateAt({0, 0, 0});
    st.slot = 3; // row 1, col 1 of a 2x2 sheet
    em.SetState(st);
    Sweep(em, 6, 1.0f / 60.0f, 0.1f, 3);

    std::vector<whiteout::flakes::renderer::Vertex> verts;
    em.BuildStrip(verts);
    REQUIRE_FALSE(verts.empty());
    // Slot 3 of a 2x2 grid starts at (0.5, 0.5) and the head edge is age 0, so
    // the newest column sits exactly on the cell's left edge.
    REQUIRE(verts.back().uv.x == Approx(0.5f));
    REQUIRE(verts[0].uv.y == Approx(0.5f));
}

TEST_CASE("wc3 ribbon holds when invisible and when the rate is zero", "[ribbon]") {
    RibbonEmitter em(MakeDesc(), RibbonBehavior::Wc3());
    RibbonState st = StateAt({0, 0, 0});
    st.visibility = 0.0f;
    em.SetState(st);
    em.Update(1.0f / 60.0f);
    REQUIRE(em.Edges().empty());

    RibbonDesc zero = MakeDesc();
    zero.edgesPerSecond = 0.0f;
    RibbonEmitter noRate(zero, RibbonBehavior::Wc3());
    noRate.SetState(StateAt({0, 0, 0}));
    noRate.SetState(StateAt({1, 0, 0}));
    noRate.Update(1.0f / 60.0f);
    REQUIRE(noRate.Edges().empty());
}

// ---------------------------------------------------------------------------
// The shared half really is shared
// ---------------------------------------------------------------------------

TEST_CASE("both dialects agree on the interpolation curve", "[ribbon]") {
    // Same desc, same poses, same dt, and a dt/rate pair chosen so both emit
    // the same number of interpolated edges on the same frame. The positions
    // must match exactly: InterpEdge and the MDX blend are the same
    // expression, so any difference here means one of them was rewritten.
    RibbonDesc d = MakeDesc();
    d.edgesPerSecond = 4.0f;
    d.edgeLifespan = 10.0f;

    RibbonBehavior wowCurveOnly = RibbonBehavior::Wow();
    // Hold the first-frame dt substitution off. It is a real divergence and it
    // has its own test below; leaving it on here would feed the two emitters
    // different dt and compare curves sampled at different parameters, which
    // says nothing about the curve itself. The remaining WoW flags do not
    // touch a single moving step's interpolated edges.
    wowCurveOnly.firstFrameEmitsOneEdge = false;

    RibbonEmitter wc3(d, RibbonBehavior::Wc3());
    RibbonEmitter wow(d, wowCurveOnly);
    for (RibbonEmitter* em : {&wc3, &wow}) {
        em->SetState(StateAt({0, 0, 0}));
        em->SetState(StateAt({1, 2, 3}));
        em->Update(0.5f);
    }

    const auto& a = wc3.Edges();
    const auto& b = wow.Edges();
    // Two interpolated edges (endTime = 2.0) plus each side's head.
    REQUIRE(a.size() == 3);
    REQUIRE(b.size() == 3);
    const std::size_t n = a.size();
    for (std::size_t i = 0; i < n; ++i) {
        REQUIRE(a[i].top.x == Approx(b[i].top.x));
        REQUIRE(a[i].top.y == Approx(b[i].top.y));
        REQUIRE(a[i].top.z == Approx(b[i].top.z));
        REQUIRE(a[i].bot.x == Approx(b[i].bot.x));
        REQUIRE(a[i].bot.y == Approx(b[i].bot.y));
        REQUIRE(a[i].bot.z == Approx(b[i].bot.z));
        REQUIRE(a[i].age == Approx(b[i].age));
    }
}

// ---------------------------------------------------------------------------
// Dialect divergences, each one measured
// ---------------------------------------------------------------------------

TEST_CASE("wow ribbon commits exactly one edge on its first tick", "[ribbon]") {
    RibbonDesc d = MakeDesc();
    d.edgesPerSecond = 10.0f;
    RibbonEmitter em(d, RibbonBehavior::Wow());
    em.SetState(StateAt({0, 0, 0}));
    em.SetState(StateAt({1, 0, 0}));

    // dt is replaced by 1/edgesPerSecond + 1e-4 regardless of what is passed,
    // so the first frame lays one interpolated edge plus the provisional head.
    em.Update(1.0f / 600.0f);
    REQUIRE(em.Edges().size() == 2);
}

TEST_CASE("wow ribbon head is provisional and does not accumulate", "[ribbon]") {
    RibbonDesc d = MakeDesc();
    d.edgesPerSecond = 10.0f;
    d.edgeLifespan = 100.0f;
    RibbonEmitter wow(d, RibbonBehavior::Wow());
    RibbonEmitter wc3(d, RibbonBehavior::Wc3());
    wow.SetState(StateAt({0, 0, 0}));
    wc3.SetState(StateAt({0, 0, 0}));

    // 60 frames at 1/60s with a 10/sec rate: ten edges' worth of emission.
    Sweep(wow, 60, 1.0f / 60.0f, 0.1f);
    Sweep(wc3, 60, 1.0f / 60.0f, 0.1f);

    // WoW's trail tracks the emission rate — ~10 committed edges plus the one
    // provisional head. Its first tick spends a full edge period, so it is one
    // frame's worth ahead; the point is the order of magnitude, not the exact
    // count.
    REQUIRE(wow.Edges().size() <= 14);
    // The MDX path keeps every per-frame head, so its trail tracks the FRAME
    // rate instead: six times as many edges for the same motion. This is the
    // divergence with the largest practical consequence, and it is asserted
    // rather than fixed, because changing it would change every WC3 ribbon.
    REQUIRE(wc3.Edges().size() >= 60);
}

TEST_CASE("wow ribbon stops emitting while stationary", "[ribbon]") {
    RibbonDesc d = MakeDesc();
    d.edgeLifespan = 100.0f;
    RibbonEmitter em(d, RibbonBehavior::Wow());
    em.SetState(StateAt({0, 0, 0}));
    em.SetState(StateAt({1, 0, 0}));
    em.Update(1.0f / 60.0f);
    const std::size_t moved = em.Edges().size();

    // Hold the pose. InitInterpDeltas returns 0 below 0.001 units of travel,
    // which suppresses the emission loop — but NOT the head, which is still
    // re-placed, so the count holds steady instead of growing.
    for (i32 i = 0; i < 30; ++i) {
        em.SetState(StateAt({1, 0, 0}));
        em.Update(1.0f / 60.0f);
    }
    REQUIRE(em.Edges().size() == moved);

    // The MDX path has no such check and keeps stacking coincident edges.
    RibbonEmitter wc3(d, RibbonBehavior::Wc3());
    wc3.SetState(StateAt({0, 0, 0}));
    wc3.SetState(StateAt({1, 0, 0}));
    wc3.Update(1.0f / 60.0f);
    const std::size_t before = wc3.Edges().size();
    for (i32 i = 0; i < 30; ++i) {
        wc3.SetState(StateAt({1, 0, 0}));
        wc3.Update(1.0f / 60.0f);
    }
    REQUIRE(wc3.Edges().size() > before);
}

TEST_CASE("wow ribbon gravity falls the other way", "[ribbon]") {
    RibbonDesc d = MakeDesc();
    d.gravity = 10.0f;
    d.edgeLifespan = 100.0f;
    RibbonEmitter em(d, RibbonBehavior::Wow());
    em.SetState(StateAt({0, 0, 0}, 0.0f, 0.0f));
    em.SetState(StateAt({1, 0, 0}, 0.0f, 0.0f));
    em.Update(0.5f);
    const f32 startZ = em.Edges()[0].top.z;
    for (i32 i = 0; i < 4; ++i) {
        em.SetState(StateAt({2.0f + static_cast<f32>(i), 0, 0}, 0.0f, 0.0f));
        em.Update(0.25f);
    }
    // Update adds the step to z where the MDX path subtracts it.
    REQUIRE(em.Edges()[0].top.z > startZ);
}

TEST_CASE("wow ribbon clamps dt instead of restarting the trail", "[ribbon]") {
    RibbonDesc d = MakeDesc();
    d.edgeLifespan = 0.3f;
    RibbonEmitter em(d, RibbonBehavior::Wow());
    em.SetState(StateAt({0, 0, 0}));
    Sweep(em, 10, 1.0f / 60.0f, 0.1f);
    REQUIRE_FALSE(em.Edges().empty());

    em.SetState(StateAt({5, 0, 0}));
    em.Update(0.4f);
    // dt is clamped to the lifespan, so the trail retires normally rather than
    // being thrown away — the head at minimum survives.
    REQUIRE_FALSE(em.Edges().empty());
}

// ---------------------------------------------------------------------------
// The service around it
// ---------------------------------------------------------------------------

TEST_CASE("ribbon service keys emitters by model and id", "[ribbon]") {
    RibbonService svc;
    svc.AddEmitter(1, 0, MakeDesc(), RibbonBehavior::Wc3());
    svc.AddEmitter(1, 1, MakeDesc(), RibbonBehavior::Wc3());
    svc.AddEmitter(2, 0, MakeDesc(), RibbonBehavior::Wow());

    REQUIRE(svc.EmitterCount() == 3);
    REQUIRE(svc.HasEmittersForModel(1));
    REQUIRE(svc.HasEmittersForModel(2));
    REQUIRE_FALSE(svc.HasEmittersForModel(3));

    // Two models can hold different dialects at once, which is the whole point
    // of the behaviour living on the emitter rather than on a global.
    REQUIRE_FALSE(svc.GetEmitter(1, 0)->Behavior().headEdgeIsProvisional);
    REQUIRE(svc.GetEmitter(2, 0)->Behavior().headEdgeIsProvisional);

    svc.RemoveModel(1);
    REQUIRE(svc.EmitterCount() == 1);
    REQUIRE_FALSE(svc.HasEmittersForModel(1));
    REQUIRE(svc.GetEmitter(1, 0) == nullptr);
}

TEST_CASE("ribbon service simulates one model at a time", "[ribbon]") {
    RibbonService svc;
    svc.AddEmitter(1, 0, MakeDesc(), RibbonBehavior::Wc3());
    svc.AddEmitter(2, 0, MakeDesc(), RibbonBehavior::Wc3());
    svc.SetState(1, 0, StateAt({0, 0, 0}));
    svc.SetState(2, 0, StateAt({0, 0, 0}));
    svc.SetState(1, 0, StateAt({1, 0, 0}));
    svc.SetState(2, 0, StateAt({1, 0, 0}));

    svc.SimulateModel(1, 1.0f / 60.0f);
    REQUIRE(svc.EdgeCountForModel(1) > 0);
    // Model 2 was not ticked, so the visibility gate FrameTicker applies per
    // actor really does isolate one model's trail from another's.
    REQUIRE(svc.EdgeCountForModel(2) == 0);
}

TEST_CASE("ribbon service builds geometry per model with local offsets", "[ribbon]") {
    RibbonService svc;
    svc.AddEmitter(7, 0, MakeDesc(), RibbonBehavior::Wc3());
    svc.AddEmitter(7, 1, MakeDesc(), RibbonBehavior::Wc3());
    for (i32 id : {0, 1}) {
        svc.SetState(7, id, StateAt({0, 0, 0}));
        for (i32 i = 0; i < 5; ++i) {
            svc.SetState(7, id, StateAt({0.1f * static_cast<f32>(i + 1), 0, 0}));
            svc.SimulateModel(7, 1.0f / 60.0f);
        }
    }

    std::vector<whiteout::flakes::renderer::Vertex> verts;
    std::vector<RibbonDrawList> lists;
    svc.BuildGeometry(7, verts, lists);

    REQUIRE(lists.size() == 2);
    // Offsets are relative to this call, because the pipeline uploads each
    // actor into its own vertex buffer and binds it before the draw.
    REQUIRE(lists[0].vertexOffset == 0);
    REQUIRE(lists[1].vertexOffset == lists[0].vertexCount);
    REQUIRE(lists[0].vertexCount + lists[1].vertexCount == static_cast<i32>(verts.size()));
    // Emitter order is the map's, so the draw-order tie-break stays a function
    // of the id rather than of a hash.
    REQUIRE(lists[0].emitterId == 0);
    REQUIRE(lists[1].emitterId == 1);
}

TEST_CASE("ribbon desc carries the mdx config through unchanged", "[ribbon]") {
    whiteout::flakes::renderer::effects::RibbonEmitterConfig cfg;
    cfg.textureId = 4;
    cfg.filterMode = 3;
    cfg.rows = 2;
    cfg.cols = 4;
    cfg.unshaded = true;
    cfg.twoSided = false;
    cfg.emission = 25.0f;
    cfg.life = 0.75f;
    cfg.gravity = -3.0f;
    cfg.priorityPlane = 2;

    const RibbonDesc d = DescFromWc3Config(cfg);
    REQUIRE(d.textureId == 4);
    REQUIRE(d.filterMode == 3);
    REQUIRE(d.rows == 2);
    REQUIRE(d.cols == 4);
    REQUIRE(d.unshaded);
    REQUIRE_FALSE(d.twoSided);
    REQUIRE(d.edgesPerSecond == Approx(25.0f));
    REQUIRE(d.edgeLifespan == Approx(0.75f));
    REQUIRE(d.gravity == Approx(-3.0f));
    REQUIRE(d.priorityPlane == 2);
}

// ---------------------------------------------------------------------------
// Model units vs renderer units.
//
// The simulation runs on positions the actor's transform has already scaled,
// but `above`/`below` and `gravity` come off the `.m2` record unscaled. Across
// the corpus those heights run 0.04..6.9 model units (mean 0.6), so at the wow
// profile's 100 an unscaled ribbon draws roughly 1% of its authored width — a
// sub-pixel sliver against a model 200+ renderer units tall. The independent
// check on the factor is WC3's own authoring: its half-widths are ~20 renderer
// units, which is what 0.22 model units becomes.
// ---------------------------------------------------------------------------

TEST_CASE("a ribbon's half-widths are model units and scale to renderer units") {
    const f32 kAbove = 0.25f, kBelow = 0.75f;

    auto widthAfterSweep = [&](f32 unitScale) {
        RibbonEmitter em(MakeDesc(), RibbonBehavior::Wow());
        for (i32 i = 0; i < 4; ++i) {
            RibbonState st = StateAt({static_cast<f32>(i) * 10.0f, 0, 0}, kAbove, kBelow);
            st.unitScale = unitScale;
            em.SetState(st);
            em.Update(0.1f);
        }
        REQUIRE_FALSE(em.Edges().empty());
        // The edge straddles the emitter's vertical axis, which for a pure
        // translation is +Y — so measure the separation, not a chosen component.
        const auto& e = em.Edges().front();
        const f32 dx = e.top.x - e.bot.x, dy = e.top.y - e.bot.y, dz = e.top.z - e.bot.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };

    SECTION("unscaled, as MDX leaves it") {
        CHECK(widthAfterSweep(1.0f) == Approx(kAbove + kBelow));
    }
    SECTION("at the wow profile's 100") {
        CHECK(widthAfterSweep(100.0f) == Approx((kAbove + kBelow) * 100.0f));
    }
}

TEST_CASE("ribbon gravity is model units per second squared too") {
    // Same record value under two scales: the fall has to differ by the scale,
    // not stay put. Gravity lives on the desc, which is built at load time with
    // no actor to ask, so this is the reason the factor rides the frame state.
    auto fallAfter = [](f32 unitScale) {
        RibbonDesc d = MakeDesc();
        d.gravity = -2.0f;
        RibbonEmitter em(d, RibbonBehavior::Wow());
        for (i32 i = 0; i < 3; ++i) {
            RibbonState st = StateAt({static_cast<f32>(i) * 10.0f, 0, 0}, 0.25f, 0.25f);
            st.unitScale = unitScale;
            em.SetState(st);
            em.Update(0.1f);
        }
        REQUIRE_FALSE(em.Edges().empty());
        return em.Edges().front().top.z;
    };

    const f32 one = fallAfter(1.0f);
    const f32 hundred = fallAfter(100.0f);
    // The vertical axis of a pure translation is +Y, so top.z carries the fall
    // and nothing else. Scaled gravity makes it linear in the factor; an
    // unscaled one would leave both runs falling by the same absolute amount.
    CHECK(one < 0.0f);
    CHECK(hundred == Approx(one * 100.0f));
}
