// A StarCraft II mod, map or campaign read as a fallback asset source.

#include <catch2/catch_test_macros.hpp>

#include "io/storage/sc2_document_source.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
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
    tree.Write("P.SC2Mod", "packed");
    fs::create_directories(tree.Root() / "Q.SC2Mod");
    tree.Write("R.zip", "zip");

    CHECK(OpenError(tree.Root() / "P.SC2Mod") == "packed mods are not supported yet");
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
