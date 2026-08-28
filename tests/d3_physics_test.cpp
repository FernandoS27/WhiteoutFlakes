// ============================================================================
// D3 physics — the census the rig decode is corrected against, and the gates
// that pin it. See D3_PHYSICS_PLAN.md.
//
// Runs against the extracted corpus under `C:/Projects/WhiteoutLib/Corpus/D3`;
// override with WDX_TEST_D3_CORPUS. Skipped is not passed.
//
// The census exists because four `CollisionShape` fields are named for the
// wrong job (plan §3.3) and a decompile alone is not enough to rename a field
// in a parser three products depend on. `PhysicsBridge_CreateFixture` says
// `+4` is the shape KIND and `+16` is a DENSITY; the corpus has to agree —
// a kind field takes three values and a density is zero on exactly the shapes
// the ragdoll builder leaves kinematic. If the distributions disagree with the
// decompile, the decompile is what gets re-read.
//
// `.app` parsing is expensive (11,347 files, 14.4 GB, p95 8.3 MB), so the
// sweep is bounded by WDX_TEST_D3_LIMIT (default 2000, deterministically the
// first N in sorted order; 0 = all) and prints what it skipped. A bounded
// sweep that reads like full coverage is worse than an honest partial one.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "renderer/profiles/diablo3/d3_collision.h"

#include <whiteout/sno/d3/native/d3_native.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace d3n = ::whiteout::sno::d3::native;
using namespace ::whiteout;

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

/// @brief A `{value -> count}` tally that prints itself in value order.
struct Tally {
    std::map<i64, std::size_t> counts;
    void Add(i64 v) { ++counts[v]; }
    std::size_t Total() const {
        std::size_t n = 0;
        for (const auto& [_, c] : counts)
            n += c;
        return n;
    }
    void Print(const char* label, std::size_t maxRows = 16) const {
        std::printf("[d3-phys] %-28s", label);
        std::size_t row = 0;
        for (const auto& [v, c] : counts) {
            if (row++ >= maxRows) {
                std::printf(" ...(%zu more)", counts.size() - maxRows);
                break;
            }
            std::printf(" %lld:%zu", static_cast<long long>(v), c);
        }
        std::printf("\n");
    }
};

/// @brief Float tallies keyed on the bit pattern, so 0.0 and -0.0 stay apart
///        and a "constant in the corpus" claim is exact rather than binned.
struct FloatTally {
    std::map<f32, std::size_t> counts;
    void Add(f32 v) { ++counts[v]; }
    void Print(const char* label, std::size_t maxRows = 10) const {
        std::printf("[d3-phys] %-28s", label);
        std::vector<std::pair<std::size_t, f32>> byFreq;
        byFreq.reserve(counts.size());
        for (const auto& [v, c] : counts)
            byFreq.emplace_back(c, v);
        std::sort(byFreq.begin(), byFreq.end(), [](auto a, auto b) { return a.first > b.first; });
        for (std::size_t i = 0; i < byFreq.size() && i < maxRows; ++i)
            std::printf(" %g:%zu", static_cast<double>(byFreq[i].second), byFreq[i].first);
        if (byFreq.size() > maxRows)
            std::printf(" ...(%zu distinct)", byFreq.size());
        std::printf("\n");
    }
};

} // namespace

