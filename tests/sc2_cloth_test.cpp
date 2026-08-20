// `PHCL` -> Snowball's cloth solver, end to end.
//
// The chain is longer than the rigid-body one and fails in places that one
// cannot. `sc2_physics.cpp` maps a chunk onto a body and the body is the
// answer; here the chunk describes a *mesh*, the mesh is authored into records
// by a second pipeline (`snowball::BuildCloth`), the records are simulated, and
// the result has to arrive back in the skinning palette as one node per
// particle. Four seams, and three of them look identical from a screenshot:
//
//   - **Which region is the cloth.** `PHCL.clothMeshCount` is a REGN *index*
//     wearing a count's name. Reading it as a count picks region 4 of a model
//     whose cloth is region 0 and quietly builds a cape out of somebody's boot.
//   - **Which vertices are pinned.** `simEnabled` bit 0 is *movable*; inverting
//     it gives a cloth pinned where it should hang and free where it should
//     hold, which settles into a plausible shape and is completely wrong.
//   - **Whether the anchors are alive.** A cloth whose bind and animated anchor
//     tables agree every frame hangs correctly and never follows the body — the
//     single most plausible-looking failure in the path, exactly as a kinematic
//     ragdoll is for `sc2_physics`.
//   - **Whether the palette rewrite lands.** The visible region is skinned to
//     particles, not bones; a rewrite that misses leaves the cape rigid while
//     every number in the simulation is right.
//
// So the cases below assert on shipped content and on motion, not on shapes:
// that the build picks the region the `PHAC` records agree on, that the pinned
// set is the authored one, that a settled cloth has moved *down* from its rest
// shape while its pinned particles have not moved at all, and that a rotated
// skeleton drags the free particles with it.

#include "io/m3/m3_model_adapter.h"
#include "renderer/profiles/sc2_heroes/sc2_cloth.h"
#include "whiteout/flakes/pose_stage.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/m3/parser.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace sc2 = whiteout::flakes::renderer::profiles::sc2_heroes;

using whiteout::Matrix44f;
using whiteout::Vector3f;
using whiteout::f32;
using whiteout::flakes::io::M3ModelAdapter;
using whiteout::flakes::renderer::animation::PoseStageContext;
using whiteout::flakes::renderer::model::FrameState;
using Catch::Approx;

namespace {

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_SC2_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus");
}

/// Kael'thas: one cloth, 189 particles over a two-collider proxy, driven by 30
/// bones. The biggest cloth measured in the corpus and the one nearest the
/// 256-particle ceiling a `u8` vertex index imposes.
fs::path KaelPath() {
    return CorpusRoot() / "HotSM3" / "Storm_Hero_Kaelthas_Base.m3";
}

/// Leoric's cape: 169 particles, seven colliders, and — unlike Kael'thas — a
/// `PHAC` whose visible vertices are mostly single-influence, so a rewrite that
/// drops a slot shows up as geometry rather than as a soft blend.
fs::path LeoricPath() {
    return CorpusRoot() / "HotSM3" / "Storm_Hero_KingLeoric_Base.m3";
}

std::vector<whiteout::u8> ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool LoadModel(const fs::path& p, whiteout::m3::Model& out) {
    if (!fs::exists(p))
        return false;
    const auto bytes = ReadAll(p);
    if (bytes.empty())
        return false;
    whiteout::m3::Parser parser;
    out = parser.parse(bytes);
    return !out.bones.empty();
}

/// The pose an actor gets on frame 0: bind pose, no clips.
FrameState BindPose(const M3ModelAdapter& adapter) {
    whiteout::flakes::PoseRequest req;
    return const_cast<M3ModelAdapter&>(adapter).Evaluate(req);
}

PoseStageContext Ctx(const std::vector<whiteout::i32>& parents, whiteout::i32 dtMs) {
    PoseStageContext ctx;
    ctx.nodeParents = parents;
    ctx.frameDtMs = dtMs;
    return ctx;
}

Vector3f OriginOf(const Matrix44f& m) {
    return {m.data[3][0], m.data[3][1], m.data[3][2]};
}

