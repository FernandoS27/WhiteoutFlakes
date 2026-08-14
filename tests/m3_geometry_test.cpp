// ============================================================================
// M3 geometry (REFACTOR_PLAN.md P10) — the untextured-white claim, checked.
//
// Runs against the extracted corpora under `C:/Projects/WhiteoutLib/Corpus`
// (Sc2M3, Sc2BetaM3, StarM3, HotSM3); override the root with
// WDX_TEST_SC2_CORPUS. `.m3` is one self-contained file, so unlike `.m2` there
// are no siblings to resolve and no content provider in the picture.
//
// The plan wanted a committed fixture; its own risk register named "skip when
// no corpus is configured" as the fallback, because these files cannot be
// committed for licensing reasons. That is what this does. Skipped is not
// passed.
//
// The corpus is ~56,000 models, which is far more than a unit test should walk
// on every run. WDX_TEST_M3_LIMIT caps the count per corpus (default 400,
// deterministically the first N in sorted order); set it to 0 for the whole
// set. What was skipped is printed, because a bounded sweep that reads like
// full coverage is worse than an honest partial one.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/m3/m3_model_adapter.h"
#include "whiteout/flakes/content_ref.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using whiteout::flakes::ContentRef;
namespace io = whiteout::flakes::io;
namespace fs = std::filesystem;

namespace {

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_SC2_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus");
}

// The four extracted sets, each a separate game/build generation. Listed
// rather than globbed so an unrelated corpus directory appearing next to them
// does not silently join the sweep.
constexpr const char* kCorpora[] = {"Sc2M3", "Sc2BetaM3", "StarM3", "HotSM3"};

std::size_t PerCorpusLimit() {
    if (const char* v = std::getenv("WDX_TEST_M3_LIMIT"); v && *v)
        return static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
    return 400;
}

std::vector<fs::path> FindModels(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (!it->is_regular_file(ec))
            continue;
        auto ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".m3")
            out.push_back(it->path());
    }
    // Deterministic order, so a failure names the same model on every run and
    // the limit takes the same slice.
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<whiteout::u8> ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return {};
    return std::vector<whiteout::u8>((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("corpus .m3 files load geometry", "[m3]") {
    const fs::path root = CorpusRoot();
    const std::size_t limit = PerCorpusLimit();

    std::size_t loaded = 0, noVertices = 0, noDrawable = 0, hadRegions = 0, regnBelowV3 = 0,
                considered = 0, skippedByLimit = 0;
    // Extent statistics, which is what the profile's WorldScale is set from.
    // Reported rather than asserted: there is no correct answer to assert, only
    // a number that has to come from data instead of from a guess.
    float minExtent = 0.0f, maxExtent = 0.0f;
    double sumExtent = 0.0;
    std::size_t extentCount = 0;
    std::string smallest, largest;

    for (const char* name : kCorpora) {
        auto models = FindModels(root / name);
        if (models.empty())
            continue;
        if (limit > 0 && models.size() > limit) {
            skippedByLimit += models.size() - limit;
            models.resize(limit);
        }

        for (const auto& path : models) {
            ++considered;
            const std::string utf8 = path.string();
            INFO("model " << utf8);

            const auto bytes = ReadAll(path);
            REQUIRE_FALSE(bytes.empty());

            auto adapter = io::M3ModelAdapter::Load(ContentRef::FromPath(utf8),
                                                    std::span<const whiteout::u8>(bytes));
            if (!adapter) {
                // Effect-only and physics-only `.m3` carry no vertex buffer at
                // all — beams, splats, decals, camera rigs. A real and common
                // shape of the format, not a failure.
                ++noVertices;
                continue;
            }

            const auto meshes = adapter->GetMeshes();
            if (meshes.empty()) {
                // Vertices, but no region carried a face range. Split out and
                // attributed by REGN version because the cause is a known
                // parser gap, not a property of these models: below v3
                // WhiteoutLib never reads firstIndex/indexCount. Keeping the
                // tally here is what turns "some models don't draw" into a
                // number someone can fix against.
                ++noDrawable;
                const auto& m = adapter->SourceModel();
                if (!m.divisions.empty() && !m.divisions[0].regions.empty()) {
                    ++hadRegions;
                    if (m.divisions[0].regions[0].getVersion() < 3)
                        ++regnBelowV3;
                }
                continue;
            }

            std::size_t totalVerts = 0, totalIndices = 0;
            for (const auto& m : meshes) {
                CHECK_FALSE(m.positions.empty());
                CHECK(m.indices.size() % 3 == 0);
                // The claim the header makes and this asserts: DIV_.faces
                // entries are region-LOCAL. If they were global, every region
                // past the first would index beyond its own vertex slice and
                // this fires, naming the model.
                for (auto idx : m.indices)
                    REQUIRE(idx < m.positions.size());
                // Normals and UVs used to be zero-filled to match, because the
                // upload path re-interleaved all three arrays. They are in the
                // baked buffer now and the arrays are gone; mesh_buffer_test
                // checks the buffer itself against the parser's decode.
                CHECK(m.baked.Valid());
                CHECK(m.baked.VertexCount() == m.positions.size());
                totalVerts += m.positions.size();
                totalIndices += m.indices.size();
            }
            CHECK(totalVerts > 0);
            CHECK(totalIndices > 0);

            // Bounds are what camera framing measures against; a degenerate
            // box is exactly the failure that renders as a sub-pixel dot.
            const auto b = adapter->GetBounds();
            REQUIRE(b.valid);
            const float sx = b.max.x - b.min.x;
            const float sy = b.max.y - b.min.y;
            const float sz = b.max.z - b.min.z;
            CHECK((sx > 0.0f || sy > 0.0f || sz > 0.0f));

            const float extent = std::max({sx, sy, sz});
            if (extentCount == 0 || extent < minExtent) {
                minExtent = extent;
                smallest = path.filename().string();
            }
            if (extentCount == 0 || extent > maxExtent) {
                maxExtent = extent;
                largest = path.filename().string();
            }
            sumExtent += extent;
            ++extentCount;
            ++loaded;
        }
    }

    if (considered == 0) {
        SKIP("no .m3 files under " + root.string() +
             " (set WDX_TEST_SC2_CORPUS to point elsewhere)");
    }

    std::printf("[m3] %zu considered, %zu loaded, %zu without vertices, %zu with vertices but "
                "no drawable region (%zu had regions, %zu of those REGN v<3), %zu skipped by "
                "limit\n",
                considered, loaded, noVertices, noDrawable, hadRegions, regnBelowV3,
                skippedByLimit);
    // The gap is entirely REGN v<3, and this pins that. If a v3+ model ever
    // lands in this bucket the cause is something new and worth finding.
    CHECK(regnBelowV3 == hadRegions);
    if (extentCount > 0) {
        std::printf("[m3] extents: min %.2f (%s), max %.2f (%s), mean %.2f over %zu models\n",
                    static_cast<double>(minExtent), smallest.c_str(),
                    static_cast<double>(maxExtent), largest.c_str(),
                    sumExtent / static_cast<double>(extentCount), extentCount);
    }
    // Not every `.m3` has a mesh, but if none of them did the adapter would be
    // returning null for reasons unrelated to the format's shape.
    CHECK(loaded > 0);
}
