// Fallback mods: searched, in order, only after the disk and the game's
// storage miss, and only for the game they were set for.

#include <catch2/catch_test_macros.hpp>

#include "io/file_content_provider.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using whiteout::flakes::ProductId;
using whiteout::flakes::io::FileContentProvider;

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

    void Write(const std::string& relative, const std::string& bytes) {
        const fs::path p = root_ / relative;
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << bytes;
    }

private:
    fs::path root_;
};

// A StarCraft II provider that opens no storage, so every hit below is the
// disk's or a fallback mod's.
void MakeSc2WithoutStorage(FileContentProvider& p, const fs::path& base) {
    p.SetGame(ProductId::Sc2);
    p.SetIgnoreCasc(true);
    p.SetBasePath(base);
}

std::string Read(FileContentProvider& p, const std::string& path) {
    const auto bytes = p.ReadFile(path);
    return bytes ? std::string(bytes->begin(), bytes->end()) : std::string{};
}

} // namespace

TEST_CASE("A fallback mod fills a miss") {
    TempTree tree("wdx_test_fallback_fill");
    fs::create_directories(tree.Root() / "base");
    tree.Write("A.SC2Mod/Assets/Textures/x.dds", "A");

    FileContentProvider p;
    MakeSc2WithoutStorage(p, tree.Root() / "base");
    p.SetFallbackMods(ProductId::Sc2, {tree.Root() / "A.SC2Mod"});
    CHECK(Read(p, "assets/textures/x.dds") == "A");
}

TEST_CASE("The disk wins over a fallback mod") {
    TempTree tree("wdx_test_fallback_disk");
    tree.Write("base/x.dds", "disk");
    tree.Write("A.SC2Mod/Assets/Textures/x.dds", "A");

    FileContentProvider p;
    MakeSc2WithoutStorage(p, tree.Root() / "base");
    p.SetFallbackMods(ProductId::Sc2, {tree.Root() / "A.SC2Mod"});
    CHECK(Read(p, "assets/textures/x.dds") == "disk");
}

TEST_CASE("Fallback mods are tried in list order, and a new list replaces the old") {
    TempTree tree("wdx_test_fallback_order");
    fs::create_directories(tree.Root() / "base");
    tree.Write("A.SC2Mod/Assets/Textures/x.dds", "A");
    tree.Write("B.SC2Mod/Assets/Textures/x.dds", "B");

    FileContentProvider p;
    MakeSc2WithoutStorage(p, tree.Root() / "base");
    p.SetFallbackMods(ProductId::Sc2, {tree.Root() / "A.SC2Mod", tree.Root() / "B.SC2Mod"});
    CHECK(Read(p, "assets/textures/x.dds") == "A");
    p.SetFallbackMods(ProductId::Sc2, {tree.Root() / "B.SC2Mod", tree.Root() / "A.SC2Mod"});
    CHECK(Read(p, "assets/textures/x.dds") == "B");
}

TEST_CASE("Fallback mods belong to the game they were set for") {
    TempTree tree("wdx_test_fallback_game");
    fs::create_directories(tree.Root() / "base");
    tree.Write("A.SC2Mod/Assets/Textures/x.dds", "A");

    FileContentProvider p;
    MakeSc2WithoutStorage(p, tree.Root() / "base");
    p.SetFallbackMods(ProductId::Sc2, {tree.Root() / "A.SC2Mod"});

    p.SetGame(ProductId::Wc3);
    p.SetIgnoreCasc(true);
    p.SetIgnoreMpq(true);
    CHECK(Read(p, "assets/textures/x.dds").empty());

    p.SetGame(ProductId::Sc2);
    CHECK(Read(p, "assets/textures/x.dds") == "A");
}