f32 Dist(const Vector3f& a, const Vector3f& b) {
    const f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

TEST_CASE("the cloth build agrees with the regions its PHAC records name", "[m3][cloth][corpus]") {
    whiteout::m3::Model model;
    if (!LoadModel(KaelPath(), model)) {
        SKIP("corpus model not present: " + KaelPath().string());
    }
    REQUIRE(model.clothPhysics.size() == 1);

    const sc2::Sc2ClothBuild build = sc2::Sc2BuildCloth(model);
    REQUIRE(build.pieces.size() == 1);
    const auto& piece = build.pieces[0];

    // The region the chunk names is the region the proxy names, and it is the
    // one flagged ClothSimulated. Three sources, one answer — which is the
    // check `clothMeshCount`'s name would otherwise fail.
    const auto& div = model.divisions[0];
    REQUIRE(piece.simRegion < div.regions.size());
    const auto simFlags = static_cast<whiteout::u32>(div.regions[piece.simRegion].flags);
    CHECK((simFlags & 0x4u) != 0u);
    REQUIRE(model.clothPhysics[0].proxies.size() == 1);
    CHECK(model.clothPhysics[0].proxies[0].clothIndex == piece.simRegion);

    REQUIRE(piece.influencedRegions.size() == 1);
    const auto infFlags =
        static_cast<whiteout::u32>(div.regions[piece.influencedRegions[0]].flags);
    CHECK((infFlags & 0x8u) != 0u);

    // Every simulated vertex becomes a particle: the build only drops one when
    // the mesh is degenerate, and shipped cloth is not.
    CHECK(piece.particleCount == div.regions[piece.simRegion].vertexCount);
    CHECK(piece.restPositions.size() == piece.particleCount);
    CHECK(piece.oldToNew.size() == piece.particleCount);
    CHECK(build.particleCount == piece.particleCount);

    // `simEnabled` bit 0 is *movable*, so the pinned count is the number of
    // vertices with it clear. Inverting the sense passes every other case here.
    std::size_t authoredPinned = 0;
    for (whiteout::u8 f : model.clothPhysics[0].simEnabled)
        if ((f & 1u) == 0u)
            ++authoredPinned;
    CHECK(piece.def.pinnedCount == authoredPinned);
    CHECK(authoredPinned > 0);
    CHECK(authoredPinned < piece.particleCount);

    // Both colliders survive classification. A capsule rejected as degenerate
    // is compacted out by the build, so a wrong scale recipe shows up as a
    // missing collider before it shows up as cloth through the body.
    CHECK(piece.def.capsules.size() == model.clothPhysics[0].colliders.size());
    CHECK(piece.def.params.hasColliders);

    // Records exist for the three constraint families. A mesh that produced
    // distances but no bends would drape as a chain-mail sheet.
    CHECK(!piece.def.edges.empty());
    CHECK(!piece.def.bends.empty());
    CHECK(!piece.def.triangles.empty());

    // Gravity is the world's, scaled by the chunk — not the world's alone.
    CHECK(piece.def.params.gravity.z == Approx(-8.8f * model.clothPhysics[0].gravity));
    // The tuning block maps one-to-one and in order; three spot checks across
    // its three groups are enough to catch a shifted transcription.
    CHECK(piece.def.params.massMultiplier == Approx(model.clothPhysics[0].density));
    CHECK(piece.def.params.distanceStiffness[1] ==
          Approx(model.clothPhysics[0].horizontalStiffness));
    CHECK(piece.def.params.tetherScale == Approx(model.clothPhysics[0].skinOffset));
}

TEST_CASE("a settled cloth hangs, and its pinned particles do not move", "[m3][cloth][corpus]") {
    whiteout::m3::Model model;
    if (!LoadModel(KaelPath(), model)) {
        SKIP("corpus model not present: " + KaelPath().string());
    }
    M3ModelAdapter adapter(model);

    const auto skeleton = adapter.GetSkeleton();
    const std::size_t boneCount = model.bones.size();
    const sc2::Sc2ClothBuild build = sc2::Sc2BuildCloth(model);
    REQUIRE(build.particleCount > 0);
    // The palette grew by exactly one node per particle, and each one's inverse
    // bind is the translation that makes the seeded frame the identity.
    REQUIRE(static_cast<std::size_t>(skeleton.nodeCount) == boneCount + build.particleCount);
    REQUIRE(skeleton.inverseBindMatrices.size() == boneCount + build.particleCount);
    REQUIRE(skeleton.nodeParents.size() == boneCount + build.particleCount);
    CHECK(skeleton.nodeParents[boneCount] == -1);

    whiteout::flakes::renderer::animation::PoseStageList stages;
    adapter.CreatePoseStages(stages);
    REQUIRE(!stages.empty());

    FrameState fs = BindPose(adapter);
    REQUIRE(fs.boneWorldMatrices.size() == boneCount + build.particleCount);

    // The seed is the rest pose: node = translate(rest), so the composed offset
    // matrix is the identity and the cape draws exactly where it was modelled.
    const auto& piece = build.pieces[0];
    for (std::size_t k = 0; k < piece.particleCount; ++k) {
        const Vector3f seeded = OriginOf(fs.boneWorldMatrices[boneCount + k]);
        CHECK(Dist(seeded, piece.restPositions[k]) < 1e-4f);
    }

    const auto ctx = Ctx(skeleton.nodeParents, 16);
    // The first Run creates the cloth and runs the 50-step warm-up, so one call
    // is already ~0.85 s of settling.
    for (auto& stage : stages)
        stage->Run(fs, ctx);

    std::size_t moved = 0;
    std::size_t pinnedMoved = 0;
    f32 lowestDrop = 0.0f;
    for (std::size_t k = 0; k < piece.particleCount; ++k) {
        const Vector3f now = OriginOf(fs.boneWorldMatrices[boneCount + k]);
        const f32 d = Dist(now, piece.restPositions[k]);
        const bool pinned = k < piece.def.pinnedCount;
        if (pinned) {
            // A pinned particle is placed by its bones, and at bind pose those
            // put it back exactly where it was modelled. Any drift here is the
            // anchor skin disagreeing with the mesh it was built from.
            if (d > 1e-3f)
                ++pinnedMoved;
        } else {
            if (d > 1e-3f)
                ++moved;
            lowestDrop = (std::min)(lowestDrop, now.z - piece.restPositions[k].z);
        }
    }
    INFO("moved=" << moved << " pinnedMoved=" << pinnedMoved << " lowestDrop=" << lowestDrop);
    CHECK(pinnedMoved == 0);
    // Most of the free half has to have moved, and the cloth has to have fallen
    // rather than merely jittered.
    CHECK(moved * 2 > piece.particleCount - piece.def.pinnedCount);
    CHECK(lowestDrop < -0.05f);

    // And it has to stay finite. A NaN anywhere in the solver reaches the
    // palette as a vertex at infinity, which renders as the whole model
    // vanishing rather than as a broken cape.
    for (std::size_t k = 0; k < piece.particleCount; ++k) {
        const Matrix44f& m = fs.boneWorldMatrices[boneCount + k];
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c)
                REQUIRE(std::isfinite(m.data[r][c]));
    }
}

