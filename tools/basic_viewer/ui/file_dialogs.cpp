#include "ui/file_dialogs.h"

#include "documents/model_formats.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <nfd.hpp>

#include <vector>

namespace whiteout::flakes {

std::optional<std::filesystem::path> PickModelToOpen() {
    const std::vector<FileFilter> filters = OpenDialogFilters();
    std::vector<nfdu8filteritem_t> items;
    items.reserve(filters.size());
    for (const FileFilter& f : filters)
        items.push_back({f.name.c_str(), f.extensions.c_str()});
    // The UTF-8 entry points keep the specs plain `char` on every platform.
    NFD::UniquePathU8 outPath;
    if (NFD::OpenDialog(outPath, items.data(), static_cast<nfdfiltersize_t>(items.size())) !=
        NFD_OKAY)
        return std::nullopt;
    return io::FsPathFromUtf8(outPath.get());
}

} // namespace whiteout::flakes
