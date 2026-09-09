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
#include "renderer/ribbon/ribbon_vs_math.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

using namespace whiteout::flakes;
using namespace whiteout::flakes::renderer::ribbon;
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

        const u8 got = SelectSc2SimTechnique(cfg);
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
// O3 — the emit clock. `Sc2EmitGate` replays UpdateEmit bit-for-bit: the
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
        g.simTechnique = static_cast<u8>(in["tech"].I());
        g.haveHead = (buffer == "head");
        g.headU = Bits(in["headU"]);
        g.headBirthU = Bits(in["birthU"]);
        g.headElemPos = {Bits(in["elemX"]), 0.0f, 0.0f};
        g.quality = in["quality"].I();
        g.lodCut = in["lodCut"].I();
        g.lodReduce = in["lodReduce"].I();
        g.cullMethod = static_cast<u8>(in["cull"].I());
        g.emissionScale = Bits(in["scale"]);
        g.divisions = Bits(in["div"]);
        g.lifetimeAux = Bits(in["lifeAux"]);
        g.maxLengthAux = Bits(in["maxAux"]);
        g.dt = Bits(in["dt"]);
        g.sampledActive = in["sampleActive"].I() != 0;
        g.nodeActive = in["nodeActive"].I() != 0;
        g.elementCount = in["elemCount"].U();

        const sc2::EmitGateResult r = sc2::Sc2EmitGate(clk, g);
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
        h.simTechnique = static_cast<u8>(in["tech"].I());
        h.ribbonType = static_cast<u8>(in["ribbonType"].I());
        h.cullMethod = static_cast<u8>(in["cull"].I());
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

        sc2::HeadElement e = sc2::Sc2WriteHead(h);
        // The 1e-4 stationary floor (techs 0/2/3) moved out of Sc2WriteHead to
        // the caller (DRIFT-1: the binary floors AFTER transform+inherit). This
        // golden is the full UpdateHeadSegment and its subset is local +
        // no-inherit + identity, so replay the floor here on the local dir.
        if (h.simTechnique == 0 || h.simTechnique == 2 || h.simTechnique == 3) {
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
// waves / world). Replays Sc2WriteHead (waves + swap) then the
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
// velocity magnitude in the binary, but Sc2WriteHead only has the local
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
        h.simTechnique = static_cast<u8>(in["tech"].I());
        h.ribbonType = static_cast<u8>(in["ribbonType"].I());
        h.cullMethod = static_cast<u8>(in["cull"].I());
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

        const sc2::HeadElement e0 = sc2::Sc2WriteHead(h);

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
        if (h.simTechnique == 0 || h.simTechnique == 2 || h.simTechnique == 3) {
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
        if (!(h.cullMethod == 1 && inherit))
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

        const sc2::CatchUpResult r = sc2::Sc2CatchUpTicks(cull, speed, lifetime, maxLen);
        INFO("case " << i << " tag=" << (c.Has("tag") ? c["tag"].S() : ""));
        REQUIRE(r.ticks == out["ticks"].I());
    }
}

// ---------------------------------------------------------------------------
// O11 — the overlay-wave sampler. `Sc2SampleWave` replays M3_SampleAnimValue:
// type 0 off, 1 sin·amp, 2 cos·amp, 3 saw (amp·(2·fmod(phase,1)−1)), 4 square.
// Types 0/4 are bit-exact; 1/2 go through libm sin/cos and 3 narrows a double
// fmod, so those carry a ULP bound. Type 5 (RNG) and type 6 (Noise1D over a
// runtime-initialized table) are documented deviations the C++ returns 0 for and
// the gate does not record.
// ---------------------------------------------------------------------------
TEST_CASE("o11: the overlay-wave sampler replays M3_SampleAnimValue",
          "[ribbon][sc2_ribbon][oracle]") {
    const fs::path path = GoldenDir() / "o11_waves.json";
    if (!fs::exists(path))
        SKIP("no golden at " + path.string());
    const auto doc = wdx_golden::Load(path.string());
    const auto& cases = (*doc)["cases"];
    REQUIRE(cases.Size() >= 100);
    for (std::size_t i = 0; i < cases.Size(); ++i) {
        const auto& c = cases[i];
        const auto& in = c["in"];
        const u32 type = in["type"].U();
        const f32 phase = Bits(in["phaseBits"]);
        const f32 amp = Bits(in["ampBits"]);
        const f32 want = Bits(c["out"]["retBits"]);
        const f32 got = sc2::Sc2SampleWave(type, phase, amp);
        INFO("case " << i << " type=" << type << " phase=" << phase << " amp=" << amp);
        if (type == 0 || type == 4)
            REQUIRE(std::bit_cast<u32>(got) == std::bit_cast<u32>(want)); // exact
        else
            CloseRel(got, want, 1e-6f, "wave"); // 1/2 sin/cos, 3 double fmod
    }
}

// ---------------------------------------------------------------------------
// O12 — the vertex-shader math. `ribbon_vs_math.h` is the CPU transcription of
// the shipped .fx family; this replays the refimpl golden against it so the
// two stay identical (the slang M3_RIBBON permutation implements the same math
// on the GPU). The W3 BUILD uses these functions; the drag closed form and the
// GPU spline (fn "drag"/"spline"/"splineup") land with W4/W5, so those cases
// are skipped here and picked up when their consumers exist.
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
