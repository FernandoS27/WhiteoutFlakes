// The asset roots a loose StarCraft II model resolves its textures against:
// the enclosing mod/map/campaign's Base.SC2Assets and root, or the folder
// holding the Assets/ tree it sits in.

#include <catch2/catch_test_macros.hpp>

#include "io/storage/sc2_asset_roots.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using whiteout::flakes::io::FindSc2AssetRoots;
using whiteout::flakes::io::IsSc2DocumentName;

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

    fs::path Touch(const std::string& relative) {
        const fs::path p = root_ / relative;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << "x";
        return p;
    }

private:
    fs::path root_;
};

} // namespace

TEST_CASE("A model inside a mod resolves against the mod's root") {
    TempTree tree("wdx_test_sc2roots_mod");
    const fs::path model = tree.Touch("X.SC2Mod/Assets/Models/a.m3");

    REQUIRE(FindSc2AssetRoots(model) == std::vector<fs::path>{tree.Root() / "X.SC2Mod"});
}

TEST_CASE("A model under Base.SC2Assets resolves against that component, in any case") {
    TempTree tree("wdx_test_sc2roots_base");
    const fs::path model = tree.Touch("Y.sc2mod/base.sc2assets/Assets/Effects/b/b.m3");

    REQUIRE(FindSc2AssetRoots(model) == std::vector<fs::path>{tree.Root() / "Y.sc2mod/base.sc2assets"});
}

TEST_CASE("A document with both components lists Base.SC2Assets first") {
    TempTree tree("wdx_test_sc2roots_both");
    tree.Touch("Z.SC2Map/Base.SC2Assets/Assets/Textures/t.dds");
    const fs::path model = tree.Touch("Z.SC2Map/Assets/m.m3");

    REQUIRE(FindSc2AssetRoots(model) ==
            std::vector<fs::path>{tree.Root() / "Z.SC2Map/Base.SC2Assets", tree.Root() / "Z.SC2Map"});
}

TEST_CASE("A model in a bare Assets tree resolves against the tree's parent") {
    TempTree tree("wdx_test_sc2roots_bare");
    const fs::path model = tree.Touch("foo/Assets/Models/c.m3");

    REQUIRE(FindSc2AssetRoots(model) == std::vector<fs::path>{tree.Root() / "foo"});
}

TEST_CASE("An unrelated Assets folder nearby is not a root") {
    TempTree tree("wdx_test_sc2roots_none");
    tree.Touch("Assets/Textures/t.dds");
    const fs::path model = tree.Touch("loose/c.m3");

    REQUIRE(FindSc2AssetRoots(model).empty());
}

TEST_CASE("IsSc2DocumentName recognises mods, maps and campaigns by name") {
    REQUIRE(IsSc2DocumentName("A.SC2Mod"));
    REQUIRE(IsSc2DocumentName("dir/b.sc2map"));
    REQUIRE(IsSc2DocumentName("C.SC2Campaign"));
    REQUIRE_FALSE(IsSc2DocumentName("D.SC2ModX"));
    REQUIRE_FALSE(IsSc2DocumentName("E.SC2Assets"));
    REQUIRE_FALSE(IsSc2DocumentName("F"));
}
