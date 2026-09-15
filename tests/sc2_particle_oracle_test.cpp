// ============================================================================
// The SC2 particle module measured against the ORIGINAL rather than against
// its own description: `tools/sc2_particle_oracle/` runs the real StarCraft II
// 4.8 client under Unicorn and records what it computed; this replays those
// goldens. No emulator and no binary needed to run it — that is the point.
//
// The first two citizens are the two generators everything downstream stands
// on. If `sc2::Rng` is not draw-for-draw the engine's, every spawn gate after
// it becomes an argument about tolerances instead of an equality; if the noise
// table is not the engine's table, the offsets a Euler emitter applies at BUILD
// are plausible noise rather than THE noise.
// ============================================================================

#include "oracle_golden.h"
#include "renderer/particle/particle_adapters.h"
#include "renderer/particle/particle_stages_sc2.h"
#include "renderer/particle/sc2_compose.h"
#include "renderer/particle/sc2_tick.h"
#include "renderer/sc2/sc2_element.h"
#include "renderer/sc2/sc2_element_math.h"
#include "renderer/sc2/sc2_rng.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace whiteout::flakes;
namespace sc2 = whiteout::flakes::renderer::sc2;
namespace vs = whiteout::flakes::renderer::sc2::vs;
namespace particle = whiteout::flakes::renderer::particle;
namespace effects = whiteout::flakes::renderer::effects;
namespace fs = std::filesystem;

namespace {

fs::path GoldenDir() {
    if (const char* v = std::getenv("WDX_SC2_PARTICLE_GOLDEN"); v && *v)
        return fs::path(v);
#ifdef WDX_SC2_PARTICLE_GOLDEN_DIR
    return fs::path(WDX_SC2_PARTICLE_GOLDEN_DIR);
#else
    return {};
#endif
}

/// The gradient normalise runs through `rsqrtss`, whose seed is a hardware
/// approximation here and correctly rounded under Unicorn — so the gradients
/// and everything computed from them carry this bound, and nothing else does.
constexpr f32 kRsqrtRtol = 2e-6f;

void CloseRel(f32 got, f32 want, f32 rtol, const std::string& what) {
    const f32 tol = rtol * (std::max)(1.0f, std::fabs(want));
    INFO(what << " got=" << got << " want=" << want);
    REQUIRE(std::fabs(got - want) <= tol);
}

} // namespace

// ---------------------------------------------------------------------------
// OP0 — the generator.
// ---------------------------------------------------------------------------

TEST_CASE("op0: the RNG table is the image's 256 bytes",
          "[sc2_particle][oracle][op0]") {
    const fs::path path = GoldenDir() / "op0_rngtable.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& bytes = (*doc)["cases"][0]["out"]["bytes"];
    REQUIRE(bytes.Size() == sc2::kRngTable.size());
    for (std::size_t i = 0; i < bytes.Size(); ++i) {
        INFO("byte " << i);
        REQUIRE(int(sc2::kRngTable[i]) == bytes[i].I());
    }
}

TEST_CASE("op0: the generator replays draw for draw", "[sc2_particle][oracle][op0]") {
    const fs::path path = GoldenDir() / "op0_rng.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0);

    std::size_t replayed = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& out = c["out"];
        const std::string tag = c["tag"].S();

        sc2::Rng rng(in["acc"].U(), in["idx4"].U());
        INFO("case " << i << " tag=" << tag);

        if (tag == "next_u32") {
            const auto& words = out["words"].A();
            for (std::size_t k = 0; k < words.size(); ++k) {
                INFO("draw " << k);
                REQUIRE(rng.NextU32() == words[k]->U());
            }
        } else if (tag == "range_f") {
            // Bit-exact: RangeF is integer bit twiddling plus three float ops,
            // with no square root anywhere.
            const auto& vals = out["floats"].A();
            const f32 a = in["a"].F(), b = in["b"].F();
            for (std::size_t k = 0; k < vals.size(); ++k) {
                INFO("draw " << k);
                REQUIRE(rng.RangeF(a, b) == vals[k]->F());
            }
        } else if (tag == "range_i32" || tag == "range_u16") {
            // One kernel at two addresses — the same 214 bytes of code, so one
            // C++ function under two names is the transcription, not a merge.
            const auto& vals = out["ints"].A();
            const u32 lo = in["lo"].U(), hi = in["hi"].U();
            for (std::size_t k = 0; k < vals.size(); ++k) {
                INFO("draw " << k);
                REQUIRE(rng.RangeInt(lo, hi) == vals[k]->U());
            }
        } else {
            FAIL("unmodelled op0 tag: " << tag);
        }

        // The state at the far end, which is what lets a gate downstream hand
        // its own recorded state in and continue the same stream.
        REQUIRE(rng.acc() == out["accOut"].U());
        REQUIRE(rng.idx4() == out["idx4Out"].U());
        ++replayed;
    }
    REQUIRE(replayed == cases.Size());
}

// ---------------------------------------------------------------------------
// OP2 — the noise table and its two samplers.
// ---------------------------------------------------------------------------

TEST_CASE("op2: the seed-0 noise table regenerates the engine's",
          "[sc2_particle][oracle][op2]") {
    const fs::path path = GoldenDir() / "op2_noisetable.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& out = (*doc)["cases"][0]["out"];

    // The arrays are private and widening the class for a test would be the
    // tail wagging the dog, so the table is checked through the two samplers —
    // which is also the only way anything reads it. This case pins the two
    // structural claims a sampler grid cannot see on its own: the permutation
    // is a permutation, and the gradients are unit.
    const auto& perm = out["perm"];
    REQUIRE(perm.Size() == 256);
    std::vector<int> seen;
    seen.reserve(256);
    for (std::size_t i = 0; i < perm.Size(); ++i)
        seen.push_back(perm[i].I());
    std::sort(seen.begin(), seen.end());
    for (int i = 0; i < 256; ++i)
        REQUIRE(seen[static_cast<std::size_t>(i)] == i);

    const auto& grad2 = out["grad2"];
    REQUIRE(grad2.Size() == 512);
    for (std::size_t i = 0; i < 256; ++i) {
        const f32 x = grad2[2 * i].F(), y = grad2[2 * i + 1].F();
        INFO("grad2 " << i);
        REQUIRE(std::fabs((x * x + y * y) - 1.0f) <= kRsqrtRtol);
    }

    // grad1 is the raw draw and is deliberately NOT normalised — the 1-D
    // sampler leans on that, and a "tidying" normalise would pass every unit
    // check above while changing every 1-D value.
    const auto& grad1 = out["grad1"];
    REQUIRE(grad1.Size() == 256);
    bool anyNonUnit = false;
    for (std::size_t i = 0; i < grad1.Size(); ++i) {
        const f32 v = grad1[i].F();
        REQUIRE(v >= -1.0f);
        REQUIRE(v < 1.0f);
        anyNonUnit = anyNonUnit || std::fabs(std::fabs(v) - 1.0f) > 0.01f;
    }
    REQUIRE(anyNonUnit);
}

TEST_CASE("op2: the samplers replay the recorded grid", "[sc2_particle][oracle][op2]") {
    const fs::path path = GoldenDir() / "op2_noise.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    const sc2::NoiseTable& table = sc2::GlobalNoiseTable();

    bool sawGrid = false, sawPhases = false;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const std::string tag = c["tag"].S();

        if (tag == "noise2d") {
            sawGrid = true;
            const auto& xs = c["in"]["xs"].A();
            const auto& ys = c["in"]["ys"].A();
            const auto& want = c["out"]["samples"].A();
            REQUIRE(want.size() == xs.size() * ys.size() * 3);
            std::size_t k = 0;
            for (const auto& xv : xs) {
                for (const auto& yv : ys) {
                    f32 got[3] = {0, 0, 0};
                    table.Sample(xv->F(), yv->F(), got);
                    for (int row = 0; row < 3; ++row, ++k)
                        CloseRel(got[row], want[k]->F(), kRsqrtRtol,
                                 "x=" + std::to_string(xv->F()) + " y=" +
                                     std::to_string(yv->F()) + " row=" + std::to_string(row));
                }
            }
        } else if (tag == "noise1d") {
            sawPhases = true;
            const auto& phases = c["in"]["phases"].A();
            const auto& want = c["out"]["values"].A();
            REQUIRE(want.size() == phases.size());
            for (std::size_t k = 0; k < phases.size(); ++k) {
                // grad1 never went through rsqrt, so the 1-D sampler is
                // bit-exact where the 2-D one carries the gradient tolerance.
                INFO("phase " << phases[k]->F());
                REQUIRE(table.Sample1D(phases[k]->F()) == want[k]->F());
            }
        }
    }
    REQUIRE(sawGrid);
    REQUIRE(sawPhases);
}

// ---------------------------------------------------------------------------
// OP1 — the motion split.
// ---------------------------------------------------------------------------

TEST_CASE("op1: CanUseGpuMotion replays the binary's truth table",
          "[sc2_particle][oracle][op1]") {
    const fs::path path = GoldenDir() / "op1_gpumotion.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 90);

    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];

        // Through the CONFIG, not by hand-filling the desc: the conversion is
        // half of what this gate is for. A `Sc2CanUseGpuMotion` that agreed
        // with the binary while `DescFromSc2ParticleConfig` fed it the wrong
        // force word would pass a desc-only test and ship the wrong integrator.
        effects::Sc2ParticleEmitterConfig cfg;
        cfg.instanceType = static_cast<u8>(in["instanceType"].I());
        cfg.forces = static_cast<u32>(in["localForces"].I()) |
                     (static_cast<u32>(in["worldForces"].I()) << 16);
        cfg.forcesFallback = static_cast<u32>(in["localForcesFallback"].I()) |
                             (static_cast<u32>(in["worldForcesFallback"].I()) << 16);
        cfg.flags = in["flags"].U();
        cfg.windMultiplier = in["windMultiplier"].F();
        cfg.killRadius = in["killRadius"].F();
        cfg.noiseAmplitude = in["noiseAmplitude"].F();

        const auto desc = particle::DescFromSc2ParticleConfig(cfg, {});
        INFO("case " << i << " tag=" << c["tag"].S() << " instanceType=" << int(cfg.instanceType)
                     << " flags=0x" << std::hex << cfg.flags << std::dec
                     << " wind=" << cfg.windMultiplier << " kill=" << cfg.killRadius
                     << " noise=" << cfg.noiseAmplitude);
        REQUIRE(desc->sc2.motion.analytic == c["out"]["analytic"].B());
    }
}

// ---------------------------------------------------------------------------
// OP10 — the interpolation modes and the control-point conversion.
// ---------------------------------------------------------------------------

TEST_CASE("op10: smoothstep is the shipped polynomial", "[sc2_particle][oracle][op10]") {
    const fs::path path = GoldenDir() / "op10_smoothstep.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& c = (*doc)["cases"][0];
    const auto& ts = c["in"]["ts"].A();
    const auto& want = c["out"]["values"].A();
    REQUIRE(ts.size() == want.size());
    for (std::size_t i = 0; i < ts.size(); ++i) {
        INFO("t=" << ts[i]->F());
        REQUIRE(vs::SmoothStep(ts[i]->F()) == want[i]->F());
    }
}

TEST_CASE("op10: the five interpolation modes replay bit-exact",
          "[sc2_particle][oracle][op10]") {
    const fs::path path = GoldenDir() / "op10_curve1d.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 240);

    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const int mode = in["mode"].I();
        const f32 mid = in["mid"].F();
        const f32 hold = in["hold"].F();
        const auto& keys = in["keys"].A();
        const auto& ts = in["ts"].A();
        const auto& want = c["out"]["values"].A();
        REQUIRE(keys.size() == 3);
        REQUIRE(ts.size() == want.size());
        // The reciprocal is the CALLER's, computed once before the switch and
        // handed in — which is exactly why mode 0's lower piece multiplies by
        // it while its upper piece divides. Recomputing `1/mid` per branch
        // would be algebraically identical and numerically different.
        const f32 invMid = 1.0f / mid;
        for (std::size_t k = 0; k < ts.size(); ++k) {
            INFO("case " << i << " mode=" << mode << " mid=" << mid << " hold=" << hold
                         << " t=" << ts[k]->F());
            REQUIRE(vs::InterpolateValue(ts[k]->F(), keys[0]->F(), keys[1]->F(), keys[2]->F(), mid,
                                         invMid, hold, mode) == want[k]->F());
        }
    }
}

TEST_CASE("op10: the mid key converts to the same control point",
          "[sc2_particle][oracle][op10]") {
    const fs::path path = GoldenDir() / "op10_convert.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    std::size_t scalars = 0, packed = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const std::string tag = c["tag"].S();
        INFO("case " << i << " tag=" << tag << " mid=" << in["midTime"].F());
        if (tag == "scalar") {
            const auto& keys = in["keys"].A();
            f32 k[3] = {keys[0]->F(), keys[1]->F(), keys[2]->F()};
            sc2::ConvertColorNode(k, in["midTime"].F());
            const auto& want = c["out"]["converted"].A();
            for (int j = 0; j < 3; ++j) {
                INFO("channel " << j);
                REQUIRE(k[j] == want[static_cast<std::size_t>(j)]->F());
            }
            ++scalars;
        } else if (tag == "bgra") {
            const u32 got = sc2::ConvertColorNode3(in["c0"].U(), in["c1"].U(), in["c2"].U(),
                                                   in["midTime"].F());
            REQUIRE(got == c["out"]["converted"].U());
            ++packed;
        }
    }
    REQUIRE(scalars > 0);
    REQUIRE(packed > 0);
}

// ---------------------------------------------------------------------------
// OP3 / OP3b / OP7a - the emit clock and the sub-step split.
//
// These three are one chain: the clock decides how many sub-steps a frame gets,
// ComputeEmitCount decides how much each slot wants, and EmitParticles decides
// when inside the frame that want is spent. Replaying only the arithmetic would
// miss the ordering, which is where both of this phase's transcription errors
// were - a folded scalar that cost an ulp, and a bit cleared on consumption.
// ---------------------------------------------------------------------------

TEST_CASE("op3: the emit clock replays Tick's whole argument tuple",
          "[sc2_particle][oracle][op3]") {
    const fs::path path = GoldenDir() / "op3_tick.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 80);

    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& want = c["out"];

        particle::Sc2EmitClock clock;
        clock.emitterTime = in["emitterTime"].F();
        clock.variationTime = in["variationTime"].F();
        clock.lastSubStepTime = in["lastSubStepTime"].F();
        clock.lastFrameIndex = in["lastFrameIndex"].I();
        clock.lastTimeMs = in["lastTimeMs"].I();
        clock.prevPos = {in["prevPos"][0].F(), in["prevPos"][1].F(), in["prevPos"][2].F()};
        clock.stateFlags = in["flags"].U();

        particle::Sc2ClockInputs args;
        args.dtMs = in["dtMs"].I();
        args.timeScale = in["timeScale"].F();
        args.subStepRate = in["subStepRate"].F();
        args.timeOffset = in["timeOffset"].F();
        args.nowMs = in["nowMs"].I();
        args.frameIndex = in["frameIndex"].I();
        args.worldPos = {in["worldPos"][0].F(), in["worldPos"][1].F(), in["worldPos"][2].F()};
        args.modelPaused = in["modelPaused"].I() != 0;
        args.force60Hz = in["force60Hz"].I() != 0;

        // The restart check first, as `Tick` makes it: ahead of the
        // once-per-frame test, so a frame that returns early still runs it.
        const auto restart = particle::Sc2TickRestartCheck(clock, args);
        const auto plan = particle::Sc2TickClock(clock, args);
        INFO("case " << i << " tag=" << c["tag"].S());

        const auto& ep = want["emitParticles"];
        REQUIRE(restart.owed == ep["restart"].B());
        REQUIRE(plan.ticked == ep["called"].B());
        if (plan.ticked) {
            REQUIRE(plan.fullStep == (ep["fullStep"].I() != 0));
            REQUIRE(plan.nSteps == ep["nSteps"].U());
            REQUIRE(plan.subDt == ep["subDt"].F());
            REQUIRE(plan.dt == ep["dt"].F());
            REQUIRE(plan.catchUp == ep["catchUp"].F());
            REQUIRE(plan.remainder == ep["remainder"].F());
            REQUIRE(plan.displacement.x == ep["displacement"][0].F());
            REQUIRE(plan.displacement.y == ep["displacement"][1].F());
            REQUIRE(plan.displacement.z == ep["displacement"][2].F());
        }

        const auto& st = want["state"];
        REQUIRE(clock.emitterTime == st["emitterTime"].F());
        REQUIRE(clock.variationTime == st["variationTime"].F());
        REQUIRE(clock.lastSubStepTime == st["lastSubStepTime"].F());
        REQUIRE(clock.lastFrameIndex == st["lastFrameIndex"].I());
        REQUIRE(clock.lastTimeMs == st["lastTimeMs"].I());
        REQUIRE(clock.stateFlags == st["stateFlags"].U());
        REQUIRE(clock.prevPos.x == st["prevPos"][0].F());
        REQUIRE(clock.prevPos.y == st["prevPos"][1].F());
        REQUIRE(clock.prevPos.z == st["prevPos"][2].F());
    }
}

TEST_CASE("op7a: ComputeEmitCount replays the LOD tables and the burst gates",
          "[sc2_particle][oracle][op7a]") {
    const fs::path path = GoldenDir() / "op7a_emitcount.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 190);

    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];

        // For a `PARC` copy the flags that gate the burst come from ITS record,
        // not the `PAR_`'s. Without that field the two slot vectors carry
        // identical inputs and different answers - the golden was re-recorded
        // to say which, rather than the replay guessing.
        const bool copy = in["slot"].I() > 0;
        const auto& flagsNode = copy ? in["parcSquirtFlags"] : in["squirtFlags"];
        const unsigned flags = flagsNode.IsNull() ? 0u : flagsNode.U();

        // The keys as the sink holds them — u16 — summed by the kernel the
        // crossing shares with `ComputeEmitCount`, which reads them back
        // SIGNED and drops the negative ones.
        f32 burst = 0.0f;
        if (in["allowSquirt"].B() && (flags & 2) != 0) {
            particle::Sc2KeySink sink;
            for (const auto& k : in["squirtKeys"].A())
                sink.keys[sink.count++] = {static_cast<u16>(static_cast<i16>(k->I())), 0};
            burst = particle::Sc2SquirtBurst(sink);
        }

        particle::Sc2EmitCountInputs args;
        args.rate = in["rate"].F();
        args.dt = in["dt"].F();
        args.timeScale = in["timeScale"].F();
        args.burst = burst;
        args.lodCut = in["lodCut"].I();
        args.lodReduce = in["lodReduce"].I();
        args.quality = in["quality"].I();
        args.elemScaleX = in["elemScaleX"].F();
        args.suppressed = in["renderState"].I() == 14 || in["enabled"].I() == 0 ||
                          (in["emitFlags"].U() & 0x20) != 0;
        args.nodeVisible = in["nodeVisible"].B();

        INFO("case " << i << " tag=" << c["tag"].S() << " slot=" << in["slot"].I());
        REQUIRE(particle::Sc2ComputeEmitCount(args) == c["out"]["count"].F());
    }
}

TEST_CASE("op3b: the sub-step split replays EmitParticles' event log",
          "[sc2_particle][oracle][op3b]") {
    const fs::path path = GoldenDir() / "op3b_emitparticles.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 75);

    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& want = c["out"];

        const auto& rates = in["rates"].A();
        const std::size_t slots = rates.size();
        // The recorder's stub returned `rate * dt`, and the dt it was handed is
        // the window — the same one `Sc2ComputeEmitCount` applies internally.
        const f32 window = particle::Sc2EmitWindow(in["fullStep"].I() != 0,
                                                   in["subDt"].F(), in["frameDt"].F());
        std::vector<f32> counts(slots);
        std::vector<f32> carry(slots);
        std::vector<u32> targets(slots, 0u);
        for (std::size_t s = 0; s < slots; ++s) {
            counts[s] = rates[s]->F() * window;
            carry[s] = in["emitFracIn"][s].F();
        }

        particle::Sc2ScheduleInputs args;
        args.fullStep = in["fullStep"].I() != 0;
        args.nSubSteps = in["nSubSteps"].U();
        args.subDt = in["subDt"].F();
        args.frameDt = in["frameDt"].F();
        args.catchUp = in["catchUp"].F();
        args.remainder = in["remainder"].F();
        args.accumTime = in["accumTimeIn"].F();
        args.frozen = in["elementState"].I() != 0;
        args.anythingAlive = in["elementCount"].I() != 0 || in["aliveTest"].I() != 0;
        args.haveRequests = in["spawnRequestCount"].I() != 0;
        args.counts = counts;

        std::vector<particle::Sc2EmitEvent> events;
        const auto out = particle::Sc2SpawnSchedule(args, carry, targets, events);
        INFO("case " << i << " tag=" << c["tag"].S());

        // The trampolines recorded WHICH call happened and in what order; a
        // schedule that spawns the right totals in the wrong order is a
        // different frame, so the log is compared before the numbers.
        const auto& log = want["log"].A();
        REQUIRE(events.size() == log.size());
        std::vector<u32> emitted(slots, 0u);
        for (std::size_t e = 0; e < events.size(); ++e) {
            const std::string& ev = (*log[e])["ev"].S();
            INFO("  event " << e << " want=" << ev);
            switch (events[e].kind) {
            case particle::Sc2EmitEventKind::Count:
                REQUIRE(ev == "count");
                REQUIRE(events[e].slot == (*log[e])["slot"].U());
                break;
            case particle::Sc2EmitEventKind::PreEmit:
                REQUIRE(ev == "preemit");
                break;
            case particle::Sc2EmitEventKind::Spawn:
                REQUIRE(ev == "spawn");
                REQUIRE(events[e].slot == (*log[e])["slot"].U());
                emitted[events[e].slot] += events[e].count;
                break;
            case particle::Sc2EmitEventKind::Update:
                REQUIRE(ev == "update");
                break;
            }
        }

        for (std::size_t s = 0; s < slots; ++s) {
            INFO("  slot " << s);
            REQUIRE(targets[s] == want["emitCounts"][s].U());
            REQUIRE(emitted[s] == want["emittedCounts"][s].U());
            REQUIRE(carry[s] == want["emitFrac"][s].F());
        }
        REQUIRE(out.accumTime == want["accumTime"].F());
        // The reciprocal lane is an rcpps estimate plus one Newton step, so it
        // carries the golden's own tolerance rather than an equality.
        CloseRel(out.spawnTimeStep, want["spawnTimeStep"].F(), 2e-6f, "spawnTimeStep");

        // The position lane. This replay once stopped at the time lane, and the
        // tick never made the `prevPos` write the golden records.
        particle::Sc2SweepInputs sw;
        sw.fullStep = args.fullStep;
        sw.nSubSteps = args.nSubSteps;
        sw.total = out.total;
        sw.prevPos = {in["prevPosIn"][0].F(), in["prevPosIn"][1].F(), in["prevPosIn"][2].F()};
        sw.worldPos = {in["worldPos"][0].F(), in["worldPos"][1].F(), in["worldPos"][2].F()};
        const particle::Sc2Sweep sweep = particle::Sc2SpawnSweep(sw);
        REQUIRE(sweep.prevPos.x == want["prevPos"][0].F());
        REQUIRE(sweep.prevPos.y == want["prevPos"][1].F());
        REQUIRE(sweep.prevPos.z == want["prevPos"][2].F());
        CloseRel(sweep.spawnPosStep.x, want["spawnPosStep"][0].F(), 2e-6f, "spawnPosStep.x");
        CloseRel(sweep.spawnPosStep.y, want["spawnPosStep"][1].F(), 2e-6f, "spawnPosStep.y");
        CloseRel(sweep.spawnPosStep.z, want["spawnPosStep"][2].F(), 2e-6f, "spawnPosStep.z");
    }
}

// ---------------------------------------------------------------------------
// OP7b - the squirt crossing; OP3c / OP8b - the pre-roll and the batch order.
// ---------------------------------------------------------------------------

