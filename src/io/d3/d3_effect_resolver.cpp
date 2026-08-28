#include "io/d3/d3_effect_resolver.h"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace whiteout::flakes::io::d3 {

namespace {

/// The whole SNO-group vocabulary a TriggerEvent's payload can carry. Only
/// these two are assets this renderer can do anything with; Sound (5,313
/// refs), Explosion, Shakes, Rope, Trail, Light and the 1,290 assetless
/// records fall through, which is the honest answer for each of them.
constexpr i32 kGroupEffectGroup = 14;
constexpr i32 kGroupParticle = 27;

/// Guards against an effect group naming itself, directly or round a cycle.
/// Nothing in the corpus does, but the format permits it and one bad file
/// would hang a load.
constexpr int kMaxDepth = 8;

bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (usize i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb))
            return false;
    }
    return true;
}

void FillFromEvent(const d3n::TriggerEvent& ev, ResolvedEffect& r) {
    // nChance is a DT_PERCENT: one byte, and the three above it are zero in
    // 30,463 of 30,463 items. Reading the whole dword is therefore the same
    // number, but mask anyway — a single authoring stray would otherwise turn
    // into a chance of several million.
    r.chance = static_cast<f32>(ev.tConditions.nChance & 0xFF) / 255.0f;
    r.tmDelayMin = ev.tConditions.tmDelayMin;
    r.tmDelayRange = ev.tConditions.tmDelayRange;
    r.tmDuration = ev.tmDuration;
    r.color0 = ev.dwColor188;
    r.tmColor0 = ev.tmColor188Time;
    r.color1 = ev.dwColor190;
    r.tmColor1 = ev.tmColor190Time;
    r.hardpoint = ev.tHardpoint0.szName;
}

/// Which items of @p grp fire.
///
/// `EffectGroup_Play` (0x710008B230) switches on eSelectMode over sixteen
/// cases, and ten of them read live actor state — the monster's quality, its
/// equipped rune, a tag-map value. A viewer has none of that. Rather than
/// invent an actor, each of those takes the branch the engine itself takes
/// when its query comes back empty: item 0.
void SelectItems(const d3n::EffectGroup& grp, std::string_view lookName,
                 std::vector<usize>& out) {
    const usize n = grp.arEffectItems.size();
    if (n == 0)
        return;

    switch (grp.eSelectMode) {
    case 2: // 4,888 of 6,426 files, and the constructor's default.
        out.resize(n);
        for (usize i = 0; i < n; ++i)
            out[i] = i;
        return;

    case 0:
    case 1: {
        // Weighted-random over nWeight (mode 1 repeats the draw). Deterministic
        // stand-in: the first item of maximal weight, which is what a uniform
        // 100/100/100 group — 30,254 of 30,463 items — makes every draw anyway.
        usize best = 0;
        for (usize i = 1; i < n; ++i)
            if (grp.arEffectItems[i].nWeight > grp.arEffectItems[best].nWeight)
                best = i;
        out.push_back(best);
        return;
    }

    case 10:
        // The one actor-driven mode a viewer can actually answer: the look-link
        // is the Appearance's own look name.
        for (usize i = 0; i < n; ++i)
            if (!grp.arEffectItems[i].szLookLink.empty() &&
                IEquals(grp.arEffectItems[i].szLookLink, lookName))
                out.push_back(i);
        return;

    default:
        // 3, 5, 7, 9, 11, 12, 13, 14, 15, 16, 17 — quality, rune, tag map,
        // view mode. 1,449 files between them.
        out.push_back(0);
        return;
    }
}

void Expand(const d3n::TriggerEvent& ev, D3SnoCache& cache, std::string_view lookName, int depth,
            std::unordered_set<i32>& visited, i32 fromGroup, i32 weight,
            std::vector<ResolvedEffect>& out) {
    // 813 shipped items are authored to never fire. That is a decision, not a
    // die roll, so it is honoured here where the roll is not.
    if ((ev.tConditions.nChance & 0xFF) == 0)
        return;

    const i32 group = ev.tPayload.eSnoGroup;
    const i32 handle = ev.tPayload.dwNameHandle;
    if (handle == -1)
        return;

    if (group == kGroupParticle) {
        ResolvedEffect r;
        FillFromEvent(ev, r);
        r.snoParticle = handle;
        r.fromEffectGroup = fromGroup;
        r.weight = weight;
        out.push_back(std::move(r));
        return;
    }

    if (group != kGroupEffectGroup || depth >= kMaxDepth || !visited.insert(handle).second)
        return;

    auto grp = cache.EffectGroup(handle);
    if (!grp)
        return;

    std::vector<usize> picked;
    SelectItems(*grp, lookName, picked);
    for (usize i : picked) {
        const auto& item = grp->arEffectItems[i];
        Expand(item.tEvent.tEvent, cache, lookName, depth + 1, visited, handle, item.nWeight, out);
    }
}

} // namespace

bool IsD3RootHardpoint(std::string_view name) {
    // "Don't Override" means "keep whatever hardpoint the event that played me
    // was using". Nothing played an actor's own init events, so for them it
    // resolves to the origin like the other three.
    return name.empty() || IEquals(name, "Default") || IEquals(name, "None") ||
           IEquals(name, "- None -") || IEquals(name, "Don't Override");
}

i32 FindD3Hardpoint(const d3n::Appearances& app, std::string_view name) {
    if (IsD3RootHardpoint(name))
        return -1;
    for (usize i = 0; i < app.arHardpoints.size(); ++i)
        if (IEquals(app.arHardpoints[i].szName, name))
            return static_cast<i32>(i);
    return -1;
}

void ExpandD3TriggerEvent(const d3n::TriggerEvent& ev, D3SnoCache& cache,
                          std::string_view lookName, std::vector<ResolvedEffect>& out) {
    std::unordered_set<i32> visited;
    Expand(ev, cache, lookName, 0, visited, -1, 100, out);
}

std::vector<ResolvedEffect> ResolveActorEffects(const d3n::Actor& actor, i32 msgId,
                                                D3SnoCache& cache, std::string_view lookName) {
    std::vector<ResolvedEffect> out;
    for (const auto& ev : actor.arMsgTriggeredEvents) {
        if (ev.eMessageType != msgId)
            continue;
        // MsgTriggeredEvent_MatchesKey also compares tConditions.nSubKey
        // against the key's aux word, which is 0 for the spawn message. An
        // event that wants a non-zero sub-key is waiting for a different
        // caller, not for this one.
        if (ev.tEvent.tConditions.nSubKey != 0)
            continue;
        std::unordered_set<i32> visited;
        Expand(ev.tEvent, cache, lookName, 0, visited, -1, 100, out);
    }
    return out;
}

} // namespace whiteout::flakes::io::d3
