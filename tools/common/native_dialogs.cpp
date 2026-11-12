#include "native_dialogs.h"

#include "whiteout/flakes/util/path_utf8.h"

#include <nfd.hpp>

#include <cstdio>

// NFD API: https://github.com/btzy/nativefiledialog-extended#usage
//
// TODO: pass the host window as parentWindow (nfd_glfw3.h) so dialogs stay
// attached and modal. Deferred: NFD's portal backend only speaks xdg-foreign
// v1, which KWin no longer offers, so on Plasma Wayland it would be a no-op.

namespace whiteout::flakes::tools {

namespace {

const char* LastError() {
    const char* err = NFD::GetError();
    return err ? err : "unknown error";
}

bool Ready() {
    static const bool ready = [] {
        if (NFD::Init() == NFD_OKAY)
            return true;
        std::fprintf(stderr, "[dialogs] native file dialogs unavailable: %s\n", LastError());
        return false;
    }();
    if (!ready)
        std::fprintf(stderr, "[dialogs] file dialog skipped: native dialogs failed to initialise\n");
    return ready;
}

bool Picked(nfdresult_t result) {
    if (result == NFD_ERROR)
        std::fprintf(stderr, "[dialogs] file dialog failed: %s\n", LastError());
    return result == NFD_OKAY;
}

nfdfiltersize_t Count(std::span<const nfdu8filteritem_t> filters) {
    return static_cast<nfdfiltersize_t>(filters.size());
}

} // namespace

std::optional<std::filesystem::path> OpenFileDialog(std::span<const nfdu8filteritem_t> filters) {
    if (!Ready())
        return std::nullopt;
    NFD::UniquePathU8 out;
    if (!Picked(NFD::OpenDialog(out, filters.data(), Count(filters))))
        return std::nullopt;
    return io::FsPathFromUtf8(out.get());
}

std::vector<std::filesystem::path> OpenFilesDialog(std::span<const nfdu8filteritem_t> filters) {
    std::vector<std::filesystem::path> paths;
    if (!Ready())
        return paths;
    NFD::UniquePathSet set;
    if (!Picked(NFD::OpenDialogMultiple(set, filters.data(), Count(filters))))
        return paths;
    nfdpathsetsize_t count = 0;
    if (!Picked(NFD::PathSet::Count(set, count)))
        return paths;
    for (nfdpathsetsize_t i = 0; i < count; ++i) {
        NFD::UniquePathSetPathU8 path;
        if (Picked(NFD::PathSet::GetPath(set, i, path)))
            paths.push_back(io::FsPathFromUtf8(path.get()));
    }
    return paths;
}

std::optional<std::filesystem::path> SaveFileDialog(std::span<const nfdu8filteritem_t> filters,
                                                    const char* defaultName) {
    if (!Ready())
        return std::nullopt;
    NFD::UniquePathU8 out;
    if (!Picked(NFD::SaveDialog(out, filters.data(), Count(filters), nullptr, defaultName)))
        return std::nullopt;
    return io::FsPathFromUtf8(out.get());
}

std::optional<std::filesystem::path> PickFolderDialog() {
    if (!Ready())
        return std::nullopt;
    NFD::UniquePathU8 out;
    if (!Picked(NFD::PickFolder(out)))
        return std::nullopt;
    return io::FsPathFromUtf8(out.get());
}

} // namespace whiteout::flakes::tools
