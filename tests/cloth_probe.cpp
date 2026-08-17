// Throwaway: what does clothMeshCount name when it is not a ClothSimulated region?
#include <catch2/catch_test_macros.hpp>

#include <whiteout/models/m3/parser.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace m3 = whiteout::m3;

namespace {

std::vector<whiteout::u8> ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool HasPhcl(const std::vector<whiteout::u8>& d) {
    static const char tag[4] = {'L', 'C', 'H', 'P'};
    return std::search(d.begin(), d.end(), tag, tag + 4) != d.end();
}

} // namespace

TEST_CASE("probe", "[probe]") {
    const fs::path root = "C:/Projects/WhiteoutLib/Corpus";
    std::size_t total = 0, agree = 0, noSimFlag = 0, multiSim = 0, oversize = 0, emptyArrays = 0;
    std::size_t maxSim = 0;
    std::map<std::size_t, std::size_t> simSizeHisto;
    std::vector<std::string> odd;

    for (const char* dir : {"HotSM3", "Sc2M3"}) {
        const fs::path d = root / dir;
        if (!fs::exists(d))
            continue;
        for (const auto& e : fs::directory_iterator(d)) {
            if (e.path().extension() != ".m3")
                continue;
            const auto bytes = ReadAll(e.path());
            if (bytes.empty() || !HasPhcl(bytes))
                continue;
            m3::Model model;
            try {
                m3::Parser parser;
                model = parser.parse(bytes);
            } catch (...) {
                continue;
            }
            if (model.clothPhysics.empty() || model.divisions.empty())
                continue;
            const auto& div = model.divisions[0];
            for (const auto& c : model.clothPhysics) {
                ++total;
                if (c.simEnabled.empty()) {
                    ++emptyArrays;
                    continue;
                }
                // Every region flagged ClothSimulated whose vertex count matches
                // this chunk's per-vertex arrays.
                std::vector<std::size_t> byFlag, byLen;
                for (std::size_t r = 0; r < div.regions.size(); ++r) {
                    const auto f = static_cast<whiteout::u32>(div.regions[r].flags);
                    if ((f & 0x4u) != 0u)
                        byFlag.push_back(r);
                    if (div.regions[r].vertexCount == c.simEnabled.size())
                        byLen.push_back(r);
                }
                const bool named = c.clothMeshCount < div.regions.size() &&
                                   div.regions[c.clothMeshCount].vertexCount ==
                                       c.simEnabled.size();
                if (named)
                    ++agree;
                if (byFlag.empty())
                    ++noSimFlag;
                if (byFlag.size() > 1)
                    ++multiSim;
                maxSim = (std::max)(maxSim, c.simEnabled.size());
                simSizeHisto[(c.simEnabled.size() + 63) / 64]++;
                if (c.simEnabled.size() > 256)
                    ++oversize;
                if (!named && odd.size() < 18) {
                    char buf[400];
                    std::snprintf(buf, sizeof(buf),
                                  "%s clothMeshCount=%u sim=%zu regions=%zu byFlag=%zu byLen=%zu "
                                  "proxyClothIdx=%u divs=%zu",
                                  e.path().filename().string().c_str(), c.clothMeshCount,
                                  c.simEnabled.size(), div.regions.size(), byFlag.size(),
                                  byLen.size(),
                                  c.proxies.empty() ? 9999u : c.proxies[0].clothIndex,
                                  model.divisions.size());
                    odd.emplace_back(buf);
                }
            }
        }
    }
    std::printf("cloths=%zu clothMeshCountNamesTheRegion=%zu noSimFlag=%zu multiSimFlag=%zu "
                "emptyArrays=%zu oversize(>256)=%zu maxSim=%zu\n",
                total, agree, noSimFlag, multiSim, emptyArrays, oversize, maxSim);
    std::printf("sim-vertex histogram (bucket = 64 verts):\n");
    for (const auto& [k, v] : simSizeHisto)
        std::printf("  <=%4zu  %zu\n", k * 64, v);
    for (const auto& s : odd)
        std::printf("  ? %s\n", s.c_str());
}
