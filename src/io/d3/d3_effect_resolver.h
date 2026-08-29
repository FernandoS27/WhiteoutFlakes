#pragma once

// ============================================================================
// D3EffectResolver — how a `.prt` (and a whole second model) reaches a
// Diablo III actor.
//
// Not through the model. `BoneStructure::snoParticle` exists and works, but it
// is authored on 5 of the first 2,500 appearances — 33 attachments, 3 distinct
// files. The route that carries the shipped content is a **TriggerEvent**, and
// it appears at three sites. Measured over the full corpus by matching every
// aligned `{eSnoGroup, snoHandle}` pair against the real id maps:
//
//     site                          refs      unique .prt   exclusive
//     EffectGroup.arEffectItems    16,942        11,849       11,544
//     Actor.arMsgTriggeredEvents   10,690         4,430        4,166
//     Anim ... arAttachments        7,777         4,243        3,896
//     -------------------------------------------------------------
//     union                                      20,060 of 21,593 (92.9%)
//
// The three barely overlap, and the effect group is the biggest by a factor of
// two and a half. An `.efg` does not play itself, though: it is a *library*,
// reached from the other two, which name 685 and 513 distinct groups. So the
// shape is one indirection deep —
//
//     Actor / Anim --(TriggerEvent)--> Particle
//                                  |-> Actor  (another whole model)
//                                  '-> EffectGroup --(TriggerEvent)--> ...
//
// and this resolver flattens it.
//
// **A payload is not always an effect.** 783 anim attachments (549 distinct)
// and 594 actor events (251 distinct) name a group 1 **Actor**:
// `Actor_SpawnFromSno` (0x710021FDA0) builds a full ACD with its own
// Appearance and AnimSet and rides it on the event's hardpoint. A relic on an
// altar, the lid a chest throws, the corpse a death animation leaves behind.
// Those come back as @ref ResolvedEffect::Kind::Actor and the renderer turns
// them into child models.
//
// **`MsgTriggeredEvent.eMessageType` decides which of an actor's events fire.**
// `Actor_FireMsgTriggeredEvents` (0x71002101B0) walks the array and plays every
// entry whose type matches the message it was handed. Message **1000** is the
// one a viewer wants: `Actor_FireInitEffects` (0x7100210170) builds that key
// and its only caller is actor-model setup, so 1000 means "this actor has come
// into existence". It is also the overwhelming majority — 15,630 of 27,362
// events — and the shipped names say the same thing outright (`staffGlow`,
// `lightWhisps_cone`, `groundTrail`, `crystals`) where 17 is `CloseingFX` and
// `death_wings_dissipate`, and 2021/2510/2550 are chest, shrine and waypoint
// activations. An **anim** attachment has no message: its key is a frame.
//
// **`eTriggerType` and the payload group agree, everywhere.** A Particle or
// Actor payload only ever rides type 0 (`TriggerEvent_Spawn`) or 25
// (`TriggerEvent_SpawnAttached`); an EffectGroup payload only ever rides type
// 16 (`EffectGroup_Play`). That holds at all three sites over 82,000 events, so
// the type is checked here as well as the group — a file that breaks the
// pattern is asking for something this cannot do, not for a spawn.
//
// What this deliberately does NOT do is roll dice. `TriggerConditions_RollDelay`
// (0x710060CEA0) is `chance == 0 ? drop : (rand() % 255 > chance ? drop
// : delayMin + rand() % (delayRange + 1))`, and an EffectGroup's weighted modes
// draw again. A viewer that re-rolled on every reload would show a different
// model each time, so the random draws are replaced by their deterministic
// representative — full chance is honoured, `nChance == 0` still drops (813
// shipped items are authored off), and the roll itself is reported rather than
// taken. See D3_PARTICLE_DESIGN.md §16.
// ============================================================================

#include "io/d3/d3_sno_cache.h"
#include "whiteout/flakes/types.h"

#include <string>
#include <vector>

