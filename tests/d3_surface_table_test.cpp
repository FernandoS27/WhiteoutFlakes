// ============================================================================
// D3 looks, materials and the surface table — offline, no device.
//
// Three claims, all measured over the corpus rather than argued:
//
//  1. **`arVariants.size() == dwLookCount` on every AppearanceMaterial.** The
//     guide verified this on 64,899/64,899 slots, so a mismatch is a parse bug
//     and not content variation — asserted, and the count reported either way.
//
//  2. **The join is on `SubObject.szName`, not `szMaterialName`.** The two
//     WhiteoutLib field names are the wrong way round for what they hold, and
//     this is the measurement that says so: over 300 corpus files
//     `AppearanceMaterial.szName` matches `SubObject.szName` **2413 of 2413**
//     times and `szMaterialName` **once**. `szMaterialName` holds a per-instance
//     mesh id ("HC_x02_y01_<material>_001"). Joining on the plausibly-named
//     field leaves every sub-object materialless, which draws as unlit grey
//     rather than failing — so nothing says so.
//
//  3. **The (type, UV mode) census.** EMaterialTextureType's authored names were
//     stripped from the build; every branch of
//     Render_ResolveMaterialTextureStages names itself through the core asset it
//     falls back to. The type is the field at 0x00 — see io/d3/d3_types.h for
//     why, and for the two fields beside it that WhiteoutLib also names for
//     something else. What the census adds is the residue: types 25..38 arrive
//     as a model-wide block that is byte-identical across every material in a
//     file and is still unexplained. Printed so the day it is understood the
//     numbers are already in front of whoever reads it.
//
//  Three more cases follow, added when those field names turned out to be
//  wrong: the LUT-key measurement that settles which field is the type, the
//  per-look sub-object visibility bit, and the render state on the Shaders
//  asset's RenderPass (which needs an install, and says so).
//
// Corpus root: WDX_TEST_D3_CORPUS, default C:/Projects/WhiteoutLib/Corpus/D3.
// Skipped is not passed.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/d3/d3_model_adapter.h"
#include "io/d3/d3_sno_cache.h"
#include "io/file_content_provider.h"
#include "renderer/profiles/diablo3/d3_surface_table.h"

#include <whiteout/sno/d3/native/d3_native.h>
#include <whiteout/sno/d3/native/geometry.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <bit>
#include <string>
#include <functional>
#include <functional>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
namespace d3n = ::whiteout::sno::d3::native;
namespace d3p = ::whiteout::flakes::renderer::profiles::diablo3;
using namespace ::whiteout;
using ::whiteout::flakes::ContentRef;

namespace {

fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_D3_CORPUS"); v && *v)
        return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus/D3");
}

std::size_t SweepLimit() {
    if (const char* v = std::getenv("WDX_TEST_D3_LIMIT"); v && *v)
        return static_cast<std::size_t>(std::strtoul(v, nullptr, 10));
    return 300;
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

const char* NameOfType(i32 t) {
    switch (t) {
    case 1:
        return "Diffuse";
    case 2:
        return "Lightmap";
    case 3:
    case 47:
    case 48:
    case 49:
    case 50:
    case 51:
    case 52:
        return "NormalMap";
    case 7:
    case 9:
    case 39:
    case 60:
    case 61:
        return "engine render target";
    case 8:
        return "Irradiance";
    case 20:
    case 22:
    case 23:
    case 53:
        return "computed per draw";
    case 21:
        return "Vignette";
    case 24:
        return "ShadowMask";
    case 56:
        return "DyeRamp";
    case 59:
        return "BannerDye";
    default:
        return "(default branch: the entry's own texture)";
    }
}

std::size_t CountIf(const std::vector<fs::path>& files, std::size_t take,
                    const std::function<void(const d3n::Appearances&)>& fn) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < take; ++i) {
        auto app = d3n::parseAppearances(ReadAll(files[i]));
        if (!app)
            continue;
        ++n;
        fn(*app);
    }
    return n;
}

bool EqualCiSv(const std::string& a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    for (std::size_t k = 0; k < a.size(); ++k) {
        if (std::tolower(static_cast<unsigned char>(a[k])) !=
            std::tolower(static_cast<unsigned char>(b[k])))
            return false;
    }
    return true;
}

} // namespace

