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
// What is gated here is the whole path — the table's own decode, the join that
// finds the row, and the substitution rule, which is narrower than it looks:
// values but never key times, RGB but never alpha, and exactly three keys or
// nothing at all. M2_SKIN_RECOLOR_DESIGN.md has the client side.
//
// Needs the corpus (`C:/Projects/WhiteoutLib/Corpus/WoW`, override with
// WDX_TEST_WOW_CORPUS) for the models and its `dbfilesclient/` dump for the
// tables. Skips when either is absent; skipped is not passed.
// ============================================================================

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "io/file_content_provider.h"
#include "io/m2/m2_model_adapter.h"
#include "io/wow/particle_color_table.h"
#include "renderer/profiles/wow/wow_replaceable_textures.h"
#include "whiteout/flakes/content_ref.h"

#include <whiteout/models/m2/m2.h>

#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
namespace io = whiteout::flakes::io;
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

/// What a plain load gives — the thing a recolour has to differ from.
std::vector<M2ParticleEmitterConfig> Configs(io::IContentProvider& provider,
                                             const std::string& rel) {
    auto adapter = LoadModel(provider, rel);
    return adapter ? adapter->GetM2ParticleConfigs() : std::vector<M2ParticleEmitterConfig>{};
}

void CheckColor(const Vector3f& got, u32 rgb) {
    CHECK(got.x == Approx(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f));
    CHECK(got.y == Approx(static_cast<float>((rgb >> 8) & 0xFF) / 255.0f));
    CHECK(got.z == Approx(static_cast<float>(rgb & 0xFF) / 255.0f));
}

bool SameColor(const Vector3f& got, const Vector3f& want) {
    return got.x == Approx(want.x) && got.y == Approx(want.y) && got.z == Approx(want.z);
}

M2ParticleColorOverride Flat(const Vector3f& c) {
    M2ParticleColorOverride o;
    for (auto& slot : o.key)
        for (auto& key : slot)
            key = c;
    return o;
}

struct Fixture {
    const char* model;
    u32 fileId;
    u32 particleColorId; ///< on the lowest-id display, which is variation 0
};

// Two creatures, chosen for what they prove rather than for being typical.
//
// `babyfireelemental` is the only kind of model that can pin the slot mapping:
// its three emitters claim slots 11, 12 and 13 one each, so a table read with
// the array and the field the wrong way round gives three wrong colours rather
// than one plausible one.
//
// `avengingangel` declares NO replaceable texture — every texture it has is
// type 0 — and nine emitters that all claim slot 11. It is the 139-of-464 case:
// a model whose entire skin is the colour of what it is breathing, and which a
// texture-only gate never looks at.
constexpr Fixture kBabyFireElemental{"creature/babyfireelemental/babyfireelemental.m2", 4870631,
                                     2204};
constexpr Fixture kAvengingAngel{"creature/avengingangel/avengingangel.m2", 445928, 596};

} // namespace

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
    if (!HaveTables(root) || !fs::exists(root / kBabyFireElemental.model, ec)) {
        WARN("corpus or babyfireelemental not found — skipping");
        return;
    }

    io::FileContentProvider backing;
    backing.SetBasePath(root);
    NamedProvider provider(backing);
    provider.ids.emplace(kBabyFireElemental.model, kBabyFireElemental.fileId);

    const auto plain = Configs(backing, kBabyFireElemental.model);
    REQUIRE(plain.size() == 3);

    wow::WowReplaceableTextures replaceables;
    replaceables.SetContentProvider(&provider);
    auto adapter = LoadModel(provider, kBabyFireElemental.model);
    REQUIRE(adapter);
    replaceables.Apply(*adapter, ContentRef::FromPath(kBabyFireElemental.model));
    const auto skinned = adapter->GetM2ParticleConfigs();
    REQUIRE(skinned.size() == plain.size());

    // Variation 0 is the lowest display id, and its row is the one below.
    const auto& offered = replaceables.Variations(ContentRef::FromPath(kBabyFireElemental.model));
    REQUIRE_FALSE(offered.empty());
    REQUIRE(offered.front().particleColorId == kBabyFireElemental.particleColorId);

    M2ParticleColorOverride expected;
    REQUIRE(replaceables.ParticleColors().Resolve(kBabyFireElemental.particleColorId, expected));

    // Three emitters claiming three different slots, so the mapping is pinned
    // end to end and not just inside the table.
    const auto& emitters = adapter->SourceModel().particleEmitters;
    REQUIRE(emitters.size() == skinned.size());
    usize recoloured = 0;
    for (usize i = 0; i < skinned.size(); ++i) {
        INFO("emitter " << i << " particleColorIndex " << emitters[i].particleColorIndex);
        const auto& before = plain[i];
        const auto& after = skinned[i];

        // Whatever else happens, the timing and the alpha stay the model's.
        CHECK(after.colorTimes == before.colorTimes);
        CHECK(after.alphaTimes == before.alphaTimes);
        CHECK(after.alphaValues == before.alphaValues);

        const auto slot = M2ParticleColorOverride::SlotOf(emitters[i].particleColorIndex);
        if (slot < 0) {
            CHECK(after.colorValues == before.colorValues);
            continue;
        }
        ++recoloured;
        REQUIRE(after.colorValues.size() == M2ParticleColorOverride::kKeys);
        for (u32 k = 0; k < M2ParticleColorOverride::kKeys; ++k) {
            INFO("key " << k);
            CHECK(SameColor(after.colorValues[k], expected.key[slot][k]));
        }
    }
    // The premise, and the reason the checks above are not vacuous: a table
    // that resolved nothing would satisfy every one of them by leaving all
    // three emitters alone.
    CHECK(recoloured == 3);
}

