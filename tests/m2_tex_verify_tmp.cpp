#include <catch2/catch_test_macros.hpp>

#include "io/file_content_provider.h"
#include "io/m2/m2_model_adapter.h"
#include "renderer/profiles/wow/wow_replaceable_textures.h"
#include "whiteout/flakes/content_ref.h"

#include <whiteout/models/m2/m2.h>

#include <cstdio>
#include <filesystem>
#include <span>
#include <string>

using whiteout::flakes::ContentRef;
namespace io = whiteout::flakes::io;
namespace wow = whiteout::flakes::renderer::profiles::wow;
namespace fs = std::filesystem;

TEST_CASE("probe: absolute-path model resolution", "[probe]") {
    io::FileContentProvider provider;
    provider.SetGame(whiteout::flakes::ProductId::Wow);
    provider.SetListfilePath(fs::path("C:/Tools/CASCExplorer/listfile.csv"));
    provider.SetTactKeyPath(fs::path("C:/Projects/WhiteoutLib/Corpus/tactkeys.txt"));

    std::printf("\nFileIdForPath probes:\n");
    const char* kProbe[] = {
        "C:/Projects/WhiteoutLib/Corpus/WoW/creature/revenantair/revenantair.m2",
        "Projects/WhiteoutLib/Corpus/WoW/creature/revenantair/revenantair.m2",
        "WoW/creature/revenantair/revenantair.m2",
        "creature/revenantair/revenantair.m2",
        "revenantair.m2",
    };
    for (const char* p : kProbe)
        std::printf("   %-72s -> %u\n", p, provider.FileIdForPath(p));

    wow::WowReplaceableTextures repl;
    repl.SetContentProvider(&provider);
    const char* kModels[] = {
        "C:/Projects/WhiteoutLib/Corpus/WoW/creature/revenantair/revenantair.m2",
        "C:/Projects/WhiteoutLib/Corpus/WoW/creature/sporebat3mountglowing/sporebat3mountglowing.m2",
    };
    for (const char* path : kModels) {
        auto bytes = provider.ReadFile(path);
        if (!bytes) {
            std::printf("\n### %s UNREADABLE\n", path);
            continue;
        }
        auto a = io::M2ModelAdapter::Load(
            ContentRef::FromPath(path),
            std::span<const whiteout::u8>(bytes->data(), bytes->size()), &provider);
        if (!a) {
            std::printf("\n### %s NO ADAPTER\n", path);
            continue;
        }
        const std::size_t bound = repl.Apply(*a, ContentRef::FromPath(path));
        const auto& vars = repl.Variations(ContentRef::FromPath(path));
        const auto& m = a->SourceModel();
        const auto tex = a->GetTextures();
        std::printf("\n### %s bound=%zu variations=%zu\n", path, bound, vars.size());
        for (std::size_t v = 0; v < vars.size() && v < 8; ++v) {
            std::printf("   var %zu %-24s:", v, vars[v].label.c_str());
            for (const auto& t : vars[v].texture)
                std::printf(" [%s]", t.c_str());
            std::printf("\n");
        }
        for (std::size_t i = 0; i < m.textures.size(); ++i) {
            if (m.textures[i].type == 0)
                continue;
            std::printf("   tex %2zu type=%2u key='%s'\n", i, m.textures[i].type,
                        i < tex.size() ? tex[i].sharedKey.c_str() : "");
        }
    }
}
