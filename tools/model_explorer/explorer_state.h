#pragma once

// Which browser the Storage Explorer panel shows, and everything about the
// panel worth handing back next session.
//
// Split out of storage_explorer.h so a host can persist this without linking
// the panel: that header pulls in RenderService and therefore a gfx backend,
// and where the user left a folder is a question about strings.

#include "io/storage_browser.h" // io::BrowseType
#include "whiteout/flakes/enums.h" // ProductId

#include <string>

namespace whiteout::flakes::tools {

// Which of the panel's two browsers is on screen.
//
//   Grid - one folder at a time, as a wall of live thumbnails.
//   Tree - the whole storage as an outline on the left, and the file selected
//          in it as one large thumbnail on the right.
//
// One storage, one selection and one activate callback sit behind both. Only
// the filter changes meaning: the grid narrows the current folder by entry
// name, the tree matches full paths and so prunes subtrees rather than levels
// (io::StorageBrowser::TreeChildren).
enum class ExplorerView { Grid, Tree };

// Deliberately NOT the tree's expanded folders: those are how you got
// somewhere, not where you are, and restoring a hundred of them is a different
// panel from the one that was closed.
//
// Nothing here is applied at once. `game`, `folder`, `filter` and `selected`
// all name things INSIDE a storage that is not open yet (and on a task runner,
// will not be for several seconds), so StorageExplorer::RestoreState stages
// them and the open's completion applies them - and only if the storage that
// opened is the one they were recorded from.
struct ExplorerState {
    ExplorerView view = ExplorerView::Grid;
    // Which game the panel's own combo was on - not the host's profile, which
    // the user is free to have moved away from since.
    ProductId game = ProductId::Neutral;
    // Heroes of the Storm, which is a browse target but not a ProductId: it
    // shares ProductId::Sc2 with StarCraft II because it shares a render
    // profile, and the two are separate installs. Meaningless unless `game` is
    // Sc2. Without this a panel closed on Heroes comes back on StarCraft II.
    bool heroes = false;
    // None means "not recorded": keep whatever the game's default is rather
    // than restoring an empty set and showing nothing.
    io::BrowseType browseTypes = io::BrowseType::None;
    std::string folder;   // the grid's folder, display form
    std::string filter;   // the APPLIED filter, not what is half-typed in the box
    std::string selected; // selected file, display path ("" = none)
    float iconSize = 128.0f;
    float treeSplit = 300.0f;
};

} // namespace whiteout::flakes::tools
