// Character `.m2` support, the device-free half.
//
// Three rules, all from the 6.0.1 client:
//
//   * a model is a character model iff it declares texture type 1, 6 or 8 —
//     the three slots CCharacterComponent fills (0x100340c00, 0x10033a440)
//   * a `skinSectionId` is `group * 100 + value`, and one value per group draws
//     — CCharacterComponent::GeosRenderPrep (0x10033fa00)
//   * a composite sheet is built by pasting section-sized pieces in layer
//     order — CCharacterComponent::PasteToSection (0x10034b3f0)
//
// Plus the `.skin` fact that broke every character model before it was found:
// `SkinSection::level` is the high word of `indexStart`.

#include "io/wow/character_appearance.h"
#include "io/wow/character_geosets.h"
#include "renderer/profiles/wow/wow_character_appearance.h"

#include <whiteout/models/m2/m2.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

using namespace whiteout::flakes::io::wow;
namespace m2 = ::whiteout::m2;

namespace {

bool Visible(const std::vector<::whiteout::u16>& set, ::whiteout::u16 id) {
    return std::find(set.begin(), set.end(), id) != set.end();
}

m2::Model ModelWithTextureTypes(std::initializer_list<::whiteout::u32> types) {
    m2::Model model;
    for (const ::whiteout::u32 t : types) {
        m2::Texture tex;
        tex.type = t;
        model.textures.push_back(tex);
    }
    return model;
}

} // namespace

TEST_CASE("A character model is the one that declares a slot only the game fills",
          "[m2][character]") {
    // The three CCharacterComponent replaces: base skin, hair, extra skin.
    CHECK(IsCharacterModel(ModelWithTextureTypes({1})));
    CHECK(IsCharacterModel(ModelWithTextureTypes({6, 0})));
    CHECK(IsCharacterModel(ModelWithTextureTypes({8})));
    // `humanmale_hd.m2`'s own declaration, verbatim.
    CHECK(IsCharacterModel(ModelWithTextureTypes({1, 6, 2, 15, 16, 18, 17, 19, 0, 0, 0})));

    // A creature fills 11..13 instead (ReplaceMonsterSkin), an item 2..4, and a
    // guild tabard 15..18. None of those is a character.
    CHECK_FALSE(IsCharacterModel(ModelWithTextureTypes({0, 11, 12})));
    CHECK_FALSE(IsCharacterModel(ModelWithTextureTypes({2, 3, 4})));
    CHECK_FALSE(IsCharacterModel(ModelWithTextureTypes({15, 16, 17, 18})));
    CHECK_FALSE(IsCharacterModel(ModelWithTextureTypes({})));
}

TEST_CASE("A skinSectionId splits into a group and a value", "[m2][character]") {
    CHECK(GeosetGroupOf(0) == 0);
    CHECK(GeosetValueOf(0) == 0);
    CHECK(GeosetGroupOf(401) == 4);
    CHECK(GeosetValueOf(401) == 1);
    CHECK(GeosetGroupOf(1703) == 17);
    CHECK(GeosetValueOf(1703) == 3);
    // Retail races reach past the 21 groups the 6.0.1 client knew.
    CHECK(GeosetGroupOf(5103) == 51);
    CHECK(GeosetValueOf(5103) == 3);
}

