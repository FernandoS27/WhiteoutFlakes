// ============================================================================
// Skin-driven particle colour — does the display record say what colour a
// creature's fire is?
//
// A `.m2` particle emitter can hand its colour keys over to the game: a
// `particleColorIndex` of 11, 12 or 13 claims one of the three slots
// `CreatureDisplayInfo::ParticleColorID` fills from `ParticleColor`, which is
// the *same* slot numbering `M2Texture::type` 11/12/13 uses for the skin's
// three textures. 2 375 shipped emitters across 464 models do it, and until
// this they all drew whatever colours the file happened to carry.
//
// Two halves, gated apart because they fail apart:
//
//   * The substitution rule, on hand-built models. It is narrower than it
//     looks — values but never key times, RGB but never alpha, exactly three
//     keys, and only the three indices that claim a slot — and every one of
//     those is a rule the shipped data cannot exercise on demand.
//   * The join, on the corpus. `CreatureModelData` names the `.m2`,
//     `CreatureDisplayInfo` names the colour, `ParticleColor` holds it.
//
// M2_SKIN_RECOLOR_DESIGN.md has the client side. The join half needs the corpus
// (`C:/Projects/WhiteoutLib/Corpus/WoW`, override with WDX_TEST_WOW_CORPUS) and
// its `dbfilesclient/` dump; it skips without them, and skipped is not passed.
// ============================================================================

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "io/file_content_provider.h"
#include "io/m2/m2_model_adapter.h"
#include "io/wow/creature_skin_table.h"
#include "io/wow/particle_color_table.h"
#include "renderer/core/particle_dialect.h"
#include "renderer/particle/base/particle2_emitter.h"
#include "renderer/particle/particle_adapters.h"
#include "renderer/profiles/wow/wow_replaceable_textures.h"
#include "whiteout/flakes/content_ref.h"

#include <whiteout/models/m2/m2.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
namespace io = whiteout::flakes::io;
namespace m2 = ::whiteout::m2;
namespace core = whiteout::flakes::renderer::core;
namespace particle = whiteout::flakes::renderer::particle;
namespace wow = whiteout::flakes::renderer::profiles::wow;

using Catch::Approx;
using whiteout::flakes::ContentRef;
using whiteout::flakes::u32;
using whiteout::flakes::usize;
using whiteout::flakes::Vector3f;
using whiteout::flakes::renderer::M2ParticleColorOverride;
using whiteout::flakes::renderer::M2ParticleEmitterConfig;

namespace {

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_WOW_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/WoW");
}

bool HaveTables(const fs::path& root) {
    std::error_code ec;
    return fs::is_directory(root / "dbfilesclient", ec);
}

void CheckColor(const Vector3f& got, u32 rgb) {
    CHECK(got.x == Approx(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f));
    CHECK(got.y == Approx(static_cast<float>((rgb >> 8) & 0xFF) / 255.0f));
    CHECK(got.z == Approx(static_cast<float>(rgb & 0xFF) / 255.0f));
}

bool SameColor(const Vector3f& got, const Vector3f& want) {
    return got.x == Approx(want.x) && got.y == Approx(want.y) && got.z == Approx(want.z);
}

/// A distinct colour per slot and key, so a wrong slot or a wrong key is a
/// wrong number rather than a coincidence.
M2ParticleColorOverride Distinct() {
    M2ParticleColorOverride o;
    for (u32 s = 0; s < M2ParticleColorOverride::kSlots; ++s)
        for (u32 k = 0; k < M2ParticleColorOverride::kKeys; ++k)
            o.key[s][k] = Vector3f{0.1f * static_cast<float>(s + 1),
                                   0.1f * static_cast<float>(k + 1), 0.5f};
    return o;
}

// ---- Hand-built models ------------------------------------------------------
//
// Only the colour track and the index matter here; everything else is left at
// its default, which is exactly the point — the rule under test reads two
// fields and must touch nothing else.

