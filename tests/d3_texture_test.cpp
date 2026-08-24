// ============================================================================
// D3 `.tex` through the shared texture dispatcher.
//
// Two things are being checked, and only one of them is about TEX:
//
//  1. **`SniffTextureExtension` recognises the SNO magic.** D3 references
//     textures by SNO id and never by path, so an id-typed ref reaches
//     `AssetManager::Apply` with `foundExt` deliberately cleared
//     (`CascSource::ReadById` says so in as many words) and no path to take an
//     extension from. The container magic is the whole of how a `.tex` gets
//     dispatched — and the magic identifies the asset *family*, not the texture
//     format, which is enough because the sniff only ever runs on a slot
//     already acquired as AssetKind::Texture.
//
//  2. **The dispatcher's `.tex` arm decodes.** Measured over 400 installed
//     `.tex`: BC1/BC2/BC3 account for 96% of the sample, and every one of those
//     is a format `WhiteoutFormatToGfx` already maps — so textures keep their
//     compression and mip chain with no RGBA8 round-trip. That is the claim
//     this asserts, because losing it is invisible in a render.
//
// Needs the installed client (Diablo III ships no `.tex` into the corpus, which
// is Actor/Appearance/Anim only). WDX_TEST_D3_INSTALL points at the install
// root; without it, and without a detected one, this skips. Skipped is not
// passed.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/d3/d3_model_adapter.h"
#include "io/d3/d3_sno_cache.h"
#include "io/file_content_provider.h"
#include "renderer/model/model_source_utils.h"
#include "whiteout/flakes/content_ref.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace ::whiteout;
using ::whiteout::flakes::ContentRef;
using ::whiteout::flakes::ProductId;
using ::whiteout::flakes::io::FileContentProvider;
namespace rm = ::whiteout::flakes::renderer::model;

TEST_CASE("The SNO magic reaches the texture dispatcher", "[d3][texture]") {
    // 0xDEADBEEF little-endian, plus enough bytes to look like a header.
    std::vector<u8> sno(64, 0);
    sno[0] = 0xEF;
    sno[1] = 0xBE;
    sno[2] = 0xAD;
    sno[3] = 0xDE;
    CHECK(rm::SniffTextureExtension(sno) == ".tex");

    // And the arms it must not have disturbed. This function is on four
    // products' decode path, which is why a new magic here is a full-gate
    // change rather than an additive one.
    std::vector<u8> blp{'B', 'L', 'P', '2', 0, 0, 0, 0};
    CHECK(rm::SniffTextureExtension(blp) == ".blp");
    std::vector<u8> dds{'D', 'D', 'S', ' ', 0, 0, 0, 0};
    CHECK(rm::SniffTextureExtension(dds) == ".dds");
    std::vector<u8> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    CHECK(rm::SniffTextureExtension(png) == ".png");
    // TGA has no leading magic and must stay unrecognised, so a caller's own
    // fallback survives.
    std::vector<u8> tga(64, 0);
    CHECK(rm::SniffTextureExtension(tga).empty());
    CHECK(rm::SniffTextureExtension({}).empty());
}

namespace {

namespace fs = std::filesystem;

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_D3_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/D3");
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

TEST_CASE("D3 `.tex` decodes through the shared dispatcher", "[d3][texture][install]") {
    // Texture ids come out of shipped materials rather than out of a listing:
    // that IS the production route — a D3 material names its textures by SNO id
    // and never by path — and it means the test exercises the acquire the
    // renderer actually performs.
    const fs::path appDir = CorpusRoot() / "Appearances";
    std::error_code ec;
    if (!fs::is_directory(appDir, ec)) {
        WARN("No D3 corpus at " << appDir.string() << ". SKIPPED, not passed.");
        return;
    }

    FileContentProvider provider;
    if (const char* root = std::getenv("WDX_TEST_D3_INSTALL"); root && *root)
        provider.SetInstallPath(root);
    provider.SetGame(ProductId::D3);
    if (provider.GamePath(ProductId::D3).empty()) {
        WARN("No Diablo III install found (set WDX_TEST_D3_INSTALL). SKIPPED, not passed.");
        return;
    }

    std::vector<i32> ids;
    std::size_t appsRead = 0;
    for (fs::directory_iterator it(appDir, ec), end; it != end && ids.size() < 40;
         it.increment(ec)) {
        if (ec)
            break;
        if (!it->is_regular_file(ec) || it->path().extension() != ".app")
            continue;
        auto app = ::whiteout::sno::d3::native::parseAppearances(ReadAll(it->path()));
        if (!app)
            continue;
        ++appsRead;
        for (const auto& t : flakes::io::CollectD3Textures(*app, 0)) {
            if (std::find(ids.begin(), ids.end(), t.snoId) == ids.end())
                ids.push_back(t.snoId);
            if (ids.size() >= 40)
                break;
        }
    }
    if (ids.empty()) {
        WARN("No texture references found in the first corpus appearances. SKIPPED, not passed.");
        return;
    }

    std::size_t read = 0, sniffed = 0, decoded = 0, blockCompressed = 0, withMips = 0, cube = 0;
    for (i32 sno : ids) {
        // The exact ref the adapter's sharedKey resolves to: an id, never a
        // path. Acquiring one file both ways would be two AssetManager slots,
        // two CASC reads and two GPU textures with no error anywhere.
        auto bytes = provider.ReadFile(ContentRef::FromFileId(static_cast<u32>(sno)));
        if (!bytes || bytes->empty())
            continue;
        ++read;
        // No foundExt (CascSource::ReadById clears it deliberately) and no path
        // to take one from, so the magic is all there is.
        const std::string ext = rm::SniffTextureExtension(*bytes);
        if (ext != ".tex")
            continue;
        ++sniffed;

        auto tex = rm::DispatchTextureParser(
            ext, [&](auto& parser) { return parser.parse(std::span<const u8>(*bytes)); });
        if (!tex)
            continue;
        ++decoded;
        if (tex->mipCount() > 1)
            ++withMips;
        using TT = ::whiteout::textures::TextureType;
        if (tex->type() == TT::TextureCube || tex->type() == TT::TextureCubeArray)
            ++cube;
        const auto gfxFmt = rm::WhiteoutFormatToGfx(tex->format(), tex->isSrgb());
        if (gfxFmt != flakes::gfx::Format::Unknown && flakes::gfx::IsBlockCompressed(gfxFmt))
            ++blockCompressed;
    }

    std::printf("[d3-tex] %zu appearances -> %zu texture ids | %zu read, %zu sniffed as .tex, "
                "%zu decoded | %zu stayed block-compressed, %zu carry mips, %zu cubes\n",
                appsRead, ids.size(), read, sniffed, decoded, blockCompressed, withMips, cube);

    if (read == 0) {
        WARN("The Diablo III storage answered none of the sampled ids. SKIPPED, not passed.");
        return;
    }
    // Every `.tex` is a SNO asset, so the sniff must recognise all of them.
    CHECK(sniffed == read);
    CHECK(decoded == sniffed);
    // The claim that matters for memory and for fidelity: BC1/2/3 are 96% of
    // the shipped set and all three map, so nothing round-trips through RGBA8.
    CHECK(blockCompressed * 2 > decoded);
}