TEST_CASE("The default selection is the naked body, one value per group",
          "[m2][character]") {
    // `humanmale_hd00.skin`'s declared set, trimmed to what the rule turns on
    // or off. 902/1002/1102 are the armour variants whose `01` the model does
    // not carry; 1502 is a cloak; 2 and 3 are hairstyles.
    const std::vector<::whiteout::u16> declared = {0,    2,    3,    102,  202,  302,  401,
                                                   402,  501,  502,  701,  702,  802,  902,
                                                   1002, 1102, 1301, 1302, 1501, 1502, 1701,
                                                   1801, 2001, 2002, 2201, 3201, 3202, 3301,
                                                   5101};
    const std::vector<::whiteout::u16> visible = VisibleGeosets(DefaultSelection(), declared);

    // The body is not a choice — GeosRenderPrep shows geoset 0 before it
    // consults anything.
    CHECK(Visible(visible, 0));
    // One value per group, and it is the first: bare hands, bare legs, bare
    // feet, no cloak, no belt buckle, bare torso.
    CHECK(Visible(visible, 401));
    CHECK(Visible(visible, 501));
    CHECK(Visible(visible, 1301));
    CHECK(Visible(visible, 1501));
    CHECK(Visible(visible, 1801));
    CHECK(Visible(visible, 2001));
    CHECK(Visible(visible, 2201));
    // Ears are the client's one exception: RemoveItem writes 702, not 701.
    CHECK(Visible(visible, 702));
    CHECK_FALSE(Visible(visible, 701));

    // Everything past the first value of its group stays hidden. This is the
    // whole point: without it a character draws every hairstyle at once. (Not
    // the face group, which is one head split in two — see below.)
    CHECK_FALSE(Visible(visible, 2));
    CHECK_FALSE(Visible(visible, 3));
    CHECK_FALSE(Visible(visible, 402));
    CHECK_FALSE(Visible(visible, 502));
    CHECK_FALSE(Visible(visible, 1302));
    CHECK_FALSE(Visible(visible, 1502));
    CHECK_FALSE(Visible(visible, 2002));

    // A group with no `01` is armour-only and contributes nothing rather than a
    // dangling id.
    CHECK_FALSE(Visible(visible, 802));
    CHECK_FALSE(Visible(visible, 902));
    CHECK_FALSE(Visible(visible, 1002));
    CHECK_FALSE(Visible(visible, 1102));

    // Facial hair is data-fed, not a `+1` fall-through: RemoveItem writes
    // `rec + 100/200/300` and variation 0 is "none". Same for the eye glow at
    // 1701, which is why every character used to look like a death knight.
    CHECK_FALSE(Visible(visible, 102));
    CHECK_FALSE(Visible(visible, 202));
    CHECK_FALSE(Visible(visible, 302));
    CHECK_FALSE(Visible(visible, 1701));

    // Above group 20 the client has no `m_geosets` slot, so the rule is
    // WoWModelViewer's: the `01` of each group, and the face group whole. 3201
    // and 3202 are one head between them — the small piece at the base of the
    // skull and the face above it — not two heads to pick from.
    CHECK(Visible(visible, 3201));
    CHECK(Visible(visible, 3202));
    CHECK(Visible(visible, 3301));
    CHECK(Visible(visible, 5101));
}

TEST_CASE("A customisation option owns its own ids and no others", "[m2][character]") {
    const std::vector<::whiteout::u16> declared = {0, 1, 2, 3, 401, 702, 703, 3201, 3202, 3203};
    CharacterGeosetSelection sel = DefaultSelection();
    // A Hair Style option over 1..3, an Ears option over 702/703, and a Face
    // Shape option over 3202/3203 — which never mentions 3201.
    for (const ::whiteout::u16 id : {1, 2, 3, 702, 703, 3202, 3203})
        sel.Control(id);
    sel.Show(3);
    sel.Show(703);
    sel.Show(3203);

    const std::vector<::whiteout::u16> visible = VisibleGeosets(sel, declared);
    CHECK(Visible(visible, 3));
    CHECK_FALSE(Visible(visible, 1));
    CHECK_FALSE(Visible(visible, 2));
    CHECK(Visible(visible, 703));
    CHECK_FALSE(Visible(visible, 702));
    CHECK(Visible(visible, 3203));
    CHECK_FALSE(Visible(visible, 3202));
    // The piece the option never names survives it. Narrowing the whole group
    // instead is what left a face-shaped hole where 3201 should be.
    CHECK(Visible(visible, 3201));
    // Untouched groups keep the default.
    CHECK(Visible(visible, 401));
    CHECK(Visible(visible, 0));
}

TEST_CASE("VisibleGeosets never names a geoset the model does not carry",
          "[m2][character]") {
    // The client asks for 901 whatever the model holds; a retail HD model
    // carries 902 and up. Emitting 901 anyway would hand the adapter an id no
    // submesh answers to.
    const std::vector<::whiteout::u16> declared = {0, 902, 903};
    const std::vector<::whiteout::u16> visible = VisibleGeosets(DefaultSelection(), declared);
    CHECK(visible.size() == 1);
    CHECK(visible[0] == 0);
    CHECK(std::is_sorted(visible.begin(), visible.end()));
}