TEST_CASE("the cloth follows the skeleton it is anchored to", "[m3][cloth][corpus]") {
    whiteout::m3::Model model;
    if (!LoadModel(LeoricPath(), model)) {
        SKIP("corpus model not present: " + LeoricPath().string());
    }
    M3ModelAdapter adapter(model);
    const auto skeleton = adapter.GetSkeleton();
    const std::size_t boneCount = model.bones.size();
    const sc2::Sc2ClothBuild build = sc2::Sc2BuildCloth(model);
    REQUIRE(build.pieces.size() == 1);
    const auto& piece = build.pieces[0];

    whiteout::flakes::renderer::animation::PoseStageList stages;
    adapter.CreatePoseStages(stages);
    REQUIRE(!stages.empty());

    FrameState fs = BindPose(adapter);
    const auto ctx = Ctx(skeleton.nodeParents, 16);
    for (auto& stage : stages)
        stage->Run(fs, ctx);

    std::vector<Vector3f> settled(piece.particleCount);
    for (std::size_t k = 0; k < piece.particleCount; ++k)
        settled[k] = OriginOf(fs.boneWorldMatrices[boneCount + k]);

    // Now walk the whole skeleton sideways, which is what an animation that
    // moves the character does to every anchor at once. A cloth reading a stale
    // bind table would sit exactly where it was.
    const Vector3f shift{3.0f, 0.0f, 0.0f};
    for (std::size_t b = 0; b < boneCount; ++b) {
        fs.boneWorldMatrices[b].data[3][0] += shift.x;
        fs.boneWorldMatrices[b].data[3][1] += shift.y;
        fs.boneWorldMatrices[b].data[3][2] += shift.z;
    }
    for (int frame = 0; frame < 120; ++frame)
        for (auto& stage : stages)
            stage->Run(fs, ctx);

    std::size_t followed = 0;
    for (std::size_t k = 0; k < piece.particleCount; ++k) {
        const Vector3f now = OriginOf(fs.boneWorldMatrices[boneCount + k]);
        // Two seconds is long enough for the tethers to drag every particle
        // most of the way, without asking a swinging cape to be exact.
        if (Dist(now, {settled[k].x + shift.x, settled[k].y + shift.y, settled[k].z + shift.z}) <
            1.0f) {
            ++followed;
        }
    }
    INFO("followed=" << followed << " of " << piece.particleCount);
    CHECK(followed * 4 > piece.particleCount * 3);

    // The pinned half is not "mostly": it is placed outright, every frame.
    for (std::size_t k = 0; k < piece.def.pinnedCount; ++k) {
        const Vector3f now = OriginOf(fs.boneWorldMatrices[boneCount + k]);
        CHECK(Dist(now, {settled[k].x + shift.x, settled[k].y + shift.y, settled[k].z + shift.z}) <
              1e-3f);
    }
}

