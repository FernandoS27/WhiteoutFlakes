// ============================================================================
// MeshBuffer â€” does the layout description match the bytes it describes?
//
// This is the one thing the baked upload path can get wrong silently. The
// renderer takes an adapter at its word: it uploads `MeshBuffer::data` verbatim
// and builds an input layout from `MeshBuffer::attributes` without ever
// decoding a single attribute. If an offset, a format or a stride is wrong, no
// bounds check fires and no buffer overruns â€” the GPU just reads the wrong
// bytes and shades them confidently.
//
// So the assertions here decode the blob *independently*, using only the
// description, and compare against the parser's own accessors
// (`getPositions()`, `getNormals()`, and `m2::Vertex`). Structural checks â€”
// stride, size, in-bounds attributes â€” come first because they localise a
// failure; the value comparisons are what actually prove the description.
//
// Device-free, so this is a G0 test and runs in CI. It needs the same corpora
// m2_geometry_test / m3_geometry_test use and skips without them. Skipped is
// not passed.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "whiteout/flakes/model_types.h"

#if WDX_ENABLE_M2
#include "io/file_content_provider.h"
#include "io/m2/m2_model_adapter.h"
#endif
#if WDX_ENABLE_M3
#include "io/m3/m3_model_adapter.h"
#endif
#include "whiteout/flakes/content_ref.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using whiteout::flakes::ContentRef;
using whiteout::flakes::MeshBuffer;
using whiteout::flakes::VertexAttribute;
using whiteout::flakes::VertexSemantic;
namespace gfx = whiteout::flakes::gfx;
namespace io = whiteout::flakes::io;
namespace fs = std::filesystem;

namespace {

std::vector<fs::path> FindByExt(const fs::path& root, const char* ext) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return out;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (!it->is_regular_file(ec))
            continue;
        auto e = it->path().extension().string();
        std::transform(e.begin(), e.end(), e.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (e == ext)
            out.push_back(it->path());
    }
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

const VertexAttribute* Find(const MeshBuffer& b, VertexSemantic s, whiteout::u8 index = 0) {
    for (const auto& a : b.attributes)
        if (a.semantic == s && a.semanticIndex == index)
            return &a;
    return nullptr;
}

// Every attribute has to fit inside one record. This is the cheap check that
// catches a stride computed from the wrong flags, and it is worth running on
// every mesh because it is O(attributes) rather than O(vertices).
void CheckStructure(const MeshBuffer& b) {
    REQUIRE(b.stride > 0);
    REQUIRE_FALSE(b.attributes.empty());
    REQUIRE(b.data.size() % b.stride == 0);
    for (const auto& a : b.attributes) {
        INFO("attribute semantic " << static_cast<int>(a.semantic) << " index "
                                   << static_cast<int>(a.semanticIndex));
        const whiteout::u32 size = gfx::FormatBytesPerBlock(a.format);
        REQUIRE(size > 0); // Unknown format = an attribute nothing can bind
        REQUIRE(a.offset + size <= b.stride);
    }
}

whiteout::Vector3f ReadPosition(const MeshBuffer& b, std::size_t v, whiteout::u16 offset) {
    whiteout::Vector3f p{};
    std::memcpy(&p, b.data.data() + v * b.stride + offset, sizeof(p));
    return p;
}

bool NearlyEqual(float a, float b, float eps) {
    return std::fabs(a - b) <= eps;
}

} // namespace