TEST_CASE("D3 corpus: the look invariant and the material name join", "[d3][corpus]") {
    const auto files = FindFiles(CorpusRoot() / "Appearances", ".app");
    if (files.empty()) {
        WARN("No D3 corpus at " << (CorpusRoot() / "Appearances").string()
                                << " (set WDX_TEST_D3_CORPUS). SKIPPED, not passed.");
        return;
    }
    const std::size_t limit = SweepLimit();
    const std::size_t take = (limit == 0) ? files.size() : (std::min)(limit, files.size());

    std::size_t materials = 0, variantMismatch = 0;
    std::size_t subObjects = 0, matched = 0;
    std::size_t looksTotal = 0, multiLook = 0;
    std::map<i32, std::size_t> typeCounts;
    std::map<i32, std::size_t> slotCounts;
    std::map<std::pair<i32,i32>, std::size_t> pairCounts;
    std::map<int, std::size_t> type0PerVariant;
    std::vector<std::string> sampleNames;
    std::size_t byName = 0, byMaterialName = 0;
    auto eqCi = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size())
            return false;
        for (std::size_t k = 0; k < a.size(); ++k) {
            if (std::tolower(static_cast<unsigned char>(a[k])) !=
                std::tolower(static_cast<unsigned char>(b[k])))
                return false;
        }
        return true;
    };
    std::size_t textureEntries = 0, entriesWithTexture = 0, pastStageCap = 0;
    std::vector<std::string> mismatchNames;

    for (std::size_t i = 0; i < take; ++i) {
        auto app = d3n::parseAppearances(ReadAll(files[i]));
        if (!app)
            continue;

        const std::size_t lookCount = app->arLooks.size();
        looksTotal += lookCount;
        if (lookCount > 1)
            ++multiLook;

        for (const auto& mat : app->arMaterials) {
            ++materials;
            // dwLookCount and arLooks agree in every shipped file; arVariants
            // is what has to match them.
            if (lookCount > 0 && mat.arVariants.size() != lookCount) {
                ++variantMismatch;
                if (mismatchNames.size() < 20) {
                    mismatchNames.push_back(files[i].filename().string() + ":" + mat.szName);
                }
            }
            for (const auto& v : mat.arVariants) {
                {
                    int n0 = 0;
                    for (const auto& e : v.tMaterial.arTextures)
                        if (flakes::io::D3TextureTypeOf(e) == 1)
                            ++n0;
                    ++type0PerVariant[n0];
                }
                if (v.tMaterial.arTextures.size() > d3p::kD3MaxTextureStages)
                    pastStageCap += v.tMaterial.arTextures.size() - d3p::kD3MaxTextureStages;
                for (const auto& e : v.tMaterial.arTextures) {
                    ++textureEntries;
                    ++typeCounts[flakes::io::D3TextureTypeOf(e)];
                    ++slotCounts[flakes::io::D3UvFlagsOf(e)];
                    ++pairCounts[{flakes::io::D3TextureTypeOf(e), flakes::io::D3UvModeOf(e)}];
                    if (e.snoTexture.valid())
                        ++entriesWithTexture;
                }
            }
        }

        const d3n::GeoSet* sets[2] = {&app->tGeoSet0, &app->tGeoSet1};
        for (const auto* set : sets) {
            for (const auto& sub : set->arSubObjects) {
                if (sub.arVertices.empty() || sub.arIndices.empty())
                    continue;
                ++subObjects;
                for (const auto& m : app->arMaterials) {
                    if (eqCi(m.szName, sub.szName))
                        ++byName;
                    if (eqCi(m.szName, sub.szMaterialName))
                        ++byMaterialName;
                }
                if (flakes::io::D3VariantFor(*app, sub, 0) != nullptr) {
                    ++matched;
                } else if (sampleNames.size() < 10) {
                    std::string s = files[i].filename().string() + " sub '" + sub.szName +
                                    "' wants material '" + sub.szMaterialName + "'; file has:";
                    for (std::size_t m = 0; m < app->arMaterials.size() && m < 4; ++m)
                        s += " '" + app->arMaterials[m].szName + "'";
                    sampleNames.push_back(std::move(s));
                }
            }
        }
    }

    const double matchPct =
        subObjects ? (100.0 * static_cast<double>(matched) / static_cast<double>(subObjects)) : 0.0;
    std::printf("[d3-mat] %zu/%zu files | %zu materials (%zu with arVariants != dwLookCount) | "
                "%zu looks total, %zu files with more than one\n",
                take, files.size(), materials, variantMismatch, looksTotal, multiLook);
    std::printf("[d3-mat] %zu drawable sub-objects, %zu found a material (%.2f%%), "
                "%zu UNMATCHED\n",
                subObjects, matched, matchPct, subObjects - matched);
    std::printf("[d3-mat] %zu texture entries, %zu naming a texture, %zu past the matTex11 cap\n",
                textureEntries, entriesWithTexture, pastStageCap);
    std::printf("[d3-mat] EMaterialTextureType census (the field at 0x00):\n");
    for (const auto& [type, n] : typeCounts)
        std::printf("[d3-mat]   %4d  x%-8zu  %s\n", type, n, NameOfType(type));
    std::printf("[d3-mat] JOIN: AppearanceMaterial.szName matches SubObject.szName %zu times, "
                "SubObject.szMaterialName %zu times, of %zu sub-objects\n",
                byName, byMaterialName, subObjects);
    std::printf("[d3-mat] (type, UV mode) pairs:\n");
    for (const auto& [k, n] : pairCounts)
        std::printf("[d3-mat]   type %-3d mode %-3d x%zu\n", k.first, k.second, n);
    std::printf("[d3-mat] entries of type 1 (Diffuse), per variant:\n");
    for (const auto& [n0, n] : type0PerVariant)
        std::printf("[d3-mat]   %d x%zu\n", n0, n);
    std::printf("[d3-mat] UV flags word (the field at 0x98) census:\n");
    for (const auto& [slot, n] : slotCounts)
        std::printf("[d3-mat]   %4d  x%zu\n", slot, n);
    for (const auto& s : sampleNames)
        std::printf("[d3-mat]   UNMATCHED %s\n", s.c_str());
    for (const auto& n : mismatchNames)
        std::printf("[d3-mat]   variant/look mismatch: %s\n", n.c_str());

    REQUIRE(materials > 0);
    REQUIRE(subObjects > 0);
    // The invariant the guide verified on 64,899/64,899 slots. A mismatch here
    // is a parse bug — the look index is what selects a variant, and a short
    // list means every look past the end silently reads look 0.
    CHECK(variantMismatch == 0);
    // The join, and the evidence for which field it is on. `szName` matched
    // every sampled sub-object; `szMaterialName` matched one. Both numbers are
    // asserted, because the second is what makes the first a *choice* rather
    // than a coincidence — a change that silently swapped them back would leave
    // matchPct high only if it also broke this.
    CHECK(matchPct > 99.0);
    CHECK(byName == subObjects);
    CHECK(byMaterialName * 100 < subObjects);
}

