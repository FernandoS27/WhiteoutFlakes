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
#include "renderer/profiles/diablo3/d3_standard_shading.h"
#include "renderer/profiles/diablo3/d3_surface_table.h"

#include <whiteout/sno/d3/native/d3_native.h>
#include <whiteout/sno/core_toc.h>
#include <whiteout/sno/d3/native/geometry.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
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
                ++declaredBy[ts.dwTextureType];
                if (ts.dwTextureType == 1)
                    base = true;
                if (ts.dwTextureType == 12 || ts.dwTextureType == 14 || ts.dwTextureType == 19)
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
                if (ts.dwTextureType != 0)
                    types.push_back(ts.dwTextureType);
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
        // The ENGINE's blend enum, not D3DBLEND, and the corpus never leaves it.
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

// ============================================================================
// The lighting switch, and the surface it was found on.
//
// `Render_EnsureShaderVariant` compiles one GPU program per combination of five
// clamped light counts, each read from the RenderPass's own tag map. One tag
// above those turns the light block off entirely, and this is the measurement
// that identifies it: pair every corpus `.shd` with the ARB vertex programs
// shipped beside it under `OpenGLShaders/` and ask whether the program contains
// a light block at all.
//
// The signature is unmistakable — `MAD Rd.xyz, -Rs, c[N].w, c[N]` is
// `L = lightPos.xyz - P * lightPos.w`, the one instruction that makes a point
// light and a directional share an array — and it is the only place a constant
// is used both as a vector and as its own `.w` in the same instruction.
// ============================================================================

TEST_CASE("D3 corpus: a pass says whether it takes light at all", "[d3][corpus]") {
    const auto shd = CorpusRoot() / "Shaders";
    const auto gl = CorpusRoot() / "OpenGLShaders";
    if (!fs::is_directory(shd) || !fs::is_directory(gl)) {
        WARN("No D3 Shaders/OpenGLShaders corpus. SKIPPED, not passed.");
        return;
    }

    // Asset name -> the ARB vertex programs compiled for it. One name can carry
    // several, one per shader variant.
    std::map<std::string, std::vector<fs::path>> vsByName;
    for (const auto& de : fs::directory_iterator(gl)) {
        const auto file = de.path().filename().string();
        const auto ext = file.find(".vs.glsl");
        // "<name>_<8 hex>.vs.glsl"
        if (ext == std::string::npos || ext < 9)
            continue;
        vsByName[file.substr(0, ext - 9)].push_back(de.path());
    }

    // `MAD R0.xyz, -R1, c[25].w, c[25];` -- the same constant twice, once whole
    // and once as its own `.w`. Nothing else in these programs does that.
    auto hasLightBlock = [](const fs::path& p) {
        const auto bytes = ReadAll(p);
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        for (std::size_t i = 0; (i = text.find("MAD R", i)) != std::string::npos; ++i) {
            const auto eol = text.find('\n', i);
            const auto line = text.substr(i, (eol == std::string::npos ? text.size() : eol) - i);
            const auto w = line.find("].w, c[");
            if (w == std::string::npos || line.find(".xyz, -R") == std::string::npos)
                continue;
            const auto open = line.rfind("c[", w);
            if (open == std::string::npos)
                continue;
            const auto close = line.find(']', w + 7);
            if (close == std::string::npos)
                continue;
            if (line.substr(open + 2, w - open - 2) == line.substr(w + 7, close - w - 7))
                return true;
        }
        return false;
    };

    constexpr u32 kTag = 0xA000Fu;
    std::size_t off = 0, offUnlit = 0, on = 0, onLit = 0, absent = 0, absentLit = 0;
    std::map<u32, std::size_t> values;
    for (const auto& de : fs::directory_iterator(shd)) {
        if (de.path().extension() != ".shd")
            continue;
        auto sh = d3n::parseShaders(ReadAll(de.path()));
        if (!sh)
            continue;
        const auto it = vsByName.find(de.path().stem().string());
        if (it == vsByName.end())
            continue;
        // Only an asset whose variants agree can settle anything.
        const bool lit = hasLightBlock(it->second.front());
        bool agree = true;
        for (const auto& v : it->second)
            agree = agree && (hasLightBlock(v) == lit);
        if (!agree)
            continue;

        for (const auto& p : sh->arRenderPasses) {
            const d3n::ShaderTagMapEntry* tag = nullptr;
            for (const auto& t : p.arShaderParams)
                if (t.dwTagId == kTag)
                    tag = &t;
            if (!tag) {
                ++absent;
                absentLit += lit ? 1 : 0;
                continue;
            }
            ++values[tag->dwValue];
            if (tag->dwValue == 0) {
                ++off;
                offUnlit += lit ? 0 : 1;
            } else {
                ++on;
                onLit += lit ? 1 : 0;
            }
        }
    }
    std::printf("[d3-lit] tag 0x%X: value 0 on %zu passes (%zu with no light block), nonzero on "
                "%zu (%zu with one), absent on %zu (%zu with one)\n",
                kTag, off, offUnlit, on, onLit, absent, absentLit);
    for (const auto& [v, n] : values)
        std::printf("[d3-lit]   value %u: %zu\n", v, n);

    REQUIRE(off > 100);
    REQUIRE(on > 20);
    // The claim: the tag decides it, with no residue. Measured 666/666 and
    // 114/114 over the corpus.
    CHECK(offUnlit == off);
    CHECK(onLit == on);
    // And the global default behind an absent tag is ON, by a wide margin.
    CHECK(absentLit * 10 > absent * 8);
    // Only ever a flag.
    for (const auto& [v, n] : values) {
        INFO("tag value " << v << " on " << n << " passes");
        CHECK(v <= 1u);
    }
}

// ============================================================================
// Imperius's wings, which is where both halves of this landed.
//
// The report was that they occlude each other, and asked whether depth write
// was not disabled for them. It is, and this pins that end to end — because the
// answer is that a near-black surface at high alpha is indistinguishable from
// an occluder, and the blackness was the lighting rather than the depth state.
// ============================================================================

TEST_CASE("D3 install: the wings are unlit, blended and depth-write-off",
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
        const char* sub;
        bool lit;       ///< tag 0xA000F
        bool blends;
        bool depthWrite;
        bool whiteVcol; ///< an unlit surface with no vertex colour draws black.
        /// The fixed-function chain, decoded: which types feed the colour,
        /// which feed the alpha, and the two output gains. Zero and 0.0f mean
        /// "this pass carries no stage block", which every non-Legacy entry
        /// here does.
        u64 colorTypes;
        u64 alphaTypes;
        f32 colorGain;
        f32 alphaGain;
    };
    // One bit per EMaterialTextureType: 1 base map, 6 glow, 12/14/16 masks.
    constexpr u64 kT1 = 1ull << 1, kT6 = 1ull << 6, kT12 = 1ull << 12, kT14 = 1ull << 14,
                  kT16 = 1ull << 16;
    const Want kWant[] = {
        // Imperius. Stages (6, 1, 12, 14), and both halves of
        // `actor_glowTendril_cm2x_bloom_skin` verbatim:
        //     colour = 2 * vcol * glow.rgb * diffuse.rgb * mask14.rgb
        //     alpha  = 4 * vcol.a * diffuse.a * mask12.a * mask14.a
        {"Imperius", "wing_mat", false, true, false, true, kT1 | kT6 | kT14, kT1 | kT12 | kT14,
         2.0f, 4.0f},
        // Malthael takes no glow into the colour and stacks his alpha gain to
        // 32 across four stages -- the same grammar, a different chain. The
        // shipped `ps_legacy_Malthael_wings_flow` spells all of it out:
        //
        //     dp2_sat r0.z, r1.wwww, v1.wwww   ; alpha code 26: x2 AND saturate
        //     mul r0.x, r0.z, r1.w             ; x mask12.a
        //     mul r0.x, r0.x, l(4.0)           ; code 25
        //     mul r0.x, r0.x, r2.w             ; x mask14.a
        //     mul r0.x, r0.x, l(4.0)           ; code 25
        //     mul r0.x, r0.x, r1.w             ; x mask16.a
        //
        // -- 2 x 4 x 4 = 32. Reading the units digit as a gain alone made code
        // 26 a x1 and this 16.
        {"x1_Malthael", "wingOuter_mat", false, true, false, true, kT12 | kT14,
         kT1 | kT12 | kT14 | kT16, 1.0f, 32.0f},
        // Cain's smoke plume is the OTHER Legacy program, and the one that made
        // the glow map look ruleless: its colour chain is three replaces and an
        // add (`saturate(diffuse + glow)`), so nothing modulates the colour and
        // the glow keeps the additive path. Its alpha is the two masks at x2.
        {"a1_Id_All_Book_Of_Cain", "SMOKE", false, true, false, true, 0, kT12 | kT14, 1.0f, 2.0f},
        // The body beside them: lit, and it writes depth. Both halves matter --
        // treating every blended surface as sorted would sweep in every D3
        // character, and treating every Legacy surface as unlit would flatten
        // the ones that do take light. ActorIrrad compiles its own program and
        // carries no stage block at all, which is what the zeroes say.
        {"Imperius", "A_normal_mat", true, true, true, true, 0, 0, 1.0f, 1.0f},
    };

    std::size_t checked = 0;
    for (const auto& w : kWant) {
        auto app = d3n::parseAppearances(
            ReadAll(CorpusRoot() / "Appearances" / (std::string(w.file) + ".app")));
        if (!app) {
            WARN("missing " << w.file << " -- SKIPPED, not passed.");
            continue;
        }
        const d3n::GeoSet* sets[2] = {&app->tGeoSet0, &app->tGeoSet1};
        const d3n::SubObject* sub = nullptr;
        for (const auto* set : sets)
            for (const auto& s : set->arSubObjects)
                if (EqualCiSv(s.szName, w.sub))
                    sub = &s;
        REQUIRE(sub != nullptr);
        const auto* v = flakes::io::D3VariantFor(*app, *sub, 0);
        REQUIRE(v != nullptr);
        const auto st = d3p::D3PassStateFor(*v, &cache);
        if (!st.resolved) {
            WARN(w.file << " / " << w.sub << ": ShaderMap did not resolve -- SKIPPED.");
            continue;
        }
        ++checked;

        std::size_t white = 0;
        for (const auto& vx : sub->arVertices) {
            const auto c = d3n::vertexColor(vx);
            white += (c.r == 255 && c.g == 255 && c.b == 255) ? 1 : 0;
        }
        std::printf("[d3-wing] %-24s %-16s lit=%d fx=%-14s blend=%d dW=%d cull=%u white=%zu/%zu"
                    " rgb=0x%llx a=0x%llx gain=(%.0f, %.0f)\n",
                    w.file, w.sub, st.lit, st.effectFile.c_str(), st.blendEnable, st.depthWrite,
                    st.cull, white, sub->arVertices.size(),
                    static_cast<unsigned long long>(st.colorTypes),
                    static_cast<unsigned long long>(st.alphaTypes), st.colorGain, st.alphaGain);

        CHECK(st.lit == w.lit);
        CHECK(st.blendEnable == w.blends);
        CHECK(st.depthWrite == w.depthWrite);
        if (w.whiteVcol)
            CHECK(white == sub->arVertices.size());
        // The stage block, decoded. Imperius's wings are the whole grammar in
        // one pass: stages (6, 1, 12, 14), colour from the glow, the base map
        // and mask 14 at x2, alpha from the base map and both masks at x4 —
        // which is what `actor_glowTendril_cm2x_bloom_skin` closes on, and what
        // makes the tendrils a sheet reaching the armour instead of a few
        // separated strands floating beside it.
        CHECK(st.stageArgs == (w.colorTypes != 0 || w.alphaTypes != 0));
        CHECK(st.colorTypes == w.colorTypes);
        CHECK(st.alphaTypes == w.alphaTypes);
        if (st.stageArgs) {
            CHECK(st.colorGain == w.colorGain);
            CHECK(st.alphaGain == w.alphaGain);
        }

        // And the classification the sorted-transparent bucket is chosen by: a
        // blended pass that does not write depth is the one that cannot be
        // depth-resolved, so it is the one that gets sorted.
        d3p::D3Surface surf;
        surf.valid = true;
        surf.pass = st;
        surf.alphaBlend = st.blendEnable;
        const auto sc = d3p::D3ClassifySurface(surf);
        CHECK(sc.visible);
        CHECK((sc.blend == whiteout::flakes::renderer::core::BlendClass::Transparent) ==
              (w.blends && !w.depthWrite));
    }
    CHECK(checked >= 3);
}

// ============================================================================
// The fixed-function stage block, against the programs it was compiled into.
//
// `Legacy.fx` is the engine's fixed-function path: its passes carry three
// groups of six tags — an op at 0xA0010+i and two combine codes at 0xA0016+i
// (colour) and 0xA001C+i (alpha) — and the shipped ARB programs are what those
// codes were compiled into. Pairing the two is the only way to read them, and
// it settles two things this shading model needs:
//
//   * WHICH CHANNEL each stage's texture feeds. `TEX R0.w` samples alpha only,
//     `TEX R0.xyz` colour only and `TEX R0` both, so the destination swizzle is
//     the answer written down.
//   * THE OUTPUT GAIN. A units digit of 4 is a MODULATE2X and of 5 a
//     MODULATE4X, and they multiply — which the program folds into the constant
//     its last instruction multiplies in. Imperius's wings are (2, 4), and the
//     shader asset is called `actor_glowTendril_cm2x_bloom_skin`.
//
// Both are majority rules and reported as such: a pass is matched to a program
// only where the match is unambiguous (one pass with that stage count, one
// program with that sampler count, and every variant of it agreeing), which is
// what makes a percentage mean anything here.
// ============================================================================

namespace {

// The first ARB program in a `.ps.glsl` blob. The file repeats it once per
// shader variant and they are byte-identical; the first is the whole content.
std::string ArbProgram(const fs::path& p) {
    const auto bytes = ReadAll(p);
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto b = text.find("!!ARBfp1.0");
    if (b == std::string::npos)
        return {};
    const auto e = text.find("END", b);
    return text.substr(b, (e == std::string::npos ? text.size() : e + 3) - b);
}

// `texture[N]` -> {reads rgb, reads alpha}, from the TEX destination swizzles.
struct ArbSampler {
    bool rgb = false;
    bool alpha = false;
};

std::map<int, ArbSampler> ArbSamplers(const std::string& prog) {
    std::map<int, ArbSampler> out;
    for (std::size_t i = 0; (i = prog.find("TEX ", i)) != std::string::npos; ++i) {
        const auto eol = prog.find('\n', i);
        const auto line = prog.substr(i, (eol == std::string::npos ? prog.size() : eol) - i);
        const auto t = line.find("texture[");
        if (t == std::string::npos)
            continue;
        const int unit = std::atoi(line.c_str() + t + 8);
        // "TEX R0.xyz, ..." — the swizzle between the destination and the comma.
        const auto comma = line.find(',');
        const auto dot = line.find('.', 4);
        std::string swz = "xyzw";
        if (dot != std::string::npos && comma != std::string::npos && dot < comma)
            swz = line.substr(dot + 1, comma - dot - 1);
        auto& s = out[unit];
        s.rgb = s.rgb || swz.find_first_of("xyz") != std::string::npos;
        s.alpha = s.alpha || swz.find('w') != std::string::npos;
    }
    return out;
}

int ArbSamplerCount(const std::string& prog) {
    int n = 0;
    for (std::size_t i = 0; (i = prog.find("texture[", i)) != std::string::npos; ++i)
        n = (std::max)(n, std::atoi(prog.c_str() + i + 8) + 1);
    return n;
}

// The constant the program's final write to `result.color.<mask>` multiplies
// in — the folded output gain. Returns -1 where the program does not end in a
// `MUL ... c[N]` (a MAD, a fog lerp, an env constant), because those carry the
// gain somewhere this cannot read and guessing would pollute the measurement.
f32 ArbOutputGain(const std::string& prog, bool alpha) {
    // PARAM c[K] = { {a, b, ...}, ... }; — literal blocks only. A block naming
    // program.env holds a uniform, and its value is not in the file.
    std::map<int, std::vector<f32>> params;
    const auto pb = prog.find("PARAM c[");
    if (pb != std::string::npos && prog.find("program.env", pb) == std::string::npos) {
        const auto end = prog.find("};", pb);
        std::size_t at = prog.find('{', pb);
        int idx = 0;
        while (at != std::string::npos && at < end) {
            const auto open = prog.find('{', at + 1);
            if (open == std::string::npos || open > end)
                break;
            const auto close = prog.find('}', open);
            std::vector<f32> vals;
            std::string cur;
            for (std::size_t i = open + 1; i < close; ++i) {
                if (prog[i] == ',') {
                    vals.push_back(static_cast<f32>(std::atof(cur.c_str())));
                    cur.clear();
                } else {
                    cur += prog[i];
                }
            }
            if (!cur.empty())
                vals.push_back(static_cast<f32>(std::atof(cur.c_str())));
            params[idx++] = vals;
            at = close;
        }
    }

    const std::string want = alpha ? "result.color.w" : "result.color.xyz";
    std::string last;
    for (std::size_t i = 0; (i = prog.find(want, i)) != std::string::npos; ++i) {
        auto bol = prog.rfind('\n', i);
        bol = (bol == std::string::npos) ? 0 : bol + 1;
        const auto eol = prog.find('\n', i);
        last = prog.substr(bol, (eol == std::string::npos ? prog.size() : eol) - bol);
    }
    if (last.empty()) {
        // `MOV result.color, ...` writes all four channels at unit gain.
        if (prog.find("MOV result.color,") != std::string::npos)
            return 1.0f;
        return -1.0f;
    }
    while (!last.empty() && (last.front() == ' ' || last.front() == '\t'))
        last.erase(last.begin());
    if (last.rfind("MOV", 0) == 0)
        return 1.0f;
    if (last.rfind("MUL", 0) != 0)
        return -1.0f;
    const auto c = last.find("c[");
    if (c == std::string::npos)
        return 1.0f; // a MUL of two varying terms — no constant gain
    const int idx = std::atoi(last.c_str() + c + 2);
    const auto close = last.find(']', c);
    int comp = 0;
    if (close != std::string::npos && close + 1 < last.size() && last[close + 1] == '.') {
        const char ch = last[close + 2];
        comp = ch == 'y' ? 1 : ch == 'z' ? 2 : ch == 'w' ? 3 : 0;
    }
    const auto it = params.find(idx);
    if (it == params.end() || comp >= static_cast<int>(it->second.size()))
        return -1.0f;
    return it->second[static_cast<std::size_t>(comp)];
}

} // namespace

