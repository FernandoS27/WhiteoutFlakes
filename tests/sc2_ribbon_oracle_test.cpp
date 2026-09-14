// ============================================================================
// The SC2 ribbon module measured against the ORIGINAL rather than against its
// own description: `tools/sc2_ribbon_oracle/` runs the real StarCraft II 4.8
// client under Unicorn and records what it computed; this replays those
// goldens. No emulator and no binary needed to run it — that is the point.
//
// O1 (this file's first citizen) is the load-time simulation-technique
// derivation: the golden is the full 720-vector truth table over every
// decision bit `Ribbon_SelectSimTechnique` reads, so a pass means our
// `SelectSc2SimTechnique` IS that function, boundary behaviour included —
// noise exactly float32(0.001) does not demote, and the forces operand is
// the fallback-pair dword (SC2_RIBBON_RE.md §3).
// ============================================================================

#include "oracle_golden.h"
#include "renderer/ribbon/ribbon_emitter.h"
#include "renderer/sc2/sc2_element.h"
#include "renderer/sc2/sc2_element_math.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

using namespace whiteout::flakes;
using namespace whiteout::flakes::renderer::ribbon;
// The element math moved out of `ribbon::` into `sc2::` (R6), so the
// using-directive above no longer reaches it.
namespace vs = whiteout::flakes::renderer::sc2::vs;
namespace fs = std::filesystem;

namespace {

fs::path GoldenDir() {
    if (const char* v = std::getenv("WDX_SC2_RIBBON_GOLDEN"); v && *v)
        return fs::path(v);
#ifdef WDX_SC2_RIBBON_GOLDEN_DIR
    return fs::path(WDX_SC2_RIBBON_GOLDEN_DIR);
#else
    return {};
#endif
}

f32 Bits(const wdx_golden::Value& v) {
    return std::bit_cast<f32>(v.U());
}

/// A float lane stored as a shortest-decimal (elem_f32 / read_f32 in the gate).
void CloseRel(f32 got, f32 want, f32 rtol, const char* what) {
    const f32 tol = rtol * (std::max)(1.0f, std::fabs(want));
    INFO(what << " got=" << got << " want=" << want);
    REQUIRE(std::fabs(got - want) <= tol);
}

} // namespace

TEST_CASE("o1: technique selection replays the binary's truth table",
          "[ribbon][sc2_ribbon][oracle]") {
    const fs::path path = GoldenDir() / "o1_simtech.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string() + "; record with tools/sc2_ribbon_oracle");

    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 720);

    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];

        Sc2RibbonEmitterConfig cfg;
        cfg.flags = in["flags"].U();
        cfg.splines.resize(static_cast<std::size_t>(in["splineCount"].I()));
        cfg.forcesFallback = in["forces"].U();
        cfg.noiseAmplitude = std::bit_cast<f32>(in["noiseBits"].U());
        cfg.cullMethod = static_cast<u8>(in["cull"].I());

        const u8 got = static_cast<u8>(SelectSc2SimTechnique(cfg));
        const u8 want = static_cast<u8>(c["out"]["technique"].I());
        INFO("case " << i << ": flags=0x" << std::hex << cfg.flags << std::dec
                     << " splines=" << cfg.splines.size() << " forces=" << cfg.forcesFallback
                     << " noiseBits=0x" << std::hex << in["noiseBits"].U() << std::dec
                     << " cull=" << int(cfg.cullMethod));
        REQUIRE(int(got) == int(want));
    }
}

TEST_CASE("o1: the derivation lands in the desc at conversion",
          "[ribbon][sc2_ribbon][oracle]") {
    // The truth table proves the function; this pins that DescFromSc2Config
    // actually calls it — a desc whose simTechnique were left 0 would replay
    // green above and still ship the wrong integrator.
    Sc2RibbonEmitterConfig cfg;
    cfg.flags = 0x2000;
    REQUIRE(int(DescFromSc2Config(cfg).sc2.simTechnique) == 3);
    cfg.flags = 0;
    cfg.cullMethod = 1;
    REQUIRE(int(DescFromSc2Config(cfg).sc2.simTechnique) == 2);
    cfg.splines.resize(1);
    REQUIRE(int(DescFromSc2Config(cfg).sc2.simTechnique) == 1);
    cfg.noiseAmplitude = 0.5f;
    REQUIRE(int(DescFromSc2Config(cfg).sc2.simTechnique) == 4);
}

// ---------------------------------------------------------------------------
// O3 — the emit clock. `EmitGate` replays UpdateEmit bit-for-bit: the
// accumulate-or-not behaviour of every early-out, the one-period-per-call
// bank, the 1.0/3.0/startBlend returns, and the renderFlag bookkeeping.
// ---------------------------------------------------------------------------
TEST_CASE("o3: the emit clock replays UpdateEmit", "[ribbon][sc2_ribbon][oracle]") {
    const fs::path path = GoldenDir() / "o3_emitclock.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string());
    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& out = c["out"];
        const std::string buffer = in["buffer"].S();

        sc2::EmitClock clk;
        clk.dtAccumulator = Bits(in["acc0"]);
        clk.renderFlags = static_cast<u16>(in["rf0"].U());
        clk.renderFlagsHi = static_cast<u8>(in["rfHi0"].U());

        sc2::EmitGateInputs g;
        g.splinePresent = in["spline"].I() != 0;
        g.active = in["active"].I() != 0;
        g.worldReemit = in["flag8"].I() != 0;
        g.simTechnique = static_cast<SimTechnique>(in["tech"].I());
        g.haveHead = (buffer == "head");
        g.headU = Bits(in["headU"]);
        g.headBirthU = Bits(in["birthU"]);
        g.headElemPos = {Bits(in["elemX"]), 0.0f, 0.0f};
        g.quality = in["quality"].I();
        g.lodCut = in["lodCut"].I();
        g.lodReduce = in["lodReduce"].I();
        g.cullMethod = static_cast<CullMethod>(in["cull"].I());
        g.emissionScale = Bits(in["scale"]);
        g.divisions = Bits(in["div"]);
        g.lifetimeAux = Bits(in["lifeAux"]);
        g.maxLengthAux = Bits(in["maxAux"]);
        g.dt = Bits(in["dt"]);
        g.sampledActive = in["sampleActive"].I() != 0;
        g.nodeActive = in["nodeActive"].I() != 0;
        g.elementCount = in["elemCount"].U();

        const sc2::EmitGateResult r = sc2::EmitGate(clk, g);
        INFO("case " << i << " tag=" << (c.Has("tag") ? c["tag"].S() : ""));
        REQUIRE(std::bit_cast<u32>(r.ret) == out["ret_bits"].U());
        REQUIRE(std::bit_cast<u32>(clk.dtAccumulator) == out["acc_bits"].U());
        REQUIRE(u32(clk.renderFlags) == out["rf"].U());
        REQUIRE(u32(clk.renderFlagsHi) == out["rfHi"].U());
        REQUIRE(r.activeState == out["activeState"].U());
        REQUIRE((r.sampled ? 1u : 0u) == out["sampled"].U());
    }
}

