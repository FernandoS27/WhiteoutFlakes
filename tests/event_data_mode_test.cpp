// Event-data SLK tables are cached per render mode.
//
// SD and HD read the same paths through a different CASC prefix order, so the
// two modes can legitimately parse to different rows — but a flip back to a
// mode this session has already been in must re-read nothing. Before the
// tables were mode-keyed, every SD<->HD flip forced a full re-parse of all
// eighteen SLKs, so a tab switch between two documents in different modes paid
// for it twice on the round trip.
//
// The whole file is one TEST_CASE on purpose: the tables live in a process-
// global cache with no reset hook, so the steps below are ordered and each one
// depends on the state the previous left behind.

#include <catch2/catch_test_macros.hpp>

#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/event_data.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>

using whiteout::flakes::ContentRef;
using whiteout::flakes::Wc3ArtTier;
namespace io = whiteout::flakes::io;

namespace {

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::replace(s.begin(), s.end(), '/', '\\');
    return s;
}

// A header row plus one data row, in the record shape real .slk files use.
std::string OneRow(std::string_view colA, std::string_view colB, std::string_view colC,
                   std::string_view a, std::string_view b, std::string_view c) {
    std::string s = "ID;PWXL;N;E\n";
    s += "C;Y1;X1;K\"" + std::string(colA) + "\"\n";
    s += "C;X2;K\"" + std::string(colB) + "\"\n";
    s += "C;X3;K\"" + std::string(colC) + "\"\n";
    s += "C;Y2;X1;K\"" + std::string(a) + "\"\n";
    s += "C;X2;K\"" + std::string(b) + "\"\n";
    s += "C;X3;K\"" + std::string(c) + "\"\n";
    s += "E\n";
    return s;
}

/// Serves the three splat SLKs with mode-dependent contents and an empty table
/// for every other one the loader asks for, and counts every read. Reads are
/// the measurement: a mode the cache already holds must cost zero.
class ModeProvider final : public io::IContentProvider {
public:
    io::RequestId Request(const ContentRef& ref, io::CompletionCallback cb) override {
        ++reads;
        io::RequestResult r;
        r.ok = true;
        r.actualExt = ".slk";
        const std::string body = Body(ref.IsFileId() ? std::string{} : Lower(ref.path));
        r.data.assign(reinterpret_cast<const whiteout::u8*>(body.data()),
                      reinterpret_cast<const whiteout::u8*>(body.data() + body.size()));
        cb(std::move(r));
        return 1;
    }
    void Wait(io::RequestId) override {}
    void Cancel(io::RequestId) override {}
    void Pump() override {}
    // The tier is what a provider actually implements; SetHdMode/HdMode are
    // the two-state view the base class expresses in terms of it. This test
    // only cares about "classic vs not", which is exactly what HdMode() means.
    void SetArtTier(Wc3ArtTier tier) override {
        hd_ = tier != Wc3ArtTier::Classic;
    }
    Wc3ArtTier ArtTier() const override {
        return hd_ ? Wc3ArtTier::Reforged : Wc3ArtTier::Classic;
    }

    std::size_t reads = 0;

private:
    std::string Body(const std::string& path) const {
        const std::string tag = hd_ ? "Hd" : "Sd";
        if (path == "splats\\spawndata.slk")
            return OneRow("ID", "Model", "Pad", "footprint", "Units\\" + tag + "\\Foo.mdl", "");
        if (path == "splats\\splatdata.slk")
            return OneRow("ID", "Dir", "file", "blood", "ReplaceableTextures\\Splats",
                          "Blood" + tag);
        if (path == "splats\\ubersplatdata.slk")
            return OneRow("ID", "Dir", "file", "scorch", "ReplaceableTextures\\Splats",
                          "Scorch" + tag);
        return "ID;PWXL;N;E\nE\n";
    }

    bool hd_ = false;
};

} // namespace

TEST_CASE("event-data tables are cached per render mode") {
    ModeProvider cp;

    // ---- SD, first visit: everything is read. ----
    cp.SetHdMode(false);
    io::LoadEventDataFiles(&cp, /*force=*/false);
    const std::size_t firstPass = cp.reads;
    REQUIRE(firstPass > 0);
    REQUIRE(io::IsSplCachePopulated());
    REQUIRE(io::FindSpn("footprint") != nullptr);
    CHECK(io::FindSpn("footprint")->modelPath == "Units\\Sd\\Foo.mdx");
    CHECK(io::FindSpl("blood")->file == "ReplaceableTextures\\Splats\\BloodSd.blp");
    CHECK(io::FindUbr("scorch")->file == "ReplaceableTextures\\Splats\\ScorchSd.blp");

    // ---- SD again: the load is a no-op, as it always was. ----
    io::LoadEventDataFiles(&cp, /*force=*/false);
    CHECK(cp.reads == firstPass);

    // ---- HD, first visit: its own tables, parsed from scratch. ----
    cp.SetHdMode(true);
    io::LoadEventDataFiles(&cp, /*force=*/false);
    CHECK(cp.reads == firstPass * 2);
    REQUIRE(io::FindSpn("footprint") != nullptr);
    CHECK(io::FindSpn("footprint")->modelPath == "Units\\Hd\\Foo.mdx");
    CHECK(io::FindSpl("blood")->file == "ReplaceableTextures\\Splats\\BloodHd.blp");

    // ---- Back to SD: this is the fix. Nothing is re-read, and the rows are
    //      the ones SD parsed the first time, not HD's. ----
    const std::size_t beforeFlipBack = cp.reads;
    cp.SetHdMode(false);
    io::LoadEventDataFiles(&cp, /*force=*/false);
    CHECK(cp.reads == beforeFlipBack);
    CHECK(io::FindSpn("footprint")->modelPath == "Units\\Sd\\Foo.mdx");
    CHECK(io::FindSpl("blood")->file == "ReplaceableTextures\\Splats\\BloodSd.blp");

    // ---- A force re-parse applies to the mode being served and no other. ----
    io::LoadEventDataFiles(&cp, /*force=*/true);
    CHECK(cp.reads == beforeFlipBack + firstPass);
    cp.SetHdMode(true);
    io::LoadEventDataFiles(&cp, /*force=*/false);
    CHECK(cp.reads == beforeFlipBack + firstPass); // HD's tables still stand
    CHECK(io::FindSpn("footprint")->modelPath == "Units\\Hd\\Foo.mdx");
}