TEST_CASE("D3 corpus: a Legacy pass says which channel each stage feeds", "[d3][corpus]") {
    const auto shdDir = CorpusRoot() / "Shaders";
    const auto glDir = CorpusRoot() / "OpenGLShaders";
    if (!fs::is_directory(shdDir) || !fs::is_directory(glDir)) {
        WARN("No D3 Shaders/OpenGLShaders corpus. SKIPPED, not passed.");
        return;
    }

    std::map<std::string, std::vector<fs::path>> psByName;
    for (const auto& de : fs::directory_iterator(glDir)) {
        const auto file = de.path().filename().string();
        const auto ext = file.find(".ps.glsl");
        if (ext == std::string::npos || ext < 9)
            continue;
        psByName[file.substr(0, ext - 9)].push_back(de.path());
    }

    // ---- The block is the fixed-function family's, and no other's ----------
    std::map<std::string, std::pair<std::size_t, std::size_t>> byFx; // fx -> {with, total}
    std::size_t chanOk = 0, chanBad = 0, gainOkC = 0, gainBadC = 0, gainOkA = 0, gainBadA = 0;

    for (const auto& de : fs::directory_iterator(shdDir)) {
        if (de.path().extension() != ".shd")
            continue;
        auto sh = d3n::parseShaders(ReadAll(de.path()));
        if (!sh)
            continue;
        for (const auto& p : sh->arRenderPasses) {
            bool has = false;
            for (const auto& t : p.arShaderParams) {
                if (t.dwTagId >= 0xA0010u && t.dwTagId <= 0xA0021u)
                    has = true;
            }
            auto& e = byFx[p.szEffectFile];
            e.second++;
            e.first += has ? 1 : 0;
        }

        // ---- The decode, against the programs shipped beside it -----------
        const auto it = psByName.find(de.path().stem().string());
        if (it == psByName.end())
            continue;
        std::map<int, std::vector<std::string>> bySamplers;
        for (const auto& p : it->second) {
            const auto prog = ArbProgram(p);
            if (!prog.empty())
                bySamplers[ArbSamplerCount(prog)].push_back(prog);
        }
        for (const auto& pass : sh->arRenderPasses) {
            if (pass.szEffectFile != "Legacy.fx")
                continue;
            const int n = static_cast<int>(pass.arTextureStages.size());
            if (n == 0)
                continue;
            // Unambiguous only: one pass with this stage count, and every
            // program with that many samplers agreeing on what it does.
            std::size_t sameCount = 0;
            for (const auto& q : sh->arRenderPasses)
                sameCount += (static_cast<int>(q.arTextureStages.size()) == n) ? 1 : 0;
            if (sameCount != 1)
                continue;
            const auto cand = bySamplers.find(n);
            if (cand == bySamplers.end())
                continue;
            const auto samp = ArbSamplers(cand->second.front());
            bool agree = true;
            for (const auto& prog : cand->second) {
                const auto s = ArbSamplers(prog);
                for (int i = 0; i < n; ++i) {
                    const auto a = s.find(i), b = samp.find(i);
                    const bool ar = a != s.end() && a->second.rgb;
                    const bool br = b != samp.end() && b->second.rgb;
                    const bool aa = a != s.end() && a->second.alpha;
                    const bool ba = b != samp.end() && b->second.alpha;
                    agree = agree && ar == br && aa == ba;
                }
            }
            if (!agree)
                continue;

            f32 gc = 1.0f, ga = 1.0f;
            for (u32 i = 0; i < flakes::io::kD3StageArgCount; ++i) {
                u32 cCode = 0, aCode = 0;
                for (const auto& t : pass.arShaderParams) {
                    if (t.dwTagId == flakes::io::kD3TagStageColor + i)
                        cCode = t.dwValue;
                    if (t.dwTagId == flakes::io::kD3TagStageAlpha + i)
                        aCode = t.dwValue;
                }
                const auto ca = flakes::io::D3ReadStageArg(cCode);
                const auto aa = flakes::io::D3ReadStageArg(aCode);
                gc *= ca.gain;
                ga *= aa.gain;
                if (static_cast<int>(i) >= n)
                    continue;
                const auto s = samp.find(static_cast<int>(i));
                const bool actRgb = s != samp.end() && s->second.rgb;
                const bool actA = s != samp.end() && s->second.alpha;
                (ca.usesTexture == actRgb) ? ++chanOk : ++chanBad;
                (aa.usesTexture == actA) ? ++chanOk : ++chanBad;
            }

            const f32 progC = ArbOutputGain(cand->second.front(), false);
            const f32 progA = ArbOutputGain(cand->second.front(), true);
            if (progC >= 0.0f)
                (std::abs(progC - gc) < 1e-3f) ? ++gainOkC : ++gainBadC;
            if (progA >= 0.0f)
                (std::abs(progA - ga) < 1e-3f) ? ++gainOkA : ++gainBadA;
        }
    }

    for (const auto& [fx, n] : byFx) {
        if (n.second >= 100)
            std::printf("[d3-stage] %-16s block on %zu of %zu passes\n", fx.c_str(), n.first,
                        n.second);
    }
    std::printf("[d3-stage] channel: %zu agree, %zu disagree; colour gain: %zu / %zu; "
                "alpha gain: %zu / %zu\n",
                chanOk, chanBad, gainOkC, gainOkC + gainBadC, gainOkA, gainOkA + gainBadA);

    // Every Legacy pass carries the block, and the families whose programs this
    // shading model reproduces overwhelmingly do not: measured 855/855 against
    // 15/236 (ActorIrrad), 12/155 (Prop) and 6/179 (Scene). `Billboard.fx` is
    // 223/223 -- it is the other fixed-function family, and it draws particles
    // rather than geosets, so nothing here binds one.
    REQUIRE(byFx.count("Legacy.fx") == 1);
    CHECK(byFx["Legacy.fx"].first == byFx["Legacy.fx"].second);
    for (const char* fx : {"ActorIrrad.fx", "Prop.fx", "Scene.fx"}) {
        if (!byFx.count(fx))
            continue;
        INFO(fx << ": block on " << byFx[fx].first << " of " << byFx[fx].second);
        CHECK(byFx[fx].first * 5 < byFx[fx].second);
    }

    // Majority rules, and the numbers they were set from: 93.5% channel,
    // 97.0% colour gain, 91.8% alpha gain. The bounds are deliberately below
    // those — this gate is here to catch the decode being *lost*, not to freeze
    // a percentage that a corpus refresh would move.
    REQUIRE(chanOk + chanBad > 500);
    CHECK(chanOk * 10 > (chanOk + chanBad) * 8);
    REQUIRE(gainOkC + gainBadC > 100);
    CHECK(gainOkC * 10 > (gainOkC + gainBadC) * 8);
    REQUIRE(gainOkA + gainBadA > 100);
    CHECK(gainOkA * 10 > (gainOkA + gainBadA) * 8);
}

// ============================================================================
// DIAG: every RenderPass a named actor's sub-objects resolve to, verbatim.
//
// The two-sided report. `D3PassStateFor` reads pass 0's cull and nothing else,
// so this prints the whole `arRenderPasses` list per sub-object: if the
// original's two-sidedness is a second pass with the opposite winding, it is
// visible here and nowhere in the surface table.
// ============================================================================

TEST_CASE("D3 diag: the render passes of one actor", "[.diag][d3][install]") {
    using ::whiteout::flakes::ProductId;

    flakes::io::FileContentProvider provider;
    if (const char* root = std::getenv("WDX_TEST_D3_INSTALL"); root && *root)
        provider.SetInstallPath(root);
    provider.SetGame(ProductId::D3);
    if (provider.GamePath(ProductId::D3).empty()) {
        WARN("No Diablo III install. SKIPPED.");
        return;
    }
    flakes::io::D3SnoCache cache(&provider);

    const char* kFiles[] = {"Tyrael", "Imperius", "x1_Malthael"};
    if (const char* only = std::getenv("WDX_DIAG_APP"); only && *only)
        kFiles[0] = only;

    constexpr u32 kChain[] = {0x30502u, 0x30850u, 0x30830u, 0x30600u, 0x30500u};

    for (const char* file : kFiles) {
        const auto path = CorpusRoot() / "Appearances" / (std::string(file) + ".app");
        auto app = d3n::parseAppearances(ReadAll(path));
        if (!app) {
            WARN("missing " << path.string());
            continue;
        }
        std::printf("\n===== %s =====\n", file);
        const d3n::GeoSet* sets[2] = {&app->tGeoSet0, &app->tGeoSet1};
        for (int gs = 0; gs < 2; ++gs) {
            for (const auto& sub : sets[gs]->arSubObjects) {
                const auto* v = flakes::io::D3VariantFor(*app, sub, 0);
                if (!v) {
                    std::printf("  [%d] %-28s  (no variant)\n", gs, sub.szName.c_str());
                    continue;
                }
                const i32 shm = v->tMaterial.snoShaderMap.id;
                auto map = v->tMaterial.snoShaderMap.valid() ? cache.ShaderMap(shm) : nullptr;
                i32 shadersId = -1;
                u32 hitTag = 0;
                if (map) {
                    for (const u32 tag : kChain) {
                        for (const auto& e : map->arShaders) {
                            if (e.dwTagId == tag && e.snoShader.valid() && shadersId < 0) {
                                shadersId = e.snoShader.id;
                                hitTag = tag;
                            }
                        }
                        if (shadersId >= 0)
                            break;
                    }
                }
                auto sh = shadersId >= 0 ? cache.Shaders(shadersId) : nullptr;
                std::printf("  [%d] %-28s shm=%-7d tag=%05X shaders=%-7d passes=%zu %s\n", gs,
                            sub.szName.c_str(), shm, hitTag, shadersId,
                            sh ? sh->arRenderPasses.size() : 0u,
                            sh ? sh->szName.c_str() : "(unresolved)");
                if (!sh)
                    continue;
                for (std::size_t p = 0; p < sh->arRenderPasses.size(); ++p) {
                    const auto& rp = sh->arRenderPasses[p];
                    const auto& r = rp.tRenderParams;
                    std::printf("        p%zu cull=%d zw=%d aRef=%u blend=%d(%d,%d) flags=%08X "
                                "u00=%d u04=%d fx=%s vs=%s ps=%s\n",
                                p, r.dwCullMode, r.dwZWriteEnable,
                                static_cast<unsigned>(r.bAlphaRef), r.dwAlphaBlendEnable, r.dwSrcBlend,
                                r.dwDestBlend, static_cast<unsigned>(rp.dwPassFlags),
                                rp.dwUnknown00, rp.dwUnknown04, rp.szEffectFile.c_str(),
                                rp.szVertexShaderEntry.c_str(), rp.szPixelShaderEntry.c_str());
                    // Every RenderParams field, so a difference between two
                    // passes cannot hide in one this dump does not name.
                    const i32 all[] = {r.dwCullMode, r.dwZWriteEnable, r.dwZFunc,
                                       std::bit_cast<i32>(r.flDepthBias),
                                       std::bit_cast<i32>(r.flUnknown10), r.dwStencilEnable,
                                       r.dwStencilFunc, r.dwStencilRef, r.dwStencilPass, r.dwStencilFail,
                                       r.dwStencilZFail, r.dwAlphaTestEnable, r.dwAlphaFunc,
                                       static_cast<i32>(r.bAlphaRef), r.dwAlphaToCoverage, r.dwFogEnable,
                                       r.dwFillMode, r.dwColorWriteEnable, r.dwAlphaWriteEnable, r.dwAlphaBlendEnable,
                                       r.dwBlendOp, r.dwSrcBlend, r.dwDestBlend};
                    std::printf("           raw:");
                    for (i32 x : all)
                        std::printf(" %d", x);
                    std::printf("\n           stages:");
                    for (const auto& st : rp.arTextureStages)
                        std::printf(" [%d %d %d %d %d %.3f]", st.dwTextureType, st.dwAddressU,
                                    st.dwAddressV, st.dwAddressW, st.dwFilter, st.flMipMapLodBias);
                    std::printf("\n           tags:");
                    for (const auto& t : rp.arShaderParams)
                        std::printf(" %X=%u", t.dwTagId, t.dwValue);
                    std::printf("\n");
                }
                if (map) {
                    std::printf("        shm entries:");
                    for (const auto& e : map->arShaders)
                        std::printf(" %05X->%d", e.dwTagId, e.snoShader.id);
                    std::printf("\n");
                }
            }
        }
    }
}


// ============================================================================
// Two-sided is TWO PASSES, and drawing only the first draws half a cape.
//
// D3DCULL has no two-sided value. Content that wants a sheet lit from both
// sides therefore ships the same RenderPass twice — the second culling the
// opposite winding and raising tag 0xA003D, which negates the normal — and a
// build that submits pass 0 alone renders the front of every cape and nothing
// of the back. That is what Tyrael's clothes were doing.
//
// Two claims, both measured over all 1,507 corpus `.shd`:
//
//  1. **0xA003D is worth 1 on exactly 12 passes**, every one of them the LAST
//     pass of a `cloth_*` shader culling CCW against a pass 0 culling CW.
//
//  2. **`D3IsTwoSidedPassPair` fires on exactly those twelve.** The corpus has
//     293 multi-pass shaders and every other one varies its program, its
//     stages or its state as well — a second effect layer, not the same draw
//     mirrored — so the collapse can never be applied to one of those.
// ============================================================================

TEST_CASE("D3 corpus: two-sided is a CW pass plus a CCW pass", "[d3][corpus]") {
    const auto files = FindFiles(CorpusRoot() / "Shaders", ".shd");
    if (files.empty()) {
        WARN("No D3 Shaders corpus at " << (CorpusRoot() / "Shaders").string()
                                        << " (set WDX_TEST_D3_CORPUS). SKIPPED, not passed.");
        return;
    }

    // The whole population, by name. A thirteenth would be new content to look
    // at, not a number to bump.
    const std::vector<std::string> kExpect = {
        "cloth_alphatest",
        "cloth_alphatest_alphamask",
        "cloth_alphatest_gloss",
        "cloth_alphatest_gloss_alphamask",
        "cloth_alphatest_gloss_glow",
        "cloth_alphatest_gloss_glow_alphamask",
        "cloth_alphatest_gloss_glow_herotint",
        "cloth_alphatest_gloss_glow_herotint_alphamask",
        "cloth_alphatest_gloss_herotint",
        "cloth_alphatest_gloss_herotint_alphamask",
        "cloth_alphatest_herotint",
        "cloth_alphatest_herotint_alphamask",
    };

    std::vector<std::string> flagged; // D3IsTwoSidedPassPair said yes
    std::vector<std::string> tagged;  // carries 0xA003D != 0 on some pass
    std::size_t multiPass = 0, parsed = 0;
    std::map<i32, std::size_t> cullCounts;

    for (const auto& f : files) {
        auto sh = d3n::parseShaders(ReadAll(f));
        if (!sh)
            continue;
        ++parsed;
        if (sh->arRenderPasses.size() > 1)
            ++multiPass;
        if (d3p::D3IsTwoSidedPassPair(*sh))
            flagged.push_back(f.stem().string());
        for (const auto& pass : sh->arRenderPasses) {
            cullCounts[pass.tRenderParams.dwCullMode]++;
            for (const auto& t : pass.arShaderParams) {
                if (t.dwTagId != 0xA003Du || t.dwValue == 0)
                    continue;
                tagged.push_back(f.stem().string());
                INFO(f.stem().string());
                // The tagged pass is the CCW one, and it is the last.
                CHECK(pass.tRenderParams.dwCullMode == 3);
                CHECK(&pass == &sh->arRenderPasses.back());
            }
        }
    }
    std::sort(flagged.begin(), flagged.end());
    std::sort(tagged.begin(), tagged.end());

    std::printf("[d3-2sided] %zu shaders parsed, %zu multi-pass, %zu pairs, %zu tagged\n", parsed,
                multiPass, flagged.size(), tagged.size());
    for (const auto& n : flagged)
        std::printf("   %s\n", n.c_str());

    CHECK(flagged == kExpect);
    CHECK(tagged == kExpect);

    // The reason `frontCCW = true` with a plain back-face cull is enough for
    // everything this rule does not catch: **no shipped shader ever leads with
    // CCW.** Cull 3 appears on 12 passes and they are exactly the back halves
    // above, so there is no reverse-winding surface to get inside-out.
    std::printf("[d3-2sided] cull over every pass: none=%zu CW=%zu CCW=%zu\n", cullCounts[1],
                cullCounts[2], cullCounts[3]);
    CHECK(cullCounts[3] == kExpect.size());
    for (const auto& [value, n] : cullCounts) {
        INFO("cull " << value << " on " << n << " passes");
        CHECK(value >= 1);
        CHECK(value <= 3);
    }
}

// ============================================================================
// The same thing end to end: the surface a cape resolves to is two-sided, and
// the body beside it is not.
// ============================================================================

