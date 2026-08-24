// ============================================================================
// D3 skinning — the rigid-first claim, and the palette that is not there.
//
// Measured over 300 `.app`: **4,372 sub-objects, of which 378 carry vertex
// weights.** The other 3,994 are rigid, positioned by `SubObject.nBoneIndex`.
// The rigid path is the common case and treating it as a fallback is backwards,
// so this sweep reports the split rather than assuming it.
//
// Two measurements that remove work, both re-checked here rather than trusted:
//
//  * **Influence bone indices are GLOBAL skeleton indices** — there is no
//    per-sub-object palette to remap and no `SkinningInfo` batching to
//    reconstruct. That is a runtime GPU-upload artifact, not something in the
//    file.
//  * **Weights already sum to 1.** The loader's normalisation is a no-op here;
//    it stays in as the invariant check it is.
//
// And one thing deliberately NOT inherited: the 45-bone draw split. The engine
// batches geometry because `matBones` is declared 135 float4 and the upload
// truncates past it. Our palette is not that constant buffer, and D3's real
// ceiling is 512 bones per pose.
//
// Corpus root: WDX_TEST_D3_CORPUS. Skipped is not passed.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/d3/d3_model_adapter.h"
#include "io/d3/d3_sno_cache.h"
#include "renderer/animation/animation.h"

#include <whiteout/sno/d3/native/d3_native.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace d3n = ::whiteout::sno::d3::native;
using namespace ::whiteout;
using ::whiteout::flakes::ContentRef;

namespace {

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_D3_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/D3");
}

std::size_t SweepLimit() {
    if (const char* v = std::getenv("WDX_TEST_D3_LIMIT"); v && *v)
        return static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
    return 300;
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

} // namespace

TEST_CASE("D3 corpus: rigid is the common case, and influences are global", "[d3][corpus]") {
    const auto files = FindFiles(CorpusRoot() / "Appearances", ".app");
    if (files.empty()) {
        WARN("No D3 corpus at " << (CorpusRoot() / "Appearances").string()
                                << " (set WDX_TEST_D3_CORPUS). SKIPPED, not passed.");
        return;
    }
    const std::size_t limit = SweepLimit();
    const std::size_t take = (limit == 0) ? files.size() : (std::min)(limit, files.size());

    std::size_t subObjects = 0, rigid = 0, skinned = 0;
    std::size_t influences = 0, outOfRange = 0, weightsOffUnity = 0;
    std::size_t maxBones = 0, over512 = 0;
    f32 worstWeightSum = 0.0f;

    for (std::size_t i = 0; i < take; ++i) {
        auto app = d3n::parseAppearances(ReadAll(files[i]));
        if (!app)
            continue;
        const i32 boneCount = static_cast<i32>(app->arBones.size());
        maxBones = (std::max)(maxBones, app->arBones.size());
        if (app->arBones.size() > 512)
            ++over512;

        const d3n::GeoSet* sets[2] = {&app->tGeoSet0, &app->tGeoSet1};
        for (const auto* set : sets) {
            for (const auto& sub : set->arSubObjects) {
                if (sub.arVertices.empty() || sub.arIndices.empty())
                    continue;
                ++subObjects;
                if (sub.arVertexInfluences.empty()) {
                    ++rigid;
                    continue;
                }
                ++skinned;
                for (const auto& inf : sub.arVertexInfluences) {
                    const d3n::Influence* three[3] = {&inf.tInfluence0, &inf.tInfluence1,
                                                      &inf.tInfluence2};
                    f32 sum = 0.0f;
                    for (const auto* one : three) {
                        ++influences;
                        if (one->flWeight <= 0.0f)
                            continue;
                        sum += one->flWeight;
                        if (one->nBoneIndex < 0 || one->nBoneIndex >= boneCount)
                            ++outOfRange;
                    }
                    if (std::fabs(sum - 1.0f) > 1e-3f) {
                        ++weightsOffUnity;
                        worstWeightSum = (std::max)(worstWeightSum, std::fabs(sum - 1.0f));
                    }
                }
            }
        }
    }

    const double rigidPct =
        subObjects ? (100.0 * static_cast<double>(rigid) / static_cast<double>(subObjects)) : 0.0;
    std::printf("[d3-skin] %zu/%zu files | %zu sub-objects: %zu rigid (%.1f%%), %zu skinned\n",
                take, files.size(), subObjects, rigid, rigidPct, skinned);
    std::printf("[d3-skin] %zu influence slots, %zu out of range | %zu vertices whose weights do "
                "not sum to 1 (worst |err| %.6f)\n",
                influences, outOfRange, weightsOffUnity, static_cast<double>(worstWeightSum));
    std::printf("[d3-skin] largest skeleton %zu bones, %zu appearances past the 512 ceiling\n",
                maxBones, over512);

    REQUIRE(subObjects > 0);
    // Global indices: an out-of-range one would mean there IS a per-sub-object
    // palette and the adapter is reading through it as if there were not.
    CHECK(outOfRange == 0);
    // Both halves exist in shipped content. A sweep reporting 100% rigid would
    // mean the influence arrays are not being read at all — which is exactly
    // what a wrong record stride looks like.
    CHECK(rigid > 0);
    CHECK(skinned > 0);
    // The measurement the rigid-first ordering rests on.
    CHECK(rigidPct > 50.0);
    // 512 bones native. Reported rather than enforced: a model past it is data
    // we have not seen, and the palette would be the thing to fix.
    CHECK(over512 == 0);
}