// ---------------------------------------------------------------------------
// Compositing
// ---------------------------------------------------------------------------

namespace {

// A layout with one 8×4 sheet for texture type 1 and two sections: the whole
// thing, and a 4×2 patch at (4, 2).
ChrModelInfo TinyModel() {
    ChrModelInfo m;
    m.id = 1;
    m.layoutId = 1;
    m.composites.push_back({/*textureType*/ 1, /*width*/ 8, /*height*/ 4});
    m.sections.push_back({/*sectionType*/ 0, 0, 0, 8, 4});
    m.sections.push_back({/*sectionType*/ 1, 4, 2, 4, 2});
    return m;
}

std::vector<::whiteout::u8> SolidRGBA(::whiteout::u32 w, ::whiteout::u32 h,
                                      ::whiteout::u8 r, ::whiteout::u8 g, ::whiteout::u8 b,
                                      ::whiteout::u8 a) {
    std::vector<::whiteout::u8> px(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = r;
        px[i + 1] = g;
        px[i + 2] = b;
        px[i + 3] = a;
    }
    return px;
}

} // namespace

TEST_CASE("A composite pastes its base layer whole and its section layers in place",
          "[m2][character]") {
    const ChrModelInfo model = TinyModel();

    ResolvedAppearance appearance;
    // Layer 0: the skin, all-ones mask — one sheet covering the composite.
    appearance.pastes.push_back({1, 0, 0, -1, /*file*/ 10});
    // Layer 1: an overlay confined to section 1, half transparent.
    appearance.pastes.push_back({1, 1, 1, /*bit 1*/ 2, /*file*/ 11});

    const ImageFetch fetch = [](::whiteout::u32 file, std::vector<::whiteout::u8>& rgba,
                                ::whiteout::u32& w, ::whiteout::u32& h) {
        if (file == 10) {
            w = 8;
            h = 4;
            rgba = SolidRGBA(w, h, 200, 0, 0, 0); // alpha 0 on purpose — see below
            return true;
        }
        if (file == 11) {
            w = 4;
            h = 2;
            rgba = SolidRGBA(w, h, 0, 0, 200, 128);
            return true;
        }
        return false;
    };

    const std::vector<ComposedTexture> out = ComposeCharacter(model, appearance, fetch);
    REQUIRE(out.size() == 1);
    const ComposedTexture& sheet = out.front();
    CHECK(sheet.textureType == 1);
    CHECK(sheet.width == 8);
    CHECK(sheet.height == 4);
    REQUIRE(sheet.rgba.size() == 8u * 4u * 4u);

    auto at = [&](::whiteout::u32 x, ::whiteout::u32 y) {
        return &sheet.rgba[(static_cast<size_t>(y) * 8 + x) * 4];
    };

    // Outside the overlay: the base layer, forced opaque. A `.blp` whose alpha
    // channel is empty must not leave the body see-through.
    CHECK(at(0, 0)[0] == 200);
    CHECK(at(0, 0)[3] == 255);

    // Inside section 1: half of the blue overlay over the red base.
    const ::whiteout::u8* blended = at(5, 3);
    CHECK(blended[0] > 90);
    CHECK(blended[0] < 110);
    CHECK(blended[2] > 90);
    CHECK(blended[2] < 110);

    // Just outside the section rect, untouched.
    CHECK(at(3, 3)[0] == 200);
    CHECK(at(3, 3)[2] == 0);
}

TEST_CASE("A composite whose every source is missing produces nothing", "[m2][character]") {
    // Not a transparent sheet: an empty one would bind over whatever the model
    // already had and blank it.
    const ChrModelInfo model = TinyModel();
    ResolvedAppearance appearance;
    appearance.pastes.push_back({1, 0, 0, -1, 10});
    const ImageFetch none = [](::whiteout::u32, std::vector<::whiteout::u8>&, ::whiteout::u32&,
                               ::whiteout::u32&) { return false; };
    CHECK(ComposeCharacter(model, appearance, none).empty());
}