namespace {

i32 AddWrap32(i32 a, i32 b) {
    return static_cast<i32>(static_cast<u32>(a) + static_cast<u32>(b));
}

/// `cvttss2si`: NaN and anything out of range land on the integer indefinite.
i32 TruncI32(f32 v) {
    if (!(v >= -2147483648.0f && v < 2147483648.0f))
        return static_cast<i32>(0x80000000u);
    return static_cast<i32>(v);
}

/// A player's frame as `M3Anim_CollectCrossedKeys` builds it: the two
/// offsets, the scale, the one-sided saturation and the loop modulo.
i32 KeyPlayerFrame(const wdx_golden::Value& p, i32 duration) {
    const i32 delta = static_cast<i32>(static_cast<u32>(p["now"].I()) -
                                       static_cast<u32>(p["timeOffsetMs"].I()) -
                                       static_cast<u32>(p["timeBiasMs"].I()));
    const f32 scaled = static_cast<f32>(delta) * p["timeScale"].F();
    const i32 base = AddWrap32(p["frameOffsetB"].I(), p["frameOffsetA"].I());
    const i32 whole = TruncI32(scaled);
    i32 frame = AddWrap32(base, whole);
    if (frame < 0)
        frame = 0x7FFFFFFF;
    if (scaled <= 0.0f || base < 0)
        frame = AddWrap32(base, whole);
    if ((p["flags"].U() & 1u) != 0 && duration > 0)
        frame %= duration;
    return frame;
}

/// The reported time: the frame formula run backwards, dividing where the
/// forward one multiplied.
i32 KeyTimeMs(const wdx_golden::Value& p, i32 frame) {
    const i32 delta = static_cast<i32>(static_cast<u32>(frame) -
                                       static_cast<u32>(p["frameOffsetB"].I()) -
                                       static_cast<u32>(p["frameOffsetA"].I()));
    const f32 ms = static_cast<f32>(delta) / p["timeScale"].F();
    return static_cast<i32>(static_cast<u32>(TruncI32(ms)) +
                            static_cast<u32>(p["timeOffsetMs"].I()) +
                            static_cast<u32>(p["timeBiasMs"].I()));
}

enum class KeyPart : u8 { Forward, Tail, Head };

struct RefKey {
    u16 value = 0;
    i32 frame = 0;
    /// An entry an earlier frame left, kept by the early-out.
    bool stale = false;
    std::size_t player = 0;
    u32 index = 0;
    KeyPart part = KeyPart::Forward;
    i32 then = 0;
    i32 now = 0;
    u32 last = 0;
};

struct RefPlayer {
    std::vector<i32> times;
    std::vector<u16> values;
    i32 frame = 0;
};

struct RefSink {
    std::vector<RefKey> keys;
    std::vector<i32> cursors;
    bool resync = false;
    bool prime = false;
};

/// The bisection both of retail's searches run: `(lo + hi) >> 1` from lo = 0,
/// stopping on an exact hit and answering `hi` otherwise — so it never lands
/// on index 0.
u32 RetailBisect(const std::vector<i32>& t, i32 target, u32 last) {
    u32 lo = 0;
    u32 hi = last;
    u32 k = last;
    do {
        const u32 mid = (hi + lo) >> 1;
        k = mid;
        if (t[mid] <= target) {
            lo = mid;
            if (t[mid] >= target)
                return k;
        } else {
            hi = mid;
        }
        k = hi;
    } while (hi - lo > 1);
    return k;
}

/// `M3Anim_CollectCrossedKeys` transcribed from the 4.8 decompile, with its
/// searches switchable. `quirks` on is the binary, which has to be the golden;
/// off is design §8's rule, which has to be the kernel.
bool ReferenceCrossing(const std::vector<RefPlayer>& players, i32 bias, RefSink& sink,
                       bool quirks) {
    constexpr std::size_t kCap = 128;
    if (sink.cursors.size() < players.size())
        sink.cursors.resize(players.size(), 0);
    if (!sink.resync && !sink.prime) {
        bool moved = false;
        for (std::size_t i = 0; i < players.size() && !moved; ++i)
            moved = sink.cursors[i] != AddWrap32(players[i].frame, 1);
        if (!moved)
            return false;
    }
    sink.keys.clear();
    for (std::size_t i = 0; i < players.size(); ++i) {
        const RefPlayer& p = players[i];
        const i32 now = AddWrap32(p.frame, bias);
        if (sink.prime && !sink.resync) {
            sink.cursors[i] = now;
            continue;
        }
        if (sink.resync)
            sink.cursors[i] = now;
        const i32 then = sink.cursors[i];
        const std::vector<i32>& t = p.times;
        const u32 n = static_cast<u32>(t.size());
        const u32 last = n == 0 ? 0u : n - 1u;
        const auto report = [&](u32 k, KeyPart part) {
            sink.keys.push_back({p.values[k], t[k], false, i, k, part, then, now, last});
            return sink.keys.size() < kCap;
        };
        if (n != 0 && sink.keys.size() < kCap) {
            bool enter = true;
            u32 k = 0;
            if (!quirks)
                k = static_cast<u32>(std::lower_bound(t.begin(), t.end(), then) - t.begin());
            else if (t[0] > then)
                k = 0;
            else if (t[last] >= then)
                k = last >= 2 ? RetailBisect(t, then, last) : last;
            else
                enter = false;
            for (; enter && k < n; ++k) {
                const i32 tk = t[k];
                const bool inWindow = now >= then ? (tk <= now && tk >= then)
                                      : quirks    ? (tk <= then && tk >= now)
                                                  : tk >= then;
                if (!inWindow || !report(k, now >= then ? KeyPart::Forward : KeyPart::Tail))
                    break;
            }
            if (now < then && t[0] <= now && sink.keys.size() < kCap) {
                u32 end;
                if (!quirks)
                    end = static_cast<u32>(std::upper_bound(t.begin(), t.end(), now) - t.begin());
                else if (last < 2 || t[last] < now)
                    end = last;
                else
                    end = RetailBisect(t, now, last);
                for (u32 h = 0; h < end && t[h] <= now; ++h)
                    if (!report(h, KeyPart::Head))
                        break;
            }
        }
        if (!sink.prime)
            sink.cursors[i] = AddWrap32(now, 1);
    }
    sink.resync = false;
    sink.prime = false;
    return true;
}

} // namespace

TEST_CASE("op7b: the squirt crossing reports every key the playhead stepped over",
          "[sc2_particle][oracle][op7b]") {
    const fs::path path = GoldenDir() / "op7b_crossedkeys.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    // The sink starts where retail's constructor leaves the part anyone reads:
    // nothing counted, room for 128.
    const fs::path ctorPath = GoldenDir() / "op7b_batchctor.json";
    if (fs::exists(ctorPath)) {
        const auto ctor = wdx_golden::Load(ctorPath.string());
        const auto& o = (*ctor)["cases"][0]["out"];
        const particle::Sc2KeySink fresh;
        CHECK(fresh.count == o["count"].U());
        CHECK(particle::Sc2KeySink::kCapacity == o["cap"].U());
    }

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 95);

    // Index 0 on the cursor, a wrap's tail, a wrap's head at now, its last key.
    unsigned losses[4] = {};
    unsigned rebuilt = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& want = c["out"];
        INFO("case " << i << " tag=" << c["tag"].S());

        // The live players in list order. A skipped player, an unbound ref, a
        // missing track and a zero duration all drop out WITHOUT a cursor slot.
        std::vector<RefPlayer> live;
        std::vector<const wdx_golden::Value*> specs;
        const bool bound = in["slot"].U() != 0xFFFFu;
        for (const auto& pp : in["players"].A()) {
            const wdx_golden::Value& p = *pp;
            if (!bound || (p["flags"].U() & 0x4000u) != 0)
                continue;
            const wdx_golden::Value* track = nullptr;
            for (const auto& tt : in["tracks"].A())
                if ((*tt)["stc"].U() == p["stc"].U())
                    track = &*tt;
            if (track == nullptr || (*track)["duration"].I() == 0)
                continue;
            RefPlayer rp;
            for (const auto& t : (*track)["times"].A())
                rp.times.push_back(t->I());
            for (const auto& v : (*track)["values"].A())
                rp.values.push_back(static_cast<u16>(v->U()));
            rp.frame = KeyPlayerFrame(p, (*track)["duration"].I());
            live.push_back(std::move(rp));
            specs.push_back(&p);
        }

        std::vector<i32> cursors;
        for (const auto& cur : in["cursorsIn"].A())
            cursors.push_back(cur->I());
        const i32 bias = in["timeBias"].I();
        const bool resync = in["resync"].I() != 0;
        const bool prime = in["prime"].I() != 0;
        const u32 pre = in["preCount"].U();
        // The gate's poisoned entries are {0xBEEF, -1}, standing in for an
        // earlier frame's keys.
        const auto seed = [&](RefSink& s) {
            s.cursors = cursors;
            s.resync = resync;
            s.prime = prime;
            s.keys.assign(pre, RefKey{0xBEEF, -1, true});
        };

        // The binary, transcribed, against what it recorded.
        RefSink retail;
        seed(retail);
        ReferenceCrossing(live, bias, retail, true);
        const auto& entries = want["entries"];
        REQUIRE(retail.keys.size() == want["count"].U());
        REQUIRE(entries.Size() == retail.keys.size());
        for (std::size_t k = 0; k < retail.keys.size(); ++k) {
            const RefKey& key = retail.keys[k];
            CHECK(key.value == entries[k]["value"].U());
            CHECK((key.stale ? -1 : KeyTimeMs(*specs[key.player], key.frame)) ==
                  entries[k]["timeMs"].I());
        }
        for (std::size_t k = 0; k < cursors.size(); ++k)
            CHECK(retail.cursors[k] == want["cursors"][k].I());

        // The kernel against the rule.
        RefSink ideal;
        seed(ideal);
        const bool refRebuilt = ReferenceCrossing(live, bias, ideal, false);
        std::vector<particle::Sc2KeyPlayer> players;
        for (const RefPlayer& rp : live)
            players.push_back({{rp.times, rp.values}, rp.frame});
        particle::Sc2KeySink sink;
        sink.cursors = cursors;
        sink.resync = resync;
        sink.prime = prime;
        for (u32 k = 0; k < pre; ++k)
            sink.keys[sink.count++] = {0xBEEF, -1};
        const bool kernelRebuilt = particle::Sc2CollectCrossedKeys(players, bias, sink);
        CHECK(kernelRebuilt == refRebuilt);
        rebuilt += kernelRebuilt ? 1u : 0u;
        REQUIRE(sink.count == ideal.keys.size());
        for (std::size_t k = 0; k < ideal.keys.size(); ++k) {
            CHECK(sink.keys[k].value == ideal.keys[k].value);
            CHECK(sink.keys[k].frame == ideal.keys[k].frame);
        }
        for (std::size_t k = 0; k < cursors.size(); ++k)
            CHECK(sink.cursors[k] == ideal.cursors[k]);

        // What the binary reported, the rule reports too, in order; and each
        // key the binary lost is one of the four named losses. Not asked once
        // the rule reaches the 128 cap, where the two keep different 128.
        if (ideal.keys.size() < particle::Sc2KeySink::kCapacity) {
            std::size_t r = 0;
            for (const RefKey& k : ideal.keys) {
                if (r < retail.keys.size() && retail.keys[r].stale == k.stale &&
                    retail.keys[r].player == k.player && retail.keys[r].index == k.index &&
                    retail.keys[r].part == k.part) {
                    ++r;
                    continue;
                }
                INFO("lost key " << k.index << " at " << k.frame << " then=" << k.then
                                 << " now=" << k.now);
                bool named = false;
                if (k.part != KeyPart::Head && k.index == 0 && k.frame == k.then) {
                    ++losses[0];
                    named = true;
                }
                if (k.part == KeyPart::Tail && k.frame > k.then) {
                    ++losses[1];
                    named = true;
                }
                if (k.part == KeyPart::Head && k.frame == k.now) {
                    ++losses[2];
                    named = true;
                }
                if (k.part == KeyPart::Head && k.index == k.last) {
                    ++losses[3];
                    named = true;
                }
                CHECK(named);
            }
            CHECK(r == retail.keys.size());
        }
    }
    // Each named loss has a recorded vector behind it; otherwise the deviation
    // would be a claim about code nobody ran.
    CHECK(losses[0] > 0u);
    CHECK(losses[1] > 0u);
    CHECK(losses[2] > 0u);
    CHECK(losses[3] > 0u);
    // And the early-out is in the vectors on both sides.
    CHECK(rebuilt > 0u);
    CHECK(rebuilt < cases.Size());
}

TEST_CASE("op3c: the pre-roll runs the lifetime peak in 33 ms blocks",
          "[sc2_particle][oracle][op3c]") {
    const fs::path path = GoldenDir() / "op3c_emitburst.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 70);

    unsigned deviated = 0;
    unsigned ran = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& want = c["out"];
        INFO("case " << i << " tag=" << c["tag"].S());

        // No resolved sequence, no pre-roll: `EmitBurst` returns -1 untouched.
        if (in["activeSequenceIndex"].U() == 0xFFFFFFFFu) {
            CHECK(want["steps"].U() == 0u);
            CHECK(want["ret"].U() == 0xFFFFFFFFu);
            continue;
        }

        // The randomised AnimRef under additionalFlags & 2, its curve when it
        // has one, its raw init value when it does not.
        const bool useRandom = in["useRandom"].I() != 0;
        const u32 interp = useRandom ? in["randomInterp"].U() : in["lifetimeInterp"].U();
        const f32 init = useRandom ? in["randomInit"].F() : in["lifetimeInit"].F();
        const bool haveCurve = interp != 0xFFFFu && !in["curveMissing"].B();
        std::vector<f32> curve;
        for (const auto& v : in["values"].A())
            curve.push_back(v->F());
        const f32 peak = particle::Sc2PreRollPeak(curve, haveCurve, init);

        // The binary's budget, to show what the golden is: its clamp reads the
        // wall clock before it reads the request.
        const u32 wantMs = static_cast<u32>(TruncI32(peak * 1000.0f));
        const u32 requested = in["requested"].U();
        const u32 nowMs = in["nowMs"].U();
        const u32 retailBudget = wantMs > nowMs ? wantMs : (std::min)(wantMs, requested);
        const u32 retailBlocks = retailBudget == 0 ? 0u : (retailBudget + 32u) / 33u;
        REQUIRE(want["steps"].U() == retailBlocks);

        const particle::Sc2PreRollPlan plan = particle::Sc2PlanPreRoll(peak, requested);
        if (wantMs > nowMs && requested < wantMs) {
            // Design §8: the request caps the budget whatever the clock says.
            ++deviated;
            CHECK(plan.budgetMs == requested);
            CHECK(plan.blocks <= retailBlocks);
        } else {
            CHECK(plan.budgetMs == retailBudget);
            CHECK(plan.blocks == retailBlocks);
        }

        // Every block is 33 ms and moves the offset on by another 0.033, float
        // added to float; the frame index falls by one per block.
        f32 offset = 0.0f;
        for (u32 k = 0; k < want["steps"].U(); ++k) {
            CHECK(want["ticks"][k]["dtMs"].I() == particle::kSc2PreRollBlockMs);
            offset = offset + particle::kSc2PreRollOffsetStep;
        }
        CHECK(offset == want["timeOffset"].F());
        CHECK(want["lastFrameIndex"].I() == 1000 - static_cast<i32>(want["steps"].U()));
        ++ran;
    }
    CHECK(deviated > 0u);
    CHECK(ran > 60u);
}

TEST_CASE("op8b: a spawn call takes its requests first and flushes where retail does",
          "[sc2_particle][oracle][op8b]") {
    const fs::path path = GoldenDir() / "op8b_spawnparticles.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 40);

    unsigned multiFlush = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& want = c["out"];
        INFO("case " << i << " tag=" << c["tag"].S());

        particle::Sc2SpawnBatchInputs args;
        args.requests = in["requests"].U();
        args.plain = in["count"].U();
        args.elementCount = in["elementCountIn"].U();
        args.maxParticles = in["maxParticles"].U();
        const particle::Sc2SpawnBatchPlan plan = particle::Sc2PlanSpawnBatch(args);

        REQUIRE(plan.created == want["created"].U());
        CHECK(want["elementCount"].U() == args.elementCount + plan.created);
        // Node ADDRESSES say nothing about indices: the pool arena is global,
        // so a later case carves blocks a previous one freed, in free-list
        // order. What they do pin is the order — every element distinct, and
        // the flushes handing the initialiser exactly the live list, in append
        // order. That is the order `Sc2ParticleStore::Acquire` links.
        const auto& order = want["liveOrder"];
        REQUIRE(order.Size() == plan.created);
        std::vector<u32> live;
        for (std::size_t k = 0; k < order.Size(); ++k)
            live.push_back(order[k].U());
        std::vector<u32> distinct = live;
        std::sort(distinct.begin(), distinct.end());
        CHECK(std::adjacent_find(distinct.begin(), distinct.end()) == distinct.end());

        const auto& flushes = want["flushes"];
        REQUIRE(flushes.Size() == plan.flushes.size());
        CHECK(plan.requestsConsumed == (flushes.Size() != 0));
        std::size_t next = 0;
        for (std::size_t f = 0; f < flushes.Size(); ++f) {
            const auto& wf = flushes[f];
            const particle::Sc2SpawnFlush& pf = plan.flushes[f];
            CHECK(wf["slot"].U() == in["slot"].U());
            REQUIRE(wf["count"].U() == pf.requests + pf.plain);
            for (u32 k = 0; k < wf["count"].U(); ++k) {
                const i32 req = wf["requestIndex"][k].I();
                if (k < pf.requests)
                    CHECK(req == static_cast<i32>(pf.requestBegin + k));
                else
                    CHECK(req == -1);
                REQUIRE(next < live.size());
                CHECK(wf["elements"][k].U() == live[next++]);
            }
        }
        CHECK(next == live.size());
        multiFlush += flushes.Size() > 1 ? 1u : 0u;
    }
    CHECK(multiFlush > 0u);
}

// ---------------------------------------------------------------------------
// OP4 / OP5 - the two spawn samplers.
//
// Both are gated on the generator state as well as the value: a sampler that
// lands on the right point from the wrong stream position is wrong the moment
// anything else draws in the same frame. Every ordering in these two kernels
// was separated by a recorded vector - the "obvious" order was wrong for the
// disc, for the box's axis pick and for the speed overlay.
// ---------------------------------------------------------------------------

TEST_CASE("op4: the spawn shapes replay the draw stream, not just the point",
          "[sc2_particle][oracle][op4]") {
    const fs::path path = GoldenDir() / "op4_spawnpos.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 580);

    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& want = c["out"];

        particle::Sc2SpawnPosInputs args;
        args.shape = static_cast<particle::Sc2SpawnShape>(in["shape"].U());
        args.cutout = in["cutout"].B();
        args.shapeOuter = {in["shapeOuter"][0].F(), in["shapeOuter"][1].F(),
                           in["shapeOuter"][2].F()};
        args.shapeInner = {in["shapeInner"][0].F(), in["shapeInner"][1].F(),
                           in["shapeInner"][2].F()};
        args.outerRadius = in["outerRadius"].F();
        args.innerRadius = in["innerRadius"].F();
        args.elemScale = {in["elemScale"][0].F(), in["elemScale"][1].F(),
                          in["elemScale"][2].F()};

        std::vector<Vector3f> spline;
        for (const auto& p : in["spline"].A())
            spline.push_back({(*p)[0].F(), (*p)[1].F(), (*p)[2].F()});
        args.spline = spline;

        sc2::Rng rng(in["rngIn"][0].U(), in["rngIn"][1].U());
        const Vector3f got = particle::Sc2SampleSpawnPosition(rng, args);

        INFO("case " << i << " tag=" << c["tag"].S() << " shape=" << in["shape"].U()
                     << " cutout=" << in["cutout"].B() << " ex=" << in["extents"].S());

        // Point, plane and box are pure range draws - no transcendental in
        // sight - so they are held to the bit. The round shapes go through
        // sin/cos/sqrt and carry the harness bound.
        const u32 shape = in["shape"].U();
        const bool exact = shape == 0 || shape == 1 || shape == 3;
        const f32 axes[3] = {got.x, got.y, got.z};
        for (int k = 0; k < 3; ++k) {
            const f32 w = want["position"][static_cast<std::size_t>(k)].F();
            if (exact)
                REQUIRE(std::bit_cast<u32>(axes[k]) == std::bit_cast<u32>(w));
            else
                CloseRel(axes[k], w, 2e-6f, "position");
        }
        // Shape 0 draws nothing at all; the others must land on the exact
        // stream position the engine did.
        REQUIRE(rng.acc() == want["rngOut"][0].U());
        REQUIRE(rng.idx4() == want["rngOut"][1].U());
    }
}

TEST_CASE("op5: the velocity types replay the overlay order and the speed branch",
          "[sc2_particle][oracle][op5]") {
    const fs::path path = GoldenDir() / "op5_spawnvel.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 355);

    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& want = c["out"];

        particle::Sc2SpawnVelInputs args;
        args.velocityType = in["velocityType"].U();
        args.spawnYaw = in["yaw"].F();
        args.spawnPitch = in["pitch"].F();
        args.spawnHorizontal = in["horizontal"].F();
        args.spawnVertical = in["vertical"].F();
        args.speed = in["speed"].F();
        args.speedRandom = in["speedRandom"].F();
        args.speedIsEndpoint = in["speedIsEndpoint"].B();
        args.flattenXY = in["flattenXY"].B();
        args.position = {in["position"][0].F(), in["position"][1].F(), in["position"][2].F()};
        args.normal = {in["normal"][0].F(), in["normal"][1].F(), in["normal"][2].F()};
        args.variationTime = in["variationTime"].F();
        args.variationPhase = in["variationPhase"].F();

        // `overlays` names the wave type per armed group; the amplitude and
        // frequency live in the 18-float param block at [2g] and [2g+1].
        const auto& ov = in["overlayParams"];
        const auto& armed = in["overlays"];
        const auto group = [&](const char* key, std::size_t g) {
            particle::Sc2Overlay o;
            // `overlays` names only the ARMED groups, so absence is type 0.
            o.type = armed.Has(key) ? armed[key].U() : 0u;
            o.amplitude = ov[2 * g].F();
            o.frequency = ov[2 * g + 1].F();
            return o;
        };
        args.yawOverlay = group("0", 0);
        args.pitchOverlay = group("1", 1);
        args.speedOverlay = group("2", 2);
        args.horizontalOverlay = group("7", 7);
        args.verticalOverlay = group("8", 8);

        sc2::Rng rng(in["rngIn"][0].U(), in["rngIn"][1].U());
        const Vector3f got = particle::Sc2SampleSpawnVelocity(rng, args);

        INFO("case " << i << " tag=" << c["tag"].S() << " vt=" << in["velocityType"].U()
                     << " endpoint=" << in["speedIsEndpoint"].B()
                     << " flatten=" << in["flattenXY"].B());

        const f32 axes[3] = {got.x, got.y, got.z};
        for (int k = 0; k < 3; ++k)
            CloseRel(axes[k], want["velocity"][static_cast<std::size_t>(k)].F(), 2e-6f,
                     "velocity");
        // A type-5 overlay costs one draw. This pair is what makes that
        // measurable at all, and what a stand-in wave would fail.
        REQUIRE(rng.acc() == want["rngOut"][0].U());
        REQUIRE(rng.idx4() == want["rngOut"][1].U());
    }
}

// ---------------------------------------------------------------------------
// OP6 - the three attribute samplers.
//
// Each reads one overlay group and samples it BEFORE its own draws. The grid
// originally armed only wave types that do not draw, which made that order
// unobservable; with type 5 in the grid it is pinned, and the RNG state is
// compared alongside the value for exactly that reason.
// ---------------------------------------------------------------------------

namespace {

particle::Sc2Overlay OverlayFrom(const wdx_golden::Value& in, const char* typeKey,
                                 std::size_t group) {
    particle::Sc2Overlay o;
    o.type = in[typeKey].U();
    o.amplitude = in["overlayParams"][2 * group].F();
    o.frequency = in["overlayParams"][2 * group + 1].F();
    return o;
}

} // namespace

TEST_CASE("op6: SampleParticleColor replays the integer lerp and the alpha clamp",
          "[sc2_particle][oracle][op6]") {
    const fs::path path = GoldenDir() / "op6_color.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 1000);

    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];

        particle::Sc2ColorInputs args;
        for (std::size_t k = 0; k < 3; ++k) {
            args.keys[k] = in["colors"][k].U();
            args.randomKeys[k] = in["colors"][k + 3].U();
        }
        args.randomEnable = in["colorRandomEnable"].I() != 0;
        args.colorMidTime = in["colorMidTime"].F();
        args.alphaMidTime = in["alphaMidTime"].F();
        args.alphaOverlay = OverlayFrom(in, "alphaOverlayType", 4);
        args.variationTime = in["variationTime"].F();
        args.variationPhase = in["variationPhase"].F();

        sc2::Rng rng(in["rngIn"][0].U(), in["rngIn"][1].U());
        const auto got = particle::Sc2SampleColor(rng, args);

        INFO("case " << i << " random=" << in["colorRandomEnable"].I()
                     << " wave=" << in["alphaOverlayType"].U()
                     << " colorMid=" << in["colorMidTime"].F()
                     << " alphaMid=" << in["alphaMidTime"].F());
        // Integer arithmetic end to end - nothing here is allowed to drift.
        for (std::size_t k = 0; k < 3; ++k)
            REQUIRE(got[k] == c["out"]["nodes"][k].U());
        REQUIRE(rng.acc() == c["out"]["rngOut"][0].U());
        REQUIRE(rng.idx4() == c["out"]["rngOut"][1].U());
    }
}

TEST_CASE("op6: SampleParticleSize returns half extents, unquantised",
          "[sc2_particle][oracle][op6]") {
    const fs::path path = GoldenDir() / "op6_size.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 450);

    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];

        particle::Sc2SizeInputs args;
        for (std::size_t k = 0; k < 3; ++k) {
            args.keys[k] = in["size"][k].F();
            args.randomKeys[k] = in["sizeRandom"][k].F();
        }
        args.randomEnable = in["sizeRandomEnable"].I() != 0;
        args.sizeOverlay = OverlayFrom(in, "sizeOverlayType", 3);
        args.blend = in["blend"].F();
        args.instanceType = in["instanceType"].U();
        args.instanceDistance = in["instanceDistance"].F();
        args.variationTime = in["variationTime"].F();
        args.variationPhase = in["variationPhase"].F();

        sc2::Rng rng(in["rngIn"][0].U(), in["rngIn"][1].U());
        const auto got = particle::Sc2SampleSize(rng, args);

        INFO("case " << i << " random=" << in["sizeRandomEnable"].I()
                     << " wave=" << in["sizeOverlayType"].U()
                     << " blend=" << in["blend"].F()
                     << " instanceType=" << in["instanceType"].U());
        for (std::size_t k = 0; k < 4; ++k)
            CloseRel(got[k], c["out"]["size"][k].F(), 2e-6f, "size");
        REQUIRE(rng.acc() == c["out"]["rngOut"][0].U());
        REQUIRE(rng.idx4() == c["out"]["rngOut"][1].U());
    }
}

TEST_CASE("op6: SampleParticleRotation collapses its mid key only when randomised",
          "[sc2_particle][oracle][op6]") {
    const fs::path path = GoldenDir() / "op6_rotation.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 500);

    unsigned collapsed = 0;
    unsigned kept = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];

        particle::Sc2RotationInputs args;
        for (std::size_t k = 0; k < 3; ++k) {
            args.keys[k] = in["rotation"][k].F();
            args.randomKeys[k] = in["rotationRandom"][k].F();
        }
        args.randomEnable = in["rotationRandomEnable"].I() != 0;
        args.relative = (in["rotationFlags"].U() & 2u) != 0;
        args.rotationMidTime = in["rotationMidTime"].F();
        args.rotationOverlay = OverlayFrom(in, "rotationOverlayType", 6);
        args.variationTime = in["variationTime"].F();
        args.variationPhase = in["variationPhase"].F();

        sc2::Rng rng(in["rngIn"][0].U(), in["rngIn"][1].U());
        const auto got = particle::Sc2SampleRotation(rng, args);

        INFO("case " << i << " random=" << in["rotationRandomEnable"].I()
                     << " relative=" << in["rotationFlags"].U()
                     << " mid=" << in["rotationMidTime"].F()
                     << " wave=" << in["rotationOverlayType"].U());
        for (std::size_t k = 0; k < 3; ++k)
            CloseRel(got[k], c["out"]["rotation"][k].F(), 2e-6f, "rotation");
        REQUIRE(rng.acc() == c["out"]["rngOut"][0].U());
        REQUIRE(rng.idx4() == c["out"]["rngOut"][1].U());

        if (args.rotationMidTime > 0.9959f) {
            if (args.randomEnable)
                ++collapsed;
            else
                ++kept;
        }
    }
    // The asymmetry itself, asserted rather than left to the vectors: a high
    // mid time collapses the mid key ONLY under randomisation. Both arms have
    // to be populated or the check above proves nothing about either.
    CHECK(collapsed > 0);
    CHECK(kept > 0);
}