// ---------------------------------------------------------------------------
// The census. Hidden ([.]) — it is evidence for the renames, not a gate.
// ---------------------------------------------------------------------------
TEST_CASE("D3 physics census: shapes, constraints and cloth over the corpus",
          "[.census][d3][physics]") {
    const fs::path root = CorpusRoot();
    const auto apps = FindFiles(root / "Appearances", ".app");
    const auto clts = FindFiles(root / "Cloth", ".clt");
    const auto phys = FindFiles(root / "Physics", ".phy");
    if (apps.empty()) {
        WARN("D3 corpus not found at " << root.string() << " — census skipped");
        return;
    }

    // ---- .phy -------------------------------------------------------------
    {
        Tally flagBits, bodyClass, mask;
        FloatTally friction, material2, restitution, linDamp, angDamp;
        std::size_t parsed = 0;
        for (const auto& p : phys) {
            const auto bytes = ReadAll(p);
            auto a = d3n::parsePhysics(bytes);
            if (!a)
                continue;
            ++parsed;
            for (int b = 0; b < 32; ++b)
                if ((a->dwFlags >> b) & 1)
                    flagBits.Add(b);
            bodyClass.Add(a->nBodyClass);
            mask.Add(a->wCollisionMask);
            friction.Add(a->flFriction);
            material2.Add(a->flMaterial2);
            restitution.Add(a->flRestitution);
            linDamp.Add(a->flLinearDamping);
            angDamp.Add(a->flAngularDamping);
        }
        std::printf("[d3-phys] ---- .phy: %zu of %zu parsed ----\n", parsed, phys.size());
        flagBits.Print("phy flag BIT set count");
        bodyClass.Print("phy nBodyClass");
        mask.Print("phy wCollisionMask");
        friction.Print("phy flFriction");
        restitution.Print("phy flRestitution");
        material2.Print("phy flMaterial2");
        linDamp.Print("phy flLinearDamping");
        angDamp.Print("phy flAngularDamping");
    }

    // ---- .clt -------------------------------------------------------------
    {
        Tally flagBits, relax, planes0, planes1, planes2, planes3, customWind;
        FloatTally mass, blend, stretchA, stretchB, bend, extForce, drag, gravity, root2, linDamp,
            contactDamp;
        std::size_t parsed = 0;
        for (const auto& p : clts) {
            const auto bytes = ReadAll(p);
            auto a = d3n::parseCloth(bytes);
            if (!a)
                continue;
            ++parsed;
            for (int b = 0; b < 32; ++b)
                if ((a->dwFlags >> b) & 1)
                    flagBits.Add(b);
            relax.Add(a->dwRelaxIterations);
            planes0.Add(a->nCollisionPlane0);
            planes1.Add(a->nCollisionPlane1);
            planes2.Add(a->nCollisionPlane2);
            planes3.Add(a->nCollisionPlane3);
            customWind.Add(a->nUseCustomWind);
            mass.Add(a->flMass);
            blend.Add(a->flSkinBlendRate);
            stretchA.Add(a->flStretchStiffness0);
            stretchB.Add(a->flStretchStiffness1);
            bend.Add(a->flBendStiffness);
            extForce.Add(a->flExternalForceScale);
            drag.Add(a->flDragCoefficient);
            gravity.Add(a->flGravity);
            root2.Add(a->flRootStiffness);
            linDamp.Add(a->flLinearDamping);
            contactDamp.Add(a->flContactDamping);
        }
        std::printf("[d3-phys] ---- .clt: %zu of %zu parsed ----\n", parsed, clts.size());
        flagBits.Print("clt flag BIT set count");
        relax.Print("clt dwRelaxIterations");
        planes0.Print("clt nCollisionPlane0");
        planes1.Print("clt nCollisionPlane1");
        planes2.Print("clt nCollisionPlane2");
        planes3.Print("clt nCollisionPlane3");
        customWind.Print("clt nUseCustomWind");
        mass.Print("clt flMass");
        blend.Print("clt flSkinBlendRate");
        stretchA.Print("clt flStretchStiffness0");
        stretchB.Print("clt flStretchStiffness1");
        bend.Print("clt flBendStiffness");
        extForce.Print("clt flExternalForceScale");
        drag.Print("clt flDragCoefficient");
        gravity.Print("clt flGravity");
        root2.Print("clt flRootStiffness");
        linDamp.Print("clt flLinearDamping");
        contactDamp.Print("clt flContactDamping");
    }

    // ---- .app -------------------------------------------------------------
    const std::size_t limit = SweepLimit();
    const std::size_t sweep = (limit == 0) ? apps.size() : (std::min)(limit, apps.size());

    std::size_t parsed = 0, withBoneShapes = 0, withSubShapes = 0, withBoneJoints = 0,
                withAppJoints = 0, withCloth = 0, withCapsules = 0, withBodies = 0;
    std::size_t boneShapes = 0, subShapes = 0, boneJoints = 0, appJoints = 0;
    std::size_t maxBoneShapes = 0, maxAppJoints = 0, maxBodies = 0;
    Tally shapeKind, shapeFlagsLow, shapeFlagBits, shapeLod, polyBlob, jointType, jointFlagBits;
    FloatTally density, scaleY, scaleZ, radius;
    // Cross-tabs. The three floats at +16/+20/+24 look like a scale triple *and*
    // like a density; the runtime reads only the first, so "are they equal" is
    // the question that separates the two readings.
    std::size_t xyzAllEqual = 0, xyzDiffer = 0;
    Tally kindVsRadius, kindVsBlob, kindVsBodyBit;
    // The joint record: `eConstraintType` is at runtime +8 (a FLAGS word) and
    // `dwUnknown0C` at runtime +12 (the TYPE) once szName's 64 bytes collapse
    // to an 8-byte handle — see the -56 shift. Tally both as VALUES.
    Tally jointFlagsValue, jointU10, jointU14;
    FloatTally jp[13];
    // Cloth
    std::size_t clothBlocks = 0, clothVerts = 0, clothFaces = 0, clothStaples = 0,
                clothStretch = 0, clothBend = 0, clothPinned = 0, maxClothVerts = 0;
    Tally drivingBones;

    for (std::size_t i = 0; i < sweep; ++i) {
        const auto bytes = ReadAll(apps[i]);
        auto ap = d3n::parseAppearances(bytes);
        if (!ap)
            continue;
        ++parsed;

        auto tallyJoint = [&](const d3n::ConstraintParameters& c) {
            jointType.Add(c.eConstraintType);   // runtime +12 — the switch value
            jointFlagsValue.Add(c.dwFlags); // runtime +8 — the flags word
            for (int bit = 0; bit < 8; ++bit)
                if ((c.dwFlags >> bit) & 1)
                    jointFlagBits.Add(bit);
            jointU10.Add(c.nBoneIndexA);
            jointU14.Add(c.nBoneIndexB);
            const f32 p[13] = {c.flLimitLower, c.flLimitUpper, c.flConeAngle, c.flTwistLower, c.flTwistUpper,
                               c.flParam05, c.flParam06, c.flParam07, c.flParam08, c.flParam09,
                               c.flParam10, c.flParam11, c.flParam12};
            for (int k = 0; k < 13; ++k)
                jp[k].Add(p[k]);
        };

        std::size_t here = 0, bodiesHere = 0;
        for (const auto& b : ap->arBones) {
            here += b.arCollisionShapes.size();
            bool bodied = false;
            for (const auto& s : b.arCollisionShapes) {
                shapeKind.Add(s.eShapeType);
                shapeFlagsLow.Add(s.dwFlags);
                for (int bit = 0; bit < 32; ++bit)
                    if ((s.dwFlags >> bit) & 1)
                        shapeFlagBits.Add(bit);
                if (s.dwFlags & 1)
                    bodied = true;
                shapeLod.Add(s.nLodIndex);
                density.Add(s.flScaleX);
                scaleY.Add(s.flScaleY);
                scaleZ.Add(s.flScaleZ);
                radius.Add(s.flRadius);
                polyBlob.Add(static_cast<i64>(s.arPolytopeData.size()));
                if (s.flScaleX == s.flScaleY && s.flScaleY == s.flScaleZ)
                    ++xyzAllEqual;
                else
                    ++xyzDiffer;
                // kind * 10 + predicate, so one tally reads as a cross-tab.
                kindVsRadius.Add(s.eShapeType * 10 + (s.flRadius != 0.0f ? 1 : 0));
                kindVsBlob.Add(s.eShapeType * 10 + (s.arPolytopeData.empty() ? 0 : 1));
                kindVsBodyBit.Add(s.eShapeType * 10 + (s.dwFlags & 1));
            }
            if (bodied)
                ++bodiesHere;
            boneJoints += b.arConstraints.size();
            for (const auto& c : b.arConstraints)
                tallyJoint(c);
        }
        if (bodiesHere)
            ++withBodies;
        boneShapes += here;
        maxBoneShapes = (std::max)(maxBoneShapes, here);
        maxBodies = (std::max)(maxBodies, bodiesHere);
        if (here)
            ++withBoneShapes;
        if (boneJoints && !ap->arBones.empty()) {
            bool any = false;
            for (const auto& b : ap->arBones)
                any = any || !b.arConstraints.empty();
            if (any)
                ++withBoneJoints;
        }
        appJoints += ap->arConstraints.size();
        maxAppJoints = (std::max)(maxAppJoints, ap->arConstraints.size());
        if (!ap->arConstraints.empty())
            ++withAppJoints;
        for (const auto& c : ap->arConstraints)
            tallyJoint(c);
        if (!ap->arCollisionCapsules.empty())
            ++withCapsules;

        std::size_t subHere = 0;
        bool clothHere = false;
        for (const auto* gs : {&ap->tGeoSet0, &ap->tGeoSet1}) {
            for (const auto& so : gs->arSubObjects) {
                subHere += so.arCollisionShapes.size();
                for (const auto& cs : so.arClothData) {
                    clothHere = true;
                    ++clothBlocks;
                    clothVerts += cs.arVertices.size();
                    maxClothVerts = (std::max)(maxClothVerts, cs.arVertices.size());
                    clothFaces += cs.arFaces.size();
                    clothStaples += cs.arStaples.size();
                    clothStretch += cs.arStretchConstraints.size();
                    clothBend += cs.arBendConstraints.size();
                    drivingBones.Add(cs.dwDrivingBoneCount);
                    for (const auto& v : cs.arVertices)
                        if (v.flInvMass == 0.0f)
                            ++clothPinned;
                }
            }
        }
        subShapes += subHere;
        if (subHere)
            ++withSubShapes;
        if (clothHere)
            ++withCloth;
    }

    std::printf("[d3-phys] ---- .app: %zu parsed of %zu swept (%zu in corpus, %zu skipped) ----\n",
                parsed, sweep, apps.size(), apps.size() - sweep);
    std::printf("[d3-phys] appearances with bone shapes %zu, subobject shapes %zu, bone joints "
                "%zu, appearance joints %zu, cloth %zu, capsules %zu, RAGDOLL-CAPABLE %zu\n",
                withBoneShapes, withSubShapes, withBoneJoints, withAppJoints, withCloth,
                withCapsules, withBodies);
    std::printf("[d3-phys] totals: bone shapes %zu (max/model %zu), subobject shapes %zu, bone "
                "joints %zu, appearance joints %zu (max/model %zu), bodied bones max/model %zu\n",
                boneShapes, maxBoneShapes, subShapes, boneJoints, appJoints, maxAppJoints,
                maxBodies);
    shapeKind.Print("shape +4 (KIND?)");
    shapeFlagsLow.Print("shape +0 (FLAGS?) value");
    shapeFlagBits.Print("shape +0 BIT set count");
    shapeLod.Print("shape +12 (LOD?)");
    polyBlob.Print("shape polytope blob bytes");
    density.Print("shape +16 (DENSITY?)");
    scaleY.Print("shape +20");
    scaleZ.Print("shape +24");
    radius.Print("shape +72 (RADIUS?)");
    std::printf("[d3-phys] shape +16/+20/+24 all equal %zu, differ %zu\n", xyzAllEqual, xyzDiffer);
    kindVsRadius.Print("kind*10 + (radius != 0)");
    kindVsBlob.Print("kind*10 + (has blob)");
    kindVsBodyBit.Print("kind*10 + (flags bit0)");
    jointType.Print("constraint TYPE (rt +12)");
    jointFlagsValue.Print("constraint FLAGS (rt +8)");
    jointFlagBits.Print("constraint FLAGS BIT count");
    jointU10.Print("constraint dwUnknown10");
    jointU14.Print("constraint dwUnknown14");
    for (int k = 0; k < 13; ++k) {
        char label[48];
        std::snprintf(label, sizeof(label), "constraint flParam%02d (rt +%d)", k, 120 + 4 * k);
        jp[k].Print(label, 8);
    }
    std::printf("[d3-phys] cloth: %zu blocks, %zu verts (max %zu), %zu faces, %zu staples, %zu "
                "stretch, %zu bend, %zu pinned (invMass==0)\n",
                clothBlocks, clothVerts, maxClothVerts, clothFaces, clothStaples, clothStretch,
                clothBend, clothPinned);
    drivingBones.Print("cloth dwDrivingBoneCount");

    CHECK(parsed > 0);
}

