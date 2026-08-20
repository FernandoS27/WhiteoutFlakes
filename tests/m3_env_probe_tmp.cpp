// TEMPORARY probe — env layer measurement. Delete before commit.
#include <cstring>
#include <catch2/catch_test_macros.hpp>

#include "io/m3/m3_model_adapter.h"
#include "whiteout/flakes/content_ref.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

using whiteout::flakes::ContentRef;
namespace io = whiteout::flakes::io;
namespace fs = std::filesystem;
namespace m3 = whiteout::m3;

namespace {
fs::path CorpusRoot() {
    if (const char* v = std::getenv("WDX_TEST_SC2_CORPUS"); v && *v) return fs::path(v);
    return fs::path("C:/Projects/WhiteoutLib/Corpus");
}
constexpr const char* kCorpora[] = {"Sc2M3", "Sc2BetaM3", "StarM3", "HotSM3"};
std::vector<fs::path> FindModels(const fs::path& dir) {
    std::vector<fs::path> out; std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        auto ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)std::tolower(c); });
        if (ext == ".m3") out.push_back(it->path());
    }
    std::sort(out.begin(), out.end());
    return out;
}
std::vector<whiteout::u8> ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<whiteout::u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::string Clean(std::string s) { while (!s.empty() && s.back() == '\0') s.pop_back(); return s; }
} // namespace

TEST_CASE("env probe", "[envprobe]") {
    const fs::path root = CorpusRoot();
    std::size_t limit = 400;
    if (const char* v = std::getenv("WDX_TEST_M3_LIMIT"); v && *v) limit = std::strtoul(v, nullptr, 10);

    std::size_t mats = 0, withEnv = 0, withMask = 0;
    std::map<int, std::size_t> uvModes, maskUvModes;
    std::map<int, std::size_t> ops;
    std::map<std::string, std::size_t> paths;
    std::map<int, std::size_t> chan;
    std::size_t constNonZero = 0, diffNonZero = 0, specNonZero = 0, v20 = 0;
    std::map<std::string, std::size_t> modelsWithEnv;

    for (const char* name : kCorpora) {
        auto models = FindModels(root / name);
        if (limit > 0 && models.size() > limit) models.resize(limit);
        for (const auto& path : models) {
            const auto bytes = ReadAll(path);
            if (bytes.empty()) continue;
            auto adapter = io::M3ModelAdapter::Load(ContentRef::FromPath(path.string()), std::span<const whiteout::u8>(bytes));
            if (!adapter) continue;
            const auto& model = adapter->SourceModel();
            for (const auto& mat : model.standardMaterials) {
                ++mats;
                if (mat.getVersion() >= 20) {
                    ++v20;
                    if (mat.hdrEnvironmentConstant != 0.0f) ++constNonZero;
                    if (mat.hdrEnvironmentDiffuse != 0.0f) ++diffNonZero;
                    if (mat.hdrEnvironmentSpecular != 0.0f) ++specNonZero;
                }
                if (mat.environmentLayer && !Clean(mat.environmentLayer->texturePath).empty()) {
                    ++withEnv;
                    uvModes[(int)mat.environmentLayer->uvMapping]++;
                    chan[(int)mat.environmentLayer->colorType]++;
                    ops[(int)mat.layerBlendMode]++;
                    auto p = Clean(mat.environmentLayer->texturePath);
                    std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c){ return (char)std::tolower(c); });
                    paths[p]++;
                    modelsWithEnv[path.filename().string()]++;
                }
                if (mat.environmentMaskLayer && !Clean(mat.environmentMaskLayer->texturePath).empty()) {
                    ++withMask;
                    maskUvModes[(int)mat.environmentMaskLayer->uvMapping]++;
                }
            }
        }
    }
    std::printf("\n== ENV PROBE ==\nmaterials=%zu v20=%zu constNZ=%zu diffNZ=%zu specNZ=%zu\n",
                mats, v20, constNonZero, diffNonZero, specNonZero);
    std::printf("withEnv=%zu withMask=%zu\n", withEnv, withMask);
    std::printf("env uvMapping:"); for (auto& [k,v] : uvModes) std::printf(" %d=%zu", k, v); std::printf("\n");
    std::printf("mask uvMapping:"); for (auto& [k,v] : maskUvModes) std::printf(" %d=%zu", k, v); std::printf("\n");
    std::printf("env channelSelect:"); for (auto& [k,v] : chan) std::printf(" %d=%zu", k, v); std::printf("\n");
    std::printf("material layerBlendMode (env op):"); for (auto& [k,v] : ops) std::printf(" %d=%zu", k, v); std::printf("\n");
    std::printf("distinct env texture paths=%zu\n", paths.size());
    for (auto& [k,v] : paths) std::printf("  %4zu  %s\n", v, k.c_str());
    std::printf("models with env (%zu):", modelsWithEnv.size());
    { std::size_t n = 0; for (auto& [k,v] : modelsWithEnv) { if (n++ >= 25) break; std::printf(" %s", k.c_str()); } }
    std::printf("\n");
    REQUIRE(true);
}

#include "io/file_content_provider.h"

TEST_CASE("env dds probe", "[envdds]") {
    using whiteout::flakes::io::FileContentProvider;
    using whiteout::flakes::ProductId;
    FileContentProvider p;
    if (p.GamePath(ProductId::Sc2).empty()) SKIP("no SC2");
    p.SetGame(ProductId::Sc2);
    REQUIRE(p.HasCasc());

    static const char* kNames[] = {
        "assets/textures/gold_reflection.dds",
        "assets/textures/thrall_reflection.dds",
        "assets/textures/arthas_reflection.dds",
        "assets/textures/gold_reflection_6.dds",
        "assets/textures/reflection_obsidian.dds",
        "assets/textures/silver_reflection.dds",
        "assets/textures/reflection_silver.dds",
        "assets/textures/cubemap_aiur01.dds",
        "assets/textures/storm_hexcubemap_alpha.dds",
        "assets/textures/storm_blurredcube_generic.dds",
        "assets/textures/storm_simple_bronze_sphericalreflection_red_broad.dds",
        "assets/textures/storm_simpleeyesphericalreflection.dds",
        "assets/textures/storm_sharedreflection_generic.dds",
        "assets/textures/pbrreflect_lastimpact_cubic_envio.dds",
        "assets/textures/eyereflect_01.dds",
        "assets/textures/portraitwarbot_env.dds",
        "assets/textures/envirotest2.dds",
        "assets/textures/belshircubicmaporiginal_green.dds",
    };
    std::printf("\n== ENV DDS ==\n");
    for (const char* n : kNames) {
        auto b = p.ReadFile(n);
        if (!b || b->size() < 128) { std::printf("  MISSING  %s\n", n); continue; }
        const whiteout::u8* d = b->data();
        auto u32at = [&](std::size_t o) { whiteout::u32 v; std::memcpy(&v, d + o, 4); return v; };
        const whiteout::u32 h = u32at(12), w = u32at(16), mips = u32at(28);
        const whiteout::u32 fourcc = u32at(84), caps2 = u32at(112);
        char fc[5] = {(char)(fourcc & 0xff), (char)((fourcc >> 8) & 0xff),
                      (char)((fourcc >> 16) & 0xff), (char)((fourcc >> 24) & 0xff), 0};
        std::printf("  %-62s %4ux%-4u mips=%2u cc=%-4s caps2=0x%04x %s\n", n, w, h, mips, fc,
                    caps2, (caps2 & 0x200) ? "CUBE" : "2D");
    }
}
