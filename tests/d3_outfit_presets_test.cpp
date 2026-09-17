// ============================================================================
// Diablo III outfit presets (tools/basic_viewer/features/d3_outfit_presets.h):
// the `d3_outfits.ini` round trip, the section-name sanitising, and what an
// absent preset reads as — the load path unequips every slot a preset leaves
// out, so "absent" has to stay distinguishable from "empty".
// ============================================================================

#include "features/d3_outfit_presets.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace whiteout::flakes;

namespace {

struct TempFile {
    std::filesystem::path path;
    explicit TempFile(const char* name)
        : path(std::filesystem::temp_directory_path() / "wdx_d3_outfit_presets_test" / name) {
        std::filesystem::create_directories(path.parent_path());
        std::filesystem::remove(path);
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

std::string ReadAll(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

TEST_CASE("An outfit preset round-trips by name", "[viewer][d3]") {
    TempFile file("roundtrip.ini");

    D3OutfitPreset armour;
    armour.items[1] = "Barbarian_Torso_A";
    armour.items[4] = "Sword_1H_101";
    armour.dyes[1] = 7;
    armour.sheathed = true;
    WriteD3OutfitPreset(file.path, "Heavy", armour);

    D3OutfitPreset naked;
    naked.sheathed = false;
    WriteD3OutfitPreset(file.path, "Naked", naked);

    CHECK(ListD3OutfitPresets(file.path) == std::vector<std::string>{"Heavy", "Naked"});

    const D3OutfitPreset read = ReadD3OutfitPreset(file.path, "Heavy");
    CHECK(read.items == armour.items);
    CHECK(read.dyes == armour.dyes);
    CHECK(read.sheathed == std::optional<bool>(true));

    // Only what is set is written: no slot or dye keys for the empty preset.
    const std::string text = ReadAll(file.path);
    CHECK(text.find("Dye1=7") != std::string::npos);
    CHECK(text.find("Dye0") == std::string::npos);

    const D3OutfitPreset readNaked = ReadD3OutfitPreset(file.path, "Naked");
    for (const auto& item : readNaked.items)
        CHECK_FALSE(item.has_value());
    CHECK(readNaked.sheathed == std::optional<bool>(false));
}

TEST_CASE("Saving a preset replaces its own section only", "[viewer][d3]") {
    TempFile file("replace.ini");

    D3OutfitPreset first;
    first.items[0] = "Helm_A";
    first.items[7] = "Pants_A";
    WriteD3OutfitPreset(file.path, "Set", first);
    D3OutfitPreset other;
    other.items[2] = "Boots_B";
    WriteD3OutfitPreset(file.path, "Other", other);

    D3OutfitPreset second;
    second.items[0] = "Helm_B";
    WriteD3OutfitPreset(file.path, "Set", second);

    const D3OutfitPreset read = ReadD3OutfitPreset(file.path, "Set");
    CHECK(read.items[0] == std::optional<std::string>("Helm_B"));
    CHECK_FALSE(read.items[7].has_value()); // the old slot went with the old section
    CHECK(ReadD3OutfitPreset(file.path, "Other").items[2] == std::optional<std::string>("Boots_B"));
}

TEST_CASE("Preset names that would break the ini section are sanitised", "[viewer][d3]") {
    TempFile file("names.ini");

    D3OutfitPreset preset;
    preset.items[3] = "Gloves_C";
    WriteD3OutfitPreset(file.path, "v1.2 [test]=x", preset);

    CHECK(ListD3OutfitPresets(file.path) == std::vector<std::string>{"v1_2 _test__x"});
    // Read back under the name the user typed: the same sanitising applies.
    CHECK(ReadD3OutfitPreset(file.path, "v1.2 [test]=x").items[3] == std::optional<std::string>("Gloves_C"));
}

TEST_CASE("A preset the file does not hold reads as empty", "[viewer][d3]") {
    TempFile file("missing.ini");
    const D3OutfitPreset read = ReadD3OutfitPreset(file.path, "Nothing");
    for (usize s = 0; s < kD3VisualSlots.size(); ++s) {
        CHECK_FALSE(read.items[s].has_value());
        CHECK(read.dyes[s] == 0);
    }
    CHECK_FALSE(read.sheathed.has_value());
    CHECK(ListD3OutfitPresets(file.path).empty());
}

TEST_CASE("Command-line slot names follow the visual slot table", "[viewer][d3]") {
    CHECK(D3VisualSlotFromCliName("legs") == std::optional<i32>(7));
    CHECK(D3VisualSlotFromCliName("righthand") == std::optional<i32>(4));
    CHECK(D3VisualSlotFromCliName("2") == std::optional<i32>(2));
    CHECK(D3VisualSlotFromCliName("007") == std::optional<i32>(7));
    CHECK_FALSE(D3VisualSlotFromCliName("8").has_value());
    CHECK_FALSE(D3VisualSlotFromCliName("Legs").has_value());
    CHECK_FALSE(D3VisualSlotFromCliName("").has_value());
    CHECK_FALSE(D3VisualSlotFromCliName("-1").has_value());
}
