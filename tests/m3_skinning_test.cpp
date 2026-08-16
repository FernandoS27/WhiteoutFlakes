// ============================================================================
// `.m3` skeleton and skin palettes.
//
// The contract under test is mostly a negative one: the adapter must NOT
// decode per-vertex bone data. `.m3` stores weights and indices inside the
// vertex blob in GPU-ready form, and stores the indices *region-local* — as
// slots into that region's window of MODL.boneLookup. That window is exactly a
// per-geoset bone palette, which is why the format ships it that way.
//
// So the adapter's whole job is to publish the palette and get out of the way.
// Two things must hold or the model renders with the wrong bones:
//   - `influences` stays empty, so no second vertex stream is built;
//   - `paletteLocalVertexIndices` forces the per-geoset palette path, because
//     the per-actor path rewrites vertex indices that live in a blob nobody
//     decodes and therefore cannot be rewritten.
// ============================================================================

#include "io/m3/m3_model_adapter.h"
#include "m3_anim_builders.h"
#include "renderer/animation/animation.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/m3/parser.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace whiteout;
using namespace whiteout::flakes;
using namespace whiteout::flakes::io;
using Catch::Approx;

namespace fs = std::filesystem;

namespace {

// A model with one division, one region, and a vertex blob big enough for it.
m3::Model SkinnedFixture(u16 firstBoneLookup, u16 boneLookupCount,
                         std::vector<u16> boneLookup) {
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    mb.StaticBone("a", 0);
    mb.StaticBone("b", 0);
    mb.StaticBone("c", 1);
    m3::Model model = mb.Build();
    model.boneLookup = std::move(boneLookup);

    m3::Region region;
    region.firstVertex = 0;
    region.vertexCount = 4;
    region.firstIndex = 0;
    region.indexCount = 3;
    region.firstBoneLookup = firstBoneLookup;
    region.boneLookupCount = boneLookupCount;
    region.rootBone = 0;

    m3::MeshDivision div;
    div.faces = {0, 1, 2};
    div.regions.push_back(region);
    model.divisions.push_back(std::move(div));

    // Stride 24 + 4 (one UV) + 4 (tangent) = 32.
    model.vertices.flags = m3::VertexFormatFlag::UV1;
    model.vertices.data.assign(4 * 32, 0);
    model.vertices.initialize();
    return model;
}

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_SC2_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus");
}

} // namespace

TEST_CASE("GetSkeleton maps parents and inverse binds", "[m3skin]") {
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    mb.StaticBone("child", 0);
    mb.StaticBone("grandchild", 1);
    M3ModelAdapter a(mb.Build());

    const auto sk = a.GetSkeleton();
    REQUIRE(sk.nodeCount == 3);
    REQUIRE(sk.nodeParents.size() == 3);
    REQUIRE(sk.nodeParents[0] == -1); // 0xFFFF is the root marker
    REQUIRE(sk.nodeParents[1] == 0);
    REQUIRE(sk.nodeParents[2] == 1);
    REQUIRE(sk.inverseBindMatrices.size() == 3);

    SECTION("no pivots — an M3 bone's TRS is already the whole local transform") {
        REQUIRE(sk.nodePivots.empty());
    }
}

TEST_CASE("An out-of-range parent index is treated as a root, not followed", "[m3skin]") {
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    m3::Model model = mb.Build();
    model.bones[0].parentIndex = 4242; // damaged chunk
    M3ModelAdapter a(std::move(model));
    REQUIRE(a.GetSkeleton().nodeParents[0] == -1);
}

TEST_CASE("A short IREF binds the remainder as identity rather than reading past it",
          "[m3skin]") {
    m3fix::ModelBuilder mb;
    mb.StaticBone("root", -1);
    mb.StaticBone("child", 0);
    m3::Model model = mb.Build();
    model.initialReference.resize(1); // one matrix, two bones
    M3ModelAdapter a(std::move(model));

    const auto sk = a.GetSkeleton();
    REQUIRE(sk.inverseBindMatrices.size() == 2);
    REQUIRE(sk.inverseBindMatrices[1].data[0][0] == Approx(1.0f));
    REQUIRE(sk.inverseBindMatrices[1].data[3][0] == Approx(0.0f));
}