TEST_CASE("D3: the canonical texture list and the table index into the same call", "[d3][corpus]") {
    const auto files = FindFiles(CorpusRoot() / "Appearances", ".app");
    if (files.empty()) {
        WARN("No D3 corpus. SKIPPED, not passed.");
        return;
    }
    // A cache with no content provider: every SNO lookup misses, which is
    // exactly right offline — a corpus `.app` carries its materials inline and
    // a `snoMaterial`-only variant has nothing to resolve against. That is the
    // shape a browsed file has too, so it is worth exercising.
    flakes::io::D3SnoCache cache(nullptr);

    const std::size_t take = std::min<std::size_t>(80, files.size());
    std::size_t built = 0, surfaces = 0, valid = 0, slotsBound = 0, danglingIds = 0;
    std::size_t textureLists = 0, textureRefs = 0;

    for (std::size_t i = 0; i < take; ++i) {
        const auto bytes = ReadAll(files[i]);
        auto adapter = flakes::io::D3ModelAdapter::LoadAppearance(
            ContentRef::FromPath(files[i].string()), bytes, cache);
        if (!adapter)
            continue;
        ++built;

        // The invariant that broke twice on `.m3` before it was written down:
        // GetTextures() emits exactly this list and the table indexes into the
        // same call, so the two agree by construction. Checked by building both
        // and pinning the sizes and the ids against each other.
        const auto textures =
            flakes::io::CollectD3Textures(adapter->SourceAppearance(), adapter->LookIndex());
        const auto emitted = adapter->GetTextures();
        REQUIRE(emitted.size() == textures.size());
        for (std::size_t t = 0; t < textures.size(); ++t) {
            CHECK(emitted[t].textureId == static_cast<i32>(t));
            // "#<snoId>", the form ContentRef::Describe produces and the
            // staging path parses. Any other spelling resolves as a *path*
            // against nothing and leaves every texture on the white
            // placeholder with no error anywhere.
            CHECK(emitted[t].sharedKey == "#" + std::to_string(textures[t].snoId));
        }
        if (!textures.empty()) {
            ++textureLists;
            textureRefs += textures.size();
        }

        d3p::D3TypeCensus census;
        auto table = d3p::BuildD3SurfaceTable(adapter->SourceAppearance(), adapter->LookIndex(),
                                              textures, adapter->EmittedSubObjects(), &cache, {},
                                              &census);
        REQUIRE(table != nullptr);
        // Entry g describes geoset g: the table and the mesh list are the same
        // length by construction, never by parallel iteration.
        CHECK(table->Surfaces().size() == adapter->EmittedSubObjects().size());
        CHECK(table->Product() == flakes::renderer::core::ProductId::D3);

        for (const auto& s : table->Surfaces()) {
            ++surfaces;
            if (s.valid)
                ++valid;
            for (const auto& slot : s.slots) {
                if (slot.textureId < 0)
                    continue;
                ++slotsBound;
                // A slot's textureId indexes the canonical list. Out of range
                // would mean the table and GetTextures had drifted, which is
                // the failure this whole arrangement exists to make impossible.
                if (static_cast<std::size_t>(slot.textureId) >= textures.size())
                    ++danglingIds;
            }
        }
    }

    std::printf("[d3-tbl] %zu appearances built | %zu surfaces, %zu valid | %zu bound slots, "
                "%zu dangling texture ids | %zu appearances with textures, %zu refs\n",
                built, surfaces, valid, slotsBound, danglingIds, textureLists, textureRefs);

    REQUIRE(built > 0);
    CHECK(surfaces > 0);
    CHECK(danglingIds == 0);
}

// Hidden by default (Catch2 skips a tag starting with '.'): run it with
// `d3_surface_table_test [.diag]` when the question is "what does one shipped
// material actually contain". It is what settled which of the two string fields
// is the material name, and which texture entries are per-material.
TEST_CASE("D3 diag: one character's material entries", "[.diag][d3]") {
    const fs::path f = CorpusRoot() / "Appearances" / "Barbarian_Male.app";
    std::error_code ec;
    if (!fs::exists(f, ec)) {
        WARN("no Barbarian_Male.app");
        return;
    }
    auto app = d3n::parseAppearances(ReadAll(f));
    REQUIRE(app);
    std::printf("[diag] looks=%zu materials=%zu bones=%zu\n", app->arLooks.size(),
                app->arMaterials.size(), app->arBones.size());
    for (std::size_t m = 0; m < app->arMaterials.size() && m < 3; ++m) {
        const auto& mat = app->arMaterials[m];
        std::printf("[diag] material '%s' variants=%zu\n", mat.szName.c_str(),
                    mat.arVariants.size());
        for (std::size_t v = 0; v < mat.arVariants.size() && v < 2; ++v) {
            const auto& var = mat.arVariants[v];
            std::printf("[diag]   variant %zu: snoMaterial=%d embedded entries=%zu shaderMap=%d\n",
                        v, var.snoMaterial.id, var.tMaterial.arTextures.size(),
                        var.tMaterial.snoShaderMap.id);
            for (const auto& e : var.tMaterial.arTextures) {
                std::printf("[diag]     type=%-3d uvMode=%-3d tex=%-7d uvFlags=0x%X unk04=%d "
                            "unk9C=%d\n",
                            flakes::io::D3TextureTypeOf(e), flakes::io::D3UvModeOf(e), e.snoTexture.id,
                            flakes::io::D3UvFlagsOf(e),
                            e.dwUnknown04, e.dwUnknown9C);
            }
        }
    }
    const d3n::GeoSet* sets[2] = {&app->tGeoSet0, &app->tGeoSet1};
    int shown = 0;
    for (const auto* set : sets)
        for (const auto& sub : set->arSubObjects)
            if (shown++ < 6)
                std::printf("[diag] sub name='%s' materialName='%s' verts=%zu\n",
                            sub.szName.c_str(), sub.szMaterialName.c_str(), sub.arVertices.size());
}

// ============================================================================
// The three fields of MaterialTextureEntry that WhiteoutLib names for the wrong
// thing, and the two mechanisms reading them wrong cost.
//
// Every claim here is a counting claim, because the failure it guards against
// is invisible by eye: a material with four texture layers renders *something*
// whichever layer you bind as the diffuse, and a model that draws its whole
// wardrobe at once still looks like a model.
// ============================================================================