// ---------------------------------------------------------------------------
// D3-PH1 gate: the shape decode, and the cooked polytope in particular.
//
// The discriminator here is deliberately NOT "does it parse". A 96-byte header
// holds four payload references and reading the wrong one still yields plausible
// floats — StarCraft II's hull work lost time to exactly that, building every
// fixture out of the FACE-NORMAL table and getting a ~2-unit blob of unit
// vectors that drew as a cube on every limb. So this asserts the two things
// that separate a point cloud from a normal table: the points are NOT unit
// length, and the header's own centroid falls inside their bounding box.
// ---------------------------------------------------------------------------
TEST_CASE("D3 collision: spheres, capsules and cooked polytopes decode", "[d3][physics][corpus]") {
    const fs::path root = CorpusRoot();
    const auto apps = FindFiles(root / "Appearances", ".app");
    if (apps.empty()) {
        WARN("D3 corpus not found at " << root.string() << " -- gate skipped");
        return;
    }
    namespace d3p = flakes::renderer::profiles::diablo3;
    using flakes::renderer::model::CollisionShapeType;

    const std::size_t limit = SweepLimit();
    const std::size_t sweep = (limit == 0) ? apps.size() : (std::min)(limit, apps.size());

    std::size_t models = 0, spheres = 0, capsules = 0, hulls = 0;
    std::size_t kind2 = 0, kind2Decoded = 0;
    std::size_t badRadius = 0, badEdgeIndex = 0, nonFinite = 0, degenerate = 0;
    std::size_t centroidInside = 0, unitish = 0;
    double meanLen = 0.0;
    std::size_t meanLenN = 0;

    for (std::size_t i = 0; i < sweep; ++i) {
        const auto bytes = ReadAll(apps[i]);
        auto ap = d3n::parseAppearances(bytes);
        if (!ap)
            continue;
        for (const auto& b : ap->arBones)
            for (const auto& sh : b.arCollisionShapes)
                if (sh.eShapeType == 2 && sh.nLodIndex == 0)
                    ++kind2;

        auto built = d3p::D3BuildCollisionShapes(*ap, bytes, 0);
        if (built.shapes.empty())
            continue;
        ++models;
        REQUIRE(built.shapes.size() == built.bones.size());

        for (std::size_t k = 0; k < built.shapes.size(); ++k) {
            const auto& d = built.shapes[k];
            REQUIRE(built.bones[k] >= 0);
            REQUIRE(static_cast<std::size_t>(built.bones[k]) < ap->arBones.size());

            if (d.type == static_cast<i32>(CollisionShapeType::Sphere)) {
                ++spheres;
                if (!(d.radius > 0.0f))
                    ++badRadius;
            } else if (d.type == static_cast<i32>(CollisionShapeType::Capsule)) {
                ++capsules;
                if (!(d.radius > 0.0f))
                    ++badRadius;
            } else if (d.type == static_cast<i32>(CollisionShapeType::Hull)) {
                ++hulls;
                ++kind2Decoded;
                if (d.hullPoints.size() < 4 || d.hullEdges.size() < 12)
                    ++degenerate;
                f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
                for (const auto& pt : d.hullPoints) {
                    const f32 c[3] = {pt.x, pt.y, pt.z};
                    for (int a = 0; a < 3; ++a) {
                        if (!std::isfinite(c[a]))
                            ++nonFinite;
                        lo[a] = (std::min)(lo[a], c[a]);
                        hi[a] = (std::max)(hi[a], c[a]);
                    }
                    const double len = std::sqrt(static_cast<double>(pt.x) * pt.x +
                                                 static_cast<double>(pt.y) * pt.y +
                                                 static_cast<double>(pt.z) * pt.z);
                    meanLen += len;
                    ++meanLenN;
                    if (len > 0.98 && len < 1.02)
                        ++unitish;
                }
                for (const auto e : d.hullEdges)
                    if (static_cast<std::size_t>(e) >= d.hullPoints.size())
                        ++badEdgeIndex;
                // vertices[0] is the header centroid.
                const f32 c[3] = {d.vertices[0].x, d.vertices[0].y, d.vertices[0].z};
                bool inside = true;
                for (int a = 0; a < 3; ++a)
                    inside = inside && c[a] >= lo[a] - 1e-3f && c[a] <= hi[a] + 1e-3f;
                if (inside)
                    ++centroidInside;
            } else {
                FAIL("unexpected shape type " << d.type);
            }
        }
    }

    std::printf("[d3-coll] %zu models, %zu spheres, %zu capsules, %zu hulls; kind2 %zu seen, "
                "%zu decoded\n",
                models, spheres, capsules, hulls, kind2, kind2Decoded);
    std::printf("[d3-coll] hull points: mean |p| %.3f, unit-length %zu of %zu; centroid inside "
                "AABB %zu of %zu\n",
                meanLenN ? meanLen / static_cast<double>(meanLenN) : 0.0, unitish, meanLenN,
                centroidInside, hulls);
    std::printf("[d3-coll] bad radius %zu, bad edge index %zu, non-finite %zu, degenerate %zu\n",
                badRadius, badEdgeIndex, nonFinite, degenerate);

    REQUIRE(hulls > 100);
    REQUIRE(spheres + capsules > 100);
    CHECK(badRadius == 0);
    CHECK(badEdgeIndex == 0);
    CHECK(nonFinite == 0);
    CHECK(degenerate == 0);
    // The two claims the SC2 hull mistake would fail. A face-normal table is
    // all unit vectors, so its mean length is 1.0 and a real centroid sits
    // outside its box; a point cloud is neither.
    CHECK(unitish * 20 < meanLenN);
    CHECK(centroidInside * 100 >= hulls * 99);
    // And the reader must not be quietly dropping most of what it is given.
    CHECK(kind2Decoded * 10 >= kind2 * 9);
}

