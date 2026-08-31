#pragma once

// CascBrowser — opens a CASC archive and presents its model/effect files as a
// navigable folder tree. The WC3 Reforged TVFS encodes the mod chain with ':'
// and the real content tree with '\' (e.g.
// "war3.w3mod:_hd.w3mod:units\nightelf\druid\druid.mdx"). To make that easy to
// browse, on open we enumerate every file of the open game's browsable types
// (BrowseTypesFor — .mdx/.mdl/.pkb/.pkfx for Warcraft III, .m2 for World of
// Warcraft, .m3 for StarCraft II), strip the leading
// "war3.w3mod:" mod prefix, treat ':' as a folder separator like '\', and build
// a tree of only the folders that lead to models. Each file leaf remembers its
// ORIGINAL archive path so the content provider can still read it verbatim.

#include "io/storage/casc_registry.h"
#include "whiteout/flakes/enums.h" // ProductId

#include <map>
#include <memory>
#include <set>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <whiteout/storages/casc/storage.h>
#if WHITEOUT_HAS_MPQ
#include <whiteout/storages/mpq/storage.h>
#endif

namespace whiteout::flakes::io {

class ProgressMonitor;

class ProgressMonitor;

class ProgressMonitor;

// Where a browser's entries come from.
enum class StorageKind {
    Casc,   // an installed game's CASC storage
    Mpq,    // a single .mpq / .w3x / .w3m archive
    Folder, // a directory on disk, walked recursively
    // Every .mpq in one directory, merged into a single tree — a pre-Reforged
    // Warcraft III install, whose content is split across War3.mpq /
    // War3x.mpq / War3xlocal.mpq / War3Patch.mpq and is only whole when all
    // four are read together. `root` is the directory, not an archive.
    MpqSet,
};

// Which kind `path` should be READ as, decided by what is actually on disk.
// Anything that is not a directory is a single archive; a directory is one of
// the three directory kinds. Split out of OpenAuto so a caller that must make
// the same decision without opening anything — an extractor choosing which
// storage to ask first — agrees with the browser by construction.
//
// The subtle case is Warcraft III, where the two generations are not exclusive
// on disk. A Battle.net-managed classic install ("Warcraft III Legacy") is a
// Reforged install with the 1.2x game laid back over it: it keeps the Data/
// tree and the .build.info of what it replaced, while every model and texture
// it can actually show lives in root-level War3.mpq / War3x.mpq /
// War3Local.mpq / War3xLocal.mpq / Deprecated.mpq. Letting the marker decide
// opens that as CASC, succeeds, and enumerates nothing — which reads as a
// broken browser rather than as the wrong storage.
//
// So the rule is not "archives win": it is War3LegacyInstaller, the one file
// that marks the downgrade, that decides. A real Reforged install owns its
// content through CASC, and any .mpq sitting beside it is a leftover of an
// older patch holding nothing the CASC has not got a newer copy of — there,
// archives are ignored.
StorageKind ClassifyStorage(const std::string& path);

// What a storage is worth walking for, one bit per thing a host would offer as
// a checkbox — so the grouping is the user's ("models", "effects"), not the
// file extension's.
//
// Which bits *apply* is a property of the game (BrowseTypesFor): Warcraft III
// ships models and PopcornFX effects, World of Warcraft ships `.m2` and
// nothing else this can draw. Which of them are *on* is the user's choice, and
// changing that re-filters without reopening anything — the tree is built once
// with every type the game has.
enum class BrowseType : u32 {
    None = 0,
    Models = 1u << 0,  // .mdx / .mdl
    Effects = 1u << 1, // .pkb / .pkfx
    M2 = 1u << 2,      // .m2
    M3 = 1u << 3,      // .m3
    Actor = 1u << 4,   // .acr / .app
    // .blp / .dds / .tga — the images the models above sample. Not drawable
    // themselves, which is why they are last: a host that browses for one
    // shows a decoded picture, not a rendered scene.
    Textures = 1u << 5,
};

constexpr BrowseType operator|(BrowseType a, BrowseType b) {
    return static_cast<BrowseType>(static_cast<u32>(a) | static_cast<u32>(b));
}
constexpr BrowseType operator&(BrowseType a, BrowseType b) {
    return static_cast<BrowseType>(static_cast<u32>(a) & static_cast<u32>(b));
}
constexpr BrowseType operator~(BrowseType a) {
    return static_cast<BrowseType>(~static_cast<u32>(a));
}
constexpr bool Any(BrowseType t) {
    return static_cast<u32>(t) != 0;
}

// Every type @p game ships. Neutral means "no game said", which a loose folder
// is — it gets all of them, because a directory of mixed content is exactly
// what nobody can narrow.
BrowseType BrowseTypesFor(ProductId game);

// Display name for a single bit, e.g. "Models (.mdx)". Empty for a mask that
// is not exactly one type.
const char* BrowseTypeLabel(BrowseType one);

// Which type @p fileName is, or None when it is not something to browse for.
BrowseType BrowseTypeOfFile(std::string_view fileName);

// Does @p name pass the free-text filter @p pattern? The syntax is the one a
// filter box is expected to have:
//
//   peasant        case-insensitive substring
//   *.mdx, foot?an `*` / `?` make the term a glob matched against the WHOLE name
//   a,b            comma-separated terms are OR'd (spaces around them are trimmed,
//                  so a term may itself contain spaces)
//   -portrait      a leading '-' excludes; an exclusion beats every include
//
// An empty pattern — or one with only exclusions — passes everything not
// excluded.
bool MatchesFilter(std::string_view name, std::string_view pattern);

class StorageBrowser {
public:
    struct Listing {
        std::vector<std::string> folders;    // immediate subfolder names
        std::vector<std::string> modelFiles; // model/effect file names in this folder
        // What this folder holds before the text filter (but after the type
        // filter) — the denominator for a host's "12 of 340 shown".
        std::size_t folderTotal = 0;
        std::size_t fileTotal = 0;
    };