TEST_CASE("D3 corpus: the texture entry's LUT key is the field at 0x00", "[d3][corpus]") {
    const auto files = FindFiles(CorpusRoot() / "Appearances", ".app");
    if (files.empty()) {
        WARN("No D3 corpus at " << (CorpusRoot() / "Appearances").string()
                                << " (set WDX_TEST_D3_CORPUS). SKIPPED, not passed.");
        return;
    }
    const std::size_t limit = SweepLimit();
    const std::size_t take = (limit == 0) ? files.size() : (std::min)(limit, files.size());

    std::size_t variants = 0, dupType = 0, dupUvFlags = 0;
    std::size_t sole = 0, soleIsDiffuse = 0, noDiffuse = 0;
    std::map<i32, std::size_t> modes;
    std::size_t animated = 0, animatedVariants = 0, entries = 0;
    const std::size_t parsed = CountIf(files, take, [&](const d3n::Appearances& app) {
        for (const auto& mat : app.arMaterials) {
            for (const auto& v : mat.arVariants) {
                const auto& t = v.tMaterial.arTextures;
                if (t.empty())
                    continue;
                ++variants;
                std::map<i32, int> byType, byUvFlags;
                bool hasDiffuse = false, moves = false;
                for (const auto& e : t) {
                    ++entries;
                    ++byType[flakes::io::D3TextureTypeOf(e)];
                    ++byUvFlags[flakes::io::D3UvFlagsOf(e)];
                    ++modes[flakes::io::D3UvModeOf(e)];
                    if (flakes::io::D3TextureTypeOf(e) == 1)
                        hasDiffuse = true;
                    if (flakes::io::D3ReadUvXform(e).animated) {
                        ++animated;
                        moves = true;
                    }
                }
                for (const auto& kv : byType) {
                    if (kv.second > 1) {
                        ++dupType;
                        break;
                    }
                }
                for (const auto& kv : byUvFlags) {
                    if (kv.second > 1) {
                        ++dupUvFlags;
                        break;
                    }
                }
                if (t.size() == 1) {
                    ++sole;
                    if (flakes::io::D3TextureTypeOf(t[0]) == 1)
                        ++soleIsDiffuse;
                }
                if (!hasDiffuse)
                    ++noDiffuse;
                if (moves)
                    ++animatedVariants;
            }
        }
    });

    std::printf("[d3-fields] %zu file(s), %zu material variants, %zu texture entries\n", parsed,
                variants, entries);
    std::printf("[d3-fields] a value repeats inside one material: type@0x00 %zu (%.2f%%), "
                "uvFlags@0x98 %zu (%.2f%%)\n",
                dupType, 100.0 * static_cast<double>(dupType) / static_cast<double>(variants),
                dupUvFlags,
                100.0 * static_cast<double>(dupUvFlags) / static_cast<double>(variants));
    std::printf("[d3-fields] single-entry variants %zu, of which type 1 %zu; variants with no "
                "type-1 entry %zu (%.2f%%)\n",
                sole, soleIsDiffuse, noDiffuse,
                100.0 * static_cast<double>(noDiffuse) / static_cast<double>(variants));
    std::printf("[d3-fields] UV mode census:");
    for (const auto& m : modes)
        std::printf(" %d:%zu", m.first, m.second);
    std::printf("\n[d3-fields] animated entries %zu, in %zu variants (%.2f%%)\n", animated,
                animatedVariants,
                100.0 * static_cast<double>(animatedVariants) / static_cast<double>(variants));

    REQUIRE(variants > 1000);

    // THE key property, and the whole reason the field at 0x00 is the type:
    // Render_ResolveMaterialTextureStages builds `dest[type] = entry`, so a
    // repeated value would silently lose an entry. It never repeats.
    CHECK(dupType == 0);
    // And the field that was being read as the type does repeat, in most
    // materials — it cannot be a key at all. Asserted as well as measured,
    // because it is what makes the first number a *choice* rather than a
    // coincidence: a swap back would have to break this to pass that.
    CHECK(dupUvFlags * 2 > variants);

    // A material with exactly one texture has a base map and nothing else,
    // which is what makes type 1 the diffuse rather than a guess.
    CHECK(sole > 100);
    CHECK(soleIsDiffuse * 1000 > sole * 999);

    // Almost everything authors a diffuse. The residue is real content — effect
    // materials whose only layers are detail types — not a mapping gap, but it
    // is bounded, and a change that widened it would be a regression.
    CHECK(noDiffuse * 20 < variants);

    // The UV mode field takes only sub_71000F8590's own case labels. A value
    // outside 0..6 would mean the field is not that enum.
    CHECK(modes.upper_bound(6) == modes.end());
    CHECK(modes.begin()->first >= 0);

    // Enough of the corpus animates that dropping the rates is a visible loss,
    // which is the case for reproducing them at all.
    CHECK(animatedVariants * 4 > variants);
}

TEST_CASE("D3 corpus: a look decides which sub-objects draw", "[d3][corpus]") {
    const auto files = FindFiles(CorpusRoot() / "Appearances", ".app");
    if (files.empty()) {
        WARN("No D3 corpus. SKIPPED, not passed.");
        return;
    }
    const std::size_t limit = SweepLimit();
    const std::size_t take = (limit == 0) ? files.size() : (std::min)(limit, files.size());

    std::size_t drawable = 0, hidden = 0, allHiddenLooks = 0, looksSeen = 0;
    const std::size_t parsed = CountIf(files, take, [&](const d3n::Appearances& app) {
        const std::size_t looks = (std::max)(static_cast<std::size_t>(1), app.arLooks.size());
        const d3n::GeoSet* sets[2] = {&app.tGeoSet0, &app.tGeoSet1};
        for (std::size_t look = 0; look < looks; ++look) {
            ++looksSeen;
            std::size_t shown = 0, total = 0;
            for (const auto* set : sets) {
                for (const auto& sub : set->arSubObjects) {
                    if (sub.arVertices.empty() || sub.arIndices.empty())
                        continue;
                    ++total;
                    const auto* v = flakes::io::D3VariantFor(app, sub, static_cast<u32>(look));
                    if (look == 0)
                        ++drawable;
                    if (v && (v->dwUnknown00 & flakes::io::kD3SubObjectVisibleBit) == 0) {
                        if (look == 0)
                            ++hidden;
                    } else {
                        ++shown;
                    }
                }
            }
            if (total > 0 && shown == 0)
                ++allHiddenLooks;
        }
    });

    std::printf("[d3-look] %zu file(s), %zu look(s); at look 0, %zu of %zu drawable sub-objects "
                "are hidden by the visibility bit (%.2f%%); looks that hide everything: %zu\n",
                parsed, looksSeen, hidden, drawable,
                100.0 * static_cast<double>(hidden) / static_cast<double>(drawable),
                allHiddenLooks);

    REQUIRE(drawable > 1000);
    // The bit does work. Without this the rule could be a no-op and every model
    // would look exactly as it did before it existed.
    CHECK(hidden > 0);
    // ...and it is not a rule that hides most of the world. A model's default
    // look shows most of what it ships; the hidden residue is death bodies,
    // alternate weapons and the pieces another look wears instead.
    CHECK(hidden * 2 < drawable);
    // Looks that hide EVERYTHING exist and are named for it — Imperius ships
    // one called `Invisible` — so this is a bound rather than a zero. Measured
    // at 8.1% of looks; a rule that had the bit inverted would put it near 100.
    CHECK(allHiddenLooks * 4 < looksSeen);
}