// ---------------------------------------------------------------------------
// O3b — EmitSegments, the per-frame sub-step marcher (settles A2/A3). The
// binary marches head U/pos per sub-step; the C++ Sc2Append/CommitSc2Segment is
// a per-FRAME model by design (§5 deviation), so this does not replay the
// trajectory bit-for-bit. It pins the recorded truth's invariants into the
// build tree AND checks the one tight correspondence that settles A2/A3: for a
// uniform march (one append per sub-step), the C++ per-segment interpolation
// birthU = headU0 + (k/N)·headAdvance and birthPos = prev0 + k·posDelta
// reproduce the binary's per-sub-step stamping. (smoothedDir is recorded for
// O4b/inherit; the doc-reimpl in conformance.py checks it at the rcpps bound.)
// ---------------------------------------------------------------------------
TEST_CASE("o3b: EmitSegments marches head U/pos per sub-step",
          "[ribbon][sc2_ribbon][oracle]") {
    const fs::path path = GoldenDir() / "o3b_emitsegments.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string());
    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& out = c["out"];
        INFO("case " << i << " tag=" << (c.Has("tag") ? c["tag"].S() : ""));

        const int sc = in["segmentCount"].I();
        const f32 headU0 = Bits(in["headU0"]);
        const f32 headAdv = Bits(in["headAdvance"]);
        const Vector3f prev0{in["prev"][0].F(), in["prev"][1].F(), in["prev"][2].F()};
        const Vector3f delta{in["posDelta"][0].F(), in["posDelta"][1].F(),
                             in["posDelta"][2].F()};
        const f32 headUf = Bits(out["headU"]);

        REQUIRE(out["nBegin"].U() == 1u); // begin fires even when segmentCount==0
        const auto& appends = out["appends"];
        REQUIRE(out["nAppend"].U() == appends.Size());

        if (sc == 0) {
            REQUIRE(std::bit_cast<u32>(headUf) == std::bit_cast<u32>(headU0));
            REQUIRE(appends.Size() == 0);
            continue;
        }

        // headU advances by Σ(headAdvance/N) — ≈ headAdvance up to float rounding.
        CloseRel(headUf, headU0 + headAdv, 1e-5f, "headU total");
        // Roll: prevEmitPos ends equal to headPos (bit-exact — a plain copy).
        for (int j = 0; j < 3; ++j)
            REQUIRE(out["prevEmitPos"][j].U() == out["headPos"][j].U());
        // The march endpoint: headPos = prev0 + N·posDelta.
        const Vector3f endPos{prev0.x + sc * delta.x, prev0.y + sc * delta.y,
                              prev0.z + sc * delta.z};
        CloseRel(Bits(out["headPos"][0]), endPos.x, 1e-5f, "endPos.x");
        CloseRel(Bits(out["headPos"][1]), endPos.y, 1e-5f, "endPos.y");
        CloseRel(Bits(out["headPos"][2]), endPos.z, 1e-5f, "endPos.z");

        // birthU non-decreasing (strictly rising when headAdvance>0; a zero
        // advance stamps them all equal), each within the frame's (headU0, headUf].
        f32 prevU = headU0;
        for (std::size_t j = 0; j < appends.Size(); ++j) {
            const f32 u = Bits(appends[j]["u"]);
            REQUIRE(u >= prevU);
            REQUIRE(u <= headUf + 1e-6f);
            prevU = u;
        }

        // A2/A3 tie: when a segment is appended every sub-step, the C++
        // per-segment interpolation reproduces the binary's stamping exactly.
        if (appends.Size() == static_cast<std::size_t>(sc)) {
            for (int k = 1; k <= sc; ++k) {
                const auto& a = appends[static_cast<std::size_t>(k - 1)];
                const f32 fracU = headU0 + (static_cast<f32>(k) / sc) * headAdv;
                CloseRel(Bits(a["u"]), fracU, 3e-6f, "interp birthU");
                CloseRel(Bits(a["pos"][0]), prev0.x + k * delta.x, 1e-5f, "interp pos.x");
                CloseRel(Bits(a["pos"][1]), prev0.y + k * delta.y, 1e-5f, "interp pos.y");
                CloseRel(Bits(a["pos"][2]), prev0.z + k * delta.z, 1e-5f, "interp pos.z");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// O4 — the head-element writer. Direction and up run through the sin/cos
// basis, so they replay at rtol 2e-6; the size/rotation/timing/mass lanes are
// exact. (Colours and element position are pass-through in the local subset
// and covered by the surrounding sim test, not here.)
// ---------------------------------------------------------------------------
TEST_CASE("o4: the head element replays UpdateHeadSegment",
          "[ribbon][sc2_ribbon][oracle]") {
    const fs::path path = GoldenDir() / "o4_head.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string());
    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& out = c["out"];

        sc2::HeadInputs h;
        h.simTechnique = static_cast<SimTechnique>(in["tech"].I());
        h.ribbonType = static_cast<whiteout::m3::RibbonType>(in["ribbonType"].I());
        h.cullMethod = static_cast<CullMethod>(in["cull"].I());
        h.swapYawPitch = (in["flags"].U() & 0x8000u) != 0;
        h.headU = Bits(in["headU"]);
        h.yawDeg = in["yaw"].F();
        h.pitchDeg = in["pitch"].F();
        h.speed = in["speed"].F();
        h.lifetime = in["lifetime"].F();
        h.size3 = {in["size"][0].F(), in["size"][1].F(), in["size"][2].F()};
        h.rotation3 = {in["rotation"][0].F(), in["rotation"][1].F(),
                       in["rotation"][2].F()};
        h.mass = in["mass"].F();
        h.maxLengthBound = 3.0f; // the fixture's constant
        h.nowMs = in["frameMs"].U();
        h.prevExpireMs = in["expire0"].U();

        sc2::HeadElement e = sc2::WriteHead(h);
        // The 1e-4 stationary floor (techs 0/2/3) moved out of WriteHead to
        // the caller (DRIFT-1: the binary floors AFTER transform+inherit). This
        // golden is the full UpdateHeadSegment and its subset is local +
        // no-inherit + identity, so replay the floor here on the local dir.
        if (AppliesStationaryFloor(h.simTechnique)) {
            const f32 sq = (e.velocity.x * e.velocity.x + e.velocity.y * e.velocity.y) +
                           e.velocity.z * e.velocity.z;
            if (sq < 1e-4f)
                e.velocity = {e.dir.x * 1e-4f, e.dir.y * 1e-4f, e.dir.z * 1e-4f};
        }
        INFO("case " << i << " tag=" << (c.Has("tag") ? c["tag"].S() : ""));

        const auto& velA = out["velA"];
        CloseRel(e.velocity.x, velA[0].F(), 2e-6f, "vel.x");
        CloseRel(e.velocity.y, velA[1].F(), 2e-6f, "vel.y");
        CloseRel(e.velocity.z, velA[2].F(), 2e-6f, "vel.z");
        // velB equals velA in the local subset.
        REQUIRE(out["velB"][0].F() == velA[0].F());

        const auto& up = out["up"];
        CloseRel(e.up.x, up[0].F(), 2e-6f, "up.x");
        CloseRel(e.up.y, up[1].F(), 2e-6f, "up.y");
        CloseRel(e.up.z, up[2].F(), 2e-6f, "up.z");

        const auto& sz = out["size3"];
        CloseRel(e.size3.x, sz[0].F(), 1e-6f, "size.x");
        CloseRel(e.size3.y, sz[1].F(), 1e-6f, "size.y");
        CloseRel(e.size3.z, sz[2].F(), 1e-6f, "size.z");

        const auto& rot = out["rot3"];
        CloseRel(e.rotation3.x, rot[0].F(), 1e-6f, "rot.x");
        CloseRel(e.rotation3.y, rot[1].F(), 1e-6f, "rot.y");
        CloseRel(e.rotation3.z, rot[2].F(), 1e-6f, "rot.z");

        REQUIRE(std::bit_cast<u32>(e.invMass) == out["invMass"].U());
        REQUIRE(std::bit_cast<u32>(e.birthU) == out["birthU"].U());
        REQUIRE(std::bit_cast<u32>(e.deathU) == out["deathU"].U());
        REQUIRE(e.expireFrameMs == out["expire"].U());
    }
}

// ---------------------------------------------------------------------------
// O4b — UpdateHeadSegment over its FULL surface (inherit / floor order /
// waves / world). Replays WriteHead (waves + swap) then the
// CommitSc2Segment velocity/up/floor sequence (ribbon_emitter.cpp:638-672),
// reconstructed here because that method reads member state. This pins DRIFT-1
// (the 1e-4 floor runs AFTER the inherit add, on the post-inherit vector) and
// DRIFT-2 (a LOCAL ribbon still folds in inherit — the binary gates on the
// flag alone, no world/local test). Direction/up run through sin/cos (+ the
// world 3x3), rtol 2e-6; size/rotation/timing/mass are exact.
//
// Not replayed, by design: the world element position is the matrix
// translation, which the C++ replaces with the per-frame interpolated origin
// (the accepted A2 deviation). And length-mode expireFrameMs uses the FINAL
// velocity magnitude in the binary, but WriteHead only has the local
// pre-inherit speed; expireFrameMs is a dead reclamation hint (never consumed),
// so a length-mode ribbon that also inherits is an accepted deviation, checked
// only where the two velocities agree.
// ---------------------------------------------------------------------------
TEST_CASE("o4b: the head element replays the full UpdateHeadSegment surface",
          "[ribbon][sc2_ribbon][oracle]") {
    const fs::path path = GoldenDir() / "o4b_head_full.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string());
    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& out = c["out"];
        INFO("case " << i << " tag=" << (c.Has("tag") ? c["tag"].S() : ""));

        sc2::HeadInputs h;
        h.simTechnique = static_cast<SimTechnique>(in["tech"].I());
        h.ribbonType = static_cast<whiteout::m3::RibbonType>(in["ribbonType"].I());
        h.cullMethod = static_cast<CullMethod>(in["cull"].I());
        h.swapYawPitch = (in["flags"].U() & 0x8000u) != 0;
        h.headU = Bits(in["headU"]);
        h.yawDeg = in["yaw"].F();
        h.pitchDeg = in["pitch"].F();
        h.speed = in["speed"].F();
        h.lifetime = in["lifetime"].F();
        h.size3 = {in["size"][0].F(), in["size"][1].F(), in["size"][2].F()};
        h.rotation3 = {in["rotation"][0].F(), in["rotation"][1].F(),
                       in["rotation"][2].F()};
        h.mass = in["mass"].F();
        h.maxLengthBound = 3.0f;
        h.nowMs = in["frameMs"].U();
        h.prevExpireMs = in["expire0"].U();
        const auto& wv = in["waves"];
        for (int k = 0; k < 5; ++k) {
            h.waveTypes[k] = wv[static_cast<std::size_t>(k)][0].U();
            h.waveAmp[k] = wv[static_cast<std::size_t>(k)][1].F();
            h.waveFreq[k] = wv[static_cast<std::size_t>(k)][2].F();
        }
        h.overlayPhase = in["overlayPhase"].F();
        h.overlayTime = in["overlayTime"].F();

        const sc2::HeadElement e0 = sc2::WriteHead(h);

        // Reconstruct CommitSc2Segment: world transform -> inherit -> floor.
        const bool world = (in["additionalFlags"].U() & 0x8u) != 0;
        const bool inherit = (in["emitterFlags"].U() & 0x8u) != 0;
        Vector3f vel = e0.velocity, dirRef = e0.dir, up = e0.up;
        if (world) {
            whiteout::Matrix44f M;
            const bool ident = in["worldMat"].IsNull();
            for (int r = 0; r < 4; ++r)
                for (int cc = 0; cc < 4; ++cc)
                    M.data[r][cc] = ident
                        ? (r == cc ? 1.0f : 0.0f)
                        : in["worldMat"][static_cast<std::size_t>(r * 4 + cc)].F();
            vel = whiteout::transform_normal(e0.velocity, M);
            dirRef = whiteout::transform_normal(e0.dir, M);
            up = whiteout::transform_normal(e0.up, M);
        }
        if (inherit) {
            const Vector3f sd{in["smoothedDir"][0].F(), in["smoothedDir"][1].F(),
                              in["smoothedDir"][2].F()};
            const f32 pv = in["particleVel"].F();
            vel = {vel.x + sd.x * pv, vel.y + sd.y * pv, vel.z + sd.z * pv};
        }
        if (AppliesStationaryFloor(h.simTechnique)) {
            const f32 sq = (vel.x * vel.x + vel.y * vel.y) + vel.z * vel.z;
            if (sq < 1e-4f)
                vel = {dirRef.x * 1e-4f, dirRef.y * 1e-4f, dirRef.z * 1e-4f};
        }

        const auto& velA = out["velA"];
        CloseRel(vel.x, velA[0].F(), 2e-6f, "vel.x");
        CloseRel(vel.y, velA[1].F(), 2e-6f, "vel.y");
        CloseRel(vel.z, velA[2].F(), 2e-6f, "vel.z");
        REQUIRE(out["velB"][0].F() == velA[0].F()); // velA == velB

        const auto& upG = out["up"];
        CloseRel(up.x, upG[0].F(), 2e-6f, "up.x");
        CloseRel(up.y, upG[1].F(), 2e-6f, "up.y");
        CloseRel(up.z, upG[2].F(), 2e-6f, "up.z");

        const auto& sz = out["size3"];
        CloseRel(e0.size3.x, sz[0].F(), 2e-6f, "size.x");
        CloseRel(e0.size3.y, sz[1].F(), 2e-6f, "size.y");
        CloseRel(e0.size3.z, sz[2].F(), 2e-6f, "size.z");

        const auto& rot = out["rot3"];
        CloseRel(e0.rotation3.x, rot[0].F(), 1e-6f, "rot.x");
        CloseRel(e0.rotation3.y, rot[1].F(), 1e-6f, "rot.y");
        CloseRel(e0.rotation3.z, rot[2].F(), 1e-6f, "rot.z");

        REQUIRE(std::bit_cast<u32>(e0.invMass) == out["invMass"].U());
        REQUIRE(std::bit_cast<u32>(e0.birthU) == out["birthU"].U());
        REQUIRE(std::bit_cast<u32>(e0.deathU) == out["deathU"].U());
        if (!(h.cullMethod == CullMethod::Length && inherit))
            REQUIRE(e0.expireFrameMs == out["expire"].U());

        // Alpha overlay wave: each stop's alpha byte + wave*255, clamped [0,255]
        // (0 when the sum is negative).
        const u32 col[3] = {in["colStart"].U(), in["colMid"].U(),
                            in["colEnd"].U()};
        for (int k = 0; k < 3; ++k) {
            const f32 v94 = static_cast<f32>((col[k] >> 24) & 0xFFu) +
                            e0.alphaWave * 255.0f;
            const int b = (v94 >= 0.0f) ? static_cast<int>(std::fminf(v94, 255.0f))
                                        : 0;
            INFO("alpha byte " << k);
            REQUIRE(static_cast<u32>(static_cast<u8>(b)) ==
                    out["alphaBytes"][static_cast<std::size_t>(k)].U());
        }
    }
}

// ---------------------------------------------------------------------------
// O5 — the pre-roll clock. The tick count IS the observable (the loop body is
// Update, stubbed to a counter in the recording); a zero-tick result is the
// length-mode too-slow skip.
// ---------------------------------------------------------------------------
TEST_CASE("o5: catch-up tick count replays CatchUpEmission",
          "[ribbon][sc2_ribbon][oracle]") {
    const fs::path path = GoldenDir() / "o5_catchup.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string());
    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& out = c["out"];

        const u8 cull = static_cast<u8>(in["cull"].I());
        f32 speed = in["speed"].F();
        f32 lifetime = in["lifetime"].F();
        const f32 maxLen = in["maxLen"].F();

        // The track reduction is what sampling would have done: min over the
        // speed keys, max over the lifetime keys.
        const auto& track = in["track"];
        if (track.Size() > 0) {
            if (cull == 1) {
                speed = track[0].F();
                for (std::size_t k = 1; k < track.Size(); ++k)
                    speed = (std::min)(speed, track[k].F());
            } else {
                lifetime = track[0].F();
                for (std::size_t k = 1; k < track.Size(); ++k)
                    lifetime = (std::max)(lifetime, track[k].F());
            }
        }

        const sc2::CatchUpResult r = sc2::CatchUpTicks(static_cast<CullMethod>(cull), speed, lifetime, maxLen);
        INFO("case " << i << " tag=" << (c.Has("tag") ? c["tag"].S() : ""));
        REQUIRE(r.ticks == out["ticks"].I());
    }
}