m2::ParticleEmitter MakeEmitter(whiteout::u16 colorIndex, usize keys) {
    m2::ParticleEmitter e;
    e.particleColorIndex = colorIndex;
    for (usize i = 0; i < keys; ++i) {
        // 0..255 in the file; the adapter divides.
        const float v = static_cast<float>(10 * (i + 1));
        e.colorTrack.values.push_back(Vector3f{v, v + 1.0f, v + 2.0f});
        e.colorTrack.timestamps.push_back(
            ::whiteout::unorm16::from_float(static_cast<float>(i) / static_cast<float>(keys)));
        e.alphaTrack.values.push_back(::whiteout::unorm16::from_float(0.25f));
        e.alphaTrack.timestamps.push_back(
            ::whiteout::unorm16::from_float(static_cast<float>(i) / static_cast<float>(keys)));
    }
    return e;
}

std::shared_ptr<io::M2ModelAdapter> BuildAdapter(std::vector<m2::ParticleEmitter> emitters) {
    m2::Model model;
    model.particleEmitters = std::move(emitters);
    return std::make_shared<io::M2ModelAdapter>(std::move(model));
}

struct Fixture {
    const char* model;
    u32 fileId;
    whiteout::u16 colorIndex;  ///< every recolourable emitter claims this one
    u32 particleColorId;       ///< on the lowest-id display, which is variation 0
};

// The two corpus carriers this can be run on. A fixture here has to clear
// three separate bars, and almost nothing does: the model must carry a
// recolourable emitter (464 of 14 060 do), its display row must actually name
// a colour (177 of those), and the loose `.m2` must be openable — which needs
// a `.skin` sibling, since a corpus with no CASC behind it cannot resolve one.
// Nine carriers survive all three, and these are two of them. Every one of the
// nine claims slot 11; slots 12 and 13 are pinned by the hand-built tests
// above and by `babyfireelemental` off a real install below.
constexpr Fixture kBatPet{"creature/batpet/batpet.m2", 1338861, 11, 1688};
constexpr Fixture kImp{"creature/imp/imp.m2", 124622, 11, 311};

/// A storage that knows its own names — what a WoW root with a listfile is and
/// a loose directory is not. The join is on fileDataID, so without one every
/// model here would fall through to the sibling route, which cannot say what
/// colour anything is.
class NamedProvider final : public io::IContentProvider {
public:
    explicit NamedProvider(io::IContentProvider& inner) : inner_(inner) {}

    io::RequestId Request(const ContentRef& ref, io::CompletionCallback cb) override {
        return inner_.Request(ref, std::move(cb));
    }
    void Wait(io::RequestId id) override {
        inner_.Wait(id);
    }
    void Cancel(io::RequestId id) override {
        inner_.Cancel(id);
    }
    void Pump() override {
        inner_.Pump();
    }
    u32 FileIdForPath(const std::string& path) const override {
        const auto it = ids.find(path);
        return it == ids.end() ? 0u : it->second;
    }

    std::unordered_map<std::string, u32> ids;

private:
    io::IContentProvider& inner_;
};

std::shared_ptr<io::M2ModelAdapter> LoadModel(io::IContentProvider& provider,
                                              const std::string& rel) {
    auto bytes = provider.ReadFile(rel);
    if (!bytes)
        return nullptr;
    return io::M2ModelAdapter::Load(ContentRef::FromPath(rel),
                                    std::span<const whiteout::u8>(bytes->data(), bytes->size()),
                                    &provider);
}

} // namespace

// ============================================================================
// The rule
// ============================================================================

TEST_CASE("the skin replaces a claimed emitter's colours and nothing else",
          "[m2][wow][particle]") {
    // One emitter per slot, plus one that claims none. Four emitters, and only
    // three of them may move.
    auto adapter = BuildAdapter({MakeEmitter(11, 3), MakeEmitter(12, 3), MakeEmitter(13, 3),
                                 MakeEmitter(0, 3)});
    REQUIRE(adapter);
    const auto plain = adapter->GetM2ParticleConfigs();
    REQUIRE(plain.size() == 4);

    const M2ParticleColorOverride skin = Distinct();
    adapter->SetParticleColorOverride(skin);
    const auto after = adapter->GetM2ParticleConfigs();
    REQUIRE(after.size() == plain.size());

    for (usize i = 0; i < 3; ++i) {
        INFO("emitter " << i << ", slot " << i);
        REQUIRE(after[i].colorValues.size() == M2ParticleColorOverride::kKeys);
        for (u32 k = 0; k < M2ParticleColorOverride::kKeys; ++k) {
            INFO("key " << k);
            CHECK(SameColor(after[i].colorValues[k], skin.key[i][k]));
        }
        // The `.m2` still says WHEN the colour changes: the client keeps
        // sampling the model's own track and swaps only the array the two
        // bracketing keys are read from.
        CHECK(after[i].colorTimes == plain[i].colorTimes);
        // ...and the alpha curve is not part of the replacement at all — the
        // row's alpha byte is never read.
        CHECK(after[i].alphaValues == plain[i].alphaValues);
        CHECK(after[i].alphaTimes == plain[i].alphaTimes);
    }

    // `particleColorIndex` 0 is what 49 775 of the corpus's 52 162 emitters
    // carry. It claims nothing, and a recolour must not spill onto it.
    CHECK(after[3].colorValues == plain[3].colorValues);
}