TEST_CASE("D3: the three models this was found on", "[d3][corpus]") {
    struct Want {
        const char* file;
        const char* subObject;
        bool visibleAtLook0;
        bool wantsDiffuse;
        bool wantsUvAnim;
    };
    // Tyrael ships the Stranger, the Restored angel and a skeleton; look `A` is
    // the Stranger alone, and drawing all three put a skull through his head.
    // Imperius's and Malthael's wings are multi-layer scrolling materials whose
    // base map is type 1 — neither had one bound, and Malthael's had no texture
    // at all because none of his wing layers is type 0.
    const Want kWant[] = {
        {"Tyrael", "A_normal_mat", true, true, false},
        {"Tyrael", "A_restored_mat", false, true, false},
        {"Tyrael", "A_skeleton_mat", false, true, false},
        {"Imperius", "wing_mat", true, true, true},
        {"Imperius", "A_unarmed_mat", false, true, false},
        {"x1_Malthael", "wingMidLayer_mat", true, true, true},
        {"x1_Malthael", "wingOuter_mat", true, true, true},
        {"x1_Malthael", "A_wings_mat", false, true, false},
    };

    std::size_t checked = 0;
    for (const auto& w : kWant) {
        const auto path = CorpusRoot() / "Appearances" / (std::string(w.file) + ".app");
        auto app = d3n::parseAppearances(ReadAll(path));
        if (!app) {
            WARN("missing " << path.string() << " -- SKIPPED, not passed.");
            continue;
        }
        const d3n::GeoSet* sets[2] = {&app->tGeoSet0, &app->tGeoSet1};
        const d3n::SubObject* sub = nullptr;
        for (const auto* set : sets) {
            for (const auto& s : set->arSubObjects) {
                if (EqualCiSv(s.szName, w.subObject))
                    sub = &s;
            }
        }
        REQUIRE(sub != nullptr);
        const auto* v = flakes::io::D3VariantFor(*app, *sub, 0);
        REQUIRE(v != nullptr);
        ++checked;

        INFO(w.file << " / " << w.subObject);
        const bool visible = (v->dwUnknown00 & flakes::io::kD3SubObjectVisibleBit) != 0;
        CHECK(visible == w.visibleAtLook0);

        bool diffuse = false, moves = false;
        for (const auto& e : v->tMaterial.arTextures) {
            if (flakes::io::D3SlotOfType(flakes::io::D3TextureTypeOf(e)) !=
                flakes::io::D3SlotKind::Diffuse)
                continue;
            diffuse = diffuse || e.snoTexture.valid();
            moves = moves || flakes::io::D3ReadUvXform(e).animated;
        }
        CHECK(diffuse == w.wantsDiffuse);
        CHECK(moves == w.wantsUvAnim);
    }
    if (checked == 0) {
        WARN("None of the three models present. SKIPPED, not passed.");
    }
}

// ============================================================================
// The vertex colour is a LIGHT term, and the corpus is what says so.
//
// Nothing in the file marks the attribute's role; the shipped vertex programs
// do (`vs_scene` ends `MAD R0.xyz, vertex.attrib[3], 2, R0`, an ADD into the
// light sum, while `vs_irrad_*` reads only `.w`). This is the measurement that
// makes that reading the only tenable one, without needing the programs: the
// attribute is bimodal, and the black mode is the majority. A tint that is
// (0,0,0) on four fifths of the game's meshes is not a tint.
// ============================================================================

TEST_CASE("D3 corpus: the vertex colour is bimodal, so it cannot be a tint", "[d3][corpus]") {
    const auto files = FindFiles(CorpusRoot() / "Appearances", ".app");
    if (files.empty()) {
        WARN("No D3 corpus at " << (CorpusRoot() / "Appearances").string()
                                << " (set WDX_TEST_D3_CORPUS). SKIPPED, not passed.");
        return;
    }
    const std::size_t limit = SweepLimit();
    const std::size_t take = (limit == 0) ? files.size() : (std::min)(limit, files.size());

    std::size_t subObjects = 0, allBlack = 0, allWhite = 0, mixed = 0;
    CountIf(files, take, [&](const d3n::Appearances& app) {
        const d3n::GeoSet* sets[2] = {&app.tGeoSet0, &app.tGeoSet1};
        for (const auto* set : sets) {
            for (const auto& sub : set->arSubObjects) {
                if (sub.arVertices.empty())
                    continue;
                ++subObjects;
                bool black = true, white = true;
                for (const auto& v : sub.arVertices) {
                    const auto c = d3n::vertexColor(v);
                    if (c.r != 0 || c.g != 0 || c.b != 0)
                        black = false;
                    if (c.r != 255 || c.g != 255 || c.b != 255)
                        white = false;
                }
                if (black)
                    ++allBlack;
                else if (white)
                    ++allWhite;
                else
                    ++mixed;
            }
        }
    });
    REQUIRE(subObjects > 0);
    std::printf("[d3-vcol] %zu sub-objects: RGB all-zero %zu (%.1f%%), all-white %zu (%.1f%%), "
                "graded %zu (%.1f%%)\n",
                subObjects, allBlack, 100.0 * static_cast<double>(allBlack) / static_cast<double>(subObjects),
                allWhite, 100.0 * static_cast<double>(allWhite) / static_cast<double>(subObjects),
                mixed, 100.0 * static_cast<double>(mixed) / static_cast<double>(subObjects));

    // The claim, in the form that fails if the field is ever a tint after all:
    // most meshes are at zero. Measured 81.2% over 300 files.
    CHECK(allBlack * 2 > subObjects);
    // And the rest are overwhelmingly at the other extreme rather than spread —
    // two families, one of which computes its light elsewhere. Measured 18.2%
    // white against 0.6% graded.
    CHECK(allWhite > mixed * 4);
    // Between them they are essentially the whole corpus, which is what makes
    // "the attribute is a per-family light term" a complete account of it.
    CHECK((allBlack + allWhite) * 20 > subObjects * 19);
}

// ============================================================================
// The RenderPass's stage list decides which of a material's entries are live,
// and its effect file decides what the vertex colour means. Both are read off
// the `.shd` corpus, which is name-keyed and so needs no install.
// ============================================================================

