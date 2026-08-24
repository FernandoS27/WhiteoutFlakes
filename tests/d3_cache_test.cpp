// ============================================================================
// D3SnoCache — gate D3-G5, the "nothing is read or parsed twice" claim.
//
// A counter gate, not a timing one, and deliberately so: "it got faster" is not
// a claim a CI run can make, "it read the file once" is.
//
// Two halves:
//
//  * **Offline** (runs in CI): a stub content provider that counts reads, over
//    corpus bytes fed in directly. This is where the sharing claim is actually
//    proven — N actors naming one appearance parse it once — because a stub
//    provider can count what a CASC storage cannot.
//
//  * **Eviction under a held reference.** The cache hands out
//    `shared_ptr<const T>` that live models keep, so getting eviction wrong is
//    a dangling read rather than a wrong picture. The rule that makes it
//    impossible is "eviction only ever drops the cache's OWN reference", and
//    that rule is worth a direct test: evict under a held reference and read
//    through it.
//
// Corpus root: WDX_TEST_D3_CORPUS. Skipped is not passed.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/d3/d3_sno_cache.h"
#include "whiteout/flakes/content_provider.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace d3n = ::whiteout::sno::d3::native;
using namespace ::whiteout;
using ::whiteout::flakes::ContentRef;
using ::whiteout::flakes::io::D3SnoCache;

namespace {

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_D3_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/D3");
}

std::vector<fs::path> FindFiles(const fs::path& dir, const char* ext) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (it->is_regular_file(ec) && it->path().extension() == ext)
            out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
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

i32 SnoIdOf(const std::vector<u8>& bytes) {
    return ::whiteout::flakes::io::D3SnoIdOfBytes(bytes);
}

// Serves a fixed id -> bytes map and counts every read. Everything else on the
// interface is a stub: the cache only ever calls ReadFile.
class CountingProvider final : public ::whiteout::flakes::io::IContentProvider {
public:
    std::map<u32, std::vector<u8>> files;
    std::map<u32, std::size_t> reads;
    std::size_t totalReads = 0;

    using RequestId = ::whiteout::flakes::io::RequestId;
    using CompletionCallback = ::whiteout::flakes::io::CompletionCallback;

    RequestId Request(const ContentRef& ref, CompletionCallback cb) override {
        // Synchronous: the cache reaches the provider through ReadFile, which
        // is Request + Wait, so firing the callback inline is the whole of what
        // this has to do.
        ::whiteout::flakes::io::RequestResult r;
        if (ref.IsFileId()) {
            ++totalReads;
            ++reads[ref.fileId];
            auto it = files.find(ref.fileId);
            if (it != files.end()) {
                r.ok = true;
                r.data = it->second;
            }
        }
        if (cb)
            cb(std::move(r));
        return 1;
    }
    void Wait(RequestId) override {}
    void Cancel(RequestId) override {}
    void Pump() override {}
};

} // namespace

TEST_CASE("D3SnoCache reads and parses each asset exactly once", "[d3][cache]") {
    const auto files = FindFiles(CorpusRoot() / "Appearances", ".app");
    if (files.empty()) {
        WARN("No D3 corpus at " << (CorpusRoot() / "Appearances").string()
                                << " (set WDX_TEST_D3_CORPUS). SKIPPED, not passed.");
        return;
    }

    // Three appearances is enough to tell "cached" from "keyed wrong": one
    // would pass with a cache that ignores its key.
    CountingProvider provider;
    std::vector<i32> ids;
    for (std::size_t i = 0; i < files.size() && ids.size() < 3; ++i) {
        auto bytes = ReadAll(files[i]);
        const i32 sno = SnoIdOf(bytes);
        if (sno <= 0 || provider.files.count(static_cast<u32>(sno)))
            continue;
        provider.files[static_cast<u32>(sno)] = std::move(bytes);
        ids.push_back(sno);
    }
    REQUIRE(ids.size() == 3);

    D3SnoCache cache(&provider);

    // 594 actors name one appearance in the shipped install. This is that,
    // scaled to something a test can run: spawn the same three appearances
    // fifty times each and assert the file was read once and parsed once.
    constexpr int kSpawns = 50;
    for (int n = 0; n < kSpawns; ++n) {
        for (i32 sno : ids) {
            auto app = cache.Appearance(sno);
            REQUIRE(app != nullptr);
            CHECK(app->dwSnoId == sno);
        }
    }

    const auto st = cache.GetStats();
    std::printf("[d3-g5] %d spawns x %zu appearances: %zu reads, %zu parses, %zu hits, "
                "%zu misses, %zu resident bytes\n",
                kSpawns, ids.size(), st.reads, st.parses, st.hits, st.misses, st.bytesResident);

    // The whole claim, as counters.
    CHECK(st.reads == ids.size());
    CHECK(st.parses == ids.size());
    CHECK(st.misses == ids.size());
    CHECK(st.hits == ids.size() * (kSpawns - 1));
    for (i32 sno : ids)
        CHECK(provider.reads[static_cast<u32>(sno)] == 1);

    // Two appearances resolving to the same object would also produce these
    // numbers, so the identity is checked too.
    auto a = cache.Appearance(ids[0]);
    auto b = cache.Appearance(ids[1]);
    CHECK(a != b);
    CHECK(a == cache.Appearance(ids[0]));
}

