// ============================================================================
// D3 cloth — the PH4/PH5 gate.
//
// The failure mode this area shares with the two rigid profiles is a green gate
// over a simulation that never ran: a cape that hangs in its authored shape and
// never moves passes "no NaN, no throw, still on the model" perfectly. So every
// case below asserts a *quantity* — how far something moved, in which
// direction, and by how much it differs from the wrong answer.
//
// It also asserts the two pieces of arithmetic that are invisible in any
// settled pose:
//
//   * **the rational distance correction**, `1 - 2L^2/(L^2 + d^2)`, which
//     agrees with the exact `1 - L/d` to first order and only parts company
//     under stretch — so a cloth built on the exact form looks right at rest
//     and wrong exactly where a cape is interesting;
//   * **the skin blend is a rigid delta**, not a re-skin toward the bind shape,
//     which nothing about a hanging cape can distinguish.
//
// Most of the file needs no corpus: PH4's solver is buildable from plain
// arrays, which is the point of `D3ClothDef` being a struct of vectors.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/d3/d3_model_adapter.h"
#include "io/d3/d3_sno_cache.h"
#include "renderer/animation/anim_math.h"
#include "renderer/profiles/diablo3/d3_cloth.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/model_types.h"

#include <whiteout/sno/d3/native/d3_native.h>
#include <whiteout/sno/d3/native/geometry.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace d3n = ::whiteout::sno::d3::native;
namespace d3c = ::whiteout::flakes::renderer::profiles::diablo3;

using namespace ::whiteout;
using ::whiteout::flakes::renderer::animation::PoseStageContext;
using ::whiteout::flakes::ContentRef;
using ::whiteout::flakes::renderer::model::FrameState;

namespace {

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_D3_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/D3");
}

std::size_t SweepLimit() {
    if (const char* v = std::getenv("WDX_TEST_D3_LIMIT"); v && *v)
        return static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
    return 2000;
}

std::vector<u8> ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return {};
    f.seekg(0, std::ios::end);
    const auto n = static_cast<std::size_t>(f.tellg());
    f.seekg(0);
    std::vector<u8> b(n);
    f.read(reinterpret_cast<char*>(b.data()), static_cast<std::streamsize>(n));
    return b;
}

std::vector<fs::path> FindFiles(const fs::path& dir, const char* ext) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (it->is_regular_file(ec) && it->path().extension() == ext)
            out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

