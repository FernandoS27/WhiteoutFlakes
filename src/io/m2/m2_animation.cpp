#include "io/m2/m2_animation.h"

#include <cmath>
#include <cstring>
#include <iterator>

namespace whiteout::flakes::io {

namespace wm2 = ::whiteout::m2;

Vector3f SampleM2Vec3(const wm2::AnimationTrack<Vector3f>& track, const M2AnimTime& at,
                      const Vector3f& def) {
    const M2KeySpan k = LocateM2Key(track, at);
    if (!k.valid)
        return def;
    const auto& v = track.values[static_cast<usize>(k.sub)];
    const Vector3f& a = v[static_cast<usize>(k.k0)];
    if (k.k0 == k.k1 || M2TrackSteps(track))
        return a;
    const Vector3f& b = v[static_cast<usize>(k.k1)];
    return {a.x + (b.x - a.x) * k.blend, a.y + (b.y - a.y) * k.blend,
            a.z + (b.z - a.z) * k.blend};
}

namespace {

// The two key encodings a rotation track can carry. Which one a track uses is
// a property of what it drives, not of the file version.
Quaternion M2QuatKey(const wm2::CompatQuaternion& q) {
    return M2DecodeQuat(q);
}
Quaternion M2QuatKey(const Quaternion& q) {
    return q;
}

template <class Key>
Quaternion SampleQuatTrack(const wm2::AnimationTrack<Key>& track, const M2AnimTime& at,
                           const Quaternion& def) {
    const M2KeySpan k = LocateM2Key(track, at);
    if (!k.valid)
        return def;
    const auto& v = track.values[static_cast<usize>(k.sub)];
    const Quaternion a = M2QuatKey(v[static_cast<usize>(k.k0)]);
    if (k.k0 == k.k1 || M2TrackSteps(track))
        return a;
    const Quaternion b = M2QuatKey(v[static_cast<usize>(k.k1)]);

    // Nlerp, and no shortest-path sign flip: C4Quaternion::Nlerp lerps the four
    // components and renormalises, full stop. Flipping here would be a
    // *different* rotation than the client produces wherever a key pair
    // straddles hemispheres.
    Quaternion q{a.x + (b.x - a.x) * k.blend, a.y + (b.y - a.y) * k.blend,
                 a.z + (b.z - a.z) * k.blend, a.w + (b.w - a.w) * k.blend};
    const f32 len2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
    if (len2 > 1e-12f) {
        const f32 inv = 1.0f / std::sqrt(len2);
        q.x *= inv;
        q.y *= inv;
        q.z *= inv;
        q.w *= inv;
    }
    return q;
}

} // namespace

Quaternion SampleM2Quat(const wm2::AnimationTrack<wm2::CompatQuaternion>& track,
                        const M2AnimTime& at, const Quaternion& def) {
    return SampleQuatTrack(track, at, def);
}

Quaternion SampleM2Quat(const wm2::AnimationTrack<Quaternion>& track, const M2AnimTime& at,
                        const Quaternion& def) {
    return SampleQuatTrack(track, at, def);
}

f32 SampleM2Fixed16(const wm2::AnimationTrack<i16>& track, const M2AnimTime& at, f32 def) {
    const M2KeySpan k = LocateM2Key(track, at);
    if (!k.valid)
        return def;
    const auto& v = track.values[static_cast<usize>(k.sub)];
    const f32 a = M2DecodeFixed16(v[static_cast<usize>(k.k0)]);
    if (k.k0 == k.k1 || M2TrackSteps(track))
        return a;
    return a + (M2DecodeFixed16(v[static_cast<usize>(k.k1)]) - a) * k.blend;
}

f32 SampleM2Float(const wm2::AnimationTrack<f32>& track, const M2AnimTime& at, f32 def) {
    const M2KeySpan k = LocateM2Key(track, at);
    if (!k.valid)
        return def;
    const auto& v = track.values[static_cast<usize>(k.sub)];
    const f32 a = v[static_cast<usize>(k.k0)];
    if (k.k0 == k.k1 || M2TrackSteps(track))
        return a;
    return a + (v[static_cast<usize>(k.k1)] - a) * k.blend;
}

Vector3f M2DecodeCompressedGravity(f32 packed) {
    // Bit-for-bit as CM2Shared::DecompressParticleSequence decodes it,
    // arithmetic order included — the client folds `1 - dx*dx` before
    // subtracting `dy*dy`, and takes the Z hemisphere from the magnitude's sign
    // rather than storing it in the direction.
    u8 raw[4];
    std::memcpy(raw, &packed, 4);
    const f32 dx = static_cast<f32>(static_cast<i8>(raw[0])) * 0.0078125f;
    const f32 dy = static_cast<f32>(static_cast<i8>(raw[1])) * 0.0078125f;
    i16 mz = 0;
    std::memcpy(&mz, raw + 2, 2);
    f32 mag = static_cast<f32>(mz) * 0.042385526f;

    const f32 zz = (1.0f - dx * dx) - dy * dy;
    f32 dz = std::sqrt(zz > 0.0f ? zz : 0.0f);
    if (mag < 0.0f) {
        dz = -dz;
        mag = -mag;
    }
    return {dx * mag, dy * mag, dz * mag};
}

Vector3f SampleM2ParticleGravity(const wm2::AnimationTrack<f32>& track, const M2AnimTime& at,
                                 bool compressed) {
    if (!compressed)
        return {0.0f, 0.0f, -SampleM2Float(track, at, 0.0f)};

    const M2KeySpan k = LocateM2Key(track, at);
    if (!k.valid)
        return {0.0f, 0.0f, 0.0f};
    const auto& v = track.values[static_cast<usize>(k.sub)];
    const Vector3f a = M2DecodeCompressedGravity(v[static_cast<usize>(k.k0)]);
    if (k.k0 == k.k1 || M2TrackSteps(track))
        return a;
    const Vector3f b = M2DecodeCompressedGravity(v[static_cast<usize>(k.k1)]);
    return {a.x + (b.x - a.x) * k.blend, a.y + (b.y - a.y) * k.blend,
            a.z + (b.z - a.z) * k.blend};
}

u8 SampleM2U8(const wm2::AnimationTrack<u8>& track, const M2AnimTime& at, u8 def) {
    const M2KeySpan k = LocateM2Key(track, at);
    if (!k.valid)
        return def;
    return track.values[static_cast<usize>(k.sub)][static_cast<usize>(k.k0)];
}

namespace {

// s_animationNames, dumped verbatim from the 6.0.1 client (the table
// CGUnit_C::ValidateAnimWarning reports against, terminated there by the
// "DBFilesClient\AnimationData.dbc" literal). Index IS the AnimationData id,
// which is what M2Sequence::id carries.
constexpr const char* kAnimationNames[] = {
    "Stand", "Death", "Spell", "Stop", "Walk", "Run", "Dead", "Rise", "StandWound",
    "CombatWound", "CombatCritical", "ShuffleLeft", "ShuffleRight", "Walkbackwards", "Stun",
    "HandsClosed", "AttackUnarmed", "Attack1H", "Attack2H", "Attack2HL", "ParryUnarmed",
    "Parry1H", "Parry2H", "Parry2HL", "ShieldBlock", "ReadyUnarmed", "Ready1H", "Ready2H",
    "Ready2HL", "ReadyBow", "Dodge", "SpellPrecast", "SpellCast", "SpellCastArea", "NPCWelcome",
    "NPCGoodbye", "Block", "JumpStart", "Jump", "JumpEnd", "Fall", "SwimIdle", "Swim",
    "SwimLeft", "SwimRight", "SwimBackwards", "AttackBow", "FireBow", "ReadyRifle",
    "AttackRifle", "Loot", "ReadySpellDirected", "ReadySpellOmni", "SpellCastDirected",
    "SpellCastOmni", "BattleRoar", "ReadyAbility", "Special1H", "Special2H", "ShieldBash",
    "EmoteTalk", "EmoteEat", "EmoteWork", "EmoteUseStanding", "EmoteTalkExclamation",
    "EmoteTalkQuestion", "EmoteBow", "EmoteWave", "EmoteCheer", "EmoteDance", "EmoteLaugh",
    "EmoteSleep", "EmoteSitGround", "EmoteRude", "EmoteRoar", "EmoteKneel", "EmoteKiss",
    "EmoteCry", "EmoteChicken", "EmoteBeg", "EmoteApplaud", "EmoteShout", "EmoteFlex",
    "EmoteShy", "EmotePoint", "Attack1HPierce", "Attack2HLoosePierce", "AttackOff",
    "AttackOffPierce", "Sheath", "HipSheath", "Mount", "RunRight", "RunLeft", "MountSpecial",
    "Kick", "SitGroundDown", "SitGround", "SitGroundUp", "SleepDown", "Sleep", "SleepUp",
    "SitChairLow", "SitChairMed", "SitChairHigh", "LoadBow", "LoadRifle", "AttackThrown",
    "ReadyThrown", "HoldBow", "HoldRifle", "HoldThrown", "LoadThrown", "EmoteSalute",
    "KneelStart", "KneelLoop", "KneelEnd", "AttackUnarmedOff", "SpecialUnarmed", "StealthWalk",
    "StealthStand", "Knockdown", "EatingLoop", "UseStandingLoop", "ChannelCastDirected",
    "ChannelCastOmni", "Whirlwind", "Birth", "UseStandingStart", "UseStandingEnd",
    "CreatureSpecial", "Drown", "Drowned", "FishingCast", "FishingLoop", "Fly",
    "EmoteWorkNoSheathe", "EmoteStunNoSheathe", "EmoteUseStandingNoSheathe", "SpellSleepDown",
    "SpellKneelStart", "SpellKneelLoop", "SpellKneelEnd", "Sprint", "InFlight", "Spawn",
    "Close", "Closed", "Open", "Opened", "Destroy", "Destroyed", "Rebuild", "Custom0",
    "Custom1", "Custom2", "Custom3", "Despawn", "Hold", "Decay", "BowPull", "BowRelease",
    "ShipStart", "ShipMoving", "ShipStop", "GroupArrow", "Arrow", "CorpseArrow", "GuideArrow",
    "Sway", "DruidCatPounce", "DruidCatRip", "DruidCatRake", "DruidCatRavage", "DruidCatClaw",
    "DruidCatCower", "DruidBearSwipe", "DruidBearBite", "DruidBearMaul", "DruidBearBash",
    "DragonTail", "DragonStomp", "DragonSpit", "DragonSpitHover", "DragonSpitFly", "EmoteYes",
    "EmoteNo", "JumpLandRun", "LootHold", "LootUp", "StandHigh", "Impact", "LiftOff", "Hover",
    "SuccubusEntice", "EmoteTrain", "EmoteDead", "EmoteDanceOnce", "Deflect",
    "EmoteEatNoSheathe", "Land", "Submerge", "Submerged", "Cannibalize", "ArrowBirth",
    "GroupArrowBirth", "CorpseArrowBirth", "GuideArrowBirth", "EmoteTalkNoSheathe",
    "EmotePointNoSheathe", "EmoteSaluteNoSheathe", "EmoteDanceSpecial", "Mutilate",
    "CustomSpell01", "CustomSpell02", "CustomSpell03", "CustomSpell04", "CustomSpell05",
    "CustomSpell06", "CustomSpell07", "CustomSpell08", "CustomSpell09", "CustomSpell10",
    "StealthRun", "Emerge", "Cower", "Grab", "GrabClosed", "GrabThrown", "FlyStand", "FlyDeath",
    "FlySpell", "FlyStop", "FlyWalk", "FlyRun", "FlyDead", "FlyRise", "FlyStandWound",
    "FlyCombatWound", "FlyCombatCritical", "FlyShuffleLeft", "FlyShuffleRight",
    "FlyWalkbackwards", "FlyStun", "FlyHandsClosed", "FlyAttackUnarmed", "FlyAttack1H",
    "FlyAttack2H", "FlyAttack2HL", "FlyParryUnarmed", "FlyParry1H", "FlyParry2H", "FlyParry2HL",
    "FlyShieldBlock", "FlyReadyUnarmed", "FlyReady1H", "FlyReady2H", "FlyReady2HL",
    "FlyReadyBow", "FlyDodge", "FlySpellPrecast", "FlySpellCast", "FlySpellCastArea",
    "FlyNPCWelcome", "FlyNPCGoodbye", "FlyBlock", "FlyJumpStart", "FlyJump", "FlyJumpEnd",
    "FlyFall", "FlySwimIdle", "FlySwim", "FlySwimLeft", "FlySwimRight", "FlySwimBackwards",
    "FlyAttackBow", "FlyFireBow", "FlyReadyRifle", "FlyAttackRifle", "FlyLoot",
    "FlyReadySpellDirected", "FlyReadySpellOmni", "FlySpellCastDirected", "FlySpellCastOmni",
    "FlyBattleRoar", "FlyReadyAbility", "FlySpecial1H", "FlySpecial2H", "FlyShieldBash",
    "FlyEmoteTalk", "FlyEmoteEat", "FlyEmoteWork", "FlyEmoteUseStanding",
    "FlyEmoteTalkExclamation", "FlyEmoteTalkQuestion", "FlyEmoteBow", "FlyEmoteWave",
    "FlyEmoteCheer", "FlyEmoteDance", "FlyEmoteLaugh", "FlyEmoteSleep", "FlyEmoteSitGround",
    "FlyEmoteRude", "FlyEmoteRoar", "FlyEmoteKneel", "FlyEmoteKiss", "FlyEmoteCry",
    "FlyEmoteChicken", "FlyEmoteBeg", "FlyEmoteApplaud", "FlyEmoteShout", "FlyEmoteFlex",
    "FlyEmoteShy", "FlyEmotePoint", "FlyAttack1HPierce", "FlyAttack2HLoosePierce",
    "FlyAttackOff", "FlyAttackOffPierce", "FlySheath", "FlyHipSheath", "FlyMount",
    "FlyRunRight", "FlyRunLeft", "FlyMountSpecial", "FlyKick", "FlySitGroundDown",
    "FlySitGround", "FlySitGroundUp", "FlySleepDown", "FlySleep", "FlySleepUp",
    "FlySitChairLow", "FlySitChairMed", "FlySitChairHigh", "FlyLoadBow", "FlyLoadRifle",
    "FlyAttackThrown", "FlyReadyThrown", "FlyHoldBow", "FlyHoldRifle", "FlyHoldThrown",
    "FlyLoadThrown", "FlyEmoteSalute", "FlyKneelStart", "FlyKneelLoop", "FlyKneelEnd",
    "FlyAttackUnarmedOff", "FlySpecialUnarmed", "FlyStealthWalk", "FlyStealthStand",
    "FlyKnockdown", "FlyEatingLoop", "FlyUseStandingLoop", "FlyChannelCastDirected",
    "FlyChannelCastOmni", "FlyWhirlwind", "FlyBirth", "FlyUseStandingStart",
    "FlyUseStandingEnd", "FlyCreatureSpecial", "FlyDrown", "FlyDrowned", "FlyFishingCast",
    "FlyFishingLoop", "FlyFly", "FlyEmoteWorkNoSheathe", "FlyEmoteStunNoSheathe",
    "FlyEmoteUseStandingNoSheathe", "FlySpellSleepDown", "FlySpellKneelStart",
    "FlySpellKneelLoop", "FlySpellKneelEnd", "FlySprint", "FlyInFlight", "FlySpawn", "FlyClose",
    "FlyClosed", "FlyOpen", "FlyOpened", "FlyDestroy", "FlyDestroyed", "FlyRebuild",
    "FlyCustom0", "FlyCustom1", "FlyCustom2", "FlyCustom3", "FlyDespawn", "FlyHold", "FlyDecay",
    "FlyBowPull", "FlyBowRelease", "FlyShipStart", "FlyShipMoving", "FlyShipStop",
    "FlyGroupArrow", "FlyArrow", "FlyCorpseArrow", "FlyGuideArrow", "FlySway",
    "FlyDruidCatPounce", "FlyDruidCatRip", "FlyDruidCatRake", "FlyDruidCatRavage",
    "FlyDruidCatClaw", "FlyDruidCatCower", "FlyDruidBearSwipe", "FlyDruidBearBite",
    "FlyDruidBearMaul", "FlyDruidBearBash", "FlyDragonTail", "FlyDragonStomp", "FlyDragonSpit",
    "FlyDragonSpitHover", "FlyDragonSpitFly", "FlyEmoteYes", "FlyEmoteNo", "FlyJumpLandRun",
    "FlyLootHold", "FlyLootUp", "FlyStandHigh", "FlyImpact", "FlyLiftOff", "FlyHover",
    "FlySuccubusEntice", "FlyEmoteTrain", "FlyEmoteDead", "FlyEmoteDanceOnce", "FlyDeflect",
    "FlyEmoteEatNoSheathe", "FlyLand", "FlySubmerge", "FlySubmerged", "FlyCannibalize",
    "FlyArrowBirth", "FlyGroupArrowBirth", "FlyCorpseArrowBirth", "FlyGuideArrowBirth",
    "FlyEmoteTalkNoSheathe", "FlyEmotePointNoSheathe", "FlyEmoteSaluteNoSheathe",
    "FlyEmoteDanceSpecial", "FlyMutilate", "FlyCustomSpell01", "FlyCustomSpell02",
    "FlyCustomSpell03", "FlyCustomSpell04", "FlyCustomSpell05", "FlyCustomSpell06",
    "FlyCustomSpell07", "FlyCustomSpell08", "FlyCustomSpell09", "FlyCustomSpell10",
    "FlyStealthRun", "FlyEmerge", "FlyCower", "FlyGrab", "FlyGrabClosed", "FlyGrabThrown",
    "ToFly", "ToHover", "ToGround", "FlyToFly", "FlyToHover", "FlyToGround", "Settle",
    "FlySettle", "DeathStart", "DeathLoop", "DeathEnd", "FlyDeathStart", "FlyDeathLoop",
    "FlyDeathEnd", "DeathEndHold", "FlyDeathEndHold", "Strangulate", "FlyStrangulate",
    "ReadyJoust", "LoadJoust", "HoldJoust", "FlyReadyJoust", "FlyLoadJoust", "FlyHoldJoust",
    "AttackJoust", "FlyAttackJoust", "ReclinedMount", "FlyReclinedMount", "ToAltered",
    "FromAltered", "FlyToAltered", "FlyFromAltered", "InStocks", "FlyInStocks", "VehicleGrab",
    "VehicleThrow", "FlyVehicleGrab", "FlyVehicleThrow", "ToAlteredPostSwap",
    "FromAlteredPostSwap", "FlyToAlteredPostSwap", "FlyFromAlteredPostSwap",
    "ReclinedMountPassenger", "FlyReclinedMountPassenger", "Carry2H", "Carried2H", "FlyCarry2H",
    "FlyCarried2H", "EmoteSniff", "EmoteFlySniff", "AttackFist1H", "FlyAttackFist1H",
    "AttackFist1HOff", "FlyAttackFist1HOff", "ParryFist1H", "FlyParryFist1H", "ReadyFist1H",
    "FlyReadyFist1H", "SpecialFist1H", "FlySpecialFist1H", "EmoteReadStart",
    "FlyEmoteReadStart", "EmoteReadLoop", "FlyEmoteReadLoop", "EmoteReadEnd", "FlyEmoteReadEnd",
    "SwimRun", "FlySwimRun", "SwimWalk", "FlySwimWalk", "SwimWalkBackwards",
    "FlySwimWalkBackwards", "SwimSprint", "FlySwimSprint", "MountSwimIdle", "FlyMountSwimIdle",
    "MountSwimBackwards", "FlyMountSwimBackwards", "MountSwimLeft", "FlyMountSwimLeft",
    "MountSwimRight", "FlyMountSwimRight", "MountSwimRun", "FlyMountSwimRun", "MountSwimSprint",
    "FlyMountSwimSprint", "MountSwimWalk", "FlyMountSwimWalk", "MountSwimWalkBackwards",
    "FlyMountSwimWalkBackwards", "MountFlightIdle", "FlyMountFlightIdle",
    "MountFlightBackwards", "FlyMountFlightBackwards", "MountFlightLeft", "FlyMountFlightLeft",
    "MountFlightRight", "FlyMountFlightRight", "MountFlightRun", "FlyMountFlightRun",
    "MountFlightSprint", "FlyMountFlightSprint", "MountFlightWalk", "FlyMountFlightWalk",
    "MountFlightWalkBackwards", "FlyMountFlightWalkBackwards", "MountFlightStart",
    "FlyMountFlightStart", "MountSwimStart", "FlyMountSwimStart", "MountSwimLand",
    "FlyMountSwimLand", "MountSwimLandRun", "FlyMountSwimLandRun", "MountFlightLand",
    "FlyMountFlightLand", "MountFlightLandRun", "FlyMountFlightLandRun", "ReadyBlowDart",
    "FlyReadyBlowDart", "LoadBlowDart", "FlyLoadBlowDart", "HoldBlowDart", "FlyHoldBlowDart",
    "AttackBlowDart", "FlyAttackBlowDart", "CarriageMount", "FlyCarriageMount",
    "CarriagePassengerMount", "FlyCarriagePassengerMount", "CarriageMountAttack",
    "FlyCarriageMountAttack", "BarTendStand", "FlyBarTendStand", "BarServerWalk",
    "FlyBarServerWalk", "BarServerRun", "FlyBarServerRun", "BarServerShuffleLeft",
    "FlyBarServerShuffleLeft", "BarServerShuffleRight", "FlyBarServerShuffleRight",
    "BarTendEmoteTalk", "FlyBarTendEmoteTalk", "BarTendEmotePoint", "FlyBarTendEmotePoint",
    "BarServerStand", "FlyBarServerStand", "BarSweepWalk", "FlyBarSweepWalk", "BarSweepRun",
    "FlyBarSweepRun", "BarSweepShuffleLeft", "FlyBarSweepShuffleLeft", "BarSweepShuffleRight",
    "FlyBarSweepShuffleRight", "BarSweepEmoteTalk", "FlyBarSweepEmoteTalk",
    "BarPatronSitEmotePoint", "FlyBarPatronSitEmotePoint", "MountSelfIdle", "FlyMountSelfIdle",
    "MountSelfWalk", "FlyMountSelfWalk", "MountSelfRun", "FlyMountSelfRun", "MountSelfSprint",
    "FlyMountSelfSprint", "MountSelfRunLeft", "FlyMountSelfRunLeft", "MountSelfRunRight",
    "FlyMountSelfRunRight", "MountSelfShuffleLeft", "FlyMountSelfShuffleLeft",
    "MountSelfShuffleRight", "FlyMountSelfShuffleRight", "MountSelfWalkBackwards",
    "FlyMountSelfWalkBackwards", "MountSelfSpecial", "FlyMountSelfSpecial", "MountSelfJump",
    "FlyMountSelfJump", "MountSelfJumpStart", "FlyMountSelfJumpStart", "MountSelfJumpEnd",
    "FlyMountSelfJumpEnd", "MountSelfJumpLandRun", "FlyMountSelfJumpLandRun", "MountSelfStart",
    "FlyMountSelfStart", "MountSelfFall", "FlyMountSelfFall", "Stormstrike", "FlyStormstrike",
    "ReadyJoustNoSheathe", "FlyReadyJoustNoSheathe", "Slam", "FlySlam", "DeathStrike",
    "FlyDeathStrike", "SwimAttackUnarmed", "FlySwimAttackUnarmed", "SpinningKick",
    "FlySpinningKick", "RoundHouseKick", "FlyRoundHouseKick", "RollStart", "FlyRollStart",
    "Roll", "FlyRoll", "RollEnd", "FlyRollEnd", "PalmStrike", "FlyPalmStrike",
    "MonkOffenseAttackUnarmed", "FlyMonkOffenseAttackUnarmed", "MonkOffenseAttackUnarmedOff",
    "FlyMonkOffenseAttackUnarmedOff", "MonkOffenseParryUnarmed", "FlyMonkOffenseParryUnarmed",
    "MonkOffenseReadyUnarmed", "FlyMonkOffenseReadyUnarmed", "MonkOffenseSpecialUnarmed",
    "FlyMonkOffenseSpecialUnarmed", "MonkDefenseAttackUnarmed", "FlyMonkDefenseAttackUnarmed",
    "MonkDefenseAttackUnarmedOff", "FlyMonkDefenseAttackUnarmedOff", "MonkDefenseParryUnarmed",
    "FlyMonkDefenseParryUnarmed", "MonkDefenseReadyUnarmed", "FlyMonkDefenseReadyUnarmed",
    "MonkDefenseSpecialUnarmed", "FlyMonkDefenseSpecialUnarmed", "MonkHealAttackUnarmed",
    "FlyMonkHealAttackUnarmed", "MonkHealAttackUnarmedOff", "FlyMonkHealAttackUnarmedOff",
    "MonkHealParryUnarmed", "FlyMonkHealParryUnarmed", "MonkHealReadyUnarmed",
    "FlyMonkHealReadyUnarmed", "MonkHealSpecialUnarmed", "FlyMonkHealSpecialUnarmed",
    "FlyingKick", "FlyFlyingKick", "FlyingKickStart", "FlyFlyingKickStart", "FlyingKickEnd",
    "FlyFlyingKickEnd", "CraneStart", "FlyCraneStart", "CraneLoop", "FlyCraneLoop", "CraneEnd",
    "FlyCraneEnd", "Despawned", "FlyDespawned", "ThousandFists", "FlyThousandFists",
    "MonkHealReadySpellDirected", "FlyMonkHealReadySpellDirected", "MonkHealReadySpellOmni",
    "FlyMonkHealReadySpellOmni", "MonkHealSpellCastDirected", "FlyMonkHealSpellCastDirected",
    "MonkHealSpellCastOmni", "FlyMonkHealSpellCastOmni", "MonkHealChannelCastDirected",
    "FlyMonkHealChannelCastDirected", "MonkHealChannelCastOmni", "FlyMonkHealChannelCastOmni",
    "Torpedo", "FlyTorpedo", "Meditate", "FlyMeditate", "BreathOfFire", "FlyBreathOfFire",
    "RisingSunKick", "FlyRisingSunKick", "GroundKick", "FlyGroundKick", "KickBack",
    "FlyKickBack", "PetBattleStand", "FlyPetBattleStand", "PetBattleDeath", "FlyPetBattleDeath",
    "PetBattleRun", "FlyPetBattleRun", "PetBattleWound", "FlyPetBattleWound", "PetBattleAttack",
    "FlyPetBattleAttack", "PetBattleReadySpell", "FlyPetBattleReadySpell", "PetBattleSpellCast",
    "FlyPetBattleSpellCast", "PetBattleCustom0", "FlyPetBattleCustom0", "PetBattleCustom1",
    "FlyPetBattleCustom1", "PetBattleCustom2", "FlyPetBattleCustom2", "PetBattleCustom3",
    "FlyPetBattleCustom3", "PetBattleVictory", "FlyPetBattleVictory", "PetBattleLoss",
    "FlyPetBattleLoss", "PetBattleStun", "FlyPetBattleStun", "PetBattleDead",
    "FlyPetBattleDead", "PetBattleFreeze", "FlyPetBattleFreeze", "MonkOffenseAttackWeapon",
    "FlyMonkOffenseAttackWeapon", "BarTendEmoteWave", "FlyBarTendEmoteWave",
    "BarServerEmoteTalk", "FlyBarServerEmoteTalk", "BarServerEmoteWave",
    "FlyBarServerEmoteWave", "BarServerPourDrinks", "FlyBarServerPourDrinks", "BarServerPickup",
    "FlyBarServerPickup", "BarServerPutDown", "FlyBarServerPutDown", "BarSweepStand",
    "FlyBarSweepStand", "BarPatronSit", "FlyBarPatronSit", "BarPatronSitEmoteTalk",
    "FlyBarPatronSitEmoteTalk", "BarPatronStand", "FlyBarPatronStand",
    "BarPatronStandEmoteTalk", "FlyBarPatronStandEmoteTalk", "BarPatronStandEmotePoint",
    "FlyBarPatronStandEmotePoint", "CarrionSwarm", "FlyCarrionSwarm", "WheelLoop",
    "FlyWheelLoop", "StandCharacterCreate", "FlyStandCharacterCreate",
};

} // namespace

std::string_view M2AnimationName(u16 animationId) {
    if (animationId >= std::size(kAnimationNames))
        return {};
    return kAnimationNames[animationId];
}

} // namespace whiteout::flakes::io