TEST_CASE("D3 install: cloth resolves two-sided and a body does not",
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
        bool pair;     ///< two-sidedness spelled as a CW pass plus a CCW one
        bool twoSided; ///< what the surface ends up with, either spelling
    };
    // Tyrael's cape is the report. Malthael's cloth resolves to the same
    // Shaders asset, so it is the second actor the one fix reaches; the bodies
    // beside them are single-pass CW and must stay culled, or a fix that simply
    // stopped culling would pass this too. Imperius is the third spelling: his
    // cloth pass 0 already asks for no culling, so he needs no pair rule and
    // never did — which is why the wing work never turned this up.
    const Want kWant[] = {
        {"Tyrael", "A_restored_cloth", true, true},
        {"Tyrael", "A_normal_mat", false, false},
        {"x1_Malthael", "A_normal_cloth", true, true},
        {"x1_Malthael", "A_normal_mat", false, false},
        {"Imperius", "A_normal_Cloth", false, true},
    };

    std::size_t resolved = 0;
    for (const auto& w : kWant) {
        const auto path = CorpusRoot() / "Appearances" / (std::string(w.file) + ".app");
        const auto bytes = ReadAll(path);
        auto app = d3n::parseAppearances(bytes);
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
        INFO(w.file << " / " << w.subObject);
        if (!st.resolved) {
            WARN(w.file << " / " << w.subObject << ": ShaderMap did not resolve -- SKIPPED.");
            continue;
        }
        ++resolved;

        // The whole way through, not just the pass state: the flag the PSO key
        // reads is `D3Surface::twoSided`, and it is a different line of code.
        auto adapter = flakes::io::D3ModelAdapter::LoadAppearance(
            ContentRef::FromPath(path.string()), bytes, cache);
        REQUIRE(adapter != nullptr);
        const auto textures =
            flakes::io::CollectD3Textures(adapter->SourceAppearance(), adapter->LookIndex());
        auto table = d3p::BuildD3SurfaceTable(adapter->SourceAppearance(), adapter->LookIndex(),
                                              textures, adapter->EmittedSubObjects(), &cache, {},
                                              nullptr);
        REQUIRE(table != nullptr);
        const auto emitted = adapter->EmittedSubObjects();
        const d3p::D3Surface* surface = nullptr;
        for (std::size_t g = 0; g < emitted.size(); ++g) {
            const d3n::GeoSet& set =
                (emitted[g].geoSet == 0) ? adapter->SourceAppearance().tGeoSet0
                                         : adapter->SourceAppearance().tGeoSet1;
            if (emitted[g].index < set.arSubObjects.size() &&
                EqualCiSv(set.arSubObjects[emitted[g].index].szName, w.subObject))
                surface = table->Surface(static_cast<i32>(g));
        }
        REQUIRE(surface != nullptr);

        std::printf("[d3-2sided] %-12s %-18s cull=%u pair=%d -> surface.twoSided=%d\n", w.file,
                    w.subObject, st.cull, static_cast<int>(st.twoSidedPair),
                    static_cast<int>(surface->twoSided));
        CHECK(st.twoSidedPair == w.pair);
        CHECK(surface->twoSided == w.twoSided);
    }
    if (resolved == 0) {
        WARN("No ShaderMap resolved. SKIPPED, not passed.");
        return;
    }
    CHECK(resolved == std::size(kWant));
}

// ============================================================================
// How much of the game the twelve shaders reach.
//
// Offline end to end: a `.shd` carries its own SNO id and so does a `.shm`, so
// the ShaderMap -> Shaders join needs no install — only the tag chain
// `ShaderMap_ResolveShaderOpaque` walks. Every appearance whose variant names
// one of those ShaderMaps was drawing one side of a two-sided surface.
// ============================================================================

TEST_CASE("D3 corpus: how many appearances the two-sided pairs reach", "[d3][corpus]") {
    const auto shaderFiles = FindFiles(CorpusRoot() / "Shaders", ".shd");
    const auto mapFiles = FindFiles(CorpusRoot() / "ShaderMap", ".shm");
    const auto appFiles = FindFiles(CorpusRoot() / "Appearances", ".app");
    if (shaderFiles.empty() || mapFiles.empty() || appFiles.empty()) {
        WARN("No D3 corpus (Shaders/ShaderMap/Appearances). SKIPPED, not passed.");
        return;
    }

    std::map<i32, std::string> pairShaders; // sno -> name, the twelve
    for (const auto& f : shaderFiles) {
        auto sh = d3n::parseShaders(ReadAll(f));
        if (sh && d3p::D3IsTwoSidedPassPair(*sh))
            pairShaders[sh->dwSnoId] = f.stem().string();
    }
    REQUIRE(pairShaders.size() == 12);

    // The same chain D3PassStateFor walks, so a map that reaches a pair through
    // a tag we never probe is correctly not counted.
    constexpr u32 kChain[] = {0x30502u, 0x30850u, 0x30830u, 0x30600u, 0x30500u};
    std::map<i32, std::string> pairMaps; // ShaderMap sno -> shader name
    for (const auto& f : mapFiles) {
        auto map = d3n::parseShaderMap(ReadAll(f));
        if (!map)
            continue;
        for (const u32 tag : kChain) {
            i32 hit = -1;
            for (const auto& e : map->arShaders) {
                if (e.dwTagId == tag && e.snoShader.valid() && hit < 0)
                    hit = e.snoShader.id;
            }
            if (hit < 0)
                continue;
            if (auto it = pairShaders.find(hit); it != pairShaders.end())
                pairMaps[map->dwSnoId] = it->second;
            break;
        }
    }

    std::size_t appsHit = 0, subObjectsHit = 0, subObjectsTotal = 0, parsed = 0;
    std::vector<std::string> names;
    for (const auto& f : appFiles) {
        auto app = d3n::parseAppearances(ReadAll(f));
        if (!app)
            continue;
        ++parsed;
        bool hit = false;
        const d3n::GeoSet* sets[2] = {&app->tGeoSet0, &app->tGeoSet1};
        for (const auto* set : sets) {
            for (const auto& sub : set->arSubObjects) {
                ++subObjectsTotal;
                const auto* v = flakes::io::D3VariantFor(*app, sub, 0);
                if (!v || !v->tMaterial.snoShaderMap.valid())
                    continue;
                if (!pairMaps.count(v->tMaterial.snoShaderMap.id))
                    continue;
                ++subObjectsHit;
                hit = true;
            }
        }
        if (hit) {
            ++appsHit;
            if (names.size() < 12)
                names.push_back(f.stem().string());
        }
    }

    std::printf("[d3-2sided] %zu ShaderMaps reach a pair | %zu of %zu appearances, "
                "%zu of %zu sub-objects\n",
                pairMaps.size(), appsHit, parsed, subObjectsHit, subObjectsTotal);
    std::printf("            e.g.");
    for (const auto& n : names)
        std::printf(" %s;", n.c_str());
    std::printf("\n");

    // The population is the point: every one of these was drawing half a
    // surface, and a change that stopped reaching them would show up here as a
    // collapse toward zero rather than as a render that merely looks fine.
    CHECK(pairMaps.size() > 0);
    CHECK(appsHit > 0);
    CHECK(subObjectsHit >= appsHit);
}

TEST_CASE("D3 diag: which emitted sub-objects are two-sided", "[.diag][d3][install]") {
    using ::whiteout::flakes::ProductId;
    flakes::io::FileContentProvider provider;
    provider.SetGame(ProductId::D3);
    if (provider.GamePath(ProductId::D3).empty()) {
        WARN("No install. SKIPPED.");
        return;
    }
    flakes::io::D3SnoCache cache(&provider);
    const char* kModels[] = {"Barbarian_Male", "Wizard_Female", "Demonhunter_Male",
                             "SkeletonKing",   "Skeleton",      "Cow_skeleton",
                             "Diablo",         "Tyrael",        "Imperius",
                             "x1_Malthael"};
    for (const char* m : kModels) {
        const auto path = CorpusRoot() / "Appearances" / (std::string(m) + ".app");
        const auto bytes = ReadAll(path);
        auto adapter = flakes::io::D3ModelAdapter::LoadAppearance(
            ContentRef::FromPath(path.string()), bytes, cache);
        if (!adapter)
            continue;
        const auto textures =
            flakes::io::CollectD3Textures(adapter->SourceAppearance(), adapter->LookIndex());
        auto table = d3p::BuildD3SurfaceTable(adapter->SourceAppearance(), adapter->LookIndex(),
                                              textures, adapter->EmittedSubObjects(), &cache, {},
                                              nullptr);
        const auto emitted = adapter->EmittedSubObjects();
        std::printf("\n%s (look %d, %zu emitted)\n", m, adapter->LookIndex(), emitted.size());
        for (std::size_t g = 0; g < emitted.size(); ++g) {
            const d3n::GeoSet& set = (emitted[g].geoSet == 0)
                                         ? adapter->SourceAppearance().tGeoSet0
                                         : adapter->SourceAppearance().tGeoSet1;
            if (emitted[g].index >= set.arSubObjects.size())
                continue;
            const auto* s = table->Surface(static_cast<i32>(g));
            std::printf("   %2zu %-28s valid=%d cull=%u pair=%d twoSided=%d\n", g,
                        set.arSubObjects[emitted[g].index].szName.c_str(),
                        s ? static_cast<int>(s->valid) : -1, s ? s->pass.cull : 0u,
                        s ? static_cast<int>(s->pass.twoSidedPair) : -1,
                        s ? static_cast<int>(s->twoSided) : -1);
        }
    }
}

// ============================================================================
// The RenderParams field map, named off the Windows 2.8.x build.
//
// `sub_5717F0` @0x5717F0 hands every field of a RenderPass to one D3D9 setter
// apiece -- the setters are the thin vtable wrappers at 0x73D450..0x73DC50,
// each of which is one `SetRenderState(D3DRS_*, value)` -- so the offsets read
// straight off the decompile:
//
//     +8  CULLMODE      +12 ZWRITEENABLE   +16 ZFUNC (+ ZENABLE = func != 8)
//     +20 DEPTHBIAS     +28 STENCILENABLE  +32..+48 stencil func/ref/pass/fail/zfail
//     +52 ALPHATESTENABLE  +56 ALPHAFUNC   +60 ALPHAREF
//     +64 ALPHATOCOVERAGE (no D3D9 setter)  +68 fog enable (not a device state)
//     +72 FILLMODE/SHADEMODE   +76/+80 COLORWRITEENABLE rgb / alpha
//     +84 ALPHABLENDENABLE  +88 BLENDOP  +92 SRCBLEND  +96 DESTBLEND
//     +100 a packed RGBA constant, -1 when unset
//
// Two of those are not `sub_5717F0` readings. RenderParams+0x38 goes to a
// vtable slot the D3D9 device leaves as a nullsub, and is named from the data:
// it is 1 on exactly the 43 `*_alphamask` passes, every one of which carries
// TAG_VS_ALPHA_TO_COVERAGE. RenderParams+0x3C is applied nowhere in
// `sub_5717F0`; `Render_BindShaderPass` @0x56CEC0 copies it into the render
// context and then clears it when `Render_IsFogEnabled` is false, which is
// what makes it the fog enable. Same function unpacks RenderParams+0x5C -- a
// field WhiteoutLib used to drop as trailing padding -- into the context
// float4, with -1 meaning (1,1,1,1).
//
// RenderParams sits at RenderPass+8, so subtract 8 for the offsets above. This
// case is the census that keeps the map honest: every count below was read off
// the shipped 1,506 `.shd`, and a value outside the enum the map claims is a
// naming error, not content.
// ============================================================================
TEST_CASE("D3 corpus: the RenderParams field map and the premultiplied family",
          "[d3][corpus][shd]") {
    const auto files = FindFiles(CorpusRoot() / "Shaders", ".shd");
    if (files.empty()) {
        WARN("No D3 shader corpus at " << (CorpusRoot() / "Shaders").string()
                                       << " (set WDX_TEST_D3_CORPUS). SKIPPED, not passed.");
        return;
    }

    std::map<i32, std::size_t> zfunc, alphaFunc, fill, blendOp, srcFactor, dstFactor;
    std::map<std::pair<i32, i32>, std::size_t> colorWrite;
    std::size_t passes = 0, alphaTestOn = 0, depthBias = 0, stencilOn = 0;
    std::map<i32, std::size_t> pmaTag, constColor;
    std::size_t pmaByBlend = 0;
    // The same three states restricted to PASS 0, which is the only pass this
    // build submits. A count over every pass overstates what it can act on.
    std::map<std::pair<i32, i32>, std::size_t> colorWrite0;
    std::map<i32, std::size_t> fill0;
    std::size_t depthBias0 = 0;
    // The two fields the D3D9 wrapper layer does not name.
    std::size_t a2cOn = 0, a2cTaggedAndSet = 0, a2cTaggedNotSet = 0, fogOn = 0;
    // Two labelled samples: a shader whose NAME states its depth state, and the
    // head of the premultiplied family.
    bool sawNoZ = false, sawPma = false;

    for (const auto& f : files) {
        auto sh = d3n::parseShaders(ReadAll(f));
        if (!sh)
            continue;
        const std::string name = f.stem().string();
        for (const auto& pass : sh->arRenderPasses) {
            const auto& r = pass.tRenderParams;
            ++passes;
            zfunc[r.dwZFunc]++;
            if (r.flDepthBias != 0.0f)
                ++depthBias;
            if (r.dwStencilEnable != 0)
                ++stencilOn;
            if (r.dwAlphaTestEnable != 0)
                ++alphaTestOn;
            alphaFunc[r.dwAlphaFunc]++;
            fill[r.dwFillMode]++;
            colorWrite[{r.dwColorWriteEnable, r.dwAlphaWriteEnable}]++;
            blendOp[r.dwBlendOp]++;
            srcFactor[r.dwSrcBlend]++;
            dstFactor[r.dwDestBlend]++;
            if (r.dwSrcBlend == 11)
                ++pmaByBlend;
            constColor[r.dwConstantColor]++;
            if (r.dwFogEnable != 0)
                ++fogOn;
            bool a2cTag = false;
            for (const auto& t : pass.arShaderParams) {
                if (t.dwTagId == 0xA002Bu)
                    pmaTag[static_cast<i32>(t.dwValue)]++;
                if (t.dwTagId == 0xA003Au)
                    a2cTag = true;
            }
            if (r.dwAlphaToCoverage != 0) {
                ++a2cOn;
                if (a2cTag)
                    ++a2cTaggedAndSet;
            } else if (a2cTag) {
                ++a2cTaggedNotSet;
            }

            // The dead fields, which is a claim worth asserting rather than
            // leaving as an absence: nothing varies at RenderPass+4, at
            // RenderParams+16, or at RenderPass+104.
            CHECK(pass.dwUnknown04 == 0);
            CHECK(r.flUnknown10 == 0.0f);
            CHECK(pass.dwUnknown68 == 0);
        }
        if (!sh->arRenderPasses.empty()) {
            const auto& r0 = sh->arRenderPasses.front().tRenderParams;
            colorWrite0[{r0.dwColorWriteEnable, r0.dwAlphaWriteEnable}]++;
            fill0[r0.dwFillMode]++;
            if (r0.flDepthBias != 0.0f)
                ++depthBias0;
        }
        if (name == "3D_prims_no_Z") {
            sawNoZ = true;
            // The one shader that states its own depth state out loud: no
            // write, and a compare of Always, which `sub_73DA60` turns into
            // ZENABLE 0.
            CHECK(sh->arRenderPasses.at(0).tRenderParams.dwZWriteEnable == 0);
            CHECK(sh->arRenderPasses.at(0).tRenderParams.dwZFunc == 8);
        }
        if (name == "particle_transparent_pma") {
            sawPma = true;
            const auto& p0 = sh->arRenderPasses.at(0);
            const auto& r = p0.tRenderParams;
            // (BLENDFACTOR, SRCALPHA), no alpha test, the alpha channel
            // written, no depth test -- the four things that separate it from
            // `particle_transparent`, which is (5, 6) / test on / alpha masked
            // off / compare LessEqual.
            CHECK(r.dwSrcBlend == 11);
            CHECK(r.dwDestBlend == 5);
            CHECK(r.dwAlphaTestEnable == 0);
            CHECK(r.dwColorWriteEnable == 1);
            CHECK(r.dwAlphaWriteEnable == 1);
            CHECK(r.dwZFunc == 8);
            bool tagged = false;
            for (const auto& t : p0.arShaderParams)
                if (t.dwTagId == 0xA002Bu && t.dwValue == 1u)
                    tagged = true;
            CHECK(tagged);
        }
    }
    CHECK(sawNoZ);
    CHECK(sawPma);

    auto dump = [](const char* label, const std::map<i32, std::size_t>& m) {
        std::printf("[d3-rp] %-12s", label);
        for (const auto& [v, n] : m)
            std::printf(" %d:%zu", v, n);
        std::printf("\n");
    };
    std::printf("[d3-rp] %zu passes | alpha test on %zu | depth bias %zu | stencil %zu\n", passes,
                alphaTestOn, depthBias, stencilOn);
    dump("zfunc", zfunc);
    dump("alphaFunc", alphaFunc);
    dump("fill", fill);
    dump("blendOp", blendOp);
    dump("src", srcFactor);
    dump("dst", dstFactor);
    dump("pma tag", pmaTag);
    std::printf("[d3-rp] colorWrite(rgb,a):");
    for (const auto& [k, n] : colorWrite)
        std::printf(" (%d,%d):%zu", k.first, k.second, n);
    std::printf("\n");

    // Every compare is a D3DCMPFUNC and every factor an engine blend enum. The
    // bounds are what make the map falsifiable: a field named for the wrong job
    // would spill outside them on the first shipped file that used it.
    for (const auto& [v, n] : zfunc) {
        INFO("zfunc " << v << " on " << n);
        CHECK(v >= 1);
        CHECK(v <= 8);
    }
    for (const auto& [v, n] : alphaFunc) {
        INFO("alphaFunc " << v << " on " << n);
        CHECK(v >= 0);
        CHECK(v <= 8);
    }
    for (const auto& [v, n] : srcFactor) {
        INFO("src " << v << " on " << n);
        CHECK(v >= 1);
        CHECK(v <= 11);
    }
    for (const auto& [v, n] : dstFactor) {
        INFO("dst " << v << " on " << n);
        CHECK(v >= 1);
        CHECK(v <= 11);
    }
    // BLENDOP is ADD on every shipped pass, which is why the field is not read.
    CHECK(blendOp.size() == 1);
    CHECK(blendOp.begin()->first == 1);
    // The colour write mask is a pair of booleans, not a D3D9 bit mask: the
    // wrapper builds `(rgb ? 7 : 0) | (alpha ? 8 : 0)` out of the two.
    for (const auto& [k, n] : colorWrite) {
        INFO("colorWrite (" << k.first << ", " << k.second << ") on " << n);
        CHECK(k.first >= 0);
        CHECK(k.first <= 1);
        CHECK(k.second >= 0);
        CHECK(k.second <= 1);
    }
    // The premultiplied family: the tag states the mode on 56 passes and the
    // blend factor is the constant on 58, so the two agree bar two passes that
    // carry no tag. Neither number is allowed to drift silently.
    CHECK(pmaByBlend == 58);
    CHECK(pmaTag[1] + pmaTag[2] == 56);

    // RenderParams+0x38 is alpha-to-coverage, and this is the whole evidence:
    // it is set on 43 passes, every one of them ALSO carrying the shader tag
    // the exe's own registry calls TAG_VS_ALPHA_TO_COVERAGE. Four more passes
    // carry the tag with the field clear, which is the direction that has to
    // hold -- the tag picks a program, the field asks the device for a mode
    // D3D9 cannot give it.
    std::printf("[d3-rp] alphaToCoverage set on %zu, all tagged %d, tag without the field %zu\n",
                a2cOn, a2cOn == a2cTaggedAndSet, a2cTaggedNotSet);
    CHECK(a2cOn == 43);
    CHECK(a2cTaggedAndSet == a2cOn);
    CHECK(a2cTaggedNotSet == 4);

    // RenderParams+0x3C is the fog enable -- a render-context flag rather than
    // a device state, which is why nothing in `sub_5717F0` touches it.
    std::printf("[d3-rp] fog enable on %zu\n", fogOn);
    CHECK(fogOn == 817);

    // RenderParams+0x5C, the field the generated layout used to discard: a
    // packed RGBA whose -1 means "unset". 931 passes leave it unset, 451 ship
    // an all-zero colour and 301 ship opaque black.
    std::printf("[d3-rp] constant colour: %zu unset, %zu zero, %zu 0xFF000000, %zu distinct\n",
                constColor[-1], constColor[0], constColor[static_cast<i32>(0xFF000000u)],
                constColor.size());
    CHECK(constColor[-1] == 931);
    CHECK(constColor[0] == 453);
    CHECK(constColor[static_cast<i32>(0xFF000000u)] == 301);

    // The three states this build applies out of the tail of the struct. The
    // pass-0 numbers are the ones that bound what it can act on, and they are
    // smaller: the (0,1) group is almost entirely pass 1 of a multi-pass
    // shader, and this build submits pass 0 alone.
    std::printf("[d3-rp] pass 0: colourWrite");
    for (const auto& [k, n] : colorWrite0)
        std::printf(" (%d,%d):%zu", k.first, k.second, n);
    std::printf(" | depth bias %zu | fill 1:%zu 2:%zu\n", depthBias0, fill0[1], fill0[2]);
    CHECK(depthBias == 28);
    CHECK(fill[1] == 1);
    CHECK(fill[2] == 4);
    CHECK(colorWrite[{0, 0}] == 73);
    CHECK(colorWrite[{0, 1}] == 111);
    CHECK(stencilOn == 61);
    CHECK(colorWrite0[{0, 0}] == 68);
    CHECK(colorWrite0[{0, 1}] == 4);
    CHECK(colorWrite0[{1, 0}] == 1166);
    CHECK(colorWrite0[{1, 1}] == 270);
    CHECK(depthBias0 == 28);
    // Every shipped non-solid fill is 2 -- solid with flat shading. The
    // wireframe wiring in D3StandardShading is faithful and unreachable.
    CHECK(fill0[1] == 1);
    CHECK(fill0[2] == 4);

    // The alpha test is three fields. Reading the reference alone gets 266 of
    // these passes wrong, and the two halves of that number are different
    // mistakes: 194 carry a reference the pass never applies, and 72 compare
    // the other way round.
    std::size_t refWithoutEnable = 0, inverted = 0;
    for (const auto& f : files) {
        auto sh = d3n::parseShaders(ReadAll(f));
        if (!sh)
            continue;
        for (const auto& pass : sh->arRenderPasses) {
            const auto& r = pass.tRenderParams;
            if (r.dwAlphaTestEnable == 0 && r.bAlphaRef != 0)
                ++refWithoutEnable;
            if (r.dwAlphaTestEnable != 0 && (r.dwAlphaFunc == 2 || r.dwAlphaFunc == 4))
                ++inverted;
        }
    }
    std::printf("[d3-rp] alpha test: %zu carry a reference with the test off, %zu compare "
                "the other way round\n",
                refWithoutEnable, inverted);
    CHECK(refWithoutEnable == 194);
    CHECK(inverted == 72);
}