TEST_CASE("only a three-key track is replaced", "[m2][wow][particle]") {
    // The client's replacement array is three entries indexed by the track's
    // own key indices, so anything else is out of range — it fatals in a debug
    // build (`M2Model.cpp:3143`) and reads past the array in a release one.
    // Skipping is this renderer's one deliberate divergence, and the shipped
    // case is `spells/fotf_wings_slow.m2`: six emitters claiming slot 11 with
    // four keys each.
    auto adapter = BuildAdapter({MakeEmitter(11, 1), MakeEmitter(11, 2), MakeEmitter(11, 3),
                                 MakeEmitter(11, 4), MakeEmitter(11, 6)});
    REQUIRE(adapter);
    const auto plain = adapter->GetM2ParticleConfigs();
    REQUIRE(plain.size() == 5);

    adapter->SetParticleColorOverride(Distinct());
    const auto after = adapter->GetM2ParticleConfigs();
    REQUIRE(after.size() == plain.size());

    for (usize i = 0; i < after.size(); ++i) {
        INFO("emitter " << i << " with " << plain[i].colorValues.size() << " keys");
        if (plain[i].colorValues.size() == M2ParticleColorOverride::kKeys)
            CHECK(after[i].colorValues != plain[i].colorValues);
        else
            CHECK(after[i].colorValues == plain[i].colorValues);
    }
}

TEST_CASE("clearing the override puts the model's own colours back", "[m2][wow][particle]") {
    auto adapter = BuildAdapter({MakeEmitter(11, 3), MakeEmitter(13, 3)});
    REQUIRE(adapter);
    const auto plain = adapter->GetM2ParticleConfigs();

    adapter->SetParticleColorOverride(Distinct());
    REQUIRE(adapter->GetM2ParticleConfigs()[0].colorValues != plain[0].colorValues);

    // Stepping from a recoloured variation back to one that names no row has to
    // be symmetric, or the last colour applied sticks to every look after it.
    adapter->ClearParticleColorOverride();
    const auto restored = adapter->GetM2ParticleConfigs();
    REQUIRE(restored.size() == plain.size());
    for (usize i = 0; i < restored.size(); ++i) {
        INFO("emitter " << i);
        CHECK(restored[i].colorValues == plain[i].colorValues);
    }
}

TEST_CASE("only 11, 12 and 13 claim a slot", "[m2][wow][particle]") {
    // The client compares for equality against exactly these three. 12 corpus
    // emitters carry the -1 sentinel and the rest carry 0; neither ever
    // matches, which is what keeps them inert without a special case.
    CHECK(M2ParticleColorOverride::SlotOf(0) == -1);
    CHECK(M2ParticleColorOverride::SlotOf(1) == -1);
    CHECK(M2ParticleColorOverride::SlotOf(10) == -1);
    CHECK(M2ParticleColorOverride::SlotOf(11) == 0);
    CHECK(M2ParticleColorOverride::SlotOf(12) == 1);
    CHECK(M2ParticleColorOverride::SlotOf(13) == 2);
    CHECK(M2ParticleColorOverride::SlotOf(14) == -1);
    CHECK(M2ParticleColorOverride::SlotOf(0xFFFF) == -1);
}

// ============================================================================
// The join
// ============================================================================