TEST_CASE("the influenced region is skinned to particles, not bones", "[m3][cloth][corpus]") {
    whiteout::m3::Model model;
    if (!LoadModel(LeoricPath(), model)) {
        SKIP("corpus model not present: " + LeoricPath().string());
    }
    M3ModelAdapter adapter(model);
    const std::size_t boneCount = model.bones.size();
    const sc2::Sc2ClothBuild build = sc2::Sc2BuildCloth(model);
    REQUIRE(build.pieces.size() == 1);
    const auto& piece = build.pieces[0];

    const auto meshes = adapter.GetMeshes();
    const auto weights = adapter.GetSkinWeights();
    REQUIRE(meshes.size() == weights.size());

    // Find the geoset the cloth drives, by the region index the build reported.
    const auto& div = model.divisions[0];
    int clothGeoset = -1;
    int proxyGeoset = -1;
    std::size_t seen = 0;
    for (std::size_t r = 0; r < div.regions.size(); ++r) {
        const auto& region = div.regions[r];
        if (region.vertexCount == 0 || region.indexCount == 0)
            continue;
        if (r == piece.influencedRegions[0])
            clothGeoset = static_cast<int>(seen);
        if (r == piece.simRegion)
            proxyGeoset = static_cast<int>(seen);
        ++seen;
    }
    REQUIRE(clothGeoset >= 0);
    REQUIRE(proxyGeoset >= 0);

    // Its palette is the particle nodes, in particle order.
    const auto& sw = weights[static_cast<std::size_t>(clothGeoset)];
    REQUIRE(sw.subsetNodeIndices.size() == piece.particleCount);
    CHECK(sw.subsetNodeIndices.front() ==
          static_cast<whiteout::i32>(boneCount + piece.firstParticle));
    CHECK(sw.subsetNodeIndices.back() ==
          static_cast<whiteout::i32>(boneCount + piece.firstParticle + piece.particleCount - 1));

    // And its vertices name slots in that palette, with the weights the `PHAC`
    // carries — not the bone indices the blob shipped with.
    const auto& mesh = meshes[static_cast<std::size_t>(clothGeoset)];
    const std::size_t stride = mesh.baked.stride;
    const std::size_t count = mesh.baked.data.size() / stride;
    REQUIRE(count == div.regions[piece.influencedRegions[0]].vertexCount);
    const auto& proxy = model.clothPhysics[piece.chunkIndex].proxies[0];
    std::size_t checked = 0;
    for (std::size_t v = 0; v < count; ++v) {
        const whiteout::u8* rec = mesh.baked.data.data() + v * stride;
        whiteout::u32 total = 0;
        for (int k = 0; k < 4; ++k) {
            const auto slot = static_cast<std::size_t>((proxy.proxyVertices[v] >> (16 * k)) &
                                                       0xFFFFu);
            const auto w = static_cast<whiteout::u8>((proxy.proxyWeights[v] >> (8 * k)) & 0xFFu);
            total += rec[12 + k];
            if (slot < piece.oldToNew.size() && piece.oldToNew[slot] >= 0) {
                CHECK(rec[16 + k] == static_cast<whiteout::u8>(piece.oldToNew[slot]));
                CHECK(rec[12 + k] == w);
                ++checked;
            } else {
                CHECK(rec[12 + k] == 0);
            }
        }
        // Whatever the rounding, a vertex has to end up somewhere.
        REQUIRE(total > 0);
    }
    // Leoric's cape averages a little over two live influences per vertex, and
    // every vertex has at least one — a rewrite that dropped a whole region
    // would leave this at zero.
    CHECK(checked >= count);

    // The proxy itself is taken out of the draw list. It is a collision shape
    // that happens to be geometry, and drawing it puts a grey sheet through the
    // cape it drives.
    const FrameState fs = BindPose(adapter);
    REQUIRE(fs.geosetHidden.size() == meshes.size());
    CHECK(fs.geosetHidden[static_cast<std::size_t>(proxyGeoset)] == 1);
    CHECK(fs.geosetHidden[static_cast<std::size_t>(clothGeoset)] == 0);
}

