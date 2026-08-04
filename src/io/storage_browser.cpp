#include "io/storage_browser.h"

#include <filesystem>
#include <system_error>

#include <algorithm>
#include <cctype>

namespace whiteout::flakes::io {

namespace {

std::string ToLower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool IsModelOrEffect(std::string_view name) {
    const auto dot = name.find_last_of('.');
    if (dot == std::string_view::npos)
        return false;
    std::string ext = ToLower(std::string(name.substr(dot + 1)));
    return ext == "mdx" || ext == "mdl" || ext == "pkb" || ext == "pkfx";
}

// Display form of an archive path: drop the leading "war3.w3mod:" mod prefix
// and treat ':' as a folder separator like '\'.
// CASC only: drop the mod prefix and fold ':' into the separator.
std::string CascToDisplay(std::string_view archivePath) {
    std::string p(archivePath);
    constexpr std::string_view kRoot = "war3.w3mod:";
    if (ToLower(p).rfind(std::string(kRoot), 0) == 0)
        p = p.substr(kRoot.size());
    for (char& c : p)
        if (c == ':' || c == '/')
            c = '\\';
    return p;
}

void SplitSegments(const std::string& path, std::vector<std::string>& out) {
    out.clear();
    std::string seg;
    for (char c : path) {
        if (c == '\\' || c == '/') {
            if (!seg.empty()) {
                out.push_back(seg);
                seg.clear();
            }
        } else {
            seg.push_back(c);
        }
    }
    if (!seg.empty())
        out.push_back(seg);
}

std::string JoinSegments(const std::vector<std::string>& segs) {
    std::string out;
    for (const auto& s : segs) {
        if (!out.empty())
            out.push_back('\\');
        out += s;
    }
    return out;
}

} // namespace

bool StorageBrowser::Open(const std::string& root, StorageKind kind, std::string* error) {
    open_ = false;
    kind_ = kind;
    storage_.reset();
    tree_ = Node{};
    currentPath_.clear();

    bool ok = false;
    switch (kind) {
    case StorageKind::Casc:
        ok = OpenCasc(root, error);
        break;
    case StorageKind::Mpq:
        ok = OpenMpq(root, error);
        break;
    case StorageKind::Folder:
        ok = OpenFolder(root, error);
        break;
    }
    if (!ok)
        return false;

    open_ = true;
    Refresh();
    return true;
}

bool StorageBrowser::OpenAuto(const std::string& path, std::string* error) {
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::path(path);
    if (std::filesystem::is_directory(p, ec)) {
        // A CASC install is a directory too, so the marker file decides.
        // Checking Data/ as well matches what OpenCasc itself accepts.
        const bool casc = std::filesystem::exists(p / ".build.info", ec) ||
                          std::filesystem::exists(p / "Data" / ".build.info", ec);
        return Open(path, casc ? StorageKind::Casc : StorageKind::Folder, error);
    }
    if (std::filesystem::is_regular_file(p, ec))
        return Open(path, StorageKind::Mpq, error);
    if (error)
        *error = "no such file or directory: " + path;
    return false;
}

void StorageBrowser::Insert(const std::string& original, const std::string& display) {
    std::vector<std::string> segs;
    SplitSegments(display, segs);
    if (segs.empty())
        return;
    Node* node = &tree_;
    for (size_t i = 0; i + 1 < segs.size(); ++i) {
        const std::string key = ToLower(segs[i]);
        node->folderDisplay.emplace(key, segs[i]);
        node = &node->folders[key];
    }
    node->files.emplace(segs.back(), original);
}

bool StorageBrowser::OpenCasc(const std::string& root, std::string* error) {
    std::string err;
    std::optional<storages::casc::Storage> s = storages::casc::Storage::open(root, &err);
    std::string usedRoot = root;
    if (!s) {
        std::string err2;
        s = storages::casc::Storage::open(root + "/Data", &err2);
        if (s)
            usedRoot = root + "/Data";
        else {
            if (error)
                *error = err.empty() ? err2 : err;
            return false;
        }
    }
    storage_ = std::move(*s);
    root_ = usedRoot;

    storage_->enumerate([this](const storages::casc::EnumerateEntry& e) {
        if (IsModelOrEffect(e.path))
            Insert(std::string(e.path), CascToDisplay(e.path));
        return true;
    });
    return true;
}

bool StorageBrowser::OpenMpq(const std::string& path, std::string* error) {
#if WHITEOUT_HAS_MPQ
    std::string err;
    std::optional<storages::mpq::Storage> s = storages::mpq::Storage::open(path, &err);
    if (!s) {
        if (error)
            *error = err.empty() ? ("could not open MPQ: " + path) : err;
        return false;
    }
    root_ = path;
    // MPQ paths are already ''-separated and carry no mod prefix, so the
    // display form is the stored form.
    for (const auto& name : s->listFiles()) {
        if (IsModelOrEffect(name))
            Insert(name, name);
    }
    return true;
#else
    (void)path;
    if (error)
        *error = "this build has no MPQ support (enable the `mpq` feature)";
    return false;
#endif
}

bool StorageBrowser::OpenFolder(const std::string& path, std::string* error) {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::path(path);
    if (!std::filesystem::is_directory(base, ec)) {
        if (error)
            *error = "not a directory: " + path;
        return false;
    }
    root_ = path;

    // Skip-on-error so one unreadable subdirectory does not abort the walk —
    // a system folder the user pointed at may well contain some.
    std::filesystem::recursive_directory_iterator it(
        base, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) {
        if (error)
            *error = "could not walk " + path + ": " + ec.message();
        return false;
    }
    // One bad entry must not cost the whole tree. The per-entry work is
    // guarded because path::string() throws on a name this platform's narrow
    // encoding cannot represent, and the walk is advanced with the
    // non-throwing overload — a range-for would throw out of operator++.
    const std::filesystem::recursive_directory_iterator end;
    while (it != end) {
        try {
            const std::filesystem::directory_entry& entry = *it;
            std::error_code fe;
            if (entry.is_regular_file(fe) && !fe) {
                const std::string name = entry.path().filename().string();
                if (IsModelOrEffect(name)) {
                    // Display relative to the root, with the tree's separator;
                    // the original stays absolute so a provider can open it
                    // directly.
                    std::string rel = std::filesystem::relative(entry.path(), base, fe).string();
                    if (!fe && !rel.empty()) {
                        for (char& c : rel)
                            if (c == '/')
                                c = '\\';
                        Insert(entry.path().string(), rel);
                    }
                }
            }
        } catch (...) {
            // Unreadable or unrepresentable: skip it and keep walking.
        }
        it.increment(ec);
        if (ec) {
            // The iterator cannot say where it stopped, so continuing risks
            // looping on the same failure. Keep what was found.
            ec.clear();
            break;
        }
    }
    return true;
}

const StorageBrowser::Node* StorageBrowser::NodeAt(const std::string& displayPath) const {
    std::vector<std::string> segs;
    SplitSegments(displayPath, segs);
    const Node* node = &tree_;
    for (const auto& s : segs) {
        auto it = node->folders.find(ToLower(s));
        if (it == node->folders.end())
            return nullptr;
        node = &it->second;
    }
    return node;
}

void StorageBrowser::Refresh() {
    listing_.folders.clear();
    listing_.modelFiles.clear();
    const Node* node = NodeAt(currentPath_);
    if (node) {
        for (const auto& [key, disp] : node->folderDisplay)
            listing_.folders.push_back(disp);
        for (const auto& [name, orig] : node->files)
            listing_.modelFiles.push_back(name);
        auto ci = [](const std::string& a, const std::string& b) { return ToLower(a) < ToLower(b); };
        std::sort(listing_.folders.begin(), listing_.folders.end(), ci);
        std::sort(listing_.modelFiles.begin(), listing_.modelFiles.end(), ci);
    }
    SplitSegments(currentPath_, breadcrumb_);
}

void StorageBrowser::Descend(const std::string& folderName) {
    if (folderName.empty())
        return;
    if (!currentPath_.empty())
        currentPath_.push_back('\\');
    currentPath_ += folderName;
    Refresh();
}

void StorageBrowser::Ascend() {
    std::vector<std::string> segs;
    SplitSegments(currentPath_, segs);
    if (segs.empty())
        return;
    segs.pop_back();
    currentPath_ = JoinSegments(segs);
    Refresh();
}

void StorageBrowser::NavigateTo(const std::string& displayPath) {
    std::vector<std::string> segs;
    SplitSegments(displayPath, segs);
    currentPath_ = JoinSegments(segs);
    Refresh();
}

std::string StorageBrowser::ChildPath(const std::string& fileName) const {
    const Node* node = NodeAt(currentPath_);
    if (!node)
        return {};
    auto it = node->files.find(fileName);
    return it != node->files.end() ? it->second : std::string{};
}

} // namespace whiteout::flakes::io
