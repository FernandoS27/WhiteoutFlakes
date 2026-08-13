// ============================================================================
// The content axis (REFACTOR_PLAN.md P6): ContentRef as the identity a piece
// of content is named by, and the three invariants that a later change could
// break without failing to compile.
//
// Device-free throughout. The one test that touches a real CASC install skips
// itself when none is configured, so this file runs on a CI box with no GPU
// and no game archives.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/file_content_provider.h"
#include "io/product_detect.h"
#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/views.h"

#if WHITEOUT_HAS_CASC
#include <whiteout/storages/casc/storage.h>
#endif

#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>

using whiteout::flakes::ContentRef;
using whiteout::flakes::ProductId;
namespace io = whiteout::flakes::io;

// ---------------------------------------------------------------------------
// The value type
// ---------------------------------------------------------------------------

TEST_CASE("ContentRef distinguishes the two ways of naming content") {
    const auto p = ContentRef::FromPath("Units/Human/Footman/Footman.mdx");
    const auto f = ContentRef::FromFileId(1234567);

    CHECK(p.IsPath());
    CHECK_FALSE(p.IsFileId());
    CHECK(f.IsFileId());
    CHECK_FALSE(f.IsPath());

    // Describe is what reaches logs and AssetPreload::Paths; an id has no
    // name, so it renders as one that cannot be mistaken for a path.
    CHECK(p.Describe() == "Units/Human/Footman/Footman.mdx");
    CHECK(f.Describe() == "#1234567");
}

TEST_CASE("ContentRef::Empty covers both discriminants") {
    CHECK(ContentRef{}.Empty());
    CHECK(ContentRef::FromPath("").Empty());
    CHECK_FALSE(ContentRef::FromPath("x.blp").Empty());

    // CASC treats fileDataID 0 as unset rather than as a valid id, so a
    // zero-id ref names nothing. Acquire and Request both reject on this.
    CHECK(ContentRef::FromFileId(0).Empty());
    CHECK_FALSE(ContentRef::FromFileId(1).Empty());
}

// ---------------------------------------------------------------------------
// The no-cross-discriminant-dedup rule
//
// AssetManager::refToSlot_ is an `unordered_map<ContentRef, SlotId>` and
// Acquire's whole lookup is one `find(norm)` against it. So the documented
// behaviour — "the same bytes acquired once by path and once by fileDataID
// occupy two slots and fetch twice" — is exactly the key semantics below and
// nothing else. Pinning them here pins the rule.
//
// The rule is deliberate, not an oversight: resolving it would need a path↔id
// resolver we do not have, and WoW content is id-addressed while WC3 content
// is path-addressed, so the two never meet inside one model. A future change
// that folds ids into paths, or drops the discriminant out of the hash to
// "simplify" it, breaks it — and would otherwise do so silently.
// ---------------------------------------------------------------------------

TEST_CASE("a path ref and an id ref are distinct map keys") {
    const auto byPath = ContentRef::FromPath("123");
    const auto byId = ContentRef::FromFileId(123);

    CHECK(byPath != byId);
    CHECK_FALSE(byPath == byId);

    std::unordered_map<ContentRef, int> slots;
    slots[byPath] = 1;
    slots[byId] = 2;
    REQUIRE(slots.size() == 2);
    CHECK(slots.at(byPath) == 1);
    CHECK(slots.at(byId) == 2);

    // std::hash folds the discriminant in for this reason. Equal hashes would
    // still be *correct* (a map compares on ==), so this is a quality check,
    // not a correctness one — but a collision on every single pair would make
    // a mixed-product slot table degenerate.
    CHECK(std::hash<ContentRef>{}(byPath) != std::hash<ContentRef>{}(byId));
}

TEST_CASE("refs of the same discriminant dedup normally") {
    std::unordered_set<ContentRef> seen;
    seen.insert(ContentRef::FromPath("a/b.blp"));
    seen.insert(ContentRef::FromPath("a/b.blp"));
    seen.insert(ContentRef::FromFileId(7));
    seen.insert(ContentRef::FromFileId(7));
    CHECK(seen.size() == 2);
}