TEST_CASE("the example models build and settle", "[m3][cloth][corpus]") {
    // The models this feature is meant to be looked at on, asserted rather than
    // written down: each has to build, and each has to settle to a finite pose.
    // Two of them are the shapes the single-cloth cases cannot reach — Uther
    // and the Mengsk barracks carry *two* cloths each, so the second one's
    // particles start at a non-zero offset into the palette and every index
    // that is really "particle + firstParticle" gets a chance to be wrong.
    struct Example {
        const char* path;
        std::size_t cloths;
    };
    const Example examples[] = {
        {"HotSM3/Storm_Hero_Kaelthas_Base.m3", 1},   {"HotSM3/Storm_Hero_KingLeoric_Base.m3", 1},
        {"HotSM3/Storm_Hero_Artanis_Base.m3", 1},    {"HotSM3/Storm_Hero_Alexstrasza_Base.m3", 1},
        {"HotSM3/Storm_Hero_Anduin_Base.m3", 1},     {"HotSM3/Storm_Hero_Uther_Base.m3", 2},
        {"Sc2M3/Storm_Hero_Jaina_Base.m3", 1},       {"Sc2M3/Ghost_Heavens_COOP.m3", 1},
        {"Sc2M3/Barracks_Mengsk_COOP.m3", 2},
    };

    std::size_t ran = 0;
    for (const Example& ex : examples) {
        const fs::path path = CorpusRoot() / ex.path;
        whiteout::m3::Model model;
        if (!LoadModel(path, model))
            continue;
        ++ran;
        INFO(ex.path);

        M3ModelAdapter adapter(model);
        const auto skeleton = adapter.GetSkeleton();
        const std::size_t boneCount = model.bones.size();
        const sc2::Sc2ClothBuild build = sc2::Sc2BuildCloth(model);
        CHECK(build.pieces.size() == ex.cloths);
        if (build.pieces.empty())
            continue;
        CHECK(static_cast<std::size_t>(skeleton.nodeCount) == boneCount + build.particleCount);

        // The pieces tile the palette end to end, with no gap and no overlap.
        std::size_t expect = 0;
        for (const auto& piece : build.pieces) {
            CHECK(piece.firstParticle == expect);
            CHECK(piece.particleCount > 0);
            CHECK(!piece.influencedRegions.empty());
            expect += piece.particleCount;
        }
        CHECK(expect == build.particleCount);

        whiteout::flakes::renderer::animation::PoseStageList stages;
        adapter.CreatePoseStages(stages);
        REQUIRE(!stages.empty());
        FrameState fs = BindPose(adapter);
        const auto ctx = Ctx(skeleton.nodeParents, 16);
        for (int frame = 0; frame < 30; ++frame)
            for (auto& stage : stages)
                stage->Run(fs, ctx);

        std::size_t moved = 0;
        for (std::size_t k = 0; k < build.particleCount; ++k) {
            const Matrix44f& m = fs.boneWorldMatrices[boneCount + k];
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 3; ++c)
                    REQUIRE(std::isfinite(m.data[r][c]));
            if (Dist(OriginOf(m), OriginOf(Matrix44f::identity())) > 0.0f)
                ++moved;
        }
        // Every particle sits somewhere, and something in the model hangs.
        CHECK(moved > 0);
    }
    if (ran == 0)
        SKIP("no corpus models present under " + CorpusRoot().string());
    INFO("ran " << ran << " of " << std::size(examples));
    CHECK(ran > 0);
}

