// SLK parsing — the format WC3 ships its game-data tables in (CliffTypes.slk,
// UnitUI.slk, …). The cursor semantics (X/Y persist across records) are the
// part that regressions hide in.

#include <catch2/catch_test_macros.hpp>

#include "io/slk.h"

#include <span>
#include <string_view>

using whiteout::flakes::io::ParseSlk;
using whiteout::flakes::io::SlkTable;

namespace {

SlkTable Parse(std::string_view text) {
    return ParseSlk(std::span<const char>(text.data(), text.size()));
}

// Header + two data rows, in the record shape real .slk files use: the first
// cell of a row carries both X and Y, the rest only X.
constexpr std::string_view kUnitTable = "ID;PWXL;N;E\n"
                                        "P;PGeneral\n"
                                        "C;Y1;X1;K\"unitID\"\n"
                                        "C;X2;K\"name\"\n"
                                        "C;X3;K\"hitPoints\"\n"
                                        "C;Y2;X1;K\"hfoo\"\n"
                                        "C;X2;K\"Footman\"\n"
                                        "C;X3;K420\n"
                                        "E\n";

} // namespace

TEST_CASE("ParseSlk reads a header row and data rows") {
    const SlkTable t = Parse(kUnitTable);

    REQUIRE(t.RowCount() == 2);
    REQUIRE(t.HeaderCount() == 3);

    const auto colId = t.FindColumn("unitID");
    const auto colName = t.FindColumn("name");
    const auto colHp = t.FindColumn("hitPoints");
    REQUIRE(colId == 0);
    REQUIRE(colName == 1);
    REQUIRE(colHp == 2);

    REQUIRE(t.Cell(1, colId) == "hfoo");
    REQUIRE(t.Cell(1, colName) == "Footman");
    REQUIRE(t.Cell(1, colHp) == "420"); // unquoted values stay verbatim
}

TEST_CASE("FindColumn is case-insensitive and reports misses") {
    const SlkTable t = Parse(kUnitTable);

    REQUIRE(t.FindColumn("UNITID") == 0);
    REQUIRE(t.FindColumn("HitPoints") == 2);
    REQUIRE(t.FindColumn("armor") == -1);

    const SlkTable empty;
    REQUIRE(empty.FindColumn("anything") == -1);
}

TEST_CASE("ParseSlk decodes quoted cells") {
    SECTION("quotes are stripped") {
        const SlkTable t = Parse("C;Y1;X1;K\"Abilities\\Weapons\\Bolt\"\nE\n");
        REQUIRE(t.Cell(0, 0) == "Abilities\\Weapons\\Bolt");
    }
    SECTION("doubled quotes collapse to one") {
        const SlkTable t = Parse("C;Y1;X1;K\"say \"\"hi\"\"\"\nE\n");
        REQUIRE(t.Cell(0, 0) == "say \"hi\"");
    }
    SECTION("an empty quoted cell is empty, not a stray quote") {
        const SlkTable t = Parse("C;Y1;X1;K\"\"\nE\n");
        REQUIRE(t.Cell(0, 0).empty());
    }
}

TEST_CASE("ParseSlk carries the row/column cursor across records") {
    // A record may set only X (row inherited), only Y (column inherited), or
    // neither — and a record with no K still moves the cursor for the next one.
    const SlkTable t = Parse("C;Y3;X2\n"
                             "C;K\"landed here\"\n"
                             "C;Y4;K\"same column, next row\"\n"
                             "E\n");

    REQUIRE(t.RowCount() == 4);
    REQUIRE(t.Cell(2, 1) == "landed here");
    REQUIRE(t.Cell(3, 1) == "same column, next row");
}

TEST_CASE("ParseSlk stops at the E record") {
    const SlkTable t = Parse("C;Y1;X1;K\"kept\"\n"
                             "E\n"
                             "C;Y1;X2;K\"dropped\"\n");

    REQUIRE(t.HeaderCount() == 1);
    REQUIRE(t.Cell(0, 0) == "kept");
}

TEST_CASE("ParseSlk ignores records it does not understand") {
    // F (format), O (options), B (bounds) and the ID header carry no cell data.
    const SlkTable t = Parse("ID;PWXL;N;E\n"
                             "B;Y2;X2;D0\n"
                             "O;L;D;V\n"
                             "F;P0;DG0G8;M280\n"
                             "C;Y1;X1;K\"only cell\"\n"
                             "E\n");

    REQUIRE(t.RowCount() == 1);
    REQUIRE(t.Cell(0, 0) == "only cell");
}

TEST_CASE("ParseSlk handles CRLF and blank lines") {
    const SlkTable t = Parse("C;Y1;X1;K\"a\"\r\n"
                             "\r\n"
                             "C;Y2;X1;K\"b\"\r\n"
                             "E\r\n");

    REQUIRE(t.RowCount() == 2);
    REQUIRE(t.Cell(0, 0) == "a");
    REQUIRE(t.Cell(1, 0) == "b");
}

TEST_CASE("SlkTable::Cell is bounds-checked") {
    const SlkTable t = Parse(kUnitTable);

    REQUIRE(t.Cell(99, 0).empty());  // past the last row
    REQUIRE(t.Cell(0, 99).empty());  // past the last column
    REQUIRE(t.Cell(0, -1).empty());  // what FindColumn returns on a miss
}

TEST_CASE("ParseSlk survives truncated input") {
    // No E record, no trailing newline, a record cut off mid-field.
    const SlkTable t = Parse("C;Y1;X1;K\"a\"\nC;X2;K");
    REQUIRE(t.Cell(0, 0) == "a");

    REQUIRE(Parse("").RowCount() == 0);
    REQUIRE(Parse("E").RowCount() == 0);
}