    // Open `root` as `kind` and build the folder tree. For Casc, `root` is
    // the directory holding .build.info (or its Data subdir); for Mpq, the
    // archive file; for Folder, the directory to walk. Returns false and
    // fills `error` on failure.
    // @param progress Optional. Covers both halves of an open — the CASC
    //        open itself and the manifest walk that follows it, which on a
    //        StarCraft II install is three quarters of a million entries and
    //        the longest single wait the product has. Also carries
    //        cancellation: a cancelled open leaves the browser closed.
    bool Open(const std::string& root, StorageKind kind, std::string* error,
              ProgressMonitor* progress = nullptr);

    // Listfile and TACT key list to open a CASC with. Only World of Warcraft
    // needs either — its root is id-keyed, so without a listfile a browse of it
    // is empty rather than merely unnamed — and passing the *same* paths the
    // rest of the process uses is what makes this share its storage instead of
    // opening a second copy (see casc_registry.h). Set before Open.
    void SetCascKeys(std::string listfilePath, std::string tactKeyPath);

    // Open `path` as whatever ClassifyStorage says it is: a Reforged install
    // is CASC, a classic one is the directory of MPQs beside its (misleading)
    // .build.info, a file is a single archive, anything else is a folder.
    // Convenience for a dialog that just received a drop or a path from the
    // user, and the one call a host should make when "the Warcraft III
    // install" may be either generation.
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
    // Which game this storage holds, resolved once at Open. Neutral when the
    // storage is closed or its build-product string isn't one we know.
    ProductId Product() const {
        return product_;
    }

    // Narrow what the NEXT Open walks for, within what the game ships.
    //
    // The tree is built once with every available type because toggling a
    // checkbox should re-filter rather than reopen. That trade only pays while
    // the types cost about the same, and textures break it: a Warcraft III
    // install holds an order of magnitude more of them than models, and a WoW
    // one is mostly `.blp`. So a host that will only ever show ONE kind — a
    // picker, whose caller already said which — says so here and pays for that
    // kind alone.
    //
    // None (the default) means "everything the game ships", which is what a
    // general browser with a type row wants. A mask the game has none of is
    // ignored rather than obeyed: an empty tree is never the useful reading.
    void SetOpenTypes(BrowseType types) {
        openTypes_ = types;
    }
    BrowseType OpenTypes() const {
        return openTypes_;
    }

    // The types this storage was walked for — the open game's set narrowed by
    // SetOpenTypes, and the menu a host's checkboxes should offer.
    BrowseType AvailableTypes() const {
        return available_;
    }
    // The subset currently listed. Defaults to all of AvailableTypes at Open;
    // setting it re-filters the current folder and nothing else, so a host can
    // toggle a checkbox per frame without reopening the storage.
    BrowseType EnabledTypes() const {
        return enabled_;
    }
    void SetEnabledTypes(BrowseType types);