// ---------------------------------------------------------------------------
// O5b — the pre-rolled trail STATE. Composes the two real functions the pre-roll
// is built from: CatchUpEmission (K + earlyOut) and EmitSegments run K times,
// carrying headU / the scroll remainder / elementCount across ticks. The golden
// is the resulting emission SCHEDULE: per-tick emit count, headU trajectory,
// scroll remainder, running count, and the cumulative birthU total. This is the
// binary's own multi-tick composition; the C++ pre-roll (CatchUpTicks + the
// Sc2Append loop, ribbon_emitter.cpp:706) reimplements the same accumulator
// (numNew = floor(accum + dt·rate), remainder carried, headU += dt), so replay
// it here: K from the real CatchUpTicks, then the accumulator with the same
// per-tick step (uvStep = dt·rate). Bit-exact — identical f32 accumulation.
//
// Pins the cross-tick composition (counts, headU, remainder, total). The
// per-SEGMENT birthU spread within a tick is the A2 per-frame deviation (§5 item
// 8), and the element COMMIT (AppendAndMaintain / CParticlePool) + deathU/retire
// are out of this gate (O4b deathU, O6/O7 recycle). The smoothing ring converging
// to the (constant) per-tick posDelta is O3b's; checked lightly here.
// ---------------------------------------------------------------------------
TEST_CASE("o5b: the pre-roll emission schedule composes CatchUpEmission + EmitSegments",
          "[ribbon][sc2_ribbon][oracle]") {
    const fs::path path = GoldenDir() / "o5b_catchup_state.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string());
    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    const f32 dt = std::bit_cast<f32>(0x3D072B02u); // float32(0.033), the tick dt

    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& out = c["out"];
        const std::string tag = c.Has("tag") ? c["tag"].S() : "";
        INFO("case " << i << " tag=" << tag);

        // K + earlyOut from the real pre-roll clock.
        const sc2::CatchUpResult cu = sc2::CatchUpTicks(
            static_cast<CullMethod>(in["cull"].I()), in["speed"].F(), in["lifetime"].F(),
            in["maxLen"].F());
        // The binary signals "no pre-roll" by running zero ticks (the too-slow
        // length skip returns before the loop); the C++ reports it as earlyOut.
        // The observable both share is the executed tick count.
        const i32 kCpp = cu.earlyOut ? 0 : cu.ticks;
        REQUIRE(kCpp == out["ticks"].I());
        REQUIRE(out["earlyOut"].B() == (out["ticks"].I() == 0));
        if (out["ticks"].I() == 0) {
            REQUIRE(out["totalEmitted"].I() == 0);
            continue;
        }

        // The Sc2Append accumulator over K ticks, with the same per-tick step.
        const f32 uvStep = std::bit_cast<f32>(in["uvStep"].U());
        const auto& perTick = out["perTick"];
        REQUIRE((i32)perTick.Size() == cu.ticks);
        f32 accum = 0.0f, headU = 0.0f;
        i32 total = 0;
        for (i32 k = 0; k < cu.ticks; ++k) {
            const f32 endCount = accum + uvStep;
            const i32 numNew = static_cast<i32>(std::floor(endCount));
            accum = endCount - std::floor(endCount);
            headU += dt;
            total += numNew;
            const auto& p = perTick[k];
            REQUIRE(numNew == p["emit"].I());
            REQUIRE(std::bit_cast<u32>(headU) == p["headU"].U());   // f32-exact
            REQUIRE(std::bit_cast<u32>(accum) == p["scroll"].U());  // f32-exact
            REQUIRE(total == p["count"].I());
        }
        REQUIRE(total == out["totalEmitted"].I());

        // Smoothing ring: identical per-tick posDelta -> smoothedDir converges to
        // it (the dt-weighted average of equal taps). O3b pins the ring math; here
        // just confirm the cross-tick convergence.
        const auto& sd = out["smoothedDir"];
        if (in["emitterFlags"].I() & 8) {
            const auto& pd = in["posDelta"];
            for (int k = 0; k < 3; ++k)
                CloseRel(sd[k].F(), pd[k].F(), 3e-4f, "smoothedDir");
        }
    }
}

