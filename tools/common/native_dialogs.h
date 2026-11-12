#pragma once

// ============================================================================
// The one way tools pop a native file dialog (nativefiledialog-extended).
//
// NFD is initialised lazily on the first dialog and only once. A failed init —
// no session bus, no portal — is logged, and later dialogs return "nothing
// picked" without calling into NFD: on the xdg-desktop-portal backend, a
// dialog over a failed init aborts inside libdbus rather than returning an
// error. A real dialog error (NFD_ERROR) is logged too; Cancel stays silent.
// Logs go to stderr, which the Log Console captures.
//
// Filters are NFD's own `{name, "ext1,ext2"}` pairs. Main thread only.
// ============================================================================

#include <nfd.h>

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace whiteout::flakes::tools {

std::optional<std::filesystem::path> OpenFileDialog(std::span<const nfdu8filteritem_t> filters = {});

/// Empty when cancelled.
std::vector<std::filesystem::path> OpenFilesDialog(std::span<const nfdu8filteritem_t> filters = {});

std::optional<std::filesystem::path> SaveFileDialog(std::span<const nfdu8filteritem_t> filters = {},
                                                    const char* defaultName = nullptr);

std::optional<std::filesystem::path> PickFolderDialog();

} // namespace whiteout::flakes::tools
