#include "documents/model_formats.h"

#include "string_util.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <array>

namespace whiteout::flakes {

namespace {

#if WDX_ENABLE_M2
constexpr bool kM2 = true;
#else
constexpr bool kM2 = false;
#endif
#if WDX_ENABLE_M3
constexpr bool kM3 = true;
#else
constexpr bool kM3 = false;
#endif
#if WDX_ENABLE_D3
constexpr bool kD3 = true;
#else
constexpr bool kD3 = false;
#endif

constexpr std::string_view kWc3Group = "Warcraft III Model";
constexpr std::string_view kEffectGroup = "PKB Effect";
constexpr std::string_view kWemGroup = "WEM model";
constexpr std::string_view kForeignGroup = "Other Blizzard model";
constexpr std::string_view kD3Group = "Diablo III model";

// Order is the order the extensions appear in "All supported".
constexpr std::array kFormats = {
    ModelFormat{".mdx", ModelKind::Wc3Model, ProductId::Wc3, kWc3Group, {}, true},
    ModelFormat{".mdl", ModelKind::Wc3Model, ProductId::Wc3, kWc3Group, {}, true},
    ModelFormat{".pkb", ModelKind::Effect, ProductId::Wc3, kEffectGroup, {}, true},
    ModelFormat{".pkfx", ModelKind::Effect, ProductId::Wc3, kEffectGroup, {}, true},
    // WEM and glTF are in the library whatever this build enables; which
    // profiles one file opens as is the file's answer at open time.
    ModelFormat{".wem", ModelKind::Wem, ProductId::Neutral, kWemGroup, {}, true},
    ModelFormat{".gltf", ModelKind::Gltf, ProductId::Neutral, {}, {}, true},
    ModelFormat{".glb", ModelKind::Gltf, ProductId::Neutral, {}, {}, true},
    ModelFormat{".m2", ModelKind::ForeignModel, ProductId::Wow, kForeignGroup, "WDX_ENABLE_M2", kM2},
    ModelFormat{".m3", ModelKind::ForeignModel, ProductId::Sc2, kForeignGroup, "WDX_ENABLE_M3", kM3},
    ModelFormat{".acr", ModelKind::D3Actor, ProductId::D3, kD3Group, "WDX_ENABLE_D3", kD3},
    ModelFormat{".app", ModelKind::D3Actor, ProductId::D3, kD3Group, "WDX_ENABLE_D3", kD3},
};

} // namespace

std::span<const ModelFormat> ModelFormats() {
    return kFormats;
}

const ModelFormat* FindModelFormat(const std::filesystem::path& path) {
    const std::string ext = io::PathToUtf8(path.extension());
    for (const ModelFormat& f : kFormats)
        if (tools::EqualsIgnoreCase(ext, f.extension))
            return &f;
    return nullptr;
}

std::vector<FileFilter> OpenDialogFilters() {
    std::vector<FileFilter> filters;
    filters.push_back({"All supported", {}});
    for (const ModelFormat& f : kFormats) {
        if (!f.compiled)
            continue;
        const std::string_view bare = f.extension.substr(1);
        std::string& all = filters.front().extensions;
        if (!all.empty())
            all += ',';
        all += bare;
        if (f.filterGroup.empty())
            continue;
        FileFilter* group = nullptr;
        for (FileFilter& existing : filters)
            if (existing.name == f.filterGroup)
                group = &existing;
        if (!group)
            group = &filters.emplace_back(FileFilter{std::string(f.filterGroup), {}});
        if (!group->extensions.empty())
            group->extensions += ',';
        group->extensions += bare;
    }
    return filters;
}

} // namespace whiteout::flakes