// ============================================================================
// D3BlendFactor is not a D3DBLEND table.
//
// `sub_73DAD0` in the Windows 2.8.x build is the whole proof: a bare switch
// from the engine's enum to D3DBLEND that swaps the two DEST pairs and sends 11
// to BLENDFACTOR, whose constant `sub_73D580` pins at 0x00FFFFFF. Reading the
// field as D3DBLEND draws 3,175 of the corpus's 21,593 particle systems as
// opaque black rectangles.
// ============================================================================
TEST_CASE("D3: the blend enum is the engine's, not D3DBLEND", "[d3][blend]") {
    using BF = ::whiteout::flakes::gfx::BlendFactor;
    const auto c = [](u32 v) { return d3p::D3BlendFactor(v, BF::Zero, false); };
    const auto a = [](u32 v) { return d3p::D3BlendFactor(v, BF::Zero, true); };

    CHECK(c(1) == BF::Zero);
    CHECK(c(2) == BF::One);
    CHECK(c(3) == BF::SrcColor);
    CHECK(c(4) == BF::InvSrcColor);
    CHECK(c(5) == BF::SrcAlpha);
    CHECK(c(6) == BF::InvSrcAlpha);
    // The two pairs D3DBLEND orders the other way round.
    CHECK(c(7) == BF::DstColor);
    CHECK(c(8) == BF::InvDstColor);
    CHECK(c(9) == BF::DstAlpha);
    CHECK(c(10) == BF::InvDstAlpha);
    // The constant, which is white with a zero alpha -- exactly One for the
    // colour and Zero for the alpha, so it needs no constant-blend support.
    CHECK(c(11) == BF::One);
    CHECK(a(11) == BF::Zero);
    // Every other value is the same in both channels.
    for (u32 v : {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u}) {
        INFO("factor " << v);
        CHECK(c(v) == a(v));
    }
}

// ============================================================================
// The shader-tag registry.
//
// The Windows 2.8.x build carries the whole thing as data: a 63-entry array at
// 0x148B680, stride 44, each entry `{ u32 id, u32 typeCode, const void*
// pDefault, 0, const char* description, const char* TAG_NAME, ... }`. That is
// where every tag id this build keys on comes from, and it turns four names
// that were read out of the corpus into readings out of the binary:
//
//     0xA000F  TAG_VS_LIGHTING                 "Enable Lighting"
//     0xA0016  TAG_VS_PS0C_FUNC                "Stage 1 Color Function"
//     0xA001C  TAG_VS_PS0A_FUNC                "Stage 1 Alpha Function"
//     0xA002B  TAG_VS_PMA_FUNC                 "PMA Func"
//     0xA003D  TAG_VS_FLIP_NORMAL_BACKFACE     "Flip Normal (BackFace)"
//
// The last one is the sharpest: `kD3TagBackFacePass` was named from twelve
// `cloth_*` shaders and nothing else, and the registry calls it exactly what
// this build assumed -- the flip-the-normal switch of a back-face pass.
//
// The census below is the falsifiable half. A shipped pass carrying a tag the
// registry does not list would mean the array is not the whole enum.
// ============================================================================
TEST_CASE("D3 corpus: every shipped shader tag is in the registry", "[d3][corpus][shd]") {
    struct Tag {
        u32 id;
        const char* name;
    };
    // 0x148B680, in table order -- which is neither id order nor offset order.
    static constexpr Tag kRegistry[] = {
        {0xA0002u, "TAG_VS_ENABLE_SKINNING"},
        {0xA0003u, "TAG_VS_NUM_BONE_WEIGHTS"},
        {0xA0005u, "TAG_VS_EDGEALPHA"},
        {0xA0006u, "TAG_VS_LIGHTMAP"},
        {0xA000Au, "TAG_VS_NUM_DIRECTIONAL_LIGHTS"},
        {0xA0008u, "TAG_VS_NUM_POINT_LIGHTS"},
        {0xA0009u, "TAG_VS_NUM_SPOT_LIGHTS"},
        {0xA000Cu, "TAG_VS_NUM_CYLINDRICAL_LIGHTS"},
        {0xA000Du, "TAG_VS_NUM_POINT_LINEAR_LIGHTS"},
        {0xA0007u, "TAG_VS_ENABLE_FOGGING"},
        {0xA0004u, "TAG_VS_TIMER_PERIOD"},
        {0xA000Eu, "TAG_VS_MATERIAL_FUNC"},
        {0xA000Fu, "TAG_VS_LIGHTING"},
        {0xA0022u, "TAG_VS_GLOSSY"},
        {0xA0023u, "TAG_VS_TINT"},
        {0xA0024u, "TAG_VS_MASK"},
        {0xA0025u, "TAG_VS_SHADOW_SELF"},
        {0xA0026u, "TAG_VS_GLOW"},
        {0xA0027u, "TAG_VS_FRESNEL_BIAS"},
        {0xA0028u, "TAG_VS_FRESNEL_POWER"},
        {0xA000Bu, "TAG_VS_NUM_WAVES"},
        {0xA0010u, "TAG_VS_TEXCOORD0_FUNC"},
        {0xA0011u, "TAG_VS_TEXCOORD1_FUNC"},
        {0xA0012u, "TAG_VS_TEXCOORD2_FUNC"},
        {0xA0013u, "TAG_VS_TEXCOORD3_FUNC"},
        {0xA0014u, "TAG_VS_TEXCOORD4_FUNC"},
        {0xA0015u, "TAG_VS_TEXCOORD5_FUNC"},
        {0xA0016u, "TAG_VS_PS0C_FUNC"},
        {0xA0017u, "TAG_VS_PS1C_FUNC"},
        {0xA0018u, "TAG_VS_PS2C_FUNC"},
        {0xA0019u, "TAG_VS_PS3C_FUNC"},
        {0xA001Au, "TAG_VS_PS4C_FUNC"},
        {0xA001Bu, "TAG_VS_PS5C_FUNC"},
        {0xA001Cu, "TAG_VS_PS0A_FUNC"},
        {0xA001Du, "TAG_VS_PS1A_FUNC"},
        {0xA001Eu, "TAG_VS_PS2A_FUNC"},
        {0xA001Fu, "TAG_VS_PS3A_FUNC"},
        {0xA0020u, "TAG_VS_PS4A_FUNC"},
        {0xA0021u, "TAG_VS_PS5A_FUNC"},
        {0xA0029u, "TAG_VS_SHADER_MODEL"},
        {0xA002Au, "TAG_VS_USES_SHADOWS"},
        {0xA002Bu, "TAG_VS_PMA_FUNC"},
        {0xA002Cu, "TAG_VS_CLOTH"},
        {0xA002Du, "TAG_VS_CLOTH_SINGLE_SIDED"},
        {0xA002Eu, "TAG_VS_FLOAT_TEX_COORD"},
        {0xA002Fu, "TAG_VS_HERO_TINT"},
        {0xA0030u, "TAG_VS_ENABLE_DEFORM"},
        {0xA0031u, "TAG_VS_WEATHER_SCALES_DEFORM"},
        {0xA0032u, "TAG_VS_DIFF_ALPHA_IS_GLOSS"},
        {0xA0033u, "TAG_VS_SHADOW_ENABLED"},
        {0xA0034u, "TAG_VS_SHADOW_TECHNIQUE"},
        {0xA0035u, "TAG_VS_ALPHATESTFUNC"},
        {0xA0036u, "TAG_VS_VB_FORMAT"},
        {0xA0037u, "TAG_VS_USES_TANGENTS"},
        {0xA0038u, "TAG_VS_TRANSPARENT_ALPHA_TO_ONE"},
        {0xA0039u, "TAG_VS_MESH_LIGHTING"},
        {0xA003Au, "TAG_VS_ALPHA_TO_COVERAGE"},
        {0xA003Bu, "TAG_VS_ENABLE_NEAR_FADE_IN"},
        {0xA003Cu, "TAG_VS_PERPIXEL_LIGHTING"},
        {0xA003Du, "TAG_VS_FLIP_NORMAL_BACKFACE"},
        {0xA003Eu, "TAG_VS_SSAO"},
        {0xA003Fu, "TAG_VS_EARLY_DEPTH_STENCIL"},
        {0xA0040u, "TAG_VS_OUTPUT_FORMAT"},
    };
    CHECK(std::size(kRegistry) == 63);

    std::map<u32, const char*> byId;
    for (const auto& t : kRegistry)
        byId[t.id] = t.name;
    // 63 entries, 63 distinct ids: the table is a map, not a list with repeats.
    CHECK(byId.size() == std::size(kRegistry));

    // The five ids this build keys on, against the names the registry gives.
    CHECK(std::string(byId.at(flakes::io::kD3TagStageColor)) == "TAG_VS_PS0C_FUNC");
    CHECK(std::string(byId.at(flakes::io::kD3TagStageAlpha)) == "TAG_VS_PS0A_FUNC");
    CHECK(std::string(byId.at(0xA000Fu)) == "TAG_VS_LIGHTING");
    CHECK(std::string(byId.at(0xA002Bu)) == "TAG_VS_PMA_FUNC");
    CHECK(std::string(byId.at(0xA003Du)) == "TAG_VS_FLIP_NORMAL_BACKFACE");
    // The six stage-combine tags of each group are consecutive, which is what
    // lets D3PassStateFor walk them as `base + i`.
    for (u32 i = 0; i < flakes::io::kD3StageArgCount; ++i) {
        INFO("stage " << i);
        CHECK(byId.count(flakes::io::kD3TagStageColor + i) == 1);
        CHECK(byId.count(flakes::io::kD3TagStageAlpha + i) == 1);
    }

    const auto files = FindFiles(CorpusRoot() / "Shaders", ".shd");
    if (files.empty()) {
        WARN("No D3 shader corpus at " << (CorpusRoot() / "Shaders").string()
                                       << " (set WDX_TEST_D3_CORPUS). Registry checks ran; the "
                                          "corpus census SKIPPED.");
        return;
    }

    std::map<u32, std::size_t> used;
    std::size_t unknown = 0, entries = 0;
    for (const auto& f : files) {
        auto sh = d3n::parseShaders(ReadAll(f));
        if (!sh)
            continue;
        for (const auto& pass : sh->arRenderPasses)
            for (const auto& t : pass.arShaderParams) {
                ++entries;
                used[t.dwTagId]++;
                if (byId.count(t.dwTagId) == 0)
                    ++unknown;
            }
    }
    std::printf("[d3-tag] %zu tag entries over %zu shaders, %zu distinct ids, %zu outside the "
                "registry\n",
                entries, files.size(), used.size(), unknown);
    for (const auto& [id, n] : used) {
        INFO("tag id " << id << " on " << n << " passes");
        CHECK(byId.count(id) == 1);
    }
    CHECK(unknown == 0);
    // Shipped content exercises 59 of the 63. The four it never names are
    // TAG_VS_MASK (0xA0024), TAG_VS_MESH_LIGHTING (0xA0039),
    // TAG_VS_EARLY_DEPTH_STENCIL (0xA003F) and TAG_VS_OUTPUT_FORMAT (0xA0040) --
    // and the last two the registry itself marks "Set Internally", so an
    // authored asset was never going to carry them.
    CHECK(used.size() == 59);
}

// ============================================================================
// The fixed-function combine block, against the shipped programs.
//
// `d3_re_shaders/pixel/{Billboard,SoftBillboard}.fx__ps_legacy` are uber
// reconstructions of the shipped particle pixel programs -- 137 permutations,
// 121 distinct, each validated against its own bytecode. Decoding this block from the pass and running it as
// an ordered chain reproduces 199 of those 200 shader assets exactly; the eight
// witnesses below are the ones that pin the parts a single accumulated product
// could not express.
// ============================================================================