// ---------------------------------------------------------------------------
// OP8 - `InitSpawnedParticles`, the whole element.
//
// OP4/5/6 pin each sampler in isolation; this pins the order they are CALLED
// in, and the space transform that no sampler sees. Two of its arms were
// unreachable until the grid was widened: the world basis needs
// `additionalFlags & 8` AND a non-identity world matrix (never set together),
// and the normalised-basis + inherited-velocity sub-branch inside it needs
// `stateFlags & 8` on top (never set with the first).
// ---------------------------------------------------------------------------

namespace {

Matrix44f MatFrom(const wdx_golden::Value& node) {
    Matrix44f m = Matrix44f::identity();
    for (std::size_t r = 0; r < 4; ++r)
        for (std::size_t c = 0; c < 4; ++c)
            m.data[r][c] = node[4 * r + c].F();
    return m;
}

Vector3f Vec3From(const wdx_golden::Value& node) {
    return {node[0].F(), node[1].F(), node[2].F()};
}

f32 SysF(const wdx_golden::Value& sys, const char* key, f32 fallback) {
    return sys.Has(key) ? sys[key].F() : fallback;
}

u32 SysU(const wdx_golden::Value& sys, const char* key, u32 fallback) {
    return sys.Has(key) ? sys[key].U() : fallback;
}

} // namespace

TEST_CASE("op8: InitSpawnedParticles replays the element and the space arms",
          "[sc2_particle][oracle][op8]") {
    const fs::path path = GoldenDir() / "op8_initspawned.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 100);

    unsigned worldArm = 0;
    unsigned normalisedArm = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& par = in["par"];
        const auto& sys = in["sys"];

        particle::Sc2InitInputs args;
        args.parFlags = par["flags"].U();
        args.additionalFlags = par["additionalFlags"].U();
        args.rotationFlags = par["rotationFlags"].U();
        args.instanceType = par["instanceType"].U();
        args.emitFlagsWord = SysU(sys, "emitFlagsWord", 0);
        args.stateFlags = SysU(sys, "stateFlags", 0);
        args.noiseCoherence = par["noiseCoherence"].F();
        args.mass = par["mass"].F();
        args.massRandom = par["massRandom"].F();
        args.trailChance = par["trailChance"].F();
        args.flipbookColumns = static_cast<u16>(par["flipbookColumns"].U());
        args.flipbookRows = static_cast<u16>(par["flipbookRows"].U());
        args.hasChildEmitter1 = SysU(sys, "pChildEmitter1", 0) != 0;
        args.lifetime = sys["spawnLifetime"].F();
        args.lifetimeRandom = sys["spawnLifetimeRandom"].F();
        args.slot = in["slot"].U();
        args.worldMatrix = MatFrom(in["worldMatrix"]);
        args.hasBone = in["boneMatrix"].Size() == 16;
        if (args.hasBone)
            args.boneMatrix = MatFrom(in["boneMatrix"]);
        args.spawnTimeStep = in["spawnTimeStep"].F();
        args.spawnPosStep = Vec3From(in["spawnPosStep"]);
        args.smoothedPos = Vec3From(sys["smoothedPos"]);
        args.inheritVelocityScale = sys["inheritVelocityScale"].F();
        args.nowMs = in["nowMs"].U();

        args.shape.shape = static_cast<particle::Sc2SpawnShape>(par["emitterShape"].U());
        args.shape.cutout = (args.parFlags & 0x10) != 0;
        args.shape.shapeOuter = Vec3From(sys["shapeOuter"]);
        args.shape.shapeInner = Vec3From(sys["shapeInner"]);
        args.shape.outerRadius = sys["outerRadius"].F();
        args.shape.innerRadius = sys["innerRadius"].F();
        args.shape.elemScale = Vec3From(sys["elemScale"]);

        args.velocity.velocityType = par["velocityType"].U();
        args.velocity.spawnYaw = sys["spawnYaw"].F();
        args.velocity.spawnPitch = sys["spawnPitch"].F();
        args.velocity.spawnHorizontal = SysF(sys, "spawnHorizontal", 0.0f);
        args.velocity.spawnVertical = SysF(sys, "spawnVertical", 0.0f);
        args.velocity.speed = sys["spawnInitialSpeed"].F();
        args.velocity.speedRandom = sys["spawnSpeedRandom"].F();
        args.velocity.speedIsEndpoint = (args.additionalFlags & 1) != 0;
        args.velocity.flattenXY = (args.rotationFlags & 8) != 0;

        args.color.keys = {sys["spawnColorStart"].U(), sys["spawnColorMid"].U(),
                           sys["spawnColorEnd"].U()};
        args.color.randomKeys = {SysU(sys, "colorStartRandom", 0),
                                 SysU(sys, "colorMidRandom", 0),
                                 SysU(sys, "colorEndRandom", 0)};
        args.color.randomEnable = SysU(sys, "colorRandomEnable", 0) != 0;
        args.color.colorMidTime = SysF(sys, "colorMidTime", 0.0f);
        args.color.alphaMidTime = SysF(sys, "alphaMidTime", 0.0f);

        for (std::size_t k = 0; k < 3; ++k) {
            args.size.keys[k] = sys["spawnSizeAnim"][k].F();
            args.size.randomKeys[k] = sys["sizeRandomAnim"][k].F();
            args.rotation.keys[k] = sys["spawnRotationAnim"][k].F();
            args.rotation.randomKeys[k] = sys["rotationRandomAnim"][k].F();
        }
        args.size.randomEnable = SysU(sys, "sizeRandomEnable", 0) != 0;
        args.size.instanceType = args.instanceType;
        args.size.instanceDistance = SysF(sys, "instanceDistance", 0.0f);
        args.rotation.randomEnable = SysU(sys, "rotationRandomEnable", 0) != 0;
        args.rotation.relative = (args.rotationFlags & 2) != 0;
        args.rotation.rotationMidTime = SysF(sys, "rotationMidTime", 0.0f);

        std::vector<particle::SpawnRequest> reqs;
        for (const auto& r : in["requests"].A()) {
            particle::SpawnRequest q;
            q.position = Vec3From((*r)[0]);
            q.velocityScale = Vec3From((*r)[1]);
            q.orientVec = Vec3From((*r)[2]);
            reqs.push_back(q);
        }
        args.requests = reqs;

        particle::Sc2InitState st;
        st.emitterTime = sys["emitterTime"].F();
        st.expireFrameMs = in["expireIn"].U();

        const std::size_t count = static_cast<std::size_t>(in["count"].U());
        std::vector<particle::Sc2SpawnedElement> elems(count);
        sc2::Rng rng(in["rngIn"][0].U(), in["rngIn"][1].U());
        particle::Sc2InitSpawned(rng, args, st, elems);

        if ((args.additionalFlags & 8) != 0 && reqs.empty()) {
            ++worldArm;
            if ((args.stateFlags & 8) != 0)
                ++normalisedArm;
        }

        const auto& want = c["out"]["elements"];
        REQUIRE(want.Size() == count);
        for (std::size_t n = 0; n < count; ++n) {
            const auto& w = want[n];
            const auto& e = elems[n];
            INFO("case " << i << " tag=" << c["tag"].S() << " elem " << n
                         << " slot=" << args.slot << " addl=" << args.additionalFlags
                         << " state=" << args.stateFlags
                         << " itype=" << args.instanceType);
            const f32 pos[3] = {e.position.x, e.position.y, e.position.z};
            const f32 vel[3] = {e.velocity.x, e.velocity.y, e.velocity.z};
            const f32 orient[3] = {e.orientVec.x, e.orientVec.y, e.orientVec.z};
            const f32 origin[3] = {e.spawnOrigin.x, e.spawnOrigin.y, e.spawnOrigin.z};
            for (std::size_t k = 0; k < 3; ++k) {
                CloseRel(pos[k], w["position"][k].F(), 2e-6f, "position");
                CloseRel(vel[k], w["velocity"][k].F(), 2e-6f, "velocity");
                CloseRel(orient[k], w["orientVec"][k].F(), 2e-6f, "orientVec");
                CloseRel(origin[k], w["spawnOrigin"][k].F(), 2e-6f, "spawnOrigin");
                REQUIRE(e.rotation[k] == w["rotation"][k].U());
                REQUIRE(e.colorNodes[k] == w["colorNodes"][k].U());
            }
            for (std::size_t k = 0; k < 4; ++k)
                REQUIRE(e.size[k] == w["size"][k].U());
            CloseRel(e.invMass, w["invMass"].F(), 2e-6f, "invMass");
            CloseRel(e.birthTime, w["birthTime"].F(), 2e-6f, "birthTime");
            CloseRel(e.deathTime, w["deathTime"].F(), 2e-6f, "deathTime");
            CloseRel(e.noisePhase, w["noisePhase"].F(), 2e-6f, "noisePhase");
            CloseRel(e.flipbookRandStart, w["flipbookRandStart"].F(), 2e-6f,
                     "flipbookRandStart");
            REQUIRE(e.flags == w["flags"].U());
            REQUIRE(e.flipbookRand == w["flipbookRand"].U());
            REQUIRE(e.vbSlot == w["vbSlot"].I());
            REQUIRE(e.bounceCount == w["bounceCount"].I());
        }

        CloseRel(st.emitterTime, c["out"]["emitterTime"].F(), 2e-6f, "emitterTime");
        REQUIRE(st.expireFrameMs == c["out"]["expireFrameMs"].U());
        REQUIRE(rng.acc() == c["out"]["rngOut"][0].U());
        REQUIRE(rng.idx4() == c["out"]["rngOut"][1].U());
    }

    // Both world-arm sub-branches have to be populated, or the transform above
    // is being checked against vectors that never enter it. This is the check
    // the gate lacked: for its whole life every "space" vector took the local
    // path and the assertions passed on the identity.
    CHECK(worldArm > 0);
    CHECK(normalisedArm > 0);
}

// ---------------------------------------------------------------------------
// OP9 (retire rows) + OP12 (`proc` rows) — the analytic path's whole frame.
//
// The two halves of X3's MOVE/RETIRE: the closed form that replaces
// integration, and the sweep that is all the CPU does instead.
// ---------------------------------------------------------------------------

TEST_CASE("op9: RetireExpiredParticles walks the list both ways",
          "[sc2_particle][oracle][op9]") {
    const fs::path path = GoldenDir() / "op9_simulate.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 90);

    unsigned replayed = 0;
    unsigned selectedRetire = 0;
    unsigned selectedSimulate = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const bool useRetire =
            particle::Sc2UseRetirePath(in["stateFlags"].U(), in["forceCpu"].U() != 0);
        (useRetire ? selectedRetire : selectedSimulate)++;

        // The CPU branch is X4's. Its vectors still contribute the selection
        // count above, so a predicate that answered "retire" everywhere would
        // show up as an empty simulate arm rather than as silence.
        if (!useRetire)
            continue;

        const auto& src = in["elementsIn"];
        const std::size_t n = src.Size();
        std::vector<particle::Sc2SpawnedElement> elems(n);
        for (std::size_t k = 0; k < n; ++k) {
            elems[k].deathTime = src[k]["deathTime"].F();
            elems[k].vbSlot = src[k]["vbSlot"].I();
        }

        particle::Sc2ElementList list;
        list.Reset(n);
        particle::Sc2RecycleArray recycle;
        recycle.enabled = c["out"].Has("recycleCount");

        const u32 retired =
            particle::Sc2RetireExpired(list, elems, in["emitterTime"].F(), recycle);

        INFO("case " << i << " tag=" << c["tag"].S() << " n=" << n);
        std::vector<i32> alive;
        std::vector<i32> back;
        std::vector<i32> freed;
        list.Walk(alive);
        list.WalkBackward(back);
        list.WalkFree(freed);

        const auto& wantAlive = c["out"]["alive"];
        REQUIRE(alive.size() == wantAlive.Size());
        for (std::size_t k = 0; k < alive.size(); ++k)
            REQUIRE(alive[k] == wantAlive[k].I());

        // The seam the two gates each left to the other: OP9 recorded only the
        // forward chain, OP11b walks backward only over a pristine list, so
        // the links a retirement leaves behind were unmeasured by both — and
        // under `PAR_.flags & 0x100` the backward chain is the order the
        // upload and the batch builder walk, i.e. the draw order.
        const auto& wantBack = c["out"]["aliveReverse"];
        REQUIRE(back.size() == wantBack.Size());
        for (std::size_t k = 0; k < back.size(); ++k)
            REQUIRE(back[k] == wantBack[k].I());

        const auto& wantFreed = c["out"]["freed"];
        REQUIRE(freed.size() == wantFreed.Size());
        for (std::size_t k = 0; k < freed.size(); ++k)
            REQUIRE(freed[k] == wantFreed[k].I());

        REQUIRE(list.poolCount == c["out"]["poolCount"].U());
        REQUIRE(n - retired == c["out"]["elementCount"].U());

        const auto& wantElems = c["out"]["elements"];
        for (std::size_t k = 0; k < n; ++k)
            REQUIRE(elems[k].vbSlot == wantElems[k]["vbSlot"].I());

        if (recycle.enabled) {
            const auto& wantSlots = c["out"]["recycled"];
            REQUIRE(recycle.slots.size() == c["out"]["recycleCount"].U());
            REQUIRE(recycle.slots.size() == wantSlots.Size());
            for (std::size_t k = 0; k < recycle.slots.size(); ++k)
                REQUIRE(recycle.slots[k] == wantSlots[k].I());
        }
        ++replayed;
    }

    CHECK(replayed >= 9);
    // Both arms of the path pick have to be populated, or the predicate is
    // being asserted against vectors that all answer the same way.
    CHECK(selectedRetire > 0);
    CHECK(selectedSimulate > 0);
}

TEST_CASE("op12: the analytic step replays the shader's closed form",
          "[sc2_particle][oracle][op12]") {
    const fs::path path = GoldenDir() / "op12_particlefx.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];

    unsigned replayed = 0;
    unsigned beforeBirth = 0;
    unsigned clamped = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        if (!in["procedural"].B())
            continue;

        const auto& vtx = in["vertex"];
        const auto& bat = in["batch"];
        const auto& bdd = vtx["birthDeathDrag"];
        const auto& i1 = vtx["interp1"];
        const auto& i2 = vtx["interp2"];

        particle::Sc2AnalyticInputs args;
        args.position = {vtx["position"][0].F(), vtx["position"][1].F(),
                         vtx["position"][2].F()};
        args.velocity0 = {i1[0].F(), i1[1].F(), i1[2].F()};
        args.invMass = i1[3].F();
        args.birthTime = bdd[0].F();
        args.deathTime = bdd[1].F();
        args.drag = bdd[2].F();
        args.invDrag = bdd[3].F();
        args.gravityZ = i2[3].F();
        args.systemTime = bat["sys"][0].F();
        args.instanceType = in["type"].U();
        args.tailLength = i2[0].F();
        args.fixedTailLength = in["fixedTail"].B();
        args.clampedTailLength = in["clampedTail"].B();

        // The size the tail clamp budgets against — the same scalar
        // `InterpolateValue` the vertex build uses, already O12-gated.
        const f32 age = vs::Saturate((args.systemTime - args.birthTime) /
                                          (args.deathTime - args.birthTime));
        args.size = vs::InterpolateValue(
            age, static_cast<f32>(vtx["size"][0].U()) / 256.0f,
            static_cast<f32>(vtx["size"][1].U()) / 256.0f,
            static_cast<f32>(vtx["size"][2].U()) / 256.0f,
            bat["midKey"][0].F(), bat["invMidKey"][0].F(), bat["hold"][0].F(),
            static_cast<int>(in["sizeInterp"].U()));

        const auto step = particle::Sc2StepAnalytic(args);

        if (args.systemTime < args.birthTime)
            ++beforeBirth;
        if (args.clampedTailLength && step.tailLength != args.tailLength)
            ++clamped;

        INFO("case " << i << " type=" << args.instanceType
                     << " drag=" << args.drag << " invDrag=" << args.invDrag
                     << " invMass=" << args.invMass << " t=" << step.elapsed);

        // The tolerance is DERIVED, not picked. The closed form subtracts two
        // terms of magnitude `mass * invDrag * (|v0| + mass*|g|*invDrag)`, so
        // whatever `exp` rounds to, the answer carries an absolute error of
        // that scale's ULP — half a world unit at the drag floor, which is
        // also the setting any emitter authoring no drag lands on. A flat
        // relative bound here would either fail on correct code or hide a real
        // one; this bound is the arithmetic's own.
        const f32 mass = 1.0f / args.invMass;
        const f32 gravity = std::abs(args.gravityZ);
        const f32 scale = std::abs(mass * args.invDrag) *
                          (std::abs(args.velocity0.x) + std::abs(args.velocity0.y) +
                           std::abs(args.velocity0.z) + mass * gravity * args.invDrag);
        // ONE ULP of that scale, plus a floor so an all-zero lane still has
        // one. The factor was tightened down from 4 until it was the smallest
        // that stays green on both toolchains, rather than picked with room to
        // spare: clang's and MSVC's `expf` both agree with the golden's
        // double-rounded exp to inside the closed form's own rounding, so
        // anything looser would be slack nothing needs.
        const f32 slack = std::max(std::abs(scale) * 1.1920929e-7f, 1e-6f);

        const auto& wantPos = c["out"]["procPosition"];
        const auto& wantDisp = c["out"]["procDisplacement"];
        const auto& wantVel = c["out"]["procVelocity"];
        const f32 pos[3] = {step.position.x, step.position.y, step.position.z};
        const f32 disp[3] = {step.displacement.x, step.displacement.y,
                             step.displacement.z};
        const f32 vel[3] = {step.velocity.x, step.velocity.y, step.velocity.z};
        for (std::size_t k = 0; k < 3; ++k) {
            CHECK(std::abs(pos[k] - wantPos[k].F()) <= slack);
            CHECK(std::abs(disp[k] - wantDisp[k].F()) <= slack);
            // The velocity is not the cancelling expression the displacement
            // is, so it keeps a plain relative bound.
            CloseRel(vel[k], wantVel[k].F(), 1e-5f, "procVelocity");
        }
        CHECK(std::abs(step.tailLength - c["out"]["procTail"].F()) <= slack);
        ++replayed;
    }

    CHECK(replayed >= 140);
    // `systemTime < birthTime` is the only thing that exercises the elapsed
    // clamp, and the section had no such row until this phase added one.
    CHECK(beforeBirth > 0);
    CHECK(clamped > 0);
}

TEST_CASE("op11b: the analytic vertex body is the element, uploaded once",
          "[sc2_particle][oracle][op11b]") {
    const fs::path path = GoldenDir() / "op11b_gpuupload.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 60);

    // The lane retail leaves to whatever owned the slot before, and the dead
    // `.w` of the position. Both are asserted below rather than skipped
    // silently.
    constexpr std::size_t kHoleWord = 15;
    constexpr std::size_t kPositionWWord = 3;

    unsigned bodies = 0;
    unsigned seams = 0;
    unsigned reversed = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& out = c["out"];
        const auto& src = in["elementsIn"];
        const std::size_t n = src.Size();

        std::vector<particle::Sc2SpawnedElement> elems(n);
        for (std::size_t k = 0; k < n; ++k) {
            const auto& e = src[k];
            elems[k].position = Vec3From(e["position"]);
            elems[k].velocity = Vec3From(e["velocity"]);
            elems[k].orientVec = Vec3From(e["orientVec"]);
            elems[k].spawnOrigin = Vec3From(e["spawnOrigin"]);
            elems[k].noiseVec = Vec3From(e["noiseVec"]);
            elems[k].invMass = e["invMass"].F();
            elems[k].birthTime = e["birthTime"].F();
            elems[k].deathTime = e["deathTime"].F();
            elems[k].flipbookRandStart = e["flipbookRandStart"].F();
            elems[k].flipbookRand = static_cast<u16>(e["flipbookRand"].U());
            elems[k].vbSlot = e["vbSlot"].I();
            for (std::size_t j = 0; j < 4; ++j)
                elems[k].size[j] = static_cast<u16>(e["size"][j].U());
            for (std::size_t j = 0; j < 3; ++j) {
                elems[k].colorNodes[j] = e["colorNodes"][j].U();
                elems[k].rotation[j] = static_cast<u16>(e["rotation"][j].U());
            }
        }

        particle::Sc2VertexBodyInputs vin;
        vin.drag = in["drag"].F();
        vin.gravity = in["gravity"].F();
        vin.worldGravityScale =
            in.Has("gravityScale") ? in["gravityScale"].F() : 1.0f;
        vin.instanceType = in["instanceType"].U();
        vin.tailLength = in["tailLength"].F();
        vin.instanceAngle = Vec3From(in["instanceAngle"]);
        vin.batchIndex = in["batchIndex"].U();

        INFO("case " << i << " tag=" << c["tag"].S());

        // The seam: these vectors ran a real retirement before the upload, so
        // the list the walk sees is one `Sc2RetireExpired` produced rather
        // than one a fixture linked. Replaying both kernels in order is the
        // only way this test can measure the handover at all.
        if (!out["afterRetire"].IsNull()) {
            particle::Sc2ElementList list;
            list.Reset(n);
            particle::Sc2RecycleArray recycle;
            const u32 gone = particle::Sc2RetireExpired(
                list, elems, in["emitterTime"].F(), recycle);
            (void)gone;

            const auto& after = out["afterRetire"];
            std::vector<i32> alive;
            std::vector<i32> back;
            list.Walk(alive);
            list.WalkBackward(back);

            const auto& wantAlive = after["alive"];
            REQUIRE(alive.size() == wantAlive.Size());
            for (std::size_t k = 0; k < alive.size(); ++k)
                REQUIRE(alive[k] == wantAlive[k].I());
            const auto& wantBack = after["aliveReverse"];
            REQUIRE(back.size() == wantBack.Size());
            for (std::size_t k = 0; k < back.size(); ++k)
                REQUIRE(back[k] == wantBack[k].I());

            const auto& wantFreed = after["freed"];
            REQUIRE(recycle.slots.size() == wantFreed.Size());
            for (std::size_t k = 0; k < recycle.slots.size(); ++k)
                REQUIRE(recycle.slots[k] == wantFreed[k].I());

            // And the order the upload then walks: the backward chain under
            // sort-reverse, the forward one otherwise. Elements needing a
            // slot get one in that order, so the golden's slot numbers put
            // the walk in evidence without reimplementing the allocator.
            const std::vector<i32>& order = in["sortReverse"].U() ? back : alive;
            std::vector<i32> fresh;
            for (i32 k : order)
                if (after["vbSlots"][static_cast<std::size_t>(k)].I() == -1)
                    fresh.push_back(k);
            std::vector<std::pair<i32, i32>> got;
            for (i32 k : fresh)
                got.emplace_back(out["elements"][static_cast<std::size_t>(k)]["vbSlot"].I(), k);
            for (std::size_t k = 1; k < got.size(); ++k)
                REQUIRE(got[k - 1].first < got[k].first);
            if (in["sortReverse"].U() && !fresh.empty())
                reversed++;
            seams++;
        }

        for (std::size_t k = 0; k < n; ++k) {
            const auto& want = out["vertices"][k];
            if (want.IsNull())
                continue;
            REQUIRE(want.Size() == 4);
            const auto quad = particle::Sc2VertexBody(vin, elems[k]);
            for (std::size_t j = 0; j < 4; ++j) {
                std::array<u32, 29> words{};
                std::memcpy(words.data(), &quad[j], sizeof(quad[j]));
                for (std::size_t w = 0; w < words.size(); ++w) {
                    INFO("element " << k << " vertex " << j << " dword " << w);
                    if (w == kHoleWord) {
                        // Retail's block path never writes +60, so the golden
                        // holds the fill the slot carried; ours writes the
                        // emitter's batch index, because we bind one emitter's
                        // constants instead of inheriting a stale lane.
                        REQUIRE(want[j][w].U() != words[w]);
                        REQUIRE(words[w] == vin.batchIndex);
                        continue;
                    }
                    if (w == kPositionWWord) {
                        // Retail copies the element's lane; no shader path
                        // reads `vPosition.w`, so ours is 0.
                        REQUIRE(want[j][w].U() == src[k]["posW"].U());
                        REQUIRE(words[w] == 0u);
                        continue;
                    }
                    REQUIRE(want[j][w].U() == words[w]);
                }
                bodies++;
            }
        }
    }

    CHECK(bodies >= 250);
    CHECK(seams >= 6);
    CHECK(reversed >= 3);
}