TEST_CASE("D3 adapter: what GetSkinWeights hands the loader", "[d3][corpus]") {
    const auto files = FindFiles(CorpusRoot() / "Appearances", ".app");
    if (files.empty()) {
        WARN("No D3 corpus. SKIPPED, not passed.");
        return;
    }
    flakes::io::D3SnoCache cache(nullptr);
    const std::size_t take = std::min<std::size_t>(60, files.size());

    std::size_t built = 0, geosets = 0, rigidGeosets = 0, mismatched = 0, laneThreeUsed = 0;
    std::size_t boneless = 0;
    for (std::size_t i = 0; i < take; ++i) {
        const auto bytes = ReadAll(files[i]);
        auto adapter = flakes::io::D3ModelAdapter::LoadAppearance(
            ContentRef::FromPath(files[i].string()), bytes, cache);
        if (!adapter)
            continue;
        ++built;

        const auto meshes = adapter->GetMeshes();
        const auto weights = adapter->GetSkinWeights();
        // A boneless appearance is a real thing — a static prop with no
        // skeleton — and it yields no weights at all, exactly as the MDX and M3
        // adapters do. The loader looks weights up by geoset id and simply
        // finds none, so the geoset draws on the actor's world transform.
        // Measured: 188 of the first 300 corpus files have no bones.
        if (weights.empty()) {
            ++boneless;
            continue;
        }
        // Otherwise: one entry per emitted geoset, in the same order — every
        // per-geoset accessor takes the same skips or `geosetId` stops meaning
        // one thing.
        REQUIRE(weights.size() == meshes.size());

        for (std::size_t g = 0; g < weights.size(); ++g) {
            ++geosets;
            CHECK(weights[g].geosetId == static_cast<i32>(g));
            // The loader builds the BoneVertex stream only when the influence
            // count matches the vertex count exactly, so a mismatch is a geoset
            // that would silently draw unskinned.
            const std::size_t verts = meshes[g].baked.VertexCount();
            if (weights[g].influences.size() != verts) {
                ++mismatched;
                continue;
            }
            // Global indices go to the loader untouched: `paletteLocalVertexIndices`
            // is what `.m3` sets, and setting it here would put D3's global
            // indices on the path that never remaps them.
            CHECK_FALSE(weights[g].paletteLocalVertexIndices);
            CHECK(weights[g].subsetNodeIndices.empty());

            bool anyMultiBone = false;
            for (const auto& inf : weights[g].influences) {
                f32 sum = 0.0f;
                for (int k = 0; k < 4; ++k)
                    sum += inf.weight[k];
                CHECK(std::fabs(sum - 1.0f) < 1e-3f);
                // D3 ships THREE influences. Lane 3 exists because the renderer's
                // BoneVertex has four; it must always be empty.
                if (inf.weight[3] != 0.0f)
                    ++laneThreeUsed;
                if (inf.weight[1] > 0.0f || inf.weight[2] > 0.0f)
                    anyMultiBone = true;
            }
            if (!anyMultiBone)
                ++rigidGeosets;
        }
    }

    std::printf("[d3-skin] %zu appearances built (%zu boneless) | %zu geosets (%zu single-bone), "
                "%zu influence/vertex mismatches, %zu lane-3 weights\n",
                built, boneless, geosets, rigidGeosets, mismatched, laneThreeUsed);
    REQUIRE(built > 0);
    CHECK(geosets > 0);
    CHECK(mismatched == 0);
    CHECK(laneThreeUsed == 0);
}

