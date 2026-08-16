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
#include "whiteout/flakes/pose_request.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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

// ---------------------------------------------------------------------------
// The pre-Legion container.
//
// Everything above is chunked MD21, because that is all retail ships. A client
// from Warlords or earlier — and the Warlords client on this machine is one —
// stores the same MD20 payload with no chunk wrapper, and therefore no SFID
// naming its `.skin` files. The parser used to read skin profiles only out of
// SFID, so every such model parsed with no geometry at all and the adapter
// returned null.
//
// Stripping the wrapper off a corpus model reproduces that exactly: MD21's
// payload offsets are already relative to the chunk's data, so the payload
// *is* a valid MD20 file byte for byte. Same skins, same geometry, one branch
// apart.
// ---------------------------------------------------------------------------
TEST_CASE("a de-chunked MD20 loads the same geometry as its MD21 original", "[m2]") {
    const auto models = FindModels();
    if (models.empty()) {
        SKIP("no .m2 files under " + CorpusRoot().string() +
             " (set WDX_TEST_WOW_CORPUS to point elsewhere)");
    }

    // Smallest first: the container branch is what is under test, not the
    // model, and the fixture is a byte copy.
    std::vector<fs::path> bySize = models;
    std::error_code ec;
    std::sort(bySize.begin(), bySize.end(), [&](const fs::path& a, const fs::path& b) {
        return fs::file_size(a, ec) < fs::file_size(b, ec);
    });

    io::FileContentProvider provider;
    const fs::path temp = fs::temp_directory_path() / "wdx_md20_fixture";
    fs::remove_all(temp, ec);
    fs::create_directories(temp, ec);

    std::size_t checked = 0;
    for (const auto& path : bySize) {
        std::ifstream in(path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        if (bytes.size() < 8 || std::memcmp(bytes.data(), "MD21", 4) != 0)
            continue;
        std::uint32_t payload = 0;
        std::memcpy(&payload, bytes.data() + 4, 4);
        if (payload == 0 || payload + 8 > bytes.size())
            continue;

        const std::string stem = path.stem().string();
        const fs::path skin = path.parent_path() / (stem + "00.skin");
        if (!fs::exists(skin, ec))
            continue;

        INFO("model " << path.string());

        provider.SetBasePath(path.parent_path());
        auto original = provider.ReadFile(path.string());
        REQUIRE(original.has_value());
        auto chunked = io::M2ModelAdapter::Load(
            ContentRef::FromPath(path.string()),
            std::span<const whiteout::u8>(original->data(), original->size()), &provider);
        REQUIRE(chunked);
        // Smallest-first hits collision proxies and other one-submesh stubs
        // before it hits anything that would notice a mangled index rebase.
        if (chunked->SubmeshCount() < 8)
            continue;

        std::ofstream out(temp / (stem + ".m2"), std::ios::binary);
        out.write(bytes.data() + 8, static_cast<std::streamsize>(payload));
        out.close();
        // Every profile the header counts, not just the first — the loop under
        // test walks them positionally.
        for (int i = 0; i < 8; ++i) {
            char suffix[3] = {static_cast<char>('0' + i / 10), static_cast<char>('0' + i % 10), 0};
            const fs::path src = path.parent_path() / (stem + suffix + ".skin");
            if (fs::exists(src, ec))
                fs::copy_file(src, temp / src.filename(), fs::copy_options::overwrite_existing, ec);
        }

        const fs::path plainPath = temp / (stem + ".m2");
        provider.SetBasePath(temp);
        auto plainBytes = provider.ReadFile(plainPath.string());
        REQUIRE(plainBytes.has_value());
        auto plain = io::M2ModelAdapter::Load(
            ContentRef::FromPath(plainPath.string()),
            std::span<const whiteout::u8>(plainBytes->data(), plainBytes->size()), &provider);
        REQUIRE(plain);

        CHECK(plain->SubmeshCount() == chunked->SubmeshCount());
        const auto a = chunked->GetMeshes();
        const auto b = plain->GetMeshes();
        REQUIRE(b.size() == a.size());
        for (std::size_t i = 0; i < a.size(); ++i) {
            CHECK(b[i].positions.size() == a[i].positions.size());
            CHECK(b[i].indices.size() == a[i].indices.size());
        }
        ++checked;
        break;
    }
    fs::remove_all(temp, ec);
    CHECK(checked == 1);
}

// ---------------------------------------------------------------------------
// Re-dressing a live adapter.
//
// ModelLoader::RestyleWowModel changes a character's appearance without
// respawning the actor, and that rests on one claim about this adapter: a new
// geoset selection lands in the next Evaluate and leaves the geometry alone.
// If it did not — if the meshes were re-emitted, or the selection only took
// effect through GetMeshes — the loader would have to re-upload vertex buffers
// and the actor would have to be rebuilt after all.
//
// Character models are not in the corpus and the tables that name their choices
// are not reachable from one, so this drives the selection directly. The rules
// that *produce* a selection are covered device-free in wow_character_test.
// ---------------------------------------------------------------------------
TEST_CASE("a new geoset selection lands without touching the geometry", "[m2]") {
    const auto models = FindModels();
    if (models.empty()) {
        SKIP("no .m2 files under " + CorpusRoot().string() +
             " (set WDX_TEST_WOW_CORPUS to point elsewhere)");
    }

    io::FileContentProvider provider;

    std::size_t checked = 0;
    for (const auto& path : models) {
        provider.SetBasePath(path.parent_path());
        auto bytes = provider.ReadFile(path.string());
        REQUIRE(bytes.has_value());
        auto adapter = io::M2ModelAdapter::Load(
            ContentRef::FromPath(path.string()),
            std::span<const whiteout::u8>(bytes->data(), bytes->size()), &provider);
        REQUIRE(adapter);

        const auto before = adapter->GetMeshes();
        const auto sections = adapter->EmittedSkinSections();
        // Two submeshes in distinct groups, so "show one, hide the other" is a
        // selection the adapter can actually distinguish.
        if (before.size() < 2 || sections.size() != before.size())
            continue;
        if (sections.front() == sections.back())
            continue;

        INFO("model " << path.string());

        const whiteout::flakes::ClipRef clip{};
        const auto pose = whiteout::flakes::PoseRequest::OneClip(clip);

        // Nothing selected yet: an untouched model is a creature or a prop and
        // must not pay for this at all.
        CHECK(adapter->Evaluate(pose).geosetHidden.empty());

        adapter->SetVisibleGeosets({sections.front()});
        auto first = adapter->Evaluate(pose).geosetHidden;
        REQUIRE(first.size() == before.size());
        CHECK(first.front() == 0);
        CHECK(first.back() == 1);

        // The restyle: a second selection, on the same adapter, with no reload
        // in between.
        adapter->SetVisibleGeosets({sections.back()});
        auto second = adapter->Evaluate(pose).geosetHidden;
        REQUIRE(second.size() == before.size());
        CHECK(second.front() == 1);
        CHECK(second.back() == 0);

        // And the geometry the loader already uploaded is still the geometry
        // the adapter reports — same submeshes, same vertices, same indices.
        const auto after = adapter->GetMeshes();
        REQUIRE(after.size() == before.size());
        for (std::size_t i = 0; i < before.size(); ++i) {
            CHECK(after[i].positions.size() == before[i].positions.size());
            CHECK(after[i].indices.size() == before[i].indices.size());
        }
        CHECK(adapter->EmittedSkinSections() == sections);
        ++checked;
        break;
    }
    CHECK(checked == 1);
}