// ---------------------------------------------------------------------------
// The bound mirror
//
// `AssetsView::Kind` is a hand-written copy of `assets::AssetKind` and is
// `@bind`, so a divergence reaches the C ABI and the Rust crate.
//
// The *pairing* of the two enums is asserted at compile time in
// renderer_api.cpp — one static_assert per value — because that is the only
// translation unit that legitimately sees both. This file cannot: the
// internal header pulls in cornflakes, which the test targets do not link.
//
// What is pinned here is the other half, which no static_assert covers: the
// absolute values the C ABI emits. Together the two say "the mirror agrees
// AND the numbers are these numbers".
// ---------------------------------------------------------------------------

TEST_CASE("AssetsView::Kind holds the values the C ABI emits") {
    using Kind = whiteout::flakes::AssetsView::Kind;
    // Written out rather than derived: the point is to notice a renumbering,
    // and a derived expectation would renumber along with it.
    CHECK(static_cast<int>(Kind::Texture) == 0);
    CHECK(static_cast<int>(Kind::Model) == 1);
    CHECK(static_cast<int>(Kind::Effect) == 2);
    CHECK(static_cast<int>(Kind::Data) == 3);
}

// ---------------------------------------------------------------------------
// Product detection
// ---------------------------------------------------------------------------

TEST_CASE("build-product strings normalise to a ProductId") {
    CHECK(io::ProductIdFromBuildProduct("War3") == ProductId::Wc3);
    CHECK(io::ProductIdFromBuildProduct("w3") == ProductId::Wc3);
    CHECK(io::ProductIdFromBuildProduct("WoW") == ProductId::Wow);
    CHECK(io::ProductIdFromBuildProduct("wow_classic") == ProductId::Wow);
    CHECK(io::ProductIdFromBuildProduct("Sc2") == ProductId::Sc2);
    CHECK(io::ProductIdFromBuildProduct("s2") == ProductId::Sc2);
}

TEST_CASE("product detection is case-insensitive and fails closed") {
    CHECK(io::ProductIdFromBuildProduct("WAR3") == ProductId::Wc3);
    CHECK(io::ProductIdFromBuildProduct("wAr3") == ProductId::Wc3);

    // Neutral rather than a guess. A scene with no product is a real state,
    // so there is no need for a wrong answer to stand in for "don't know" —
    // and a wrong one would pick the wrong render profile for everything in
    // that scene.
    CHECK(io::ProductIdFromBuildProduct("") == ProductId::Neutral);
    CHECK(io::ProductIdFromBuildProduct("d3") == ProductId::Neutral);
    CHECK(io::ProductIdFromBuildProduct("prometheus") == ProductId::Neutral);
    // Not prefix-matched: "w3" must not swallow an unrelated future product
    // whose name happens to start with it.
    CHECK(io::ProductIdFromBuildProduct("w3something") == ProductId::Neutral);
}

TEST_CASE("ProductId holds the values the C ABI emits") {
    // Same mirror hazard as AssetsView::Kind, same split: the pairing against
    // renderer::core::ProductId is a static_assert in renderer_api.cpp.
    CHECK(static_cast<int>(ProductId::Neutral) == 0);
    CHECK(static_cast<int>(ProductId::Wc3) == 1);
    CHECK(static_cast<int>(ProductId::Wow) == 2);
    CHECK(static_cast<int>(ProductId::Sc2) == 3);
}

