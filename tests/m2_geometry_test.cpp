// ============================================================================
// M2 geometry (REFACTOR_PLAN.md P9) — the untextured-white claim, checked.
//
// Runs against the extracted corpus at
// `C:/Projects/WhiteoutLib/Corpus/WoW`, override with WDX_TEST_WOW_CORPUS.
// Loose `.m2` files with their `.skin` siblings beside them, which is the
// path-addressed route — a shipped install's fileDataIDs cannot be resolved
// without a listfile, so the corpus is what a test can actually reach.
//
// The plan wanted a committed fixture; its own risk register named "skip when
// no corpus is configured" as the fallback, because `.m2` files cannot be
// committed for licensing reasons. That is what this does. Skipped is not
// passed.
//
// Every `.m2` in the corpus is exercised, not one: the two-level index
// indirection through `skin.vertices` is the kind of thing that happens to
// work on a model whose first submesh starts at zero and breaks on the next.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/file_content_provider.h"
#include "io/m2/m2_model_adapter.h"
#include "whiteout/flakes/content_ref.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using whiteout::flakes::ContentRef;
namespace io = whiteout::flakes::io;
namespace fs = std::filesystem;

namespace {

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_WOW_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/WoW");
}

std::vector<fs::path> FindModels() {
    std::vector<fs::path> out;
    std::error_code ec;
    const fs::path root = CorpusRoot();
    if (!fs::is_directory(root, ec))
        return out;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (!it->is_regular_file(ec))
            continue;
        auto ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".m2")
            out.push_back(it->path());
    }
    // Deterministic order, so a failure names the same model on every run.
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace

TEST_CASE("every corpus .m2 loads geometry", "[m2]") {
    const auto models = FindModels();
    if (models.empty()) {
        SKIP("no .m2 files under " + CorpusRoot().string() +
             " (set WDX_TEST_WOW_CORPUS to point elsewhere)");
    }

    io::FileContentProvider provider;

    std::size_t loaded = 0;
    for (const auto& path : models) {
        // The provider resolves relative to its base path, and the parser asks
        // it for siblings by name — so the model's own directory is the root.
        provider.SetBasePath(path.parent_path());
        const std::string utf8 = path.string();
        INFO("model " << utf8);

        auto bytes = provider.ReadFile(utf8);
        REQUIRE(bytes.has_value());
        REQUIRE_FALSE(bytes->empty());

        auto adapter = io::M2ModelAdapter::Load(
            ContentRef::FromPath(utf8),
            std::span<const whiteout::u8>(bytes->data(), bytes->size()), &provider);
        // Null means no skin profile resolved — the `.skin` sibling was not
        // found. That is the whole point of the phase, so it fails rather than
        // skipping.
        REQUIRE(adapter);
        CHECK(adapter->SubmeshCount() > 0);

        const auto meshes = adapter->GetMeshes();
        REQUIRE_FALSE(meshes.empty());

        std::size_t totalVerts = 0, totalIndices = 0;
        for (const auto& m : meshes) {
            CHECK_FALSE(m.positions.empty());
            // Triangles, and every index inside its own submesh. The rebase
            // from profile-global to submesh-local is where an off-by-one
            // produces a silently mangled mesh rather than a crash, so this is
            // the assertion that actually earns its keep.
            CHECK(m.indices.size() % 3 == 0);
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

        // Bounds are what camera framing measures against; a degenerate box is
        // exactly the failure that renders as a sub-pixel dot.
        const auto b = adapter->GetBounds();
        REQUIRE(b.valid);
        const float sx = b.max.x - b.min.x;
        const float sy = b.max.y - b.min.y;
        const float sz = b.max.z - b.min.z;
        CHECK((sx > 0.0f || sy > 0.0f || sz > 0.0f));
        ++loaded;
    }
    CHECK(loaded == models.size());
}
