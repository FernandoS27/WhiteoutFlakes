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
//  3. **The (`dwSlotIndex`, `dwTextureType`) census.** EMaterialTextureType's
//     authored names were stripped from the build; every branch of
//     Render_ResolveMaterialTextureStages names itself through the core asset it
//     falls back to. What the census adds is which of those branches carry
//     *this material's* texture: `dwTextureType == 0` (the default branch) does,
//     and the named types arrive as a model-wide block at slots 25..38 that is
//     byte-identical across every material in a file. Printed so the day that
//     block is understood the numbers are already in front of whoever reads it.
//
// Corpus root: WDX_TEST_D3_CORPUS, default C:/Projects/WhiteoutLib/Corpus/D3.
// Skipped is not passed.
// ============================================================================

#include <catch2/catch_test_macros.hpp>

#include "io/d3/d3_model_adapter.h"
#include "renderer/profiles/diablo3/d3_surface_table.h"

#include <whiteout/sno/d3/native/d3_native.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
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
                        if (e.dwTextureType == 0)
                            ++n0;
                    ++type0PerVariant[n0];
                }
                if (v.tMaterial.arTextures.size() > d3p::kD3MaxTextureStages)
                    pastStageCap += v.tMaterial.arTextures.size() - d3p::kD3MaxTextureStages;
                for (const auto& e : v.tMaterial.arTextures) {
                    ++textureEntries;
                    ++typeCounts[e.dwTextureType];
                    ++slotCounts[e.dwSlotIndex];
                    ++pairCounts[{e.dwSlotIndex, e.dwTextureType}];
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
    std::printf("[d3-mat] dwTextureType census:\n");
    for (const auto& [type, n] : typeCounts)
        std::printf("[d3-mat]   %4d  x%-8zu  %s\n", type, n, NameOfType(type));
    std::printf("[d3-mat] JOIN: AppearanceMaterial.szName matches SubObject.szName %zu times, "
                "SubObject.szMaterialName %zu times, of %zu sub-objects\n",
                byName, byMaterialName, subObjects);
    std::printf("[d3-mat] (dwSlotIndex, dwTextureType) pairs:\n");
    for (const auto& [k, n] : pairCounts)
        std::printf("[d3-mat]   slot %-3d type %-3d x%zu\n", k.first, k.second, n);
    std::printf("[d3-mat] entries with dwTextureType == 0, per variant:\n");
    for (const auto& [n0, n] : type0PerVariant)
        std::printf("[d3-mat]   %d x%zu\n", n0, n);
    std::printf("[d3-mat] dwSlotIndex census:\n");
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
                std::printf("[diag]     slotIndex=%-3d type=%-3d tex=%-7d flags=0x%X unk04=%d "
                            "unk9C=%d\n",
                            e.dwSlotIndex, e.dwTextureType, e.snoTexture.id, e.dwTextureFlags,
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
