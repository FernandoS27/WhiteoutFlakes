#include "io/d3/d3_effect_resolver.h"

#include "renderer/animation/anim_math.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_set>

namespace whiteout::flakes::io::d3 {

namespace {

/// The whole SNO-group vocabulary a TriggerEvent's payload can carry. Only
/// these three are assets this renderer can do anything with; Sound (5,313
/// refs), Explosion, Shakes, Rope, Trail, Light and the 1,290 assetless
/// records fall through, which is the honest answer for each of them.
constexpr i32 kGroupActor = 1;
constexpr i32 kGroupEffectGroup = 14;
constexpr i32 kGroupParticle = 27;

/// `TriggerEvent_Execute`'s switch (0x7100211040). A Particle or Actor payload
/// rides one of the first two and an EffectGroup payload the third, at every
/// one of the three authoring sites; see the header.
constexpr i32 kTriggerSpawn = 0;
constexpr i32 kTriggerSpawnAttached = 25;
constexpr i32 kTriggerPlayEffectGroup = 16;

/// The one message a viewer can serve out of a `.prt`'s own event array.
/// `ParticleSystem_Spawn` fires 3000 when the system exists, `_Release` 3001,
/// `_RequestStop` 3002, `_EmitParticle` 3500 per particle and three sites 3501
/// when one dies. Only 3000 happens without a running simulation.
constexpr i32 kMsgParticleSpawned = 3000;

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

void ExpandParticleSpawn(i32 sno, D3SnoCache& cache, std::string_view lookName, int depth,
                         std::unordered_set<i32>& visited, std::vector<ResolvedEffect>& out);

void Expand(const d3n::TriggerEvent& ev, D3SnoCache& cache, std::string_view lookName, int depth,
            std::unordered_set<i32>& visited, i32 fromGroup, i32 weight,
            std::vector<ResolvedEffect>& out) {
    // 813 shipped items are authored to never fire. That is a decision, not a
    // die roll, so it is honoured here where the roll is not.
    if ((ev.tConditions.nChance & 0xFF) == 0)
        return;

    const i32 group = ev.tPayload.eSnoGroup;
    const i32 handle = ev.tPayload.dwNameHandle;
    const i32 type = ev.eTriggerType;
    if (handle == -1)
        return;

    if (type == kTriggerSpawn || type == kTriggerSpawnAttached) {
        if (group != kGroupParticle && group != kGroupActor)
            return;
        ResolvedEffect r;
        FillFromEvent(ev, r);
        r.kind = (group == kGroupActor) ? ResolvedEffect::Kind::Actor
                                        : ResolvedEffect::Kind::Particle;
        r.sno = handle;
        r.fromEffectGroup = fromGroup;
        r.weight = weight;
        out.push_back(std::move(r));
        // A `.prt` is an authoring site of its own, not just a leaf: 773 of
        // them carry a message-3000 event, which `ParticleSystem_Spawn`
        // (0x71000AE0B0) fires the moment the system exists. Recurse so that
        // playing one plays what it names.
        if (group == kGroupParticle)
            ExpandParticleSpawn(handle, cache, lookName, depth, visited, out);
        return;
    }

    if (type != kTriggerPlayEffectGroup || group != kGroupEffectGroup || depth >= kMaxDepth ||
        !visited.insert(handle).second)
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

void ExpandParticleSpawn(i32 sno, D3SnoCache& cache, std::string_view lookName, int depth,
                         std::unordered_set<i32>& visited, std::vector<ResolvedEffect>& out) {
    if (depth >= kMaxDepth || !visited.insert(sno).second)
        return;
    auto prt = cache.Particle(sno);
    if (!prt)
        return;
    for (const auto& ev : prt->arTriggeredEvents) {
        // The spawn key is `{3000, 0.0f, 0}`, so the sub-key must be 0 — it is,
        // on all 2,038 shipped particle events, and so is the impulse window
        // the other arm of `MsgTriggeredEvent_MatchesKey` tests.
        if (ev.eMessageType != kMsgParticleSpawned || ev.tEvent.tConditions.nSubKey != 0)
            continue;
        Expand(ev.tEvent, cache, lookName, depth + 1, visited, -1, 100, out);
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

/// PRS -> matrix, normalising as the client does. Local to the resolver because
/// the adapter's own copy takes a PRSTransform and a hardpoint carries a PR.
Matrix44f MatrixOf(const Vector3f& t, const Vector4f& q, f32 scale) {
    const f32 n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    const f32 i = (n > 1e-8f) ? (1.0f / n) : 1.0f;
    return renderer::animation::ComposePivotSRT(
        t, Quaternion{q.x * i, q.y * i, q.z * i, q.w * i}, {scale, scale, scale},
        {0.0f, 0.0f, 0.0f});
}

/// @brief The frame a hardpoint sits in, which is NOT its bone's animated pose.
///
/// `Skeleton_BuildSkinningPaletteAndBounds` writes three transforms into each
/// 96-byte palette entry and `sub_7100213590`, the shipped hardpoint resolver,
/// reads the middle one: quaternion at +32, translation at +48, scale at +60 —
/// the pose composed with the bone's `tTransform1`.
///
/// That is not a refinement. A hardpoint's transform is authored in the same
/// space as its bone's `tTransform0`, and `tTransform1` is exactly its inverse,
/// so a hardpoint that rides its bone composes to the identity here and one
/// like `HP_rightWeapon_mid` keeps only its own displacement along the blade.
/// Compose the hardpoint straight onto the bone instead and the two add:
/// Tyrael's sword lands 6.08 units out on a skeleton that reaches 6.04, holding
/// the pose of a hand a body's length away. `d3_cloth.cpp`'s `AttachFrame` is
/// the same composition, recovered for the collision capsules.
Matrix44f HardpointBoneFrame(const d3n::Appearances& app, i32 bone) {
    if (bone < 0 || static_cast<usize>(bone) >= app.arBones.size())
        return Matrix44f::identity();
    const d3n::PRSTransform& inv = app.arBones[static_cast<usize>(bone)].tTransform1;
    return MatrixOf(inv.vTranslation, inv.qRotation, inv.flScale);
}

i32 FindD3Hardpoint(const d3n::Appearances& app, std::string_view name) {
    if (IsD3RootHardpoint(name))
        return -1;
    for (usize i = 0; i < app.arHardpoints.size(); ++i)
        if (IEquals(app.arHardpoints[i].szName, name))
            return static_cast<i32>(i);
    return -1;
}

D3Attach ResolveD3Attach(const d3n::Appearances& app, std::string_view hardpoint) {
    D3Attach a;
    const i32 hp = FindD3Hardpoint(app, hardpoint);
    if (hp < 0)
        return a;
    const auto& h = app.arHardpoints[static_cast<usize>(hp)];
    a.bone = h.nBoneIndex;
    a.offset = MatrixOf(h.tTransform.vTranslation, h.tTransform.qRotation, 1.0f) *
               HardpointBoneFrame(app, a.bone);
    return a;
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