TEST_CASE("ParticleColor decodes a row the way the client reads it", "[m2][wow][db2][particle]") {
    const fs::path root = CorpusRoot();
    if (!HaveTables(root)) {
        WARN("WoW corpus dbfilesclient/ not found — skipping");
        return;
    }

    io::FileContentProvider provider;
    provider.SetBasePath(root);

    io::wow::ParticleColorTable table;
    REQUIRE(table.Load(provider));
    CHECK(table.RowCount() > 1000);

    // One shipped row, every value. The table is three array fields of three,
    // and the array runs over the SLOT while the field runs over the KEY —
    // `SELECT "ID","START0","START1","START2","MID0",…` — so reading it
    // transposed produces colours that are individually plausible and wrong on
    // every model. Row 282's nine values are all different, which is what makes
    // them able to catch that.
    static constexpr u32 kRow282[M2ParticleColorOverride::kSlots][M2ParticleColorOverride::kKeys] =
        {
            {0x000766, 0x6D72C6, 0x385BB6},
            {0x005966, 0x008EA3, 0x00E8D4},
            {0x006266, 0x00BADD, 0xB5FDFF},
        };
    M2ParticleColorOverride row;
    REQUIRE(table.Resolve(282, row));
    for (u32 slot = 0; slot < M2ParticleColorOverride::kSlots; ++slot) {
        for (u32 key = 0; key < M2ParticleColorOverride::kKeys; ++key) {
            INFO("slot " << slot << " key " << key);
            CheckColor(row.key[slot][key], kRow282[slot][key]);
        }
    }
}

TEST_CASE("a colour id of zero is not the same as one that misses", "[m2][wow][db2][particle]") {
    const fs::path root = CorpusRoot();
    if (!HaveTables(root)) {
        WARN("WoW corpus dbfilesclient/ not found — skipping");
        return;
    }

    io::FileContentProvider provider;
    provider.SetBasePath(root);
    io::wow::ParticleColorTable table;
    REQUIRE(table.Load(provider));

    // Zero is what most display rows carry, and it means "leave the model's own
    // colours alone" — not "replace them with nothing".
    M2ParticleColorOverride out;
    CHECK_FALSE(table.Resolve(0, out));

    // A non-zero id that names no row is a different case, and the client marks
    // it: opaque green in all nine slots. Reproduced rather than quietly
    // skipped, because a green creature is a missing database row and hiding
    // that hides the reason.
    REQUIRE(table.Resolve(0xFFFFFF, out));
    for (const auto& slot : out.key)
        for (const auto& key : slot)
            CheckColor(key, 0x00FF00);
}

TEST_CASE("a creature's skin recolours the emitters that asked for it",
          "[m2][wow][db2][particle]") {
    const fs::path root = CorpusRoot();
    std::error_code ec;
    if (!HaveTables(root)) {
        WARN("WoW corpus dbfilesclient/ not found — skipping");
        return;
    }

    io::FileContentProvider backing;
    backing.SetBasePath(root);
    NamedProvider provider(backing);
    for (const Fixture& f : {kBatPet, kImp})
        provider.ids.emplace(f.model, f.fileId);

    wow::WowReplaceableTextures replaceables;
    replaceables.SetContentProvider(&provider);

    usize checked = 0;
    for (const Fixture& f : {kBatPet, kImp}) {
        INFO(f.model);
        if (!fs::exists(root / f.model, ec))
            continue;
        const auto plain = [&] {
            auto a = LoadModel(backing, f.model);
            return a ? a->GetM2ParticleConfigs() : std::vector<M2ParticleEmitterConfig>{};
        }();
        auto adapter = LoadModel(provider, f.model);
        REQUIRE(adapter);
        REQUIRE_FALSE(plain.empty());

        replaceables.Apply(*adapter, ContentRef::FromPath(f.model));

        // Variation 0 is the lowest display id, and that row names this colour.
        const auto& offered = replaceables.Variations(ContentRef::FromPath(f.model));
        REQUIRE_FALSE(offered.empty());
        REQUIRE(offered.front().particleColorId == f.particleColorId);

        M2ParticleColorOverride expected;
        REQUIRE(replaceables.ParticleColors().Resolve(f.particleColorId, expected));
        const auto slot = M2ParticleColorOverride::SlotOf(f.colorIndex);
        REQUIRE(slot >= 0);

        const auto skinned = adapter->GetM2ParticleConfigs();
        const auto& emitters = adapter->SourceModel().particleEmitters;
        REQUIRE(skinned.size() == plain.size());
        REQUIRE(emitters.size() == skinned.size());

        usize recoloured = 0;
        for (usize i = 0; i < skinned.size(); ++i) {
            INFO("emitter " << i << " particleColorIndex " << emitters[i].particleColorIndex);
            CHECK(skinned[i].colorTimes == plain[i].colorTimes);
            CHECK(skinned[i].alphaValues == plain[i].alphaValues);
            if (M2ParticleColorOverride::SlotOf(emitters[i].particleColorIndex) < 0) {
                CHECK(skinned[i].colorValues == plain[i].colorValues);
                continue;
            }
            ++recoloured;
            REQUIRE(skinned[i].colorValues.size() == M2ParticleColorOverride::kKeys);
            for (u32 k = 0; k < M2ParticleColorOverride::kKeys; ++k) {
                INFO("key " << k);
                CHECK(SameColor(skinned[i].colorValues[k], expected.key[slot][k]));
            }
        }
        // The premise, and what stops every check above from being vacuous: a
        // join that resolved nothing would satisfy all of them by leaving every
        // emitter alone.
        CHECK(recoloured > 0);
        ++checked;
    }
    CHECK(checked == 2);
}