f32 Dist(const Vector3f& a, const Vector3f& b) {
    const f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

bool Finite(const Vector3f& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

/// @brief A `.clt` with everything off but the fields a case cares about.
///
/// Not the registered defaults: a case that wants gravity and nothing else
/// should get gravity and nothing else, so that a failure names one term.
d3c::D3ClothParams Quiet() {
    d3c::D3ClothParams p;
    p.relaxIterations = 0;
    p.mass = 1.0f;
    p.skinBlendRate = 0.0f;
    p.stretchStiffness0 = 0.0f;
    p.stretchStiffness1 = 0.0f;
    p.bendStiffness = 0.0f;
    p.dragCoefficient = 0.0f;
    p.gravity = 0.0f;
    p.rootStiffness = 1.0f;
    p.linearDamping = 0.0f;
    p.contactDamping = 0.0f;
    return p;
}

d3c::D3ClothVertex Particle(const Vector3f& at, f32 invMass) {
    d3c::D3ClothVertex v;
    v.position = at;
    v.prevPosition = at;
    v.invMass = invMass;
    v.normal = {0.0f, 0.0f, 1.0f};
    return v;
}

/// @brief A `w x h` grid laid FLAT in the XY plane, one edge pinned.
///
/// Flat, not already hanging: a sheet built along -Z is already in equilibrium
/// under a -Z gravity with its own rest lengths, so it sags 0.08 units in 25
/// steps and a "did it move" assertion measures nothing. Horizontal, the free
/// edge has to swing through a right angle to get there.
///
/// Pinned first, because that is the ordering the whole solver assumes: every
/// integration loop starts at `firstFreeVertex` and a def that interleaves them
/// simulates the wrong particles without ever failing.
d3c::D3ClothDef Strip(int w, int h, f32 spacing = 1.0f) {
    d3c::D3ClothDef def;
    def.params = Quiet();
    def.firstFreeVertex = w;

    const auto place = [&](int x, int y) {
        return Vector3f{static_cast<f32>(x) * spacing, static_cast<f32>(y) * spacing, 0.0f};
    };
    // Row 0 pinned, then rows 1..h-1 free — so index = (y == 0) ? x : w + (y-1)*w + x.
    for (int x = 0; x < w; ++x)
        def.vertices.push_back(Particle(place(x, 0), 0.0f));
    for (int y = 1; y < h; ++y)
        for (int x = 0; x < w; ++x)
            def.vertices.push_back(Particle(place(x, y), 1.0f));

    const auto index = [&](int x, int y) { return (y == 0) ? x : (w + (y - 1) * w + x); };
    for (int i = 0; i < static_cast<int>(def.vertices.size()); ++i)
        def.vertices[static_cast<std::size_t>(i)].collisionProxy = i;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            def.vertices[static_cast<std::size_t>(index(x, y))].pinDistance = y;

    const auto link = [&](int a, int b) {
        d3c::D3ClothConstraint c;
        c.v0 = a;
        c.v1 = b;
        const f32 d = Dist(def.vertices[static_cast<std::size_t>(a)].position,
                           def.vertices[static_cast<std::size_t>(b)].position);
        c.restLengthSq = d * d;
        const f32 ma = def.vertices[static_cast<std::size_t>(a)].invMass;
        const f32 mb = def.vertices[static_cast<std::size_t>(b)].invMass;
        const f32 sum = ma + mb;
        c.weight0 = (sum != 0.0f) ? ma / sum : 0.0f;
        c.weight1 = (sum != 0.0f) ? mb / sum : 0.0f;
        def.stretch.push_back(c);
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (x + 1 < w)
                link(index(x, y), index(x + 1, y));
            if (y + 1 < h)
                link(index(x, y), index(x, y + 1));
        }

    for (int y = 0; y + 1 < h; ++y)
        for (int x = 0; x + 1 < w; ++x) {
            d3c::D3ClothFace f;
            f.v[0] = index(x, y);
            f.v[1] = index(x + 1, y);
            f.v[2] = index(x, y + 1);
            f.restArea = 0.5f * spacing * spacing;
            def.faces.push_back(f);
            d3c::D3ClothFace g;
            g.v[0] = index(x + 1, y);
            g.v[1] = index(x + 1, y + 1);
            g.v[2] = index(x, y + 1);
            g.restArea = 0.5f * spacing * spacing;
            def.faces.push_back(g);
        }

    def.meshToCloth.resize(def.vertices.size());
    for (std::size_t i = 0; i < def.vertices.size(); ++i) {
        def.vertices[i].meshVertex = static_cast<i32>(i);
        def.meshToCloth[i] = static_cast<i32>(i);
    }
    return def;
}

/// @brief Give a def one driving bone covering every free vertex, with one
///        staple per pinned vertex.
void AddDrivingBone(d3c::D3ClothDef& def, i32 boneNode) {
    d3c::D3ClothDrivingBone db;
    db.contributors = def.firstFreeVertex;
    db.minVertex = def.firstFreeVertex;
    db.maxVertex = static_cast<i32>(def.vertices.size()) - 1;
    db.vertexCount = db.maxVertex + 1 - db.minVertex;
    db.contiguous = true;
    def.drivingBones.push_back(db);
    for (i32 i = 0; i < def.firstFreeVertex; ++i) {
        d3c::D3ClothStaple s;
        s.vertex = i;
        s.bone[0] = boneNode;
        s.weight[0] = 1.0f;
        def.staples.push_back(s);
        def.stapleNormals.push_back({0.0f, 0.0f, 1.0f});
    }
}

Matrix44f Rigid(const Vector3f& t) {
    return Matrix44f::translation(t);
}

/// @brief Corpus `.clt` files indexed by SNO id, so a `snoCloth` reference can
///        be resolved the way the runtime resolves it.
const std::map<i32, d3n::Cloth>& ClothById() {
    static const std::map<i32, d3n::Cloth> table = [] {
        std::map<i32, d3n::Cloth> m;
        for (const auto& p : FindFiles(CorpusRoot() / "Cloth", ".clt")) {
            const auto bytes = ReadAll(p);
            if (auto c = d3n::parseCloth(bytes))
                m.emplace(c->dwSnoId, std::move(*c));
        }
        return m;
    }();
    return table;
}

/// @brief The same files as @ref ClothById, unparsed, for a stub provider to
///        serve: the adapter resolves a `.clt` through D3SnoCache, not from a
///        parsed table a test happens to hold.
const std::map<i32, std::vector<u8>>& ClothBytesById() {
    static const std::map<i32, std::vector<u8>> table = [] {
        std::map<i32, std::vector<u8>> m;
        for (const auto& p : FindFiles(CorpusRoot() / "Cloth", ".clt")) {
            auto bytes = ReadAll(p);
            if (auto c = d3n::parseCloth(bytes))
                m.emplace(c->dwSnoId, std::move(bytes));
        }
        return m;
    }();
    return table;
}

/// @brief Serves a fixed id -> bytes map. The same stub `d3_cache_test` uses,
///        because the cache only ever reaches a provider through ReadFile.
class CorpusProvider final : public ::whiteout::flakes::io::IContentProvider {
public:
    std::map<u32, std::vector<u8>> files;

    using RequestId = ::whiteout::flakes::io::RequestId;
    using CompletionCallback = ::whiteout::flakes::io::CompletionCallback;

    RequestId Request(const ContentRef& ref, CompletionCallback cb) override {
        ::whiteout::flakes::io::RequestResult r;
        if (ref.IsFileId()) {
            if (auto it = files.find(ref.fileId); it != files.end()) {
                r.ok = true;
                r.data = it->second;
            }
        }
        if (cb)
            cb(std::move(r));
        return 1;
    }
    void Wait(RequestId) override {}
    void Cancel(RequestId) override {}
    void Pump() override {}
};

struct CapedModel {
    std::string name;
    std::shared_ptr<const d3n::Appearances> app;
    std::vector<d3c::D3ClothPiece> pieces;
    std::vector<std::shared_ptr<const d3n::Cloth>> cloths;
};

/// @brief The first `.app` in name order that carries cloth with a `.clt` the
///        corpus can resolve, and at least one staple.
///
/// Found rather than named so the gate keeps working when the corpus is
/// re-extracted; the name it settled on is printed, so a change of subject is
/// visible in the log rather than silent.
const CapedModel& FirstCaped() {
    static const CapedModel found = [] {
        CapedModel out;
        const auto& clt = ClothById();
        if (clt.empty())
            return out;
        std::size_t swept = 0;
        for (const auto& p : FindFiles(CorpusRoot() / "Appearances", ".app")) {
            if (++swept > SweepLimit())
                break;
            const auto bytes = ReadAll(p);
            auto parsed = d3n::parseAppearances(bytes);
            if (!parsed)
                continue;
            auto app = std::make_shared<d3n::Appearances>(std::move(*parsed));
            auto pieces = d3c::D3FindClothPieces(*app, 0);
            std::vector<d3c::D3ClothPiece> keep;
            std::vector<std::shared_ptr<const d3n::Cloth>> cloths;
            for (const auto& piece : pieces) {
                auto it = clt.find(piece.clothSno);
                if (it == clt.end() || it->second.flMass < 1.0e-6f)
                    continue;
                const auto& sub = app->tGeoSet0.arSubObjects[static_cast<std::size_t>(piece.subObject)];
                if (sub.arClothData.front().arStaples.empty())
                    continue;
                keep.push_back(piece);
                cloths.push_back(std::make_shared<const d3n::Cloth>(it->second));
            }
            if (keep.empty())
                continue;
            out.name = p.filename().string();
            out.app = std::move(app);
            out.pieces = std::move(keep);
            out.cloths = std::move(cloths);
            break;
        }
        return out;
    }();
    return found;
}

/// @brief The first `.app` in name order carrying a `CollisionCapsule` whose
///        hardpoint bone sits somewhere other than the hardpoint itself.
///
/// The offset is the point: a hardpoint composed with its bone's *pose* instead
/// of its bone's *attachment frame* lands at roughly the sum of the two, so a
/// subject whose bones happen to sit at the origin would pass either way.
struct CapsuledModel {
    std::string name;
    std::shared_ptr<const d3n::Appearances> app;
    f32 worstBoneOffset = 0.0f;
};

const CapsuledModel& FirstCapsuled() {
    static const CapsuledModel found = [] {
        CapsuledModel out;
        std::size_t swept = 0;
        for (const auto& p : FindFiles(CorpusRoot() / "Appearances", ".app")) {
            if (++swept > SweepLimit())
                break;
            const auto bytes = ReadAll(p);
            auto parsed = d3n::parseAppearances(bytes);
            if (!parsed || parsed->arCollisionCapsules.empty())
                continue;
            f32 worst = 0.0f;
            for (const auto& c : parsed->arCollisionCapsules) {
                const i32 b = c.tHardpoint.nBoneIndex;
                if (b < 0 || static_cast<std::size_t>(b) >= parsed->arBones.size())
                    continue;
                worst = (std::max)(worst, Dist(parsed->arBones[static_cast<std::size_t>(b)]
                                                   .tTransform0.vTranslation,
                                               c.tHardpoint.tTransform.vTranslation));
            }
            if (worst < 0.5f)
                continue;
            out.name = p.filename().string();
            out.worstBoneOffset = worst;
            out.app = std::make_shared<d3n::Appearances>(std::move(*parsed));
            break;
        }
        return out;
    }();
    return found;
}

/// @brief The bind pose as `boneWorldMatrices`, from `tTransform0` — the same
///        model-space pose A the rigid gate uses.
FrameState BindPose(const d3n::Appearances& app) {
    FrameState fs;
    fs.boneWorldMatrices.reserve(app.arBones.size());
    for (const auto& b : app.arBones) {
        const auto& t = b.tTransform0;
        const Quaternion q{t.qRotation.x, t.qRotation.y, t.qRotation.z, t.qRotation.w};
        const f32 s = t.flScale != 0.0f ? t.flScale : 1.0f;
        fs.boneWorldMatrices.push_back(::whiteout::flakes::renderer::animation::ComposePivotSRT(
            t.vTranslation, q, {s, s, s}, {0.0f, 0.0f, 0.0f}));
    }
    return fs;
}

} // namespace

// ---------------------------------------------------------------------------
// PH4 — the solver in isolation
// ---------------------------------------------------------------------------

TEST_CASE("D3 cloth: gravity is flGravity*3600 - 43.2, and nothing else",
          "[d3][cloth][physics]") {
    d3c::D3ClothDef def;
    def.params = Quiet();
    // -0.005555 authored -> -20 u/s^2 scaled, and the client then subtracts a
    // bare 43.2 on top. Neither half is derivable from the other, and the rigid
    // world's -32.2 is a third unrelated number.
    def.params.gravity = -20.0f;
    def.vertices.push_back(Particle({0.0f, 0.0f, 0.0f}, 1.0f));
    def.firstFreeVertex = 0;
    def.meshToCloth = {0};

    d3c::D3ClothSolver solver(std::move(def));
    const f32 dt = 1.0f / 60.0f;
    solver.Step(dt);

    // One explicit Euler step: v = a*dt, p = v*dt.
    const f32 expected = ((-20.0f) + (-43.2f)) * dt * dt;
    const Vector3f p = solver.Vertices()[0].position;
    CHECK(std::fabs(p.z - expected) < 1.0e-6f);
    CHECK(std::fabs(p.x) < 1.0e-9f);
    CHECK(std::fabs(p.y) < 1.0e-9f);

    // The bias alone is a quarter of a unit over a second. A port that dropped
    // it would still fall, which is why the value is checked and not the sign.
    CHECK(std::fabs(expected) > std::fabs((-20.0f) * dt * dt));
}

TEST_CASE("D3 cloth: the distance correction is the rational form, not 1 - L/d",
          "[d3][cloth][physics]") {
    // Two free particles at 4x their rest separation, one full-stiffness
    // constraint, one step at dt = 1/60 so the `* dt * 60` factor is exactly 1.
    // Four, not two: both forms converge to the rest length after the step's
    // two stretch passes, and at 2x they land 0.024 apart — close enough that
    // the case would be testing its own tolerance. Under real stretch the
    // rational form **overshoots and rebounds**, and the gap is ten times that.
    const f32 rest = 1.0f;
    d3c::D3ClothDef def;
    def.params = Quiet();
    def.params.stretchStiffness0 = 1.0f;
    def.params.stretchStiffness1 = 1.0f;
    def.params.gravity = 43.2f; // cancel the unconditional bias; see below
    def.vertices.push_back(Particle({0.0f, 0.0f, 0.0f}, 1.0f));
    def.vertices.push_back(Particle({4.0f, 0.0f, 0.0f}, 1.0f));
    def.firstFreeVertex = 0;
    def.meshToCloth = {0, 1};
    d3c::D3ClothConstraint c;
    c.v0 = 0;
    c.v1 = 1;
    c.restLengthSq = rest * rest;
    c.weight0 = 0.5f;
    c.weight1 = 0.5f;
    c.stiffnessBlend = 0.0f;
    def.stretch.push_back(c);

    d3c::D3ClothSolver solver(std::move(def));
    solver.Step(1.0f / 60.0f);

    // The step runs the stretch set TWICE, either side of a bend set that is
    // empty here — so the closed form is the correction applied to the result
    // of the correction.
    const auto apply = [&](f32 d) {
        const f32 term = (rest * rest * -2.0f) / (rest * rest + d * d) + 1.0f;
        // Both endpoints move by half the correction, so the gap changes by the
        // whole of it.
        return d - d * term;
    };
    const f32 after = apply(apply(4.0f));
    const f32 got = solver.Vertices()[1].position.x - solver.Vertices()[0].position.x;
    CHECK(std::fabs(got - after) < 1.0e-5f);

    // And it is measurably *not* the exact constraint, which at 2x stretch
    // would pull the pair all the way to the rest length in one pass.
    const auto exact = [&](f32 d) { return d - d * (1.0f - rest / d); };
    const f32 exactAfter = exact(exact(4.0f));
    CHECK(std::fabs(got - exactAfter) > 0.15f);
}

TEST_CASE("D3 cloth: the bend set resists compression only", "[d3][cloth][physics]") {
    const auto run = [](f32 separation) {
        d3c::D3ClothDef def;
        def.params = Quiet();
        def.params.bendStiffness = 1.0f;
        def.vertices.push_back(Particle({0.0f, 0.0f, 0.0f}, 1.0f));
        def.vertices.push_back(Particle({separation, 0.0f, 0.0f}, 1.0f));
        def.firstFreeVertex = 0;
        def.meshToCloth = {0, 1};
        d3c::D3ClothConstraint c;
        c.v0 = 0;
        c.v1 = 1;
        c.restLengthSq = 1.0f;
        c.weight0 = 0.5f;
        c.weight1 = 0.5f;
        def.bend.push_back(c);
        d3c::D3ClothSolver solver(std::move(def));
        solver.Step(1.0f / 60.0f);
        return solver.Vertices()[1].position.x - solver.Vertices()[0].position.x;
    };

    // Compressed to half the rest length: the term is negative and the pair is
    // pushed apart.
    CHECK(run(0.5f) > 0.5f + 1.0e-4f);
    // Stretched to twice it: the term is positive and nothing happens at all.
    CHECK(std::fabs(run(2.0f) - 2.0f) < 1.0e-6f);
}

TEST_CASE("D3 cloth: a pinned strip hangs, and the pins do not move",
          "[d3][cloth][physics]") {
    d3c::D3ClothDef def = Strip(5, 6);
    def.params.gravity = -20.0f;
    def.params.stretchStiffness0 = 1.0f;
    def.params.stretchStiffness1 = 1.0f;
    def.params.relaxIterations = 25;

    const std::vector<Vector3f> before = [&] {
        std::vector<Vector3f> v;
        for (const auto& p : def.vertices)
            v.push_back(p.position);
        return v;
    }();
    const i32 pinned = def.firstFreeVertex;

    d3c::D3ClothSolver solver(std::move(def));
    solver.Presettle();

    const auto verts = solver.Vertices();
    for (i32 i = 0; i < pinned; ++i) {
        INFO("pinned vertex " << i);
        CHECK(Dist(verts[static_cast<std::size_t>(i)].position, before[static_cast<std::size_t>(i)]) < 1.0e-6f);
    }

    f32 worst = 0.0f;
    for (std::size_t i = static_cast<std::size_t>(pinned); i < verts.size(); ++i) {
        REQUIRE(Finite(verts[i].position));
        worst = (std::max)(worst, Dist(verts[i].position, before[i]));
        // Everything free falls; nothing rises.
        CHECK(verts[i].position.z <= before[i].z + 1.0e-4f);
    }
    // 25 relaxation steps at 1/60 is 0.42 s of fall before any constraint acts,
    // so a strip that moved less than a spacing did not simulate.
    CHECK(worst > 1.0f);

    // And the sheet did not tear: no stretch constraint past 1.6x its rest.
    for (const auto& c : solver.Def().stretch) {
        const f32 d = Dist(verts[static_cast<std::size_t>(c.v0)].position,
                           verts[static_cast<std::size_t>(c.v1)].position);
        INFO("constraint " << c.v0 << " -> " << c.v1);
        CHECK(d * d < c.restLengthSq * 2.56f);
    }
}

TEST_CASE("D3 cloth: the snap latch holds for the whole presettle, not one step",
          "[d3][cloth][physics]") {
    // `ClothSim_BlendToSkinnedPose` only READS solver+60; the sole place it is
    // written is the tail of `ClothInstance_UpdateDrivingBones`. So the request
    // `ClothInstance_Presettle` raises survives every one of its
    // `relaxIterations` steps, each of which snaps and zeroes the velocity it
    // just integrated. The result is a relaxation: a free vertex creeps down by
    // `a*dt^2` per step, linearly.
    //
    // Consume the latch after one step instead and the remaining steps
    // integrate normally, so the same vertex falls `a*dt^2*N(N+1)/2` -- 13x
    // further at the shipped 25 iterations, which is a cape that settles
    // hanging off its own body. The two are told apart by the *shape* of the
    // fall, which is why this measures both counts and not just one.
    const f32 dt = 1.0f / 60.0f;
    const f32 accel = -43.2f; // gravity 0, so the unconditional bias alone
    const auto fall = [&](int steps) {
        d3c::D3ClothDef def;
        def.params = Quiet();
        def.params.skinBlendRate = 0.0f; // only the latch can force blend = 1
        def.vertices.push_back(Particle({0.0f, 0.0f, 0.0f}, 0.0f)); // pinned
        def.vertices.push_back(Particle({0.0f, 0.0f, 0.0f}, 1.0f)); // free
        def.firstFreeVertex = 1;
        def.meshToCloth = {0, 1};
        AddDrivingBone(def, 0);
        d3c::D3ClothSolver solver(std::move(def));
        std::vector<Matrix44f> skin{Matrix44f::identity()};
        solver.UpdateDrivingBones(skin); // latches the constructor's request
        for (int i = 0; i < steps; ++i)
            solver.Step(dt);
        return solver.Vertices()[1].position.z;
    };

    for (int n : {5, 10, 25}) {
        INFO("steps = " << n);
        const f32 linear = accel * dt * dt * static_cast<f32>(n);
        CHECK(std::fabs(fall(n) - linear) < 1.0e-4f);
    }
    // ...and it really is the flat one: at 25 steps the quadratic answer is a
    // different number by an order of magnitude, so the case cannot pass by
    // accident on a constant.
    CHECK(std::fabs(accel * dt * dt * (25.0f * 26.0f * 0.5f) - fall(25)) > 3.0f);
}

TEST_CASE("D3 cloth: presettle is exactly relaxIterations steps at 1/60",
          "[d3][cloth][physics]") {
    d3c::D3ClothDef def = Strip(4, 4);
    def.params.gravity = -20.0f;
    def.params.stretchStiffness0 = 0.5f;
    def.params.stretchStiffness1 = 0.5f;
    def.params.relaxIterations = 7;

    d3c::D3ClothSolver a(def);
    a.Presettle();
    d3c::D3ClothSolver b(std::move(def));
    for (int i = 0; i < 7; ++i)
        b.Step(1.0f / 60.0f);

    for (std::size_t i = 0; i < a.Vertices().size(); ++i)
        CHECK(Dist(a.Vertices()[i].position, b.Vertices()[i].position) < 1.0e-6f);
}

TEST_CASE("D3 cloth: the plane pass keeps a 0.01 skin and damps only the tangent",
          "[d3][cloth][physics]") {
    d3c::D3ClothDef def;
    def.params = Quiet();
    def.params.gravity = -20.0f;
    // exp(-x/60) at x = 60 is 0.368 — a tangential velocity cut to a third,
    // which is the whole of D3's friction model.
    def.params.contactDamping = 60.0f;
    def.vertices.push_back(Particle({0.0f, 0.0f, 0.5f}, 1.0f));
    def.firstFreeVertex = 0;
    def.meshToCloth = {0};
    d3c::D3ClothSolver solver(std::move(def));

    d3c::D3ClothPlane ground;
    ground.normal = {0.0f, 0.0f, 1.0f};
    ground.d = 0.0f;
    solver.SetPlanes({ground});

    // Give it a sideways velocity to damp, and enough steps to land.
    for (int i = 0; i < 120; ++i)
        solver.Step(1.0f / 60.0f);

    const auto& v = solver.Vertices()[0];
    CHECK(v.position.z >= 0.01f - 1.0e-5f);
    CHECK(v.position.z < 0.02f);
    CHECK(v.contactFlag == 1);
    // Resting on the plane, the vertical velocity is whatever one step of
    // gravity put in and the position pass took straight back out — so it is
    // the *tangential* half being scaled that has to be visible, and with no
    // sideways force there is nothing tangential left.
    CHECK(std::fabs(v.velocity.x) < 1.0e-4f);
    CHECK(std::fabs(v.velocity.y) < 1.0e-4f);
}

TEST_CASE("D3 cloth: a capsule pushes the vertex out, and probes with its proxy",
          "[d3][cloth][physics]") {
    d3c::D3ClothCapsule cap;
    cap.p0 = {-1.0f, 0.0f, 0.0f};
    cap.p1 = {1.0f, 0.0f, 0.0f};
    cap.radius = 1.0f;

    // Vertex 0 sits inside the capsule; vertex 1 is its proxy, off to +Y.
    const auto run = [&](i32 proxy) {
        d3c::D3ClothDef def;
        def.params = Quiet();
        // **The -43.2 bias applies whatever `flGravity` says**, so "no gravity"
        // is `gravity = 43.2`, not zero. Left at zero the vertex falls 0.012
        // units inside the step and tilts the contact normal seven degrees —
        // which is how this case first read as a broken capsule pass.
        def.params.gravity = 43.2f;
        def.vertices.push_back(Particle({0.0f, 0.1f, 0.0f}, 1.0f));
        def.vertices.push_back(Particle({0.0f, 0.5f, 0.0f}, 1.0f));
        def.vertices[0].collisionProxy = proxy;
        def.vertices[1].collisionProxy = 1;
        def.firstFreeVertex = 0;
        def.meshToCloth = {0, 1};
        d3c::D3ClothSolver solver(std::move(def));
        solver.SetCapsules({cap});
        solver.Step(1.0f / 60.0f);
        return solver.Vertices()[0].position;
    };

    const Vector3f own = run(0);
    // Probing with itself, the normal is +Y and the vertex ends on the surface.
    CHECK(own.y > 0.99f);
    CHECK(std::fabs(own.x) < 1.0e-4f);
    CHECK(std::fabs(own.z) < 1.0e-4f);

    // Probing with the proxy the normal is still +Y here — the interesting part
    // is that the *depth* is measured from the vertex, so it lands on the
    // surface either way but by a different amount.
    const Vector3f viaProxy = run(1);
    CHECK(viaProxy.y > 0.99f);
    CHECK(Dist(own, viaProxy) < 1.0e-4f);

    // A proxy pointing somewhere else moves the contact normal with it, which
    // is the whole reason the field exists.
    const auto sideways = [&] {
        d3c::D3ClothDef def;
        def.params = Quiet();
        def.params.gravity = 43.2f;
        def.vertices.push_back(Particle({0.0f, 0.1f, 0.0f}, 1.0f));
        def.vertices.push_back(Particle({0.0f, 0.0f, 0.5f}, 1.0f));
        def.vertices[0].collisionProxy = 1;
        def.vertices[1].collisionProxy = 1;
        def.firstFreeVertex = 0;
        def.meshToCloth = {0, 1};
        d3c::D3ClothSolver solver(std::move(def));
        solver.SetCapsules({cap});
        solver.Step(1.0f / 60.0f);
        return solver.Vertices()[0].position;
    }();
    // Pushed along +Z now, not +Y.
    CHECK(sideways.z > 0.5f);
    CHECK(std::fabs(sideways.y - 0.1f) < 1.0e-4f);
}

// ---------------------------------------------------------------------------
// PH5 — anchoring
// ---------------------------------------------------------------------------

TEST_CASE("D3 cloth: staples follow their bone and the pinned normal survives",
          "[d3][cloth][physics]") {
    d3c::D3ClothDef def = Strip(4, 4);
    AddDrivingBone(def, 0);
    d3c::D3ClothSolver solver(std::move(def));

    std::vector<Matrix44f> skin{Rigid({3.0f, 0.0f, 0.0f})};
    solver.UpdateDrivingBones(skin);

    // Every pinned vertex is now its rest position through the bone, exactly.
    for (i32 i = 0; i < solver.Def().firstFreeVertex; ++i) {
        const auto& v = solver.Vertices()[static_cast<std::size_t>(i)];
        CHECK(std::fabs(v.position.x - (v.prevPosition.x + 3.0f)) < 1.0e-5f);
        CHECK(std::fabs(v.position.y - v.prevPosition.y) < 1.0e-5f);
        CHECK(std::fabs(v.position.z - v.prevPosition.z) < 1.0e-5f);
        // Rotated, not accumulated from faces: a pinned vertex is never in a
        // face's normal sum, so a solver that let it be would leave the cape's
        // top row lit as cloth rather than as the model it is stapled to.
        CHECK(std::fabs(v.normal.z - 1.0f) < 1.0e-5f);
    }
}

TEST_CASE("D3 cloth: the skin blend is a rigid delta, not a snap to the bind pose",
          "[d3][cloth][physics]") {
    d3c::D3ClothDef def = Strip(4, 4);
    def.params.skinBlendRate = 1.0f;
    def.params.gravity = -20.0f;
    AddDrivingBone(def, 0);

    d3c::D3ClothSolver solver(std::move(def));
    std::vector<Matrix44f> skin{Matrix44f::identity()};
    solver.UpdateDrivingBones(skin);
    // First step consumes the one-shot snap the solver starts armed with, so
    // the delta measured below is a real frame-to-frame one.
    solver.Step(1.0f / 60.0f);
    for (int i = 0; i < 20; ++i) {
        solver.UpdateDrivingBones(skin);
        solver.Step(1.0f / 60.0f);
    }

    std::vector<Vector3f> hung;
    for (const auto& v : solver.Vertices())
        hung.push_back(v.position);

    // Now translate the bone. At blend 1 every free vertex should move by
    // exactly the same offset — the cloth is carried, not re-skinned.
    skin[0] = Rigid({5.0f, 0.0f, 0.0f});
    solver.UpdateDrivingBones(skin);
    solver.Step(1.0f / 60.0f);

    const auto verts = solver.Vertices();
    for (std::size_t i = static_cast<std::size_t>(solver.Def().firstFreeVertex); i < verts.size(); ++i) {
        INFO("free vertex " << i);
        // The step also integrates one frame of gravity and runs the
        // constraints, so the X offset is the part that has to be exact.
        CHECK(std::fabs((verts[i].position.x - hung[i].x) - 5.0f) < 1.0e-3f);
        // A re-skin toward the bind shape would have flattened the hang; the
        // sag has to survive the carry.
        CHECK(verts[i].position.z < -0.05f);
    }

    // Hold the bone still for a frame. A **delta** carries nothing further; the
    // absolute frame — the mistake this is here to catch — would move the cloth
    // another five units every frame the bone stays put, and the two are
    // indistinguishable on the step that does the moving.
    std::vector<Vector3f> carried;
    for (const auto& v : solver.Vertices())
        carried.push_back(v.position);
    solver.UpdateDrivingBones(skin);
    solver.Step(1.0f / 60.0f);
    for (std::size_t i = static_cast<std::size_t>(solver.Def().firstFreeVertex);
         i < solver.Vertices().size(); ++i) {
        INFO("free vertex " << i << " after a still frame");
        CHECK(std::fabs(solver.Vertices()[i].position.x - carried[i].x) < 1.0e-3f);
    }
}

TEST_CASE("D3 cloth: a big driving-bone jump forces a full snap", "[d3][cloth][physics]") {
    d3c::D3ClothDef def = Strip(4, 4);
    // Blend 0 — free simulation. The only thing that can move the cloth with
    // the bone is the >1.0 squared-displacement override.
    def.params.skinBlendRate = 0.0f;
    def.params.gravity = -20.0f;
    AddDrivingBone(def, 0);

    d3c::D3ClothSolver solver(std::move(def));
    std::vector<Matrix44f> skin{Matrix44f::identity()};
    solver.UpdateDrivingBones(skin);
    solver.Step(1.0f / 60.0f);
    for (int i = 0; i < 10; ++i) {
        solver.UpdateDrivingBones(skin);
        solver.Step(1.0f / 60.0f);
    }
    const f32 beforeX = solver.Vertices()[static_cast<std::size_t>(solver.Def().firstFreeVertex)].position.x;

    // A quarter-unit hop is under the threshold: the cloth stays put.
    skin[0] = Rigid({0.25f, 0.0f, 0.0f});
    solver.UpdateDrivingBones(skin);
    solver.Step(1.0f / 60.0f);
    const f32 small =
        solver.Vertices()[static_cast<std::size_t>(solver.Def().firstFreeVertex)].position.x - beforeX;
    CHECK(std::fabs(small) < 0.05f);

    // Ten units is not: the whole sheet is carried and its velocities zeroed.
    skin[0] = Rigid({10.25f, 0.0f, 0.0f});
    solver.UpdateDrivingBones(skin);
    solver.Step(1.0f / 60.0f);
    const auto& v = solver.Vertices()[static_cast<std::size_t>(solver.Def().firstFreeVertex)];
    CHECK(v.position.x - beforeX > 9.0f);
}

// ---------------------------------------------------------------------------
// Corpus
// ---------------------------------------------------------------------------

TEST_CASE("D3 cloth: a capsule rides its bone's attachment frame, not its pose",
          "[d3][cloth][physics][corpus]") {
    const CapsuledModel& m = FirstCapsuled();
    if (!m.app) {
        WARN("no .app with offset collision capsules under " << CorpusRoot().string()
                                                             << " -- gate skipped");
        return;
    }
    std::printf("[d3-cloth] capsule subject: %s (%zu capsule(s), worst bone offset %.2f)\n",
                m.name.c_str(), m.app->arCollisionCapsules.size(),
                static_cast<double>(m.worstBoneOffset));

    // A hardpoint's transform is authored in MODEL space, and the palette entry
    // it rides (`worldPose+32`) is the animated pose composed with the bone's
    // `tTransform1` — the inverse of bind pose A. In the bind pose those cancel
    // exactly, so every capsule must come back centred on the translation the
    // file states, with no trace of where its bone happens to be.
    //
    // Composing with the bone pose instead adds the two, which is a collider
    // at roughly twice its height. It still collides, still looks like a
    // capsule, and is wrong for every bone that is not at the origin.
    const FrameState fs = BindPose(*m.app);
    const auto caps =
        d3c::D3GatherClothCapsules(*m.app, fs.boneWorldMatrices, {0.0f, 0.0f, 0.0f}, 1.0e9f);
    REQUIRE(!caps.empty());

    std::size_t checked = 0;
    for (const auto& c : m.app->arCollisionCapsules) {
        const i32 b = c.tHardpoint.nBoneIndex;
        if (b < 0 || static_cast<std::size_t>(b) >= m.app->arBones.size())
            continue;
        REQUIRE(checked < caps.size());
        const auto& got = caps[checked++];
        const Vector3f mid{(got.p0.x + got.p1.x) * 0.5f, (got.p0.y + got.p1.y) * 0.5f,
                           (got.p0.z + got.p1.z) * 0.5f};
        CHECK(Dist(mid, c.tHardpoint.tTransform.vTranslation) < 1.0e-3f);
        CHECK(std::fabs(Dist(got.p0, got.p1) - c.flLength) < 1.0e-3f);
        CHECK(std::fabs(got.radius - c.flRadius) < 1.0e-4f);
    }
    CHECK(checked == caps.size());
    // The subject is only evidence if its bones really are somewhere else.
    CHECK(m.worstBoneOffset > 0.5f);
}

TEST_CASE("D3 cloth: a shipped cape builds, presettles and hangs",
          "[d3][cloth][physics][corpus]") {
    const CapedModel& m = FirstCaped();
    if (!m.app) {
        WARN("no caped .app found under " << CorpusRoot().string() << " -- gate skipped");
        return;
    }
    std::printf("[d3-cloth] subject: %s (%zu cloth piece(s))\n", m.name.c_str(), m.pieces.size());

    const d3n::SubObject& sub =
        m.app->tGeoSet0.arSubObjects[static_cast<std::size_t>(m.pieces[0].subObject)];
    auto def = d3c::D3BuildCloth(sub, m.cloths[0].get(), 1.0f);
    REQUIRE(def.has_value());
    REQUIRE(def->firstFreeVertex > 0);
    REQUIRE(static_cast<std::size_t>(def->firstFreeVertex) < def->vertices.size());

    // Pinned-first, to the unit: every vertex below the staple count has zero
    // inverse mass and every one above it does not. This is the ordering every
    // loop in the solver assumes and nothing in the file states.
    for (i32 i = 0; i < def->firstFreeVertex; ++i)
        CHECK(def->vertices[static_cast<std::size_t>(i)].invMass == 0.0f);
    std::size_t freeWithMass = 0;
    for (std::size_t i = static_cast<std::size_t>(def->firstFreeVertex); i < def->vertices.size(); ++i)
        if (def->vertices[i].invMass > 0.0f)
            ++freeWithMass;
    CHECK(freeWithMass == def->vertices.size() - static_cast<std::size_t>(def->firstFreeVertex));

    auto output = std::make_shared<d3c::D3ClothOutput>();
    // Only the adapter knows the emitted-geoset mapping, so `D3FindClothPieces`
    // leaves `geoset` at -1 and the stage publishes no deform for it — correct,
    // and not what this case is testing. Filled in here the way the adapter
    // fills it, so the renderer's half is exercised rather than skipped.
    std::vector<d3c::D3ClothPiece> pieces = m.pieces;
    for (auto& piece : pieces)
        piece.geoset = piece.subObject;
    auto stage = d3c::CreateD3ClothStage(*m.app, pieces, m.cloths, output);
    REQUIRE(stage != nullptr);

    FrameState fs = BindPose(*m.app);
    const std::vector<Vector3f> rest = [&] {
        std::vector<Vector3f> v;
        for (const auto& fv : sub.arVertices)
            v.push_back(fv.vPosition);
        return v;
    }();

    PoseStageContext ctx;
    ctx.frameDtMs = 16;
    stage->Run(fs, ctx);
    REQUIRE(output->revision == 1);

    // The renderer's half of the deform: one `FrameState` entry per piece,
    // naming the emitted geoset and covering every one of its vertices. The
    // pipeline patches position and normal into the retained upload bytes off
    // exactly this, so an entry that is short or names -1 is a geoset drawn
    // half in its rest pose — which looks like a modelling error, not a bug.
    REQUIRE(fs.geosetDeforms.size() == output->pieces.size());
    for (std::size_t i = 0; i < fs.geosetDeforms.size(); ++i) {
        const auto& gd = fs.geosetDeforms[i];
        CHECK(gd.geoset == output->pieces[i].geoset);
        CHECK(gd.positions.size() == output->pieces[i].positions.size());
        CHECK(gd.normals.size() == output->pieces[i].normals.size());
        CHECK(gd.positions.data() == output->pieces[i].positions.data());
    }
    REQUIRE(!output->pieces.empty());
    REQUIRE(output->pieces[0].positions.size() == sub.arVertices.size());

    f32 worst = 0.0f;
    for (std::size_t i = 0; i < rest.size(); ++i) {
        REQUIRE(Finite(output->pieces[0].positions[i]));
        REQUIRE(Finite(output->pieces[0].normals[i]));
        worst = (std::max)(worst, Dist(output->pieces[0].positions[i], rest[i]));
    }
    // The presettle is 25 full steps of a cape falling under -63.2 u/s^2 with
    // its constraints resisting. A tenth of a unit is far below what that has
    // to produce and far above float noise.
    CHECK(worst > 0.1f);

    // ...and it did not fly apart. The whole cloth stays inside the model's own
    // bounds inflated by its own diagonal, which a diverging solver leaves in
    // one step.
    const auto& b = m.app->tBounds;
    const f32 diag = std::sqrt(b.vHalfExtent.x * b.vHalfExtent.x +
                               b.vHalfExtent.y * b.vHalfExtent.y +
                               b.vHalfExtent.z * b.vHalfExtent.z);
    for (const auto& p : output->pieces[0].positions) {
        INFO("cloth vertex escaped the model bounds");
        CHECK(Dist(p, {b.vCenter.x, b.vCenter.y, b.vCenter.z}) < diag * 3.0f + 1.0f);
    }

    // Steady state: fifty more frames and it is still finite and still bounded.
    for (int i = 0; i < 50; ++i)
        stage->Run(fs, ctx);
    for (const auto& p : output->pieces[0].positions)
        REQUIRE(Finite(p));
    CHECK(output->revision == 51);
}

TEST_CASE("D3 cloth: two actors on one drawable get two deform buffers",
          "[d3][cloth][physics][corpus]") {
    // `ModelLoader::D3Drawable` files one adapter per (appearance, look) and
    // hands it to every actor that asks for it -- 594 shipped actors name one
    // appearance, so this is the normal path, not a corner. What a cloth stage
    // publishes, though, is per actor: `FrameState::geosetDeforms` carries
    // *spans* into the buffer, drained a whole frame later, so an adapter that
    // memoises one buffer makes every actor but the last to step draw somebody
    // else's cape -- in somebody else's pose. Two actors is one Storage
    // Explorer preview of the model already in the viewer.
    const CapedModel& m = FirstCaped();
    if (!m.app) {
        WARN("no caped .app found under " << CorpusRoot().string() << " -- gate skipped");
        return;
    }
    const fs::path appPath = CorpusRoot() / "Appearances" / m.name;
    const auto appBytes = ReadAll(appPath);
    REQUIRE(!appBytes.empty());

    // The adapter reaches its `.clt` (and the `.app` bytes the rig needs)
    // through the cache, so the stage only builds if the provider serves both.
    CorpusProvider provider;
    provider.files[static_cast<u32>(m.app->dwSnoId)] = appBytes;
    for (const auto& [sno, bytes] : ClothBytesById())
        provider.files[static_cast<u32>(sno)] = bytes;
    ::whiteout::flakes::io::D3SnoCache cache(&provider);
    auto adapter = ::whiteout::flakes::io::D3ModelAdapter::LoadAppearance(
        ContentRef::FromFileId(static_cast<u32>(m.app->dwSnoId)), appBytes, cache);
    REQUIRE(adapter != nullptr);

    ::whiteout::flakes::renderer::animation::PoseStageList first, second;
    adapter->CreatePoseStages(first);
    adapter->CreatePoseStages(second);
    REQUIRE(!first.empty());
    REQUIRE(!second.empty());

    PoseStageContext ctx;
    ctx.frameDtMs = 16;

    // Actor one settles at the bind pose.
    FrameState fsA = BindPose(*m.app);
    for (auto& stage : first)
        stage->Run(fsA, ctx);
    REQUIRE(!fsA.geosetDeforms.empty());
    const std::vector<Vector3f> mine(fsA.geosetDeforms[0].positions.begin(),
                                     fsA.geosetDeforms[0].positions.end());

    // Actor two is the same model somewhere else entirely -- a thumbnail cell
    // has its own camera and its own pose, and 200 units is only far enough to
    // make "whose cloth is this" answerable.
    FrameState fsB = BindPose(*m.app);
    for (auto& mat : fsB.boneWorldMatrices)
        mat.data[3][2] += 200.0f;
    for (int i = 0; i < 4; ++i)
        for (auto& stage : second)
            stage->Run(fsB, ctx);
    REQUIRE(!fsB.geosetDeforms.empty());

    // Actor one's spans, re-read after actor two stepped. Same count, same
    // values: the two never touched each other's storage.
    REQUIRE(fsA.geosetDeforms[0].positions.size() == mine.size());
    f32 drift = 0.0f;
    for (std::size_t i = 0; i < mine.size(); ++i)
        drift = (std::max)(drift, Dist(fsA.geosetDeforms[0].positions[i], mine[i]));
    std::printf("[d3-cloth] shared-drawable drift: %.6f u over %zu vertices\n", drift,
                mine.size());
    // Drift first, then identity: the magnitude is what says how wrong the
    // picture is; a bare pointer comparison says only that it happened.
    CHECK(drift == 0.0f);
    CHECK(fsA.geosetDeforms[0].positions.data() != fsB.geosetDeforms[0].positions.data());

    // ...and the two really did diverge, so a gate that passed because nothing
    // moved would fail here instead.
    f32 apart = 0.0f;
    for (std::size_t i = 0; i < mine.size(); ++i)
        apart = (std::max)(apart, Dist(fsB.geosetDeforms[0].positions[i], mine[i]));
    CHECK(apart > 100.0f);
}

TEST_CASE("D3 cloth: the deform's mesh<->cloth map agrees with the seeds",
          "[d3][cloth][physics][corpus]") {
    // The deform writes one cloth vertex into one mesh vertex, and nothing
    // downstream can tell a wrong pairing from a modelling error: the cape
    // still has the right bounds, the right vertex count and no NaN — it is
    // just scrambled. The seeds are the check, because a cloth vertex was
    // seeded from the mesh vertex it names, so `meshToCloth[i] == c` is right
    // only when the two share a position.
    //
    // The face winding rides along for the same reason: `RecomputeNormals`
    // takes `cross(p1-p0, p2-p0)` in the *cloth's* winding while the geoset is
    // drawn with the mesh's own index buffer, and a cloth authored the other
    // way round would light every deformed surface from behind.
    const auto& clt = ClothById();
    if (clt.empty()) {
        WARN("no .clt under " << CorpusRoot().string() << " -- gate skipped");
        return;
    }
    std::size_t checked = 0, verts = 0, corners = 0;
    for (const auto& path : FindFiles(CorpusRoot() / "Appearances", ".app")) {
        if (checked >= 40)
            break;
        const auto bytes = ReadAll(path);
        auto parsed = d3n::parseAppearances(bytes);
        if (!parsed)
            continue;
        for (const auto& piece : d3c::D3FindClothPieces(*parsed, 0)) {
            auto it = clt.find(piece.clothSno);
            if (it == clt.end())
                continue;
            const auto& sub =
                parsed->tGeoSet0.arSubObjects[static_cast<std::size_t>(piece.subObject)];
            auto def = d3c::D3BuildCloth(sub, &it->second, 1.0f);
            if (!def || def->faces.empty())
                continue;
            ++checked;
            INFO(path.filename().string() << " sub " << piece.subObject);

            for (std::size_t m = 0; m < def->meshToCloth.size(); ++m) {
                const i32 c = def->meshToCloth[m];
                if (c < 0)
                    continue;
                ++verts;
                REQUIRE(static_cast<std::size_t>(c) < def->vertices.size());
                CHECK(Dist(def->vertices[static_cast<std::size_t>(c)].position,
                           sub.arVertices[m].vPosition) < 1.0e-4f);
            }

            // Aggregate, not per corner: a corner of a curved sheet sits near
            // 90 degrees to its own face and 1.5% of the corpus's do cross
            // zero. A cloth authored the other way round takes the *mean* to
            // -1, so that is what this asserts.
            //
            // The bar is below zero rather than near +1 because three of the
            // forty pieces (`Body_Hanged*_caOut_Gore`) are double-shelled: one
            // sheet of cloth faces drives two shells of mesh whose normals
            // oppose, and their mean lands at 0. Those three the sign cannot
            // speak for at all -- but the single-shell capes, which is every
            // wearable piece, still cannot flip without failing here.
            double dotSum = 0;
            std::size_t dotCount = 0;
            for (const auto& f : def->faces) {
                const Vector3f& p0 = def->vertices[static_cast<std::size_t>(f.v[0])].position;
                const Vector3f& p1 = def->vertices[static_cast<std::size_t>(f.v[1])].position;
                const Vector3f& p2 = def->vertices[static_cast<std::size_t>(f.v[2])].position;
                const Vector3f e1{p1.x - p0.x, p1.y - p0.y, p1.z - p0.z};
                const Vector3f e2{p2.x - p0.x, p2.y - p0.y, p2.z - p0.z};
                Vector3f fn{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z,
                            e1.x * e2.y - e1.y * e2.x};
                const f32 len = std::sqrt(fn.x * fn.x + fn.y * fn.y + fn.z * fn.z);
                if (!(len > 1.0e-6f))
                    continue;
                fn = {fn.x / len, fn.y / len, fn.z / len};
                for (int k = 0; k < 3; ++k) {
                    const i32 mv = def->vertices[static_cast<std::size_t>(f.v[k])].meshVertex;
                    if (mv < 0 || static_cast<std::size_t>(mv) >= sub.arVertices.size())
                        continue;
                    const Vector3f mn = d3n::vertexNormal(sub.arVertices[static_cast<std::size_t>(mv)]);
                    ++corners;
                    dotSum += fn.x * mn.x + fn.y * mn.y + fn.z * mn.z;
                    ++dotCount;
                }
            }
            CHECK(dotCount > 0);
            if (dotCount)
                CHECK(dotSum / static_cast<double>(dotCount) > -0.2);
        }
    }
    std::printf("[d3-cloth] map/winding: %zu piece(s), %zu mapped vertices, %zu face corners\n",
                checked, verts, corners);
    CHECK(checked > 0);
}

// ---------------------------------------------------------------------------
// Census. Hidden — evidence for the header's numbers, not a gate.
// ---------------------------------------------------------------------------

TEST_CASE("D3 cloth census over the corpus", "[.census][d3][cloth]") {
    const fs::path root = CorpusRoot();
    const auto apps = FindFiles(root / "Appearances", ".app");
    if (apps.empty()) {
        WARN("D3 corpus not found at " << root.string() << " -- census skipped");
        return;
    }

    std::map<i64, std::size_t> relax, flags, planeSlot0, planeSlot1;
    std::size_t clothFiles = 0;
    for (const auto& [id, c] : ClothById()) {
        (void)id;
        ++clothFiles;
        ++relax[c.dwRelaxIterations];
        ++flags[c.dwFlags];
        ++planeSlot0[c.nCollisionPlane0];
        ++planeSlot1[c.nCollisionPlane1];
    }
    std::printf("[d3-cloth] .clt files %zu\n", clothFiles);
    const auto dump = [](const char* label, const std::map<i64, std::size_t>& m) {
        std::printf("[d3-cloth] %-22s", label);
        for (const auto& [v, c] : m)
            std::printf(" %lld:%zu", static_cast<long long>(v), c);
        std::printf("\n");
    };
    dump("relaxIterations", relax);
    dump("dwFlags", flags);
    dump("nCollisionPlane0", planeSlot0);
    dump("nCollisionPlane1", planeSlot1);

    std::size_t swept = 0, parsed = 0, withCloth = 0, blocks = 0;
    std::size_t verts = 0, faces = 0, staples = 0, stretch = 0, bend = 0;
    std::size_t pinnedFirstOk = 0, pinnedFirstBad = 0, zeroMass = 0;
    std::size_t withIndexBit = 0, capsuleModels = 0, capsules = 0, planeHardpoints = 0;
    std::size_t drivingBones = 0, contiguousBones = 0, usedBones = 0;
    const std::size_t limit = SweepLimit();
    for (const auto& p : apps) {
        if (++swept > limit)
            break;
        const auto bytes = ReadAll(p);
        auto app = d3n::parseAppearances(bytes);
        if (!app)
            continue;
        ++parsed;
        if (!app->arCollisionCapsules.empty()) {
            ++capsuleModels;
            capsules += app->arCollisionCapsules.size();
        }
        for (const auto& h : app->arHardpoints)
            if (h.szName.rfind("HP_cloth_plane_", 0) == 0)
                ++planeHardpoints;

        bool any = false;
        for (const auto& sub : app->tGeoSet0.arSubObjects) {
            if (sub.arClothData.empty() || sub.arClothData.front().arVertices.empty())
                continue;
            any = true;
            ++blocks;
            const auto& cd = sub.arClothData.front();
            verts += cd.arVertices.size();
            faces += cd.arFaces.size();
            staples += cd.arStaples.size();
            stretch += cd.arStretchConstraints.size();
            bend += cd.arBendConstraints.size();
            drivingBones += static_cast<std::size_t>((std::max)(cd.dwDrivingBoneCount, 0));
            if ((sub.dwVertexFormat & d3n::kSubObjectHasClothIndex) != 0)
                ++withIndexBit;

            const std::size_t pin = cd.arStaples.size();
            bool ok = true;
            for (std::size_t i = 0; i < cd.arVertices.size(); ++i) {
                const bool isZero = cd.arVertices[i].flInvMass == 0.0f;
                if (isZero)
                    ++zeroMass;
                if (isZero != (i < pin))
                    ok = false;
            }
            (ok ? pinnedFirstOk : pinnedFirstBad)++;

            // Are driving-bone vertex ranges contiguous? The client computes the
            // flag; how often it comes out true decides whether the fast path
            // is the common one or the exception.
            const i32 nb = (std::max)(cd.dwDrivingBoneCount, 0);
            std::vector<i32> lo(static_cast<std::size_t>(nb), -1), hi(static_cast<std::size_t>(nb), -1),
                cnt(static_cast<std::size_t>(nb), 0);
            for (std::size_t i = pin; i < cd.arVertices.size(); ++i) {
                const i32 b = cd.arVertices[i].nDrivingBone;
                if (b < 0 || b >= nb)
                    continue;
                const i32 idx = static_cast<i32>(i);
                lo[static_cast<std::size_t>(b)] =
                    (lo[static_cast<std::size_t>(b)] < 0) ? idx : (std::min)(lo[static_cast<std::size_t>(b)], idx);
                hi[static_cast<std::size_t>(b)] = (std::max)(hi[static_cast<std::size_t>(b)], idx);
                ++cnt[static_cast<std::size_t>(b)];
            }
            for (i32 b = 0; b < nb; ++b) {
                if (cnt[static_cast<std::size_t>(b)] <= 0)
                    continue;
                ++usedBones;
                if (hi[static_cast<std::size_t>(b)] + 1 - lo[static_cast<std::size_t>(b)] ==
                    cnt[static_cast<std::size_t>(b)])
                    ++contiguousBones;
            }
        }
        if (any)
            ++withCloth;
    }

    std::printf("[d3-cloth] .app parsed %zu of %zu swept; %zu carry cloth, %zu blocks\n", parsed,
                swept, withCloth, blocks);
    std::printf("[d3-cloth] verts %zu faces %zu staples %zu stretch %zu bend %zu\n", verts, faces,
                staples, stretch, bend);
    std::printf("[d3-cloth] flInvMass==0 %zu (staples %zu); pinned-first blocks %zu ok / %zu bad\n",
                zeroMass, staples, pinnedFirstOk, pinnedFirstBad);
    std::printf("[d3-cloth] cloth-index bit on %zu of %zu blocks\n", withIndexBit, blocks);
    std::printf("[d3-cloth] driving bones %zu declared, %zu with free vertices, %zu contiguous\n",
                drivingBones, usedBones, contiguousBones);
    std::printf("[d3-cloth] models with capsules %zu (%zu capsules); HP_cloth_plane_* %zu\n",
                capsuleModels, capsules, planeHardpoints);
}