// ---------------------------------------------------------------------------
// The fileDataID route, end to end
//
// The one link the plan called out as missing: a fileDataID could not reach
// the disk because every step was keyed on a path string. This drives the
// whole new route — ContentRef → Request → worker → casc::Storage::readFile
// (i32, FileIdHint) — against a real install.
//
// Needs a World of Warcraft CASC to read from, which cannot be committed, so
// the install is configured out of band and the test skips without it:
//
//   set WDX_TEST_WOW_INSTALL=D:\Games\World of Warcraft
//
// The *id* is discovered by enumeration rather than hardcoded, because a
// literal fileDataID is a fixture with an expiry date — a patch can retire
// one, and the test would then fail for a reason that has nothing to do with
// this code. `WDX_TEST_WOW_FILEID` overrides when a specific id matters.
//
// Skipped is not passed: a green suite on a machine with no install has not
// exercised this route at all.
// ---------------------------------------------------------------------------

namespace {
const char* EnvOrNull(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

// First id-addressed entry small enough to read quickly. Enumeration stops at
// the first hit, so this costs a scan prefix rather than a whole manifest.
whiteout::flakes::u32 DiscoverFileId(const std::string& root) {
    if (const char* forced = EnvOrNull("WDX_TEST_WOW_FILEID"))
        return static_cast<whiteout::flakes::u32>(std::stoul(forced));

    std::string err;
    auto storage = whiteout::storages::casc::Storage::open(root, &err);
    if (!storage)
        storage = whiteout::storages::casc::Storage::open(root + "/Data", &err);
    if (!storage)
        return 0;

    whiteout::flakes::u32 found = 0;
    storage->enumerate([&](const whiteout::storages::casc::EnumerateEntry& e) {
        if (e.fileDataId > 0 && e.fileSize > 0 && e.fileSize < 1u << 20) {
            found = static_cast<whiteout::flakes::u32>(e.fileDataId);
            return false; // stop
        }
        return true;
    });
    return found;
}
} // namespace

TEST_CASE("a CASC file reads by fileDataID", "[casc]") {
    const char* rootEnv = EnvOrNull("WDX_TEST_WOW_INSTALL");
    if (!rootEnv) {
        SKIP("set WDX_TEST_WOW_INSTALL to run this");
    }
    const std::string root = rootEnv;

    const auto fileId = DiscoverFileId(root);
    if (fileId == 0) {
        SKIP(std::string("no id-addressed entry found in ") + root);
    }

    io::FileContentProvider provider;
    provider.SetInstallPath(root);
    if (!provider.HasCasc()) {
        SKIP(std::string("no CASC storage opened at ") + root);
    }

    std::string ext;
    auto bytes = provider.ReadFile(ContentRef::FromFileId(fileId), &ext);
    REQUIRE(bytes.has_value());
    CHECK_FALSE(bytes->empty());
    // The root manifest is id-keyed, so there is no name to take an extension
    // from and the provider says so rather than inventing one. Consumers that
    // decode by extension must know the kind from whatever produced the id.
    CHECK(ext.empty());
}

TEST_CASE("an unresolvable fileDataID is a clean miss", "[casc]") {
    const char* root = EnvOrNull("WDX_TEST_WOW_INSTALL");
    if (!root) {
        SKIP("set WDX_TEST_WOW_INSTALL to run this");
    }

    io::FileContentProvider provider;
    provider.SetInstallPath(root);
    if (!provider.HasCasc()) {
        SKIP(std::string("no CASC storage opened at ") + root);
    }

    // No disk fallback, no MPQ fallback, no extension probing — a fileDataID
    // resolves in the root manifest or not at all. What must not happen is a
    // hang: the request has to retire so Wait() returns.
    auto bytes = provider.ReadFile(ContentRef::FromFileId(0xFFFFFFFEu));
    CHECK_FALSE(bytes.has_value());
}

TEST_CASE("a provider rejects an empty ref outright") {
    io::FileContentProvider provider;
    const auto id = provider.Request(ContentRef{}, [](io::RequestResult&&) {});
    CHECK(id == io::kInvalidRequestId);
    CHECK(provider.Request(ContentRef::FromFileId(0), [](io::RequestResult&&) {}) ==
          io::kInvalidRequestId);
}