    // Free-text filter over the current folder's entries — folders and files
    // alike, so a host's box narrows what is on screen rather than only half of
    // it. See MatchesFilter for the syntax. Like the type filter this only
    // re-lists the current folder, so a host can set it every keystroke; it is
    // not recursive, and "" (the default) shows everything.
    void SetFilter(std::string pattern);
    const std::string& Filter() const {
        return filter_;
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

    // ---- Whole-tree browsing (a host that shows every level at once) ----
    //
    // Everything above is a folder at a time: one current directory, one
    // listing, a filter matched against entry NAMES. A tree shows all levels
    // together, where that filter rule reads wrong - a folder whose own name
    // misses the pattern is usually the only way to reach the files that hit
    // it. So here the filter is matched against each file's full display path,
    // and a folder is kept exactly when some file under it survived. That
    // prunes subtrees rather than levels, and it leaves the current folder,
    // the breadcrumb and Current() untouched: a host can drive both views off
    // one open browser.
    struct TreeListing {
        std::vector<std::string> folders; // subfolder display names to show
        std::vector<std::string> files;   // model/effect display names to show
    };

    // Children of @p displayPath ("" = the root) under the rule above.
    TreeListing TreeChildren(const std::string& displayPath) const;

    // Original archive path for @p fileName inside @p displayPath - ChildPath
    // for a folder that is not the current one.
    std::string ChildPathAt(const std::string& displayPath,
                            const std::string& fileName) const;

    // How many folders anywhere in the tree the filter left standing. A host
    // that wants to expand to the matches needs the count BEFORE it expands
    // anything: auto-expanding a pattern that matched half a World of Warcraft
    // install is not a search result, it is the tree again. 0 when no filter is
    // set - nothing was pruned, so there is nothing to count.
    std::size_t TreeVisibleFolderCount() const;

    // Files anywhere in the tree the filter left standing. 0 when no filter is
    // set, for the same reason.
    std::size_t TreeMatchCount() const;

private:
    // A folder node: subfolders + the model files directly inside it (display
    // name → original archive path).
    struct Node {
        std::map<std::string, Node> folders;              // key = lowercase name
        std::map<std::string, std::string> folderDisplay; // lowercase → display
        std::map<std::string, std::string> files;         // display name → archive path
    };

    void Refresh();
    const Node* NodeAt(const std::string& displayPath) const;
    // Build the full-path filter cache (see TreeChildren) if it is stale. One
    // pass over the whole tree, which is why it is cached rather than redone
    // per folder: a host draws the tree every frame and the filter moves only
    // when the user stops typing.
    void EnsureTreeCache() const;
    // DFS half of the above: records each folder under @p node whose subtree
    // held a match, and returns whether @p node's own subtree did.
    bool MarkTreeMatches(const Node& node, const std::string& path) const;
    // Insert one entry into the tree: `original` is what a provider reads,
    // `display` is what the user navigates.
    void Insert(const std::string& original, const std::string& display);

    bool OpenCasc(const std::string& root, std::string* error, ProgressMonitor* progress);
    bool OpenMpq(const std::string& path, std::string* error);
    bool OpenMpqSet(const std::string& directory, std::string* error);
    bool OpenFolder(const std::string& path, std::string* error);
    // Insert every browsable entry of one already-open MPQ. Shared by the
    // single-archive open and the set, which differ only in how many times
    // this runs.
    std::size_t InsertMpqEntries(const std::string& archiveFile, std::string* error);
    // available_ for @p product, narrowed by openTypes_ when that leaves
    // anything standing. The one place the narrowing rule lives.
    BrowseType ResolveAvailable(ProductId product) const;

    bool open_ = false;
    StorageKind kind_ = StorageKind::Casc;
    ProductId product_ = ProductId::Neutral;
    BrowseType available_ = BrowseType::None;
    BrowseType enabled_ = BrowseType::None;
    BrowseType openTypes_ = BrowseType::None; // see SetOpenTypes
    std::string filter_;
    std::string listfilePath_;
    std::string tactKeyPath_;
    // Kept alive only for CASC: enumerate() borrows the storage. MPQ and
    // Folder are read once into the tree and need nothing retained. Shared
    // with every scene reading the same install — see casc_registry.h.
    std::shared_ptr<const SharedCasc> storage_;
    std::string root_;
    std::string currentPath_; // display form, '\\'-separated
    std::vector<std::string> breadcrumb_;
    Listing listing_;
    Node tree_;
    // Full-path filter cache: lowercase display paths of the folders that
    // survived, and how many files did. Built lazily, so a host that never
    // asks for a tree never pays the walk.
    mutable std::set<std::string> treeKeep_;
    mutable std::size_t treeMatches_ = 0;
    mutable bool treeCacheDirty_ = true;
};

} // namespace whiteout::flakes::io
