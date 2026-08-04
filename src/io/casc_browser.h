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

namespace whiteout::flakes::io {

class CascBrowser {
public:
    struct Listing {
        std::vector<std::string> folders;    // immediate subfolder names
        std::vector<std::string> modelFiles; // model/effect file names in this folder
    };

    // Opens the CASC at `root` (the dir containing .build.info, or its Data
    // subdir). Returns false and fills `error` on failure. Builds the model
    // folder tree and sets the current path to the root.
    bool Open(const std::string& root, std::string* error);
    bool IsOpen() const {
        return storage_.has_value();
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

    std::optional<storages::casc::Storage> storage_;
    std::string root_;
    std::string currentPath_; // display form, '\\'-separated
    std::vector<std::string> breadcrumb_;
    Listing listing_;
    Node tree_;
};

} // namespace whiteout::flakes::io