TEST_CASE("D3 corpus: a pass declares the types it binds", "[d3][corpus]") {
    const auto files = FindFiles(CorpusRoot() / "Shaders", ".shd");
    if (files.empty()) {
        WARN("No D3 Shaders corpus at " << (CorpusRoot() / "Shaders").string()
                                        << ". SKIPPED, not passed.");
        return;
    }
    std::map<i32, std::size_t> declaredBy;
    std::map<std::string, std::size_t> effects;
    std::size_t passes = 0, maxStages = 0;
    std::size_t maskPasses = 0, maskWithBase = 0;
    for (const auto& p : files) {
        auto sh = d3n::parseShaders(ReadAll(p));
        if (!sh)
            continue;
        for (const auto& rp : sh->arRenderPasses) {
            ++passes;
            ++effects[rp.szEffectFile];
            maxStages = (std::max)(maxStages, rp.arTextureStages.size());
            bool base = false, mask = false;
            for (const auto& ts : rp.arTextureStages) {
                ++declaredBy[ts.dwUnknown00];
                if (ts.dwUnknown00 == 1)
                    base = true;
                if (ts.dwUnknown00 == 12 || ts.dwUnknown00 == 14 || ts.dwUnknown00 == 19)
                    mask = true;
            }
            if (mask) {
                ++maskPasses;
                if (base)
                    ++maskWithBase;
            }
        }
    }
    REQUIRE(passes > 0);

    std::size_t detailBlock = 0;
    for (i32 t = 26; t <= 38; ++t)
        detailBlock += declaredBy[t];
    std::printf("[d3-stage] %zu passes, max %zu stages; type 1 declared by %zu, type 2 by %zu, "
                "type 5 by %zu, type 6 by %zu; types 26..38 by %zu\n",
                passes, maxStages, declaredBy[1], declaredBy[2], declaredBy[5], declaredBy[6],
                detailBlock);
    std::printf("[d3-stage] effect files:");
    for (const auto& [name, n] : effects) {
        if (n * 40 > passes)
            std::printf(" %s x%zu", name.c_str(), n);
    }
    std::printf("\n[d3-stage] passes binding an alpha mask: %zu, of which %zu also bind a type-1 "
                "base map\n", maskPasses, maskWithBase);

    // THE 26..38 BLOCK IS DEAD DATA. It is byte-identical across every material
    // in a file and had no recovered meaning; this is what it turns out to be —
    // not one of the 1,831 shipped passes asks for any of it. So the surface
    // table's stage filter drops it by construction rather than by a rule about
    // those particular numbers.
    CHECK(detailBlock == 0);
    // Type 1 is the base map, which is why it is the type a pass asks for most.
    CHECK(declaredBy[1] > declaredBy[2] * 2);
    // A stage list is short: it fits a u64 type bitmask with room to spare, and
    // the bitmask is only valid while every type stays under 62.
    CHECK(maxStages <= 32);
    // The families the surface table switches on are all present and none is a
    // rounding error.
    CHECK(effects.count("Scene.fx") == 1);
    CHECK(effects.count("Prop.fx") == 1);
    CHECK(effects.count("ActorIrrad.fx") == 1);
    CHECK(effects.count("Legacy.fx") == 1);
    // An alpha mask nearly always sits beside a base map — which is what makes
    // "no type 1 in this pass, so this id is the base map here" a safe reading
    // of the minority. Measured 264 of 305.
    CHECK(maskWithBase * 4 > maskPasses * 3);
}

// ============================================================================
// The types are NAMED by the shipped programs, not guessed.
//
// EMaterialTextureType's authored names are gone from the build, and the
// core-asset fallback trick only names the handful of types that have one. What
// names the rest is that the game ships the OpenGL build of its own shaders:
// `OpenGLShaders/<program>_<hash>.ps.glsl` is an ARB assembly carrying its
// sampler names in a string table, in texture-unit order, and a `RenderPass`
// carries an ORDERED list of the stage types it binds. Zip the two and each
// type takes its name from the original.
//
// The alignment is anchored on the types the fallback had already named, so a
// permutation that does not belong to this pass is rejected rather than
// shifting every name along by one. What the anchor cannot vouch for is not
// reported.
// ============================================================================