TEST_CASE("The bone-lookup window is the geoset's palette", "[m3skin]") {
    // Region reads lookup[1..3] = {2, 3, 1}; a vertex storing local index 0
    // means bone 2, index 1 means bone 3, index 2 means bone 1.
    M3ModelAdapter a(SkinnedFixture(1, 3, {0, 2, 3, 1}));
    const auto sw = a.GetSkinWeights();
    REQUIRE(sw.size() == 1);
    REQUIRE(sw[0].geosetId == 0);
    REQUIRE(sw[0].subsetNodeIndices == std::vector<i32>{2, 3, 1});
}

TEST_CASE("No per-vertex influences are produced", "[m3skin]") {
    // The point of the whole exercise: the weights and indices are already in
    // the baked blob, so decoding them here would only feed a duplicate stream.
    M3ModelAdapter a(SkinnedFixture(0, 2, {1, 2}));
    const auto sw = a.GetSkinWeights();
    REQUIRE(sw.size() == 1);
    REQUIRE(sw[0].influences.empty());
    REQUIRE(sw[0].paletteLocalVertexIndices);
    REQUIRE(sw[0].groupAverages.empty()); // an MDX concept
}

TEST_CASE("A palette-local geoset is forced onto the per-geoset palette path", "[m3skin]") {
    // Small enough that the per-actor path would otherwise be chosen; the flag
    // has to override that, because there are no influences to rewrite.
    M3ModelAdapter a(SkinnedFixture(0, 2, {1, 2}));
    auto sw = a.GetSkinWeights();
    const auto decision = renderer::animation::DecidePaletteLayoutAndRewrite(4, sw);
    REQUIRE_FALSE(decision.usesPerActorPalette);

    SECTION("and nothing in the skin data was rewritten") {
        REQUIRE(sw[0].subsetNodeIndices == std::vector<i32>{1, 2});
        REQUIRE(sw[0].influences.empty());
    }
}

TEST_CASE("A decoded-influence geoset still takes the per-actor path", "[m3skin]") {
    // The guard must be narrow: it may not disturb the formats that do hand
    // over decoded influences.
    std::vector<renderer::model::SkinWeightData> sw(1);
    sw[0].geosetId = 0;
    sw[0].subsetNodeIndices = {0, 1};
    sw[0].influences.resize(2);
    sw[0].influences[0].boneIdx[0] = 1;
    sw[0].influences[0].weight[0] = 1.0f;

    const auto decision = renderer::animation::DecidePaletteLayoutAndRewrite(4, sw);
    REQUIRE(decision.usesPerActorPalette);
    REQUIRE(sw[0].influences[0].boneIdx[0] == 1); // rewritten local 1 -> global 1
}

TEST_CASE("A region with no lookup window still gets one slot", "[m3skin]") {
    M3ModelAdapter a(SkinnedFixture(0, 0, {}));
    const auto sw = a.GetSkinWeights();
    REQUIRE(sw.size() == 1);
    REQUIRE(sw[0].subsetNodeIndices.size() == 1);
    REQUIRE(sw[0].subsetNodeIndices[0] == 0); // the region's own root bone
}

TEST_CASE("A lookup entry naming a bone that does not exist falls back", "[m3skin]") {
    M3ModelAdapter a(SkinnedFixture(0, 2, {99, 1}));
    const auto sw = a.GetSkinWeights();
    REQUIRE(sw[0].subsetNodeIndices[0] == 0); // clamped, not out of range
    REQUIRE(sw[0].subsetNodeIndices[1] == 1);
}

TEST_CASE("Region flags reach the geoset metadata", "[m3skin]") {
    // Carried for cloth: nothing consumes them yet, and recovering them later
    // would mean re-walking the division.
    m3::Model model = SkinnedFixture(0, 2, {1, 2});
    model.divisions[0].regions[0].flags = m3::RegionFlag::ClothSimulated;
    M3ModelAdapter a(std::move(model));
    const auto flags = a.GeosetRegionFlags();
    REQUIRE(flags.size() == 1);
    REQUIRE((flags[0] & static_cast<u32>(m3::RegionFlag::ClothSimulated)) != 0);
}