// ---------------------------------------------------------------------------
// O6 — the analytic simulators Simulate_Type2 (0x10295F520) / Simulate_Type3
// (0x10295FBE0). Three pins:
//   * DRAG CLOSED FORM (every element, both techs): curPos = birthPos +
//     vs::CalculateDisplacementAndVelocity(age, v0, mass, drag, g = -gravity).
//     Through expf -> rtol 3e-6. Confirms the "gravity quirk" sign convention.
//   * LENGTH-MODE ARC WALK (cull 1): the head-to-tail arc = maxLength cut that
//     sets rpVScale / cutU / cutPos. This pins DRIFT-3 (arc-based V, stable
//     maxLength denominator). The UNCUT denominator is the binary's literal
//     v45/(v72-v41) with v72 = the newest element's birthU — kept literal so the
//     gate pins the binary truth on these synthetic inputs. In the real runtime
//     the newest element is the persistent head (pBuffer[3]) re-stamped at
//     birthU==headU every frame, so v72==headU; BuildStripSc2 synthesises that
//     live head, so its headU-oldest span equals v72-v41 (matches, not a
//     deviation). Arc length uses rsqrt+1 Newton in the binary -> rtol 3e-4.
//   * TECH-3 INTERIOR TANGENT: the binary precomputes an adjacent-segment
//     central-difference tangent; the interior nodes match BuildStripSc2's
//     recomputed central difference. The head region (last two elements), the
//     per-segment degenerate->(0,0,1) guard, and the binormal sign-flip diverge
//     (RIBBON_REVIEW_FINDINGS A6/O6) — head-of-ribbon cosmetics for Track C, so
//     only the interior is asserted here.
//
// Not replayed here: the binary's arc walk opens with an emitterHeadPos->newest
// segment. BuildStripSc2 now supplies it via the synthesised live head (a head
// node at the emitter, birthU==headU), so the leading edge tracks the emitter
// between the sub-frame spawns instead of snapping (the at-rest twinkle). This
// synthetic replay still measures arc from the newest element, so it does not
// exercise that opening segment.
// ---------------------------------------------------------------------------
TEST_CASE("o6: the analytic simulators replay the drag form and the arc walk",
          "[ribbon][sc2_ribbon][oracle]") {
    const fs::path path = GoldenDir() / "o6_analytic.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string());
    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];

    auto vec3 = [](const wdx_golden::Value& a) {
        return Vector3f{a[0].F(), a[1].F(), a[2].F()};
    };

    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& out = c["out"];
        const std::string tag = c.Has("tag") ? c["tag"].S() : "";
        INFO("case " << i << " tag=" << tag);

        const bool tech3 = in["func"].S() == "t3";
        const f32 headU = in["headU"].F();
        const f32 mass = in["mass"].F();
        const f32 drag = in["drag"].F();
        const f32 g = -in["gravity"].F();           // BuildStripSc2:1035 (local)
        const auto& elemsIn = in["elems"];
        const auto& elemsOut = out["elems"];
        const std::size_t n = elemsIn.Size();

        // Drag closed form for every element, and collect curPos for the arc walk.
        // displacement = A·(1−e⁻ᵏᵗ) − vMgod·age, A = (v0+vMgod)·(mass/drag),
        // vMgod = (0,0, mass·g/drag). Those two terms nearly cancel for a young,
        // low-drag element, so the tolerance scales with that cancelling magnitude:
        // the binary's in-image expf and std::exp differ by a couple ULP, which a
        // cancellation of scale S inflates to ~S·3e-5 (tight where well-conditioned).
        const f32 mOverDrag = mass / drag;
        const f32 gz = mass * g / drag;   // vMgod.z
        std::vector<Vector3f> pos(n);
        std::vector<f32> birthU(n);
        for (std::size_t k = 0; k < n; ++k) {
            const Vector3f bp = vec3(elemsIn[k]["birthPos"]);
            const Vector3f v0 = vec3(elemsIn[k]["v0"]);
            birthU[k] = elemsIn[k]["birthU"].F();
            const f32 age = headU - birthU[k];
            const vs::DragResult d = vs::CalculateDisplacementAndVelocity(
                age, v0, mass, 1.0f / mass, drag, 1.0f / drag, g);
            pos[k] = vs::Add(bp, d.displacement);
            const Vector3f got = vec3(elemsOut[k]["curPos"]);
            const f32 oneMinus = 1.0f - std::exp(-(drag / mass) * age);
            auto chk = [&](f32 want, f32 gotc, f32 v0c, f32 vMgodC, const char* w) {
                const f32 A = (v0c + vMgodC) * mOverDrag;
                const f32 tol = (std::fabs(A * oneMinus) + std::fabs(vMgodC * age))
                                    * 3e-5f + 1e-6f;
                INFO(w << " got=" << gotc << " want=" << want);
                REQUIRE(std::fabs(want - gotc) <= tol);
            };
            // Type3 leaves the NEWEST element (the head) untouched — its curPos
            // is UpdateHeadSegment's, not re-simulated — so only Type2 pins it.
            if (tech3 && k + 1 == n)
                continue;
            chk(pos[k].x, got.x, v0.x, 0.0f, "curPos.x");
            chk(pos[k].y, got.y, v0.y, 0.0f, "curPos.y");
            chk(pos[k].z, got.z, v0.z, gz, "curPos.z");
        }

        // Length-mode arc walk (cull 1) -> rpVScale / cutU / cutPos.
        if (in["cull"].I() == 1 && tag != "arc-headpos-off" && n >= 2) {
            const f32 maxLen = in["maxLength"].F();
            const std::size_t head = n - 1;
            std::vector<f32> arcFromHead(n, 0.0f);
            f32 acc = 0.0f;
            bool cutFound = false;
            f32 cutU = birthU.front();
            Vector3f cutPos = pos.front();
            for (std::size_t j = head; j > 0; --j) {
                const f32 seg = vs::Length3(vs::Sub(pos[j - 1], pos[j]));
                if (acc + seg >= maxLen && seg > 1e-6f) {
                    const f32 t = (maxLen - acc) / seg;
                    cutPos = vs::Lerp(pos[j], pos[j - 1], t);
                    arcFromHead[j - 1] = maxLen;
                    cutU = birthU[j] + (birthU[j - 1] - birthU[j]) * t;
                    cutFound = true;
                    break;
                }
                acc += seg;
                arcFromHead[j - 1] = acc;
            }
            // Uncut span keeps the binary's newest-birthU denominator to pin the
            // recorded truth; production anchors it to headU instead (see header).
            const f32 span = cutFound ? (headU - cutU) : (birthU.back() - cutU);
            const f32 rpVScale = (span <= 1e-6f) ? 0.0f
                               : cutFound        ? 1.0f / span
                                                 : (arcFromHead.front() / maxLen) / span;
            CloseRel(rpVScale, out["rpVScale"].F(), 3e-4f, "rpVScale");
            const auto& cut = out["rpTailCutPos"];
            if (cutFound) {
                CloseRel(cutPos.x, cut[0].F(), 3e-4f, "cutPos.x");
                CloseRel(cutPos.y, cut[1].F(), 3e-4f, "cutPos.y");
                CloseRel(cutPos.z, cut[2].F(), 3e-4f, "cutPos.z");
                CloseRel(cutU, cut[3].F(), 3e-4f, "cutU");
            }
        }

        // Tech-3 interior tangent = central difference (skip the head region and
        // the degenerate-segment vector, both documented deviations).
        if (tech3 && n >= 4 && tag != "tan-degenerate") {
            for (std::size_t k = 1; k + 2 < n; ++k) {
                const Vector3f cd = vs::Sub(pos[k + 1], pos[k - 1]);
                const Vector3f bt = vec3(elemsOut[k]["tangent"]);
                CloseRel(cd.x, bt.x, 3e-5f, "tangent.x");
                CloseRel(cd.y, bt.y, 3e-5f, "tangent.y");
                CloseRel(cd.z, bt.z, 3e-5f, "tangent.z");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// O7 — the legacy simulator CRibbon_Simulate_Type4 (0x1029604A0). Two pins:
//   * PHASE-1 EULER + DRAG (cull 0): each `steps` snapshot of every element's
//     pos/vel under the semi-implicit integrate — pos += (0.5·a·dt + v)·dt on the
//     OLD velocity, v += a·dt, v *= max(1 − invMass·drag·dt, 0), a = gravity3, the
//     damp floor 0.0. Pure float32 (no libm), reconstructed in the binary's op
//     order → rtol 1e-6 absorbs only FMA-contraction. This pins A4's integration.
//     The NEWEST element (the head) is EXCLUDED from phase-1 under renderFlags&2
//     (the shipped render config): the loop stops at the tail, so the head is
//     UpdateHeadSegment's, exactly as Simulate_Type3 leaves it. Our C++ integrates
//     every edge, moving the head one frame early — a head-of-ribbon cosmetic for
//     Track C; the golden shows the head unmoved and this asserts it (deviation).
//   * PHASE-2 LENGTH-MODE ARC V (cull 1) — DRIFT-3's tech-4 pin. The kept
//     (post-recycle) elements' arcNorm = arcFromHead/maxLength, reconstructed as
//     BuildStripSc2's tech-4 path does it (walk newest→oldest, interpolate the cut
//     onto arc = maxLength, drop beyond). flags&0x1000 maxes arcNorm with the age
//     fraction (headU − birthU)/(deathU − birthU). Arc length uses rsqrt+1 Newton
//     in the binary → rtol 3e-4. Every tested cut keeps ≥3 naturally, so the
//     binary's min-3 recycle floor (absent from our erase) never bites; the <3
//     degenerate case (pathologically short maxLength) is a documented deviation.
// ---------------------------------------------------------------------------
TEST_CASE("o7: the legacy simulator replays Euler+drag and the tech-4 arc V",
          "[ribbon][sc2_ribbon][oracle]") {
    const fs::path path = GoldenDir() / "o7_legacy.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string());
    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];

    auto vec3 = [](const wdx_golden::Value& a) {
        return Vector3f{a[0].F(), a[1].F(), a[2].F()};
    };

    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& out = c["out"];
        const std::string tag = c.Has("tag") ? c["tag"].S() : "";
        INFO("case " << i << " tag=" << tag);

        const auto& elemsIn = in["elems"];
        const std::size_t n = elemsIn.Size();

        if (in["cull"].I() == 0) {
            // Semi-implicit Euler + drag, reconstructed in the binary's op order.
            const f32 dt = in["dt"].F();
            const f32 halfDt = 0.5f * dt;
            const Vector3f a = vec3(in["gravity"]);
            const f32 dragCoeff = in["drag"].F() * dt;
            const f32 damping = (std::max)(1.0f - in["invMass"].F() * dragCoeff, 0.0f);
            std::vector<Vector3f> pos(n), vel(n);
            for (std::size_t k = 0; k < n; ++k) {
                pos[k] = vec3(elemsIn[k]["pos"]);
                vel[k] = vec3(elemsIn[k]["v0"]);
            }
            const int steps = in["steps"].I();
            for (int s = 0; s < steps; ++s) {
                const auto& snap = out["steps"][(std::size_t)s];
                for (std::size_t k = 0; k < n; ++k) {
                    // The head (newest) is excluded — its lanes stay at birth. The
                    // golden proves it; our C++ would move it (documented deviation).
                    if (k + 1 == n) {
                        const Vector3f gp = vec3(snap[k]["pos"]);
                        const Vector3f gv = vec3(snap[k]["vel"]);
                        const Vector3f bp = vec3(elemsIn[k]["pos"]);
                        const Vector3f bv = vec3(elemsIn[k]["v0"]);
                        CloseRel(bp.x, gp.x, 1e-6f, "head.pos.x (excluded)");
                        CloseRel(bp.y, gp.y, 1e-6f, "head.pos.y (excluded)");
                        CloseRel(bp.z, gp.z, 1e-6f, "head.pos.z (excluded)");
                        CloseRel(bv.x, gv.x, 1e-6f, "head.vel.x (excluded)");
                        CloseRel(bv.y, gv.y, 1e-6f, "head.vel.y (excluded)");
                        CloseRel(bv.z, gv.z, 1e-6f, "head.vel.z (excluded)");
                        continue;
                    }
                    const Vector3f ov = vel[k];
                    pos[k] = {(halfDt * a.x + ov.x) * dt + pos[k].x,
                              (halfDt * a.y + ov.y) * dt + pos[k].y,
                              (halfDt * a.z + ov.z) * dt + pos[k].z};
                    vel[k] = {(a.x * dt + ov.x) * damping, (a.y * dt + ov.y) * damping,
                              (a.z * dt + ov.z) * damping};
                    const Vector3f gp = vec3(snap[k]["pos"]);
                    const Vector3f gv = vec3(snap[k]["vel"]);
                    CloseRel(pos[k].x, gp.x, 1e-6f, "pos.x");
                    CloseRel(pos[k].y, gp.y, 1e-6f, "pos.y");
                    CloseRel(pos[k].z, gp.z, 1e-6f, "pos.z");
                    CloseRel(vel[k].x, gv.x, 1e-6f, "vel.x");
                    CloseRel(vel[k].y, gv.y, 1e-6f, "vel.y");
                    CloseRel(vel[k].z, gv.z, 1e-6f, "vel.z");
                }
            }
            continue;
        }

        // Length-mode arc V (cull 1). v0 = 0 and dt = 0, so phase-1 is a no-op and
        // the walk runs on the static positions — headPos = newest, so the virtual
        // head→newest segment is 0. Mirrors BuildStripSc2's tech-4 length path.
        const f32 maxLen = in["maxLength"].F();
        const f32 headU = in["headU"].F();
        const bool lengthAndTime = (in["flags"].U() & 0x1000u) != 0;
        std::vector<Vector3f> pos(n);
        std::vector<f32> birthU(n), deathU(n);
        for (std::size_t k = 0; k < n; ++k) {
            pos[k] = vec3(elemsIn[k]["pos"]);
            birthU[k] = elemsIn[k]["birthU"].F();
            deathU[k] = elemsIn[k]["deathU"].F();
        }
        const std::size_t head = n - 1;
        std::vector<f32> arcFromHead(n, 0.0f);
        f32 acc = 0.0f;
        std::size_t keepFrom = 0;
        bool cutFound = false;
        for (std::size_t j = head; j > 0; --j) {
            const f32 seg = vs::Length3(vs::Sub(pos[j - 1], pos[j]));
            if (acc + seg >= maxLen && seg > 1e-6f) {
                const f32 t = (maxLen - acc) / seg;
                pos[j - 1] = vs::Lerp(pos[j], pos[j - 1], t);
                arcFromHead[j - 1] = maxLen;
                keepFrom = j - 1;
                cutFound = true;
                break;
            }
            acc += seg;
            arcFromHead[j - 1] = acc;
        }
        (void)cutFound;
        const std::size_t keptN = n - keepFrom;
        REQUIRE(out["elementCount"].I() == (int)keptN);
        REQUIRE(out["kept"].Size() == keptN);
        for (std::size_t m = 0; m < keptN; ++m) {
            const std::size_t idx = keepFrom + m;
            f32 v = arcFromHead[idx] / maxLen;
            if (lengthAndTime) {
                const f32 ageV = (headU - birthU[idx]) / (deathU[idx] - birthU[idx]);
                v = (std::max)(v, ageV);
            }
            const auto& kp = out["kept"][m];
            CloseRel(v, kp["arcNorm"].F(), 3e-4f, "arcNorm");
            const Vector3f gp = vec3(kp["pos"]);
            CloseRel(pos[idx].x, gp.x, 3e-4f, "pos.x");
            CloseRel(pos[idx].y, gp.y, 3e-4f, "pos.y");
            CloseRel(pos[idx].z, gp.z, 3e-4f, "pos.z");
        }
    }
}

