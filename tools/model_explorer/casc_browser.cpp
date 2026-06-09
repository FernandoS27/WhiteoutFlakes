#include "casc_browser.h"

#include <algorithm>
#include <cctype>

namespace whiteout::flakes::tools {

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
std::string ToDisplay(std::string_view archivePath) {
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

bool CascBrowser::Open(const std::string& root, std::string* error) {
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

    // Build the model folder tree: enumerate every entry, keep model/effect
    // files, insert each by its display path while remembering the original.
    tree_ = Node{};
    storage_->enumerate([this](const storages::casc::EnumerateEntry& e) {
        if (!IsModelOrEffect(e.path))
            return true;
        const std::string original(e.path);
        std::vector<std::string> segs;
        SplitSegments(ToDisplay(e.path), segs);
        if (segs.empty())
            return true;
        Node* node = &tree_;
        for (size_t i = 0; i + 1 < segs.size(); ++i) {
            const std::string key = ToLower(segs[i]);
            node->folderDisplay.emplace(key, segs[i]);
            node = &node->folders[key];
        }
        node->files.emplace(segs.back(), original);
        return true;
    });

    currentPath_.clear();
    Refresh();
    return true;
}

const CascBrowser::Node* CascBrowser::NodeAt(const std::string& displayPath) const {
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

void CascBrowser::Refresh() {
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

void CascBrowser::Descend(const std::string& folderName) {
    if (folderName.empty())
        return;
    if (!currentPath_.empty())
        currentPath_.push_back('\\');
    currentPath_ += folderName;
    Refresh();
}

void CascBrowser::Ascend() {
    std::vector<std::string> segs;
    SplitSegments(currentPath_, segs);
    if (segs.empty())
        return;
    segs.pop_back();
    currentPath_ = JoinSegments(segs);
    Refresh();
}

void CascBrowser::NavigateTo(const std::string& displayPath) {
    std::vector<std::string> segs;
    SplitSegments(displayPath, segs);
    currentPath_ = JoinSegments(segs);
    Refresh();
}

std::string CascBrowser::ChildPath(const std::string& fileName) const {
    const Node* node = NodeAt(currentPath_);
    if (!node)
        return {};
    auto it = node->files.find(fileName);
    return it != node->files.end() ? it->second : std::string{};
}

} // namespace whiteout::flakes::tools