TEST_CASE("A source of a different size is resampled into its section",
          "[m2][character]") {
    // PasteToSection walks mips down to the section width and pastes 1:1, and
    // PasteScale covers the smaller case. Both come out as "fit the rect".
    const ChrModelInfo model = TinyModel();
    ResolvedAppearance appearance;
    appearance.pastes.push_back({1, 0, 0, -1, 10});
    appearance.pastes.push_back({1, 1, 1, 2, 11});

    const ImageFetch fetch = [](::whiteout::u32 file, std::vector<::whiteout::u8>& rgba,
                                ::whiteout::u32& w, ::whiteout::u32& h) {
        w = (file == 10) ? 4 : 8; // base half-size, overlay double-size
        h = (file == 10) ? 2 : 4;
        rgba = SolidRGBA(w, h, file == 10 ? 200 : 0, 0, file == 10 ? 0 : 200, 255);
        return true;
    };

    const std::vector<ComposedTexture> out = ComposeCharacter(model, appearance, fetch);
    REQUIRE(out.size() == 1);
    auto at = [&](::whiteout::u32 x, ::whiteout::u32 y) {
        return &out.front().rgba[(static_cast<size_t>(y) * 8 + x) * 4];
    };
    CHECK(at(0, 0)[0] == 200); // base stretched over the whole sheet
    CHECK(at(7, 3)[2] == 200); // overlay squeezed into the section rect
    CHECK(at(0, 3)[0] == 200); // and not outside it
}

// ---------------------------------------------------------------------------
// Collections models — the parts that are not in the character's own `.m2`.
//
// A Dracthyr's horns are twenty geosets of
// `item/objectcomponents/collections/collections_dracthyr_dt_m.m2`, a
// thirteen-bone rig against the character's two hundred and fifty-five. Nothing
// pairs the two by index; the only thing they agree on is the key bone.
// ---------------------------------------------------------------------------

namespace {

m2::Model RigWithKeyBones(std::initializer_list<::whiteout::i32> keyBoneIds) {
    m2::Model model;
    for (const ::whiteout::i32 k : keyBoneIds) {
        m2::Bone b;
        b.keyBoneId = k;
        b.parentBoneId = static_cast<::whiteout::i16>(model.bones.size()) - 1;
        model.bones.push_back(b);
    }
    return model;
}

} // namespace

TEST_CASE("A rig that rides another is paired by key bone, not by index",
          "[m2][character]") {
    namespace wow = ::whiteout::flakes::renderer::profiles::wow;

    // The shapes the real files have: the character is long and its key bones
    // land wherever the artist put them; the collections rig is short and
    // carries the same key bones in a different order at different indices.
    const m2::Model character = RigWithKeyBones({-1, -1, 4, -1, 2, 3, -1, 0, 1});
    const m2::Model collections = RigWithKeyBones({4, -1, 2, 3, 0, 1});

    const auto pairing = wow::PairBonesByKeyBone(character, collections);
    REQUIRE(pairing.size() == collections.bones.size());
    CHECK(pairing[0] == 2); // key 4
    CHECK(pairing[1] == -1); // the unpaired link in the middle of the chain
    CHECK(pairing[2] == 4); // key 2
    CHECK(pairing[3] == 5); // key 3
    CHECK(pairing[4] == 7); // key 0
    CHECK(pairing[5] == 8); // key 1

    // Every paired entry names a parent bone carrying that same key bone —
    // the property the whole bridge rests on.
    for (std::size_t i = 0; i < pairing.size(); ++i) {
        if (pairing[i] < 0)
            continue;
        CHECK(character.bones[static_cast<std::size_t>(pairing[i])].keyBoneId ==
              collections.bones[i].keyBoneId);
    }
}

TEST_CASE("A key bone the parent does not carry pairs with nothing", "[m2][character]") {
    namespace wow = ::whiteout::flakes::renderer::profiles::wow;
    // Not an error and not a fallback to index: a bone the character has no
    // counterpart for keeps posing itself off its own parent chain.
    const auto pairing = wow::PairBonesByKeyBone(RigWithKeyBones({0, 1}),
                                                 RigWithKeyBones({0, 7, 1}));
    REQUIRE(pairing.size() == 3);
    CHECK(pairing[0] == 0);
    CHECK(pairing[1] == -1);
    CHECK(pairing[2] == 1);
}
