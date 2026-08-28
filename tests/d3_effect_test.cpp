// ============================================================================
// Diablo III effect wiring — how a `.prt` reaches an actor.
//
// Two things are gated here, and they are different in kind.
//
// **The census.** The claim this whole route rests on is that the effect group
// carries the bulk of the shipped particles. That was first measured by a
// throwaway byte scan over the corpus; the sweeps below re-derive the same
// numbers through the real `parseEffectGroup` / `parseActor`, which is the only
// way the claim is worth anything — two independent readers agreeing on 16,942
// references, or one reader agreeing with itself.
//
// **The resolver.** Synthetic groups, built in memory, for the decisions a
// viewer has to make that the engine makes with an actor in hand: which items
// of a group fire, what happens to a chance of zero, and what a cycle does.
//
// The sweeps skip without `C:/Projects/WhiteoutLib/Corpus/D3` (override with
// WDX_TEST_D3_CORPUS). Skipped is not passed — each sweep prints its counts.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/d3/d3_effect_resolver.h"
#include "io/d3/d3_sno_cache.h"
#include "whiteout/flakes/content_provider.h"

#include <whiteout/sno/d3/native/d3_native.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
namespace d3n = ::whiteout::sno::d3::native;
namespace wio = ::whiteout::flakes::io;
namespace wd3 = ::whiteout::flakes::io::d3;
using namespace ::whiteout;