TEST_CASE("D3 corpus: a pass's combine block decodes to the shipped chain",
          "[d3][material][corpus]") {
    namespace wio = ::whiteout::flakes::io;
    struct Stage {
        i32 type;
        u8 colorOp;
        f32 colorGain;
        bool colorClamp;
        u8 alphaOp;
        f32 alphaGain;
        bool alphaClamp;
    };
    struct Want {
        const char* file;
        std::vector<Stage> stages;
        bool colorFirst, colorLast, alphaFirst, alphaLast;
    };
    constexpr u8 kSkip = wio::kD3StageSkip;
    constexpr u8 kMod = wio::kD3StageModulate;
    constexpr u8 kAdd = wio::kD3StageAdd;
    constexpr u8 kD3Rep = wio::kD3StageReplace;
    const Want kWant[] = {
        // The floor: one stage, plain modulate, the vertex colour at the head.
        {"particle_additive", {{1, kMod, 1, false, kMod, 1, false}}, true, false, true, false},
        // The `cm2x` / `am4x` in the name, and where they sit: on the SECOND
        // stage, not on the chain's output.
        {"particle_transparent_colorMult2x_alphaMult4x",
         {{1, kMod, 1, false, kMod, 1, false}, {19, kMod, 2, false, kMod, 4, false}},
         true, false, true, false},
        // Codes 28 and 27 — the same chain with both alpha stages saturating.
        // Reading the units digit as a gain alone loses the x4 and both clamps.
        {"particle_transparent_colorMult2x_alphaMult4x_pma",
         {{1, kMod, 1, false, kMod, 1, true}, {19, kMod, 2, false, kMod, 4, true}},
         true, false, true, false},
        // Alpha code 86 on the third stage: the erosion tail, which is a chain
        // operation on COLOR1 and not a texture. Its stage samples nothing.
        {"Particle_transparent_am4x_clamp_errosion",
         {{1, kSkip, 1, false, kMod, 4, false},
          {19, kSkip, 1, false, kMod, 4, false},
          {12, kSkip, 1, false, kSkip, 1, false}},
         true, false, true, false},
        // SoftBillboard declares the scene depth (type 39) as stage 0 and the
        // combine block does not count it. Read at the array's own index this
        // gives type 1 the colour SKIP that belongs to type 19, which drops the
        // diffuse's colour on 417 of the corpus's systems.
        {"softParticle_transparent_am4x",
         {{1, kMod, 1, false, kMod, 1, false}, {19, kSkip, 1, false, kMod, 4, false}},
         true, false, true, false},
        // An ADD stage, and the vertex colour entering the colour chain at BOTH
        // ends — code 20 at the head and code 40 as a textureless stage 3. Its
        // ALPHA opens on code 3, which is the REPLACE: at stage 0 that is the
        // same number as a modulate against the head's 1.0, so this row pins
        // the decode rather than a rendered difference. See kD3StageReplace.
        {"particle_transparent_blizzard",
         {{1, kMod, 1, false, kD3Rep, 1, false},
          {19, kMod, 2, false, kMod, 4, false},
          {12, kAdd, 1, true, kAdd, 1, true},
          {0, kSkip, 1, false, kSkip, 1, false}},
         true, true, false, true},
        // A leading code 3 in BOTH channels: the chain starts from the texture
        // and the vertex colour never enters at all.
        {"particle_transparent_blood_cm1x_pma",
         {{1, kD3Rep, 1, false, kD3Rep, 1, false},
          {19, kMod, 2, false, kMod, 2, true},
          {0, kSkip, 1, false, kSkip, 1, false}},
         false, false, false, true},
        // Four stages, and the alpha gain reaching 16 the way the asset spells
        // it: 1 x 2 x 2 x 4, one stage at a time.
        {"particle_transparent_glowTendril",
         {{1, kMod, 1, false, kMod, 1, false},
          {19, kMod, 2, false, kMod, 2, false},
          {12, kSkip, 1, false, kMod, 2, false},
          {14, kSkip, 1, false, kMod, 4, false}},
         true, false, true, false},
    };

    std::size_t checked = 0;
    for (const auto& w : kWant) {
        const fs::path p = CorpusRoot() / "Shaders" / (std::string(w.file) + ".shd");
        if (!fs::exists(p)) {
            WARN("missing " << p.string() << " -- SKIPPED, not passed.");
            continue;
        }
        auto sh = d3n::parseShaders(ReadAll(p));
        REQUIRE(sh);
        const auto st = d3p::D3PassStateOf(*sh);
        INFO(w.file);
        REQUIRE(st.resolved);
        REQUIRE(st.stageArgs);
        REQUIRE(st.combineCount == w.stages.size());
        for (std::size_t i = 0; i < w.stages.size(); ++i) {
            const auto& got = st.combines[i];
            const auto& e = w.stages[i];
            INFO("stage " << i << " of " << w.file);
            CHECK(got.type == e.type);
            CHECK(got.colorOp == e.colorOp);
            CHECK(got.alphaOp == e.alphaOp);
            CHECK(got.colorGain == e.colorGain);
            CHECK(got.alphaGain == e.alphaGain);
            CHECK(got.colorClamp == e.colorClamp);
            CHECK(got.alphaClamp == e.alphaClamp);
        }
        CHECK(st.colorVcolFirst == w.colorFirst);
        CHECK(st.colorVcolLast == w.colorLast);
        CHECK(st.alphaVcolFirst == w.alphaFirst);
        CHECK(st.alphaVcolLast == w.alphaLast);
        ++checked;
    }
    if (checked == 0) {
        WARN("No D3 Shaders corpus. SKIPPED, not passed.");
        return;
    }
    CHECK(checked == std::size(kWant));

    // And the population the decode moves, over the whole `.shd` corpus: how
    // many billboard passes carry a mid-chain clamp, an ADD, or a vertex colour
    // that is not simply at the head.
    const auto files = FindFiles(CorpusRoot() / "Shaders", ".shd");
    std::size_t billboard = 0, clamped = 0, added = 0, vcolNone = 0, vcolLast = 0, softShift = 0;
    for (const auto& f : files) {
        auto sh = d3n::parseShaders(ReadAll(f));
        if (!sh || sh->arRenderPasses.empty())
            continue;
        const std::string& fx = sh->arRenderPasses[0].szEffectFile;
        if (fx != "Billboard.fx" && fx != "SoftBillboard.fx")
            continue;
        const auto st = d3p::D3PassStateOf(*sh);
        ++billboard;
        bool anyClamp = false, anyAdd = false;
        for (u32 i = 0; i < st.combineCount; ++i) {
            anyClamp = anyClamp || st.combines[i].colorClamp || st.combines[i].alphaClamp;
            anyAdd = anyAdd || st.combines[i].colorOp == kAdd || st.combines[i].alphaOp == kAdd;
            // The whole point of indexing by content stage: the depth texture
            // is never a combine stage on any shipped pass.
            CHECK(st.combines[i].type != wio::kD3TextureTypeSceneDepth);
        }
        clamped += anyClamp ? 1 : 0;
        added += anyAdd ? 1 : 0;
        vcolNone += (!st.colorVcolFirst || !st.alphaVcolFirst) ? 1 : 0;
        vcolLast += (st.colorVcolLast || st.alphaVcolLast) ? 1 : 0;
        if (fx == "SoftBillboard.fx")
            ++softShift;
    }
    std::printf("[d3-combine] %zu billboard passes: %zu clamp mid-chain, %zu add, %zu drop the "
                "vertex colour, %zu take it last; %zu are SoftBillboard\n",
                billboard, clamped, added, vcolNone, vcolLast, softShift);
    CHECK(billboard == 245);
    CHECK(softShift == 21);
    CHECK(added == 5);
}

// ===========================================================================
// The pass NAMES its program, and its texcoord block says how wide it is.
//
// `szPixelShaderEntry` was read and thrown away until this pass. It is the only
// thing that separates the 15,841 `.prt` on the fixed-function chain from the
// 1,662 on one of eleven other programs — and for the flow shaders among them,
// `TAG_VS_TEXCOORD{i}_FUNC` is what says whether the fourth texture is a second
// flow map or nothing at all. Both are transcribed here from the shipped
// assets, and the corpus-wide census is printed so the populations are on the
// record rather than in a commit message.
// ===========================================================================
TEST_CASE("D3 corpus: the pass names its pixel program and its texcoord count",
          "[d3][material][corpus]") {
    namespace wio = ::whiteout::flakes::io;
    struct Want {
        const char* file;
        const char* entry;
        u32 texcoords;
    };
    const Want kWant[] = {
        // One flow map at slot 1: two texcoords, and the second is the noise.
        {"particle_additive_flow", "ps_particle_flow", 2},
        // The same program with three, which is what `_flowMult_masked` means.
        {"particle_additive_flowMult_masked", "ps_particle_flow", 4},
        // Two diffuse layers and one flow map. The pass parks a combine code on
        // the flow slot; the program does not run it.
        {"particle_transparent_blood_cm2x_flow", "ps_legacy_flow", 3},
        {"particle_additive_alphaWipe_flowMult", "ps_legacy_flow", 4},
        // Three colour layers and the flow map last.
        {"particle_Transparent_firewall_flow", "ps_firewall_flow", 4},
        {"particle_Transparent_firewall_am4x_flow", "ps_firewall_am4x_flow", 4},
        // Not a chain at all: two premultiplied layers summed.
        {"particle_blendAdd_flow", "ps_billboard_blendAdd_flowMult", 3},
        {"particle_blendAdd_flowMult", "ps_billboard_blendAdd_flowMult", 4},
        // And the floor, so the census below is anchored at both ends.
        {"particle_additive", "ps_legacy", 1},
    };

    std::size_t checked = 0;
    for (const auto& w : kWant) {
        const fs::path p = CorpusRoot() / "Shaders" / (std::string(w.file) + ".shd");
        if (!fs::exists(p)) {
            WARN("missing " << p.string() << " -- SKIPPED, not passed.");
            continue;
        }
        auto sh = d3n::parseShaders(ReadAll(p));
        REQUIRE(sh);
        const auto st = d3p::D3PassStateOf(*sh);
        INFO(w.file);
        REQUIRE(st.resolved);
        CHECK(st.pixelEntry == w.entry);
        CHECK(st.texcoordCount == w.texcoords);
        ++checked;
    }
    if (checked == 0) {
        WARN("No D3 Shaders corpus. SKIPPED, not passed.");
        return;
    }
    CHECK(checked == std::size(kWant));

    // The decode itself. A live code names a vertex uv SET, and each set is the
    // baked transform of one texture TYPE; 10 is the engine's "no such slot".
    CHECK(wio::D3TexcoordUvSet(0) == 0);
    CHECK(wio::D3TexcoordUvSet(1) == 0);
    CHECK(wio::D3TexcoordUvSet(8) == 1);
    CHECK(wio::D3TexcoordUvSet(11) == 0);
    CHECK(wio::D3TexcoordUvSet(13) == 2);
    CHECK(wio::D3TexcoordUvSet(15) == 3);
    CHECK(!wio::D3TexcoordIsLive(10));
    CHECK(wio::D3TexcoordIsLive(11));
    CHECK(wio::kD3TexcoordSetType[0] == 1);
    CHECK(wio::kD3TexcoordSetType[3] == 14);

    const auto files = FindFiles(CorpusRoot() / "Shaders", ".shd");
    std::map<std::string, std::size_t> entries;
    std::map<u32, std::size_t> widths;
    std::size_t billboard = 0, unnamed = 0, reroute = 0;
    for (const auto& f : files) {
        auto sh = d3n::parseShaders(ReadAll(f));
        if (!sh || sh->arRenderPasses.empty())
            continue;
        const std::string& fx = sh->arRenderPasses[0].szEffectFile;
        if (fx != "Billboard.fx" && fx != "SoftBillboard.fx")
            continue;
        const auto st = d3p::D3PassStateOf(*sh);
        ++billboard;
        ++entries[st.pixelEntry];
        ++widths[st.texcoordCount];
        if (st.pixelEntry.empty())
            ++unnamed;
        for (u32 i = 0; i < st.texcoordCount && i < 4; ++i) {
            if (wio::D3TexcoordUvSet(st.texcoordFunc[i]) != i) {
                ++reroute;
                break;
            }
        }
    }
    std::printf("[d3-program] %zu billboard passes, %zu distinct pixel entries:", billboard,
                entries.size());
    for (const auto& [e, k] : entries)
        std::printf(" %s=%zu", e.c_str(), k);
    std::printf("\n[d3-program] %zu route a stage to another layer's uv; texcoord count:",
                reroute);
    for (const auto& [w, k] : widths)
        std::printf(" %u=%zu", w, k);
    std::printf("\n");
    // 245 and not the 243 this said until 2026-08-31: two of the shipped
    // billboard shaders have no PATH in CASC (`Base/unk_0/1997` and
    // `.../2000`), so a by-name extraction of the corpus never wrote them and
    // every corpus-side sweep was two short -- which is where the audit's
    // "906 `.prt` resolve to no Shaders" came from. They carry their names
    // internally (`particle_additive_am4x`,
    // `particle_transparent_am4x_soft_noFog`) and are extracted under them now.
    // The runtime always resolved them: the SNO cache reads by file id.
    // See D3_MATERIAL_AUDIT.md 9.1.
    CHECK(billboard == 245);
    CHECK(entries.size() == 12);
    // Every shipped billboard pass names its program. A blank would put the
    // material back on the chain silently, which is the failure this replaces.
    CHECK(unnamed == 0);
    // Nine passes declare a FIFTH texcoord, which a particle has no texture for
    // -- `Particle_DrawBatch` binds four. They are why the program shapes are
    // applied only where the layer types are a prefix of the bind order, and
    // asserted here so the day a tenth appears this says so.
    // The vertex program has four texcoord outputs, so no pass may resolve to
    // more than four live slots -- which is the check that catches an absent
    // tag read as disabled rather than as its registry default, and a hole read
    // as the end of the block.
    CHECK(widths.find(5) == widths.end());
    CHECK(widths.find(6) == widths.end());
    // And the reroutes: 39 passes give some stage a uv set that is not its own
    // index, which is the half of the block this build now acts on.
    // `particle_additive_am4x_alphatest` tags (1, 8, 1, 8), so its stages 2 and
    // 3 sample at stages 0 and 1's transforms. Nothing distinguishes code 1
    // from code 13 at slot 2 except that -- both name matTex[2] -- so a matrix
    // reading of the block makes the author's choice mean nothing.
    CHECK(reroute == 39);
}

// ============================================================================
// A dump, not a gate. Hidden (`[.d3dump]`), run by name:
//
//   tests/d3_surface_table_test.exe "[.d3dump]"
//
// WDX_D3_DUMP names the actors/appearances, comma separated, by their bare
// stem; every sub-object material, its resolved RenderPass and its slots come
// out. This is the tool the "why is that mesh white" questions are answered
// with -- a surface with no Diffuse slot has one line saying so.
// ============================================================================