TEST_CASE("a model with no replaceable texture is still skinned by its colours",
          "[m2][wow][db2][particle]") {
    const fs::path root = CorpusRoot();
    std::error_code ec;
    if (!HaveTables(root) || !fs::exists(root / kAvengingAngel.model, ec)) {
        WARN("corpus or avengingangel not found — skipping");
        return;
    }

    io::FileContentProvider backing;
    backing.SetBasePath(root);
    NamedProvider provider(backing);
    provider.ids.emplace(kAvengingAngel.model, kAvengingAngel.fileId);

    const auto plain = Configs(backing, kAvengingAngel.model);
    REQUIRE(plain.size() == 9);

    wow::WowReplaceableTextures replaceables;
    replaceables.SetContentProvider(&provider);
    auto adapter = LoadModel(provider, kAvengingAngel.model);
    REQUIRE(adapter);
    // The premise: nothing here is a texture slot, so a gate that looks only
    // for one stops before reading a single row.
    for (const auto& tex : adapter->SourceModel().textures)
        REQUIRE(tex.type == 0);

    // ...and Apply still reports zero textures bound, because zero were. The
    // count means what it always meant.
    CHECK(replaceables.Apply(*adapter, ContentRef::FromPath(kAvengingAngel.model)) == 0);

    const auto& offered = replaceables.Variations(ContentRef::FromPath(kAvengingAngel.model));
    REQUIRE_FALSE(offered.empty());
    CHECK(offered.front().particleColorId == kAvengingAngel.particleColorId);
    // A look with no texture in it. The sibling fallback would have offered
    // this model's `.blp` neighbours here, filling a slot it does not declare.
    for (const auto& tex : offered.front().texture)
        CHECK(tex.empty());

    M2ParticleColorOverride expected;
    REQUIRE(replaceables.ParticleColors().Resolve(kAvengingAngel.particleColorId, expected));

    const auto skinned = adapter->GetM2ParticleConfigs();
    REQUIRE(skinned.size() == plain.size());
    usize recoloured = 0;
    for (usize i = 0; i < skinned.size(); ++i) {
        INFO("emitter " << i);
        CHECK(skinned[i].colorTimes == plain[i].colorTimes);
        CHECK(skinned[i].alphaValues == plain[i].alphaValues);
        REQUIRE(skinned[i].colorValues.size() == M2ParticleColorOverride::kKeys);
        // All nine claim slot 11, so all nine take slot 0's three colours.
        for (u32 k = 0; k < M2ParticleColorOverride::kKeys; ++k) {
            INFO("key " << k);
            CHECK(SameColor(skinned[i].colorValues[k], expected.key[0][k]));
        }
        recoloured += (skinned[i].colorValues != plain[i].colorValues) ? 1 : 0;
    }
    CHECK(recoloured > 0);
}

