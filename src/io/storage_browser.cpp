#include "io/storage_browser.h"

#include "io/product_detect.h"
#include "io/progress.h"

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

} // namespace

BrowseType BrowseTypeOfFile(std::string_view fileName) {
    const auto dot = fileName.find_last_of('.');
    if (dot == std::string_view::npos)
        return BrowseType::None;
    const std::string ext = ToLower(std::string(fileName.substr(dot + 1)));
    if (ext == "mdx" || ext == "mdl")
        return BrowseType::Models;
    if (ext == "pkb" || ext == "pkfx")
        return BrowseType::Effects;
    if (ext == "m2")
        return BrowseType::M2;
    if (ext == "m3")
        return BrowseType::M3;
    // Both halves of a Diablo III drawable: the `.acr` is what a host names,
    // the `.app` is what actually holds geometry and is worth opening alone.
    if (ext == "acr" || ext == "app")
        return BrowseType::Actor;
    return BrowseType::None;
}

BrowseType BrowseTypesFor(ProductId game) {
    switch (game) {
    case ProductId::Wc3:
        return BrowseType::Models | BrowseType::Effects;
    case ProductId::Wow:
        // `.m2` and nothing else. A WoW install also ships `.wmo`, `.adt` and
        // the rest of a world, none of which this draws, so offering a filter
        // for them would be offering an empty grid.
        return BrowseType::M2;
    case ProductId::Sc2:
        return BrowseType::M3;
    case ProductId::D3:
        return BrowseType::Actor;
    default:
        // Nobody said, which is what a loose folder is: show everything rather
        // than guess which half of a mixed directory was meant.
        return BrowseType::Models | BrowseType::Effects | BrowseType::M2 | BrowseType::M3 |
               BrowseType::Actor;
    }
}

namespace {

// `*` (any run) / `?` (any one char) against the whole string, both already
// lowercased. Iterative with one backtrack point, so a pattern of nothing but
// stars can't blow the stack on a long name.
bool GlobMatch(std::string_view pat, std::string_view s) {
    std::size_t pi = 0, si = 0, star = std::string_view::npos, mark = 0;
    while (si < s.size()) {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == s[si])) {
            ++pi;
            ++si;
        } else if (pi < pat.size() && pat[pi] == '*') {
            star = pi++;
            mark = si;
        } else if (star != std::string_view::npos) {
            pi = star + 1;
            si = ++mark;
        } else {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == '*')
        ++pi;
    return pi == pat.size();
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
        s.remove_suffix(1);
    return s;
}

} // namespace

bool MatchesFilter(std::string_view name, std::string_view pattern) {
    const std::string hay = ToLower(std::string(name));
    bool anyInclude = false;
    bool included = false;
    for (std::size_t i = 0; i <= pattern.size();) {
        const std::size_t comma = pattern.find(',', i);
        std::string_view term = Trim(pattern.substr(i, comma - i));
        i = comma == std::string_view::npos ? pattern.size() + 1 : comma + 1;
        const bool exclude = !term.empty() && term.front() == '-';
        if (exclude)
            term.remove_prefix(1);
        if (term.empty())
            continue;
        const std::string needle = ToLower(std::string(term));
        const bool hit = needle.find_first_of("*?") == std::string::npos
                             ? hay.find(needle) != std::string::npos
                             : GlobMatch(needle, hay);
        if (exclude) {
            if (hit)
                return false; // an exclusion beats every include
        } else {
            anyInclude = true;
            included = included || hit;
        }
    }
    return !anyInclude || included;
}

const char* BrowseTypeLabel(BrowseType one) {
    switch (one) {
    case BrowseType::Models:
        return "Models (.mdx/.mdl)";
    case BrowseType::Effects:
        return "Effects (.pkb/.pkfx)";
    case BrowseType::M2:
        return "Models (.m2)";
    case BrowseType::M3:
        return "Models (.m3)";
    case BrowseType::Actor:
        return "Actors (.acr/.app)";
    default:
        return "";
    }
}