TEST_CASE("D3 adapter: the skeleton inverts pose B, not pose A", "[d3][corpus]") {
    const auto files = FindFiles(CorpusRoot() / "Appearances", ".app");
    if (files.empty()) {
        WARN("No D3 corpus. SKIPPED, not passed.");
        return;
    }
    flakes::io::D3SnoCache cache(nullptr);

    // Find a model that actually has divergent bind poses — on one where A and
    // B agree, reading the wrong field is indistinguishable from reading the
    // right one, so a test that took the first file would prove nothing.
    for (std::size_t i = 0; i < files.size() && i < 400; ++i) {
        const auto bytes = ReadAll(files[i]);
        auto app = d3n::parseAppearances(bytes);
        if (!app || app->arBones.empty())
            continue;

        std::size_t divergentBone = static_cast<std::size_t>(-1);
        for (std::size_t n = 0; n < app->arBones.size(); ++n) {
            const auto& t0 = app->arBones[n].tTransform0.vTranslation;
            const auto& t3 = app->arBones[n].tTransform3.vTranslation;
            const f32 d = std::fabs(t0.x - t3.x) + std::fabs(t0.y - t3.y) + std::fabs(t0.z - t3.z);
            if (d > 1e-3f) {
                divergentBone = n;
                break;
            }
        }
        if (divergentBone == static_cast<std::size_t>(-1))
            continue;

        auto adapter = flakes::io::D3ModelAdapter::LoadAppearance(
            ContentRef::FromPath(files[i].string()), bytes, cache);
        REQUIRE(adapter != nullptr);
        const auto sk = adapter->GetSkeleton();
        REQUIRE(sk.nodeCount == static_cast<i32>(app->arBones.size()));

        // The inverse bind is built from tTransform4 — the inverse of pose B —
        // and NOT from tTransform1, the inverse of pose A. On a divergent bone
        // the two differ by a real displacement, so the translation row is
        // enough to tell them apart.
        const auto& m = sk.inverseBindMatrices[divergentBone];
        const auto& want = app->arBones[divergentBone].tTransform4.vTranslation;
        const auto& wrong = app->arBones[divergentBone].tTransform1.vTranslation;
        const f32 dWant = std::fabs(m.data[3][0] - want.x) + std::fabs(m.data[3][1] - want.y) +
                          std::fabs(m.data[3][2] - want.z);
        const f32 dWrong = std::fabs(m.data[3][0] - wrong.x) + std::fabs(m.data[3][1] - wrong.y) +
                           std::fabs(m.data[3][2] - wrong.z);
        std::printf("[d3-skin] %s bone %zu: |palette - t4| = %.6f, |palette - t1| = %.6f\n",
                    files[i].filename().string().c_str(), divergentBone,
                    static_cast<double>(dWant), static_cast<double>(dWrong));
        CHECK(dWant < 1e-4f);
        CHECK(dWrong > 1e-3f);

        // The attachment frame is the OTHER one: hardpoints, trails and
        // particle attachments read pose A's inverse, which is why both are
        // carried rather than one being derived from the other.
        REQUIRE(adapter->AttachmentFrames().size() == app->arBones.size());
        const auto& att = adapter->AttachmentFrames()[divergentBone];
        const f32 dAtt = std::fabs(att.data[3][0] - wrong.x) + std::fabs(att.data[3][1] - wrong.y) +
                         std::fabs(att.data[3][2] - wrong.z);
        CHECK(dAtt < 1e-4f);
        return;
    }
    WARN("No appearance with divergent bind poses in the first 400 corpus files. "
         "SKIPPED, not passed.");
}

