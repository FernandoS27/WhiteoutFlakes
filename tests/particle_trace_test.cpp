// The particle regression harness itself. `--particle-diff`'s verdict is only
// as trustworthy as CompareTraces: a comparator that stopped looking at a field
// would print MATCH while every regression in that field walked through. So the
// comparator is fed a deliberate divergence in each field it claims to cover.

#include <catch2/catch_test_macros.hpp>

#include "renderer/particle/particle_trace.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

using namespace whiteout::flakes::renderer::particle;
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

private:
    fs::path path_;
};

// Two particles and a full L2 summary — awkward values on purpose so a field
// copied from the wrong source shows up rather than coinciding.
Trace MakeTrace() {
    TraceEmitter e;
    e.model = 7;
    e.output = static_cast<u8>(ParticleOutput::Billboard);
    e.emitterId = 2;
    e.particles.push_back({{1.0f, 2.0f, 3.0f}, {0.25f, -0.5f, 4.0f}, 0.125f, 1u});
    e.particles.push_back({{-8.0f, 0.5f, 16.0f}, {1.5f, 0.0f, -2.25f}, 0.75f, 3u});
    e.vertexCount = 12;
    e.vertexHash = 0xDEADBEEFCAFEBABEull;
    e.priorityPlane = 4;
    e.boundsMin = {-9.0f, -1.0f, 2.0f};
    e.boundsMax = {2.0f, 3.0f, 17.0f};
    e.meanColor = {0.5f, 0.25f, 0.125f, 1.0f};

    TraceFrame f;
    f.frame = 0;
    f.emitters.push_back(e);

    Trace t;
    t.frames.push_back(f);
    return t;
}

bool Mentions(const std::string& report, const char* what) {
    return report.find(what) != std::string::npos;
}

} // namespace

TEST_CASE("A trace compares identical to itself") {
    const Trace t = MakeTrace();
    std::string report;
    REQUIRE(CompareTraces(t, t, CompareTolerance{}, report));
    REQUIRE(report == "identical");
}

TEST_CASE("Every field the trace records is actually compared") {
    const Trace base = MakeTrace();
    Trace other = MakeTrace();
    std::string report;
    const char* field = nullptr;

    SECTION("particle position") {
        other.frames[0].emitters[0].particles[1].position.y += 0.5f;
        field = "position";
    }
    SECTION("particle velocity") {
        other.frames[0].emitters[0].particles[0].velocity.z += 0.5f;
        field = "velocity";
    }
    SECTION("particle age") {
        other.frames[0].emitters[0].particles[0].age += 0.5f;
        field = "age";
    }
    SECTION("the per-particle curve cursor") {
        other.frames[0].emitters[0].particles[1].keyFrame = 9u;
        field = "keyFrame";
    }
    SECTION("alive count") {
        other.frames[0].emitters[0].particles.pop_back();
        field = "alive count";
    }
    SECTION("vertex count") {
        other.frames[0].emitters[0].vertexCount = 6;
        field = "vertex count";
    }
    SECTION("the vertex stream hash") {
        other.frames[0].emitters[0].vertexHash ^= 1ull;
        field = "vertex stream";
    }
    SECTION("mean colour") {
        other.frames[0].emitters[0].meanColor.z += 0.25f;
        field = "mean colour";
    }
    SECTION("bounds") {
        other.frames[0].emitters[0].boundsMax.x += 1.0f;
        field = "bounds";
    }
    SECTION("emitter identity") {
        other.frames[0].emitters[0].emitterId = 5;
        field = "identity";
    }
    SECTION("emitter count") {
        other.frames[0].emitters.push_back(other.frames[0].emitters[0]);
        field = "emitter count";
    }
    SECTION("frame count") {
        other.frames.push_back(other.frames[0]);
        field = "frame count";
    }

    REQUIRE_FALSE(CompareTraces(base, other, CompareTolerance{}, report));
    INFO(report);
    REQUIRE(Mentions(report, field));
}

TEST_CASE("The output kind is part of an emitter's identity") {
    // PE1 and PE2 emitter ids are both 0-based indices into different chunks,
    // so without the output column they alias.
    const Trace base = MakeTrace();
    Trace other = MakeTrace();
    other.frames[0].emitters[0].output = static_cast<u8>(ParticleOutput::ChildModel);

    std::string report;
    REQUIRE_FALSE(CompareTraces(base, other, CompareTolerance{}, report));
    REQUIRE(Mentions(report, "identity"));
}

TEST_CASE("Zero tolerance means bit-identical") {
    const Trace base = MakeTrace();
    Trace other = MakeTrace();
    // One ULP-ish nudge, far below any sane epsilon — still a failure at zero
    // tolerance, which is what every refactor step but the curve/PE1 ones use.
    other.frames[0].emitters[0].particles[0].position.x += 1e-7f;

    std::string report;
    REQUIRE_FALSE(CompareTraces(base, other, CompareTolerance{}, report));
    REQUIRE(Mentions(report, "position"));
}