TEST_CASE("op12: every instance type builds the shader quad, corner for corner",
          "[sc2_particle][oracle][op12]") {
    const fs::path path = GoldenDir() / "op12_particlefx.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 500);

    const auto arr4 = [](const wdx_golden::Value& n) {
        std::array<f32, 4> a{};
        for (std::size_t i = 0; i < 4; ++i)
            a[i] = n[i].F();
        return a;
    };
    const auto mat = [](const wdx_golden::Value& n) {
        std::array<f32, 16> m{};
        for (std::size_t i = 0; i < 16; ++i)
            m[i] = n[i].F();
        return m;
    };

    unsigned replayed = 0;
    // Every branch has to be reached, or a type whose rows all skipped
    // would leave its transcription unmeasured behind a green total.
    std::array<unsigned, 11> perType{};
    unsigned flipbook = 0;
    unsigned instanced = 0;
    unsigned tilted = 0;
    unsigned localSpace = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& vsrc = in["vertex"];
        const auto& bsrc = in["batch"];
        const auto& csrc = in["camera"];

        particle::Sc2QuadInput v;
        v.position = Vec3From(vsrc["position"]);
        v.size = arr4(vsrc["size"]);
        for (std::size_t k = 0; k < 3; ++k)
            v.color[k] = arr4(vsrc["color" + std::to_string(k)]);
        v.rotation = arr4(vsrc["rotation"]);
        const auto bdd = arr4(vsrc["birthDeathDrag"]);
        v.birthTime = bdd[0];
        v.deathTime = bdd[1];
        v.drag = bdd[2];
        v.invDrag = bdd[3];
        const auto i1 = arr4(vsrc["interp1"]);
        v.velocity = {i1[0], i1[1], i1[2]};
        v.invMass = i1[3];
        const auto i2 = arr4(vsrc["interp2"]);
        v.instanceVec = {i2[0], i2[1], i2[2]};
        v.gravityZ = i2[3];
        const auto nz = arr4(vsrc["noise"]);
        v.noise = {nz[0], nz[1], nz[2]};
        v.flipbookRandStart = nz[3];

        particle::Sc2QuadBatch b;
        b.midKey = arr4(bsrc["midKey"]);
        b.invMidKey = arr4(bsrc["invMidKey"]);
        b.hold = arr4(bsrc["hold"]);
        const auto sys = arr4(bsrc["sys"]);
        b.systemTime = sys[0];
        b.elementScale = sys[1];
        b.flipbookMidKeyTime = sys[2];
        b.flipbookColumns = sys[3];
        for (std::size_t k = 0; k < 3; ++k)
            b.flipbookFrames[k] = bsrc["flipbookFrames"][k].F();
        for (std::size_t k = 0; k < 2; ++k)
            b.cellSize[k] = bsrc["cellSize"][k].F();
        b.prWorld = mat(bsrc["prWorld"]);
        b.instanceTransform = mat(bsrc["instanceTransform"]);

        particle::Sc2QuadCamera cam;
        cam.billboardRight = Vec3From(csrc["right"]);
        cam.billboardUp = Vec3From(csrc["up"]);
        cam.direction = Vec3From(csrc["direction"]);
        cam.eye = Vec3From(csrc["eye"]);

        particle::Sc2QuadFlags fl;
        fl.instanceType = in["type"].U();
        fl.fixedTailLength = in["fixedTail"].B();
        fl.clampedTailLength = in["clampedTail"].B();
        fl.sizeInterp = in["sizeInterp"].I();
        fl.colorInterp = in["colorInterp"].I();
        fl.rotationInterp = in["rotInterp"].I();
        fl.localSpace = in["localSpace"].B();
        fl.modelInstancing = in["modelInstancing"].B();
        fl.proceduralPosition = in["procedural"].B();
        fl.randomFlipbookStart = in["randomStart"].B();
        // `PARTICLE_FLIPBOOK` is UV mapping 6; the random offset is a
        // separate per-slot arm the flipbook outranks. Slot 0 is the one the
        // golden's `uv` records.
        fl.flipbookUv = in["uvMapping"][0].I() == 6;
        fl.uvRandomOffset = in["uvRandomOffset"][0].I() != 0;

        INFO("case " << i << " tag=" << c["tag"].S());
        const auto q = particle::Sc2ExpandQuad(v, b, cam, fl);
        REQUIRE(q.supported);

        // The corner positions come out of a chain of adds around values in
        // the hundreds, so the bound is derived from the magnitude rather
        // than picked: a flat epsilon would either fail the instanced cases
        // or hide a wrong composition in the ones near the origin.
        const auto& wc = c["out"]["corners"];
        REQUIRE(wc.Size() == 4);
        for (std::size_t k = 0; k < 4; ++k) {
            const Vector3f want = Vec3From(wc[k]);
            const Vector3f got = q.corner[k].position;
            const f32 scale = std::max({std::abs(want.x), std::abs(want.y),
                                        std::abs(want.z), 1.0f});
            const f32 slack = scale * 8.0f * 1.1920929e-7f;
            CHECK(std::abs(got.x - want.x) <= slack);
            CHECK(std::abs(got.y - want.y) <= slack);
            CHECK(std::abs(got.z - want.z) <= slack);
        }

        CHECK(std::abs(q.age - c["out"]["age"].F()) <= 1e-6f);
        CHECK(std::abs(q.size - c["out"]["size"].F()) <= 1e-6f);
        const auto wcol = arr4(c["out"]["color"]);
        for (std::size_t k = 0; k < 4; ++k)
            CHECK(std::abs(q.color[k] - wcol[k]) <= 1e-6f);

        // The golden records one UV per corner from slot 0.
        const auto& wuv = c["out"]["uv"];
        REQUIRE(wuv.Size() == 4);
        for (std::size_t k = 0; k < 4; ++k) {
            CHECK(std::abs(q.corner[k].uv.x - wuv[k][0].F()) <= 1e-6f);
            CHECK(std::abs(q.corner[k].uv.y - wuv[k][1].F()) <= 1e-6f);
        }

        // The golden records the frame of corner 0. All three vectors, not
        // just the normal: the tangent and binormal are what the flip idioms
        // and the non-unit tail binormal differ in, and the bound follows the
        // magnitude for the same reason as the corners.
        const auto sameVec = [](const Vector3f& got, const Vector3f& want) {
            const f32 scale = std::max({std::abs(want.x), std::abs(want.y),
                                        std::abs(want.z), 1.0f});
            const f32 slack = scale * 8.0f * 1.1920929e-7f;
            CHECK(std::abs(got.x - want.x) <= slack);
            CHECK(std::abs(got.y - want.y) <= slack);
            CHECK(std::abs(got.z - want.z) <= slack);
        };
        sameVec(q.corner[0].normal, Vec3From(c["out"]["normal"]));
        sameVec(q.corner[0].tangent, Vec3From(c["out"]["tangent"]));
        sameVec(q.corner[0].binormal, Vec3From(c["out"]["binormal"]));

        replayed++;
        perType[std::min<std::size_t>(fl.instanceType, 10)]++;
        if (fl.flipbookUv)
            flipbook++;
        if (fl.modelInstancing)
            instanced++;
        if (fl.localSpace)
            localSpace++;
        if (std::abs(cam.direction.x) > 1e-3f)
            tilted++;
    }

    CHECK(replayed >= 500);
    for (std::size_t k = 0; k < perType.size(); ++k) {
        INFO("instance type " << k);
        CHECK(perType[k] >= 2u);
    }
    CHECK(flipbook >= 100);
    CHECK(instanced >= 2);
    CHECK(localSpace >= 2);
    CHECK(tilted >= 3);
}

TEST_CASE("op15: the batch row is the emitter's constants, lane for lane",
          "[sc2_particle][oracle][op15]") {
    const fs::path path = GoldenDir() / "op15_instanceconstants.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 70);

    // Every `out` lane in this golden is a float32 BIT PATTERN, so a lane the
    // producer never wrote survives as its keyed poison instead of vanishing.
    constexpr u32 kPoison = 0xF15D0000u;
    const auto poisoned = [](u32 w) { return (w & 0xFFFF0000u) == kPoison; };
    // Our own fill, for the same reason and with a different key: a lane the
    // golden says was never written has to still hold OURS afterwards, and a
    // shared key could not tell "left alone" from "written with the poison".
    const auto sentinel = [](u32 k) {
        return std::bit_cast<f32>(0x7EA50000u | k);
    };

    const auto mat = [](const wdx_golden::Value& n) {
        std::array<f32, 16> m{};
        for (std::size_t i = 0; i < 16; ++i)
            m[i] = n[i].F();
        return m;
    };

    unsigned replayed = 0;
    unsigned suppressed = 0;
    unsigned degraded = 0;
    unsigned instanced = 0;
    unsigned infinite = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& out = c["out"];
        INFO("case " << i << " tag=" << c["tag"].S());

        particle::Sc2BatchDesc d;
        d.sizeMidTime = in["sizeMidTime"].F();
        d.colorMidTime = in["colorMidTime"].F();
        d.alphaMidTime = in["alphaMidTime"].F();
        d.rotationMidTime = in["rotationMidTime"].F();
        d.sizeMidHoldTime = in["sizeMidHoldTime"].F();
        d.colorMidHoldTime = in["colorMidHoldTime"].F();
        d.alphaMidHoldTime = in["alphaMidHoldTime"].F();
        d.rotationMidHoldTime = in["rotationMidHoldTime"].F();
        d.flipbookStartInitIndex =
            static_cast<u8>(in["flipbookStartInitIndex"].U());
        d.flipbookStartStopIndex =
            static_cast<u8>(in["flipbookStartStopIndex"].U());
        d.flipbookEndInitIndex =
            static_cast<u8>(in["flipbookEndInitIndex"].U());
        d.flipbookMidTime = in["flipbookMidTime"].F();
        d.flipbookColumns = static_cast<u16>(in["flipbookColumns"].U());
        d.flipbookRows = static_cast<u16>(in["flipbookRows"].U());
        d.flipbookColumnFraction = in["flipbookColumnFraction"].F();
        d.flipbookRowFraction = in["flipbookRowFraction"].F();
        d.worldSpace = (in["additionalFlags"].U() & 8u) != 0u;

        // The same row, reached through the load-time desc. Everything above
        // is the row OP15 measured; this is the adapter that has to hand it
        // those values, and a transposed lane on THIS side would otherwise be
        // invisible — the gate ends at `Sc2BatchDesc` and the loader starts
        // after it.
        particle::Sc2EmitterDesc ed;
        ed.look.midTime[0] = d.sizeMidTime;
        ed.look.midTime[1] = d.colorMidTime;
        ed.look.midTime[2] = d.alphaMidTime;
        ed.look.midTime[3] = d.rotationMidTime;
        ed.look.midHold[0] = d.sizeMidHoldTime;
        ed.look.midHold[1] = d.colorMidHoldTime;
        ed.look.midHold[2] = d.alphaMidHoldTime;
        ed.look.midHold[3] = d.rotationMidHoldTime;
        ed.look.flipbookStartInit = d.flipbookStartInitIndex;
        ed.look.flipbookStartStop = d.flipbookStartStopIndex;
        ed.look.flipbookEndInit = d.flipbookEndInitIndex;
        ed.look.flipbookMidTime = d.flipbookMidTime;
        ed.look.flipbookColumns = d.flipbookColumns;
        ed.look.flipbookRows = d.flipbookRows;
        ed.look.flipbookColumnFraction = d.flipbookColumnFraction;
        ed.look.flipbookRowFraction = d.flipbookRowFraction;
        ed.emit.worldSpace = d.worldSpace;
        const particle::Sc2BatchDesc via = particle::Sc2BatchDescFrom(ed);
        CHECK(via.sizeMidTime == d.sizeMidTime);
        CHECK(via.colorMidTime == d.colorMidTime);
        CHECK(via.alphaMidTime == d.alphaMidTime);
        CHECK(via.rotationMidTime == d.rotationMidTime);
        CHECK(via.sizeMidHoldTime == d.sizeMidHoldTime);
        CHECK(via.colorMidHoldTime == d.colorMidHoldTime);
        CHECK(via.alphaMidHoldTime == d.alphaMidHoldTime);
        CHECK(via.rotationMidHoldTime == d.rotationMidHoldTime);
        CHECK(via.flipbookStartInitIndex == d.flipbookStartInitIndex);
        CHECK(via.flipbookStartStopIndex == d.flipbookStartStopIndex);
        CHECK(via.flipbookEndInitIndex == d.flipbookEndInitIndex);
        CHECK(via.flipbookMidTime == d.flipbookMidTime);
        CHECK(via.flipbookColumns == d.flipbookColumns);
        CHECK(via.flipbookRows == d.flipbookRows);
        CHECK(via.flipbookColumnFraction == d.flipbookColumnFraction);
        CHECK(via.flipbookRowFraction == d.flipbookRowFraction);
        CHECK(via.worldSpace == d.worldSpace);

        particle::Sc2BatchFrame f;
        f.world = mat(in["world"]);
        f.hasInstanceNode = !in["instance"].IsNull();
        if (f.hasInstanceNode)
            f.instanceTransform = mat(in["instance"]);
        f.emitterTime = in["emitterTime"].F();

        // A row nobody has written yet, filled so that "left alone" is a
        // readable state rather than a zero that could have come from anywhere.
        particle::Sc2QuadBatch row;
        u32 k = 0;
        for (auto& v : row.prWorld) v = sentinel(k++);
        for (auto& v : row.instanceTransform) v = sentinel(k++);
        for (auto& v : row.midKey) v = sentinel(k++);
        for (auto& v : row.invMidKey) v = sentinel(k++);
        for (auto& v : row.hold) v = sentinel(k++);
        for (auto& v : row.flipbookFrames) v = sentinel(k++);
        for (auto& v : row.cellSize) v = sentinel(k++);
        row.systemTime = sentinel(k++);
        row.elementScale = sentinel(k++);
        row.flipbookMidKeyTime = sentinel(k++);
        row.flipbookColumns = sentinel(k++);
        const particle::Sc2QuadBatch before = row;

        particle::Sc2WriteQuadBatch(row, d, f);

        // A lane the golden marks poisoned must still hold OUR fill; every
        // other lane is compared bit for bit, because all of these are copies
        // or a correctly-rounded division.
        const auto check = [&](const char* what, const wdx_golden::Value& want,
                               std::size_t lane, f32 got, f32 was) {
            INFO(what << " lane " << lane);
            const u32 w = want[lane].U();
            if (poisoned(w)) {
                CHECK(std::bit_cast<u32>(got) == std::bit_cast<u32>(was));
                return false;
            }
            CHECK(std::bit_cast<u32>(got) == w);
            return true;
        };

        for (std::size_t j = 0; j < 16; ++j)
            check("prWorld", out["prWorld"], j, row.prWorld[j],
                  before.prWorld[j]);
        for (std::size_t j = 0; j < 16; ++j)
            check("instanceTransform", out["instanceTransform"], j,
                  row.instanceTransform[j], before.instanceTransform[j]);
        for (std::size_t j = 0; j < 4; ++j)
            check("midKey", out["midKey"], j, row.midKey[j], before.midKey[j]);
        for (std::size_t j = 0; j < 4; ++j) {
            check("invMidKey", out["invMidKey"], j, row.invMidKey[j],
                  before.invMidKey[j]);
            if (out["invMidKey"][j].U() == 0x7F800000u)
                infinite++;
        }
        for (std::size_t j = 0; j < 4; ++j)
            check("hold", out["hold"], j, row.hold[j], before.hold[j]);

        // The dead lanes: three of the four authored flipbook indices are
        // uploaded and two of the four cell-size floats, so this struct
        // carries three and two — the golden's extra lanes must be poison,
        // which is the assertion that we are not dropping a live value.
        for (std::size_t j = 0; j < 3; ++j)
            check("flipbookFrames", out["flipbookFrames"], j,
                  row.flipbookFrames[j], before.flipbookFrames[j]);
        CHECK(poisoned(out["flipbookFrames"][3].U()));
        for (std::size_t j = 0; j < 2; ++j)
            check("cellSize", out["cellSize"], j, row.cellSize[j],
                  before.cellSize[j]);
        CHECK(poisoned(out["cellSize"][2].U()));
        CHECK(poisoned(out["cellSize"][3].U()));

        // `sys` is (emitterTime, elementScale, flipbookMidKeyTime, columns).
        // Only the second is not a copy, and it is the only lane in this whole
        // golden that is not compared bit for bit.
        const auto& sys = out["sys"];
        CHECK(std::bit_cast<u32>(row.systemTime) == sys[0].U());
        CloseRel(row.elementScale, std::bit_cast<f32>(sys[1].U()), kRsqrtRtol,
                 "elementScale");
        CHECK(std::bit_cast<u32>(row.flipbookMidKeyTime) == sys[2].U());
        CHECK(std::bit_cast<u32>(row.flipbookColumns) == sys[3].U());

        // The remapping table is not part of this row: it maps the emitter's
        // batch slot to the row index. The binary resolves `iBatchIndex`
        // through it before indexing any per-batch array; the shipped
        // `Particle.fx` does not declare it at all and indexes the eight-slot
        // arrays with the raw vertex lane, which is the one respect the source
        // tree is a revision behind (RE §8.3). Our renderer draws one emitter
        // per batch, so its slot is its row and the mapping is the identity
        // either way. Asserted so that a golden which stopped agreeing says so.
        CHECK(std::bit_cast<f32>(out["remap"][0].U())
              == static_cast<f32>(in["row"].U()));
        for (std::size_t j = 1; j < 4; ++j)
            CHECK(poisoned(out["remap"][j].U()));

        replayed++;
        if (d.worldSpace)
            suppressed++;
        if (d.flipbookColumns == 0 || d.flipbookRows == 0)
            degraded++;
        if (f.hasInstanceNode)
            instanced++;
    }

    CHECK(replayed >= 70);
    CHECK(suppressed >= 3);
    CHECK(degraded >= 3);
    CHECK(instanced >= 3);
    CHECK(infinite >= 4);
}

// ============================================================================
// The joins. Every kernel above is measured against a golden one at a time;
// what follows is the wiring between them, which no golden covers — and this
// phase's own finding is that the seam between two gates is the part neither
// measures.
// ============================================================================

TEST_CASE("compose: the pool links, retires and recycles in retail's order",
          "[sc2_particle][compose]") {
    particle::Sc2ParticleStore store;
    store.Init(8);
    REQUIRE(store.Capacity() == 8u);
    REQUIRE(store.AliveCount() == 0u);

    std::vector<i32> walk;
    store.list.Walk(walk);
    CHECK(walk.empty());
    // Everything free, in index order — the opposite of `Sc2ElementList::Reset`,
    // which links a pool that is entirely LIVE.
    store.list.WalkFree(walk);
    CHECK(walk == std::vector<i32>{0, 1, 2, 3, 4, 5, 6, 7});

    // Five spawns. OP8b: elements come back in carve order and go into the
    // live list in APPEND order, so the head is the oldest.
    for (i32 i = 0; i < 5; ++i) {
        const i32 n = store.Acquire();
        REQUIRE(n == i);
        store.elements[static_cast<std::size_t>(n)].deathTime =
            1.0f + static_cast<f32>(i);
        store.elements[static_cast<std::size_t>(n)].vbSlot = 100 + i;
    }
    CHECK(store.AliveCount() == 5u);
    store.list.Walk(walk);
    CHECK(walk == std::vector<i32>{0, 1, 2, 3, 4});
    store.list.WalkBackward(walk);
    CHECK(walk == std::vector<i32>{4, 3, 2, 1, 0});

    // Everything born at or before t = 3 dies: `deathTime <= emitterTime`.
    const u32 retired = particle::Sc2RetireExpired(
        store.list, store.elements, 3.0f, store.recycle);
    CHECK(retired == 3u);
    CHECK(store.AliveCount() == 2u);
    store.list.Walk(walk);
    CHECK(walk == std::vector<i32>{3, 4});
    // The reverse walk has to agree after the HEAD moved, not just after a
    // middle unlink — the tail lives in the sentinel and this is where a
    // hand-maintained tail goes wrong.
    store.list.WalkBackward(walk);
    CHECK(walk == std::vector<i32>{4, 3});
    CHECK(store.recycle.slots == std::vector<i32>{100, 101, 102});

    // The freed nodes went on the END of the free list, behind the three that
    // were never used — so the next spawns are 5, 6, 7 and only then 0, 1, 2.
    std::vector<i32> got;
    for (int i = 0; i < 6; ++i)
        got.push_back(store.Acquire());
    CHECK(got == std::vector<i32>{5, 6, 7, 0, 1, 2});
    CHECK(store.AliveCount() == 8u);
    // Full. Retail tests the ceiling per element and stops; nothing is evicted
    // to make room.
    CHECK(store.Acquire() == -1);
    CHECK(store.AliveCount() == 8u);

    store.list.Walk(walk);
    CHECK(walk == std::vector<i32>{3, 4, 5, 6, 7, 0, 1, 2});
}

TEST_CASE("compose: an empty pool draws nothing and a full one draws in order",
          "[sc2_particle][compose]") {
    particle::Sc2ParticleStore store;
    store.Init(4);

    particle::Sc2QuadBatch batch;
    batch.systemTime = 1.0f;
    batch.elementScale = 1.0f;
    particle::Sc2QuadCamera cam;
    particle::Sc2QuadFlags fl;

    std::vector<renderer::Vertex> out;
    CHECK(particle::Sc2BuildQuads(store, batch, cam, fl, false, out) == 0u);
    CHECK(out.empty());

    for (i32 i = 0; i < 3; ++i) {
        const i32 n = store.Acquire();
        auto& v = store.vertices[static_cast<std::size_t>(n)];
        v.position[0] = 10.0f * static_cast<f32>(i);
        v.position[1] = 0.0f;
        v.position[2] = 0.0f;
        for (auto& sz : v.size)
            sz = 256;
        // Three DIFFERENT nodes with four different bytes each: a decode that
        // rotates the channels, or one that reads node 1 for node 0, has to
        // change an answer here. A single colour repeated three times - which
        // is what this fixture held first - could not tell any of that apart.
        v.color[0] = 0xFF102030u;
        v.color[1] = 0xC0405060u;
        v.color[2] = 0x807080A0u;
        v.rotation[0] = 32;
        v.rotation[1] = 0;
        v.rotation[2] = 0;
        // Read only through the random-UV arm, and zero here until 2026-09-10:
        // dropping it from `Sc2QuadInputFrom` stayed green over the whole
        // suite, which is a hole in this fixture and not in the kernel.
        v.flipbookRand = 0x0507u;
        v.noise[0] = 0.5f;
        v.noise[1] = -0.25f;
        v.noise[2] = 2.0f;
        v.birthTime = 0.0f;
        v.deathTime = 2.0f;
        v.drag = 0.01f;
        v.invDrag = 100.0f;
        v.invMass = 1.0f;
    }

    out.clear();
    CHECK(particle::Sc2BuildQuads(store, batch, cam, fl, false, out) == 3u);
    REQUIRE(out.size() == 18u);

    // Two triangles per particle, wound c0 c1 c2 / c3 c2 c1 — the same winding
    // every other dialect emits, so one pipeline state draws all of them.
    for (std::size_t p = 0; p < 3; ++p) {
        const auto q = particle::Sc2ExpandQuad(
            particle::Sc2QuadInputFrom(store.vertices[p]), batch, cam, fl);
        REQUIRE(q.supported);
        const std::size_t order[6] = {0, 1, 2, 3, 2, 1};
        for (std::size_t k = 0; k < 6; ++k) {
            const auto& want = q.corner[order[k]];
            const auto& got = out[p * 6 + k];
            INFO("particle " << p << " vertex " << k);
            CHECK(got.position.x == want.position.x);
            CHECK(got.position.y == want.position.y);
            CHECK(got.position.z == want.position.z);
            CHECK(got.uv.x == want.uv.x);
            CHECK(got.uv.y == want.uv.y);
        }
    }
    // The colour is per PARTICLE, not per corner: all six carry it.
    for (std::size_t k = 1; k < 6; ++k)
        CHECK(out[k].color.x == out[0].color.x);

    // What the channels ARE, not merely that they agree. The nodes are packed
    // with alpha in the high byte then r, g, b - the packing OP6 pinned for
    // `Sc2SampleColor`. Nothing measures the vertex DECLARATION's own decode,
    // so this pins our side of it and says so.
    {
        const auto q = particle::Sc2QuadInputFrom(store.vertices[0]);
        constexpr f32 k = 1.0f / 255.0f;
        CHECK(q.color[0][0] == 0x10 * k);   // r
        CHECK(q.color[0][1] == 0x20 * k);   // g
        CHECK(q.color[0][2] == 0x30 * k);   // b
        CHECK(q.color[0][3] == 0xFF * k);   // a
        CHECK(q.color[1][0] == 0x40 * k);
        CHECK(q.color[1][3] == 0xC0 * k);
        CHECK(q.color[2][2] == 0xA0 * k);
        CHECK(q.color[2][3] == 0x80 * k);
        // The noise offset moves the particle; a build that dropped it left
        // every corner where it was.
        CHECK(q.noise.x == 0.5f);
        CHECK(q.noise.z == 2.0f);
        // `vRotation.w` is the flipbook random, and the random-UV arm is its
        // only reader.
        CHECK(q.rotation[3] == 1287.0f);
    }

    // The random-UV arm, so `.w` is not merely carried but READ.
    {
        particle::Sc2QuadFlags rnd = fl;
        rnd.uvRandomOffset = true;
        std::vector<renderer::Vertex> ruv;
        CHECK(particle::Sc2BuildQuads(store, batch, cam, rnd, false, ruv) == 3u);
        REQUIRE(ruv.size() == 18u);
        // 0x0507 splits to (5, 7) over 255 - a shift both axes can see.
        CHECK(ruv[0].uv.x != out[0].uv.x);
        CHECK(ruv[0].uv.y != out[0].uv.y);
        CHECK(std::fabs((ruv[0].uv.x - out[0].uv.x) - 5.0f / 255.0f) < 1e-6f);
        CHECK(std::fabs((ruv[0].uv.y - out[0].uv.y) - 7.0f / 255.0f) < 1e-6f);
    }

    // The reverse walk is the SortReverse order, and it is the only thing that
    // changes — same particles, same corners, opposite sequence.
    std::vector<renderer::Vertex> rev;
    CHECK(particle::Sc2BuildQuads(store, batch, cam, fl, true, rev) == 3u);
    REQUIRE(rev.size() == 18u);
    for (std::size_t p = 0; p < 3; ++p)
        for (std::size_t k = 0; k < 6; ++k)
            CHECK(rev[p * 6 + k].position.x == out[(2 - p) * 6 + k].position.x);

    // Every one of the shader's eleven types draws, and draws its OWN shape: a
    // tail is not the billboard with a different flag. Past the eleven there
    // is no branch to transcribe, so a type 11 still shows as nothing rather
    // than as the billboard the shader's final `else` would fall into.
    particle::Sc2QuadFlags tail = fl;
    tail.instanceType = 1;
    std::vector<renderer::Vertex> tailQuads;
    CHECK(particle::Sc2BuildQuads(store, batch, cam, tail, false, tailQuads) == 3u);
    REQUIRE(tailQuads.size() == out.size());
    bool reshaped = false;
    for (std::size_t k = 0; k < out.size(); ++k)
        reshaped |= tailQuads[k].position.x != out[k].position.x ||
                    tailQuads[k].position.y != out[k].position.y ||
                    tailQuads[k].position.z != out[k].position.z;
    CHECK(reshaped);

    particle::Sc2QuadFlags past = fl;
    past.instanceType = 11;
    std::vector<renderer::Vertex> none;
    CHECK(particle::Sc2BuildQuads(store, batch, cam, past, false, none) == 0u);
    CHECK(none.empty());
}

