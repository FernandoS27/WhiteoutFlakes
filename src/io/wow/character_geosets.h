#pragma once

// ============================================================================
// Character `.m2` — what it is, and which of its submeshes draw.
//
// A World of Warcraft character model is not a finished model. It ships every
// hairstyle, every beard, every ear shape and every armour cut the race can
// wear, all in one `.skin`, and expects the game to choose one per group and
// hide the rest. It also leaves its body texture blank, because that texture is
// composited at runtime from the customisation the character was created with.
// Open one in a viewer with neither step and you get what the client would show
// if `CCharacterComponent` never ran: 26 hairstyles at once, over a white body.
//
// ---------------------------------------------------------------------------
// Detection
//
// The client never asks a model "are you a character". It asks the *display*
// record, and characters get a CCharacterComponent because the unit is a
// player. A viewer opens a file, so the question has to be answered from the
// file — and it can be, exactly, because the three slots the component fills
// are declared in the model:
//
//   CCharacterComponent::CreateBaseTexture       ReplaceTexture(1, base)   0x100340c00
//   CCharacterComponent::ProcessFinishedRequest  ReplaceTexture(6, hair)   0x10033a440
//   CCharacterComponent::ProcessFinishedRequest  ReplaceTexture(8, extra)  0x10033a440
//
// Type 1 is the composited body skin and every character body model declares
// it; types 6 and 8 are the hair and extra-skin overlays. A creature declares
// 11..13 instead (see WowReplaceableTextures) and an item 2..4, so the families
// do not overlap.
//
// ---------------------------------------------------------------------------
// Geoset groups
//
// `SkinSection::skinSectionId` is `group * 100 + value`. The client toggles
// them with `CM2Model::SetGeometryVisible(first, last, on)` over id *ranges*
// (0x100f542e0 — it walks the sections and flips a visibility bit per submesh),
// and `CCharacterComponent::GeosRenderPrep` (0x10033fa00) is the whole policy:
//
//     SetGeometryVisible(0, 2200, 0)   // hide everything
//     SetGeometryVisible(0, 0, 1)      // the body, unconditionally
//     for i in 0..20:  show m_geosets[i]        // the customisation choices
//     SetGeometryVisible(1900, 1999, 0); show 1900 + conditional, default 1901
//     ... then one range per equipment slot, each hidden and one value shown
//
// `m_geosets` is **21 entries**, one per group 0..20, and the constructor
// (0x10033d2b0) seeds it `group * 100 + 1` with ears at 702 before any data is
// read. Those are the bare variants — 401 is the hand a glove replaces — so
// that seeding *is* the naked character, and DefaultSelection reproduces it.
//
// One more detail worth keeping: group 17 is forced to 1703 when the chosen
// skin section carries flag 0x4 (the death-knight eye glow).
//
// ---------------------------------------------------------------------------
// Groups above 20, where the client has nothing to say
//
// That array stops at 20 because in 6.0.1 nothing above it existed. Retail
// drives 21..63 from ChrCustomization, and there is no equivalent constructor
// to transcribe — so the rule here is WoWModelViewer's, which is the only
// worked reference for the modern groups (`WoWModel::refresh`):
//
//   show a declared id when it ends in `01`, plus the whole face group `32xx`;
//   never show eye glow `17xx` or earrings `35xx`.
//
// The face group is the one that is not a menu. Every model that declares it
// declares both 3201 and 3202, and they are not alternatives: measured against
// the corpus, 3201 is ~65 vertices sitting at the *base* of the head box and
// 3202 is the ~1100-vertex head above it. Show 3201 alone and a character has
// a blank plate for a face, which is exactly what "value 1 in every group"
// produced. Where a model has more — humanmale_hd declares 3202, 3203 and 3204
// at an identical 1106 vertices each — those *are* alternatives, and the Face
// Shape option narrows them.
//
// Which is the other half of the rule. Customisation is applied **per id**,
// not per group: an option hides the ids its own choices name and nothing
// else. Narrowing the group instead is what took 3201 away from the races that
// do have a Face Shape option, and what left the ones that do not with a face
// they could never select.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <algorithm>
#include <array>
#include <span>
#include <vector>

namespace whiteout {
namespace m2 {
struct Model;
}
} // namespace whiteout