TEST_CASE("A tolerance admits float rounding but not real drift") {
    const Trace base = MakeTrace();
    CompareTolerance tol;
    tol.position = 1e-5f;
    tol.requireVertexHash = false; // a tolerance and an exact hash contradict

    std::string report;
    SECTION("inside the tolerance") {
        Trace other = MakeTrace();
        other.frames[0].emitters[0].particles[1].position.z += 1e-4f; // |z| = 16
        REQUIRE(CompareTraces(base, other, tol, report));
    }
    SECTION("outside the tolerance") {
        Trace other = MakeTrace();
        other.frames[0].emitters[0].particles[1].position.z += 1e-2f;
        REQUIRE_FALSE(CompareTraces(base, other, tol, report));
        REQUIRE(Mentions(report, "position"));
    }
    SECTION("tolerance is relative, so it does not scale to zero near the origin") {
        // position.y is 0.5 here: an absolute epsilon would be far tighter than
        // the same tolerance applied out at |z| = 16.
        Trace other = MakeTrace();
        other.frames[0].emitters[0].particles[1].position.y += 1e-4f;
        REQUIRE_FALSE(CompareTraces(base, other, tol, report));
    }
}

TEST_CASE("requireVertexHash off drops the exact vertex-stream check") {
    const Trace base = MakeTrace();
    Trace other = MakeTrace();
    other.frames[0].emitters[0].vertexHash ^= 0xFFFFull;

    CompareTolerance tol;
    tol.requireVertexHash = false;
    std::string report;
    REQUIRE(CompareTraces(base, other, tol, report));

    // The count and the summary still bind — dropping the hash must not turn
    // the whole L2 level off.
    other.frames[0].emitters[0].vertexCount = 6;
    REQUIRE_FALSE(CompareTraces(base, other, tol, report));
}

TEST_CASE("A recorded trace round-trips exactly") {
    // %.9g is what makes a baseline file an equality test rather than an
    // approximate one — a lossy write would silently weaken every check.
    const TempFile file("wf_trace_roundtrip.wpt");
    const Trace original = MakeTrace();

    std::string err;
    REQUIRE(WriteTrace(original, file.Path(), err));

    Trace loaded;
    REQUIRE(ReadTrace(loaded, file.Path(), err));

    std::string report;
    REQUIRE(CompareTraces(original, loaded, CompareTolerance{}, report));
    REQUIRE(loaded.frames.size() == 1u);
    REQUIRE(loaded.frames[0].emitters[0].priorityPlane == 4);
}

TEST_CASE("A v1 baseline without the output column still loads") {
    // Baselines recorded before the output axis existed are exactly what the
    // step that introduced it has to be checked against.
    const TempFile file("wf_trace_v1.wpt");
    file.Write("wpt1\n"
               "f 0 1\n"
               "e 7 2 1 6 12345 4 -1 -1 -1 1 1 1 0.5 0.5 0.5 1\n"
               "p 1 2 3 0.25 -0.5 4 0.125 1\n");

    Trace t;
    std::string err;
    REQUIRE(ReadTrace(t, file.Path(), err));
    REQUIRE(t.frames.size() == 1u);
    REQUIRE(t.frames[0].emitters.size() == 1u);

    const TraceEmitter& e = t.frames[0].emitters[0];
    REQUIRE(e.model == 7u);
    REQUIRE(e.emitterId == 2);
    REQUIRE(e.output == 0u); // defaulted to Billboard, which is all v1 had
    REQUIRE(e.vertexCount == 6);
    REQUIRE(e.particles.size() == 1u);
    REQUIRE(e.particles[0].keyFrame == 1u);
}

TEST_CASE("An unreadable or corrupt baseline is reported, not silently accepted") {
    Trace t;
    std::string err;

    SECTION("missing file") {
        const fs::path missing = fs::temp_directory_path() / "wf_trace_does_not_exist.wpt";
        std::error_code ec;
        fs::remove(missing, ec);
        REQUIRE_FALSE(ReadTrace(t, missing.string(), err));
        REQUIRE_FALSE(err.empty());
    }
    SECTION("wrong magic") {
        const TempFile file("wf_trace_bad_magic.wpt");
        file.Write("not-a-trace\nf 0 0\n");
        REQUIRE_FALSE(ReadTrace(t, file.Path(), err));
        REQUIRE(Mentions(err, "magic"));
    }
    SECTION("a particle record before any emitter") {
        const TempFile file("wf_trace_orphan_particle.wpt");
        file.Write("wpt2\np 1 2 3 0 0 0 0 0\n");
        REQUIRE_FALSE(ReadTrace(t, file.Path(), err));
        REQUIRE_FALSE(err.empty());
    }
}