TEST_CASE("compose: the shader permutation follows the record",
          "[sc2_particle][compose]") {
    particle::Sc2EmitterDesc d;
    d.look.instanceType = 4;
    d.look.sizeSmoothing = 1;
    d.look.colorSmoothing = 2;
    d.look.rotationSmoothing = 3;
    d.motion.analytic = true;
    d.emit.worldSpace = false;
    d.flags = particle::ParticleFlag::RandomFlipbookStart;

    auto f = particle::Sc2QuadFlagsFrom(d, /*flipbookUv=*/true,
                                        /*uvRandomOffset=*/false);
    CHECK(f.instanceType == 4u);
    CHECK(f.sizeInterp == 1);
    CHECK(f.colorInterp == 2);
    CHECK(f.rotationInterp == 3);
    // `b_localSpace = !(additionalFlags & WorldSpace)` — the INVERSE, which is
    // the one place in this mapping a sign can be dropped silently.
    CHECK(f.localSpace);
    CHECK(f.proceduralPosition);
    CHECK(f.randomFlipbookStart);
    CHECK(f.flipbookUv);
    CHECK_FALSE(f.uvRandomOffset);
    // Never from the record: it is a render-context bit.
    CHECK_FALSE(f.modelInstancing);

    d.emit.worldSpace = true;
    d.motion.analytic = false;
    d.flags = particle::ParticleFlag::None;
    f = particle::Sc2QuadFlagsFrom(d, false, true);
    CHECK_FALSE(f.localSpace);
    CHECK_FALSE(f.proceduralPosition);
    CHECK_FALSE(f.randomFlipbookStart);
    CHECK(f.uvRandomOffset);
}

TEST_CASE("compose: the matrix and the camera keep the renderer's convention",
          "[sc2_particle][compose]") {
    // A matrix with a translation and no symmetry, so a transpose changes the
    // answer. `Matrix44f` is row-vector, row-major — the translation is row 3 —
    // and so is the layout the SC2 kernels read.
    Matrix44f m = Matrix44f::identity();
    m.data[0][0] = 2.0f;  m.data[0][1] = 0.5f;  m.data[0][2] = -1.0f;
    m.data[1][0] = 0.0f;  m.data[1][1] = 3.0f;  m.data[1][2] = 0.25f;
    m.data[2][0] = 1.5f;  m.data[2][1] = -2.0f; m.data[2][2] = 0.75f;
    m.data[3][0] = 10.0f; m.data[3][1] = -4.0f; m.data[3][2] = 7.0f;

    const Vector3f p{1.25f, -3.5f, 2.0f};
    const Vector3f want = whiteout::transform_point(p, m);
    const Vector3f got = vs::MulPointMat4(p, particle::Sc2Mat16(m));
    // The two associate their sums differently, so this is close, not equal —
    // what it pins is the LAYOUT, which a transpose would miss by whole units.
    CloseRel(got.x, want.x, 1e-6f, "MulPointMat4.x");
    CloseRel(got.y, want.y, 1e-6f, "MulPointMat4.y");
    CloseRel(got.z, want.z, 1e-6f, "MulPointMat4.z");

    // A camera built by hand, then recovered. No gate measures this
    // derivation — retail hands the shader the four uniforms from its render
    // context — so the round trip is what stands in for one.
    const Vector3f right{0.8f, 0.6f, 0.0f};
    const Vector3f up{-0.36f, 0.48f, 0.8f};
    const Vector3f dir{-0.48f, 0.64f, -0.6f};   // -(right x up)
    const Vector3f eye{3.0f, -14.0f, 9.0f};
    const Vector3f back{-dir.x, -dir.y, -dir.z};

    Matrix44f view = Matrix44f::identity();
    const Vector3f axis[3] = {right, up, back};
    for (int c = 0; c < 3; ++c) {
        view.data[0][c] = axis[c].x;
        view.data[1][c] = axis[c].y;
        view.data[2][c] = axis[c].z;
        view.data[3][c] = -(eye.x * axis[c].x + eye.y * axis[c].y + eye.z * axis[c].z);
    }

    const auto cam = particle::Sc2CameraFromView(view);
    CloseRel(cam.billboardRight.x, right.x, 1e-6f, "right.x");
    CloseRel(cam.billboardRight.y, right.y, 1e-6f, "right.y");
    CloseRel(cam.billboardUp.z, up.z, 1e-6f, "up.z");
    CloseRel(cam.direction.x, dir.x, 1e-6f, "direction.x");
    CloseRel(cam.direction.y, dir.y, 1e-6f, "direction.y");
    CloseRel(cam.direction.z, dir.z, 1e-6f, "direction.z");
    CloseRel(cam.eye.x, eye.x, 1e-5f, "eye.x");
    CloseRel(cam.eye.y, eye.y, 1e-5f, "eye.y");
    CloseRel(cam.eye.z, eye.z, 1e-5f, "eye.z");

    // The handedness the goldens fix: `direction` is MINUS the cross product,
    // which is the half a mirrored frame gets wrong while every length and
    // every angle still checks out.
    const Vector3f cross{right.y * up.z - right.z * up.y,
                         right.z * up.x - right.x * up.z,
                         right.x * up.y - right.y * up.x};
    CloseRel(cam.direction.x, -cross.x, 1e-6f, "direction = -(right x up)");
    CloseRel(cam.direction.z, -cross.z, 1e-6f, "direction = -(right x up)");
}

// ---------------------------------------------------------------------------
// The tick. Every kernel it calls is gated on its own; what these test is that
// each value reaches the kernel that reads it, in the order retail runs them.
// A number that looks right for the wrong reason is the whole risk here, so
// each case names the ONE join it would break.
// ---------------------------------------------------------------------------

namespace {

/// One emitter, driven a frame at a time.
struct Sc2Rig {
    particle::Sc2EmitterDesc d;
    particle::Sc2Runtime rt;
    particle::Sc2TickFrame f;
    /// What an emitter hands the tick as its surface.
    particle::EmitSurface surface;

    Sc2Rig(u32 maxParticles, f32 rate, f32 lifetime) {
        d.emit.maxParticles = maxParticles;
        rt.store.Init(maxParticles);
        // Analytic by default, and with the selector bit `SetDesc` sets for
        // it: these cases were written for the closed-form path, and a rig
        // that left `stateFlags & 0x10` clear would quietly run them on the
        // CPU step instead. The Euler cases opt out explicitly.
        d.motion.analytic = true;
        rt.clock.stateFlags |= 0x10u;
        rt.frame.active = true;
        rt.frame.emissionRate = rate;
        rt.frame.lifetime = lifetime;
        rt.frame.lifetimeRandom = lifetime;
        rt.frame.size3 = {1.0f, 1.0f, 1.0f};
        for (auto& c : rt.frame.colorBGRA)
            c = 0xFF204060u;
        f.timeScale = 1.0f;
        f.emissionScaler = 1.0f;
        f.quality = 4;
        f.elemScaleX = 1.0f;
    }

    particle::Sc2TickResult Step(f32 dt) {
        f.dtMs = static_cast<i32>(dt * 1000.0f + 0.5f);
        rt.wallMs += f.dtMs;
        f.nowMs = rt.wallMs;
        f.frameIndex = ++rt.frameIndex;
        f.surface = &surface;
        return particle::Sc2TickEmitter(rt, d, f);
    }

    u32 Run(int frames, f32 dt) {
        u32 spawned = 0;
        for (int i = 0; i < frames; ++i)
            spawned += Step(dt).spawned;
        return spawned;
    }
};

constexpr f32 kSixtieth = 1.0f / 60.0f;

} // namespace

TEST_CASE("compose: a frame of an emitter spawns, ages and retires",
          "[sc2_particle][compose]") {
    SECTION("the authored rate reaches the counter") {
        // The only thing separating "the rate is wired" from "something spawns"
        // is that a zero rate spawns NOTHING, over frames a non-zero one fills.
        Sc2Rig quiet(64, 0.0f, 100.0f);
        CHECK(quiet.Run(30, kSixtieth) == 0u);
        CHECK(quiet.rt.store.AliveCount() == 0u);

        Sc2Rig busy(64, 60.0f, 100.0f);
        const u32 n = busy.Run(30, kSixtieth);
        CHECK(n > 0u);
        CHECK(busy.rt.store.AliveCount() == n);
    }

    SECTION("the host's emission scaler reaches it too") {
        // Halving the scaler halves the spawns. A scaler dropped on the floor
        // would leave these equal, and one applied twice would quarter it.
        Sc2Rig full(512, 60.0f, 100.0f);
        Sc2Rig half(512, 60.0f, 100.0f);
        half.f.emissionScaler = 0.5f;
        const u32 a = full.Run(120, kSixtieth);
        const u32 b = half.Run(120, kSixtieth);
        CHECK(a > 0u);
        CHECK(b > 0u);
        CHECK(b < a);
        CHECK(std::abs(static_cast<i32>(a) - 2 * static_cast<i32>(b)) <= 2);
    }

    SECTION("an inactive emitter is silent") {
        // `active` is the transform node's visibility, and it is an EARLY zero
        // in the counter — not a draw-time skip.
        Sc2Rig rig(64, 60.0f, 100.0f);
        rig.rt.frame.active = false;
        CHECK(rig.Run(30, kSixtieth) == 0u);
    }

    SECTION("the lifetime reaches the element and the retirement reads it") {
        // A short life reaches a steady state; a long one keeps filling. Both
        // run the same number of frames at the same rate, so the only thing
        // that can separate them is the death time.
        Sc2Rig brief(512, 60.0f, 0.1f);
        Sc2Rig lasting(512, 60.0f, 100.0f);
        const u32 spawnedBrief = brief.Run(120, kSixtieth);
        lasting.Run(120, kSixtieth);
        CHECK(brief.rt.store.AliveCount() < lasting.rt.store.AliveCount());
        // 60/s for 0.1 s is about six alive at any moment.
        CHECK(brief.rt.store.AliveCount() <= 12u);
        CHECK(lasting.rt.store.AliveCount() > 100u);
        // Indices were REUSED: the short-lived emitter spawned many times what
        // it ever held at once, which is only possible if the retirement put
        // its nodes back on the free list.
        CHECK(spawnedBrief > brief.rt.store.AliveCount() * 4u);
        // The recycle array stays EMPTY, and that is not a leak. `vbSlot`
        // indexes retail's shared ParticleVB arena; this store keeps each
        // particle's vertex at the element's own index, so there is no second
        // index space to hand back and `Sc2RetireExpired` guards on
        // `vbSlot != -1`.
        CHECK(brief.rt.store.recycle.slots.empty());
    }

    SECTION("the pool cap is per element and the shortfall is not retried") {
        Sc2Rig rig(8, 6000.0f, 100.0f);
        u32 refused = 0;
        for (int i = 0; i < 10; ++i)
            refused += rig.Step(kSixtieth).refused;
        CHECK(rig.rt.store.AliveCount() == 8u);
        CHECK(refused > 0u);
        // Nothing was evicted to make room: retail tests the ceiling per
        // element and simply stops.
        CHECK(rig.rt.store.Capacity() == 8u);
    }

    SECTION("a batch is swept along the frame, not stacked on one instant") {
        // `spawnTimeStep` is `catchUp / total`, and `InitSpawnedParticles` adds
        // it per element. The spread has to be measured WITHIN one frame's
        // cohort: across frames the births differ anyway, so a sweep that never
        // reached the initialiser looked identical until 2026-09-10.
        Sc2Rig rig(4096, 3000.0f, 100.0f);
        rig.Run(3, kSixtieth);
        const f32 before = rig.rt.clock.emitterTime;
        rig.Step(kSixtieth);
        const f32 after = rig.rt.clock.emitterTime;

        std::vector<i32> walk;
        rig.rt.store.list.Walk(walk);
        std::vector<f32> births;
        std::vector<f32> cohort;
        for (const i32 n : walk) {
            const f32 b = rig.rt.store.elements[static_cast<std::size_t>(n)].birthTime;
            births.push_back(b);
            if (b > before)
                cohort.push_back(b);
        }
        REQUIRE(cohort.size() >= 4u);
        CHECK(std::adjacent_find(cohort.begin(), cohort.end()) == cohort.end());
        // It runs FORWARD: the head is the oldest, so births ascend.
        CHECK(std::is_sorted(births.begin(), births.end()));
        // And it stays INSIDE the frame it belongs to. The sweep begins where
        // the frame began, so nothing is born after the clock has arrived —
        // starting it at the frame's END instead shifts every birth past the
        // clock and nothing else changes.
        for (const f32 b : cohort) {
            CHECK(b > before);
            CHECK(b <= after);
        }
    }

    SECTION("the retirement reads the frame's END") {
        // A particle whose whole life fits inside one frame is born and retired
        // in that same frame. Retiring against the frame's START instead leaves
        // it alive for one more — a one-frame error that a steady-state count
        // absorbs, which is why this asks the question directly.
        Sc2Rig fleeting(64, 60.0f, 0.001f);
        const auto r = fleeting.Step(kSixtieth);
        CHECK(r.spawned > 0u);
        // All but the LAST die in the frame that bore them. The sweep lays the
        // batch from the frame's start to its end, so the final element is born
        // at exactly the instant the retirement then tests against, and
        // `deathTime <= emitterTime` is false for it by its whole lifetime.
        // That is the sweep working, not an off-by-one.
        CHECK(fleeting.rt.store.AliveCount() == 1u);
        CHECK(r.retired == r.spawned - 1u);

        // One authored just past the frame survives it.
        Sc2Rig surviving(64, 60.0f, 1.0f);
        const auto r2 = surviving.Step(kSixtieth);
        CHECK(r2.spawned > 0u);
        CHECK(surviving.rt.store.AliveCount() == r2.spawned);
    }

    SECTION("each emission slot reads its own rate") {
        // Slot 0 is the `PAR_` itself and 1..n are its `PARC` copies, each with
        // its own animated rate. One rate broadcast to every slot is invisible
        // until a second slot exists and disagrees.
        Sc2Rig rig(4096, 60.0f, 100.0f);
        rig.d.emit.slotBones = {0, 1};
        rig.rt.frame.slots.resize(1);
        rig.rt.frame.slots[0].emissionRate = 600.0f;
        const u32 n = rig.Run(30, kSixtieth);

        Sc2Rig same(4096, 60.0f, 100.0f);
        same.d.emit.slotBones = {0, 1};
        same.rt.frame.slots.resize(1);
        same.rt.frame.slots[0].emissionRate = 60.0f;
        const u32 m = same.Run(30, kSixtieth);

        CHECK(n > m);
        // Slot 1 asks for ten times slot 0, so the pair spawns about eleven
        // times what two equal slots would spawn one of.
        CHECK(n > 4u * m);
    }

    SECTION("a second tick in the same frame does nothing at all") {
        Sc2Rig rig(64, 60.0f, 100.0f);
        rig.Run(10, kSixtieth);
        const u32 alive = rig.rt.store.AliveCount();
        const f32 t = rig.rt.clock.emitterTime;

        // The same frame index, which is the guard `Sc2TickClock` keeps.
        rig.f.dtMs = 16;
        const auto again = particle::Sc2TickEmitter(rig.rt, rig.d, rig.f);
        CHECK_FALSE(again.plan.ticked);
        CHECK(again.spawned == 0u);
        CHECK(rig.rt.store.AliveCount() == alive);
        CHECK(rig.rt.clock.emitterTime == t);
    }

    SECTION("the clock tracks the wall rather than accumulating drift") {
        Sc2Rig rig(64, 0.0f, 100.0f);
        rig.Run(60, kSixtieth);
        // 60 frames of 17 ms — the integer millisecond is what the emitter is
        // handed, so the clock lands on the SUM OF THOSE, not on 60/60 s.
        CHECK(rig.rt.clock.emitterTime > 0.9f);
        CHECK(rig.rt.clock.emitterTime < 1.1f);
    }

    SECTION("what spawned reaches the vertex buffer") {
        Sc2Rig rig(64, 60.0f, 100.0f);
        rig.Run(10, kSixtieth);
        const u32 alive = rig.rt.store.AliveCount();
        REQUIRE(alive > 0u);

        particle::Sc2BatchFrame bf;
        bf.emitterTime = rig.rt.clock.emitterTime;
        particle::Sc2WriteQuadBatch(rig.rt.batch, particle::Sc2BatchDescFrom(rig.d), bf);
        particle::Sc2QuadCamera cam;
        const auto flags = particle::Sc2QuadFlagsFrom(rig.d, false, false);

        std::vector<renderer::Vertex> out;
        const usize drawn =
            particle::Sc2BuildQuads(rig.rt.store, rig.rt.batch, cam, flags, false, out);
        CHECK(drawn == alive);
        CHECK(out.size() == alive * 6u);
        // Nothing NaN reached the buffer — a zero mass or a zero mid time would
        // put an infinity through the interpolators and out here.
        for (const auto& v : out) {
            REQUIRE(std::isfinite(v.position.x));
            REQUIRE(std::isfinite(v.position.y));
            REQUIRE(std::isfinite(v.position.z));
            REQUIRE(std::isfinite(v.uv.x));
        }
        // And the quads have EXTENT. Finiteness alone passed for a whole
        // session while the tick zeroed each stored vertex instead of building
        // it: every particle then had size 0 at the origin, and a degenerate
        // quad is perfectly finite. Nothing drew in the viewer, and no
        // assertion here noticed.
        f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (const auto& v : out) {
            const f32 p[3] = {v.position.x, v.position.y, v.position.z};
            for (usize k = 0; k < 3; ++k) {
                lo[k] = (std::min)(lo[k], p[k]);
                hi[k] = (std::max)(hi[k], p[k]);
            }
        }
        CHECK(hi[0] - lo[0] > 0.0f);
        CHECK(hi[2] - lo[2] > 0.0f);
    }

    SECTION("the stored vertex is the element's, built once at spawn") {
        // `Sc2VertexBody` is what turns an initialised element into the vertex
        // the shader reads, and retail runs it exactly once, when the slot is
        // acquired. Every lane below is one the expander needs; a tick that
        // allocated the slot without building it left them all zero, which is
        // both a dead particle and an age of 0/0.
        Sc2Rig rig(64, 60.0f, 7.0f);
        rig.rt.frame.size3 = {2.0f, 3.0f, 4.0f};
        rig.d.motion.drag = 0.25f;
        rig.d.motion.gravity3 = {0.0f, 0.0f, -9.0f};
        rig.Run(5, kSixtieth);
        REQUIRE(rig.rt.store.AliveCount() > 0u);

        std::vector<i32> walk;
        rig.rt.store.list.Walk(walk);
        const auto n = static_cast<std::size_t>(walk.front());
        const auto& el = rig.rt.store.elements[n];
        const auto& gv = rig.rt.store.vertices[n];

        // The lifetime lanes: a zeroed pair makes `age` a 0/0 the whole
        // expander then carries.
        CHECK(gv.birthTime == el.birthTime);
        CHECK(gv.deathTime == el.deathTime);
        CHECK(gv.deathTime > gv.birthTime);
        // Size, colour and rotation, straight off the element.
        for (usize k = 0; k < 4; ++k)
            CHECK(gv.size[k] == el.size[k]);
        for (usize k = 0; k < 3; ++k) {
            CHECK(gv.color[k] == el.colorNodes[k]);
            CHECK(gv.rotation[k] == el.rotation[k]);
        }
        CHECK(gv.size[0] == 256u); // the authored 2, as a half extent x 256
        // The two lanes that are NOT the element's: the drag pair is floored
        // by `Sc2ComputeDragLanes` and the gravity is a product with the map's
        // scale, so a body that copied the element blind would miss both.
        CHECK(gv.drag == 0.25f);
        CHECK(gv.invDrag == 4.0f);
        CHECK(gv.gravityZ == -9.0f);

        // The map's scale is a real multiplier, not a decoration. Default 1
        // cannot tell "multiplied" from "dropped".
        Sc2Rig heavy(64, 60.0f, 7.0f);
        heavy.d.motion.gravity3 = {0.0f, 0.0f, -9.0f};
        heavy.f.worldGravityScale = 3.0f;
        heavy.Run(5, kSixtieth);
        REQUIRE(heavy.rt.store.AliveCount() > 0u);
        std::vector<i32> hw;
        heavy.rt.store.list.Walk(hw);
        CHECK(heavy.rt.store.vertices[static_cast<std::size_t>(hw.front())].gravityZ == -27.0f);

        // `vInterpolator2` is the INSTANCE vector, and for a billboard it is
        // zero whatever else is authored — so the type and the tail length
        // only become observable on a type that reads them. Type 1 puts the
        // tail length in .x.
        Sc2Rig tail(64, 60.0f, 7.0f);
        tail.d.look.instanceType = 1; // Tail
        tail.d.look.tailLength = 5.0f;
        tail.Run(5, kSixtieth);
        REQUIRE(tail.rt.store.AliveCount() > 0u);
        std::vector<i32> tw;
        tail.rt.store.list.Walk(tw);
        const auto& tv = tail.rt.store.vertices[static_cast<std::size_t>(tw.front())];
        CHECK(tv.instanceVec[0] == 5.0f);
        CHECK(tv.instanceVec[1] == 0.0f);
        CHECK(tv.instanceVec[2] == 0.0f);
    }

    SECTION("a spawn REQUEST builds its vertex too") {
        // The inbox is a separate spawn path with its own `Sc2InitSpawned`
        // call, so it has its own chance to allocate a slot and leave the
        // vertex behind. Nothing else in this file drives it far enough to
        // look at what it stored.
        Sc2Rig rig(64, 0.0f, 7.0f); // rate 0: every particle here is a request
        for (int i = 0; i < 4; ++i) {
            particle::SpawnRequest req;
            req.position = {static_cast<f32>(i), 0.0f, 0.0f};
            rig.rt.inbox.push_back(req);
            rig.Step(kSixtieth);
        }
        REQUIRE(rig.rt.store.AliveCount() == 4u);
        std::vector<i32> walk;
        rig.rt.store.list.Walk(walk);
        for (const i32 node : walk) {
            const auto k = static_cast<std::size_t>(node);
            const auto& el = rig.rt.store.elements[k];
            const auto& gv = rig.rt.store.vertices[k];
            CHECK(gv.deathTime == el.deathTime);
            CHECK(gv.deathTime > gv.birthTime);
            CHECK(gv.size[0] == el.size[0]);
        }
    }

    SECTION("the authored colour and size reach the element") {
        Sc2Rig rig(64, 60.0f, 100.0f);
        rig.rt.frame.size3 = {2.0f, 3.0f, 4.0f};
        rig.Run(5, kSixtieth);
        REQUIRE(rig.rt.store.AliveCount() > 0u);
        std::vector<i32> walk;
        rig.rt.store.list.Walk(walk);
        const auto& e = rig.rt.store.elements[static_cast<std::size_t>(walk.front())];
        // Colours arrive packed, the way `SampleParticleColor` produced them.
        CHECK(e.colorNodes[0] == 0xFF204060u);
        CHECK(e.colorNodes[2] == 0xFF204060u);
        // Sizes are HALF extents times 256 — the authored 2 becomes 256.
        CHECK(e.size[0] == 256u);
        CHECK(e.size[1] == 384u);
        CHECK(e.size[2] == 512u);
    }
}