namespace whiteout::flakes::io::wow {

/// `M2Texture::type`. Zero names a file in the model; everything else is a slot
/// the game fills, and which family it belongs to says what kind of model this
/// is. Only the values this renderer acts on are named.
enum class M2TextureType : u32 {
    Filename = 0,
    Skin = 1,       ///< The composited character body texture.
    ObjectSkin = 2, ///< Cape and other item components.
    CharHair = 6,
    SkinExtra = 8, ///< Underwear / overlay sheet, composited like the body.
    Monster1 = 11,
    Monster2 = 12,
    Monster3 = 13,
    CharacterEyes = 19, ///< Retail; ChrModelMaterial gives it its own layout.
};

/// The three slots `CCharacterComponent` fills, in the order it fills them.
inline constexpr std::array<u32, 3> kCharacterTextureTypes = {1, 6, 8};

/// True when @p model declares any slot only a character component fills.
///
/// Cheap, and deliberately not path-based: `character/human/male/humanmale.m2`
/// and its `_hd` sibling both qualify, and so does a model that lives somewhere
/// else, while a cape or a creature in the same tree does not.
bool IsCharacterModel(const ::whiteout::m2::Model& model);

/// The group a `skinSectionId` belongs to, and its value within that group.
inline constexpr u16 GeosetGroupOf(u16 skinSectionId) {
    return static_cast<u16>(skinSectionId / 100);
}
inline constexpr u16 GeosetValueOf(u16 skinSectionId) {
    return static_cast<u16>(skinSectionId % 100);
}

/// `SetGeometryVisible(0, 0x898, 0)` — the range GeosRenderPrep blanks before
/// it starts choosing. Ids above it are left alone.
inline constexpr u16 kGeosetBlankLimit = 2200;

/// `CCharacterComponent::m_geosets` is 21 entries, groups 0..20. Above that the
/// client of 6.0.1 has no slot and no opinion.
inline constexpr u16 kClientGeosetGroups = 21;

/// How many groups a selection carries. Past the client's 21 because retail
/// races use groups up to 51 (`humanmale_hd` alone declares 32, 33, 34 and 51).
inline constexpr usize kGeosetGroupCount = 64;

/// One character's geoset choices, in the two shapes the game states them in.
///
/// `value` is the client's: one chosen value per group 0..20, so group 4
/// holding 1 means geoset 401, and negative means nothing in that group draws —
/// the answer for every equipment group on a character wearing nothing.
///
/// `controlled` / `shown` are ChrCustomization's, and they are per *id*. An
/// option contributes every geoset any of its choices could name to
/// `controlled` and only the active choice's to `shown`; an id in the first and
/// not the second is off. Anything neither set mentions is left to the default
/// rule, which is what keeps a face's 3201 when the Face Shape option only ever
/// speaks about 3202..3204.
struct CharacterGeosetSelection {
    std::array<i16, kGeosetGroupCount> value{};
    std::vector<u16> controlled;
    std::vector<u16> shown;

    CharacterGeosetSelection() {
        value.fill(-1);
    }

    void Set(u16 group, i16 v) {
        if (group < kGeosetGroupCount)
            value[group] = v;
    }
    i16 Get(u16 group) const {
        return group < kGeosetGroupCount ? value[group] : i16{-1};
    }

    void Control(u16 skinSectionId) {
        controlled.push_back(skinSectionId);
    }
    void Show(u16 skinSectionId) {
        shown.push_back(skinSectionId);
    }
    bool IsControlled(u16 id) const {
        return std::find(controlled.begin(), controlled.end(), id) != controlled.end();
    }
    bool IsShown(u16 id) const {
        return std::find(shown.begin(), shown.end(), id) != shown.end();
    }
};

/// The `skinSectionId` set @p selection makes visible, given the ids @p declared
/// by the model. Sorted, deduplicated, ready for
/// `M2ModelAdapter::SetVisibleGeosets`.
///
/// Geoset 0 is always in it — the body is not a choice — and every id in the
/// result is one the model declares, so a chosen value it does not carry
/// contributes nothing rather than a dangling id.
std::vector<u16> VisibleGeosets(const CharacterGeosetSelection& selection,
                                std::span<const u16> declared);

/// The selection for a character wearing nothing and customised by nothing:
/// what the CCharacterComponent constructor seeds — value 1 in groups 0..20,
/// ears at 702, less the five `RemoveItem` clears.
///
/// Says nothing about the modern groups, which have no client default; those
/// are VisibleGeosets' `*01` + `32xx` rule until ChrCustomization speaks.
CharacterGeosetSelection DefaultSelection();

/// Every distinct `skinSectionId` in @p model's skin profile @p profileIndex,
/// sorted. What a host offering a geoset picker lists, and what VisibleGeosets
/// filters against.
std::vector<u16> DeclaredGeosets(const ::whiteout::m2::Model& model, usize profileIndex);

} // namespace whiteout::flakes::io::wow