TEST_CASE("a model with no replaceable texture is still skinned by its colours",
          "[m2][wow][db2][particle]") {
    // The 139-of-464 case: a creature whose entire skin is the colour of what
    // it is breathing. `avengingangel` declares nothing but type-0 textures and
    // nine emitters claiming slot 11, so a gate that looks only for a
    // replaceable texture never reads a row for it.
    //
    // Off the install rather than the corpus, because the loose `.m2` has no
    // `.skin` sibling and cannot be opened without one.
    constexpr u32 kAvengingAngel = 445928;

    io::FileContentProvider provider;
    provider.SetGame(whiteout::flakes::ProductId::Wow);
    if (provider.GamePath(whiteout::flakes::ProductId::Wow).empty() || !provider.HasCasc())
        SKIP("no World of Warcraft CASC install found");

    const ContentRef ref = ContentRef::FromFileId(kAvengingAngel);
    auto bytes = provider.ReadFile(ref);
    if (!bytes || bytes->empty())
        SKIP("creature/avengingangel is not in this build of the install");

    auto adapter = io::M2ModelAdapter::Load(
        ref, std::span<const whiteout::u8>(bytes->data(), bytes->size()), &provider);
    REQUIRE(adapter);
    // The premise: nothing here is a texture slot.
    for (const auto& tex : adapter->SourceModel().textures)
        REQUIRE(tex.type == 0);
    const auto plain = adapter->GetM2ParticleConfigs();
    REQUIRE_FALSE(plain.empty());

    wow::WowReplaceableTextures replaceables;
    replaceables.SetContentProvider(&provider);
    // Zero textures bound, because zero were. The count still means what it
    // always meant.
    CHECK(replaceables.Apply(*adapter, ref) == 0);

    const auto& offered = replaceables.Variations(ref);
    REQUIRE_FALSE(offered.empty());
    // The whole reason this model reached the tables at all. The display row
    // still names textures — a display fills TextureVariation whether or not
    // the model has a slot for it — so the colour is the only part of this look
    // that lands anywhere.
    CHECK(offered.front().particleColorId != 0);

    M2ParticleColorOverride expected;
    REQUIRE(replaceables.ParticleColors().Resolve(offered.front().particleColorId, expected));

    const auto skinned = adapter->GetM2ParticleConfigs();
    REQUIRE(skinned.size() == plain.size());
    usize recoloured = 0;
    for (usize i = 0; i < skinned.size(); ++i) {
        INFO("emitter " << i);
        const auto slot =
            M2ParticleColorOverride::SlotOf(adapter->SourceModel().particleEmitters[i].particleColorIndex);
        if (slot < 0)
            continue;
        ++recoloured;
        REQUIRE(skinned[i].colorValues.size() == M2ParticleColorOverride::kKeys);
        for (u32 k = 0; k < M2ParticleColorOverride::kKeys; ++k) {
            INFO("key " << k);
            CHECK(SameColor(skinned[i].colorValues[k], expected.key[slot][k]));
        }
    }
    CHECK(recoloured > 0);
}


