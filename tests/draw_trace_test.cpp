// The draw-path regression harness itself (gate G1). `--draw-trace`'s verdict
// is only as trustworthy as CompareTraces: a comparator that stopped looking at
// a field would print MATCH while every regression in that field walked
// through, and with ~30 recorded fields that is the difference between a gate
// and a green light. So the comparator is fed a deliberate divergence in each
// field it claims to cover.

#include <catch2/catch_test_macros.hpp>

#include "renderer/debug/draw_trace.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

using namespace whiteout::flakes::renderer::debug;
using whiteout::flakes::f32;
using whiteout::flakes::i32;
using whiteout::flakes::u8;

namespace {

// A scratch file under the system temp dir, removed when the test ends.
class TempFile {
public:
    explicit TempFile(const char* name) : path_(fs::temp_directory_path() / name) {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    ~TempFile() {
        std::error_code ec;
        fs::remove(path_, ec);
    }

    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;

    std::string Path() const {
        return path_.string();
    }
    void Write(const std::string& text) const {
        std::ofstream(path_, std::ios::binary) << text;
    }
    std::string Read() const {
        std::ifstream in(path_, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }

private:
    fs::path path_;
};

// Distinct, awkward values in every field on purpose: a field copied from the
// wrong source shows up rather than coinciding, and the %.9g round-trip is
// exercised on values that need all nine digits.
TraceDraw MakeDraw() {
    TraceDraw d;
    d.passSlot = static_cast<u8>(TracePassSlot::TransparentScene);
    d.producer = static_cast<u8>(TraceProducer::Geoset);
    d.shadingModel = static_cast<u8>(TraceShadingModel::Wc3Hd);
    d.blendClass = 3;
    d.depthFill = 1;
    d.actor = {.rootActor = 17, .role = 2, .treeDepth = 1, .emitterId = 4, .slotIndex = 6};
    d.submesh = 9;
    d.surface = 5;
    d.layer = 1;
    d.lod = 2;
    d.priorityPlane = 7;
    d.sortOrder = 11;
    d.sqDist = 12345.678901f;
    d.underWater = 1;
    d.filterMode = 4;
    d.matFlags = 0x30;
    for (i32 s = 0; s < kTraceTexSlots; ++s)
        d.texIds[s] = 100 + s;
    d.texAnimId = 3;
    d.indexCount = 240;
    d.vertexCount = 96;
    d.streamMask = kStreamBase | kStreamTangent | kStreamBone;
    d.palettePath = 1;
    d.paletteSlots = 42;
    d.lightCount = 5;
    d.lightPaletteHash = 0xDEADBEEFCAFEBABEull;
    d.psoKey = 0x1234ABCDu;
    d.cbHash = 0x0123456789ABCDEFull;
    d.combinedAlpha = 0.333333343f;
    d.texMtxHash = 0xFEDCBA9876543210ull;
    return d;
}

DrawTrace MakeTrace() {
    TraceFrame f;
    f.frame = 0;
    f.draws.push_back(MakeDraw());

    TraceDraw second = MakeDraw();
    second.producer = static_cast<u8>(TraceProducer::Particle);
    second.sortOrder = 12;
    second.submesh = -1;
    f.draws.push_back(second);

    DrawTrace t;
    t.frames.push_back(f);
    return t;
}

bool Mentions(const std::string& report, const char* what) {
    return report.find(what) != std::string::npos;
}

} // namespace

TEST_CASE("A trace compares identical to itself") {
    const DrawTrace t = MakeTrace();
    std::string report;
    REQUIRE(CompareTraces(t, t, CompareTolerance{}, report));
    REQUIRE(report == "identical");
}

TEST_CASE("Every field the draw trace records is actually compared") {
    const DrawTrace base = MakeTrace();
    DrawTrace other = MakeTrace();
    std::string report;
    const char* field = nullptr;
    TraceDraw& d = other.frames[0].draws[0];

    SECTION("pass slot") {
        d.passSlot = static_cast<u8>(TracePassSlot::OpaqueColor);
        field = "passSlot";
    }
    SECTION("producer") {
        d.producer = static_cast<u8>(TraceProducer::Ribbon);
        field = "producer";
    }
    SECTION("shading model") {
        d.shadingModel = static_cast<u8>(TraceShadingModel::Wc3Sd);
        field = "shadingModel";
    }
    SECTION("blend class") {
        d.blendClass = 2;
        field = "blendClass";
    }
    SECTION("depth fill") {
        d.depthFill = 2;
        field = "depthFill";
    }
    SECTION("root actor") {
        d.actor.rootActor = 18;
        field = "actor.rootActor";
    }
    SECTION("actor role") {
        d.actor.role = 3;
        field = "actor.role";
    }
    SECTION("actor tree depth") {
        d.actor.treeDepth = 2;
        field = "actor.treeDepth";
    }
    SECTION("spawning emitter") {
        d.actor.emitterId = 5;
        field = "actor.emitterId";
    }
    SECTION("spawning attachment slot") {
        d.actor.slotIndex = 7;
        field = "actor.slotIndex";
    }
    SECTION("submesh") {
        d.submesh = 10;
        field = "submesh";
    }
    SECTION("surface") {
        d.surface = 6;
        field = "surface";
    }
    SECTION("layer") {
        d.layer = 0;
        field = "layer";
    }
    SECTION("lod") {
        d.lod = 3;
        field = "lod";
    }
    SECTION("priority plane") {
        d.priorityPlane = 8;
        field = "priorityPlane";
    }
    SECTION("sort order") {
        d.sortOrder = 99;
        field = "sortOrder";
    }
    SECTION("camera distance") {
        d.sqDist += 1.0f;
        field = "sqDist";
    }
    SECTION("underwater flag") {
        d.underWater = 0;
        field = "underWater";
    }
    SECTION("filter mode") {
        d.filterMode = 1;
        field = "filterMode";
    }
    SECTION("material flags") {
        d.matFlags = 0x10;
        field = "matFlags";
    }
    SECTION("a resolved texture id") {
        // Slot 4 rather than slot 0: a comparator that only looked at the
        // albedo would still pass the obvious case.
        d.texIds[4] = 999;
        field = "texIds";
    }
    SECTION("texture animation id") {
        d.texAnimId = 4;
        field = "texAnimId";
    }
    SECTION("index count") {
        d.indexCount = 120;
        field = "indexCount";
    }
    SECTION("vertex count") {
        d.vertexCount = 48;
        field = "vertexCount";
    }
    SECTION("stream mask") {
        d.streamMask = kStreamBase;
        field = "streamMask";
    }
    SECTION("bone palette path") {
        d.palettePath = 2;
        field = "palettePath";
    }
    SECTION("bone palette size") {
        d.paletteSlots = 43;
        field = "paletteSlots";
    }
    SECTION("light count") {
        d.lightCount = 4;
        field = "lightCount";
    }
    SECTION("the resolved light palette") {
        d.lightPaletteHash ^= 1ull;
        field = "lightPaletteHash";
    }
    SECTION("pso key") {
        d.psoKey ^= 1u;
        field = "psoKey";
    }
    SECTION("constant-buffer values") {
        d.cbHash ^= 1ull;
        field = "cbHash";
    }
    SECTION("combined alpha") {
        d.combinedAlpha = 0.5f;
        field = "combinedAlpha";
    }
    SECTION("texture animation matrices") {
        d.texMtxHash ^= 1ull;
        field = "texMtxHash";
    }
    SECTION("draw count") {
        other.frames[0].draws.pop_back();
        field = "draw count";
    }
    SECTION("frame count") {
        other.frames.push_back(other.frames[0]);
        field = "frame count";
    }

    REQUIRE_FALSE(CompareTraces(base, other, CompareTolerance{}, report));
    INFO(report);
    REQUIRE(Mentions(report, field));
}

TEST_CASE("Submit order is what is under test, not the set of draws") {
    // Swapping two draws leaves every per-draw field and the draw count intact.
    // The transparent queue's back-to-front interleave is the most fragile
    // behaviour in the renderer, so a positional comparison is the point.
    const DrawTrace base = MakeTrace();
    DrawTrace other = MakeTrace();
    std::swap(other.frames[0].draws[0], other.frames[0].draws[1]);

    std::string report;
    REQUIRE_FALSE(CompareTraces(base, other, CompareTolerance{}, report));
    REQUIRE(Mentions(report, "producer"));
}

TEST_CASE("A draw trace round-trips through its file format exactly") {
    const DrawTrace base = MakeTrace();
    TempFile file("wdx_draw_trace_roundtrip.txt");

    std::string err;
    REQUIRE(WriteTrace(base, file.Path(), err));

    DrawTrace read;
    REQUIRE(ReadTrace(read, file.Path(), err));

    // The point of %.9g: a baseline file is an equality test, not an
    // approximate one, so the floats must survive text unchanged.
    std::string report;
    REQUIRE(CompareTraces(base, read, CompareTolerance{}, report));
    REQUIRE(report == "identical");
    REQUIRE(read.frames[0].draws[0].sqDist == base.frames[0].draws[0].sqDist);
    REQUIRE(read.frames[0].draws[0].combinedAlpha == base.frames[0].draws[0].combinedAlpha);
}

TEST_CASE("A tolerance and an exact constant-buffer hash are refused together") {
    // Mirrors CompareTolerance::requireVertexHash in particle_trace: any
    // perturbation big enough to need the tolerance also moves the CB bytes,
    // so accepting both would silently prefer one and disarm the other.
    const DrawTrace t = MakeTrace();
    CompareTolerance tol;
    tol.distance = 1e-5f;
    tol.requireCbHash = true;

    std::string report;
    REQUIRE_FALSE(CompareTraces(t, t, tol, report));
    REQUIRE(Mentions(report, "requireCbHash"));
}

TEST_CASE("Zero tolerance means bit-identical") {
    const DrawTrace base = MakeTrace();
    DrawTrace other = MakeTrace();
    other.frames[0].draws[0].sqDist += 1e-3f;

    std::string report;
    REQUIRE_FALSE(CompareTraces(base, other, CompareTolerance{}, report));
    REQUIRE(Mentions(report, "sqDist"));
}

TEST_CASE("A distance tolerance admits float rounding but not real drift") {
    const DrawTrace base = MakeTrace();
    CompareTolerance tol;
    tol.distance = 1e-5f;
    tol.requireCbHash = false;

    DrawTrace nudged = MakeTrace();
    nudged.frames[0].draws[0].sqDist *= (1.0f + 1e-7f);
    std::string report;
    REQUIRE(CompareTraces(base, nudged, tol, report));

    DrawTrace drifted = MakeTrace();
    drifted.frames[0].draws[0].sqDist *= 1.01f;
    REQUIRE_FALSE(CompareTraces(base, drifted, tol, report));
    REQUIRE(Mentions(report, "sqDist"));
}

TEST_CASE("The reader accepts a baseline recorded before a column existed") {
    // Why this matters: a baseline recorded before an output axis existed is
    // exactly what the phase that introduced it needs to be checked against.
    // Pre-declaring the schema reduces re-baselines; it does not eliminate
    // them, so the reader has to tolerate a short line.
    TempFile file("wdx_draw_trace_short.txt");
    file.Write("wdt1\n"
               "f 0 1\n"
               // Through `matFlags` only — everything after keeps its default.
               "d 1 0 1 3 1 17 2 1 4 6 9 5 1 2 7 11 12345.6787 1 4 48\n");

    DrawTrace t;
    std::string err;
    REQUIRE(ReadTrace(t, file.Path(), err));
    REQUIRE(t.frames.size() == 1);
    REQUIRE(t.frames[0].draws.size() == 1);
    const TraceDraw& d = t.frames[0].draws[0];
    REQUIRE(d.submesh == 9);
    REQUIRE(d.matFlags == 48);
    REQUIRE(d.texIds[0] == -1);
    REQUIRE(d.cbHash == 0);
}

TEST_CASE("The reader rejects a file that is not a draw trace") {
    TempFile file("wdx_draw_trace_bad.txt");
    file.Write("wpt2\nf 0 0\n");

    DrawTrace t;
    std::string err;
    REQUIRE_FALSE(ReadTrace(t, file.Path(), err));
    REQUIRE(Mentions(err, "magic"));
}

TEST_CASE("The PSO key covers the decisions and excludes the environment") {
    // HashRequest seeds with the program pointer and folds in rtv/dsv formats,
    // which move with ASLR and vary by vendor and backend. That is the exact
    // defect "never record raw gfx handles" exists for, so the trace records
    // the decision inputs instead — and every one of them has to matter.
    const TracePsoInputs base{};
    REQUIRE(TracePsoKey(base) == TracePsoKey(TracePsoInputs{}));

    auto differs = [&](TracePsoInputs in) { return TracePsoKey(in) != TracePsoKey(base); };
    REQUIRE(differs({.vsPermute = 1}));
    REQUIRE(differs({.psPermute = 1}));
    REQUIRE(differs({.matAlpha = 1}));
    REQUIRE(differs({.disables = 1}));
    REQUIRE(differs({.vertexLayout = 1}));
    REQUIRE(differs({.extraRtvCount = 1}));
    // The HD fading-opaque prepass twin and its colour draw differ on this
    // alone; without it the two collide onto one key.
    REQUIRE(differs({.extraColorWrite = true}));
    REQUIRE(differs({.wireframe = true}));
    REQUIRE(differs({.lhClipSpace = true}));
}