TEST_CASE("D3 corpus: the shipped programs name the texture types", "[d3][corpus]") {
    const auto shaders = FindFiles(CorpusRoot() / "Shaders", ".shd");
    const fs::path glDir = CorpusRoot() / "OpenGLShaders";
    std::error_code ec;
    if (shaders.empty() || !fs::is_directory(glDir, ec)) {
        WARN("No D3 Shaders/OpenGLShaders corpus under " << CorpusRoot().string()
                                                         << ". SKIPPED, not passed.");
        return;
    }

    // Every `.ps.glsl` grouped by the program name it belongs to. One name owns
    // many files: a permutation per light count, per pass, per feature toggle.
    std::map<std::string, std::vector<fs::path>> byProgram;
    for (fs::directory_iterator it(glDir, ec), end; it != end; it.increment(ec)) {
        if (ec)
            break;
        const std::string f = it->path().filename().string();
        constexpr std::string_view kSuffix = ".ps.glsl";
        if (f.size() < kSuffix.size() + 9 ||
            f.compare(f.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
            continue;
        const std::size_t stem = f.size() - kSuffix.size();
        if (stem < 9 || f[stem - 9] != '_')
            continue;
        byProgram[f.substr(0, stem - 9)].push_back(it->path());
    }
    REQUIRE(!byProgram.empty());

    // The sampler names an ARB assembly declares, in declaration order — which
    // is the order the binding table beside them indexes by unit.
    auto samplerNames = [](const fs::path& p) {
        std::vector<std::string> out;
        const auto bytes = ReadAll(p);
        std::string cur;
        for (const u8 b : bytes) {
            if (b >= 0x20 && b < 0x7F) {
                cur.push_back(static_cast<char>(b));
                continue;
            }
            if (b == 0 && cur.size() > 7 && cur.compare(cur.size() - 7, 7, "Sampler") == 0 &&
                std::find(out.begin(), out.end(), cur) == out.end())
                out.push_back(cur);
            cur.clear();
        }
        return out;
    };

    // The types the core-asset fallback had already named. An alignment that
    // disagrees with any of them is the wrong permutation, not a discovery.
    const std::map<i32, std::string> kAnchor = {
        {1, "diffuseSampler"},   {2, "lightMapSampler"},   {4, "environmentMapSampler"},
        {5, "glossMapSampler"},  {22, "shadowMapSampler"}, {23, "vignetteSampler"},
        {53, "fogSampler"},      {61, "ssaoSampler"},
    };

    std::map<i32, std::map<std::string, std::size_t>> named;
    std::map<std::string, std::vector<std::string>> cache;
    std::size_t alignments = 0;
    for (const auto& p : shaders) {
        auto sh = d3n::parseShaders(ReadAll(p));
        if (!sh)
            continue;
        const auto prog = byProgram.find(p.stem().string());
        if (prog == byProgram.end())
            continue;
        for (const auto& rp : sh->arRenderPasses) {
            std::vector<i32> types;
            for (const auto& ts : rp.arTextureStages) {
                if (ts.dwUnknown00 != 0)
                    types.push_back(ts.dwUnknown00);
            }
            if (types.empty())
                continue;
            for (const auto& gl : prog->second) {
                const std::string key = gl.string();
                auto c = cache.find(key);
                if (c == cache.end())
                    c = cache.emplace(key, samplerNames(gl)).first;
                const auto& names = c->second;
                if (names.size() != types.size())
                    continue;
                bool anchored = false, agrees = true;
                for (std::size_t k = 0; k < types.size(); ++k) {
                    const auto a = kAnchor.find(types[k]);
                    if (a == kAnchor.end())
                        continue;
                    anchored = true;
                    if (a->second != names[k])
                        agrees = false;
                }
                if (!anchored || !agrees)
                    continue;
                ++alignments;
                for (std::size_t k = 0; k < types.size(); ++k)
                    ++named[types[k]][names[k]];
                break;
            }
        }
    }
    if (alignments == 0) {
        WARN("No pass aligned with a shipped program. SKIPPED, not passed.");
        return;
    }

    auto total = [&](i32 type) {
        std::size_t n = 0;
        const auto it = named.find(type);
        if (it != named.end()) {
            for (const auto& entry : it->second)
                n += entry.second;
        }
        return n;
    };
    auto top = [&](i32 type) {
        std::pair<std::string, std::size_t> best{"", 0};
        const auto it = named.find(type);
        if (it != named.end()) {
            for (const auto& entry : it->second) {
                if (entry.second > best.second)
                    best = entry;
            }
        }
        return best;
    };

    std::printf("[d3-name] %zu anchored alignments\n", alignments);
    for (const auto& entry : named) {
        const auto best = top(entry.first);
        const auto n = total(entry.first);
        std::printf("[d3-name]   type %-3d n=%-5zu %-5.1f%% %s\n", entry.first, n,
                    100.0 * static_cast<double>(best.second) / static_cast<double>(n),
                    best.first.c_str());
    }

    // The two the slot map already had by other means, re-derived here from the
    // programs. If these disagree the alignment is broken and nothing under it
    // can be believed either.
    CHECK(top(1).first == "diffuseSampler");
    CHECK(top(2).first == "lightMapSampler");
    // The two this work added to the slot map. Type 6 is rare because a glow
    // map is rare, not because it is uncertain.
    CHECK(top(5).first == "glossMapSampler");
    CHECK(total(5) >= 40);
    CHECK(top(6).first == "glowSampler");
    // And the alpha masks, which is where a transparent D3 surface gets its
    // shape. The alphaMapN *index* shifts by family — the same id is mask 0 in
    // one program and mask 1 in another — so the assertion is on the prefix.
    // What has to hold is that all three are masks and none is a base map.
    for (const i32 type : {12, 14, 19}) {
        const auto best = top(type);
        INFO("type " << type << " -> " << best.first);
        CHECK(best.first.rfind("alphaMap", 0) == 0);
    }
}

// ============================================================================
// The render state lives on the Shaders asset, not on the material — and it is
// only reachable through an install, because a ShaderMap is named by SNO id and
// an id resolves through an opened storage.
//
// So this is the one half of the material system the corpus arm structurally
// cannot see: an extracted tree has the `.shm` and `.shd` files but no CoreTOC
// to find them by id. Gated here rather than left to the render arm for that
// reason.
// ============================================================================

TEST_CASE("D3 install: render state comes from the ShaderMap's RenderPass",
          "[d3][material][install]") {
    using ::whiteout::flakes::ProductId;

    flakes::io::FileContentProvider provider;
    if (const char* root = std::getenv("WDX_TEST_D3_INSTALL"); root && *root)
        provider.SetInstallPath(root);
    provider.SetGame(ProductId::D3);
    if (provider.GamePath(ProductId::D3).empty()) {
        WARN("No Diablo III install (set WDX_TEST_D3_INSTALL). SKIPPED, not passed.");
        return;
    }
    flakes::io::D3SnoCache cache(&provider);

    struct Want {
        const char* file;
        const char* subObject;
        bool blends;
        bool depthWrite;
    };
    // Imperius's wings and Malthael's outer wing are the two the report was
    // about: both blend and neither writes depth. The bodies beside them are
    // the other half of the discriminator — they blend TOO (1,412 of the
    // corpus's 1,831 passes do), and bucketing on the blend enable alone would
    // sweep every D3 character into the sorted transparent list.
    const Want kWant[] = {
        {"Imperius", "wing_mat", true, false},
        {"Imperius", "A_normal_mat", true, true},
        {"x1_Malthael", "wingOuter_mat", true, false},
        {"x1_Malthael", "A_normal_mat", true, true},
        {"Tyrael", "A_normal_mat", true, true},
    };

    std::size_t resolved = 0;
    for (const auto& w : kWant) {
        const auto path = CorpusRoot() / "Appearances" / (std::string(w.file) + ".app");
        auto app = d3n::parseAppearances(ReadAll(path));
        if (!app) {
            WARN("missing " << path.string() << " -- SKIPPED, not passed.");
            continue;
        }
        const d3n::GeoSet* sets[2] = {&app->tGeoSet0, &app->tGeoSet1};
        const d3n::SubObject* sub = nullptr;
        for (const auto* set : sets) {
            for (const auto& s : set->arSubObjects) {
                if (EqualCiSv(s.szName, w.subObject))
                    sub = &s;
            }
        }
        REQUIRE(sub != nullptr);
        const auto* v = flakes::io::D3VariantFor(*app, *sub, 0);
        REQUIRE(v != nullptr);

        const auto st = d3p::D3PassStateFor(*v, &cache);
        INFO(w.file << " / " << w.subObject << " shm=" << v->tMaterial.snoShaderMap.id);
        if (!st.resolved) {
            WARN(w.file << " / " << w.subObject
                        << ": ShaderMap did not resolve through this install -- SKIPPED.");
            continue;
        }
        ++resolved;
        std::printf("[d3-pass] %-12s %-18s blend=%d(%u,%u) depthW=%d cull=%u alphaRef=%u\n", w.file,
                    w.subObject, static_cast<int>(st.blendEnable), st.blendSrc, st.blendDst,
                    static_cast<int>(st.depthWrite), st.cull, static_cast<unsigned>(st.alphaRef));
        CHECK(st.blendEnable == w.blends);
        CHECK(st.depthWrite == w.depthWrite);
        // D3DBLEND, and the corpus never leaves the enum.
        CHECK(st.blendSrc >= 1u);
        CHECK(st.blendSrc <= 11u);
        CHECK(st.blendDst >= 1u);
        CHECK(st.blendDst <= 11u);
        // D3DCULL: 1 none, 2 CW, 3 CCW.
        CHECK(st.cull >= 1u);
        CHECK(st.cull <= 3u);
    }
    if (resolved == 0) {
        WARN("No ShaderMap resolved. SKIPPED, not passed.");
        return;
    }
    CHECK(resolved == std::size(kWant));
}

// ============================================================================
// One doodad, end to end, against a real install: the model the black-geometry
// report came from.
//
// `a1_Id_All_Book_Of_Cain` is worth pinning because it carries both families at
// once — Scene.fx props whose vertex colour is zero beside a Legacy.fx smoke
// plume whose opacity lives entirely in two alpha masks — and because both of
// its symptoms were invisible to every other gate: the props drew, they were
// just black, and the smoke drew, it was just opaque.
// ============================================================================

TEST_CASE("D3 install: the doodad that rendered black", "[d3][material][install]") {
    using ::whiteout::flakes::ProductId;

    flakes::io::FileContentProvider provider;
    if (const char* root = std::getenv("WDX_TEST_D3_INSTALL"); root && *root)
        provider.SetInstallPath(root);
    provider.SetGame(ProductId::D3);
    if (provider.GamePath(ProductId::D3).empty()) {
        WARN("No Diablo III install (set WDX_TEST_D3_INSTALL). SKIPPED, not passed.");
        return;
    }
    flakes::io::D3SnoCache cache(&provider);

    const auto path = CorpusRoot() / "Appearances" / "a1_Id_All_Book_Of_Cain.app";
    auto app = d3n::parseAppearances(ReadAll(path));
    if (!app) {
        WARN("missing " << path.string() << " -- SKIPPED, not passed.");
        return;
    }

    struct Want {
        const char* subObject;
        bool vertexColorLights; ///< Scene.fx / Prop.fx: the attribute is light.
        bool alphaMasks;        ///< opacity comes from 12/14/19, not the base map.
    };
    // The satchel and the room decoration are the black ones: Scene.fx, vertex
    // colour (0,0,0). SMOKE is the one that drew solid: Legacy.fx, base map for
    // colour and two masks for shape.
    const Want kWant[] = {
        {"CainsSatchel", true, false},
        {"town_interior_deco_A1", true, false},
        {"SMOKE", false, true},
    };

    std::size_t checked = 0;
    for (const auto& w : kWant) {
        const d3n::GeoSet* sets[2] = {&app->tGeoSet0, &app->tGeoSet1};
        const d3n::SubObject* sub = nullptr;
        for (const auto* set : sets) {
            for (const auto& s : set->arSubObjects) {
                if (EqualCiSv(s.szName, w.subObject))
                    sub = &s;
            }
        }
        REQUIRE(sub != nullptr);
        const auto* v = flakes::io::D3VariantFor(*app, *sub, 0);
        REQUIRE(v != nullptr);
        const auto st = d3p::D3PassStateFor(*v, &cache);
        INFO(w.subObject << " shm=" << v->tMaterial.snoShaderMap.id);
        if (!st.resolved) {
            WARN(w.subObject << ": ShaderMap did not resolve through this install -- SKIPPED.");
            continue;
        }
        ++checked;

        // What the mesh actually carries in the attribute, beside what the pass
        // says to do with it — the two halves of the black-geometry bug in one
        // line.
        bool blackVertexColor = !sub->arVertices.empty();
        for (const auto& vert : sub->arVertices) {
            const auto c = d3n::vertexColor(vert);
            if (c.r != 0 || c.g != 0 || c.b != 0)
                blackVertexColor = false;
        }
        u64 maskTypes = 0;
        for (const auto& e : v->tMaterial.arTextures) {
            const i32 t = flakes::io::D3TextureTypeOf(e);
            if (flakes::io::D3SlotIsAlphaMask(flakes::io::D3SlotOfType(t)) &&
                (st.declaredTypes & flakes::io::D3TypeBit(t)) != 0)
                maskTypes |= flakes::io::D3TypeBit(t);
        }
        std::printf("[d3-cain] %-22s vcLights=%d vcAlpha=%d blackVcol=%d masks=%d blend=%d dw=%d\n",
                    w.subObject, static_cast<int>(st.vertexColorLights),
                    static_cast<int>(st.vertexAlpha), static_cast<int>(blackVertexColor),
                    static_cast<int>(std::popcount(maskTypes)),
                    static_cast<int>(st.blendEnable), static_cast<int>(st.depthWrite));

        CHECK(st.vertexColorLights == w.vertexColorLights);
        CHECK((maskTypes != 0) == w.alphaMasks);
        // A pass always declares SOMETHING, which is what makes the stage filter
        // safe to apply whenever it resolved.
        CHECK(st.declaredTypes != 0);
        if (w.vertexColorLights) {
            // The half that made them black: a static-family mesh outside a
            // level bake carries no vertex colour at all, so multiplying it into
            // the albedo leaves nothing to draw.
            CHECK(blackVertexColor);
        }
        if (w.alphaMasks) {
            // The half that made the smoke solid: the pass blends and does not
            // write depth, so the state was already right — the alpha was not.
            CHECK(st.blendEnable);
            CHECK(!st.depthWrite);
            // Two masks, and the base map's own alpha is never read by
            // `actor_complex_Transparent_Ground`.
            CHECK(std::popcount(maskTypes) >= 2);
        }
    }
    if (checked == 0) {
        WARN("Nothing resolved. SKIPPED, not passed.");
        return;
    }
    CHECK(checked == std::size(kWant));
}