namespace {

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

// Join a folder path and one of its entries, both in display form.
std::string ChildDisplay(const std::string& path, const std::string& name) {
    return path.empty() ? name : path + '\\' + name;
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

bool StorageBrowser::Open(const std::string& root, StorageKind kind, std::string* error,
                          ProgressMonitor* progress) {
    open_ = false;
    kind_ = kind;
    product_ = ProductId::Neutral;
    available_ = BrowseType::None;
    storage_.reset();
    tree_ = Node{};
    currentPath_.clear();

    bool ok = false;
    switch (kind) {
    case StorageKind::Casc:
        ok = OpenCasc(root, error, progress);
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
    // Everything the game offers, until the host says otherwise.
    enabled_ = available_;
    treeCacheDirty_ = true;
    Refresh();
    return true;
}

void StorageBrowser::SetCascKeys(std::string listfilePath, std::string tactKeyPath) {
    listfilePath_ = std::move(listfilePath);
    tactKeyPath_ = std::move(tactKeyPath);
}

void StorageBrowser::SetEnabledTypes(BrowseType types) {
    // Only what the open storage actually has: a host that offers a stale
    // checkbox must not be able to ask for a type nothing was walked for.
    types = types & available_;
    if (types == enabled_)
        return;
    enabled_ = types;
    treeCacheDirty_ = true;
    Refresh();
}

void StorageBrowser::SetFilter(std::string pattern) {
    if (pattern == filter_)
        return;
    filter_ = std::move(pattern);
    treeCacheDirty_ = true;
    Refresh();
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

bool StorageBrowser::OpenCasc(const std::string& root, std::string* error,
                              ProgressMonitor* progress) {
    ProgressMonitor inert;
    ProgressMonitor& m = progress ? *progress : inert;
    // The open dominates in wall-clock, but the walk is the part that used
    // to look like a hang: it reports nothing, holds the calling thread and
    // on StarCraft II runs for seconds after the open has finished.
    m.Begin("Opening storage", 4);
    // Through the registry, so browsing an install a scene is already reading
    // costs nothing and shows exactly what that scene sees.
    CascOpenKey key;
    key.root = root;
    key.listfilePath = listfilePath_;
    key.tactKeyFile = tactKeyPath_;
    // Always on, matching StorageBuilder::Build — and it has to match, or the
    // browser would key a *second* storage of the same install out of the
    // registry. See the note there for why it is not tied to the key list.
    key.zeroFillEncrypted = true;
    std::string err;
    std::shared_ptr<const SharedCasc> s;
    {
        ProgressMonitor step = m.Split(3);
        s = AcquireSharedCasc(key, err, &step);
        if (!s && !m.Cancelled()) {
            std::string err2;
            key.root = root + "/Data";
            s = AcquireSharedCasc(key, err2, &step);
            if (!s) {
                if (error)
                    *error = err.empty() ? err2 : err;
                return false;
            }
        }
    }
    if (!s) {
        if (error)
            *error = m.Cancelled() ? "Open cancelled" : err;
        return false;
    }
    storage_ = std::move(s);
    root_ = storage_->Root();

    // The only place a product is genuinely *detected*. `product()` can be
    // nullopt (a storage that carries no build config), and an unrecognised
    // build-product string normalises to Neutral — both mean "we don't know",
    // which is a better answer than a confident wrong one.
    if (auto prod = storage_->Storage().product())
        product_ = ProductIdFromBuildProduct(prod->name);

    // Everything the game has, not just what is enabled: the walk is the
    // expensive part (three quarters of a million entries on StarCraft II), so
    // it happens once and a filter change re-lists rather than re-enumerates.
    available_ = BrowseTypesFor(product_);

    ProgressMonitor walk = m.Split(1);
    // entryCount() is what makes this a bar rather than a marquee. It is also
    // the only reason the library grew an accessor for it.
    walk.Begin("Indexing storage", storage_->Storage().entryCount());
    // Batched: at three quarters of a million entries a per-entry report would
    // cost more than the classification it is reporting on, and 4096 entries is
    // far below one frame's worth of walk.
    u64 seen = 0;
    bool cancelled = false;
    storage_->Storage().enumerate([&](const storages::casc::EnumerateEntry& e) {
        if (Any(BrowseTypeOfFile(e.path) & available_))
            Insert(std::string(e.path), CascToDisplay(e.path));
        if ((++seen & 0xFFF) == 0) {
            walk.Worked(0x1000);
            if (walk.Cancelled()) {
                cancelled = true;
                return false;
            }
        }
        return true;
    });
    if (cancelled) {
        if (error)
            *error = "Open cancelled";
        storage_.reset();
        tree_ = Node{};
        return false;
    }
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
    // MPQ has no build config and therefore no product record at all, but
    // the only games that ship MPQs we can browse are Warcraft III and its
    // maps. Reporting Wc3 unconditionally is a statement about the format,
    // not a guess about this particular archive.
    product_ = ProductId::Wc3;
    available_ = BrowseTypesFor(product_);
    // MPQ paths are already ''-separated and carry no mod prefix, so the
    // display form is the stored form.
    for (const auto& name : s->listFiles()) {
        if (Any(BrowseTypeOfFile(name) & available_))
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
    // A loose directory has no product record, and now that `.m2` and `.m3`
    // are browsable it cannot be assumed to be Warcraft III's either. Neutral
    // is the honest answer, and BrowseTypesFor turns it into "show everything"
    // — which is what a directory of mixed content deserves.
    product_ = ProductId::Neutral;
    available_ = BrowseTypesFor(product_);

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
                if (Any(BrowseTypeOfFile(name) & available_)) {
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
    listing_ = Listing{};
    const Node* node = NodeAt(currentPath_);
    if (node) {
        const bool filtered = !filter_.empty();
        for (const auto& [key, disp] : node->folderDisplay) {
            ++listing_.folderTotal;
            if (!filtered || MatchesFilter(disp, filter_))
                listing_.folders.push_back(disp);
        }
        for (const auto& [name, orig] : node->files) {
            if (!Any(BrowseTypeOfFile(name) & enabled_))
                continue;
            ++listing_.fileTotal;
            if (!filtered || MatchesFilter(name, filter_))
                listing_.modelFiles.push_back(name);
        }
        auto ci = [](const std::string& a, const std::string& b) {
            return ToLower(a) < ToLower(b);
        };
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

void StorageBrowser::EnsureTreeCache() const {
    if (!treeCacheDirty_)
        return;
    treeCacheDirty_ = false;
    treeKeep_.clear();
    treeMatches_ = 0;
    if (!filter_.empty())
        MarkTreeMatches(tree_, {});
}

bool StorageBrowser::MarkTreeMatches(const Node& node, const std::string& path) const {
    bool any = false;
    for (const auto& [key, disp] : node.folderDisplay) {
        auto sub = node.folders.find(key);
        if (sub == node.folders.end())
            continue;
        const std::string child = ChildDisplay(path, disp);
        if (MarkTreeMatches(sub->second, child)) {
            treeKeep_.insert(ToLower(child));
            any = true;
        }
    }
    for (const auto& [name, orig] : node.files) {
        // The type mask first: a `.wmo` beside a matching `.m2` must not keep a
        // folder alive that has nothing browsable in it.
        if (!Any(BrowseTypeOfFile(name) & enabled_))
            continue;
        if (MatchesFilter(ChildDisplay(path, name), filter_)) {
            ++treeMatches_;
            any = true;
        }
    }
    return any;
}

StorageBrowser::TreeListing StorageBrowser::TreeChildren(const std::string& displayPath) const {
    TreeListing out;
    const Node* node = NodeAt(displayPath);
    if (!node)
        return out;
    const bool filtered = !filter_.empty();
    if (filtered)
        EnsureTreeCache();
    for (const auto& [key, disp] : node->folderDisplay) {
        if (filtered && !treeKeep_.count(ToLower(ChildDisplay(displayPath, disp))))
            continue;
        out.folders.push_back(disp);
    }
    for (const auto& [name, orig] : node->files) {
        if (!Any(BrowseTypeOfFile(name) & enabled_))
            continue;
        if (filtered && !MatchesFilter(ChildDisplay(displayPath, name), filter_))
            continue;
        out.files.push_back(name);
    }
    // folderDisplay is already keyed by the lowercase name; the files are not.
    auto ci = [](const std::string& a, const std::string& b) { return ToLower(a) < ToLower(b); };
    std::sort(out.files.begin(), out.files.end(), ci);
    return out;
}

std::string StorageBrowser::ChildPathAt(const std::string& displayPath,
                                        const std::string& fileName) const {
    const Node* node = NodeAt(displayPath);
    if (!node)
        return {};
    auto it = node->files.find(fileName);
    return it != node->files.end() ? it->second : std::string{};
}

std::size_t StorageBrowser::TreeVisibleFolderCount() const {
    EnsureTreeCache();
    return treeKeep_.size();
}

std::size_t StorageBrowser::TreeMatchCount() const {
    EnsureTreeCache();
    return treeMatches_;
}

std::string StorageBrowser::ChildPath(const std::string& fileName) const {
    const Node* node = NodeAt(currentPath_);
    if (!node)
        return {};
    auto it = node->files.find(fileName);
    return it != node->files.end() ? it->second : std::string{};
}

} // namespace whiteout::flakes::io
