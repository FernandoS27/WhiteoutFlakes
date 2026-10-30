// A StarCraft II mod, map or campaign read as a fallback asset source.

#include <catch2/catch_test_macros.hpp>

#include "io/storage/sc2_document_source.h"

#if WHITEOUT_HAS_MPQ
#include <whiteout/storages/mpq/storage.h>
#endif

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

using whiteout::flakes::io::Sc2DocumentSource;
using whiteout::flakes::io::SourceRead;

namespace {

// A scratch tree under the system temp dir, removed when the test ends.
class TempTree {
public:
    explicit TempTree(const char* name) : root_(fs::temp_directory_path() / name) {
        fs::remove_all(root_);
        fs::create_directories(root_);
    }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }

    TempTree(const TempTree&) = delete;
    TempTree& operator=(const TempTree&) = delete;

    const fs::path& Root() const {
        return root_;
    }

    fs::path Write(const std::string& relative, const std::string& bytes) {
        const fs::path p = root_ / relative;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << bytes;
        return p;
    }

private:
    fs::path root_;
};

std::string Text(const SourceRead& r) {
    return std::string(r.data.begin(), r.data.end());
}

std::unique_ptr<Sc2DocumentSource> MustOpen(const fs::path& doc) {
    std::string error;
    auto src = Sc2DocumentSource::Open(doc, error);
    INFO(error);
    REQUIRE(src);
    return src;
}

std::string OpenError(const fs::path& doc) {
    std::string error;
    REQUIRE_FALSE(Sc2DocumentSource::Open(doc, error));
    return error;
}

} // namespace

TEST_CASE("A folder mod reads its assets in any case") {
    TempTree tree("wdx_test_sc2doc_case");
    tree.Write("M.SC2Mod/Assets/Textures/X.DDS", "x");

    SourceRead r;
    REQUIRE(MustOpen(tree.Root() / "M.SC2Mod")->Read("assets/textures/x.dds", r));
    CHECK(Text(r) == "x");
    CHECK(r.actualExt == ".dds");
}

TEST_CASE("A folder mod's Base.SC2Assets copy wins over its root copy") {
    TempTree tree("wdx_test_sc2doc_base");
    tree.Write("M.SC2Mod/Base.SC2Assets/Assets/Textures/x.dds", "base");
    tree.Write("M.SC2Mod/Assets/Textures/x.dds", "root");

    SourceRead r;
    REQUIRE(MustOpen(tree.Root() / "M.SC2Mod")->Read("assets/textures/x.dds", r));
    CHECK(Text(r) == "base");
}

TEST_CASE("A folder mod substitutes texture extensions") {
    TempTree tree("wdx_test_sc2doc_ext");
    tree.Write("M.SC2Mod/Assets/Textures/y.dds", "y");

    SourceRead r;
    REQUIRE(MustOpen(tree.Root() / "M.SC2Mod")->Read("assets/textures/y.tga", r));
    CHECK(Text(r) == "y");
    CHECK(r.actualExt == ".dds");
}

TEST_CASE("Opening something that is not a usable mod says why") {
    TempTree tree("wdx_test_sc2doc_errors");
    tree.Write("P.SC2Mod", "not an mpq");
    fs::create_directories(tree.Root() / "Q.SC2Mod");
    tree.Write("R.zip", "zip");

    CHECK_FALSE(OpenError(tree.Root() / "P.SC2Mod").empty());
    CHECK(OpenError(tree.Root() / "Q.SC2Mod") == "no Assets folder");
    CHECK(OpenError(tree.Root() / "R.zip") == "not a .SC2Mod, .SC2Map or .SC2Campaign");
    CHECK(OpenError(tree.Root() / "S.SC2Mod") == "not found");
}

TEST_CASE("A folder mod follows symlinks and survives a symlink loop") {
    TempTree tree("wdx_test_sc2doc_links");
    tree.Write("elsewhere/dir/z.dds", "z");
    fs::create_directories(tree.Root() / "M.SC2Mod/Assets/Textures");
    std::error_code ec;
    fs::create_directory_symlink(tree.Root() / "elsewhere/dir", tree.Root() / "M.SC2Mod/Assets/Textures/link", ec);
    if (ec) {
        SUCCEED("symlinks unavailable here");
        return;
    }
    fs::create_directory_symlink(tree.Root() / "M.SC2Mod/Assets", tree.Root() / "M.SC2Mod/Assets/Textures/loop", ec);
    REQUIRE_FALSE(ec);

    SourceRead r;
    REQUIRE(MustOpen(tree.Root() / "M.SC2Mod")->Read("assets/textures/link/z.dds", r));
    CHECK(Text(r) == "z");
}

#if WHITEOUT_HAS_MPQ

namespace {

// WhiteoutLib's MPQ writer/reader does not round-trip files under 64 bytes,
// so packed fixtures carry a 256-byte payload built from a short tag.
std::string Payload(const std::string& tag) {
    std::string s;
    while (s.size() < 256)
        s += tag;
    return s;
}

// A packed mod built the way the editor ships one: a single MPQ file.
fs::path WritePacked(const fs::path& file, const std::vector<std::pair<std::string, std::string>>& entries) {
    auto mpq = whiteout::storages::mpq::Storage::create();
    for (const auto& [name, bytes] : entries) {
        const std::string payload = Payload(bytes);
        const std::vector<whiteout::u8> data(payload.begin(), payload.end());
        REQUIRE(mpq.writeFile(name, data));
    }
    REQUIRE(mpq.save(file.string()));
    return file;
}

} // namespace

TEST_CASE("A packed mod reads its assets in any case") {
    TempTree tree("wdx_test_sc2doc_packed");
    const fs::path pack = WritePacked(tree.Root() / "Pack.SC2Mod", {{"Assets\\Textures\\Q.dds", "q"}});

    SourceRead r;
    REQUIRE(MustOpen(pack)->Read("assets/textures/q.dds", r));
    CHECK(Text(r) == Payload("q"));
}

TEST_CASE("A packed mod's Base.SC2Assets copy wins over its root copy") {
    TempTree tree("wdx_test_sc2doc_packed_base");
    const fs::path pack = WritePacked(tree.Root() / "Pack.SC2Mod", {{"Base.SC2Assets\\Assets\\Textures\\P.dds", "base"},
                                                                    {"Assets\\Textures\\P.dds", "root"}});

    SourceRead r;
    REQUIRE(MustOpen(pack)->Read("assets/textures/p.dds", r));
    CHECK(Text(r) == Payload("base"));
}

TEST_CASE("A packed mod substitutes texture extensions") {
    TempTree tree("wdx_test_sc2doc_packed_ext");
    const fs::path pack = WritePacked(tree.Root() / "Pack.SC2Mod", {{"Assets\\Textures\\Q.dds", "q"}});

    SourceRead r;
    REQUIRE(MustOpen(pack)->Read("assets/textures/q.tga", r));
    CHECK(Text(r) == Payload("q"));
}

#endif // WHITEOUT_HAS_MPQ