TEST_CASE("PHCL's active channel gates the simulation", "[m3][cloth][corpus]") {
    // `active` is not decoration: 69 of the corpus's 216 mesh-carrying cloths
    // key it, and in StarCraft II reading zero skips the cloth's whole
    // per-vertex pass — the mesh keeps the pose it last had. Asserted on the
    // flag rather than on a model that keys it, because the two questions are
    // separate: does the adapter *sample* the channel (corpus), and does the
    // stage *obey* it (here, by writing the flag directly).
    whiteout::m3::Model model;
    if (!LoadModel(KaelPath(), model)) {
        SKIP("corpus model not present: " + KaelPath().string());
    }
    M3ModelAdapter adapter(model);
    const auto skeleton = adapter.GetSkeleton();
    const std::size_t boneCount = model.bones.size();
    const sc2::Sc2ClothBuild build = sc2::Sc2BuildCloth(model);
    REQUIRE(build.pieces.size() == 1);
    const auto& piece = build.pieces[0];

    // Sampled by the source, so a stage that never runs still sees the state.
    FrameState fs = BindPose(adapter);
    REQUIRE(fs.clothActive.size() == model.clothPhysics.size());
    // Kael'thas's own flag bit 1 is clear, so his cloth is on unconditionally —
    // which is what makes him a fair fixture for the *stage* half.
    CHECK(fs.clothActive[0] == 1);

    whiteout::flakes::renderer::animation::PoseStageList stages;
    adapter.CreatePoseStages(stages);
    REQUIRE(!stages.empty());
    const auto ctx = Ctx(skeleton.nodeParents, 16);
    for (auto& stage : stages)
        stage->Run(fs, ctx);

    std::vector<Vector3f> settled(piece.particleCount);
    for (std::size_t k = 0; k < piece.particleCount; ++k)
        settled[k] = OriginOf(fs.boneWorldMatrices[boneCount + k]);

    // Off: the skeleton moves and the cloth does not follow. It also does not
    // vanish — the palette is still written, with the frames it last had.
    fs.clothActive[0] = 0;
    for (std::size_t b = 0; b < boneCount; ++b)
        fs.boneWorldMatrices[b].data[3][2] += 5.0f;
    for (int frame = 0; frame < 60; ++frame)
        for (auto& stage : stages)
            stage->Run(fs, ctx);
    for (std::size_t k = 0; k < piece.particleCount; ++k)
        CHECK(Dist(OriginOf(fs.boneWorldMatrices[boneCount + k]), settled[k]) < 1e-4f);

    // Back on: it catches up. Sixty frames is a second, which is long past the
    // point where a pinned particle is placed outright.
    fs.clothActive[0] = 1;
    for (int frame = 0; frame < 60; ++frame)
        for (auto& stage : stages)
            stage->Run(fs, ctx);
    std::size_t caught = 0;
    for (std::size_t k = 0; k < piece.def.pinnedCount; ++k) {
        if (Dist(OriginOf(fs.boneWorldMatrices[boneCount + k]),
                 {settled[k].x, settled[k].y, settled[k].z + 5.0f}) < 1e-3f) {
            ++caught;
        }
    }
    CHECK(caught == piece.def.pinnedCount);
}