// ---------------------------------------------------------------------------
// O8 — the ground-collision response. `CRibbon_CollideSegment_Terrain`/`_Dual`
// share one response block, reconstructed here in the binary's op order and
// compared to the golden. For the shipped path (terrain-only, vertical normal)
// this reduces exactly to `Sc2GroundCollide` (ribbon_emitter.cpp:213); the
// Dual selector, forward query, and `tilt-normal` vectors pin binary behavior
// the viewer does not ship (deviations §5.2/5.3) — the golden records what the
// binary does, and the reconstruction matches it. `bounceCounter` is a
// binary-only field (element+60) the C++ never maintains.
// ---------------------------------------------------------------------------
TEST_CASE("o8: the ground collision replays the CollideSegment response",
          "[ribbon][sc2_ribbon][oracle]") {
    const fs::path path = GoldenDir() / "o8_collide.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string());
    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];

    auto vec3 = [](const wdx_golden::Value& a) {
        return Vector3f{a[0].F(), a[1].F(), a[2].F()};
    };
    struct Hit { Vector3f pos, nrm; f32 toi; bool ok; };
    auto hit = [&](const wdx_golden::Value& h) -> Hit {
        return {vec3(h["pos"]), vec3(h["nrm"]), h["toi"].F(), h["hit"].I() != 0};
    };

    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& out = c["out"];
        const std::string tag = c.Has("tag") ? c["tag"].S() : "";
        INFO("case " << i << " tag=" << tag);

        const Vector3f vel = vec3(in["vel"]);
        const f32 dt = in["dt"].F();
        const f32 friction = in["friction"].F();
        const f32 bounce = in["bounce"].F();
        const std::string func = in["func"].S();
        const std::uint32_t flags = in["flags"].U();
        const Hit terrain = hit(in["terrain"]);
        const Hit forward = hit(in["forward"]);

        // Which hit reaches the response, per the binary's control flow.
        Hit chosen{}; chosen.ok = false;
        if (func == "terrain") {
            // _Terrain: the direct branch fires only at toi==0; else the dead
            // forward fallback (which never hits in these vectors).
            if (terrain.ok && terrain.toi == 0.0f) chosen = terrain;
            else if (forward.ok) chosen = forward;
        } else {
            // _Dual: nearer toi of the enabled queries; terrain wins ties/misses.
            const bool t = (flags & 0x2u) && terrain.ok;
            const bool f = (flags & 0x4u) && forward.ok;
            if (t) chosen = terrain;
            if (f && (!t || terrain.toi > forward.toi)) chosen = forward;
        }

        // Response in the binary's op order (== Sc2GroundCollide for vertical n).
        Vector3f vNew = vel, pos = vec3(in["startPos"]);
        bool responded = false;
        if (chosen.ok) {
            const Vector3f n = chosen.nrm;
            const f32 vn = ((n.z * vel.z) + (n.x * vel.x)) + (n.y * vel.y);
            if (vn < 0.0f) {
                responded = true;
                const Vector3f vNorm = {vn * n.x, vn * n.y, n.z * vn};
                vNew = {-bounce * vNorm.x, -bounce * vNorm.y, -bounce * vNorm.z};
                const f32 speedSq =
                    ((vel.z * vel.z) + (vel.x * vel.x)) + (vel.y * vel.y);
                if (speedSq > 0.01f) {
                    vNew.x += friction * (vel.x - vNorm.x);
                    vNew.y += friction * (vel.y - vNorm.y);
                    vNew.z += (vel.z - vNorm.z) * friction;
                }
                const f32 rem = 1.0f - chosen.toi;
                pos = {(vNew.x * dt) * rem + chosen.pos.x,
                       (vNew.y * dt) * rem + chosen.pos.y,
                       (vNew.z * dt) * rem + chosen.pos.z};
            }
        }

        const Vector3f gv = vec3(out["vel"]);
        const Vector3f gp = vec3(out["pos"]);
        CloseRel(vNew.x, gv.x, 1e-6f, "vel.x");
        CloseRel(vNew.y, gv.y, 1e-6f, "vel.y");
        CloseRel(vNew.z, gv.z, 1e-6f, "vel.z");
        CloseRel(pos.x, gp.x, 1e-6f, "pos.x");
        CloseRel(pos.y, gp.y, 1e-6f, "pos.y");
        CloseRel(pos.z, gp.z, 1e-6f, "pos.z");
        REQUIRE(out["bounceCounter"].I() == (responded ? 1 : 0));
    }
}