TEST_CASE("op9: the CPU sub-step replays SimulateParticles",
          "[sc2_particle][oracle][op9]") {
    const fs::path path = GoldenDir() / "op9_simulate.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];

    // Every CPU row, the child queues included. The scale push has its own
    // case below; the pending-spawn gate and the model pose are X6's.
    static constexpr const char* kRows[] = {"select",    "gravity", "drag",   "wind",
                                            "freeze",    "collide", "toi",    "bounce",
                                            "rest",      "diebounce", "kill", "bounds",
                                            "cspawn",    "cdraw",   "trail",  "cap"};

    struct Probe {
        const wdx_golden::Value* terrain = nullptr;
        const wdx_golden::Value* objects = nullptr;
        std::vector<std::array<f32, 6>> segments;
    };

    unsigned replayed = 0;
    unsigned retireSelects = 0;
    unsigned queued = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const std::string tag = c["tag"].S();
        if (std::find(std::begin(kRows), std::end(kRows), tag) == std::end(kRows))
            continue;
        const auto& in = c["in"];
        const auto& out = c["out"];
        const auto& par = in["par"];
        INFO("case " << i << " tag=" << tag);

        // Every row but one `select` is a Simulate row. That one is the
        // selector's retire arm, which the retire case replays; anything else
        // picking Retire means the selector and the replay disagree.
        if (particle::Sc2UseRetirePath(in["stateFlags"].U(), in["forceCpu"].U() != 0)) {
            REQUIRE(tag == "select");
            ++retireSelects;
            continue;
        }

        const auto& src = in["elementsIn"];
        const std::size_t n = src.Size();
        std::vector<particle::Sc2SpawnedElement> elems(n);
        for (std::size_t k = 0; k < n; ++k) {
            const auto& e = src[k];
            elems[k].position = Vec3From(e["position"]);
            elems[k].velocity = Vec3From(e["velocity"]);
            elems[k].orientVec = Vec3From(e["orientVec"]);
            elems[k].invMass = e["invMass"].F();
            elems[k].birthTime = e["birthTime"].F();
            elems[k].deathTime = e["deathTime"].F();
            elems[k].trailAccum = e["trailAccum"].F();
            elems[k].flags = static_cast<u16>(e["flags"].U());
            elems[k].bounceCount = e["bounceCount"].I();
            elems[k].vbSlot = e["vbSlot"].I();
            for (std::size_t j = 0; j < 3; ++j)
                elems[k].rotation[j] = static_cast<u16>(e["rotation"][j].U());
        }

        const auto parF = [&](const char* key, f32 def) {
            return par.Has(key) ? par[key].F() : def;
        };
        const auto parU = [&](const char* key, u32 def) {
            return par.Has(key) ? par[key].U() : def;
        };
        particle::Sc2SimulateInputs si;
        si.dt = in["dt"].F();
        si.emitterTime = in["emitterTime"].F();
        si.gravity = Vec3From(in["gravity"]);
        si.gravityScale = in["gravityScale"].F();
        si.wind = Vec3From(in["wind"]);
        si.windMultiplier = in["windMultiplier"].F();
        si.parFlags = parU("flags", 0);
        si.instanceType = parU("instanceType", 0);
        si.drag = parF("drag", 0.0f);
        si.bounce = parF("bounce", 0.0f);
        si.friction = parF("friction", 0.0f);
        si.collisionDieBounce = parU("collisionDieBounce", 0);
        si.killRadius = parF("killRadius", 0.0f);
        si.origin = Vec3From(in["origin"]);
        si.collisionEnabled = in["collisionEnabled"].I() != 0;
        si.rotationSmoothing = static_cast<i32>(parU("rotationSmoothing", 0));
        si.rotationMidTime = parF("rotationMidTime", 0.0f);
        si.worldMatrix = particle::Sc2Mat16(MatFrom(in["worldMatrix"]));
        si.worldSpace = (parU("additionalFlags", 0) & 8u) != 0;
        si.trailRate = in["trailRate"].F();
        // The fixture's child 0 is always a world-space system, so its presence
        // is the whole of the collision-child test here.
        si.collisionChild = in["children"][0].I() != 0;
        si.trailChild = in["children"][1].I() != 0;
        si.collisionSpawnChance = parF("collisionSpawnChance", 0.0f);
        si.collisionSpawnMin = parU("collisionSpawnMin", 0);
        si.collisionSpawnMax = parU("collisionSpawnMax", 0);
        si.collisionSpawnEnergy = parF("collisionSpawnEnergy", 0.0f);

        // The two queries are the recorded trampolines: they answer with the
        // fixture's contact whatever segment they are asked about, which is
        // exactly what retail's call saw.
        Probe probe;
        probe.terrain = &in["hit"];
        probe.objects = &in["objectHit"];
        particle::Sc2Collider collider;
        collider.ctx = &probe;
        collider.terrain = [](void* ctx, const Vector3f& a, const Vector3f& b,
                              particle::Sc2Contact& o) {
            auto* p = static_cast<Probe*>(ctx);
            p->segments.push_back({a.x, a.y, a.z, b.x, b.y, b.z});
            o.hit = (*p->terrain)["on"].I() != 0;
            o.position = Vec3From((*p->terrain)["pos"]);
            o.normal = Vec3From((*p->terrain)["normal"]);
            o.toi = (*p->terrain)["toi"].F();
            return true;
        };
        collider.objects = [](void* ctx, const Vector3f&, const Vector3f&,
                              particle::Sc2Contact& o) {
            auto* p = static_cast<Probe*>(ctx);
            o.hit = (*p->objects)["on"].I() != 0;
            o.position = Vec3From((*p->objects)["pos"]);
            o.normal = Vec3From((*p->objects)["normal"]);
            o.toi = (*p->objects)["toi"].F();
            return true;
        };

        particle::Sc2ElementList list;
        list.Reset(n);
        sc2::Rng rng(out["rngIn"][0].U(), out["rngIn"][1].U());
        particle::Sc2ChildRequests children;
        const auto res =
            particle::Sc2SimulateParticles(list, elems, si, collider, rng, children);

        const auto& want = out["elements"];
        REQUIRE(want.Size() == n);
        for (std::size_t k = 0; k < n; ++k) {
            INFO("element " << k);
            const auto& w = want[k];
            const Vector3f wp = Vec3From(w["position"]);
            const Vector3f wv = Vec3From(w["velocity"]);
            CHECK(elems[k].position.x == wp.x);
            CHECK(elems[k].position.y == wp.y);
            CHECK(elems[k].position.z == wp.z);
            CHECK(elems[k].velocity.x == wv.x);
            CHECK(elems[k].velocity.y == wv.y);
            CHECK(elems[k].velocity.z == wv.z);
            // The freeze normalise goes through rsqrt + one Newton step.
            const Vector3f wo = Vec3From(w["orientVec"]);
            CloseRel(elems[k].orientVec.x, wo.x, kRsqrtRtol, "orientVec.x");
            CloseRel(elems[k].orientVec.y, wo.y, kRsqrtRtol, "orientVec.y");
            CloseRel(elems[k].orientVec.z, wo.z, kRsqrtRtol, "orientVec.z");
            CHECK(elems[k].flags == w["flags"].U());
            CHECK(elems[k].bounceCount == w["bounceCount"].I());
            CHECK(elems[k].deathTime == w["deathTime"].F());
            CHECK(elems[k].trailAccum == w["trailAccum"].F());
            CHECK(elems[k].vbSlot == w["vbSlot"].I());
            for (std::size_t j = 0; j < 3; ++j)
                CHECK(elems[k].rotation[j] == w["rotation"][j].U());
        }

        std::vector<i32> alive, back, freed;
        list.Walk(alive);
        list.WalkBackward(back);
        list.WalkFree(freed);
        const auto sameList = [](const std::vector<i32>& got, const wdx_golden::Value& w) {
            if (got.size() != w.Size())
                return false;
            for (std::size_t k = 0; k < got.size(); ++k)
                if (got[k] != w[k].I())
                    return false;
            return true;
        };
        CHECK(sameList(alive, out["alive"]));
        CHECK(sameList(back, out["aliveReverse"]));
        CHECK(sameList(freed, out["freed"]));
        CHECK(list.poolCount == out["poolCount"].U());
        CHECK(n - res.killed == out["elementCount"].U());

        const Vector3f bmin = Vec3From(out["boundsMin"]);
        const Vector3f bmax = Vec3From(out["boundsMax"]);
        CHECK(res.boundsMin.x == bmin.x);
        CHECK(res.boundsMin.y == bmin.y);
        CHECK(res.boundsMin.z == bmin.z);
        CHECK(res.boundsMax.x == bmax.x);
        CHECK(res.boundsMax.y == bmax.y);
        CHECK(res.boundsMax.z == bmax.z);

        // The stream comes out where retail left it: untouched on every row
        // that draws nothing, and word for word through the chance roll, the
        // count and the trail directions on the rows that do.
        CHECK(rng.acc() == out["rngOut"][0].U());
        CHECK(rng.idx4() == out["rngOut"][1].U());

        // One `collide` in the log per terrain query retail made, over the SAME
        // world-space segment — so a step that queried without the scene
        // switch, collided a dead particle or mapped the segment wrong shows up.
        std::vector<const wdx_golden::Value*> collides;
        const auto& log = out["log"];
        for (std::size_t k = 0; k < log.Size(); ++k)
            if (log[k]["ev"].S() == "collide")
                collides.push_back(&log[k]["seg"]);
        REQUIRE(probe.segments.size() == collides.size());
        for (std::size_t k = 0; k < collides.size(); ++k)
            for (std::size_t j = 0; j < 6; ++j)
                CHECK(probe.segments[k][j] == (*collides[k])[j].F());

        // What reached each child, through the receiver's 128 cap.
        const auto sameQueue = [&](const std::vector<particle::SpawnRequest>& asked,
                                   const wdx_golden::Value& want) {
            std::vector<particle::SpawnRequest> inbox;
            for (const auto& r : asked)
                particle::Sc2QueueSpawnRequest(inbox, r);
            REQUIRE(inbox.size() == want["count"].U());
            const auto& entries = want["entries"];
            for (std::size_t k = 0; k < entries.Size(); ++k) {
                const auto& r = inbox[k];
                const f32 lanes[9] = {r.position.x,      r.position.y,      r.position.z,
                                      r.velocityScale.x, r.velocityScale.y, r.velocityScale.z,
                                      r.orientVec.x,     r.orientVec.y,     r.orientVec.z};
                for (std::size_t j = 0; j < 9; ++j)
                    CHECK(lanes[j] == entries[k][j].F());
            }
            queued += static_cast<unsigned>(inbox.size());
        };
        sameQueue(children.collision, out["child0Requests"]);
        sameQueue(children.trail, out["child1Requests"]);
        ++replayed;
    }
    // X4's 53 rows, the two added with the kill-radius and dead-collide
    // findings, the two simulate `select` rows and fourteen child rows.
    CHECK(replayed == 71);
    CHECK(retireSelects == 1);
    // The child rows really queued something: the cap row alone is 128.
    CHECK(queued > 128u);
}

TEST_CASE("op9: Update pushes the basis row lengths onto the children",
          "[sc2_particle][oracle][op9]") {
    const fs::path path = GoldenDir() / "op9_simulate.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];

    unsigned rows = 0;
    unsigned pushes = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        if (c["tag"].S() != "scale")
            continue;
        const auto& in = c["in"];
        INFO("case " << i);
        const u32 rf = in["rotationFlags"].U();
        // One push per (bit, child) pair — 0x10 onto child 0, 0x20 onto child
        // 1 — and none for a child that is not there.
        unsigned want = 0;
        for (std::size_t k = 0; k < 2; ++k)
            if ((rf & (0x10u << k)) != 0 && in["children"][k].I() != 0)
                ++want;

        const Vector3f got =
            particle::Sc2ChildScale(particle::Sc2Mat16(MatFrom(in["worldMatrix"])));
        unsigned seen = 0;
        const auto& log = c["out"]["log"];
        for (std::size_t k = 0; k < log.Size(); ++k) {
            if (log[k]["ev"].S() != "setScale")
                continue;
            ++seen;
            const auto& s = log[k]["scale"];
            // Rows 0 and 1 carry the hardware rsqrt seed's bound; row 2 is an
            // exact root and is held to the bit.
            CloseRel(got.x, s[0].F(), kRsqrtRtol, "scale.x");
            CloseRel(got.y, s[1].F(), kRsqrtRtol, "scale.y");
            CHECK(got.z == s[2].F());
        }
        CHECK(seen == want);
        pushes += seen;
        ++rows;
    }
    CHECK(rows == 18u);
    CHECK(pushes > 0u);
}

TEST_CASE("compose: a pushed scale replaces the child bone's own",
          "[sc2_particle][compose][children]") {
    // RE §16.34: the push lands on the child's BONE as its local scale. So the
    // child's matrix is its rotation rows at the PUSHED lengths, its own
    // translation, times its parent — not its own scale times the push.
    const f32 c = std::cos(0.5f), s = std::sin(0.5f);
    Matrix44f local = Matrix44f::identity();
    local.data[0] = {c * 2.0f, s * 2.0f, 0.0f, 0.0f};  // length 2
    local.data[1] = {-s * 0.5f, c * 0.5f, 0.0f, 0.0f}; // length 0.5
    local.data[2] = {0.0f, 0.0f, 3.0f, 0.0f};          // length 3
    local.data[3] = {1.0f, 2.0f, 3.0f, 1.0f};
    Matrix44f parent = Matrix44f::identity();
    parent.data[0] = {0.0f, 1.0f, 0.0f, 0.0f};
    parent.data[1] = {-1.0f, 0.0f, 0.0f, 0.0f};
    parent.data[3] = {5.0f, -4.0f, 2.0f, 1.0f};
    const Matrix44f bone = local * parent;
    const Vector3f pushed{0.758f, 0.758f, 1.5f};

    SECTION("every row takes the pushed length and the position stays") {
        const Matrix44f got = particle::Sc2PushChildScale(bone, {2.0f, 0.5f, 3.0f}, pushed);
        Matrix44f want = local;
        want.data[0] = {c * 0.758f, s * 0.758f, 0.0f, 0.0f};
        want.data[1] = {-s * 0.758f, c * 0.758f, 0.0f, 0.0f};
        want.data[2] = {0.0f, 0.0f, 1.5f, 0.0f};
        want = want * parent;
        for (usize r = 0; r < 4; ++r) {
            for (usize k = 0; k < 4; ++k) {
                INFO("row " << r << " column " << k);
                CHECK(got.data[r][k] == Catch::Approx(want.data[r][k]).margin(1e-5));
            }
        }
    }

    SECTION("a row with no length keeps what it had") {
        const Matrix44f flat = particle::Sc2PushChildScale(bone, {2.0f, 0.0f, 3.0f}, pushed);
        for (usize k = 0; k < 4; ++k)
            CHECK(flat.data[1][k] == bone.data[1][k]);
    }
}

TEST_CASE("op11: the CPU quad builder writes the element's vertex",
          "[sc2_particle][oracle][op11]") {
    const fs::path path = GoldenDir() / "op11_quadverts.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");
    REQUIRE(sizeof(particle::Sc2GpuVertex) == 116);

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];

    // The fixture's `SampleVectorField2D` trampoline answers with four
    // distinct lanes so the caller's choice is visible; the rows read lanes 0
    // and 1 back as 0.125 and 0.375.
    struct Field {
        unsigned calls = 0;
        f32 x = 0.0f, y = 0.0f;
    };
    const auto fieldFn = [](void* ctx, f32 x, f32 y, f32 out[2]) {
        auto* fd = static_cast<Field*>(ctx);
        ++fd->calls;
        fd->x = x;
        fd->y = y;
        out[0] = 0.125f;
        out[1] = 0.375f;
    };

    unsigned replayed = 0, appendRows = 0, nodeRows = 0, packedRows = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& out = c["out"];
        const std::string entry = in["entry"].S();
        INFO("case " << i << " tag=" << c["tag"].S() << " entry=" << entry);

        // `AppendParticleVertices` is the GPU batch path's builder and a
        // different function (§16.12); the render-context node transform and
        // the packed ranged walk are not recorded as inputs.
        if (entry == "append") {
            ++appendRows;
            continue;
        }
        if (in["hasNode"].I() != 0) {
            ++nodeRows;
            continue;
        }
        if (in.Has("packed")) {
            ++packedRows;
            continue;
        }

        particle::Sc2CpuVertexInputs cin;
        cin.instanceType = in["instanceType"].U();
        cin.tailLength = in["tailLength"].F();
        cin.instanceAngle = Vec3From(in["instanceAngle"]);
        cin.drag = in["drag"].F();
        cin.gravity = in["gravity"].F();
        cin.gravityScale = in["gravityScale"].F();
        cin.sizeMidTime = in["sizeMidTime"].F();
        cin.parFlags = in["parFlags"].U();
        cin.noise = in["noise"].I() != 0;
        cin.noiseAmplitude = in["noiseAmplitude"].F();
        cin.noiseFrequency = in["noiseFrequency"].F();
        cin.noiseCoherence = in["noiseCoherence"].F();
        cin.noiseEdge = in["noiseEdge"].F();
        cin.emitterTime = in["emitterTime"].F();
        cin.gpuMotion = in["gpuMotion"].I() != 0;
        cin.batchIndex = in["batchIndex"].U();
        Field field;
        if (in["hasScene"].I() != 0) {
            cin.fieldCtx = &field;
            cin.field = fieldFn;
        }

        const bool instanced = in["instanced"].I() != 0;
        const auto& src = in["elementsIn"];
        const std::size_t n = (std::min)(static_cast<std::size_t>(in["maxCount"].U()), src.Size());
        REQUIRE(out["counter"].U() == n);
        REQUIRE(out["bytesWritten"].U() == n * (instanced ? 112u : 464u));
        REQUIRE(out["vertices"].Size() == n);

        for (std::size_t p = 0; p < n; ++p) {
            INFO("particle " << p);
            // The builders write the chain they are handed and the `order`
            // rows hand it tail-first, so each written vertex is matched to
            // its element by the position lane rather than by index.
            const u32 firstWord = out["vertices"][p][0][0].U();
            std::size_t srcIndex = src.Size();
            for (std::size_t k = 0; k < src.Size(); ++k) {
                const f32 x = src[k]["position"][0].F();
                u32 bits = 0;
                std::memcpy(&bits, &x, sizeof(bits));
                if (bits == firstWord) {
                    srcIndex = k;
                    break;
                }
            }
            REQUIRE(srcIndex < src.Size());
            const auto& g = src[srcIndex];
            particle::Sc2SpawnedElement e;
            e.position = Vec3From(g["position"]);
            e.velocity = Vec3From(g["velocity"]);
            e.orientVec = Vec3From(g["orientVec"]);
            e.spawnOrigin = Vec3From(g["spawnOrigin"]);
            e.noiseVec = Vec3From(g["noiseVec"]);
            e.invMass = g["invMass"].F();
            e.noisePhase = g["noisePhase"].F();
            e.birthTime = g["birthTime"].F();
            e.deathTime = g["deathTime"].F();
            e.flipbookRandStart = g["flipbookRandStart"].F();
            e.flipbookRand = static_cast<u16>(g["flipbookRand"].U());
            for (std::size_t j = 0; j < 4; ++j)
                e.size[j] = static_cast<u16>(g["size"][j].U());
            for (std::size_t j = 0; j < 3; ++j) {
                e.colorNodes[j] = g["colorNodes"][j].U();
                e.rotation[j] = static_cast<u16>(g["rotation"][j].U());
            }

            // The element's GPU lanes, which this store keeps in the vertex.
            particle::Sc2GpuVertex cache{};
            cache.positionW = g["posW"].U();
            cache.drag = g["drag"].F();
            cache.invDrag = g["invDrag"].F();
            // The `hole` rows poison this lane with a marker the fixture writes
            // as a one-element list, to show a builder that skipped it.
            cache.batchIndex = g["batchIndex"].kind == wdx_golden::Value::Kind::Array
                                   ? g["batchIndex"][0].U()
                                   : g["batchIndex"].U();
            const Vector3f gv = Vec3From(g["gpuVelocity"]);
            cache.velocity[0] = gv.x;
            cache.velocity[1] = gv.y;
            cache.velocity[2] = gv.z;
            cache.invMass = g["gpuInvMass"].F();
            const Vector3f iv = Vec3From(g["instanceVec"]);
            cache.instanceVec[0] = iv.x;
            cache.instanceVec[1] = iv.y;
            cache.instanceVec[2] = iv.z;
            cache.gravityZ = g["gravityZ"].F();

            particle::Sc2CpuVertexBody(cin, e, cache);

            // The clamp's short branch and the noise go through rsqrt; every
            // other lane is a copy or a product and has to match to the bit.
            const bool tailTime =
                (cin.instanceType == 1 || cin.instanceType == 10) &&
                (cin.parFlags & 0x40000u) != 0 && !cin.gpuMotion;
            const auto approx = [&](std::size_t w) {
                return (tailTime && w == 20) || (cin.noise && w >= 24 && w <= 26);
            };
            const auto& wantV = out["vertices"][p];
            // One 112-byte vertex per particle when the device instances,
            // otherwise the same 112 bytes four times, each with its corner.
            const std::size_t nv = instanced ? 1 : 4;
            REQUIRE(wantV.Size() == nv);
            for (std::size_t k = 0; k < nv; ++k) {
                const auto& words = wantV[k];
                particle::Sc2GpuVertex v = cache;
                v.corner[0] = particle::kSc2Corners[k][0];
                v.corner[1] = particle::kSc2Corners[k][1];
                u32 got[29];
                std::memcpy(got, &v, sizeof(got));
                const std::size_t nw = instanced ? 28 : 29;
                REQUIRE(words.Size() == nw);
                for (std::size_t w = 0; w < nw; ++w) {
                    INFO("vertex " << k << " word " << w);
                    if (approx(w)) {
                        f32 gf, wf;
                        const u32 ww = words[w].U();
                        std::memcpy(&gf, &got[w], 4);
                        std::memcpy(&wf, &ww, 4);
                        CloseRel(gf, wf, kRsqrtRtol, "lane");
                    } else {
                        CHECK(got[w] == words[w].U());
                    }
                }
            }

            const auto& oe = out["elements"][srcIndex];
            const Vector3f wsO = Vec3From(oe["spawnOrigin"]);
            CHECK(e.spawnOrigin.x == wsO.x);
            CHECK(e.spawnOrigin.y == wsO.y);
            CHECK(e.spawnOrigin.z == wsO.z);
            for (std::size_t j = 0; j < 3; ++j)
                CHECK(e.rotation[j] == oe["rotation"][j].U());
            const Vector3f wn = Vec3From(oe["noiseVec"]);
            CloseRel(e.noiseVec.x, wn.x, kRsqrtRtol, "noiseVec.x");
            CloseRel(e.noiseVec.y, wn.y, kRsqrtRtol, "noiseVec.y");
            CloseRel(e.noiseVec.z, wn.z, kRsqrtRtol, "noiseVec.z");
        }

        // The field is asked about the particle's own x and y, once.
        if (cin.field && (cin.instanceType == 5 || cin.instanceType == 6)) {
            unsigned events = 0;
            const auto& log = out["log"];
            for (std::size_t k = 0; k < log.Size(); ++k) {
                if (log[k]["ev"].S() == "field") {
                    ++events;
                    CHECK(field.x == log[k]["x"].F());
                    CHECK(field.y == log[k]["y"].F());
                }
            }
            CHECK(field.calls == events);
        }
        ++replayed;
    }
    // A replay that skipped everything would be green too.
    CHECK(replayed >= 70);
    WARN("op11 replayed " << replayed << "; skipped append " << appendRows << ", node "
                          << nodeRows << ", packed " << packedRows);
}

TEST_CASE("compose: an Euler emitter moves, lands and draws where it is",
          "[sc2_particle][compose]") {
    // The CPU step and its vertex refresh, measured through the tick. Every
    // kernel under this is gated on its own; what these rows pin is that the
    // tick reaches them — that `stateFlags & 0x10` selects the step, that a
    // host ground query becomes a collider, and that the vertex the draw
    // expands is rebuilt from the element after it moved. A tick that did none
    // of that drew every Euler particle at its birth point, and every kernel
    // gate stayed green while it did.
    constexpr f32 kGround = -2.0f;
    const auto euler = [](Sc2Rig& rig) {
        rig.d.motion.analytic = false;
        rig.rt.clock.stateFlags &= ~0x10u;
        rig.d.motion.gravity3 = {0.0f, 0.0f, -9.8f};
        rig.d.flags = particle::ParticleFlag::CollideTerrain;
    };
    const auto ground = [](const Vector3f& p, f32 up, f32 down, f32& outZ) {
        if (kGround > p.z + up || kGround < p.z - down)
            return false;
        outZ = kGround;
        return true;
    };

    SECTION("particles fall, land on the ground and rest there") {
        Sc2Rig rig(256, 30.0f, 100.0f);
        euler(rig);
        rig.surface.groundQuery = ground;
        REQUIRE(rig.Run(180, kSixtieth) > 0u);

        std::vector<i32> alive;
        rig.rt.store.list.Walk(alive);
        REQUIRE_FALSE(alive.empty());
        u32 landed = 0;
        for (const i32 n : alive) {
            const auto& e = rig.rt.store.elements[static_cast<usize>(n)];
            INFO("element " << n << " z=" << e.position.z);
            // Nothing passes through: the push-out leaves a landed particle
            // 0.05 above the surface and a falling one is above it anyway.
            CHECK(e.position.z >= kGround + 0.05f - 1e-4f);
            if ((e.flags & 0x40u) != 0) {
                ++landed;
                CHECK(e.position.z == kGround + 0.05f);
                CHECK(e.velocity.x == 0.0f);
                CHECK(e.velocity.y == 0.0f);
                CHECK(e.velocity.z == 0.0f);
            }
            // The vertex the draw expands IS the element's position now, not
            // the one written at spawn.
            const auto& v = rig.rt.store.vertices[static_cast<usize>(n)];
            CHECK(v.position[0] == e.position.x);
            CHECK(v.position[1] == e.position.y);
            CHECK(v.position[2] == e.position.z);
        }
        // Three seconds at 30 a second, and a fall of two units takes 0.64 s:
        // most of the pool has landed.
        CHECK(landed > alive.size() / 2);
    }

    SECTION("without a ground query nothing is collided with") {
        Sc2Rig rig(256, 30.0f, 100.0f);
        euler(rig);
        REQUIRE(rig.Run(180, kSixtieth) > 0u);
        std::vector<i32> alive;
        rig.rt.store.list.Walk(alive);
        f32 lowest = 0.0f;
        for (const i32 n : alive)
            lowest = (std::min)(lowest, rig.rt.store.elements[static_cast<usize>(n)].position.z);
        CHECK(lowest < kGround - 10.0f);
    }

    SECTION("the analytic path still moves nothing on the CPU") {
        // Same emitter with the selector bit set: MOVE is the retirement
        // alone, so the element keeps its birth position and the shader does
        // the rest from the birth state.
        Sc2Rig rig(256, 30.0f, 100.0f);
        rig.d.motion.gravity3 = {0.0f, 0.0f, -9.8f};
        rig.surface.groundQuery = ground;
        REQUIRE(rig.Run(180, kSixtieth) > 0u);
        std::vector<i32> alive;
        rig.rt.store.list.Walk(alive);
        REQUIRE_FALSE(alive.empty());
        for (const i32 n : alive) {
            const auto& e = rig.rt.store.elements[static_cast<usize>(n)];
            CHECK(e.position.z == 0.0f);
            CHECK((e.flags & 0x40u) == 0u);
        }
    }
}

TEST_CASE("the runtime words Init derives from the record", "[sc2_particle][init]") {
    // `CParticleSystem::Init` (4.8 `0x102923140`). No oracle row records these
    // words, so this pins the decompile — and the ORDER of the writes is part
    // of it, because one of them is an assignment.
    particle::Sc2EmitterDesc d;
    const auto words = [&] { return particle::Sc2InitRuntimeWords(d); };

    SECTION("noise is on strictly above 0.001") {
        d.motion.noiseAmplitude = 0.001f;
        CHECK((words().emitFlags & sc2::kEmitNoise) == 0u);
        d.motion.noiseAmplitude = std::nextafter(0.001f, 1.0f);
        CHECK((words().emitFlags & sc2::kEmitNoise) != 0u);
        d.motion.noiseAmplitude = 0.0f;
        CHECK(words().emitFlags == 0u);
    }

    SECTION("the time bits come from the flags") {
        d.flags = particle::ParticleFlag::ScaleTimeByParent | particle::ParticleFlag::UseLocalTime;
        const u32 s = words().stateFlags;
        CHECK((s & sc2::kStateScaleTimeByParent) != 0u);
        CHECK((s & sc2::kStateUseLocalTime) != 0u);
    }

    SECTION("a fallback force pair ASSIGNS, and the time bits are lost") {
        d.flags = particle::ParticleFlag::ScaleTimeByParent | particle::ParticleFlag::UseLocalTime;
        d.motion.forcesFallback = 1;
        CHECK(words().stateFlags == static_cast<u32>(sc2::kStateForces));
    }

    SECTION("the writes after the assignment survive it") {
        d.motion.forcesFallback = 1;
        d.additionalFlags = static_cast<particle::ParticleAdditionalFlag>(8u);
        d.motion.forces = 0x10000u;
        d.flags = particle::ParticleFlag::InheritParentVelocity;
        // Contrived — a fallback pair demotes the emitter at load — but the
        // write order is what is under test, not the combination.
        d.motion.analytic = true;
        const u32 want = static_cast<u32>(sc2::kStateForces) | sc2::kStateWorldSpace |
                         sc2::kStateWorldForces | sc2::kStateInheritVelocity |
                         sc2::kStateGpuMotion;
        CHECK(words().stateFlags == want);
    }

    SECTION("world forces start at the second word") {
        d.motion.forces = 0xFFFFu;
        CHECK((words().stateFlags & sc2::kStateWorldForces) == 0u);
    }

    SECTION("an Euler emitter clears the selector bit") {
        d.motion.analytic = false;
        CHECK((words().stateFlags & sc2::kStateGpuMotion) == 0u);
    }
}