namespace whiteout::flakes::io::d3 {

/// The actor messages worth naming, as measured over 27,362 shipped events.
enum : i32 {
    /// The actor exists. Its events are the persistent ambient effect set —
    /// 15,630 of 27,362, and what a viewer should start on load.
    kD3MsgActorSpawned = 1000,
    /// The variant `Actor_FireInitEffects` builds for its second flavour.
    kD3MsgActorSpawnedAlt = 1110,
    /// End / death / close. 2,334 events.
    kD3MsgActorEnded = 17,
};

/// One thing an actor wants playing, flattened out of wherever it was authored.
struct ResolvedEffect {
    /// What @ref sno names. The renderer builds a different object for each:
    /// an emitter, or a whole child actor.
    enum class Kind : u8 { Particle, Actor };
    Kind kind = Kind::Particle;
    /// The `.prt` or the `.acr`, by SNO id.
    i32 sno = -1;

    /// The hardpoint to ride, verbatim from the event. `"Default"`, empty,
    /// `"- None -"` and `"Don't Override"` all mean the model origin — the
    /// last because it is the editor's "inherit from whatever played me", and
    /// nothing played this.
    std::string hardpoint;

    /// `nChance / 255`. 1.0 for 29,574 of 30,463 shipped items.
    f32 chance = 1.0f;
    /// Ticks, from TriggerConditions. `delayMin` is taken; the range is the
    /// spread this resolver refuses to roll.
    i32 tmDelayMin = 0;
    i32 tmDelayRange = 0;
    /// TriggerEvent::tmDuration, in ticks. 600 in 30,357 of 30,463 — a
    /// registered default, not a measurement of anything, so it is carried and
    /// not applied.
    i32 tmDuration = 0;

    /// The event's colour pair and their times. Two ARGB words and two
    /// DT_TIMEs; how the engine ramps between them is not settled, so they are
    /// carried unapplied rather than guessed at.
    u32 color0 = 0;
    i32 tmColor0 = 0;
    u32 color1 = 0;
    i32 tmColor1 = 0;

    /// The `.efg` this came through, or -1 for an event on the actor itself.
    i32 fromEffectGroup = -1;
    /// EffectItem::nWeight, 100 in 30,254 of 30,463.
    i32 weight = 100;
};

/// @brief Everything @p msgId starts on @p actor, effect groups expanded.
///
/// @p lookName is the actor's Appearance look, used only by select mode 10
/// (73 shipped groups); pass empty and mode 10 contributes nothing, which is
/// what the engine does when the look-link does not match.
std::vector<ResolvedEffect> ResolveActorEffects(const d3n::Actor& actor, i32 msgId,
                                                D3SnoCache& cache,
                                                std::string_view lookName = {});

/// @brief Flatten one TriggerEvent, recursing through effect groups.
///
/// Exposed because the anim route hands over exactly this: a
/// `KeyframedAttachment` is `{frame, TriggerEvent}` and its payload obeys the
/// same rules.
void ExpandD3TriggerEvent(const d3n::TriggerEvent& ev, D3SnoCache& cache,
                          std::string_view lookName, std::vector<ResolvedEffect>& out);

/// @brief True when @p name is one of the four spellings that mean "no
///        hardpoint, use the model origin".
bool IsD3RootHardpoint(std::string_view name);

/// @brief The index into `Appearances::arHardpoints` whose name matches
///        @p name, or -1.
///
/// Case-insensitive: the engine interns `HP_head` and `HP_pelvis` but the
/// shipped events spell them `HP_Head` (323 times) and `HP_Pelvis` (200), so a
/// byte compare loses over five hundred real attachments.
i32 FindD3Hardpoint(const d3n::Appearances& app, std::string_view name);

/// @brief Where a hardpoint puts what rides it: a bone, and a frame inside it.
struct D3Attach {
    i32 bone = -1; ///< -1 is the model origin, which is what `Default` means.
    /// Composes as `offset * boneWorld`, and is the identity for the majority
    /// of hardpoints — one authored on its own bone cancels exactly. See
    /// @ref ResolveD3Attach for why that is not the same as no offset at all.
    Matrix44f offset = Matrix44f::identity();
};

/// @brief @ref FindD3Hardpoint plus that hardpoint's frame, so the emitter path
///        and the child-model path do not each recompose it.
///
/// The frame is the hardpoint's own transform composed with its bone's
/// `tTransform1`, because a hardpoint is authored in the same space as the
/// bone's `tTransform0` rather than inside the bone. Dropping the second half
/// adds the bone's own offset twice and throws a held weapon further than the
/// model is tall.
D3Attach ResolveD3Attach(const d3n::Appearances& app, std::string_view hardpoint);

} // namespace whiteout::flakes::io::d3