#if WDX_ENABLE_M2
namespace {
fs::path WowCorpus() {
    if (const char* v = std::getenv("WDX_TEST_WOW_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/WoW");
}
} // namespace

TEST_CASE("m2 baked buffers describe their own bytes", "[m2][meshbuffer]") {
    const auto models = FindByExt(WowCorpus(), ".m2");
    if (models.empty()) {
        SKIP("no .m2 files under " + WowCorpus().string() +
             " (set WDX_TEST_WOW_CORPUS to point elsewhere)");
    }

    io::FileContentProvider provider;
    // A Legion-or-later `.m2` names its skins by fileDataID, which only a WoW
    // storage resolves; the provider defaults to Warcraft III, where those ids
    // mean nothing and the model reads as unloadable.
    provider.SetGame(whiteout::flakes::ProductId::Wow);
    std::size_t checked = 0;

    for (const auto& path : models) {
        provider.SetBasePath(path.parent_path());
        const std::string utf8 = path.string();
        INFO("model " << utf8);

        auto bytes = provider.ReadFile(utf8);
        REQUIRE(bytes.has_value());
        auto adapter = io::M2ModelAdapter::Load(
            ContentRef::FromPath(utf8),
            std::span<const whiteout::u8>(bytes->data(), bytes->size()), &provider);
        REQUIRE(adapter);

        for (const auto& mesh : adapter->GetMeshes()) {
            const MeshBuffer& b = mesh.baked;
            REQUIRE(b.Valid());
            CheckStructure(b);

            // The whole reason `.m2` needs no repack: the record IS the file's
            // 48-byte vertex. A stride that drifted from it would mean the
            // struct picked up padding, which the adapter's static_asserts
            // would have caught at compile time â€” this is the runtime half.
            CHECK(b.stride == 48);
            REQUIRE(b.VertexCount() == mesh.positions.size());

            const VertexAttribute* pos = Find(b, VertexSemantic::Position);
            const VertexAttribute* nrm = Find(b, VertexSemantic::Normal);
            REQUIRE(pos != nullptr);
            REQUIRE(nrm != nullptr);
            CHECK(pos->format == gfx::Format::R32G32B32_FLOAT);
            CHECK(nrm->format == gfx::Format::R32G32B32_FLOAT);

            // Position decoded from the blob using only the description must
            // equal the CPU copy the bounds and centroid are built from. Exact
            // equality: both are the same f32 bits, copied, never computed.
            for (std::size_t v = 0; v < b.VertexCount(); ++v) {
                const auto got = ReadPosition(b, v, pos->offset);
                const auto want = mesh.positions[v];
                REQUIRE(got.x == want.x);
                REQUIRE(got.y == want.y);
                REQUIRE(got.z == want.z);
            }

            // A normal is a unit vector, so this catches the offset being
            // wrong without needing a second decode to compare against: read
            // 12 bytes from the wrong place in a 48-byte record and you get a
            // position, a UV pair or a bone-weight bit pattern, none of which
            // is unit length. Degenerate zero normals are real in the corpus.
            for (std::size_t v = 0; v < b.VertexCount(); ++v) {
                const auto n = ReadPosition(b, v, nrm->offset);
                const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                REQUIRE((NearlyEqual(len, 1.0f, 0.02f) || len == 0.0f));
            }
            ++checked;
        }
    }
    CHECK(checked > 0);
}
#endif // WDX_ENABLE_M2

#if WDX_ENABLE_M3
namespace {
fs::path Sc2Corpus() {
    if (const char* v = std::getenv("WDX_TEST_SC2_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus");
}
constexpr const char* kM3Corpora[] = {"Sc2M3", "Sc2BetaM3", "StarM3", "HotSM3"};

std::size_t M3Limit() {
    if (const char* v = std::getenv("WDX_TEST_M3_LIMIT"); v && *v)
        return static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
    return 400;
}
} // namespace

TEST_CASE("m3 baked buffers describe their own bytes", "[m3][meshbuffer]") {
    const fs::path root = Sc2Corpus();
    const std::size_t limit = M3Limit();
    std::size_t considered = 0, checked = 0, skippedByLimit = 0;

    for (const char* name : kM3Corpora) {
        auto models = FindByExt(root / name, ".m3");
        if (models.empty())
            continue;
        if (limit > 0 && models.size() > limit) {
            skippedByLimit += models.size() - limit;
            models.resize(limit);
        }

        for (const auto& path : models) {
            ++considered;
            INFO("model " << path.string());
            const auto bytes = ReadAll(path);
            REQUIRE_FALSE(bytes.empty());

            auto adapter = io::M3ModelAdapter::Load(ContentRef::FromPath(path.string()),
                                                    std::span<const whiteout::u8>(bytes));
            if (!adapter)
                continue; // effect-only / physics-only .m3, no vertex buffer

            // The parser's own view of the same blob. The adapter derives the
            // layout a second time by hand, and this is what pins the two
            // together â€” if VertexBuffer::initialize() ever changes its offsets
            // or stride, this fires instead of the model rendering subtly wrong.
            const auto& vb = adapter->SourceModel().vertices;
            const auto parserNormals = vb.getNormals();

            const auto meshes = adapter->GetMeshes();
            if (meshes.empty())
                continue; // no drawable region â€” REGN v<3 parser gap

            const auto& regions = adapter->SourceModel().divisions[0].regions;

            for (const auto& mesh : meshes) {
                const MeshBuffer& b = mesh.baked;
                REQUIRE(b.Valid());
                CheckStructure(b);
                REQUIRE(b.stride == vb.vertexSize());
                REQUIRE(b.VertexCount() == mesh.positions.size());

                // A mesh is a slice of the model-global blob starting at its
                // region's firstVertex, so parser index (base + v) is mesh
                // index v. That mapping is what lets the decode below be
                // compared against the parser's rather than only sanity-checked.
                REQUIRE(static_cast<std::size_t>(mesh.geosetId) < regions.size());
                const std::size_t base = regions[static_cast<std::size_t>(mesh.geosetId)].firstVertex;
                REQUIRE(base + b.VertexCount() <= parserNormals.size());

                const VertexAttribute* pos = Find(b, VertexSemantic::Position);
                const VertexAttribute* nrm = Find(b, VertexSemantic::Normal);
                const VertexAttribute* tan = Find(b, VertexSemantic::Tangent);
                REQUIRE(pos != nullptr);
                REQUIRE(nrm != nullptr);
                REQUIRE(tan != nullptr);
                CHECK(nrm->format == gfx::Format::R8G8B8A8_SNORM);
                // Always the last four bytes of the record.
                CHECK(tan->offset + 4 == b.stride);

                for (std::size_t v = 0; v < b.VertexCount(); ++v) {
                    const auto got = ReadPosition(b, v, pos->offset);
                    const auto want = mesh.positions[v];
                    REQUIRE(got.x == want.x);
                    REQUIRE(got.y == want.y);
                    REQUIRE(got.z == want.z);
                }

                // SNORM decode, spelled the way D3D does it â€” max(v/127, -1).
                // The parser divides by 127.0 without the clamp, so a stored
                // -128 is the one value where the two legitimately differ
                // (-1.0 against -1.0079). Tolerated rather than asserted equal,
                // because the GPU is the side that clamps and it is right to.
                for (std::size_t v = 0; v < b.VertexCount(); ++v) {
                    const whiteout::u8* rec = b.data.data() + v * b.stride + nrm->offset;
                    const float nx = std::max(static_cast<float>(static_cast<int8_t>(rec[0])) / 127.0f, -1.0f);
                    const float ny = std::max(static_cast<float>(static_cast<int8_t>(rec[1])) / 127.0f, -1.0f);
                    const float nz = std::max(static_cast<float>(static_cast<int8_t>(rec[2])) / 127.0f, -1.0f);
                    const auto& want = parserNormals[base + v];
                    // The only legitimate divergence: the parser's plain /127
                    // gives -1.0079 where the clamp gives -1.0. One ulp of
                    // 1/127 covers it and nothing else.
                    REQUIRE(NearlyEqual(nx, want.x, 0.008f));
                    REQUIRE(NearlyEqual(ny, want.y, 0.008f));
                    REQUIRE(NearlyEqual(nz, want.z, 0.008f));
                }
                ++checked;
            }
        }
    }

    if (considered == 0) {
        SKIP("no .m3 files under " + root.string() +
             " (set WDX_TEST_SC2_CORPUS to point elsewhere)");
    }
    if (skippedByLimit > 0)
        std::printf("[meshbuffer] %zu .m3 models skipped by WDX_TEST_M3_LIMIT\n", skippedByLimit);
    CHECK(checked > 0);
}
#endif // WDX_ENABLE_M3
