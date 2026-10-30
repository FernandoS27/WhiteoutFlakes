#include "ui/file_dialogs.h"

#include "documents/model_formats.h"
#include "native_dialogs.h"

#include <vector>

namespace whiteout::flakes {

std::optional<std::filesystem::path> PickModelToOpen() {
    const std::vector<FileFilter> filters = OpenDialogFilters();
    std::vector<nfdu8filteritem_t> items;
    items.reserve(filters.size());
    for (const FileFilter& f : filters)
        items.push_back({f.name.c_str(), f.extensions.c_str()});
    // The UTF-8 entry points keep the specs plain `char` on every platform.
    return tools::OpenFileDialog(items);
}

} // namespace whiteout::flakes
