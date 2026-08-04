#pragma once

// CascBrowser — opens a CASC archive and presents its model/effect files as a
// navigable folder tree. The WC3 Reforged TVFS encodes the mod chain with ':'
// and the real content tree with '\' (e.g.
// "war3.w3mod:_hd.w3mod:units\nightelf\druid\druid.mdx"). To make that easy to
// browse, on open we enumerate every .mdx/.mdl/.pkb/.pkfx, strip the leading
// "war3.w3mod:" mod prefix, treat ':' as a folder separator like '\', and build
// a tree of only the folders that lead to models. Each file leaf remembers its
// ORIGINAL archive path so the content provider can still read it verbatim.

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <whiteout/storages/casc/storage.h>
#if WHITEOUT_HAS_MPQ
#include <whiteout/storages/mpq/storage.h>
#endif

namespace whiteout::flakes::io {

// Where a browser's entries come from.
enum class StorageKind {
    Casc,   // an installed game's CASC storage
    Mpq,    // a single .mpq / .w3x / .w3m archive
    Folder, // a directory on disk, walked recursively
};

class StorageBrowser {
public:
    struct Listing {
        std::vector<std::string> folders;    // immediate subfolder names
        std::vector<std::string> modelFiles; // model/effect file names in this folder
    };

    // Open `root` as `kind` and build the folder tree. For Casc, `root` is
    // the directory holding .build.info (or its Data subdir); for Mpq, the
    // archive file; for Folder, the directory to walk. Returns false and
    // fills `error` on failure.
    bool Open(const std::string& root, StorageKind kind, std::string* error);

    // Guess the kind from what is actually at `path` — a directory holding
    // .build.info is a CASC install, a file is an archive, any other
    // directory is browsed as a folder — then open it. Convenience for a
    // dialog that just received a drop or a path from the user.
    bool OpenAuto(const std::string& path, std::string* error);

    bool IsOpen() const {
        return open_;
    }
    StorageKind Kind() const {
        return kind_;
    }
    const std::string& Root() const {
        return root_;
    }

    // Current directory in display form ('\\'-separated, "" at the root).
    const std::string& CurrentPath() const {
        return currentPath_;
    }
    const std::vector<std::string>& Breadcrumb() const {
        return breadcrumb_;
    }
    const Listing& Current() const {
        return listing_;
    }

    void Descend(const std::string& folderName);
    void Ascend();
    void NavigateTo(const std::string& displayPath);

    // Original archive path (native ':' / '\\' separators) for a model file in
    // the current folder — what the content provider reads. Empty if unknown.
    std::string ChildPath(const std::string& fileName) const;

private:
    // A folder node: subfolders + the model files directly inside it (display
    // name → original archive path).
    struct Node {
        std::map<std::string, Node> folders;        // key = lowercase name
        std::map<std::string, std::string> folderDisplay; // lowercase → display
        std::map<std::string, std::string> files;   // display name → archive path
    };

    void Refresh();
    const Node* NodeAt(const std::string& displayPath) const;
    // Insert one entry into the tree: `original` is what a provider reads,
    // `display` is what the user navigates.
    void Insert(const std::string& original, const std::string& display);

    bool OpenCasc(const std::string& root, std::string* error);
    bool OpenMpq(const std::string& path, std::string* error);
    bool OpenFolder(const std::string& path, std::string* error);

    bool open_ = false;
    StorageKind kind_ = StorageKind::Casc;
    // Kept alive only for CASC: enumerate() borrows the storage. MPQ and
    // Folder are read once into the tree and need nothing retained.
    std::optional<storages::casc::Storage> storage_;
    std::string root_;
    std::string currentPath_; // display form, '\\'-separated
    std::vector<std::string> breadcrumb_;
    Listing listing_;
    Node tree_;
};

} // namespace whiteout::flakes::io
