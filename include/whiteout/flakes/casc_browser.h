#pragma once

/// @file casc_browser.h
/// @brief Navigable view of the model and effect files inside a CASC archive.

#include "types.h"

#include <memory>
#include <string>
#include <vector>

namespace whiteout::flakes {

namespace detail {
class CascBrowserImpl;
}

/// @brief Which file types @ref CascBrowser::Files reports.
///
/// Naming matches `StorageFileFilter` in the explorer panel, which has had
/// the same three-way choice since before the browser was bindable.
/// @bind
enum class CascFileFilter : u8 {
    All = 0,         ///< Models and effects both.
    ModelsOnly = 1,  ///< `.mdx` / `.mdl`.
    EffectsOnly = 2, ///< `.pkb`
};

/// @brief Browses an installed Warcraft III's CASC storage as a folder tree of
///        model and effect files.
///
/// The Reforged TVFS encodes the mod chain with `:` and the content tree with
/// `\` — `war3.w3mod:_hd.w3mod:units\nightelf\druid\druid.mdx`. This walks the
/// archive once on open, keeps only the folders that lead to a `.mdx`, `.mdl`,
/// `.pkb` or `.pkfx`, and presents them as directories you can descend into.
/// Display paths drop the leading `war3.w3mod:` and use `\` throughout; each
/// file leaf remembers its original archive path, which is what the renderer
/// reads.
///
/// The browser is standalone — it opens its own handle on the archive and
/// needs no `Renderer`. To render what you pick, point a scene at the same
/// install and spawn the path this hands you:
///
/// @code
/// CascBrowser br;
/// if (!br.Open(R"(C:\Program Files\Warcraft III)"))
///     return br.LastError();
/// br.Descend("units");
/// const auto path = br.ChildPath(br.Files()[0]);
///
/// r.Scene().SetCascInstallPath(br.Root());
/// r.Loader().SpawnUnit(path);
/// @endcode
/// @bind methods
class CascBrowser {
public:
    CascBrowser();
    ~CascBrowser();
    CascBrowser(const CascBrowser&) = delete;
    CascBrowser& operator=(const CascBrowser&) = delete;

    /// @brief Open the CASC at @p root — the directory holding `.build.info`,
    ///        or its `Data` subdirectory.
    ///
    /// Enumerating the archive is the expensive part and happens here, once.
    /// Returns `false` on failure; @ref LastError says why.
    bool Open(const std::string& root);
    /// @brief Why the last @ref Open failed. Empty after a successful one.
    bool IsOpen() const;
    /// @brief The root this was opened with.
    /// @bind rename=Root
    std::string GetRoot() const;
    /// @brief Message from the last failed @ref Open, or empty.
    /// @bind rename=LastError
    std::string GetLastError() const;

    /// @name Navigation
    /// @{
    /// @brief Current directory in display form (`\`-separated, empty at the
    ///        root).
    /// @bind rename=CurrentPath
    std::string GetCurrentPath() const;
    /// @brief The current path split into segments, for a breadcrumb bar.
    std::vector<std::string> Breadcrumb() const;
    /// @brief Immediate subfolder names of the current directory.
    std::vector<std::string> Folders() const;
    /// @brief File names directly inside the current directory, subject to
    ///        @ref GetFilter. Names only — pair with @ref ChildPath to load
    ///        one.
    std::vector<std::string> Files() const;

    /// @brief Restrict @ref Files to models, to effects, or to neither.
    ///
    /// Filters the file list only; @ref Folders is unchanged, so a folder
    /// whose contents are all filtered out still appears and simply reads as
    /// empty. That keeps the tree stable while the filter is toggled, which
    /// is what a panel wants — hiding folders would make the shape of the
    /// archive jump around.
    ///
    /// Costs nothing to change: the archive is walked once at @ref Open and
    /// the filter is applied per call.
    void SetFilter(CascFileFilter filter);
    /// @bind rename=Filter
    CascFileFilter GetFilter() const;

    /// @brief How many files the current directory holds in total, ignoring
    ///        the filter — so a panel can say "3 of 12" rather than making
    ///        an empty folder look like a dead end.
    i32 UnfilteredFileCount() const;

    /// @brief Descend into a subfolder of the current directory. No-op for a
    ///        name that is not in @ref Folders.
    void Descend(const std::string& folderName);
    /// @brief Go up one level. No-op at the root.
    void Ascend();
    /// @brief Jump to a display path, as returned by @ref GetCurrentPath.
    void NavigateTo(const std::string& displayPath);
    /// @}

    /// @brief The original archive path of a file in the current directory —
    ///        what `LoaderView::SpawnUnit` / `SpawnEffect` reads. Empty for a
    ///        name that is not in @ref Files.
    std::string ChildPath(const std::string& fileName) const;

    /// @brief `true` if @p fileName is an effect (`.pkb` / `.pkfx`) rather
    ///        than a model, so a host knows which spawn call to make.
    bool IsEffect(const std::string& fileName) const;

private:
    std::unique_ptr<detail::CascBrowserImpl> impl_;
};

} // namespace whiteout::flakes