// ---------------------------------------------------------------------------
// The shared per-actor palette rewrite, which is where D3 skinning actually
// broke. `DecidePaletteLayoutAndRewrite` re-addresses `influences[].boneIdx`
// from geoset-local slots to global palette slots by looking each index up in
// `subsetNodeIndices`. D3 has no such subset — every sub-object addresses the
// whole skeleton, so its indices are ALREADY global and the vector is empty.
// Every index therefore missed the subset, missed the pseudo-slot map, and hit
// the out-of-range fallback that points a lane at node 0.
//
// The failure is silent and looks like success: the model draws, is fully
// textured, reports a healthy draw count and a stable trace, and simply rides
// the root bone forever. No count, no assert and no render gate sees it — only
// asking what the indices became does.
// ---------------------------------------------------------------------------
TEST_CASE("D3 global bone indices survive the per-actor palette rewrite", "[d3][skinning]") {
    const auto files = FindFiles(CorpusRoot() / "Appearances", ".app");
    if (files.empty()) {
        WARN("No D3 corpus at " << (CorpusRoot() / "Appearances").string()
                                << ". SKIPPED, not passed.");
        return;
    }

    flakes::io::D3SnoCache cache(nullptr);
    const std::size_t limit = SweepLimit();
    std::size_t checked = 0, nonZeroLanes = 0;

    for (std::size_t i = 0; i < files.size() && i < limit && checked < 8; ++i) {
        const auto bytes = ReadAll(files[i]);
        auto adapter = flakes::io::D3ModelAdapter::LoadAppearance(
            ContentRef::FromPath(files[i].string()), bytes, cache);
        if (!adapter)
            continue;
        const auto skel = adapter->GetSkeleton();
        if (skel.nodeCount <= 1)
            continue;
        auto weights = adapter->GetSkinWeights();
        if (weights.empty())
            continue;

        // Every geoset must claim global indices; otherwise the rewrite below
        // is entitled to renumber them and the rest of this proves nothing.
        for (const auto& sw : weights)
            REQUIRE(sw.globalVertexIndices);

        // Snapshot, rewrite, compare. Path A is what a D3 character takes.
        std::vector<std::vector<flakes::renderer::model::VertexInfluence>> before;
        before.reserve(weights.size());
        for (const auto& sw : weights)
            before.push_back(sw.influences);

        const auto decision = flakes::renderer::animation::DecidePaletteLayoutAndRewrite(
            skel.nodeCount, weights);
        if (!decision.usesPerActorPalette)
            continue; // Path B never rewrites; nothing to prove here.
        ++checked;

        for (std::size_t g = 0; g < weights.size(); ++g) {
            REQUIRE(weights[g].influences.size() == before[g].size());
            for (std::size_t v = 0; v < before[g].size(); ++v) {
                for (int k = 0; k < 4; ++k) {
                    const i32 was = before[g][v].boneIdx[k];
                    const i32 now = weights[g].influences[v].boneIdx[k];
                    if (was >= 0 && was < skel.nodeCount) {
                        REQUIRE(now == was);
                        if (now != 0)
                            ++nonZeroLanes;
                    }
                }
            }
        }
    }

    if (checked == 0) {
        WARN("No Path A appearance in the sweep. SKIPPED, not passed.");
        return;
    }
    std::printf("[d3-skin] palette rewrite: %zu Path A model(s), %zu lane(s) left off node 0\n",
                checked, nonZeroLanes);
    // The whole point: if the rewrite collapsed everything onto the root, this
    // is zero and the model would draw perfectly while never deforming.
    CHECK(nonZeroLanes > 0);
}