namespace {

std::vector<std::string> DumpNames() {
    std::vector<std::string> out;
    const char* v = std::getenv("WDX_D3_DUMP");
    if (!v || !*v)
        return out;
    std::string s(v), cur;
    for (char c : s) {
        if (c == ',') {
            if (!cur.empty())
                out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

std::string TypeBits(u64 mask) {
    std::string s;
    for (int i = 0; i < 64; ++i) {
        if (mask & (1ull << i)) {
            if (!s.empty())
                s += ",";
            s += std::to_string(i);
        }
    }
    return s.empty() ? "-" : s;
}

const char* SlotNameOf(u32 i) {
    switch (static_cast<flakes::io::D3SlotKind>(i)) {
    case flakes::io::D3SlotKind::Diffuse:
        return "Diffuse";
    case flakes::io::D3SlotKind::Normal:
        return "Normal";
    case flakes::io::D3SlotKind::Specular:
        return "Specular";
    case flakes::io::D3SlotKind::Emissive:
        return "Emissive";
    case flakes::io::D3SlotKind::Lightmap:
        return "Lightmap";
    case flakes::io::D3SlotKind::Irradiance:
        return "Irradiance";
    case flakes::io::D3SlotKind::AlphaMask0:
        return "AlphaMask0";
    case flakes::io::D3SlotKind::AlphaMask1:
        return "AlphaMask1";
    case flakes::io::D3SlotKind::AlphaMask2:
        return "AlphaMask2";
    default:
        return "?";
    }
}

} // namespace

TEST_CASE("D3 dump: surfaces of a named actor", "[.d3dump]") {
    using ::whiteout::flakes::ProductId;
    const auto names = DumpNames();
    if (names.empty()) {
        WARN("Set WDX_D3_DUMP=<stem>[,<stem>...]. SKIPPED.");
        return;
    }
    flakes::io::FileContentProvider provider;
    if (const char* root = std::getenv("WDX_TEST_D3_INSTALL"); root && *root)
        provider.SetInstallPath(root);
    provider.SetGame(ProductId::D3);
    if (provider.GamePath(ProductId::D3).empty()) {
        WARN("No Diablo III install. SKIPPED.");
        return;
    }

    ::whiteout::sno::CoreToc toc;
    if (auto tocBytes = provider.ReadFile("Base\\CoreTOC.dat"))
        toc.parse(*tocBytes);
    auto snoName = [&toc](i32 sno) -> std::string {
        if (const auto* e = toc.findById(sno))
            return e->name;
        return "#" + std::to_string(sno);
    };

    flakes::io::D3SnoCache cache(&provider);

    for (const std::string& stem : names) {
        std::shared_ptr<flakes::io::D3ModelAdapter> adapter;
        std::string opened;
        const char* kDirs[2] = {"Base\\Actor\\", "Base\\Appearance\\"};
        const char* kExts[2] = {".acr", ".app"};
        for (int k = 0; k < 2; ++k) {
            const std::string ref = std::string(kDirs[k]) + stem + kExts[k];
            auto bytes = provider.ReadFile(ref);
            if (!bytes)
                continue;
            adapter = (k == 0) ? flakes::io::D3ModelAdapter::LoadActor(
                                     ContentRef::FromPath(ref), *bytes, cache, true)
                               : flakes::io::D3ModelAdapter::LoadAppearance(
                                     ContentRef::FromPath(ref), *bytes, cache);
            if (adapter) {
                opened = ref;
                break;
            }
        }
        if (!adapter) {
            std::printf("\n### %s: NOT FOUND\n", stem.c_str());
            continue;
        }
        const auto& app = adapter->SourceAppearance();
        const auto textures = flakes::io::CollectD3Textures(app, adapter->LookIndex());
        const auto emitted = adapter->EmittedSubObjects();
        auto table = d3p::BuildD3SurfaceTable(app, adapter->LookIndex(), textures, emitted, &cache,
                                              {}, nullptr);
        std::printf("\n### %s  (%s)  look=%u  geosets=%zu  textures=%zu\n", stem.c_str(),
                    opened.c_str(), adapter->LookIndex(), emitted.size(), textures.size());
        for (std::size_t t = 0; t < textures.size(); ++t)
            std::printf("    tex[%zu] = %s\n", t, snoName(textures[t].snoId).c_str());

        const auto hidden = adapter->GeosetHidden();
        for (std::size_t g = 0; g < emitted.size(); ++g) {
            const d3n::GeoSet& set = (emitted[g].geoSet == 0) ? app.tGeoSet0 : app.tGeoSet1;
            if (emitted[g].index >= set.arSubObjects.size())
                continue;
            const d3n::SubObject& sub = set.arSubObjects[emitted[g].index];
            const auto* s = table->Surface(static_cast<u32>(g));
            if (!s)
                continue;
            std::printf("\n  [%zu] %-30s verts=%zu tris=%zu %s\n", g, sub.szName.c_str(),
                        sub.arVertices.size(), sub.arIndices.size() / 3,
                        (g < hidden.size() && hidden[g]) ? "HIDDEN" : "");
            const auto* v = flakes::io::D3VariantFor(app, sub, adapter->LookIndex());
            std::shared_ptr<const d3n::Material> keep;
            const d3n::UberMaterial* mat = v ? flakes::io::D3MaterialOf(*v, &cache, keep) : nullptr;
            if (!mat) {
                std::printf("      NO MATERIAL (variant=%p)\n", static_cast<const void*>(v));
                continue;
            }
            std::printf("      shaderMap=%s  entries=%zu  valid=%d unlit=%d twoSided=%d\n",
                        mat->snoShaderMap.valid() ? snoName(mat->snoShaderMap.id).c_str() : "-",
                        mat->arTextures.size(), (int)s->valid, (int)s->unlit, (int)s->twoSided);
            std::printf("      colors: diff=(%.2f %.2f %.2f %.2f) spec=(%.2f %.2f %.2f %.2f) "
                        "emis=(%.2f %.2f %.2f %.2f) amb=(%.2f %.2f %.2f %.2f) shin=%.2f fl=%u\n",
                        s->diffuse.x, s->diffuse.y, s->diffuse.z, s->diffuse.w, s->specular.x,
                        s->specular.y, s->specular.z, s->specular.w, s->emissive.x, s->emissive.y,
                        s->emissive.z, s->emissive.w, s->ambient.x, s->ambient.y, s->ambient.z,
                        s->ambient.w, s->shininess, s->materialFlags);
            const auto& p = s->pass;
            if (!p.resolved) {
                std::printf("      PASS UNRESOLVED\n");
            } else {
                std::printf("      pass: %s / %s  cull=%u blend=%d(%u,%u) pma=%u zw=%d zf=%u "
                            "atest=%d(%u,%u) cw=%d,%d lit=%d vcLight=%d vcAlpha=%d glow=%d "
                            "stageArgs=%d gain=(%.2f,%.2f) hole=%d\n",
                            p.effectFile.c_str(), p.pixelEntry.c_str(), p.cull, (int)p.blendEnable,
                            p.blendSrc, p.blendDst, p.pmaMode, (int)p.depthWrite, p.depthFunc,
                            (int)p.alphaTestEnable, p.alphaFunc, (u32)p.alphaRef, (int)p.colorWrite,
                            (int)p.alphaWrite, (int)p.lit, (int)p.vertexColorLights,
                            (int)p.vertexAlpha, (int)p.glowLights, (int)p.stageArgs, p.colorGain,
                            p.alphaGain, (int)p.stageHole);
                std::printf("      stages:");
                for (u32 i = 0; i < p.stageCount; ++i)
                    std::printf(" [%u]type=%d wrap=%u", i, p.stages[i].type, p.stages[i].wrapBits);
                std::printf("\n      declared={%s} color={%s} alpha={%s} colorSampled={%s} "
                            "alphaSampled={%s}\n",
                            TypeBits(p.declaredTypes).c_str(), TypeBits(p.colorTypes).c_str(),
                            TypeBits(p.alphaTypes).c_str(), TypeBits(p.colorSampledTypes).c_str(),
                            TypeBits(p.alphaSampledTypes).c_str());
                for (u32 i = 0; i < p.combineCount; ++i) {
                    const auto& cb = p.combines[i];
                    std::printf("      combine[%u] type=%d cop=%u aop=%u cg=%.2f ag=%.2f\n", i,
                                cb.type, cb.colorOp, cb.alphaOp, cb.colorGain, cb.alphaGain);
                }
            }
            std::printf("      entries:");
            for (const auto& e : mat->arTextures) {
                const i32 type = flakes::io::D3TextureTypeOf(e);
                const auto uv = flakes::io::D3ReadUvXform(e);
                std::printf(" (t%d->%s uv=%d%s)", type,
                            e.snoTexture.valid() ? snoName(e.snoTexture.id).c_str() : "-",
                            static_cast<int>(uv.mode), uv.animated ? " ANIM" : "");
            }
            std::printf("\n");
            for (u32 i = 0; i < flakes::io::kD3SlotCount; ++i) {
                const auto& sl = s->slots[i];
                if (sl.textureId < 0 && sl.rawType == 0)
                    continue;
                std::printf("      slot %-11s tex=%d rawType=%d ch=%u\n", SlotNameOf(i),
                            sl.textureId, sl.rawType, sl.channels);
            }
            if (s->chainCount > 0) {
                std::printf("      CHAIN edgeAlpha=%u vcol=(cF%d cL%d aF%d aL%d) "
                            "factor=(cF%d cL%d aF%d aL%d)\n",
                            p.edgeAlpha, (int)p.colorVcolFirst, (int)p.colorVcolLast,
                            (int)p.alphaVcolFirst, (int)p.alphaVcolLast, (int)p.colorFactorFirst,
                            (int)p.colorFactorLast, (int)p.alphaFactorFirst,
                            (int)p.alphaFactorLast);
                for (u32 i = 0; i < s->chainCount; ++i) {
                    const auto& st = s->chain[i];
                    std::printf("      stage[%u] type=%-3d tex=%-3d cop=%u aop=%u cg=%.1f ag=%.1f "
                                "cc=%d ac=%d wrap=%u uvAnim=%d\n",
                                i, st.rawType, st.textureId, st.colorOp, st.alphaOp, st.colorGain,
                                st.alphaGain, (int)st.colorClamp, (int)st.alphaClamp, st.wrapFlags,
                                st.uvTransformId);
                }
            } else if (s->slots[(u32)flakes::io::D3SlotKind::Diffuse].textureId < 0) {
                std::printf("      *** NO DIFFUSE SLOT -> draws white ***\n");
            }
            if (!d3p::D3SurfaceDrawsToScene(*s))
                std::printf("      *** phase 3 ONLY -> not a scene draw ***\n");
            if (s->distortion) {
                const auto* dd = s->distortion.get();
                const auto& dp = dd->pass;
                std::printf("      >> DISTORTION phase=%d %s/%s twoTex=%d blend=%d(%u,%u) zw=%d "
                            "cull=%u edgeAlpha=%u chain=%u\n",
                            dp.renderPhase, dp.effectFile.c_str(), dp.pixelEntry.c_str(),
                            (int)dp.distortionTwoTex, (int)dp.blendEnable, dp.blendSrc, dp.blendDst,
                            (int)dp.depthWrite, dp.cull, dp.edgeAlpha, dd->chainCount);
                for (u32 i = 0; i < dd->chainCount; ++i) {
                    const auto& st = dd->chain[i];
                    std::printf("      >> stage[%u] type=%-3d tex=%-3d cop=%u aop=%u cg=%.1f "
                                "ag=%.1f uvAnim=%d\n",
                                i, st.rawType, st.textureId, st.colorOp, st.alphaOp, st.colorGain,
                                st.alphaGain, st.uvTransformId);
                }
            }
        }
    }
}

// The other half of the dump: a `Shaders` asset, pass by pass, with its whole
// tag map. `.shd` is name-keyed, so this needs the corpus and no install.
//
//   WDX_D3_SHD=actor_transparent_edgeAlpha_cm2x2_am4x4_bloom tests/...exe "[.d3dump]"
TEST_CASE("D3 dump: a Shaders asset, tags and all", "[.d3dump]") {
    const char* v = std::getenv("WDX_D3_SHD");
    if (!v || !*v) {
        WARN("Set WDX_D3_SHD=<stem>[,<stem>...]. SKIPPED.");
        return;
    }
    std::vector<std::string> names;
    {
        std::string s(v), cur;
        for (char c : s) {
            if (c == ',') {
                if (!cur.empty())
                    names.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty())
            names.push_back(cur);
    }
    for (const std::string& stem : names) {
        const auto path = CorpusRoot() / "Shaders" / (stem + ".shd");
        const auto bytes = ReadAll(path);
        auto sh = d3n::parseShaders(bytes);
        if (!sh) {
            std::printf("\n### %s: NOT FOUND (%s)\n", stem.c_str(), path.string().c_str());
            continue;
        }
        std::printf("\n### %s  passes=%zu\n", stem.c_str(), sh->arRenderPasses.size());
        for (std::size_t i = 0; i < sh->arRenderPasses.size(); ++i) {
            const auto& p = sh->arRenderPasses[i];
            std::printf("  pass[%zu] %s  vs=%s ps=%s  flags=%u/%u/%u\n", i, p.szEffectFile.c_str(),
                        p.szVertexShaderEntry.c_str(), p.szPixelShaderEntry.c_str(),
                        p.dwUnknown00, p.dwUnknown04, p.dwPassFlags);
            std::printf("    stages:");
            for (const auto& st : p.arTextureStages)
                std::printf(" (t%d au=%u av=%u f=%u)", st.dwTextureType, st.dwAddressU,
                            st.dwAddressV, st.dwFilter);
            std::printf("\n    tags:");
            for (const auto& t : p.arShaderParams) {
                const f32 asFloat = std::bit_cast<f32>(t.dwValue);
                std::printf(" [%X type=%d val=%u/%.3f]", t.dwTagId, t.nValueType, t.dwValue,
                            asFloat);
            }
            std::printf("\n");
        }
    }
}

// The Legacy.fx census: everything the chain needs, over every shipped `.shd`.
TEST_CASE("D3 dump: the Legacy.fx chain census", "[.d3dump]") {
    const auto files = FindFiles(CorpusRoot() / "Shaders", ".shd");
    if (files.empty()) {
        WARN("No D3 corpus. SKIPPED.");
        return;
    }
    std::map<u32, std::size_t> edgeAlpha, constColor, constColorFactor, constColorLegacy;
    std::map<u32, std::size_t> colorUnits, alphaUnits, colorTens, alphaTens;
    std::map<std::string, std::size_t> families;
    std::map<u32, std::size_t> stageCounts;
    std::map<u32, std::size_t> legacyStageTypes;
    std::map<std::string, std::size_t> legacyEntries;
    std::size_t legacy = 0, legacyWithBlock = 0, blockNoLegacy = 0, factorUsers = 0;
    std::size_t code3First = 0, code3Later = 0;
    std::vector<std::string> later;
    std::map<u32, std::size_t> tens4Codes, tens8Codes, tens0Codes;
    for (const auto& f : files) {
        auto sh = d3n::parseShaders(ReadAll(f));
        if (!sh)
            continue;
        for (const auto& p : sh->arRenderPasses) {
            ++families[p.szEffectFile];
            const bool isLegacy = p.szEffectFile == "Legacy.fx";
            bool hasBlock = false;
            u32 ea = 0;
            for (const auto& t : p.arShaderParams) {
                if (t.dwTagId == 0xA0005u)
                    ea = t.dwValue;
                if (t.dwTagId >= 0xA0016u && t.dwTagId <= 0xA0021u)
                    hasBlock = true;
            }
            if (isLegacy) {
                ++legacy;
                ++legacyEntries[p.szPixelShaderEntry];
                for (const auto& sg : p.arTextureStages)
                    ++legacyStageTypes[static_cast<u32>(sg.dwTextureType)];
                ++edgeAlpha[ea];
                ++constColorLegacy[static_cast<u32>(p.tRenderParams.dwConstantColor)];
                if (hasBlock)
                    ++legacyWithBlock;
                u32 content = 0;
                for (const auto& s : p.arTextureStages)
                    if (s.dwTextureType != 39)
                        ++content;
                ++stageCounts[content];
            } else if (hasBlock) {
                ++blockNoLegacy;
            }
            ++constColor[static_cast<u32>(p.tRenderParams.dwConstantColor)];
            bool usesFactor = false;
            bool seenColor = false, seenAlpha = false;
            for (u32 i = 0; i < 6; ++i) {
                for (int ch = 0; ch < 2; ++ch) {
                    const u32 want = (ch == 0 ? 0xA0016u : 0xA001Cu) + i;
                    const d3n::ShaderTagMapEntry* tag = nullptr;
                    for (const auto& t : p.arShaderParams)
                        if (t.dwTagId == want)
                            tag = &t;
                    if (!tag)
                        continue;
                    const u32 code = tag->dwValue;
                    const u32 tens = code / 10, units = code % 10;
                    if (ch == 0) {
                        ++colorTens[tens];
                        if (tens == 2)
                            ++colorUnits[units];
                    } else {
                        ++alphaTens[tens];
                        if (tens == 2)
                            ++alphaUnits[units];
                    }
                    if (tens == 4)
                        ++tens4Codes[code];
                    if (tens == 8)
                        ++tens8Codes[code];
                    if (tens == 0)
                        ++tens0Codes[code];
                    if ((tens == 2 && units == 2) || code == 41)
                        usesFactor = true;
                    if (code == 3) {
                        bool& seen = (ch == 0) ? seenColor : seenAlpha;
                        if (!seen) {
                            ++code3First;
                        } else {
                            ++code3Later;
                            if (ch == 0 && later.size() < 24)
                                later.push_back(f.stem().string() + "/" +
                                                std::to_string(i));
                        }
                    }
                    if (tens == 0 ? code == 3 : (code != 71 && tens != 4 && tens != 8)) {
                        if (ch == 0)
                            seenColor = true;
                        else
                            seenAlpha = true;
                    }
                }
            }
            if (usesFactor) {
                ++factorUsers;
                ++constColorFactor[static_cast<u32>(p.tRenderParams.dwConstantColor)];
            }
        }
    }
    auto dump = [](const char* label, const std::map<u32, std::size_t>& m) {
        std::printf("%-24s", label);
        for (const auto& [k, n] : m)
            std::printf(" %u:%zu", k, n);
        std::printf("\n");
    };
    auto dumpHex = [](const char* label, const std::map<u32, std::size_t>& m) {
        std::printf("%-24s", label);
        for (const auto& [k, n] : m)
            std::printf(" 0x%08X:%zu", k, n);
        std::printf("\n");
    };
    std::printf("\n[legacy-census] files=%zu Legacy.fx passes=%zu withStageBlock=%zu "
                "blockOnNonLegacy=%zu factorUsers=%zu\n",
                files.size(), legacy, legacyWithBlock, blockNoLegacy, factorUsers);
    dump("edgeAlpha(0xA0005)", edgeAlpha);
    dump("legacy contentStages", stageCounts);
    dump("legacy stage TYPES", legacyStageTypes);
    dumpHex("constColor all", constColor);
    dumpHex("constColor Legacy", constColorLegacy);
    dumpHex("constColor FACTOR", constColorFactor);
    dump("color tens", colorTens);
    dump("alpha tens", alphaTens);
    dump("color units(tens2)", colorUnits);
    dump("alpha units(tens2)", alphaUnits);
    dump("tens0 codes", tens0Codes);
    dump("tens4 codes", tens4Codes);
    dump("tens8 codes", tens8Codes);
    std::printf("code3 first=%zu later=%zu\n", code3First, code3Later);
    std::printf("code3 later examples:");
    for (const auto& n : later)
        std::printf(" %s", n.c_str());
    std::printf("\n");
    std::printf("legacy entries:");
    for (const auto& [k, n] : legacyEntries)
        std::printf(" %s=%zu", k.c_str(), n);
    std::printf("|\nfamilies:");
    for (const auto& [k, n] : families)
        std::printf(" %s=%zu", k.c_str(), n);
    std::printf("\n");
}

// ============================================================================
// The `Legacy.fx` chain — D3_MATERIAL_DESIGN.md §7.6-7.8.
//
// Three claims, each with a discriminator that the reading it replaced fails:
//
//  1. **The chain's gate is the PIXEL ENTRY, not the effect file.** 784 of the
//     855 `Legacy.fx` passes run `ps_legacy`; the other 71 are 27 different
//     programs and none of them is the combiner. Both halves are asserted, so a
//     gate widened back to the effect file trips the second.
//
//  2. **Code 3 is a REPLACE.** Read as a modulate it is the same number only
//     while the chain head is still 1, so the two named shaders here are chosen
//     for having a contributing stage BEFORE the code 3 — where the two
//     readings disagree and the shipped program says which is right.
//
//  3. **`TAG_VS_EDGEALPHA` takes five values and no others.** The counts are
//     exact, so a tag read at the wrong id or a value silently folded away
//     moves one of them.
// ============================================================================

TEST_CASE("D3 corpus: the Legacy.fx chain's population and its gate", "[d3][corpus][material]") {
    const auto files = FindFiles(CorpusRoot() / "Shaders", ".shd");
    if (files.empty()) {
        WARN("No D3 corpus at " << (CorpusRoot() / "Shaders").string()
                                << " (set WDX_TEST_D3_CORPUS). SKIPPED, not passed.");
        return;
    }
    std::size_t legacy = 0, psLegacy = 0, withBlock = 0;
    std::map<u32, std::size_t> edgeAlpha;
    std::set<std::string> otherEntries;
    for (const auto& f : files) {
        auto sh = d3n::parseShaders(ReadAll(f));
        if (!sh)
            continue;
        for (const auto& p : sh->arRenderPasses) {
            if (p.szEffectFile != "Legacy.fx")
                continue;
            ++legacy;
            if (p.szPixelShaderEntry == "ps_legacy")
                ++psLegacy;
            else
                otherEntries.insert(p.szPixelShaderEntry);
            u32 ea = 0;
            bool block = false;
            for (const auto& t : p.arShaderParams) {
                if (t.dwTagId == 0xA0005u)
                    ea = t.dwValue;
                if (t.dwTagId >= 0xA0016u && t.dwTagId <= 0xA0021u)
                    block = true;
            }
            withBlock += block ? 1 : 0;
            ++edgeAlpha[ea];
        }
    }
    std::printf("[d3-chain] Legacy.fx passes=%zu ps_legacy=%zu otherEntries=%zu withBlock=%zu\n",
                legacy, psLegacy, otherEntries.size(), withBlock);
    std::printf("[d3-chain] TAG_VS_EDGEALPHA:");
    for (const auto& [v, n] : edgeAlpha)
        std::printf(" %u=%zu", v, n);
    std::printf("\n");

    CHECK(legacy == 855);
    // The gate. 784 is the population the chain runs on; the 71 that are left
    // are what a `Legacy.fx`-wide gate would wrongly claim, and they are 27
    // distinct programs rather than a rounding error.
    CHECK(psLegacy == 784);
    CHECK(otherEntries.size() == 27);
    CHECK(otherEntries.count("ps_legacy_Malthael_wings_flow") == 1);
    // 830 of the 855 carry the combine block, which is why the block alone
    // cannot be the gate either — it is present on 46 of the 71 passes whose
    // program is not the combiner.
    CHECK(withBlock == 830);

    // Five values, exact counts, nothing else. The four non-zero ones join
    // one-to-one onto the `vs_legacy` reconstruction's COLOR0_A permutations.
    CHECK(edgeAlpha.size() == 5);
    CHECK(edgeAlpha[0] == 538);
    CHECK(edgeAlpha[1] == 172);
    CHECK(edgeAlpha[2] == 104);
    CHECK(edgeAlpha[4] == 24);
    CHECK(edgeAlpha[5] == 17);
}

TEST_CASE("D3 corpus: stage code 3 replaces the channel", "[d3][corpus][material]") {
    struct Want {
        const char* shader;
        std::vector<u8> colorOps; ///< kD3Stage*, in content-stage order.
        std::vector<u8> alphaOps;
    };
    // Both chosen because a stage that FEEDS the channel comes before the code
    // 3 — the only place a replace and a modulate are different numbers.
    //
    // `actor_complex_Transparent_Ground` codes its colour block 3/3/3/10 over
    // stages (12, 14, 1, 6) and its shipped program is `saturate(tex1 + tex6)`:
    // the two masks are sampled for their ALPHA and their colour is thrown
    // away, which only a replace does.
    //
    // `actor_seismicSlam_wave` is the Death Maiden's and the two-hander's
    // shader, 3/3/24/0 over (12, 11, 13, 0) against `tex11 * tex13 * 2`.
    const Want kWant[] = {
        {"actor_complex_Transparent_Ground",
         {flakes::io::kD3StageReplace, flakes::io::kD3StageReplace, flakes::io::kD3StageReplace, flakes::io::kD3StageAdd},
         {flakes::io::kD3StageModulate, flakes::io::kD3StageModulate, flakes::io::kD3StageSkip, flakes::io::kD3StageSkip}},
        {"actor_seismicSlam_wave",
         {flakes::io::kD3StageReplace, flakes::io::kD3StageReplace, flakes::io::kD3StageModulate, flakes::io::kD3StageSkip},
         {flakes::io::kD3StageModulate, flakes::io::kD3StageModulate, flakes::io::kD3StageModulate, flakes::io::kD3StageSkip}},
    };
    std::size_t checked = 0;
    for (const auto& w : kWant) {
        const auto bytes = ReadAll(CorpusRoot() / "Shaders" / (std::string(w.shader) + ".shd"));
        auto sh = d3n::parseShaders(bytes);
        if (!sh) {
            WARN("missing " << w.shader << " -- SKIPPED, not passed.");
            continue;
        }
        ++checked;
        INFO(w.shader);
        const auto st = d3p::D3PassStateOf(*sh);
        REQUIRE(st.resolved);
        REQUIRE(st.stageArgs);
        REQUIRE(st.combineCount == w.colorOps.size());
        for (u32 i = 0; i < st.combineCount; ++i) {
            INFO("stage " << i << " type " << st.combines[i].type);
            CHECK(st.combines[i].colorOp == w.colorOps[i]);
            CHECK(st.combines[i].alphaOp == w.alphaOps[i]);
        }
    }
    if (checked == 0) {
        WARN("No corpus shaders read. SKIPPED, not passed.");
        return;
    }
    // The units digit that is NOT the vertex colour. `actor_transparent_
    // edgeAlpha_cm2x2_am4x4_bloom` opens its colour block on 22, which the
    // reconstruction reads `MOD|FACTOR`; every units-0 neighbour is
    // `MOD|VCOLOR`. Reading 22 as the diffuse is what it did before.
    {
        auto sh = d3n::parseShaders(
            ReadAll(CorpusRoot() / "Shaders" / "actor_transparent_edgeAlpha_cm2x2_am4x4_bloom.shd"));
        if (sh) {
            const auto st = d3p::D3PassStateOf(*sh);
            CHECK(st.colorFactorFirst);
            CHECK_FALSE(st.colorVcolFirst);
            CHECK(st.alphaVcolFirst);
            CHECK_FALSE(st.alphaFactorFirst);
            CHECK(st.edgeAlpha == 1);
        }
    }
}

TEST_CASE("D3 install: the chain binds every stage it declares", "[d3][material][install]") {
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
        const char* file;      ///< `.app` under Appearances/
        const char* subObject;
        u32 chainStages;       ///< 0 = this sub-object must NOT take the chain.
        u32 edgeAlpha;
    };
    // The three actors this was written for, plus the two Malthael layers that
    // are the discriminator: `wingCore_mat` runs `ps_legacy` and takes a chain,
    // `wingMidLayer_mat` runs `ps_legacy_Malthael_wings_flow` and must not — a
    // gate on the effect file alone passes the first and fails the second.
    const Want kWant[] = {
        {"x1_deathMaiden", "A_normal_fx", 5, 1},
        {"x1_urzael_cannonball_model", "outer_mat", 3, 1},
        {"x1_urzael_cannonball_model", "inner_mat", 2, 2},
        {"Imperius", "wing_mat", 4, 0},
        {"x1_Malthael", "wingCore_mat", 1, 1},
        // The tag is read whatever the program is; only the CHAIN stands down.
        {"x1_Malthael", "wingMidLayer_mat", 0, 1},
        {"x1_Malthael", "A_normal_mat", 0, 0}, // ActorIrrad, the other family
    };

    std::size_t checked = 0;
    for (const auto& w : kWant) {
        const auto path = CorpusRoot() / "Appearances" / (std::string(w.file) + ".app");
        const auto bytes = ReadAll(path);
        auto app = d3n::parseAppearances(bytes);
        if (!app) {
            WARN("missing " << path.string() << " -- SKIPPED, not passed.");
            continue;
        }
        auto adapter = flakes::io::D3ModelAdapter::LoadAppearance(
            ContentRef::FromPath(path.string()), bytes, cache);
        REQUIRE(adapter != nullptr);
        const auto textures =
            flakes::io::CollectD3Textures(adapter->SourceAppearance(), adapter->LookIndex(),
                                          adapter->EmittedSubObjects(), adapter->GeosetLooks());
        auto table = d3p::BuildD3SurfaceTable(adapter->SourceAppearance(), adapter->LookIndex(),
                                              textures, adapter->EmittedSubObjects(), &cache, {},
                                              nullptr);
        REQUIRE(table != nullptr);

        const auto emitted = adapter->EmittedSubObjects();
        const d3p::D3Surface* surface = nullptr;
        for (std::size_t g = 0; g < emitted.size(); ++g) {
            const d3n::GeoSet& set = (emitted[g].geoSet == 0) ? adapter->SourceAppearance().tGeoSet0
                                                              : adapter->SourceAppearance().tGeoSet1;
            if (emitted[g].index < set.arSubObjects.size() &&
                EqualCiSv(set.arSubObjects[emitted[g].index].szName, w.subObject))
                surface = table->Surface(static_cast<u32>(g));
        }
        if (!surface) {
            WARN(w.file << " / " << w.subObject << ": no such sub-object -- SKIPPED.");
            continue;
        }
        if (!surface->pass.resolved) {
            WARN(w.file << " / " << w.subObject << ": ShaderMap did not resolve -- SKIPPED.");
            continue;
        }
        ++checked;
        INFO(w.file << " / " << w.subObject << " (" << surface->pass.effectFile << " / "
                    << surface->pass.pixelEntry << ")");
        std::printf("[d3-chain] %-28s %-18s %s/%s stages=%u edge=%u\n", w.file, w.subObject,
                    surface->pass.effectFile.c_str(), surface->pass.pixelEntry.c_str(),
                    surface->chainCount, surface->pass.edgeAlpha);
        CHECK(surface->chainCount == w.chainStages);
        CHECK(surface->pass.edgeAlpha == w.edgeAlpha);
        // The defect itself: a chain whose stages have no texture id draws the
        // slot defaults, which is white. Every stage the pass declares a
        // combine for must have resolved one.
        for (u32 i = 0; i < surface->chainCount; ++i) {
            if (surface->chain[i].rawType == 0)
                continue; // a hole binds nothing by construction
            INFO("stage " << i << " type " << surface->chain[i].rawType);
            CHECK(surface->chain[i].textureId >= 0);
        }
    }
    if (checked == 0) {
        WARN("Nothing resolved. SKIPPED, not passed.");
        return;
    }
    CHECK(checked >= 5);
}

// How much of the corpus the chain reaches, and how much of it resolves. The
// defect this replaced was a stage with no texture id, so that is the number
// with a bound on it.
TEST_CASE("D3 install: the chain over the corpus", "[d3][material][install]") {
    using ::whiteout::flakes::ProductId;
    flakes::io::FileContentProvider provider;
    if (const char* root = std::getenv("WDX_TEST_D3_INSTALL"); root && *root)
        provider.SetInstallPath(root);
    provider.SetGame(ProductId::D3);
    if (provider.GamePath(ProductId::D3).empty()) {
        WARN("No Diablo III install. SKIPPED, not passed.");
        return;
    }
    const auto files = FindFiles(CorpusRoot() / "Appearances", ".app");
    if (files.empty()) {
        WARN("No D3 corpus. SKIPPED, not passed.");
        return;
    }
    flakes::io::D3SnoCache cache(&provider);
    const std::size_t limit = SweepLimit();
    const std::size_t take = (limit == 0) ? files.size() : (std::min)(limit, files.size());

    std::size_t models = 0, surfaces = 0, chained = 0, stages = 0, holes = 0, unresolved = 0;
    std::size_t chainModels = 0, edgeAlphaSurfaces = 0;
    std::map<u32, std::size_t> stageCounts;
    std::vector<std::string> worst;
    for (std::size_t i = 0; i < take; ++i) {
        const auto bytes = ReadAll(files[i]);
        auto adapter = flakes::io::D3ModelAdapter::LoadAppearance(
            ContentRef::FromPath(files[i].string()), bytes, cache);
        if (!adapter)
            continue;
        ++models;
        const auto textures =
            flakes::io::CollectD3Textures(adapter->SourceAppearance(), adapter->LookIndex(),
                                          adapter->EmittedSubObjects(), adapter->GeosetLooks());
        auto table = d3p::BuildD3SurfaceTable(adapter->SourceAppearance(), adapter->LookIndex(),
                                              textures, adapter->EmittedSubObjects(), &cache, {},
                                              nullptr);
        bool any = false;
        for (const auto& s : table->Surfaces()) {
            ++surfaces;
            if (s.chainCount == 0)
                continue;
            ++chained;
            any = true;
            ++stageCounts[s.chainCount];
            if (s.pass.edgeAlpha != 0)
                ++edgeAlphaSurfaces;
            for (u32 k = 0; k < s.chainCount; ++k) {
                ++stages;
                // A type-0 hole binds nothing, and neither does a type whose
                // stage reads a CORE ASSET rather than the entry's own texture
                // (25, 40, 41 — see D3TypeOwnsTexture). Two stages in the whole
                // corpus are the second case, both type 25 on
                // `p6_necro_boneSpear_blood_spawn`, and the engine would give
                // them `DeadBodyBlood`; this build samples white there.
                if (s.chain[k].rawType == 0 ||
                    !flakes::io::D3TypeOwnsTexture(s.chain[k].rawType)) {
                    ++holes;
                } else if (s.chain[k].textureId < 0) {
                    ++unresolved;
                    if (worst.size() < 12)
                        worst.push_back(files[i].filename().string() + " type " +
                                        std::to_string(s.chain[k].rawType));
                }
            }
        }
        chainModels += any ? 1 : 0;
    }
    std::printf("[d3-chain] %zu models (%zu with a chain), %zu surfaces, %zu chained; "
                "%zu stages: %zu holes, %zu unresolved; %zu carry an edge alpha\n",
                models, chainModels, surfaces, chained, stages, holes, unresolved,
                edgeAlphaSurfaces);
    std::printf("[d3-chain] stage counts:");
    for (const auto& [k, n] : stageCounts)
        std::printf(" %u=%zu", k, n);
    std::printf("\n");
    for (const auto& w : worst)
        std::printf("[d3-chain] unresolved: %s\n", w.c_str());

    REQUIRE(models > 0);
    // The chain is not a corner case, and it is not everything either — a
    // build that ran it on every surface would fail the second half.
    CHECK(chained > 0);
    CHECK(chained < surfaces);
    // Every stage a chain declares must bind a texture. A type-0 hole is the
    // one exception, and it is counted separately so "no unresolved stages"
    // cannot be met by mislabelling one.
    CHECK(unresolved == 0);
    CHECK(holes > 0);
}

// ============================================================================
// The distortion phase. Diablo III draws a screen-space distortion by writing a
// signed offset field into a side buffer and bending the finished scene through
// it, and NOTHING about the pass's own state says so: 43 of the 48 shipped
// distortion passes are `Legacy.fx :: ps_legacy`, the same combiner half the
// game runs, with an ordinary blend and an ordinary chain. What says so is
// `RenderPass::dwUnknown00` -- the pass's RENDER PHASE -- and phase 3 is the
// distortion buffer.
//
// This is the gate on that reading, and it is a biconditional rather than a
// count: `Shaders::dwShaderFlags` bit 3 is the flag the ENGINE tests
// (`ActorModel_EmitSubObjectDrawCalls` skips a sub-object carrying it when
// capability 97 is unavailable, and capability 97 is what gates the distortion
// post effect in `sub_744200`), so the two readings have to close on each other
// exactly or one of them is wrong.
// ============================================================================
TEST_CASE("D3 corpus: the distortion phase IS the distortion flag", "[d3][corpus][material]") {
    namespace wio = ::whiteout::flakes::io;
    const auto files = FindFiles(CorpusRoot() / "Shaders", ".shd");
    if (files.empty()) {
        WARN("No D3 corpus at " << (CorpusRoot() / "Shaders").string()
                                << " (set WDX_TEST_D3_CORPUS). SKIPPED, not passed.");
        return;
    }
    std::size_t shaders = 0, flagged = 0, phased = 0, phase3Passes = 0;
    std::vector<std::string> flagNoPhase, phaseNoFlag;
    std::map<std::string, std::size_t> programs;
    std::map<i32, std::size_t> phaseCounts;
    for (const auto& f : files) {
        auto sh = d3n::parseShaders(ReadAll(f));
        if (!sh)
            continue;
        ++shaders;
        bool has3 = false;
        for (const auto& p : sh->arRenderPasses) {
            ++phaseCounts[p.dwUnknown00];
            if (p.dwUnknown00 != wio::kD3RenderPhaseDistortion)
                continue;
            has3 = true;
            ++phase3Passes;
            ++programs[p.szEffectFile + "::" + p.szPixelShaderEntry];
        }
        const bool flag = (sh->dwShaderFlags & wio::kD3ShaderFlagDistortion) != 0;
        flagged += flag ? 1 : 0;
        phased += has3 ? 1 : 0;
        if (flag && !has3)
            flagNoPhase.push_back(sh->szName);
        if (has3 && !flag)
            phaseNoFlag.push_back(sh->szName);
    }
    std::printf("[d3-distort] %zu shaders: %zu carry flag 8, %zu declare a phase-3 pass "
                "(%zu passes)\n",
                shaders, flagged, phased, phase3Passes);
    std::printf("[d3-distort] phase-3 programs:");
    for (const auto& [k, n] : programs)
        std::printf(" %s=%zu", k.c_str(), n);
    std::printf("\n[d3-distort] phases:");
    for (const auto& [k, n] : phaseCounts)
        std::printf(" %d=%zu", k, n);
    std::printf("\n");

    for (const auto& n : flagNoPhase)
        INFO("flag but no phase-3 pass: " << n);
    for (const auto& n : phaseNoFlag)
        INFO("phase-3 pass but no flag: " << n);
    // The biconditional, both directions. Either failing means the phase is not
    // what selects the distortion pass and the whole routing is guesswork.
    CHECK(flagNoPhase.empty());
    CHECK(phaseNoFlag.empty());
    CHECK(flagged == phased);

    // The atlas counted 1,506 shipped `Shaders` assets in build 2.8.0.99920;
    // an extracted corpus can carry a couple more. Bounded rather than pinned
    // so the two numbers below stay the claim.
    CHECK(shaders >= 1506);
    CHECK(flagged == 45);
    CHECK(phase3Passes == 48);
    // And the reason the EFFECT FILE cannot be the gate: only five of the 48
    // are `Distortion.fx`. A build routing on the effect file would leave 43
    // distortion passes painting their offset field into the scene as if it
    // were colour, which is what the Mystic Ally's caustics were doing.
    CHECK(programs["Distortion.fx::ps_distortion2tex"] == 5);
    CHECK(programs["Legacy.fx::ps_legacy"] == 37);
    CHECK(programs["Billboard.fx::ps_legacy"] == 5);
    CHECK(programs["ActorIrrad.fx::ps_cloak"] == 1);
}

// The other half: WHICH pass, not just whether there is one. Position cannot
// answer it -- the distortion pass is last in one shipped shader and first in
// another -- so a build reading pass 0 draws one actor's offset field as its
// body and drops the other's shimmer.
TEST_CASE("D3 corpus: the distortion pass is picked by phase, not position",
          "[d3][corpus][material]") {
    namespace wio = ::whiteout::flakes::io;
    struct Want {
        const char* shader;
        u32 scenePass;
        i32 distortionPass;
    };
    const Want kWant[] = {
        // (6, 3): the shimmer is the LAST pass. Reading pass 0 as the only pass
        // loses it entirely.
        {"actor_mysticAlly", 0, 1},
        // (3, 6, 6): the shimmer is the FIRST pass. Reading pass 0 as the scene
        // pass draws the offset field as the monster.
        {"actor_watermonster", 1, 0},
        {"actor_diamondSkin", 0, 2},
        // Both passes are phase 3, so the scene index names a pass that must
        // not reach the scene at all.
        {"actor_snakeman_cloak", 0, 0},
        // A single-pass distortion shader: the same, and the common case (26 of
        // the 45).
        {"distortion_2tex", 0, 0},
        // Two families in one asset -- Prop.fx body, Distortion.fx shimmer.
        {"prop_transparent_distortion_gloss_vertalpha", 0, 1},
        // Not a distortion shader at all.
        {"actor_alphatest_alphaComp_skin", 0, -1},
    };
    std::size_t checked = 0;
    for (const auto& w : kWant) {
        const auto path = CorpusRoot() / "Shaders" / (std::string(w.shader) + ".shd");
        const auto bytes = ReadAll(path);
        if (bytes.empty()) {
            WARN("missing " << path.string() << " -- SKIPPED, not passed.");
            continue;
        }
        auto sh = d3n::parseShaders(bytes);
        if (!sh)
            continue;
        ++checked;
        INFO(w.shader);
        CHECK(wio::D3ScenePassIndex(*sh) == w.scenePass);
        CHECK(wio::D3DistortionPassIndex(*sh) == w.distortionPass);
    }
    if (checked == 0) {
        WARN("No D3 corpus. SKIPPED, not passed.");
        return;
    }
    CHECK(checked >= 5);
}

// `Distortion.fx :: ps_distortion2tex` is six instructions and reads NO combine
// block -- `prop_transparent_distortion_gloss_vertalpha` ships one and the
// program cannot see it. So the surface table synthesises the two stages, and
// this pins that synthesis against what the reconstruction validated at zero
// difference: replace, then add, alpha from COLOR0.w alone.
TEST_CASE("D3 corpus: ps_distortion2tex is a synthesised two-stage chain",
          "[d3][corpus][material]") {
    namespace wio = ::whiteout::flakes::io;
    const auto files = FindFiles(CorpusRoot() / "Shaders", ".shd");
    if (files.empty()) {
        WARN("No D3 corpus. SKIPPED, not passed.");
        return;
    }
    std::size_t seen = 0;
    for (const auto& f : files) {
        auto sh = d3n::parseShaders(ReadAll(f));
        if (!sh)
            continue;
        for (u32 i = 0; i < sh->arRenderPasses.size(); ++i) {
            if (sh->arRenderPasses[i].szPixelShaderEntry != "ps_distortion2tex")
                continue;
            ++seen;
            INFO(sh->szName << " pass " << i);
            const d3p::D3PassState st = d3p::D3PassStateOf(*sh, i);
            CHECK(st.distortionTwoTex);
            CHECK(st.distortion);
            CHECK(st.renderPhase == wio::kD3RenderPhaseDistortion);
            // Two stages, always: the program samples texLayer0 and texLayer1
            // and nothing else.
            CHECK(st.stageCount == 2);
            // The pass state carries the edge alpha whatever the program does
            // with it; here it is the whole alpha channel.
            CHECK((st.edgeAlpha == 1 || st.edgeAlpha == 2));
        }
    }
    std::printf("[d3-distort] ps_distortion2tex passes: %zu\n", seen);
    CHECK(seen == 5);
}

// The surface side, on the two actors this work was reported for. A distortion
// material builds TWO surfaces over one material, and which of them reaches the
// scene is the thing that was wrong.
TEST_CASE("D3 install: a distortion material builds both surfaces", "[d3][material][install]") {
    using ::whiteout::flakes::ProductId;
    namespace wio = ::whiteout::flakes::io;

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
        const char* file;     ///< `.app` under Appearances/
        const char* subObject;
        bool drawsToScene;    ///< false = phase 3 is its ONLY pass
        u32 distortionStages; ///< 0 = this sub-object declares no phase-3 pass
        bool twoTex;
    };
    const Want kWant[] = {
        // A blast wave: `distortion_2tex`, one pass, phase 3. Drawn into the
        // scene it is a normal-map sphere, which is exactly what the Wizard's
        // `p1_Wizard_archon_arcaneBlast_blastWave` was -- the same two geosets,
        // and this is the twin of it the extracted corpus carries.
        {"Enchantress_arcaneOrb_aoe_blastWave", "sphere2_mat", false, 2, true},
        // ...and its sibling geoset, an ordinary additive ring that must be
        // untouched by any of this.
        {"Enchantress_arcaneOrb_aoe_blastWave", "sphere1_mat", true, 0, false},
        // The Mystic Ally: a body pass AND a distortion pass over one material,
        // four stages, the caustic layer scrolling. The body must still draw.
        {"Monk_Male_mysticAlly", "mysticAlly_mat", true, 4, false},
        {"Monk_Male_mysticAlly", "mysticAlly_cloth", true, 4, false},
    };

    std::size_t checked = 0;
    for (const auto& w : kWant) {
        const auto path = CorpusRoot() / "Appearances" / (std::string(w.file) + ".app");
        const auto bytes = ReadAll(path);
        auto app = d3n::parseAppearances(bytes);
        if (!app) {
            WARN("missing " << path.string() << " -- SKIPPED, not passed.");
            continue;
        }
        auto adapter = flakes::io::D3ModelAdapter::LoadAppearance(
            ContentRef::FromPath(path.string()), bytes, cache);
        REQUIRE(adapter != nullptr);
        const auto textures =
            flakes::io::CollectD3Textures(adapter->SourceAppearance(), adapter->LookIndex(),
                                          adapter->EmittedSubObjects(), adapter->GeosetLooks());
        auto table = d3p::BuildD3SurfaceTable(adapter->SourceAppearance(), adapter->LookIndex(),
                                              textures, adapter->EmittedSubObjects(), &cache, {},
                                              nullptr);
        REQUIRE(table != nullptr);

        const auto emitted = adapter->EmittedSubObjects();
        const d3p::D3Surface* surface = nullptr;
        for (std::size_t g = 0; g < emitted.size(); ++g) {
            const d3n::GeoSet& set = (emitted[g].geoSet == 0)
                                         ? adapter->SourceAppearance().tGeoSet0
                                         : adapter->SourceAppearance().tGeoSet1;
            if (emitted[g].index < set.arSubObjects.size() &&
                EqualCiSv(set.arSubObjects[emitted[g].index].szName, w.subObject))
                surface = table->Surface(static_cast<u32>(g));
        }
        if (!surface || !surface->pass.resolved) {
            WARN(w.file << " / " << w.subObject << ": did not resolve -- SKIPPED.");
            continue;
        }
        ++checked;
        INFO(w.file << " / " << w.subObject);
        std::printf("[d3-distort] %-40s %-18s scene=%d dist=%u twoTex=%d\n", w.file, w.subObject,
                    (int)d3p::D3SurfaceDrawsToScene(*surface),
                    surface->distortion ? surface->distortion->chainCount : 0u,
                    surface->distortion ? (int)surface->distortion->pass.distortionTwoTex : 0);

        CHECK(d3p::D3SurfaceDrawsToScene(*surface) == w.drawsToScene);
        const flakes::renderer::core::SurfaceClass sc = d3p::D3ClassifySurface(*surface);
        // The two halves are independent: a surface can be invisible in the
        // scene and still owe the distortion buffer a draw. 26 of the 45
        // shipped distortion shaders are exactly that case.
        CHECK(sc.visible == w.drawsToScene);
        CHECK(sc.needsDistortion == (w.distortionStages > 0));

        if (w.distortionStages == 0) {
            CHECK(surface->distortion == nullptr);
            continue;
        }
        REQUIRE(surface->distortion != nullptr);
        const d3p::D3Surface& d = *surface->distortion;
        CHECK(d.pass.renderPhase == wio::kD3RenderPhaseDistortion);
        CHECK(d.pass.distortion);
        CHECK(d.pass.distortionTwoTex == w.twoTex);
        CHECK(d.chainCount == w.distortionStages);
        // The defect a chain with no textures produces is white, not nothing --
        // and in this buffer white is a saturated offset in both axes, which
        // drags the whole scene sideways. Every declared stage must bind.
        for (u32 i = 0; i < d.chainCount; ++i) {
            if (d.chain[i].rawType == 0)
                continue; // a hole binds nothing by construction
            INFO("distortion stage " << i << " type " << d.chain[i].rawType);
            CHECK(d.chain[i].textureId >= 0);
        }
        if (w.twoTex) {
            // Replace then add, and neither stage touches the alpha: the whole
            // of `out.rgb = tex0 + tex1 - 0.5; out.a = COLOR0.w`.
            CHECK(d.chain[0].colorOp == wio::kD3StageReplace);
            CHECK(d.chain[1].colorOp == wio::kD3StageAdd);
            CHECK(d.chain[0].alphaOp == wio::kD3StageSkip);
            CHECK(d.chain[1].alphaOp == wio::kD3StageSkip);
            CHECK(d.pass.alphaVcolFirst);
            CHECK_FALSE(d.pass.alphaVcolLast);
        }
    }
    if (checked == 0) {
        WARN("Nothing resolved. SKIPPED, not passed.");
        return;
    }
    CHECK(checked == 4);
}

// How far the phase reaches over the whole install, and the number the routing
// is accountable for: a distortion chain stage with no texture. Same shape as
// the `Legacy.fx` chain's own sweep, because it is the same defect one target
// over.
TEST_CASE("D3 install: the distortion phase over the corpus", "[d3][material][install]") {
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

    const auto files = FindFiles(CorpusRoot() / "Appearances", ".app");
    if (files.empty()) {
        WARN("No D3 corpus. SKIPPED, not passed.");
        return;
    }
    const std::size_t sweep = SweepLimit();
    const std::size_t limit = (sweep == 0) ? files.size() : (std::min)(sweep, files.size());
    std::size_t models = 0, withDistortion = 0, surfaces = 0, distortSurfaces = 0;
    std::size_t sceneDraws = 0, stages = 0, holes = 0, unresolved = 0, twoTex = 0;
    // A distortion stage whose UV transform ANIMATES names a palette entry, and
    // that entry is only ever filled by D3ModelAdapter::PublishUvAnimation --
    // which reaches a chain through D3ChainStageTypes, whose gate is
    // `Legacy.fx :: ps_legacy`. So a `ps_distortion2tex` stage could name an
    // entry nothing writes, and its scroll would silently stand still.
    std::size_t animStages = 0, animTwoTexStages = 0;
    // ...so the gate is that the publisher and the consumer agree, stage for
    // stage, on every distortion chain in the corpus.
    std::size_t chainsChecked = 0, chainMismatch = 0;
    std::vector<std::string> animTwoTexModels;
    for (std::size_t i = 0; i < limit; ++i) {
        const auto bytes = ReadAll(files[i]);
        auto app = d3n::parseAppearances(bytes);
        if (!app)
            continue;
        auto adapter = flakes::io::D3ModelAdapter::LoadAppearance(
            ContentRef::FromPath(files[i].string()), bytes, cache);
        if (!adapter)
            continue;
        ++models;
        const auto textures =
            flakes::io::CollectD3Textures(adapter->SourceAppearance(), adapter->LookIndex(),
                                          adapter->EmittedSubObjects(), adapter->GeosetLooks());
        auto table = d3p::BuildD3SurfaceTable(adapter->SourceAppearance(), adapter->LookIndex(),
                                              textures, adapter->EmittedSubObjects(), &cache, {},
                                              nullptr);
        if (!table)
            continue;
        // The emitted order IS the surface order (BuildD3SurfaceTable indexes by
        // it), which is what lets the publisher check below reach a surface's
        // own material.
        const auto emitted = adapter->EmittedSubObjects();
        bool any = false;
        std::size_t gi = 0;
        for (const auto& s : table->Surfaces()) {
            const std::size_t g = gi++;
            if (!s.valid)
                continue;
            ++surfaces;
            if (d3p::D3SurfaceDrawsToScene(s))
                ++sceneDraws;
            if (!s.distortion)
                continue;
            any = true;
            ++distortSurfaces;
            twoTex += s.distortion->pass.distortionTwoTex ? 1 : 0;
            for (u32 k = 0; k < s.distortion->chainCount; ++k) {
                const auto& st = s.distortion->chain[k];
                ++stages;
                if (st.rawType == 0) {
                    ++holes;
                    continue;
                }
                if (st.textureId < 0 && flakes::io::D3TypeOwnsTexture(st.rawType))
                    ++unresolved;
                if (st.uvTransformId >= 0) {
                    ++animStages;
                    if (s.distortion->pass.distortionTwoTex) {
                        ++animTwoTexStages;
                        if (animTwoTexModels.size() < 6)
                            animTwoTexModels.push_back(files[i].filename().string());
                    }
                }
            }
            // Publisher vs consumer. `D3ModelAdapter::PublishUvAnimation` fills
            // the UV palette by asking D3ChainStageTypes for the distortion
            // pass's stage types and matching them against the material's
            // texture entries by TYPE at position i; the surface table built
            // `chain[i]` from the same pass. If the two ever index differently,
            // an animated stage writes one palette entry and reads another —
            // which does not fail loudly, it just stops scrolling.
            if (s.distortion->chainCount > 0 && g < emitted.size()) {
                const d3n::GeoSet& set = (emitted[g].geoSet == 0)
                                             ? adapter->SourceAppearance().tGeoSet0
                                             : adapter->SourceAppearance().tGeoSet1;
                if (emitted[g].index < set.arSubObjects.size()) {
                    const auto& sub = set.arSubObjects[emitted[g].index];
                    // The PER-GEOSET look, which is what both the builder and
                    // D3ModelAdapter::PublishUvAnimation use. Reading the
                    // model-wide LookIndex here picks a different variant on a
                    // dressed actor, and a different variant is a different
                    // material.
                    const d3n::SubObjectAppearance* variant = flakes::io::D3VariantFor(
                        adapter->SourceAppearance(), sub, adapter->LookForGeoset(g));
                    std::shared_ptr<const d3n::Material> keepAlive;
                    const d3n::UberMaterial* mat =
                        variant ? flakes::io::D3MaterialOf(*variant, &cache, keepAlive) : nullptr;
                    if (mat) {
                        std::array<i32, d3p::kD3MaxChainStages> types{};
                        const u32 n = flakes::io::D3ChainStageTypes(*mat, &cache, types, true);
                        ++chainsChecked;
                        // Positions only, and only where the chain BOUND
                        // something. A stage whose declared type has no entry
                        // in the material stays a type-0 hole (28 in the
                        // corpus) -- the pass still declares its type, and the
                        // publisher still never emits for it, because it walks
                        // the material's entries and there is none.
                        bool bad = n < s.distortion->chainCount;
                        for (u32 k = 0; !bad && k < s.distortion->chainCount; ++k)
                            bad = s.distortion->chain[k].rawType != 0 &&
                                  types[k] != s.distortion->chain[k].rawType;
                        if (bad) {
                            ++chainMismatch;
                            std::string pub, got;
                            for (u32 k = 0; k < n; ++k)
                                pub += " " + std::to_string(types[k]);
                            for (u32 k = 0; k < s.distortion->chainCount; ++k)
                                got += " " + std::to_string(s.distortion->chain[k].rawType);
                            std::printf("[d3-distort] MISMATCH %s g=%zu %s :: published[%s ] chain[%s ]\n",
                                        files[i].filename().string().c_str(), g,
                                        s.distortion->pass.pixelEntry.c_str(), pub.c_str(),
                                        got.c_str());
                        }
                    }
                }
            }
        }
        withDistortion += any ? 1 : 0;
    }
    std::printf("[d3-distort] %zu models (%zu with a distortion pass), %zu surfaces "
                "(%zu distort, %zu scene); %zu stages: %zu holes, %zu unresolved, %zu twoTex, "
                "%zu animated (%zu of them twoTex); %zu chains checked, %zu mismatched\n",
                models, withDistortion, surfaces, distortSurfaces, sceneDraws, stages, holes,
                unresolved, twoTex, animStages, animTwoTexStages, chainsChecked,
                chainMismatch);
    for (const auto& name : animTwoTexModels)
        std::printf("[d3-distort]   animated ps_distortion2tex: %s\n", name.c_str());
    if (models == 0) {
        WARN("No models parsed. SKIPPED, not passed.");
        return;
    }
    CHECK(withDistortion > 0);
    CHECK(distortSurfaces > 0);
    // The gate. A stage whose type owns a texture and did not get one samples
    // white into a buffer whose neutral is grey -- a saturated offset in both
    // axes, which drags the entire scene sideways rather than shimmering.
    CHECK(unresolved == 0);
    // ...and that every distortion chain's animation can actually be published.
    // Non-zero here is a stage frozen at its authored phase, which looks like a
    // static shimmer and reports as nothing at all.
    CHECK(chainsChecked > 0);
    CHECK(chainMismatch == 0);
}