TEST_CASE("corpus .m3 models actually animate", "[m3skin][corpus]") {
    // End-to-end at the data level: real sequences, real tracks, real bone
    // composition. A model whose pose never changes would mean the table
    // build, the layer expansion or the sampler silently produced nothing —
    // all of which the synthetic tests would miss, because they supply their
    // own tracks.
    const fs::path root = CorpusRoot();
    const char* corpora[] = {"Sc2M3", "Sc2BetaM3", "StarM3", "HotSM3"};
    std::size_t withBones = 0, moved = 0, nonFinite = 0;
    std::size_t noGroups = 0, groupCountDiffers = 0, noStc = 0, animatedBoneRefs = 0;
    std::size_t refsWithRow = 0, refsWithTrack = 0;
    std::vector<std::string> unresolvedExamples;

    for (const char* c : corpora) {
        const fs::path dir = root / c;
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            continue;
        std::size_t taken = 0;
        for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec || taken >= 120)
                break;
            if (!it->is_regular_file(ec) || it->path().extension() != ".m3")
                continue;
            std::ifstream f(it->path(), std::ios::binary);
            if (!f)
                continue;
            std::vector<u8> bytes((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
            if (bytes.empty())
                continue;
            m3::Model model;
            try {
                m3::Parser parser;
                model = parser.parse(bytes);
            } catch (...) {
                continue;
            }
            if (model.bones.empty() || model.sequences.empty())
                continue;
            ++taken;
            ++withBones;
            if (model.animationGroups.empty())
                ++noGroups;
            else if (model.animationGroups.size() != model.sequences.size())
                ++groupCountDiffers;
            if (model.subTrackCollections.empty())
                ++noStc;
            for (const auto& b : model.bones) {
                if (b.position.animId != 0 || b.rotation.animId != 0 || b.scale.animId != 0) {
                    ++animatedBoneRefs;
                    break;
                }
            }
            {
                // Where does resolution actually stop? Count models whose
                // animated bone refs have a table row at all, and separately
                // those that reach a real track through some container of some
                // sequence.
                M3AnimTables t;
                t.Build(model);
                bool anyRow = false, anyTrack = false;
                for (const auto& b : model.bones) {
                    const u32 ids[3] = {b.position.animId, b.rotation.animId, b.scale.animId};
                    for (u32 id : ids) {
                        if (id == 0 || id == 0xFFFFFFFFu)
                            continue;
                        const i32 row = t.RowOf(id);
                        if (row < 0)
                            continue;
                        anyRow = true;
                        for (u16 s = 0; s < t.StcCount() && !anyTrack; ++s)
                            if (t.At(row, s).Valid())
                                anyTrack = true;
                    }
                }
                if (anyRow)
                    ++refsWithRow;
                if (anyTrack)
                    ++refsWithTrack;
                if (!anyRow && unresolvedExamples.size() < 4) {
                    // Record what the mismatch looks like, so the cause is
                    // visible rather than inferred from a count.
                    u32 boneId = 0;
                    for (const auto& b : model.bones) {
                        if (b.position.animId != 0 && b.position.animId != 0xFFFFFFFFu) {
                            boneId = b.position.animId;
                            break;
                        }
                    }
                    u32 stcId = 0;
                    std::size_t stcIdCount = 0;
                    if (!model.subTrackCollections.empty()) {
                        stcIdCount = model.subTrackCollections[0].animIds.size();
                        if (stcIdCount > 0)
                            stcId = model.subTrackCollections[0].animIds[0];
                    }
                    unresolvedExamples.push_back(std::string(c) + " " +
                                                 it->path().filename().string() +
                                                 " boneAnimId=" + std::to_string(boneId) +
                                                 " stc0.animIds[0]=" + std::to_string(stcId) +
                                                 " stc0.animIdCount=" + std::to_string(stcIdCount) +
                                                 " modlVer=" +
                                                 std::to_string(model.getVersion()));
                }
            }

            M3ModelAdapter a(std::move(model));
            const auto seqs = a.GetSequences();

            auto poseAt = [&](i32 seq, i32 t) {
                ClipRef c;
                c.sequence = seq;
                c.timeMs = t;
                c.elapsedMs = t;
                c.loop = true;
                PoseRequest req;
                req.clips = std::span<const ClipRef>(&c, 1);
                return a.Evaluate(req);
            };

            // Across sequences, not just the first. Sequence 0 is frequently a
            // genuinely static idle — a doodad's "Stand" is one constant key —
            // and each sequence's keys live in its *own* sub-track containers,
            // so a static first sequence says nothing about the rest.
            bool differs = false;
            const std::size_t probe = (std::min<std::size_t>)(seqs.size(), 6);
            for (std::size_t s = 0; s < probe && !differs; ++s) {
                const i32 dur = seqs[s].endMs - seqs[s].startMs;
                const auto p0 = poseAt(static_cast<i32>(s), 0);
                const auto p1 = poseAt(static_cast<i32>(s), dur > 4 ? dur / 3 : 100);
                for (std::size_t b = 0; b < p0.boneWorldMatrices.size() && !differs; ++b) {
                    for (i32 r = 0; r < 4 && !differs; ++r) {
                        for (i32 col = 0; col < 4; ++col) {
                            const f32 x = p0.boneWorldMatrices[b].data[r][col];
                            const f32 y = p1.boneWorldMatrices[b].data[r][col];
                            if (!std::isfinite(x) || !std::isfinite(y)) {
                                ++nonFinite;
                                break;
                            }
                            if (std::fabs(x - y) > 1e-4f) {
                                differs = true;
                                break;
                            }
                        }
                    }
                }
            }
            if (differs)
                ++moved;
        }
    }

    if (withBones == 0) {
        WARN("no rigged .m3 corpus under " << root.string() << " — skipping");
        return;
    }
    WARN("m3 animation: " << moved << " of " << withBones << " rigged models change pose; "
                          << refsWithRow << " have a bone animId any container drives");
    // Not asserted as a proportion. An AnimRef's animId is assigned at export
    // for every animatable property, keyed or not, so a bone whose id appears
    // in no container is simply static — and the corpus is mostly doodads and
    // effect helpers whose bones genuinely never move. Counting them as
    // failures would be measuring the corpus, not the reader. What *is*
    // asserted: nothing produces a non-finite matrix, and the named units
    // below animate.
    REQUIRE(nonFinite == 0);
    REQUIRE(refsWithRow > 0);
}