TEST_CASE("compose: a sorted emitter draws back to front", "[sc2_particle][compose]") {
    // `BuildRenderBatch_CPU` (RE §8.4): `Sort` keys each particle on the BIT
    // PATTERN of its depth along the camera, `SortHeight` on the bits of its
    // position z, both read off the element's own position, and the batch goes
    // out back to front unless `SortReverse` flips it. No golden records a
    // sorted order, so this pins the decompile's keys and the one choice made
    // where it is silent: ties keep the walk order.
    particle::Sc2ParticleStore store;
    store.Init(4);
    particle::Sc2QuadBatch batch;
    batch.systemTime = 1.0f;
    batch.elementScale = 1.0f;
    // Looking down +Y from the origin, so depth is the position's y.
    particle::Sc2QuadCamera cam;
    particle::Sc2QuadFlags fl;

    // Spawned in an order that is neither sorted nor reverse-sorted.
    constexpr f32 kDepth[3] = {5.0f, 20.0f, 10.0f};
    for (std::size_t i = 0; i < 3; ++i) {
        const i32 n = store.Acquire();
        auto& v = store.vertices[static_cast<std::size_t>(n)];
        v.position[0] = 0.0f;
        v.position[1] = kDepth[i];
        v.position[2] = 0.0f;
        for (auto& sz : v.size)
            sz = 256;
        v.birthTime = 0.0f;
        v.deathTime = 2.0f;
        v.drag = 0.01f;
        v.invDrag = 100.0f;
        v.invMass = 1.0f;
    }

    // The depth each written quad was drawn at: its corners share the y.
    const auto drawnDepths = [&](bool reverse, particle::Sc2SortKey sort) {
        std::vector<renderer::Vertex> q;
        REQUIRE(particle::Sc2BuildQuads(store, batch, cam, fl, reverse, q, sort) == 3u);
        return std::vector<f32>{q[0].position.y, q[6].position.y, q[12].position.y};
    };
    using Key = particle::Sc2SortKey;

    SECTION("unsorted keeps the list, both ways") {
        CHECK(drawnDepths(false, Key::None) == std::vector<f32>{5.0f, 20.0f, 10.0f});
        CHECK(drawnDepths(true, Key::None) == std::vector<f32>{10.0f, 20.0f, 5.0f});
    }

    SECTION("depth draws the farthest first, and SortReverse the nearest") {
        CHECK(drawnDepths(false, Key::Depth) == std::vector<f32>{20.0f, 10.0f, 5.0f});
        CHECK(drawnDepths(true, Key::Depth) == std::vector<f32>{5.0f, 10.0f, 20.0f});
    }

    SECTION("the depth runs along the camera, and folds behind the eye") {
        // With the eye at y 12 the depths are -7, 8 and -2. Retail keys their
        // bit patterns as ints, so every depth behind the eye sorts before the
        // one in front of it, and among themselves by magnitude, largest first:
        // -7 is drawn before -2, where a float key would draw -2 first.
        cam.eye = {0.0f, 12.0f, 0.0f};
        CHECK(drawnDepths(false, Key::Depth) == std::vector<f32>{20.0f, 5.0f, 10.0f});
        // Turned round, the depths are 7, -8 and 2, and in front of the eye the
        // key orders like depth again.
        cam.direction = {0.0f, -1.0f, 0.0f};
        CHECK(drawnDepths(false, Key::Depth) == std::vector<f32>{5.0f, 10.0f, 20.0f});
    }

    SECTION("SortHeight keys the bits of position z, and equal heights tie") {
        // All three at z 0: one key, so the walk order stands both ways.
        CHECK(drawnDepths(false, Key::Height) == std::vector<f32>{5.0f, 20.0f, 10.0f});
        CHECK(drawnDepths(true, Key::Height) == std::vector<f32>{10.0f, 20.0f, 5.0f});
        // The lane the first port read, `posW`, which nothing writes, moves
        // nothing.
        store.vertices[0].positionW = 1;
        store.vertices[1].positionW = 3;
        store.vertices[2].positionW = 2;
        CHECK(drawnDepths(false, Key::Height) == std::vector<f32>{5.0f, 20.0f, 10.0f});
        // Heights 3, 1 and 2: the highest first.
        store.vertices[0].position[2] = 3.0f;
        store.vertices[1].position[2] = 1.0f;
        store.vertices[2].position[2] = 2.0f;
        CHECK(drawnDepths(false, Key::Height) == std::vector<f32>{5.0f, 10.0f, 20.0f});
    }

    SECTION("a local-space emitter sorts on its elements' own positions") {
        // A y flip draws every particle at minus its local y. Retail keys the
        // element's own position all the same, so the order is the local depth
        // order — 20, 10, 5 — drawn at -20, -10 and -5.
        fl.localSpace = true;
        batch.prWorld = {1, 0, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        CHECK(drawnDepths(false, Key::Depth) == std::vector<f32>{-20.0f, -10.0f, -5.0f});
    }
}

TEST_CASE("compose: the squirt crossing owes each key once and primes a frame behind",
          "[sc2_particle][compose][squirt]") {
    // `Sc2CrossSquirtKeys` is the actor layer's half: every OP7b rule underneath
    // it is replayed on its own. What these rows pin is the join — which window
    // a frame asks for, when the cursor is primed, and what reaches the burst.
    particle::Sc2EmitterDesc d;
    d.emit.slotBones = {0};
    // Container 0's block ends at 600 and container 1's at 200. Container 3's
    // has a key and ends at 0, which the reader skips outright; container 2
    // has no track at all, like a global loop's. The key at 700 is authored
    // past its block's end, as `Artillery_Mengsk_COOP`'s shell key at 4200 is
    // on a 3000 ms `Attack`.
    d.emit.squirt = {{{0, 0.0f, 5.0f, 600}, {0, 100.0f, 7.0f, 600},
                      // 0xFFF0 is -16 read back signed: it owes nothing.
                      {0, 300.0f, 65520.0f, 600},
                      {0, 500.0f, 9.0f, 600}, {0, 700.0f, 19.0f, 600},
                      {1, 50.0f, 11.0f, 200}, {1, 100.0f, 17.0f, 200},
                      {3, 0.0f, 13.0f, 0}}};
    particle::Sc2SquirtMemory memory;
    const auto walk = [&](std::initializer_list<particle::Sc2ClockSample> players) {
        const std::vector<particle::Sc2ClockSample> list(players);
        return particle::Sc2CrossSquirtKeys(d, list, memory, 16);
    };
    const auto at = [&](u16 stc, i32 timeMs, bool loop = true) {
        return walk({{stc, timeMs, loop}});
    };

    SECTION("a sequence's first frame fires its frame-0 key") {
        // Primed at now - dt, so the first window is [-16, 0]. A cursor that
        // started AT 0 would lose the key to retail's index-0 search, and a
        // crossing that primed at now would report nothing at all.
        auto c = at(0, 0);
        REQUIRE(c.bursts.size() == 1u);
        CHECK(c.bursts[0] == 5u);

        CHECK(at(0, 16).bursts[0] == 0u);   // (0, 16]
        CHECK(at(0, 100).bursts[0] == 7u);  // (16, 100]
        CHECK(at(0, 250).bursts[0] == 0u);
        CHECK(at(0, 320).bursts[0] == 0u);  // the -16 key crossed, owing nothing
        CHECK(at(0, 520).bursts[0] == 9u);
    }

    SECTION("a sequence first sampled a frame in still fires its frame-0 key") {
        // The prime puts the cursor exactly ON now - dt, and the window starts
        // there inclusive. A cursor left one past it, or primed at now, would
        // start the window at 1 or at 16 and lose the key.
        CHECK(at(0, 16).bursts[0] == 5u);
        CHECK(at(0, 32).bursts[0] == 0u);
    }

    SECTION("a playhead that has not moved owes nothing") {
        at(0, 0);
        CHECK(at(0, 100).bursts[0] == 7u);
        // Retail's sink still holds the key at 100 and its next count would
        // read it again; a paused viewer would refire it every frame.
        CHECK(at(0, 100).bursts[0] == 0u);
        CHECK(at(0, 100).bursts[0] == 0u);
    }

    SECTION("a loop wraps through the tail and the head") {
        at(0, 0);
        at(0, 450);
        // (450, end] then [0, 10]: the key at 500 before the wrap, the key at 0
        // after it — both of which retail's search would drop.
        CHECK(at(0, 10).bursts[0] == 5u + 9u);
    }

    SECTION("a clip that does not loop, stepping back, starts over") {
        at(0, 0, false);
        at(0, 450, false);
        // Treated as a restart: primed a frame behind 90, so (74, 90] — not
        // the tail from 450 a wrap would have reported.
        CHECK(at(0, 90, false).bursts[0] == 0u);
        CHECK(at(0, 100, false).bursts[0] == 7u);
    }

    SECTION("a new container primes against its own keys") {
        at(0, 0);
        at(0, 200);
        auto c = at(1, 60);
        CHECK(c.bursts[0] == 11u); // [44, 60] on container 1's table
        CHECK(at(1, 100).bursts[0] == 17u); // (60, 100]
    }

    SECTION("a container other than the first keeps its cursor between frames") {
        // A steady player on container 1 reads (61, 150], so the key at 100
        // fires. A reader that forgot which containers it walked would treat
        // every frame as a sequence change, prime a frame behind 150 and
        // lose it.
        CHECK(at(1, 60).bursts[0] == 11u);
        CHECK(at(1, 150).bursts[0] == 17u);
    }

    SECTION("a looping player wraps on its track's end, not on a caller's modulo") {
        // The time arrives unwrapped, as the pose sampler takes it. A reader
        // that never wrapped it fired a looping sequence's keys on the first
        // cycle and never again. The key at 700 is never crossed: a tail that
        // reported it would owe 19 more on every wrap.
        at(0, 0);
        at(0, 450);
        CHECK(at(0, 610).bursts[0] == 5u + 9u); // (450, 600] then [0, 10]
        CHECK(at(0, 1100).bursts[0] == 7u + 9u); // (10, 500]
        CHECK(at(0, 1210).bursts[0] == 5u);     // (500, 600] then [0, 10]
    }

    SECTION("a clip that does not loop runs on past its track's end") {
        at(0, 0, false);
        at(0, 450, false);
        CHECK(at(0, 610, false).bursts[0] == 9u);  // (450, 610]
        CHECK(at(0, 710, false).bursts[0] == 19u); // (610, 710]: the key past the end
    }

    SECTION("a global loop above the sequence hides none of its keys") {
        // Container 2 carries no squirt track and plays at the higher
        // priority, as `GLstand` does. Reading the top layer's clock alone,
        // the sequence underneath never fired.
        CHECK(walk({{2, 5000, true}, {0, 0, true}}).bursts[0] == 5u);
        CHECK(walk({{2, 5016, true}, {0, 100, true}}).bursts[0] == 7u);
    }

    SECTION("every player with a track reads its own keys") {
        // Primed a frame behind each playhead: [84, 100] on container 0 and
        // [44, 60] on container 1, into the one sink.
        CHECK(walk({{0, 100, true}, {1, 60, true}}).bursts[0] == 7u + 11u);
        // Each keeps its own cursor: nothing new on container 0, while
        // container 1's player wraps at 200 through its key at 100 and back
        // onto its key at 50.
        CHECK(walk({{0, 120, true}, {1, 250, true}}).bursts[0] == 17u + 11u);
    }

    SECTION("a track that ends at frame 0 is skipped") {
        CHECK(walk({{3, 0, true}, {0, 0, true}}).bursts[0] == 5u);
        CHECK(walk({{3, 16, true}}).bursts[0] == 0u);
    }

    SECTION("a different player list primes every cursor") {
        walk({{2, 0, true}, {0, 0, true}});
        walk({{2, 16, true}, {0, 200, true}});
        // The sequence changed under the global loop: container 1 primed at
        // [44, 60], as a first frame would be.
        CHECK(walk({{2, 32, true}, {1, 60, true}}).bursts[0] == 11u);
    }

    SECTION("no player is not a sample") {
        CHECK(particle::Sc2CrossSquirtKeys(d, {}, memory, 16).bursts.empty());
        CHECK_FALSE(memory.valid);
    }
}

TEST_CASE("compose: a Bezier channel's sampled keys become its control point",
          "[sc2_particle][compose][bezier]") {
    // `UpdateAnimatedParams` (RE §5.4): with smoothing 2 the track holds the
    // value the curve passes THROUGH at the mid time, and the keys a particle
    // is born with carry the control point instead — every channel against
    // `sizeMidTime`.
    particle::Sc2EmitterDesc d;
    d.look.midTime[0] = 0.5f; // sizeMidTime
    d.look.midTime[1] = 0.9f; // the colour's own, which the pass never reads
    renderer::model::FrameState::ParticleFrameState::Sc2ParticleFrame s;
    s.size3 = {1.0f, 4.0f, 1.0f};
    s.sizeRandom3 = {1.0f, 2.0f, 1.0f};
    s.rotation3 = {0.0f, 3.0f, 0.0f};
    // Every byte 0x40 in the middle and 0 at the ends: a control point of 0x80
    // at t 0.5, and an overshoot that wraps in the byte pack at 0.9.
    s.colorBGRA[0] = 0u;
    s.colorBGRA[1] = 0x40404040u;
    s.colorBGRA[2] = 0u;
    s.colorRandomBGRA[0] = 0u;
    s.colorRandomBGRA[1] = 0x40404040u;
    s.colorRandomBGRA[2] = 0u;

    SECTION("nothing moves while every channel is linear") {
        particle::Sc2ConvertBezierKeys(s, d);
        CHECK(s.size3.y == 4.0f);
        CHECK(s.rotation3.y == 3.0f);
        CHECK(s.colorBGRA[1] == 0x40404040u);
    }

    SECTION("each Bezier channel converts, and only it") {
        d.look.sizeSmoothing = 2;
        d.look.colorSmoothing = 2;
        particle::Sc2ConvertBezierKeys(s, d);
        CHECK(s.size3.y == 7.0f);       // (4 - 0.25 - 0.25) / 0.5
        CHECK(s.sizeRandom3.y == 3.0f); // (2 - 0.25 - 0.25) / 0.5
        CHECK(s.rotation3.y == 3.0f);   // still linear
        CHECK(s.colorBGRA[1] == 0x80808080u);
        // With random colour off its keys are never sampled, and not converted.
        CHECK(s.colorRandomBGRA[1] == 0x40404040u);
    }

    SECTION("the colour's random converts with random colour on") {
        d.look.colorSmoothing = 2;
        d.emit.colorRandom = true;
        particle::Sc2ConvertBezierKeys(s, d);
        CHECK(s.colorRandomBGRA[1] == 0x80808080u);
    }

    SECTION("rotation converts, key and random, against sizeMidTime too") {
        d.look.rotationSmoothing = 2;
        d.look.midTime[2] = 0.9f;
        d.look.midTime[3] = 0.9f;
        s.rotationRandom3 = {0.0f, 1.0f, 0.0f};
        particle::Sc2ConvertBezierKeys(s, d);
        CHECK(s.rotation3.y == 6.0f);       // 3 / 0.5
        CHECK(s.rotationRandom3.y == 2.0f); // 1 / 0.5
        CHECK(s.size3.y == 4.0f);           // still linear
    }
}

TEST_CASE("compose: a squirt burst counts in every count its host frame makes",
          "[sc2_particle][compose][squirt]") {
    SECTION("once in a plain frame, and then it is gone") {
        Sc2Rig rig(256, 0.0f, 100.0f);
        rig.rt.slots.assign(1, particle::Sc2Runtime::Slot{});
        rig.rt.slots[0].burst = 10;
        const auto r = rig.Step(kSixtieth);
        CHECK(r.spawned == 10u);
        CHECK(rig.rt.slots[0].burst == 0u);
        CHECK(rig.Step(kSixtieth).spawned == 0u);
    }

    SECTION("once per pre-roll block, as retail's stale sink is read") {
        Sc2Rig rig(4096, 0.0f, 100.0f);
        rig.d.emit.preRollInit = 0.2f; // 200 ms: seven blocks
        rig.rt.activeSequence = 0;
        rig.rt.preRollPending = true;
        rig.rt.slots.assign(1, particle::Sc2Runtime::Slot{});
        rig.rt.slots[0].burst = 3;
        const auto r = rig.Step(kSixtieth);
        CHECK(r.preRollBlocks == 7u);
        CHECK(r.spawned == 21u);
        CHECK(rig.rt.slots[0].burst == 0u);
    }
}

TEST_CASE("compose: the pre-roll runs at creation and on a gap, never on a change alone",
          "[sc2_particle][compose][preroll]") {
    SECTION("an emitter created with the ask is populated before its first frame") {
        Sc2Rig rig(4096, 60.0f, 100.0f);
        rig.d.emit.preRollInit = 1.0f;
        rig.rt.activeSequence = 0;
        rig.rt.preRollPending = true;
        const auto r = rig.Step(kSixtieth);
        CHECK(r.preRollBlocks == 31u); // ceil(1000 / 33)
        // 60 a second over 31 blocks of 33 ms.
        CHECK(r.spawned >= 55u);
        CHECK(rig.rt.store.AliveCount() == r.spawned);
        // The last block stamped this frame, so the frame's own tick returned
        // at its once-per-frame test, as retail's does.
        CHECK_FALSE(r.plan.ticked);
        // And the emitter clock now runs ahead of the scene by the pre-roll.
        CHECK(rig.rt.timeOffset > 1.0f);
        CHECK((rig.rt.clock.stateFlags & 0x80000000u) == 0u);
    }

    SECTION("a sequence change on an emitter ticked every frame pre-rolls nothing") {
        Sc2Rig rig(4096, 60.0f, 100.0f);
        rig.d.emit.preRollInit = 1.0f;
        rig.rt.activeSequence = 0;
        rig.Run(10, kSixtieth);
        rig.rt.preRollPending = true;
        const auto r = rig.Step(kSixtieth);
        CHECK(r.preRollBlocks == 0u);
        CHECK(r.plan.ticked);
        // The bit is spent all the same.
        CHECK((rig.rt.clock.stateFlags & 0x80000000u) == 0u);
    }

    SECTION("resuming after a gap pre-rolls the gap, not the lifetime") {
        Sc2Rig rig(4096, 60.0f, 100.0f);
        rig.d.emit.preRollInit = 1.0f;
        rig.rt.activeSequence = 0;
        rig.Run(10, kSixtieth);
        rig.rt.wallMs += 200; // 217 ms since the last tick, over 2 * 17
        rig.rt.preRollPending = true;
        CHECK(rig.Step(kSixtieth).preRollBlocks == 7u); // ceil(217 / 33)
    }

    SECTION("no lifetime peak, no blocks") {
        Sc2Rig rig(4096, 60.0f, 100.0f);
        rig.d.emit.preRollInit = 0.0f;
        rig.rt.activeSequence = 0;
        rig.rt.preRollPending = true;
        const auto r = rig.Step(kSixtieth);
        CHECK(r.preRollBlocks == 0u);
        CHECK(r.plan.ticked);
    }
}

TEST_CASE("compose: the pre-roll follows the active sequence, not the player list",
          "[sc2_particle][compose][preroll]") {
    using Sample = particle::Sc2ClockSample;
    const auto player = [](u16 sequence, u16 priority, bool global, bool fading) {
        Sample s;
        s.sequence = sequence;
        s.priority = priority;
        s.global = global;
        s.blendingOut = fading;
        return s;
    };

    SECTION("the active sequence is the first player that is not fading out") {
        // A cross-fade reports the incoming sequence the moment the outgoing
        // one starts to fade: `GetActiveSequenceIndex` passes over flag 4.
        const std::vector<Sample> fading = {player(3, 0, false, true), player(5, 0, false, false)};
        CHECK(particle::Sc2ActiveSequence(fading) == 5);
        const std::vector<Sample> steady = {player(7, 0, false, false), player(5, 0, false, false)};
        CHECK(particle::Sc2ActiveSequence(steady) == 7);
        const std::vector<Sample> gone = {player(3, 0, false, true)};
        CHECK(particle::Sc2ActiveSequence(gone) == -1);
        CHECK(particle::Sc2ActiveSequence({}) == -1);
    }

    SECTION("a global loop loses a priority tie, as the oldest player") {
        // The playlist holds a concurrent global above the host play for the
        // blend budget. Retail started it with the animation state, and its
        // tie rule puts the newest player first.
        const std::vector<Sample> tie = {player(9, 0, true, false), player(2, 0, false, false)};
        CHECK(particle::Sc2ActiveSequence(tie) == 2);
        // At a higher priority it leads all the same.
        const std::vector<Sample> above = {player(9, 5, true, false), player(2, 0, false, false)};
        CHECK(particle::Sc2ActiveSequence(above) == 9);
        // A fading host play hands the tie back to the global.
        const std::vector<Sample> fadingHost = {player(9, 0, true, false),
                                                player(2, 0, false, true)};
        CHECK(particle::Sc2ActiveSequence(fadingHost) == 9);
        const std::vector<Sample> alone = {player(9, 0, true, false)};
        CHECK(particle::Sc2ActiveSequence(alone) == 9);
    }

    SECTION("a SimulateInit emitter asks when the sequence moves, and only then") {
        Sc2Rig rig(64, 0.0f, 100.0f);
        rig.d.flags = particle::ParticleFlag::SimulateInit;
        // The constructor's -1 makes the first resolution a change.
        particle::Sc2NoteActiveSequence(rig.rt, rig.d, 2);
        CHECK(rig.rt.preRollPending);
        CHECK(rig.rt.activeSequence == 2);
        // The same sequence again — whatever else the player list did — asks
        // nothing.
        rig.rt.preRollPending = false;
        particle::Sc2NoteActiveSequence(rig.rt, rig.d, 2);
        CHECK_FALSE(rig.rt.preRollPending);
        particle::Sc2NoteActiveSequence(rig.rt, rig.d, 4);
        CHECK(rig.rt.preRollPending);
        // Nothing left playing is a change too, and is remembered.
        rig.rt.preRollPending = false;
        particle::Sc2NoteActiveSequence(rig.rt, rig.d, -1);
        CHECK(rig.rt.preRollPending);
        CHECK(rig.rt.activeSequence == -1);
    }

    SECTION("an emitter without SimulateInit remembers nothing") {
        Sc2Rig rig(64, 0.0f, 100.0f);
        particle::Sc2NoteActiveSequence(rig.rt, rig.d, 2);
        CHECK_FALSE(rig.rt.preRollPending);
        CHECK(rig.rt.activeSequence == -1);
    }

    SECTION("the peak is read in the column the sequence's number names") {
        Sc2Rig one(4096, 60.0f, 100.0f);
        one.d.emit.preRollPeaks = {0.1f, 1.0f, 0.2f};
        one.d.emit.preRollInit = 5.0f;
        one.rt.activeSequence = 1;
        one.rt.preRollPending = true;
        CHECK(one.Step(kSixtieth).preRollBlocks == 31u); // ceil(1000 / 33)

        Sc2Rig two(4096, 60.0f, 100.0f);
        two.d.emit.preRollPeaks = {0.1f, 1.0f, 0.2f};
        two.d.emit.preRollInit = 5.0f;
        two.rt.activeSequence = 2;
        two.rt.preRollPending = true;
        CHECK(two.Step(kSixtieth).preRollBlocks == 7u); // ceil(200 / 33)
    }

    SECTION("an unbound lifetime budgets from its init value") {
        Sc2Rig rig(4096, 60.0f, 100.0f);
        rig.d.emit.preRollInit = 0.5f;
        rig.rt.activeSequence = 3;
        rig.rt.preRollPending = true;
        CHECK(rig.Step(kSixtieth).preRollBlocks == 16u); // ceil(500 / 33)
    }

    SECTION("no resolved sequence, no pre-roll") {
        Sc2Rig rig(4096, 60.0f, 100.0f);
        rig.d.emit.preRollInit = 1.0f;
        rig.rt.preRollPending = true; // armed, but +0x3F0 is still -1
        const auto r = rig.Step(kSixtieth);
        CHECK(r.preRollBlocks == 0u);
        CHECK(r.plan.ticked);
        CHECK((rig.rt.clock.stateFlags & 0x80000000u) == 0u);
    }
}

TEST_CASE("compose: requests go first, and wait while nothing can be made",
          "[sc2_particle][compose][requests]") {
    const auto request = [](f32 x) {
        particle::SpawnRequest q;
        q.position = {x, 0.0f, 0.0f};
        return q;
    };

    SECTION("request-born particles head the frame's list") {
        Sc2Rig rig(256, 600.0f, 100.0f);
        rig.rt.inbox = {request(100.0f), request(200.0f)};
        REQUIRE(rig.Step(kSixtieth).spawned > 2u);
        std::vector<i32> walk;
        rig.rt.store.list.Walk(walk);
        REQUIRE(walk.size() > 2u);
        CHECK(rig.rt.store.elements[static_cast<usize>(walk[0])].position.x == 100.0f);
        CHECK(rig.rt.store.elements[static_cast<usize>(walk[1])].position.x == 200.0f);
        for (std::size_t k = 2; k < walk.size(); ++k)
            CHECK(rig.rt.store.elements[static_cast<usize>(walk[k])].position.x == 0.0f);
        CHECK(rig.rt.inbox.empty());
    }

    SECTION("a full pool keeps its requests for the next spawn") {
        Sc2Rig rig(4, 600.0f, 100.0f);
        rig.Run(10, kSixtieth);
        REQUIRE(rig.rt.store.AliveCount() == 4u);
        rig.rt.inbox = {request(1.0f), request(2.0f), request(3.0f)};
        rig.Step(kSixtieth);
        // Nothing was initialised, so nothing zeroed the request count.
        CHECK(rig.rt.inbox.size() == 3u);
    }

    SECTION("a call that makes anything consumes them all, the refused included") {
        Sc2Rig rig(2, 0.0f, 100.0f);
        rig.rt.inbox = {request(1.0f), request(2.0f), request(3.0f)};
        const auto r = rig.Step(kSixtieth);
        CHECK(r.spawned == 2u);
        CHECK(r.refused == 1u);
        CHECK(rig.rt.inbox.empty());
    }

    SECTION("the inbox holds 128 and drops the rest") {
        std::vector<particle::SpawnRequest> inbox;
        for (int k = 0; k < 200; ++k)
            particle::Sc2QueueSpawnRequest(inbox, request(static_cast<f32>(k)));
        REQUIRE(inbox.size() == 128u);
        CHECK(inbox.back().position.x == 127.0f);
    }
}

TEST_CASE("compose: a PARC copy emits from its own bone at its own size",
          "[sc2_particle][compose][parc]") {
    Sc2Rig rig(512, 0.0f, 100.0f);
    rig.d.emit.slotBones = {0, 1};
    rig.rt.frame.slots.resize(1);
    rig.rt.frame.slots[0].emissionRate = 120.0f;
    Matrix44f bone = Matrix44f::identity();
    bone.data[0][0] = 2.0f;
    bone.data[1][1] = 2.0f;
    bone.data[2][2] = 2.0f;
    bone.data[3][0] = 10.0f;
    rig.rt.frame.slots[0].boneWorld = bone;
    REQUIRE(rig.Run(10, kSixtieth) > 0u);

    std::vector<i32> walk;
    rig.rt.store.list.Walk(walk);
    REQUIRE_FALSE(walk.empty());
    for (const i32 n : walk) {
        const auto& e = rig.rt.store.elements[static_cast<usize>(n)];
        INFO("element " << n);
        // `bone · inverse(emitter)` puts a Point copy on its bone, while the
        // particle still simulates in the emitter's own frame.
        CHECK(e.position.x == 10.0f);
        CHECK(e.position.y == 0.0f);
        // And the size ratio: the bone's largest axis over the emitter's. The
        // authored 1 is a half extent of 0.5, times 256, times 2.
        CHECK(e.size[0] == 256u);
    }
}

TEST_CASE("compose: a moving world-space emitter spawns every slot where the unit is",
          "[sc2_particle][compose][parc]") {
    // Reaper's jets: a world-space `PAR_` and its `PARC` copy on a walking unit,
    // at a frame rate that sub-steps. Each batch is swept from `prevPos`, slot 0
    // first; a `prevPos` stuck on the first frame put slot 0 half the walk behind.
    constexpr f32 kDt = 0.007f;
    constexpr f32 kSpeed = 3.0f;
    constexpr f32 kCopyY = -0.6f;
    Sc2Rig rig(4096, 300.0f, 10.0f);
    rig.d.additionalFlags = static_cast<particle::ParticleAdditionalFlag>(8u);
    rig.d.emit.worldSpace = true;
    rig.d.emit.slotBones = {0, 1};
    rig.rt.frame.slots.resize(1);
    rig.rt.frame.slots[0].emissionRate = 300.0f;

    const auto at = [](f32 x, f32 y) {
        Matrix44f m = Matrix44f::identity();
        m.data[3][0] = x;
        m.data[3][1] = y;
        return m;
    };
    f32 x = 0.0f;
    const auto step = [&] {
        x = kSpeed * kDt * static_cast<f32>(rig.rt.frameIndex + 1);
        rig.f.worldPos = {x, 0.0f, 0.0f};
        rig.f.worldMatrix = at(x, 0.0f);
        rig.f.boneMatrix = rig.f.worldMatrix;
        rig.rt.frame.slots[0].boneWorld = at(x, kCopyY);
        rig.Step(kDt);
    };
    for (int i = 0; i < 600; ++i)
        step();

    // A sweep spans the frames since the last sub-step (three here), and the
    // running target may overshoot it by a few steps; ten frames bounds both.
    const f32 reach = 10.0f * kSpeed * kDt;
    usize seen[2] = {0, 0};
    for (int i = 0; i < 20; ++i) {
        std::vector<u8> was(rig.rt.store.Capacity(), 0);
        std::vector<i32> before;
        rig.rt.store.list.Walk(before);
        for (const i32 n : before)
            was[static_cast<usize>(n)] = 1;
        step();
        std::vector<i32> after;
        rig.rt.store.list.Walk(after);
        for (const i32 n : after) {
            if (was[static_cast<usize>(n)] != 0)
                continue;
            const auto& e = rig.rt.store.elements[static_cast<usize>(n)];
            INFO("frame " << i << " element " << n << " at " << e.position.x << ", unit at " << x);
            const bool copy = std::fabs(e.position.y - kCopyY) < 1e-4f;
            REQUIRE((copy || std::fabs(e.position.y) < 1e-4f));
            ++seen[copy ? 1 : 0];
            CHECK(std::fabs(e.position.x - x) <= reach);
        }
    }
    CHECK(seen[0] > 0u);
    CHECK(seen[1] > 0u);
}

// ---------------------------------------------------------------------------
// X6 — the Mesh shape (OP13), the model-particle pose (OP14), the pending-spawn
// draw after MOVE (OP14b), and the float4 curve the pose reads colour through.
// ---------------------------------------------------------------------------

TEST_CASE("op10: EvalAnimCurve2D replays lane for lane", "[sc2_particle][oracle][op10]") {
    // Recorded with OP10 and never replayed until a consumer existed: the pose
    // reads a model particle's tint through it. It is NOT the shader's float3
    // overload — modes 1 and 4 differ — so the scalar port could not stand in.
    const fs::path path = GoldenDir() / "op10_curve2d.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0u);
    std::size_t lanes = 0;
    std::map<u32, std::size_t> modes;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& in = cases[i]["in"];
        const u32 mode = in["mode"].U();
        const f32 mid = in["mid"].F();
        const f32 hold = in["hold"].F();
        std::array<std::array<f32, 4>, 3> k{};
        for (std::size_t j = 0; j < 3; ++j)
            for (std::size_t l = 0; l < 4; ++l)
                k[j][l] = in["keys"][j][l].F();
        const auto& ts = in["ts"].A();
        const auto& want = cases[i]["out"]["values"].A();
        REQUIRE(ts.size() == want.size());
        for (std::size_t n = 0; n < ts.size(); ++n) {
            const auto got = particle::Sc2EvalCurve2D(mode, k[0], k[1], k[2], ts[n]->F(), mid, hold);
            for (std::size_t l = 0; l < 4; ++l) {
                INFO("case " << i << " mode=" << mode << " mid=" << mid << " hold=" << hold
                             << " t=" << ts[n]->F() << " lane " << l);
                REQUIRE(got[l] == (*want[n])[l].F());
                ++lanes;
            }
        }
        ++modes[mode];
    }
    CHECK(lanes > 1000u);
    CHECK(modes.size() >= 5u);
}