TEST_CASE("stepping to a variation that names no colour restores the model's own",
          "[m2][wow][db2][particle]") {
    const fs::path root = CorpusRoot();
    std::error_code ec;
    if (!HaveTables(root) || !fs::exists(root / kImp.model, ec)) {
        WARN("corpus or creature/imp not found — skipping");
        return;
    }

    // The imp has four displays behind three looks, and one of those three
    // names no ParticleColorID at all. Stepping onto it has to put the model's
    // own colours back rather than keep the last row applied — the asymmetry a
    // one-way override would leave.
    io::FileContentProvider backing;
    backing.SetBasePath(root);
    NamedProvider provider(backing);
    provider.ids.emplace(kImp.model, kImp.fileId);

    const ContentRef ref = ContentRef::FromPath(kImp.model);
    const auto plain = [&] {
        auto a = LoadModel(backing, kImp.model);
        return a ? a->GetM2ParticleConfigs() : std::vector<M2ParticleEmitterConfig>{};
    }();
    REQUIRE_FALSE(plain.empty());

    wow::WowReplaceableTextures replaceables;
    replaceables.SetContentProvider(&provider);
    auto coloured = LoadModel(provider, kImp.model);
    REQUIRE(coloured);
    replaceables.Apply(*coloured, ref);

    const auto& offered = replaceables.Variations(ref);
    REQUIRE(offered.size() > 1);
    REQUIRE(offered.front().particleColorId == kImp.particleColorId);
    // Found rather than named: which index carries no colour belongs to the
    // table, not to this test.
    const auto uncoloured = std::find_if(offered.begin(), offered.end(),
                                         [](const auto& v) { return v.particleColorId == 0; });
    REQUIRE(uncoloured != offered.end());

    // The premise: variation 0 really did change something.
    const auto before = coloured->GetM2ParticleConfigs();
    REQUIRE(before.size() == plain.size());
    bool moved = false;
    for (usize i = 0; i < before.size(); ++i)
        moved = moved || before[i].colorValues != plain[i].colorValues;
    REQUIRE(moved);

    replaceables.SetVariation(static_cast<u32>(uncoloured - offered.begin()));
    auto restored = LoadModel(provider, kImp.model);
    REQUIRE(restored);
    replaceables.Apply(*restored, ref);
    const auto after = restored->GetM2ParticleConfigs();
    REQUIRE(after.size() == plain.size());
    for (usize i = 0; i < after.size(); ++i) {
        INFO("emitter " << i);
        CHECK(after[i].colorValues == plain[i].colorValues);
    }
}