// ---------------------------------------------------------------------------
// Which models to point a gate at. Hidden — it names exemplars, it asserts
// nothing about them.
// ---------------------------------------------------------------------------
TEST_CASE("D3 physics: name the rigs", "[.names][d3][physics]") {
    const fs::path root = CorpusRoot();
    const auto apps = FindFiles(root / "Appearances", ".app");
    if (apps.empty()) {
        WARN("no corpus");
        return;
    }
    const std::size_t limit = SweepLimit();
    const std::size_t sweep = (limit == 0) ? apps.size() : (std::min)(limit, apps.size());
    std::vector<std::pair<std::size_t, std::string>> ragdolls, capsuled, jointed, lod1, dyn, dyn1, anchored, hanging;
    // `Physics_CreateActorBoneBodies` is called with lodIndex **1**, and a body
    // whose shapes all sit at another LOD gets no fixture and is destroyed. So
    // "how many models have LOD-1 shapes at all" decides whether the client's
    // own bone-body rig exists on shipped content.
    std::size_t lod0Models = 0, lod1Models = 0, bothModels = 0;
    for (std::size_t i = 0; i < sweep; ++i) {
        const auto bytes = ReadAll(apps[i]);
        auto ap = d3n::parseAppearances(bytes);
        if (!ap)
            continue;
        std::size_t bodied = 0, caps = 0, joints = ap->arConstraints.size();
        std::size_t n0 = 0, n1 = 0, dynBones = 0, dynBones1 = 0;
        for (const auto& b : ap->arBones) {
            bool any = false, anyDyn = false, anyDyn1 = false;
            for (const auto& sh : b.arCollisionShapes) {
                if (sh.dwFlags & 1)
                    any = true;
                if (sh.eShapeType == 1)
                    ++caps;
                if (sh.nLodIndex == 0)
                    ++n0;
                else if (sh.nLodIndex == 1)
                    ++n1;
                // The body-type rule: Dynamic when a LOD-matching shape has a
                // positive flScaleX, Static otherwise. Asked at LOD 0.
                if (sh.nLodIndex == 0 && sh.flScaleX > 0.0f)
                    anyDyn = true;
                if (sh.nLodIndex == 1 && sh.flScaleX > 0.0f)
                    anyDyn1 = true;
            }
            if (any)
                ++bodied;
            if (anyDyn)
                ++dynBones;
            if (anyDyn1)
                ++dynBones1;
            joints += b.arConstraints.size();
        }
        const auto name = apps[i].filename().string();
        if (bodied)
            ragdolls.emplace_back(bodied, name);
        if (caps)
            capsuled.emplace_back(caps, name);
        if (joints)
            jointed.emplace_back(joints, name);
        if (n1)
            lod1.emplace_back(n1, name);
        // An anchored rig only GROWS through `dwFlags` bit 4 on a bone's first
        // constraint: the level below an anchor always has a kinematic ancestor,
        // so the "ancestor is Dynamic" path cannot start a chain. Which models
        // have both an anchor and a bit-4 constraint is therefore the question
        // "which models actually hang something".
        {
            std::size_t anchors = 0, hangers = 0;
            for (const auto& bb : ap->arBones) {
                bool anchor = false;
                for (const auto& sh : bb.arCollisionShapes)
                    if (sh.dwFlags & 1)
                        anchor = true;
                if (anchor)
                    ++anchors;
                else if (!bb.arConstraints.empty() && (bb.arConstraints[0].dwFlags & 0x10))
                    ++hangers;
            }
            if (anchors)
                anchored.emplace_back(anchors, name);
            if (anchors && hangers)
                hanging.emplace_back(hangers, name);
        }
        if (dynBones)
            dyn.emplace_back(dynBones, name);
        if (dynBones1)
            dyn1.emplace_back(dynBones1, name);
        if (n0)
            ++lod0Models;
        if (n1)
            ++lod1Models;
        if (n0 && n1)
            ++bothModels;
    }
    std::printf("[d3-name] models with LOD0 shapes %zu, LOD1 %zu, both %zu\n", lod0Models,
                lod1Models, bothModels);
    auto top = [](std::vector<std::pair<std::size_t, std::string>>& v, const char* what) {
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::printf("[d3-name] %-26s %zu models; top:\n", what, v.size());
        for (std::size_t i = 0; i < v.size() && i < 12; ++i)
            std::printf("[d3-name]     %5zu  %s\n", v[i].first, v[i].second.c_str());
    };
    top(ragdolls, "bodied bones");
    top(capsuled, "capsule shapes");
    top(jointed, "constraints");
    top(lod1, "LOD1 shapes");
    top(dyn, "dynamic bones @LOD0");
    top(dyn1, "dynamic bones @LOD1");
    top(anchored, "anchor bones");
    top(hanging, "hanging bones (bit4)");
    CHECK(true);
}