namespace {

Vector3f Op13Vertex(void* ctx, u32 v) {
    const auto& verts = *static_cast<const std::vector<Vector3f>*>(ctx);
    return v < verts.size() ? verts[v] : Vector3f{0.0f, 0.0f, 0.0f};
}

} // namespace

TEST_CASE("op13: the Mesh shape replays the rejection walk, draw for draw",
          "[sc2_particle][oracle][op13]") {
    // Only `M3_ComputeSkinnedRegionPositions` was stubbed, so the golden fixes
    // the pick, the fold, the mask byte, the 32-attempt cap and the generator
    // state out — and the face-index resolution through BOTH region terms.
    const fs::path path = GoldenDir() / "op13_meshspawn.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 1u);
    // The fixture the gate built, from the golden rather than from a copy.
    REQUIRE(cases[0]["tag"].S() == "fixture");
    const auto& fx = cases[0]["out"];
    std::vector<Vector3f> verts;
    for (const auto& p : fx["verts"].A())
        verts.push_back(Vec3From(*p));
    std::vector<u32> faces;
    for (const auto& f : fx["faces"].A())
        faces.push_back(f->U());
    std::vector<particle::Sc2MeshRegionBase> regions;
    for (const auto& r : fx["regions"].A())
        regions.push_back({(*r)[0].U(), (*r)[1].U()});
    std::vector<particle::Sc2MeshTriangle> tris;
    for (const auto& t : fx["tris"].A())
        tris.push_back({(*t)[0].U(), (*t)[1].U()});
    std::map<std::string, std::vector<particle::Sc2MeshTriangle>> slots;
    for (const auto& s : fx["slots"].A()) {
        std::vector<particle::Sc2MeshTriangle> table;
        if (!(*s)["tris"].IsNull()) {
            for (const auto& t : (*s)["tris"].A())
                table.push_back(tris.at(t->U()));
        }
        slots[(*s)["name"].S()] = std::move(table);
    }

    std::size_t hits = 0, refused = 0, capped = 0, collinear = 0;
    for (std::size_t i = 1; i < cases.Size(); ++i) {
        const auto& in = cases[i]["in"];
        const auto& out = cases[i]["out"];
        const std::string slot = in["slot"].S();

        particle::Sc2MeshSurfaceInputs mi;
        mi.haveAsset = in["hasAsset"].B();
        mi.haveVertexDesc = in["hasVertexDesc"].B();
        const auto& table = slots.at(slot);
        mi.triangles = table;
        mi.faces = faces;
        mi.regions = regions;
        std::vector<u8> colorR;
        if (in["colourFlag"].B()) {
            // B, G, R, A — the walk reads the byte at +2.
            for (const auto& c : in["colours"].A())
                colorR.push_back(static_cast<u8>((*c)[2].U()));
        }
        mi.colorR = colorR;
        mi.ctx = &verts;
        mi.position = &Op13Vertex;

        sc2::Rng rng(in["rngIn"][0].U(), in["rngIn"][1].U());
        const particle::Sc2MeshSample s = particle::Sc2SampleMeshSurface(rng, mi);
        INFO("case " << i << " tag=" << cases[i]["tag"].S() << " slot=" << slot);
        REQUIRE(s.hit == (out["ret"].U() == 1u));
        REQUIRE(s.tries == out["tries"].U());
        REQUIRE(rng.acc() == out["rngOut"][0].U());
        REQUIRE(rng.idx4() == out["rngOut"][1].U());
        const Vector3f wantPos = Vec3From(out["position"]);
        REQUIRE(s.position.x == wantPos.x);
        REQUIRE(s.position.y == wantPos.y);
        REQUIRE(s.position.z == wantPos.z);
        const Vector3f wantN = Vec3From(out["normal"]);
        CloseRel(s.normal.x, wantN.x, kRsqrtRtol, "normal.x");
        CloseRel(s.normal.y, wantN.y, kRsqrtRtol, "normal.y");
        CloseRel(s.normal.z, wantN.z, kRsqrtRtol, "normal.z");

        hits += s.hit ? 1u : 0u;
        refused += s.hit ? 0u : 1u;
        capped += s.tries == 32u ? 1u : 0u;
        collinear += (s.hit && wantN.x == 0.0f && wantN.y == 0.0f && wantN.z == 1.0f) ? 1u : 0u;
    }
    CHECK(hits > 500u);
    CHECK(refused > 0u);
    CHECK(capped > 0u);
    CHECK(collinear > 0u);
}

TEST_CASE("op14: UpdateModelParticle's pose replays all eleven instance types",
          "[sc2_particle][oracle][op14]") {
    // The eight setters are the output; the three that hand over the model's
    // own blocks by address are not maths and not replayed. Position, tint and
    // alpha are float32 chains and exact wherever no reciprocal root feeds
    // them; the quaternion goes through rsqrt and host sin/cos, so its lanes
    // carry the rsqrt bound.
    const fs::path path = GoldenDir() / "op14_modelparticle.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 1u);

    REQUIRE(cases[0]["tag"].S() == "presets");
    const auto& presets = cases[0]["out"]["presets"];
    REQUIRE(presets.Size() == 7u);
    for (std::size_t k = 0; k < 7; ++k)
        for (std::size_t l = 0; l < 4; ++l)
            REQUIRE(particle::kSc2ModelOrientPresets[k][l] == presets[k][l].F());

    std::size_t posed = 0, exactRotations = 0, bare = 0;
    std::map<u32, std::size_t> types;
    for (std::size_t i = 1; i < cases.Size(); ++i) {
        const auto& in = cases[i]["in"];
        const auto& out = cases[i]["out"];
        INFO("case " << i << " tag=" << cases[i]["tag"].S()
                     << " type=" << in["instanceType"].U());
        if (!in["instance"].B()) {
            // A null instance returns before the kernel's work: nothing set.
            REQUIRE(out["order"].Size() == 0u);
            ++bare;
            continue;
        }
        REQUIRE(out["order"].Size() == 8u);

        particle::Sc2ModelPoseInputs p;
        p.instanceType = in["instanceType"].U();
        p.legacyOrient = std::bit_cast<u32>(in["preset"].F()) != 0u;
        p.orientVariant = in["ribbonLinkIndex"].I();
        p.parFlags = (in["swapYZ"].B() ? 0x800000u : 0u) | (in["noTailFloor"].B() ? 0x100000u : 0u);
        p.additionalFlags = in["worldSpace"].B() ? 8u : 0u;
        p.rotationFlags =
            (in["elementKeys"].B() ? 4u : 0u) | (in["randomDirection"].B() ? 0x80u : 0u);
        p.stateFlags = in["plainOrient"].B() ? 0x80u : 0u;
        const u32 mode = in["smoothing"].U();
        p.sizeSmoothing = p.colorSmoothing = p.rotationSmoothing = mode;
        // gate_modelparticle.py authors every mid time at 0.5 and every hold
        // at 0, which the struct's defaults already are.
        for (std::size_t k = 0; k < 3; ++k) {
            p.sizeKeys[k] = in["sizeKeys"][k].F();
            p.rotationKeys[k] = in["rotKeys"][k].F();
            p.colorKeys[k] = in["colours"][k].U();
            p.elementSize[k] = static_cast<u16>(in["elemSize"][k].U());
            p.elementRotation[k] = static_cast<u16>(in["elemRot"][k].U());
            p.elementColors[k] = in["elemColours"][k].U();
        }
        p.position = Vec3From(in["position"]);
        p.velocity = Vec3From(in["velocity"]);
        p.orientVec = Vec3From(in["orientVec"]);
        p.spawnOrigin = Vec3From(in["spawnOrigin"]);
        p.randomDirection = Vec3From(in["randomDir"]);
        p.instanceAngle = Vec3From(in["instanceAngle"]);
        p.tailLength = in["tailLength"].F();
        p.modelAlpha = in["modelAlpha"].F();
        p.modelTint = Vec3From(in["modelTint"]);
        for (std::size_t r = 0; r < 3; ++r)
            p.camera[r] = Vec3From(in["cameraRows"][r]);
        for (std::size_t k = 0; k < 16; ++k)
            p.world[k] = in["worldMatrix"][k].F();
        if (!in["terrainNormal"].IsNull()) {
            p.haveTerrain = true;
            p.terrainNormal = Vec3From(in["terrainNormal"]);
        }
        p.emitterTime = in["emitterTime"].F();
        p.birthTime = in["birthTime"].F();
        p.deathTime = in["deathTime"].F();

        const particle::Sc2ModelPose pose = particle::Sc2ModelParticlePose(p);

        // A reciprocal root reaches the position only through type 10's shift,
        // and the scale through the longest row (`AlwaysSet`) and the three
        // stretching types.
        const bool rootPos = p.instanceType == 10;
        const bool rootScale = (p.rotationFlags & 4u) != 0u || p.instanceType == 1 ||
                               p.instanceType == 9 || p.instanceType == 10;
        const auto lane = [&](f32 got, f32 want, bool root, const char* what) {
            if (root)
                CloseRel(got, want, kRsqrtRtol, what);
            else {
                INFO(what << " got=" << got << " want=" << want);
                REQUIRE(got == want);
            }
        };
        const Vector3f wantPos = Vec3From(out["SetPosition"]);
        lane(pose.position.x, wantPos.x, rootPos, "position.x");
        lane(pose.position.y, wantPos.y, rootPos, "position.y");
        lane(pose.position.z, wantPos.z, rootPos, "position.z");
        const Vector3f wantScale = Vec3From(out["SetScale"]);
        lane(pose.scale.x, wantScale.x, rootScale, "scale.x");
        lane(pose.scale.y, wantScale.y, rootScale, "scale.y");
        lane(pose.scale.z, wantScale.z, rootScale, "scale.z");
        const Vector3f wantTint = Vec3From(out["SetTintColor"]);
        lane(pose.tint.x, wantTint.x, false, "tint.r");
        lane(pose.tint.y, wantTint.y, false, "tint.g");
        lane(pose.tint.z, wantTint.z, false, "tint.b");
        lane(pose.alpha, out["SetAlpha"].F(), false, "alpha");
        bool same = true;
        for (std::size_t l = 0; l < 4; ++l) {
            const f32 want = out["SetLocalRotation"][l].F();
            CloseRel(pose.rotation[l], want, kRsqrtRtol, "rotation");
            same = same && pose.rotation[l] == want;
        }
        exactRotations += same ? 1u : 0u;
        ++types[p.instanceType];
        ++posed;
    }
    INFO("posed=" << posed << " bit-exact quaternions=" << exactRotations);
    CHECK(posed > 400u);
    CHECK(bare > 0u);
    CHECK(types.size() == 11u);
}

TEST_CASE("op14b: the path pick and the random direction replay from the seed",
          "[sc2_particle][oracle][op14b]") {
    // One stream across the whole batch, exactly as the gate drove it: the
    // skip is before the draw, the index is a modulo of one wide draw, and the
    // direction is three more draws normalised. The recorder rows the host
    // owns — the descriptor, the refresh flag, the virtual calls — are not
    // the kernel's and are not replayed.
    const fs::path path = GoldenDir() / "op14b_pendingspawns.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_particle_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() > 0u);
    std::size_t calls = 0, drawn = 0, skipped = 0, directions = 0, offThread = 0;
    std::map<u32, std::size_t> paths;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& in = cases[i]["in"];
        const auto& out = cases[i]["out"];
        INFO("case " << i << " tag=" << cases[i]["tag"].S());
        if (!in["mainThread"].B()) {
            // The guard is the whole body; nothing is drawn.
            REQUIRE(out["rng"][0].U() == in["seed"][0].U());
            ++offThread;
            continue;
        }
        sc2::Rng rng(in["seed"][0].U(), in["seed"][1].U());
        const u32 nPaths = in["nPaths"].U();
        const bool rd = in["randomDirection"].B();
        const f32 et = in["emitterTime"].F();
        const bool perEntry = in["perEntrySystem"].B();
        const auto& deaths = in["deathTimes"].A();
        const auto& elems = out["elements"].A();
        REQUIRE(deaths.size() == elems.size());
        for (std::size_t e = 0; e < deaths.size(); ++e) {
            // One system per entry under `perEntrySystem`, its clock 0.25 on.
            const f32 clock = perEntry ? et + 0.25f * static_cast<f32>(e) : et;
            particle::Sc2PendingDraw d;
            const bool ok = particle::Sc2PendingSpawnDraw(rng, deaths[e]->F(), clock, nPaths, rd, d);
            const auto& want = *elems[e];
            INFO("entry " << e);
            REQUIRE(ok == want["processed"].B());
            const Vector3f gv = Vec3From(want["gpuVelocity"]);
            if (ok) {
                REQUIRE(d.pathIndex == want["path"].U());
                ++paths[d.pathIndex];
                ++drawn;
            } else {
                ++skipped;
            }
            if (ok && rd) {
                REQUIRE(d.hasDirection);
                CloseRel(d.direction.x, gv.x, kRsqrtRtol, "direction.x");
                CloseRel(d.direction.y, gv.y, kRsqrtRtol, "direction.y");
                CloseRel(d.direction.z, gv.z, kRsqrtRtol, "direction.z");
                REQUIRE(want["gpuInvMass"].F() == 0.0f);
                ++directions;
            } else {
                // The fixture's poison survives: nothing wrote the lane.
                REQUIRE(gv.x == 9.0f);
                REQUIRE(want["gpuInvMass"].F() == 9.0f);
            }
        }
        REQUIRE(rng.acc() == out["rng"][0].U());
        REQUIRE(rng.idx4() == out["rng"][1].U());
        ++calls;
    }
    CHECK(calls > 150u);
    CHECK(drawn > 0u);
    CHECK(skipped > 0u);
    CHECK(directions > 50u);
    CHECK(offThread > 0u);
    CHECK(paths.size() == 5u);

    // Design §8: retail divides by an empty path table and faults. The port
    // skips before the draw, so the stream does not move.
    sc2::Rng rng(0x12345678u, 0x0100FF1Cu);
    particle::Sc2PendingDraw d;
    CHECK_FALSE(particle::Sc2PendingSpawnDraw(rng, 2.0f, 1.0f, 0u, true, d));
    CHECK(rng.acc() == 0x12345678u);
    CHECK(rng.idx4() == 0x0100FF1Cu);
}

TEST_CASE("compose: a camera-facing model particle's rows are the camera's",
          "[sc2_particle][compose][model]") {
    // What the pose's quaternion MEANS for placing the child: no golden says
    // which rows a row-vector matrix takes from it, so this pins the one case
    // whose answer is known — a type-0 basis is the camera's own three rows.
    const f32 c = std::cos(0.5f), s = std::sin(0.5f);
    const f32 cp = std::cos(0.3f), sp = std::sin(0.3f);
    const Vector3f right{c, s, 0.0f};
    const Vector3f view{-s * cp, c * cp, -sp};
    const Vector3f up{-s * sp, c * sp, cp};
    particle::Sc2ModelPoseInputs p;
    p.instanceType = 0;
    p.emitterTime = 1.0f;
    p.birthTime = 0.0f;
    p.deathTime = 2.0f;
    p.camera = {right, view, up};
    const particle::Sc2ModelPose pose = particle::Sc2ModelParticlePose(p);
    const auto rows = particle::Sc2QuatRows(pose.rotation);
    const auto near = [](const Vector3f& a, const Vector3f& b) {
        return std::fabs(a.x - b.x) < 1e-5f && std::fabs(a.y - b.y) < 1e-5f &&
               std::fabs(a.z - b.z) < 1e-5f;
    };
    CHECK(near(rows[0], right));
    CHECK(near(rows[1], view));
    CHECK(near(rows[2], up));
}

TEST_CASE("compose: a model particle's pose runs in SC2 units and lands in the host's",
          "[sc2_particle][compose][model]") {
    // The runtime runs in SC2 units: the pose is handed the emitter's transform
    // with the host's world scale already off, as the tick and the pending
    // walk hand it, and only the position goes back into renderer units. The
    // child actor applies the world scale itself — fed the renderer's matrix,
    // `AlwaysSet`'s longest-row factor would be the 100 and the child would be
    // drawn a hundred times too big.
    particle::Sc2Runtime rt;
    rt.store.Init(4);
    const i32 node = rt.store.Acquire();
    REQUIRE(node >= 0);
    auto& e = rt.store.elements[static_cast<usize>(node)];
    e.position = {1.0f, 2.0f, 3.0f};
    e.birthTime = 0.0f;
    e.deathTime = 2.0f;
    e.size = {256, 256, 256, 0}; // s16/256: 1.0 on all three keys
    rt.clock.emitterTime = 1.0f;
    rt.frame.size3 = {1.0f, 1.0f, 1.0f};
    rt.actorWorldScale = 100.0f;

    particle::Sc2EmitterDesc d;
    d.rotationFlags = static_cast<particle::ParticleRotationFlag>(4u); // AlwaysSet
    const Matrix44f world =
        Matrix44f::scaling({100.0f, 100.0f, 100.0f}) * Matrix44f::translation({500.0f, 0.0f, 0.0f});
    const particle::Sc2ModelPose pose = particle::Sc2PoseModelParticle(
        rt, d, particle::Sc2FromHostSpace(world, rt.actorWorldScale), node);

    CHECK(pose.scale.x == Catch::Approx(1.0f).epsilon(1e-5));
    CHECK(pose.scale.z == Catch::Approx(1.0f).epsilon(1e-5));
    // And the position is where the renderer draws the particle.
    CHECK(pose.position.x == Catch::Approx(600.0f).epsilon(1e-5));
    CHECK(pose.position.y == Catch::Approx(200.0f).epsilon(1e-5));
    CHECK(pose.position.z == Catch::Approx(300.0f).epsilon(1e-5));
}

TEST_CASE("compose: a Mesh emitter is born on its surface, and type 4 follows the face",
          "[sc2_particle][compose][mesh]") {
    // OP13 measured the sampler against a fixture with the skin stubbed; this
    // is the join — the runtime's triangle table, the mesh's rest positions
    // standing in for an unskinned model, and the normal reaching velocity 4.
    const auto triangle = [] {
        auto mesh = std::make_shared<particle::EmitMesh>();
        mesh->subs = {particle::EmitMesh::SubMesh{0u, 1u}};
        mesh->tris = {0u, 1u, 2u};
        mesh->rest = {Vector3f{0.0f, 0.0f, 5.0f}, Vector3f{2.0f, 0.0f, 5.0f},
                      Vector3f{0.0f, 2.0f, 5.0f}};
        return mesh;
    };

    SECTION("on the triangle, moving along its normal") {
        Sc2Rig rig(256, 120.0f, 100.0f);
        rig.surface.mesh = triangle();
        rig.rt.meshTriangles = {particle::Sc2MeshTriangle{0u, 0u}};
        rig.d.emit.shape = 7;
        rig.d.emit.velocityType = 4;
        rig.rt.frame.speed = 3.0f;
        REQUIRE(rig.Run(10, kSixtieth) > 0u);
        std::vector<i32> walk;
        rig.rt.store.list.Walk(walk);
        REQUIRE_FALSE(walk.empty());
        for (const i32 n : walk) {
            const auto& e = rig.rt.store.elements[static_cast<usize>(n)];
            INFO("element " << n << " at " << e.position.x << "," << e.position.y << ","
                            << e.position.z);
            CHECK(std::fabs(e.position.z - 5.0f) < 1e-5f);
            CHECK(e.position.x >= -1e-5f);
            CHECK(e.position.y >= -1e-5f);
            CHECK(e.position.x + e.position.y <= 2.0f + 1e-5f);
            // (2,0,0) x (0,2,0) normalised is exactly +Z, times the speed.
            CHECK(e.velocity.x == 0.0f);
            CHECK(e.velocity.y == 0.0f);
            CHECK(e.velocity.z == 3.0f);
        }
    }

    SECTION("with no mesh set, at the origin and drawing nothing") {
        Sc2Rig rig(256, 120.0f, 100.0f);
        rig.d.emit.shape = 7;
        REQUIRE(rig.Run(10, kSixtieth) > 0u);
        std::vector<i32> walk;
        rig.rt.store.list.Walk(walk);
        for (const i32 n : walk) {
            const auto& e = rig.rt.store.elements[static_cast<usize>(n)];
            CHECK(e.position.x == 0.0f);
            CHECK(e.position.y == 0.0f);
            CHECK(e.position.z == 0.0f);
        }
    }

    SECTION("velocity type 4 on any other shape has no velocity at all") {
        // Retail zeroes the normal before the position sampler and only the
        // Mesh shape writes it; the port used to hand type 4 a default +Z.
        Sc2Rig rig(256, 120.0f, 100.0f);
        rig.d.emit.velocityType = 4;
        rig.rt.frame.speed = 3.0f;
        REQUIRE(rig.Run(10, kSixtieth) > 0u);
        std::vector<i32> walk;
        rig.rt.store.list.Walk(walk);
        REQUIRE_FALSE(walk.empty());
        for (const i32 n : walk) {
            const auto& e = rig.rt.store.elements[static_cast<usize>(n)];
            CHECK(e.velocity.x == 0.0f);
            CHECK(e.velocity.y == 0.0f);
            CHECK(e.velocity.z == 0.0f);
        }
    }
}