namespace {

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_D3_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/D3");
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

/// Every file of one extension under `root/<group>`, falling back to a flat
/// extraction at the root.
std::vector<fs::path> FindGroup(const fs::path& root, const char* dir, const char* ext) {
    std::vector<fs::path> out;
    std::error_code ec;
    fs::path d = root / dir;
    if (!fs::is_directory(d, ec))
        d = root;
    if (!fs::is_directory(d, ec))
        return out;
    for (fs::directory_iterator it(d, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (it->is_regular_file(ec) && it->path().extension() == ext)
            out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// A provider over a fixed id -> bytes map. Requests complete inside Request,
/// which is all `IContentProvider::ReadFile` needs: it submits, then waits, and
/// a wait on a request that already fired is a no-op.
class MapProvider final : public wio::IContentProvider {
public:
    void Add(i32 sno, std::vector<u8> bytes) {
        files_[sno] = std::move(bytes);
    }

    wio::RequestId Request(const wio::ContentRef& ref, wio::CompletionCallback cb) override {
        wio::RequestResult r;
        if (ref.IsFileId()) {
            auto it = files_.find(static_cast<i32>(ref.fileId));
            if (it != files_.end()) {
                r.ok = true;
                r.data = it->second;
            }
        }
        cb(std::move(r));
        return ++next_;
    }
    void Wait(wio::RequestId) override {}
    void Cancel(wio::RequestId) override {}
    void Pump() override {}

private:
    std::unordered_map<i32, std::vector<u8>> files_;
    wio::RequestId next_ = 0;
};

// ---- synthetic builders --------------------------------------------------

d3n::TriggerEvent ParticleEvent(i32 sno, const char* hardpoint = "Default", i32 chance = 255) {
    d3n::TriggerEvent ev;
    ev.tConditions.nChance = chance;
    ev.tPayload.eSnoGroup = 27;
    ev.tPayload.dwNameHandle = sno;
    ev.tHardpoint0.szName = hardpoint;
    return ev;
}

d3n::TriggerEvent GroupEvent(i32 sno) {
    d3n::TriggerEvent ev;
    ev.eTriggerType = 16;
    ev.tConditions.nChance = 255;
    ev.tPayload.eSnoGroup = 14;
    ev.tPayload.dwNameHandle = sno;
    ev.tHardpoint0.szName = "Default";
    return ev;
}

/// A group, serialised to the shipped v47 byte layout so the test drives the
/// real parser rather than hand-filling a parsed struct.
std::vector<u8> EncodeGroup(i32 sno, i32 selectMode,
                            const std::vector<std::pair<i32, d3n::TriggerEvent>>& items) {
    const std::size_t kItem = 480;
    std::vector<u8> b(16 + 120 + kItem * items.size(), 0);
    auto put = [&](std::size_t off, i32 v) { std::memcpy(b.data() + off, &v, 4); };
    put(0, static_cast<i32>(0xDEADBEEFu));
    b[4] = 47;
    put(16 + 0, sno);
    put(16 + 16, 120);                                        // arEffectItems offset
    put(16 + 20, static_cast<i32>(kItem * items.size()));      // size
    put(16 + 24, static_cast<i32>(items.size()));              // count
    put(16 + 48, selectMode);
    put(16 + 52, -1);                                          // snoPower
    for (std::size_t i = 0; i < items.size(); ++i) {
        const std::size_t it = 16 + 120 + kItem * i;
        put(it + 0, items[i].first);                           // nWeight
        // tEvent: MsgTriggeredEvent{i32 eMessageType; TriggerEvent}
        const std::size_t te = it + 68 + 4;
        put(it + 68, 5000);
        const auto& ev = items[i].second;
        put(te + 0, ev.eTriggerType);
        put(te + 4, ev.tConditions.nChance);
        put(te + 44, ev.tPayload.eSnoGroup);
        put(te + 48, ev.tPayload.dwNameHandle);
        std::memcpy(b.data() + te + 68, ev.tHardpoint0.szName.c_str(),
                    std::min<std::size_t>(63, ev.tHardpoint0.szName.size()));
    }
    return b;
}

} // namespace

// ==========================================================================
// The census
// ==========================================================================

TEST_CASE("d3 effect: every shipped .efg parses and tiles exactly",
          "[d3][effect][corpus]") {
    const auto files = FindGroup(CorpusRoot(), "EffectGroup", ".efg");
    if (files.empty()) {
        WARN("no D3 EffectGroup corpus; skipping");
        return;
    }

    std::size_t parsed = 0, items = 0, empty = 0;
    std::map<i32, std::size_t> modes;
    std::size_t lookLinks = 0, msgIs5000 = 0;

    for (const auto& f : files) {
        const auto bytes = ReadAll(f);
        auto g = d3n::parseEffectGroup(bytes);
        REQUIRE(g.has_value());
        ++parsed;

        // The struct declares its own count; the array the offset/size pair
        // describes has to agree, or the 480-byte stride is wrong.
        REQUIRE(static_cast<std::size_t>(g->dwEffectItemCount) == g->arEffectItems.size());
        // 16 header + 120 struct + the payload, with no slack.
        REQUIRE(bytes.size() == 136 + 480 * g->arEffectItems.size());
        if (g->arEffectItems.empty())
            ++empty;
        modes[g->eSelectMode] += 1;
        items += g->arEffectItems.size();
        for (const auto& it : g->arEffectItems) {
            if (!it.szLookLink.empty())
                ++lookLinks;
            if (it.tEvent.eMessageType == 5000)
                ++msgIs5000;
        }
    }

    // Measured over the full 6,426-file corpus.
    CHECK(parsed == files.size());
    CHECK(msgIs5000 == items); // 30,463/30,463 — the group IS the trigger
    if (files.size() == 6426) {
        CHECK(items == 30463);
        CHECK(empty == 38);
        CHECK(modes[2] == 4888);
        CHECK(modes[0] == 79);
        CHECK(modes[1] == 8);
        CHECK(modes[10] == 73);
        CHECK(modes[15] == 280);
        CHECK(lookLinks == 30463 - 30024);
    }
    std::printf("[d3 efg] %zu files, %zu items, %zu empty, %zu look-links; modes:",
                parsed, items, empty, lookLinks);
    for (auto& [m, n] : modes)
        std::printf(" %d=%zu", m, n);
    std::printf("\n");
}

TEST_CASE("d3 effect: the effect group is the dominant route to a particle",
          "[d3][effect][corpus]") {
    const auto root = CorpusRoot();
    const auto efgs = FindGroup(root, "EffectGroup", ".efg");
    const auto acrs = FindGroup(root, "Actor", ".acr");
    const auto prts = FindGroup(root, "Particle", ".prt");
    if (efgs.empty() || acrs.empty() || prts.empty()) {
        WARN("no D3 corpus; skipping");
        return;
    }

    // The id map every reference is checked against. A "reference" that does
    // not land in it is not a reference.
    std::set<i32> prtIds;
    for (const auto& f : prts) {
        const auto b = ReadAll(f);
        if (b.size() >= 20) {
            i32 id = 0;
            std::memcpy(&id, b.data() + 16, 4);
            prtIds.insert(id);
        }
    }

    std::set<i32> viaGroup, viaActor;
    std::size_t groupRefs = 0, actorRefs = 0, unresolved = 0;
    std::map<i32, std::size_t> msgTypes;

    for (const auto& f : efgs) {
        auto g = d3n::parseEffectGroup(ReadAll(f));
        if (!g)
            continue;
        for (const auto& it : g->arEffectItems) {
            const auto& p = it.tEvent.tEvent.tPayload;
            if (p.eSnoGroup != 27)
                continue;
            ++groupRefs;
            if (prtIds.count(p.dwNameHandle))
                viaGroup.insert(p.dwNameHandle);
            else
                ++unresolved;
        }
    }

    for (const auto& f : acrs) {
        auto a = d3n::parseActor(ReadAll(f));
        if (!a)
            continue;
        for (const auto& ev : a->arMsgTriggeredEvents) {
            msgTypes[ev.eMessageType] += 1;
            const auto& p = ev.tEvent.tPayload;
            if (p.eSnoGroup != 27)
                continue;
            ++actorRefs;
            if (prtIds.count(p.dwNameHandle))
                viaActor.insert(p.dwNameHandle);
        }
    }

    std::printf("[d3 route] %zu .prt | efg refs=%zu uniq=%zu | acr refs=%zu uniq=%zu"
                " | msg1000=%zu\n",
                prtIds.size(), groupRefs, viaGroup.size(), actorRefs, viaActor.size(),
                msgTypes[1000]);

    // The whole reason this route exists: the group half is more than twice
    // the actor half, and both resolve into the real id map.
    CHECK(viaGroup.size() > viaActor.size() * 2);
    CHECK(unresolved <= 1); // one shipped dangling reference, measured

    if (efgs.size() == 6426 && acrs.size() == 19177 && prts.size() == 21593) {
        CHECK(groupRefs == 16943);
        CHECK(viaGroup.size() == 11849);
        CHECK(actorRefs == 10690);
        CHECK(viaActor.size() == 4430);
        // Message 1000 is 57% of every event an actor carries, which is what
        // makes it the one a viewer fires.
        CHECK(msgTypes[1000] == 15630);
        CHECK(msgTypes[17] == 2334);
    }
}

TEST_CASE("d3 effect: MsgTriggeredEvent's interior is a TriggerEvent",
          "[d3][effect][corpus]") {
    const auto acrs = FindGroup(CorpusRoot(), "Actor", ".acr");
    if (acrs.empty()) {
        WARN("no D3 Actor corpus; skipping");
        return;
    }
    // The +4 offset is the claim. If it were wrong, the four inline char[64]
    // name fields would land on binary and the payload group on a float.
    std::size_t events = 0, cleanNames = 0, knownGroups = 0;
    static const std::set<i32> kGroups{-1, 1, 5, 14, 17, 23, 27, 32, 38, 40, 45, 68};
    for (const auto& f : acrs) {
        auto a = d3n::parseActor(ReadAll(f));
        if (!a)
            continue;
        for (const auto& ev : a->arMsgTriggeredEvents) {
            ++events;
            auto printable = [](const std::string& s) {
                return std::all_of(s.begin(), s.end(),
                                   [](char c) { return c >= 0x20 && c < 0x7F; });
            };
            if (printable(ev.tEvent.tHardpoint0.szName) &&
                printable(ev.tEvent.tHardpoint1.szName) && printable(ev.tEvent.szLookName) &&
                printable(ev.tEvent.szConstraintName))
                ++cleanNames;
            if (kGroups.count(ev.tEvent.tPayload.eSnoGroup))
                ++knownGroups;
            // Every shipped constraint link is empty; a misalignment fills it.
            CHECK(ev.tEvent.szConstraintName.empty());
        }
    }
    CHECK(events > 0);
    CHECK(cleanNames == events);
    CHECK(knownGroups == events);
    std::printf("[d3 mte] %zu events, all four name fields clean, all payload groups known\n",
                events);
}

TEST_CASE("d3 effect: what the spawn message actually reaches", "[d3][effect][corpus]") {
    // The reachability number. `BoneStructure::snoParticle` — the route this
    // replaced — is authored on 5 of the first 2,500 appearances; this is the
    // same measurement for the route that replaced it, taken through the real
    // resolver rather than by counting references.
    const auto root = CorpusRoot();
    const auto acrs = FindGroup(root, "Actor", ".acr");
    const auto efgs = FindGroup(root, "EffectGroup", ".efg");
    if (acrs.empty() || efgs.empty()) {
        WARN("no D3 corpus; skipping");
        return;
    }

    MapProvider prov;
    for (const auto& f : efgs) {
        auto b = ReadAll(f);
        if (b.size() >= 20) {
            i32 id = 0;
            std::memcpy(&id, b.data() + 16, 4);
            prov.Add(id, std::move(b));
        }
    }
    wio::D3SnoCache cache(&prov);

    std::size_t actors = 0, withFx = 0, effects = 0, viaGroup = 0, rootHp = 0;
    std::set<i32> distinct;
    std::map<std::string, std::size_t> hardpoints;
    for (const auto& f : acrs) {
        auto a = d3n::parseActor(ReadAll(f));
        if (!a)
            continue;
        ++actors;
        const auto fx = wd3::ResolveActorEffects(*a, wd3::kD3MsgActorSpawned, cache);
        if (fx.empty())
            continue;
        ++withFx;
        effects += fx.size();
        for (const auto& e : fx) {
            distinct.insert(e.snoParticle);
            if (e.fromEffectGroup >= 0)
                ++viaGroup;
            if (wd3::IsD3RootHardpoint(e.hardpoint))
                ++rootHp;
            else
                hardpoints[e.hardpoint] += 1;
        }
    }

    std::printf("[d3 reach] %zu actors | %zu carry spawn FX (%.1f%%) | %zu systems,"
                " %zu distinct .prt | %zu through an .efg | %zu at the model origin\n",
                actors, withFx, 100.0 * double(withFx) / double(actors), effects,
                distinct.size(), viaGroup, rootHp);
    std::printf("[d3 reach] top hardpoints:");
    std::vector<std::pair<std::string, std::size_t>> hp(hardpoints.begin(), hardpoints.end());
    std::sort(hp.begin(), hp.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (std::size_t i = 0; i < hp.size() && i < 8; ++i)
        std::printf(" %s=%zu", hp[i].first.c_str(), hp[i].second);
    std::printf("\n");

    // The bone route reached 3 distinct files. Anything in the same order of
    // magnitude would mean the resolver is not wired up.
    CHECK(withFx > actors / 10);
    CHECK(distinct.size() > 1000);
    // The effect-group indirection is a large minority HERE and a majority
    // across the corpus, which is not a contradiction: 11,849 distinct `.prt`
    // are named only by an `.efg`, but those groups are mostly reached from
    // the *other* messages and from animation events. On the spawn message
    // specifically the actor names most of its own ambient systems directly.
    CHECK(viaGroup > effects / 5);
}

// ==========================================================================
// The resolver
// ==========================================================================

TEST_CASE("d3 effect: select mode 2 plays every item, 0 plays one", "[d3][effect]") {
    MapProvider prov;
    prov.Add(500, EncodeGroup(500, 2,
                              {{100, ParticleEvent(10)}, {100, ParticleEvent(11)},
                               {100, ParticleEvent(12)}}));
    // Mode 0 is a weighted draw; the deterministic stand-in is the heaviest
    // item, which for a real uniform group is any of them.
    prov.Add(501, EncodeGroup(501, 0, {{10, ParticleEvent(20)}, {90, ParticleEvent(21)}}));
    wio::D3SnoCache cache(&prov);

    std::vector<wd3::ResolvedEffect> all;
    wd3::ExpandD3TriggerEvent(GroupEvent(500), cache, {}, all);
    REQUIRE(all.size() == 3);
    CHECK(all[0].snoParticle == 10);
    CHECK(all[2].snoParticle == 12);
    CHECK(all[0].fromEffectGroup == 500);

    std::vector<wd3::ResolvedEffect> one;
    wd3::ExpandD3TriggerEvent(GroupEvent(501), cache, {}, one);
    REQUIRE(one.size() == 1);
    CHECK(one[0].snoParticle == 21);
    CHECK(one[0].weight == 90);
}

TEST_CASE("d3 effect: a chance of zero drops the item", "[d3][effect]") {
    // 813 shipped items carry nChance 0. TriggerConditions_RollDelay returns
    // -1 for them before it ever draws, so they are authored off — not
    // improbable, off.
    MapProvider prov;
    prov.Add(600, EncodeGroup(600, 2,
                              {{100, ParticleEvent(30, "Default", 0)},
                               {100, ParticleEvent(31, "Default", 255)}}));
    wio::D3SnoCache cache(&prov);

    std::vector<wd3::ResolvedEffect> out;
    wd3::ExpandD3TriggerEvent(GroupEvent(600), cache, {}, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].snoParticle == 31);
    CHECK(out[0].chance == 1.0f);
}

TEST_CASE("d3 effect: mode 10 picks by look link", "[d3][effect]") {
    MapProvider prov;
    auto bytes = EncodeGroup(700, 10, {{100, ParticleEvent(40)}, {100, ParticleEvent(41)}});
    // Item 0 -> "A", item 1 -> "B_rare", written into szLookLink at item+4.
    std::memcpy(bytes.data() + 136 + 4, "A", 1);
    std::memcpy(bytes.data() + 136 + 480 + 4, "B_rare", 6);
    prov.Add(700, std::move(bytes));
    wio::D3SnoCache cache(&prov);

    std::vector<wd3::ResolvedEffect> out;
    wd3::ExpandD3TriggerEvent(GroupEvent(700), cache, "b_RARE", out); // case-insensitive
    REQUIRE(out.size() == 1);
    CHECK(out[0].snoParticle == 41);

    std::vector<wd3::ResolvedEffect> none;
    wd3::ExpandD3TriggerEvent(GroupEvent(700), cache, "C", none);
    CHECK(none.empty());
}

TEST_CASE("d3 effect: a cycle terminates", "[d3][effect]") {
    MapProvider prov;
    prov.Add(800, EncodeGroup(800, 2, {{100, GroupEvent(801)}, {100, ParticleEvent(50)}}));
    prov.Add(801, EncodeGroup(801, 2, {{100, GroupEvent(800)}, {100, ParticleEvent(51)}}));
    wio::D3SnoCache cache(&prov);

    std::vector<wd3::ResolvedEffect> out;
    wd3::ExpandD3TriggerEvent(GroupEvent(800), cache, {}, out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].snoParticle == 51); // the nested group resolves first
    CHECK(out[1].snoParticle == 50);
}

TEST_CASE("d3 effect: an actor fires only the matching message", "[d3][effect]") {
    MapProvider prov;
    wio::D3SnoCache cache(&prov);

    d3n::Actor actor;
    auto add = [&](i32 msg, i32 sno) {
        d3n::MsgTriggeredEvent m;
        m.eMessageType = msg;
        m.tEvent = ParticleEvent(sno);
        actor.arMsgTriggeredEvents.push_back(std::move(m));
    };
    add(wd3::kD3MsgActorSpawned, 60);
    add(wd3::kD3MsgActorEnded, 61);
    add(wd3::kD3MsgActorSpawned, 62);
    // MsgTriggeredEvent_MatchesKey also compares nSubKey against the key's aux
    // word, which the spawn key leaves at 0.
    add(wd3::kD3MsgActorSpawned, 63);
    actor.arMsgTriggeredEvents.back().tEvent.tConditions.nSubKey = 7;

    auto fx = wd3::ResolveActorEffects(actor, wd3::kD3MsgActorSpawned, cache);
    REQUIRE(fx.size() == 2);
    CHECK(fx[0].snoParticle == 60);
    CHECK(fx[1].snoParticle == 62);

    auto ended = wd3::ResolveActorEffects(actor, wd3::kD3MsgActorEnded, cache);
    REQUIRE(ended.size() == 1);
    CHECK(ended[0].snoParticle == 61);
}

TEST_CASE("d3 effect: hardpoint lookup is case-insensitive and knows the root",
          "[d3][effect]") {
    d3n::Appearances app;
    app.arHardpoints.push_back({});
    app.arHardpoints.back().szName = "HP_head";
    app.arHardpoints.back().nBoneIndex = 4;
    app.arHardpoints.push_back({});
    app.arHardpoints.back().szName = "HP_leftWeapon";
    app.arHardpoints.back().nBoneIndex = 9;

    // The engine interns "HP_head" but 323 shipped events spell it "HP_Head";
    // a byte compare loses those.
    CHECK(wd3::FindD3Hardpoint(app, "HP_Head") == 0);
    CHECK(wd3::FindD3Hardpoint(app, "HP_head") == 0);
    CHECK(wd3::FindD3Hardpoint(app, "HP_LEFTWEAPON") == 1);
    CHECK(wd3::FindD3Hardpoint(app, "HP_chest") == -1);

    for (const char* n : {"", "Default", "None", "- None -", "Don't Override"}) {
        CHECK(wd3::IsD3RootHardpoint(n));
        CHECK(wd3::FindD3Hardpoint(app, n) == -1);
    }
}