TEST_CASE("one model, three slots, off a real install", "[m2][wow][db2][particle]") {
    // `babyfireelemental` is the shape no corpus carrier has: three emitters
    // claiming 11, 12 and 13, one each. It is the only end-to-end check that a
    // wrong slot cannot pass by luck — with one slot in play, reading the wrong
    // one still produces three colours from the same row.
    //
    // Off the install rather than the corpus, because the loose `.m2` has no
    // `.skin` sibling and cannot be opened without one.
    constexpr u32 kBabyFireElemental = 4870631;

    io::FileContentProvider provider;
    provider.SetGame(whiteout::flakes::ProductId::Wow);
    if (provider.GamePath(whiteout::flakes::ProductId::Wow).empty() || !provider.HasCasc())
        SKIP("no World of Warcraft CASC install found");

    const ContentRef ref = ContentRef::FromFileId(kBabyFireElemental);
    auto bytes = provider.ReadFile(ref);
    if (!bytes || bytes->empty())
        SKIP("creature/babyfireelemental is not in this build of the install");

    auto adapter = io::M2ModelAdapter::Load(
        ref, std::span<const whiteout::u8>(bytes->data(), bytes->size()), &provider);
    REQUIRE(adapter);
    const auto plain = adapter->GetM2ParticleConfigs();
    REQUIRE(plain.size() == 3);

    // The premise: one emitter per slot.
    const auto& emitters = adapter->SourceModel().particleEmitters;
    std::vector<int> slots;
    for (const auto& e : emitters)
        slots.push_back(M2ParticleColorOverride::SlotOf(e.particleColorIndex));
    std::sort(slots.begin(), slots.end());
    REQUIRE(slots == std::vector<int>{0, 1, 2});

    wow::WowReplaceableTextures replaceables;
    replaceables.SetContentProvider(&provider);
    replaceables.Apply(*adapter, ref);

    const auto& offered = replaceables.Variations(ref);
    REQUIRE_FALSE(offered.empty());
    REQUIRE(offered.front().particleColorId != 0);

    M2ParticleColorOverride expected;
    REQUIRE(replaceables.ParticleColors().Resolve(offered.front().particleColorId, expected));

    const auto skinned = adapter->GetM2ParticleConfigs();
    REQUIRE(skinned.size() == plain.size());
    for (usize i = 0; i < skinned.size(); ++i) {
        const int slot = M2ParticleColorOverride::SlotOf(emitters[i].particleColorIndex);
        INFO("emitter " << i << " slot " << slot);
        REQUIRE(slot >= 0);
        REQUIRE(skinned[i].colorValues.size() == M2ParticleColorOverride::kKeys);
        CHECK(skinned[i].colorTimes == plain[i].colorTimes);
        CHECK(skinned[i].alphaValues == plain[i].alphaValues);
        for (u32 k = 0; k < M2ParticleColorOverride::kKeys; ++k) {
            INFO("key " << k);
            CHECK(SameColor(skinned[i].colorValues[k], expected.key[slot][k]));
        }
    }
}

TEST_CASE("a live emitter takes the new colours without losing its particles",
          "[m2][wow][particle]") {
    // What a skin change actually does in the viewer: the actor stays where it
    // is and its emitters keep running, so the new colours have to land on an
    // emitter that is already alive. Rebuilding the emitter instead would put
    // the fire out and light it again, and leaving it alone was the bug —
    // every texture moved and the fire stayed the colour of the skin the model
    // was opened with.
    auto adapter = BuildAdapter({MakeEmitter(11, 3)});
    REQUIRE(adapter);
    const bool linear = false;

    const auto plain = adapter->GetM2ParticleConfigs();
    REQUIRE(plain.size() == 1);
    auto before = particle::DescFromM2Config(plain[0], linear);
    REQUIRE(before);

    particle::Emitter2 emitter;
    emitter.SetDesc(before);
    emitter.SetBehavior(core::ParticleBehavior::Wow());
    emitter.SetSeed(0x5C1Eu);
    emitter.SetEmissionRate(60.0f);
    emitter.SetLifeSpan(emitter.Desc().lifeSpan);
    emitter.Spawn().speed.base = 0.0f;
    emitter.Spawn().speed.variance = 0.0f;
    emitter.SetModelToWorld(whiteout::flakes::Matrix44f::identity());
    emitter.SetWorldPosition({0, 0, 0});
    emitter.SetVisible(true);
    emitter.Update(1.0f / 60.0f, 1.0f);
    const usize alive = emitter.Pool().AliveCount();
    REQUIRE(alive > 0);

    adapter->SetParticleColorOverride(Distinct());
    const auto recoloured = adapter->GetM2ParticleConfigs();
    auto after = particle::DescFromM2Config(recoloured[0], linear);
    REQUIRE(after);

    const bool squirt = emitter.SquirtPending();
    emitter.SetDesc(after);
    emitter.SetSquirtPending(squirt);

    // The colours moved — read the way the sim reads them, at the key times the
    // model still owns.
    const auto& curve = emitter.Desc().curves.color;
    REQUIRE(curve.KeyCount() == M2ParticleColorOverride::kKeys);
    for (u32 k = 0; k < M2ParticleColorOverride::kKeys; ++k) {
        INFO("key " << k);
        CHECK(SameColor(curve.Evaluate(curve.KeyTime(k)), Distinct().key[0][k]));
    }
    // ...and the particles did not.
    CHECK(emitter.Pool().AliveCount() == alive);
    CHECK(emitter.SquirtPending() == squirt);
}