// ---------------------------------------------------------------------------
// O9 — the spline simulator. One cubic Bezier per SRIB, rebuilt each frame from
// FOUR control points (SC2_RIBBON_RE.md §3.4, corrected by this gate):
//   C0 = emissionOffset            C3 = endOffset · R_srib
//   C1 = emissionVector · R_rib · baseFactor     (a DIRECT control point)
//   C2 = endTangent    · R_srib · endFactor      (a DIRECT control point)
// The gate's decisive find: the binary does NOT add the endpoints to the
// tangents — rot-srib records C2 == R_srib·endTangent and C3 == R_srib·endOffset
// with no cross term, so C2 = C3/4 exactly for endTangent=(0,0,1)/endOffset=
// (0,0,4). The two rotations run through __sincosf_stret, so every rotated lane
// replays at rtol 3e-4 (not bit-exact); identity/offset/factor cases are exact.
// Sag is gravity3·dtAccum² per control point and PERSISTS (sag += ... each call:
// the `sag-twice` vector doubles it), so a `calls`-fold multiplies it here.
// ---------------------------------------------------------------------------
TEST_CASE("o9: the spline simulator replays the cubic control points",
          "[ribbon][sc2_ribbon][oracle]") {
    const fs::path path = GoldenDir() / "o9_spline.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string());
    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];

    auto vec3 = [](const wdx_golden::Value& a) {
        return Vector3f{a[0].F(), a[1].F(), a[2].F()};
    };
    // Math_MatrixFromYawPitchRoll(roll=0) as a row-major 3x3, matching the
    // (anon-namespace, so inlined here) production YawPitchMat.
    auto ypm = [](f32 yawDeg, f32 pitchDeg, bool swap) {
        constexpr f32 kDegToRad = 0.017453292f;
        const f32 a2 = (swap ? yawDeg : pitchDeg) * kDegToRad;
        const f32 a3 = (swap ? pitchDeg : yawDeg) * kDegToRad;
        const f32 s2 = std::sin(a2), c2 = std::cos(a2);
        const f32 s3 = std::sin(a3), c3 = std::cos(a3);
        vs::Mat3 m{};
        m.m[0][0] = c3;   m.m[0][1] = s3 * s2;   m.m[0][2] = -s3 * c2;
        m.m[1][0] = 0.0f; m.m[1][1] = c2;        m.m[1][2] = s2;
        m.m[2][0] = s3;   m.m[2][1] = -s2 * c3;  m.m[2][2] = c3 * c2;
        return m;
    };

    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& out = c["out"];
        const std::string tag = c.Has("tag") ? c["tag"].S() : "";
        INFO("case " << i << " tag=" << tag);

        const bool swap = in["swap"].I() != 0;
        const vs::Mat3 rRib = ypm(in["ribYaw"].F(), in["ribPitch"].F(), swap);
        const vs::Mat3 rSrib = ypm(in["sribYaw"].F(), in["sribPitch"].F(), swap);
        // Emitter world is identity; the node frame is identity rotation + a
        // translation, applied to C2/C3 as points (transform_point) — never
        // added to each other.
        const Vector3f nt = vec3(in["nodeTranslate"]);
        const Vector3f c0 = vec3(in["emissionOffset"]);
        const Vector3f c1 =
            vs::Scale(vs::MulVecMat3(vec3(in["emissionVector"]), rRib),
                      in["velBaseFactor"].F());
        const Vector3f c2 = vs::Add(
            vs::Scale(vs::MulVecMat3(vec3(in["endTangent"]), rSrib),
                      in["velEndFactor"].F()), nt);
        const Vector3f c3 = vs::Add(vs::MulVecMat3(vec3(in["endOffset"]), rSrib), nt);

        const Vector3f g = vec3(in["gravity"]);
        const f32 age = in["age"].F();
        const f32 calls = static_cast<f32>(in["calls"].I());
        const f32 age2 = age * age * calls;   // persistent: `calls` folds add
        const Vector3f sag = {g.x * age2, g.y * age2, g.z * age2};

        const Vector3f p[4] = {vs::Add(c0, sag), vs::Add(c1, sag),
                               vs::Add(c2, sag), vs::Add(c3, sag)};

        // Control points WITH sag (CRibbon+632) and the sag field (CRibbon+680).
        const auto& gcp = out["cp"];
        const auto& gsag = out["sag"];
        const char* lbl[4] = {"C0", "C1", "C2", "C3"};
        for (int k = 0; k < 4; ++k) {
            CloseRel(p[k].x, gcp[k * 3 + 0].F(), 3e-4f, lbl[k]);
            CloseRel(p[k].y, gcp[k * 3 + 1].F(), 3e-4f, lbl[k]);
            CloseRel(p[k].z, gcp[k * 3 + 2].F(), 3e-4f, lbl[k]);
            CloseRel(sag.x, gsag[k * 3 + 0].F(), 3e-4f, "sag");
            CloseRel(sag.y, gsag[k * 3 + 1].F(), 3e-4f, "sag");
            CloseRel(sag.z, gsag[k * 3 + 2].F(), 3e-4f, "sag");
        }

        // 32-sample cubic B(t)=p0(1-t)^3+3p1·t(1-t)^2+3p2·t^2(1-t)+p3·t^3, t=i/(n-1).
        const auto& gs = out["samples"];
        const int n = static_cast<int>(gs.Size());
        for (int j = 0; j < n; ++j) {
            const f32 t = static_cast<f32>(j) / (n - 1.0f);
            const f32 u = 1.0f - t;
            const f32 w0 = u * u * u, w1 = 3.0f * t * u * u, w2 = 3.0f * t * t * u,
                      w3 = t * t * t;
            const Vector3f b = {
                p[0].x * w0 + p[1].x * w1 + p[2].x * w2 + p[3].x * w3,
                p[0].y * w0 + p[1].y * w1 + p[2].y * w2 + p[3].y * w3,
                p[0].z * w0 + p[1].z * w1 + p[2].z * w2 + p[3].z * w3};
            CloseRel(b.x, gs[j][0].F(), 3e-4f, "sample.x");
            CloseRel(b.y, gs[j][1].F(), 3e-4f, "sample.y");
            CloseRel(b.z, gs[j][2].F(), 3e-4f, "sample.z");
        }
    }
}