TEST_CASE("the visible cape deforms, and stays a cape", "[m3][cloth][corpus]") {
    // The one case that runs the *whole* chain the way the GPU does: build the
    // offset matrices the shader would get (`inverseBind * node`), skin the
    // visible region's vertices through them by hand, and look at the surface.
    //
    // Everything upstream can be right and this still fail. An inverse bind
    // that is not the exact inverse of the seed collapses the cape to a point
    // at bind pose; a rotation half left in the output frame doubles it;
    // particle indices off by the region's `firstVertex` shuffle the mesh into
    // noise while every particle is in the right place. None of those change a
    // single number the other cases assert on.
    whiteout::m3::Model model;
    if (!LoadModel(LeoricPath(), model)) {
        SKIP("corpus model not present: " + LeoricPath().string());
    }
    M3ModelAdapter adapter(model);
    const auto skeleton = adapter.GetSkeleton();
    const std::size_t boneCount = model.bones.size();
    const sc2::Sc2ClothBuild build = sc2::Sc2BuildCloth(model);
    REQUIRE(build.pieces.size() == 1);

    const auto meshes = adapter.GetMeshes();
    const auto weights = adapter.GetSkinWeights();
    const auto& div = model.divisions[0];
    int clothGeoset = -1;
    std::size_t seen = 0;
    for (std::size_t r = 0; r < div.regions.size(); ++r) {
        if (div.regions[r].vertexCount == 0 || div.regions[r].indexCount == 0)
            continue;
        if (r == build.pieces[0].influencedRegions[0])
            clothGeoset = static_cast<int>(seen);
        ++seen;
    }
    REQUIRE(clothGeoset >= 0);
    const auto& mesh = meshes[static_cast<std::size_t>(clothGeoset)];
    const auto& sw = weights[static_cast<std::size_t>(clothGeoset)];
    const std::size_t stride = mesh.baked.stride;
    const std::size_t count = mesh.positions.size();

    // What the skinning shader computes, for one vertex.
    const auto Skin = [&](const FrameState& fs, std::size_t v) {
        const whiteout::u8* rec = mesh.baked.data.data() + v * stride;
        const Vector3f p = mesh.positions[v];
        Vector3f acc{0, 0, 0};
        f32 total = 0.0f;
        for (int k = 0; k < 4; ++k) {
            const f32 w = static_cast<f32>(rec[12 + k]) / 255.0f;
            if (w <= 0.0f)
                continue;
            const std::size_t slot = rec[16 + k];
            REQUIRE(slot < sw.subsetNodeIndices.size());
            const auto node = static_cast<std::size_t>(sw.subsetNodeIndices[slot]);
            REQUIRE(node < fs.boneWorldMatrices.size());
            const Matrix44f offset =
                skeleton.inverseBindMatrices[node] * fs.boneWorldMatrices[node];
            for (int c = 0; c < 3; ++c) {
                acc.data[c] += w * (p.x * offset.data[0][c] + p.y * offset.data[1][c] +
                                    p.z * offset.data[2][c] + offset.data[3][c]);
            }
            total += w;
        }
        return std::pair<Vector3f, f32>{acc, total};
    };

    FrameState fs = BindPose(adapter);
    // At the seed, the offset matrix of every particle node is the identity, so
    // the skin has to return the vertex it started from — scaled only by the
    // weight sum the exporter's u8 rounding left behind.
    for (std::size_t v = 0; v < count; ++v) {
        const auto [skinned, total] = Skin(fs, v);
        REQUIRE(total > 0.9f);
        const Vector3f expect{mesh.positions[v].x * total, mesh.positions[v].y * total,
                              mesh.positions[v].z * total};
        CHECK(Dist(skinned, expect) < 1e-3f);
    }

    whiteout::flakes::renderer::animation::PoseStageList stages;
    adapter.CreatePoseStages(stages);
    REQUIRE(!stages.empty());
    const auto ctx = Ctx(skeleton.nodeParents, 16);
    for (auto& stage : stages)
        stage->Run(fs, ctx);

    // Now the surface. Edge lengths are the test: a cape that hangs is a cape
    // whose triangles kept their size, and every way of getting the palette
    // wrong stretches or collapses them.
    f32 maxMove = 0.0f;
    f32 worstEdgeRatio = 1.0f;
    std::size_t movedVerts = 0;
    std::vector<Vector3f> now(count);
    for (std::size_t v = 0; v < count; ++v) {
        const auto [skinned, total] = Skin(fs, v);
        now[v] = {skinned.x / total, skinned.y / total, skinned.z / total};
        const f32 d = Dist(now[v], mesh.positions[v]);
        maxMove = (std::max)(maxMove, d);
        if (d > 1e-3f)
            ++movedVerts;
        for (int c = 0; c < 3; ++c)
            REQUIRE(std::isfinite(now[v].data[c]));
    }
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const std::size_t a = mesh.indices[i], b = mesh.indices[i + 1];
        const f32 rest = Dist(mesh.positions[a], mesh.positions[b]);
        if (rest < 1e-4f)
            continue;
        worstEdgeRatio = (std::max)(worstEdgeRatio, Dist(now[a], now[b]) / rest);
    }
    INFO("movedVerts=" << movedVerts << "/" << count << " maxMove=" << maxMove
                       << " worstEdgeRatio=" << worstEdgeRatio);
    // It moved, it moved by a cape's worth rather than a model's worth, and no
    // edge tore.
    CHECK(movedVerts * 2 > count);
    CHECK(maxMove > 0.05f);
    CHECK(maxMove < 5.0f);
    CHECK(worstEdgeRatio < 3.0f);
}