TEST_CASE("only a three-key track is replaced", "[m2][wow][particle]") {
    const fs::path root = CorpusRoot();
    std::error_code ec;
    // Six emitters, all claiming slot 11, all with a FOUR-key colour track.
    // The client's replacement array is three entries indexed by the track's
    // own key indices, so retail either fatals (debug) or reads past the array
    // (release). Skipping is the one place this deliberately diverges, and this
    // model is the only carrier in the whole corpus that reaches it.
    const std::string wings = "spells/fotf_wings_slow.m2";
    if (!fs::exists(root / wings, ec)) {
        WARN("spells/fotf_wings_slow.m2 not in the corpus — skipping");
        return;
    }

    io::FileContentProvider provider;
    provider.SetBasePath(root);

    const auto plain = Configs(provider, wings);
    REQUIRE(plain.size() == 6);
    for (const auto& cfg : plain)
        REQUIRE(cfg.colorValues.size() == 4); // the premise

    auto adapter = LoadModel(provider, wings);
    REQUIRE(adapter);
    for (const auto& e : adapter->SourceModel().particleEmitters)
        REQUIRE(M2ParticleColorOverride::SlotOf(e.particleColorIndex) == 0); // and the other half

    adapter->SetParticleColorOverride(Flat(Vector3f{1.0f, 0.0f, 0.0f}));
    const auto after = adapter->GetM2ParticleConfigs();
    REQUIRE(after.size() == plain.size());
    for (usize i = 0; i < after.size(); ++i) {
        INFO("emitter " << i);
        CHECK(after[i].colorValues == plain[i].colorValues);
    }
}

TEST_CASE("clearing the override puts the model's own colours back", "[m2][wow][particle]") {
    const fs::path root = CorpusRoot();
    std::error_code ec;
    if (!fs::exists(root / kAvengingAngel.model, ec)) {
        WARN("creature/avengingangel not in the corpus — skipping");
        return;
    }

    io::FileContentProvider provider;
    provider.SetBasePath(root);
    const auto plain = Configs(provider, kAvengingAngel.model);
    REQUIRE_FALSE(plain.empty());

    auto adapter = LoadModel(provider, kAvengingAngel.model);
    REQUIRE(adapter);
    adapter->SetParticleColorOverride(Flat(Vector3f{1.0f, 0.0f, 0.0f}));
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

TEST_CASE("an emitter that claims no slot is never recoloured", "[m2][wow][particle]") {
    // 49 775 of the corpus's 52 162 emitters carry `particleColorIndex` 0, and
    // 12 more carry the -1 sentinel. The client compares for equality against
    // 11, 12 and 13 only, so neither ever matches — which is what keeps a
    // recolour on one emitter from spilling onto the rest of the model.
    CHECK(M2ParticleColorOverride::SlotOf(0) == -1);
    CHECK(M2ParticleColorOverride::SlotOf(1) == -1);
    CHECK(M2ParticleColorOverride::SlotOf(10) == -1);
    CHECK(M2ParticleColorOverride::SlotOf(11) == 0);
    CHECK(M2ParticleColorOverride::SlotOf(12) == 1);
    CHECK(M2ParticleColorOverride::SlotOf(13) == 2);
    CHECK(M2ParticleColorOverride::SlotOf(14) == -1);
    CHECK(M2ParticleColorOverride::SlotOf(0xFFFF) == -1);
}

TEST_CASE("tmp diag", "[tmpdiag]") {
    io::FileContentProvider p;
    p.SetBasePath(CorpusRoot());
    std::ifstream in("C:/Users/ferna/AppData/Local/Temp/claude/"
                     "c--Projects-WhiteoutFlakes/17ac1b85-492c-4dd9-8f0f-4469871ea613/"
                     "scratchpad/carrier_info.txt");
    std::string line;
    usize ok = 0, bad = 0;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        for (usize at = 0;;) {
            const auto tab = line.find('	', at);
            f.push_back(line.substr(at, tab == std::string::npos ? tab : tab - at));
            if (tab == std::string::npos)
                break;
            at = tab + 1;
        }
        if (f.size() < 5)
            continue;
        auto a = LoadModel(p, f[0]);
        if (!a) {
            ++bad;
            continue;
        }
        ++ok;
        std::printf("OK %s slots=%s repl=%s keys=%s emitters=%zu\n", f[0].c_str(), f[2].c_str(),
                    f[3].c_str(), f[4].c_str(), a->SourceModel().particleEmitters.size());
    }
    std::printf("parsed %zu, failed %zu\n", ok, bad);
}