// ---------------------------------------------------------------------------
// O10 — the vertex-byte assembler. `FillVertices_Line` (0x1029587D0, the type
// 0/2/3 line/cylinder/star path) marches the element list and emits, per
// element, `subdivCount` 76-byte fat vertices that differ only by their
// cross-section slot; the GPU vertex shader (O12) extrudes each into a strip
// corner. This replays the golden with a direct reimplementation of the pack:
// pos/birthU/colours/velB/deathU are verbatim, and U/size/rot/corner/up are
// s16 snorm ((int)(x)-0x7FFF, low-16; size caps at nearZ then flips a negative
// component to 0x8001). Bit-exact — inline SSE, no libm. The C++ ribbon service
// extrudes on the CPU into a float vertex instead (RIBBON_SERVICE.md §9 item 7),
// so this pins the assembly + the element-attribute routing that path rests on,
// not EmitSc2Strip's output. The 120-byte animated fillers' noise offset is
// O13's.
// ---------------------------------------------------------------------------
TEST_CASE("o10: FillVertices_Line packs the fat vertex bit-exactly",
          "[ribbon][sc2_ribbon][oracle]") {
    const fs::path path = GoldenDir() / "o10_fillverts.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string());
    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 10);

    // Image snorm constants, bit-exact (gate_fillverts selfcheck pins them).
    const f32 uScale = std::bit_cast<f32>(0x477FFE00u);
    const f32 cornerScale = std::bit_cast<f32>(0x46FFFE00u);
    const f32 sizeScale = std::bit_cast<f32>(0x45CCCB33u);
    const f32 rotBias = std::bit_cast<f32>(0x441D1463u);
    const f32 rotScale = std::bit_cast<f32>(0x425099E8u);
    const f32 nearZ = 10.0f, startBlend = 1.0f;

    auto snorm = [](f32 x) -> i16 {  // (int)(x)-0x7FFF, low 16 bits
        return static_cast<i16>(static_cast<u16>(static_cast<int>(x) - 0x7FFF));
    };
    auto packU = [&](u32 bits) { return snorm(std::bit_cast<f32>(bits) * uScale); };
    auto packBias = [&](u32 bits, f32 bias, f32 scale) {
        return snorm((std::bit_cast<f32>(bits) + bias) * scale);
    };
    auto packSize = [&](u32 bits) -> i16 {
        const f32 s = std::bit_cast<f32>(bits);
        if (s < 0.0f)
            return static_cast<i16>(static_cast<u16>(0x8001));  // negative override
        return snorm((std::min)(nearZ, s) * sizeScale);
    };

    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& out = c["out"];
        const std::string tag = c.Has("tag") ? c["tag"].S() : "";
        INFO("case " << i << " tag=" << tag);

        const int subdiv = in["subdiv"].I();
        const int segCount = in["segCount"].I();
        const u32 seed = in["seed"].U();
        const auto& elems = in["elems"];
        const auto& uv = in["uv"];
        const auto& corner = in["corner"];

        // The filler returns filledCount and marches min(segCount, nElems).
        const u32 filled = out["filled"].U();
        REQUIRE(out["ret"].U() == filled);
        REQUIRE(filled == (u32)(std::min)(segCount, (int)elems.Size()));
        const auto& verts = out["verts"];
        REQUIRE(verts.Size() == filled * (u32)subdiv);

        std::size_t vi = 0;
        for (u32 e = 0; e < filled; ++e) {
            const auto& el = elems[e];
            for (int j = 0; j < subdiv; ++j, ++vi) {
                const auto& gv = verts[vi];
                INFO("elem " << e << " subdiv " << j);
                // Verbatim lanes: position+birthU, colours, velB+deathU, seed.
                for (int k = 0; k < 3; ++k)
                    REQUIRE(gv["pos"][k].U() == el["pos"][k].U());
                REQUIRE(gv["w"].U() == el["birthU"].U());
                for (int k = 0; k < 3; ++k)
                    REQUIRE(gv["col"][k].U() == el["col"][k].U());
                for (int k = 0; k < 3; ++k)
                    REQUIRE(gv["velB"][k].U() == el["velB"][k].U());
                REQUIRE(gv["deathU"].U() == el["deathU"].U());
                REQUIRE(gv["seed"].U() == seed);
                // Snorm lanes: size (cap+neg override), rotation, up ((v+1)·scale).
                for (int k = 0; k < 3; ++k) {
                    REQUIRE((i16)gv["size"][k].I() == packSize(el["size3"][k].U()));
                    REQUIRE((i16)gv["rot"][k].I() ==
                            packBias(el["rot3"][k].U(), rotBias, rotScale));
                    REQUIRE((i16)gv["up"][k].I() ==
                            packBias(el["up"][k].U(), startBlend, cornerScale));
                }
                // Cross-section lanes: U from uvTable[j], corner pair from
                // colorTable[2j..2j+1] — depend on the slot, not the element.
                REQUIRE((i16)gv["u"].I() == packU(uv[j].U()));
                REQUIRE((i16)gv["corner"][0].I() ==
                        packBias(corner[2 * j].U(), startBlend, cornerScale));
                REQUIRE((i16)gv["corner"][1].I() ==
                        packBias(corner[2 * j + 1].U(), startBlend, cornerScale));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// O11 — the overlay-wave sampler. `sc2::SampleWave` replays
// M3_SampleAnimValue: type 0 off, 1 sin·amp, 2 cos·amp, 3 saw
// (amp·(2·fmod(phase,1)−1)), 4 square.
// Types 0/4 are bit-exact; 1/2 go through libm sin/cos and 3 narrows a double
// fmod, so those carry a ULP bound. Type 6 is `Noise1D_Sample` over a second
// seed-0 table, which the gate builds the way `InitFunc_3733` does; it reads
// only `perm` and `grad1`, neither through rsqrt, so it is bit-exact too. Type 5
// (the global RNG) is a documented deviation the gate does not record.
// ---------------------------------------------------------------------------
TEST_CASE("o11: the overlay-wave sampler replays M3_SampleAnimValue",
          "[ribbon][sc2_ribbon][oracle]") {
    const fs::path path = GoldenDir() / "o11_waves.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string());
    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 100);
    usize noiseRows = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const u32 type = in["type"].U();
        const f32 phase = Bits(in["phaseBits"]);
        const f32 amp = Bits(in["ampBits"]);
        const f32 want = Bits(c["out"]["retBits"]);
        const f32 got = sc2::SampleWave(type, phase, amp);
        INFO("case " << i << " type=" << type << " phase=" << phase << " amp=" << amp);
        noiseRows += type == 6 ? 1u : 0u;
        if (type == 0 || type == 4 || type == 6)
            REQUIRE(std::bit_cast<u32>(got) == std::bit_cast<u32>(want)); // exact
        else
            CloseRel(got, want, 1e-6f, "wave"); // 1/2 sin/cos, 3 double fmod
    }
    // An older golden has no type-6 rows, and a pass over it says nothing
    // about the noise lane.
    CHECK(noiseRows >= 30u);
}

