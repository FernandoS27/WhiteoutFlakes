// Path plumbing: UTF-8 <-> fs::path, WC3-style separator normalisation, the
// on-disk resolver's extension/filename fallbacks, and the replaceable-ID
// texture mapping.

#include <catch2/catch_test_macros.hpp>

#include "io/file_resolver.h"
#include "whiteout/flakes/util/path_utf8.h"
#include "whiteout/flakes/util/replaceable_paths.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

using whiteout::flakes::io::FileResolver;
using whiteout::flakes::io::FsPathFromUtf8;
using whiteout::flakes::io::PathToUtf8;
using whiteout::flakes::io::ReplaceableCanonicalPath;
using whiteout::flakes::io::Tileset;

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

TEST_CASE("NormalizeSeparators rewrites WC3 backslashes") {
    REQUIRE(FileResolver::NormalizeSeparators("Textures\\Rock\\rock.blp") ==
            "Textures/Rock/rock.blp");
    REQUIRE(FileResolver::NormalizeSeparators("already/forward.blp") == "already/forward.blp");
    REQUIRE(FileResolver::NormalizeSeparators("").empty());
}

TEST_CASE("UTF-8 paths survive the fs::path round trip") {
    // Hex-escaped so the test doesn't depend on the compiler's source charset:
    // "Textures/日本/rock.blp".
    constexpr const char* kUtf8 = "Textures/\xe6\x97\xa5\xe6\x9c\xac/rock.blp";

    REQUIRE(PathToUtf8(FsPathFromUtf8("Textures/rock.blp")) == "Textures/rock.blp");
    REQUIRE(PathToUtf8(FsPathFromUtf8(kUtf8)) == kUtf8);
    REQUIRE(PathToUtf8(FsPathFromUtf8("")).empty());
}

TEST_CASE("FileResolver finds a file under the base path") {
    TempTree tree("wdx_test_resolver_base");
    tree.Touch("Textures/Rock/rock.blp");

    FileResolver r(tree.Root());
    // WC3 paths arrive with backslashes; the resolver normalises them.
    REQUIRE(r.ResolveTexture("Textures\\Rock\\rock.blp") == tree.Root() / "Textures/Rock/rock.blp");
}

TEST_CASE("FileResolver substitutes known extensions") {
    // Models reference .blp; the file on disk is often .dds or .tga instead.
    TempTree tree("wdx_test_resolver_ext");
    tree.Touch("Textures/rock.dds");

    FileResolver r(tree.Root());
    const fs::path found = r.ResolveTexture("Textures\\rock.blp");
    REQUIRE(found.filename() == "rock.dds");
}

TEST_CASE("FileResolver falls back to the bare filename") {
    // Exported models keep their authoring-time directories; the assets next
    // to the model are what the user actually has.
    TempTree tree("wdx_test_resolver_filename");
    tree.Touch("rock.blp");

    FileResolver r(tree.Root());
    REQUIRE(r.ResolveTexture("war3mapImported\\Textures\\rock.blp") == tree.Root() / "rock.blp");
}

TEST_CASE("FileResolver checks the system base path after the base path") {
    TempTree base("wdx_test_resolver_primary");
    TempTree sys("wdx_test_resolver_system");
    base.Touch("shared.blp");
    sys.Touch("shared.blp");
    sys.Touch("system_only.blp");

    FileResolver r(base.Root());
    r.SetSystemBasePath(sys.Root());

    REQUIRE(r.ResolveTexture("shared.blp") == base.Root() / "shared.blp");
    REQUIRE(r.ResolveTexture("system_only.blp") == sys.Root() / "system_only.blp");
}

TEST_CASE("FileResolver returns an empty path when nothing matches") {
    TempTree tree("wdx_test_resolver_missing");
    FileResolver r(tree.Root());

    REQUIRE(r.ResolveTexture("nothing/here.blp").empty());
    REQUIRE(r.ResolveModel("nothing/here.mdx").empty());

    SECTION("and with no base path configured at all") {
        FileResolver unset;
        REQUIRE(unset.ResolveTexture("anything.blp").empty());
    }
}

TEST_CASE("ResolveModel only accepts model extensions") {
    TempTree tree("wdx_test_resolver_model");
    tree.Touch("unit.mdl");

    FileResolver r(tree.Root());
    REQUIRE(r.ResolveModel("unit.mdx").filename() == "unit.mdl");
    // A texture extension must not satisfy a model lookup.
    tree.Touch("other.blp");
    REQUIRE(r.ResolveModel("other.mdx").empty());
}

TEST_CASE("Replaceable tree IDs map to their canonical textures") {
    REQUIRE(std::string(ReplaceableCanonicalPath(31, Tileset::LordaeronSummer)) ==
            "ReplaceableTextures\\LordaeronTree\\LordaeronSummerTree.blp");
    REQUIRE(std::string(ReplaceableCanonicalPath(32, Tileset::Ashenvale)) ==
            "ReplaceableTextures\\AshenvaleTree\\AshenTree.blp");
    REQUIRE(std::string(ReplaceableCanonicalPath(37, Tileset::Outland)) ==
            "ReplaceableTextures\\OutlandMushroomTree\\MushroomTree.blp");
}

TEST_CASE("Replaceable IDs without a canonical path report null") {
    REQUIRE(ReplaceableCanonicalPath(21, Tileset::LordaeronSummer) == nullptr); // team colour
    REQUIRE(ReplaceableCanonicalPath(0, Tileset::LordaeronSummer) == nullptr);
    REQUIRE(ReplaceableCanonicalPath(9999, Tileset::LordaeronSummer) == nullptr);
    REQUIRE(ReplaceableCanonicalPath(-1, Tileset::LordaeronSummer) == nullptr);
}

TEST_CASE("Cliff ID 11 falls back to the shipped cliff texture") {
    // With no game data loaded there is no per-tileset cliff table, and every
    // tileset must still resolve to something rather than null.
    for (int i = 0; i < static_cast<int>(Tileset::Count); ++i) {
        const char* p = ReplaceableCanonicalPath(11, static_cast<Tileset>(i));
        REQUIRE(p != nullptr);
        REQUIRE(std::string(p) == "ReplaceableTextures\\Cliff\\Cliff0.blp");
    }
}

TEST_CASE("The current tileset round-trips and rejects out-of-range values") {
    const Tileset original = whiteout::flakes::io::GetCurrentTileset();

    whiteout::flakes::io::SetCurrentTileset(Tileset::Northrend);
    REQUIRE(whiteout::flakes::io::GetCurrentTileset() == Tileset::Northrend);

    whiteout::flakes::io::SetCurrentTileset(Tileset::Count); // sentinel, not a value
    REQUIRE(whiteout::flakes::io::GetCurrentTileset() == Tileset::Northrend);

    whiteout::flakes::io::SetCurrentTileset(original);
}

TEST_CASE("Every tileset has a display name") {
    for (int i = 0; i < static_cast<int>(Tileset::Count); ++i) {
        const char* name = whiteout::flakes::io::TilesetName(static_cast<Tileset>(i));
        REQUIRE(name != nullptr);
        REQUIRE(std::string(name) != "Unknown");
    }
    REQUIRE(std::string(whiteout::flakes::io::TilesetName(Tileset::Count)) == "Unknown");
}