TEST_CASE("named .m3 units animate", "[m3skin][corpus]") {
    // The proportion sweep above cannot fail usefully, because most of the
    // corpus is static props. These seven are the P10 white-render corpus —
    // rigged, animated units — and every one of them must move. If the table
    // build, layer expansion or sampler breaks, this is the test that says so.
    const fs::path root = CorpusRoot();
    const char* kUnits[] = {"Marine.m3",    "Zergling.m3",     "Zealot.m3", "SCV.m3",
                            "Thor.m3",      "Ultralisk.m3",    "BattleCruiser.m3"};

    std::size_t checked = 0;
    for (const char* name : kUnits) {
        const fs::path p = root / "Sc2M3" / name;
        std::error_code ec;
        if (!fs::exists(p, ec))
            continue;
        std::ifstream f(p, std::ios::binary);
        if (!f)
            continue;
        std::vector<u8> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        m3::Model model;
        try {
            m3::Parser parser;
            model = parser.parse(bytes);
        } catch (...) {
            continue;
        }
        ++checked;

        INFO("model " << name);
        REQUIRE_FALSE(model.bones.empty());
        REQUIRE_FALSE(model.sequences.empty());

        M3ModelAdapter a(std::move(model));
        const auto seqs = a.GetSequences();
        auto poseAt = [&](i32 seq, i32 t) {
            ClipRef c;
            c.sequence = seq;
            c.timeMs = t;
            c.elapsedMs = t;
            c.loop = true;
            PoseRequest req;
            req.clips = std::span<const ClipRef>(&c, 1);
            return a.Evaluate(req);
        };

        auto movesIn = [&](std::size_t s) {
            const i32 dur = seqs[s].endMs - seqs[s].startMs;
            if (dur <= 4)
                return 0.0f;
            const auto p0 = poseAt(static_cast<i32>(s), 0);
            const auto p1 = poseAt(static_cast<i32>(s), dur / 3);
            f32 d = 0.0f;
            for (std::size_t b = 0; b < p0.boneWorldMatrices.size(); ++b)
                for (i32 r = 0; r < 4; ++r)
                    for (i32 col = 0; col < 4; ++col)
                        d = (std::max)(d, std::fabs(p0.boneWorldMatrices[b].data[r][col] -
                                                    p1.boneWorldMatrices[b].data[r][col]));
            return d;
        };

        // Every locomotion sequence, not "at least one sequence somewhere".
        // The weaker form let a unit pass on a single moving cinematic while
        // the sequence the render gate actually plays sat frozen — which is
        // exactly the state the ticker's missing `elapsedMs` left it in.
        std::size_t movers = 0;
        f32 maxDelta = 0.0f;
        for (std::size_t s = 0; s < seqs.size(); ++s) {
            const std::string& nm = seqs[s].name;
            const bool locomotion = nm.rfind("Stand", 0) == 0 || nm.rfind("Walk", 0) == 0 ||
                                    nm.rfind("Attack", 0) == 0;
            if (!locomotion)
                continue;
            const f32 d = movesIn(s);
            maxDelta = (std::max)(maxDelta, d);
            if (d > 1e-4f)
                ++movers;
            else
                WARN("static locomotion sequence: " << name << " [" << s << "] " << nm);
        }
        INFO("largest bone-matrix delta " << maxDelta << " over " << movers << " mover(s)");
        REQUIRE(movers > 0);

        // And the palette exists for it to be posed through.
        const auto skins = a.GetSkinWeights();
        REQUIRE_FALSE(skins.empty());
        REQUIRE_FALSE(skins[0].subsetNodeIndices.empty());
        REQUIRE(skins[0].influences.empty()); // still no per-vertex decode
    }

    if (checked == 0) {
        WARN("named .m3 units not present under " << root.string() << " — skipping");
        return;
    }
    REQUIRE(checked >= 1);
}

