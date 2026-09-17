// ============================================================================
// The viewer's model-format table (tools/basic_viewer/documents/model_formats.h):
// what each extension is, which game it follows, and the Open dialog filters it
// produces for this build's WDX_ENABLE_* configuration.
// ============================================================================

#include "documents/model_formats.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace whiteout::flakes;

namespace {

const FileFilter* FilterNamed(const std::vector<FileFilter>& filters, const std::string& name) {
    for (const FileFilter& f : filters)
        if (f.name == name)
            return &f;
    return nullptr;
}

} // namespace

TEST_CASE("Extensions resolve case-insensitively to their kind and game", "[viewer][formats]") {
    struct Row {
        const char* path;
        ModelKind kind;
        ProductId game;
    };
    const Row rows[] = {
        {"units/footman.mdx", ModelKind::Wc3Model, ProductId::Wc3},
        {"C:/x/Footman.MDL", ModelKind::Wc3Model, ProductId::Wc3},
        {"fx.pkb", ModelKind::Effect, ProductId::Wc3},
        {"fx.PKFX", ModelKind::Effect, ProductId::Wc3},
        {"model.wem", ModelKind::Wem, ProductId::Neutral},
        {"model.gltf", ModelKind::Gltf, ProductId::Neutral},
        {"model.GLB", ModelKind::Gltf, ProductId::Neutral},
        {"creature.m2", ModelKind::ForeignModel, ProductId::Wow},
        {"Marine.M3", ModelKind::ForeignModel, ProductId::Sc2},
        {"Wizard_Female.app", ModelKind::D3Actor, ProductId::D3},
        {"x.acr", ModelKind::D3Actor, ProductId::D3},
    };
    for (const Row& r : rows) {
        INFO(r.path);
        const ModelFormat* f = FindModelFormat(r.path);
        REQUIRE(f != nullptr);
        CHECK(f->kind == r.kind);
        CHECK(f->game == r.game);
        CHECK(IsModelKind(r.path, r.kind));
    }
    CHECK(FindModelFormat("readme.txt") == nullptr);
    CHECK(FindModelFormat("noextension") == nullptr);
    CHECK(FindModelFormat("anims.m3a") == nullptr);
    CHECK(FindModelFormat(std::filesystem::path(u8"C:/\u5149/\u6a21\u578b.mdx")) != nullptr);
}

TEST_CASE("Open dialog filters follow the compiled formats", "[viewer][formats]") {
    const std::vector<FileFilter> filters = OpenDialogFilters();
    REQUIRE(!filters.empty());
    CHECK(filters.front().name == "All supported");

    const FileFilter* wc3 = FilterNamed(filters, "Warcraft III Model");
    REQUIRE(wc3 != nullptr);
    CHECK(wc3->extensions == "mdx,mdl");
    const FileFilter* effect = FilterNamed(filters, "PKB Effect");
    REQUIRE(effect != nullptr);
    CHECK(effect->extensions == "pkb,pkfx");
    // The startup picker once lacked this row (B4); both dialogs read this list.
    const FileFilter* wem = FilterNamed(filters, "WEM model");
    REQUIRE(wem != nullptr);
    CHECK(wem->extensions == "wem");

    std::string foreign;
#if WDX_ENABLE_M2
    foreign = "m2";
#endif
#if WDX_ENABLE_M3
    foreign += foreign.empty() ? "m3" : ",m3";
#endif
    const FileFilter* other = FilterNamed(filters, "Other Blizzard model");
    if (foreign.empty()) {
        CHECK(other == nullptr);
    } else {
        REQUIRE(other != nullptr);
        CHECK(other->extensions == foreign);
    }

    const FileFilter* d3 = FilterNamed(filters, "Diablo III model");
#if WDX_ENABLE_D3
    REQUIRE(d3 != nullptr);
    CHECK(d3->extensions == "acr,app");
#else
    CHECK(d3 == nullptr);
#endif

    std::string all = "mdx,mdl,pkb,pkfx,wem,gltf,glb";
    if (!foreign.empty())
        all += "," + foreign;
#if WDX_ENABLE_D3
    all += ",acr,app";
#endif
    CHECK(filters.front().extensions == all);
}

TEST_CASE("Every row names a build option exactly when it can be compiled out",
          "[viewer][formats]") {
    for (const ModelFormat& f : ModelFormats()) {
        INFO(std::string(f.extension));
        if (f.buildOption.empty())
            CHECK(f.compiled);
        CHECK(f.extension.size() > 1);
        CHECK(f.extension.front() == '.');
    }
}