// ---------------------------------------------------------------------------
// O13 — the animated fillers' noise. `NoiseTable::Sample3D` replays the 3-D
// sampler both 120-byte fillers call (`sub_100D69F60`) over the seed-0 table,
// and `Sc2NoiseDisplacement` the envelope they wrap it in: which t, which of the
// two mutes, the three z lanes. Positions inherit the table's rtol 2e-6 — its
// gradients went through rsqrtss, which the emulator rounds correctly and the
// hardware does not.
// ---------------------------------------------------------------------------
TEST_CASE("o13: Sample3D replays the fillers' 3-D noise sampler",
          "[ribbon][sc2_ribbon][oracle]") {
    const fs::path path = GoldenDir() / "o13_noise3d.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string());
    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 1000);
    const auto& table = whiteout::flakes::renderer::sc2::GlobalNoiseTable();
    usize wraps = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        if (in.Has("check")) {
            // The shipped wrap copy is `grad3[i % 256]`, which is what lets the
            // port fold its unmasked index rather than carry the copy.
            CHECK(c["out"]["equal"].B());
            continue;
        }
        wraps += c["tag"].S() == "wrap" ? 1u : 0u;
        const f32 x = Bits(in["xBits"]), y = Bits(in["yBits"]), z = Bits(in["zBits"]);
        INFO("case " << i << " x=" << x << " y=" << y << " z=" << z);
        CloseRel(table.Sample3D(x, y, z), Bits(c["out"]["retBits"]), 2e-6f, "noise3d");
    }
    // The rows whose corner index runs past 255: without them a masked index
    // would pass as well.
    CHECK(wraps > 100u);
}

TEST_CASE("o13: the animated fillers displace each vertex by the retail envelope",
          "[ribbon][sc2_ribbon][oracle]") {
    const fs::path path = GoldenDir() / "o13_fillnoise.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string());
    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 10);
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& out = c["out"];
        const bool lifetime = in["kind"].S() == "animlife";
        const f32 headU = Bits(in["headUBits"]);
        const f32 amp = Bits(in["ampBits"]), freq = Bits(in["freqBits"]);
        const f32 coh = Bits(in["cohBits"]), edge = Bits(in["edgeBits"]);
        const bool spline = in["spline"].B();
        const u32 subdiv = in["subdiv"].U();
        const u32 filled = out["filled"].U();
        const auto& elems = in["elems"];
        const auto& verts = out["verts"];
        INFO("case " << i << " (" << c["tag"].S() << ")");
        REQUIRE(filled <= elems.Size());
        REQUIRE(verts.Size() == static_cast<std::size_t>(filled) * subdiv);
        for (u32 e = 0; e < filled; ++e) {
            const auto& el = elems[e];
            const f32 birthU = Bits(el["birthU"]), deathU = Bits(el["deathU"]);
            // `AnimatedLifetime` takes the life fraction, `Animated` the
            // element's arc parameter.
            const f32 t = lifetime ? (headU - birthU) / (deathU - birthU) : Bits(el["arcNorm"]);
            const Vector3f off = sc2::NoiseDisplacement(t, headU, amp, freq, coh, edge, spline);
            const f32 got[3] = {off.x + Bits(el["pos"][0]), off.y + Bits(el["pos"][1]),
                                off.z + Bits(el["pos"][2])};
            for (u32 s = 0; s < subdiv; ++s) {
                const auto& v = verts[e * subdiv + s];
                INFO("element " << e << " slot " << s << " t=" << t);
                // The t the filler used is exact, so a wrong one shows here and
                // not as a noise sample that happens to be off.
                REQUIRE(std::bit_cast<u32>(t) == v["t"].U());
                for (usize k = 0; k < 3; ++k)
                    CloseRel(got[k], Bits(v["pos"][k]), 2e-6f, "position");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// O12 — the vertex-shader math. `sc2/sc2_element_math.h` is the CPU
// transcription of the shipped .fx family; this replays the refimpl golden
// against it so the two stay identical (the slang M3_RIBBON permutation
// implements the same math on the GPU). The W3 BUILD uses these functions; the
// drag closed form and the GPU spline (fn "drag"/"spline"/"splineup") land
// with W4/W5, so those cases are skipped here and picked up when their
// consumers exist.
// ---------------------------------------------------------------------------
namespace {

Vector3f Vec(const wdx_golden::Value& a) {
    return {a[0].F(), a[1].F(), a[2].F()};
}
void CloseVec(const Vector3f& got, const wdx_golden::Value& want, f32 rtol,
              const char* what) {
    CloseRel(got.x, want[0].F(), rtol, what);
    CloseRel(got.y, want[1].F(), rtol, what);
    CloseRel(got.z, want[2].F(), rtol, what);
}

} // namespace

TEST_CASE("o12: the CPU vertex math matches the .fx refimpl",
          "[ribbon][sc2_ribbon][oracle]") {
    const fs::path path = GoldenDir() / "o12_vsmath.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string());
    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];

    // The refimpl's fixed control values (gate_vsmath.py) — the golden `in`
    // carries only the varying inputs, so these must match verbatim.
    const f32 v0s = 0.25f, v1s = 1.5f, v2s = 0.75f;
    const Vector3f v03 = {0.25f, 1.0f, -0.5f}, v13 = {1.5f, 0.125f, 2.0f},
                   v23 = {0.75f, -1.25f, 0.5f};
    const Vector3f rotVec = {0.3f, -0.7f, 0.64f};
    const Vector3f frameTan0 = {1, 0, 0};
    const Vector3f frameTan1 = vs::Normalize3({1, 1, 0.5f});
    const Vector3f frameCam = vs::Normalize3({0.2f, -1.0f, -0.3f});
    const Vector3f frameUp = {0, 0, 1};

    int seen = 0, skipped = 0;
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const auto& out = c["out"];
        const std::string fn = in["fn"].S();
        INFO("case " << i << " fn=" << fn);

        if (fn == "interp") {
            const int mode = in["mode"].I();
            const f32 age = in["age"].F(), mid = in["mid"].F(), hold = in["hold"].F();
            const f32 invMid = 1.0f / mid;
            const f32 s =
                vs::InterpolateValue(age, v0s, v1s, v2s, mid, invMid, hold, mode);
            const Vector3f v =
                vs::InterpolateValue3(age, v03, v13, v23, mid, invMid, hold, mode);
            CloseRel(s, out["scalar"].F(), 2e-6f, "interp.scalar");
            CloseVec(v, out["vec3"], 2e-6f, "interp.vec3");
            ++seen;
        } else if (fn == "twist") {
            const auto& r = in["rot"];
            const f32 a = vs::TwistAngle(in["age"].F(), r[0].F(), r[1].F(),
                                         r[2].F(), in["rotMid"].F());
            CloseRel(a, out["angle"].F(), 2e-6f, "twist.angle");
            ++seen;
        } else if (fn == "rotate") {
            const vs::Mat3 m = vs::MakeRotation(in["angle"].F(), Vec(in["axis"]));
            const Vector3f r = vs::MulVecMat3(rotVec, m);
            CloseVec(r, out["rotated"], 1e-5f, "rotate"); // trig lanes
            ++seen;
        } else if (fn == "safenorm") {
            const Vector3f r = vs::SafeNormalize(Vec(in["v"]), Vec(in["d"]));
            CloseVec(r, out["out"], 2e-6f, "safenorm");
            ++seen;
        } else if (fn == "age") {
            const f32 a = vs::FAge(in["headU"].F(), in["birthU"].F(),
                                   in["deathU"].F(), in["ageScalar"].F());
            const f32 v = vs::VFromAge(a, -1.0f, 1.0f);
            CloseRel(a, out["fAge"].F(), 2e-6f, "age.fAge");
            CloseRel(v, out["v"].F(), 2e-6f, "age.v");
            ++seen;
        } else if (fn == "frame") {
            const int type = in["type"].I();
            const bool smooth = in["smooth"].B();
            const Vector3f tan = (in["tangent"].I() == 0) ? frameTan0 : frameTan1;
            const vs::Frame f = vs::BuildFrame(type, tan, frameUp, frameCam,
                                               0.70710678f, 0.70710678f, 0.5f,
                                               smooth);
            CloseVec(f.offset, out["offset"], 1e-5f, "frame.offset");
            CloseVec(f.normal, out["normal"], 1e-5f, "frame.normal");
            CloseVec(f.tangent, out["vtangent"], 1e-5f, "frame.vtangent");
            CloseVec(f.binormal, out["binormal"], 1e-5f, "frame.binormal");
            ++seen;
        } else if (fn == "drag") {
            const f32 mass = in["mass"].F(), drag = in["drag"].F();
            const vs::DragResult r = vs::CalculateDisplacementAndVelocity(
                in["t"].F(), Vec(in["v0"]), mass, 1.0f / mass, drag, 1.0f / drag,
                in["gravity"].F());
            CloseVec(r.displacement, out["displacement"], 1e-5f, "drag.disp");
            CloseVec(r.velocity, out["velocity"], 1e-5f, "drag.vel");
            ++seen;
        } else {
            ++skipped; // spline / splineup — W5 consumers.
        }
    }
    INFO("replayed " << seen << " vs-math cases, skipped " << skipped
                     << " (W4/W5 fn kinds)");
    REQUIRE(seen > 0);
}