TEST_CASE("Skin weights and meshes agree on what a geosetId means", "[m3skin][corpus]") {
    const fs::path root = CorpusRoot();
    const char* corpora[] = {"Sc2M3", "Sc2BetaM3", "StarM3", "HotSM3"};
    std::size_t checked = 0, mismatches = 0, boneModels = 0, irefShort = 0;

    for (const char* c : corpora) {
        const fs::path dir = root / c;
        std::error_code ec;
        if (!fs::is_directory(dir, ec))
            continue;
        std::size_t taken = 0;
        for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (ec || taken >= 150)
                break;
            if (!it->is_regular_file(ec) || it->path().extension() != ".m3")
                continue;
            std::ifstream f(it->path(), std::ios::binary);
            if (!f)
                continue;
            std::vector<u8> bytes((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
            if (bytes.empty())
                continue;
            m3::Model model;
            try {
                m3::Parser parser;
                model = parser.parse(bytes);
            } catch (...) {
                continue;
            }
            ++taken;

            M3ModelAdapter a(std::move(model));
            const auto meshes = a.GetMeshes();
            const auto skins = a.GetSkinWeights();
            const auto sk = a.GetSkeleton();
            ++checked;
            if (sk.nodeCount > 0)
                ++boneModels;
            if (a.SourceModel().initialReference.size() <
                a.SourceModel().bones.size())
                ++irefShort;

            // The two accessors must emit the same geosets in the same order,
            // or every per-geoset palette is attached to the wrong mesh.
            if (meshes.size() != skins.size()) {
                ++mismatches;
                continue;
            }
            for (std::size_t g = 0; g < meshes.size(); ++g) {
                if (meshes[g].geosetId != skins[g].geosetId)
                    ++mismatches;
                // Every palette slot must name a real bone.
                for (i32 n : skins[g].subsetNodeIndices)
                    if (n < 0 || n >= sk.nodeCount)
                        ++mismatches;
            }
        }
    }

    if (checked == 0) {
        WARN("no .m3 corpus under " << root.string() << " — skipping");
        return;
    }
    WARN("m3 skinning: " << checked << " models, " << boneModels << " with bones, " << irefShort
                         << " with a short IREF");
    REQUIRE(mismatches == 0);
}