TEST_CASE("D3SnoCache remembers a miss", "[d3][cache]") {
    // References are -1 constantly, and a live reference can still miss: 2 of
    // Barbarian_Male's 259 clip references do not resolve in the shipped
    // install. A remembered miss costs one hash lookup; a forgotten one costs a
    // CASC probe per actor, forever.
    CountingProvider provider; // serves nothing
    D3SnoCache cache(&provider);

    for (int i = 0; i < 20; ++i)
        CHECK(cache.Appearance(4242) == nullptr);

    const auto st = cache.GetStats();
    CHECK(st.reads == 1);
    CHECK(st.negativeMisses == 1);
    // Counted apart from `misses` on purpose, so "my model is missing things"
    // and "my cache is thrashing" stay distinguishable.
    CHECK(st.negativeHits == 19);
    CHECK(st.misses == 0);

    // -1 is D3's "no reference" and never becomes an entry at all.
    CHECK(cache.Appearance(-1) == nullptr);
    CHECK(cache.GetStats().reads == 1);
}

TEST_CASE("D3SnoCache eviction drops only the cache's own reference", "[d3][cache]") {
    const auto files = FindFiles(CorpusRoot() / "Appearances", ".app");
    if (files.empty()) {
        WARN("No D3 corpus. SKIPPED, not passed.");
        return;
    }

    CountingProvider provider;
    std::vector<i32> ids;
    for (std::size_t i = 0; i < files.size() && ids.size() < 4; ++i) {
        auto bytes = ReadAll(files[i]);
        const i32 sno = SnoIdOf(bytes);
        if (sno <= 0 || provider.files.count(static_cast<u32>(sno)))
            continue;
        provider.files[static_cast<u32>(sno)] = std::move(bytes);
        ids.push_back(sno);
    }
    REQUIRE(ids.size() == 4);

    D3SnoCache cache(&provider);
    auto held = cache.Appearance(ids[0]);
    REQUIRE(held != nullptr);
    const i32 heldSno = held->dwSnoId;
    const std::size_t heldBones = held->arBones.size();

    // A budget of one byte evicts everything unpinned on the next insert.
    cache.SetBudgetBytes(1);
    for (std::size_t i = 1; i < ids.size(); ++i)
        (void)cache.Appearance(ids[i]);

    const auto st = cache.GetStats();
    std::printf("[d3-g5] eviction: %zu evictions, %zu entries left, %zu resident bytes\n",
                st.evictions, cache.EntryCount(), st.bytesResident);
    CHECK(st.evictions > 0);

    // The rule. A live model holding this pointer must be able to read through
    // it after the cache has forgotten the entry — eviction reclaims the
    // cache's reference and nothing else.
    CHECK(held->dwSnoId == heldSno);
    CHECK(held->arBones.size() == heldBones);

    // And the forgotten entry re-reads rather than returning a dangling one.
    const std::size_t readsBefore = st.reads;
    auto again = cache.Appearance(ids[0]);
    REQUIRE(again != nullptr);
    CHECK(cache.GetStats().reads > readsBefore);
    CHECK(again->dwSnoId == heldSno);
}

TEST_CASE("D3SnoCache: a typed hit of the wrong type returns null", "[d3][cache]") {
    const auto files = FindFiles(CorpusRoot() / "Appearances", ".app");
    if (files.empty()) {
        WARN("No D3 corpus. SKIPPED, not passed.");
        return;
    }
    CountingProvider provider;
    auto bytes = ReadAll(files[0]);
    const i32 sno = SnoIdOf(bytes);
    REQUIRE(sno > 0);
    provider.files[static_cast<u32>(sno)] = std::move(bytes);

    D3SnoCache cache(&provider);
    REQUIRE(cache.Appearance(sno) != nullptr);
    // SNO ids are globally unique across groups, so the key cannot collide —
    // but the *accessor a caller picks* can still be the wrong one, and a hit
    // that reinterprets the value is a silent cast into unrelated memory. The
    // stored group is what makes that checkable.
    CHECK(cache.Anim(sno) == nullptr);
    CHECK(cache.Material(sno) == nullptr);
    CHECK(cache.Actor(sno) == nullptr);
    CHECK(cache.GroupOf(sno) == d3n::Group::Appearance);
    // And the wrong-type asks cost no extra read.
    CHECK(cache.GetStats().reads == 1);
}

TEST_CASE("D3SnoCache: Clear forgets everything and keeps handed-out data alive", "[d3][cache]") {
    const auto files = FindFiles(CorpusRoot() / "Appearances", ".app");
    if (files.empty()) {
        WARN("No D3 corpus. SKIPPED, not passed.");
        return;
    }
    CountingProvider provider;
    auto bytes = ReadAll(files[0]);
    const i32 sno = SnoIdOf(bytes);
    REQUIRE(sno > 0);
    provider.files[static_cast<u32>(sno)] = std::move(bytes);

    D3SnoCache cache(&provider);
    auto held = cache.Appearance(sno);
    REQUIRE(held != nullptr);

    // The provider can be re-pointed at a different install, where a sno id
    // means something else. A cache that survived that would serve one
    // install's geometry against another's textures.
    cache.Clear();
    CHECK(cache.EntryCount() == 0);
    CHECK(cache.GetStats().bytesResident == 0);
    CHECK(held->dwSnoId == sno); // still readable

    const std::size_t readsBefore = cache.GetStats().reads;
    REQUIRE(cache.Appearance(sno) != nullptr);
    CHECK(cache.GetStats().reads == readsBefore + 1);
}
