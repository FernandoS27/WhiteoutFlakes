#pragma once

// ============================================================================
// The `[StorageExplorer]` half of WhiteoutFlakes.ini — where the embedded
// browser panel was left: its view, game, folder, filter, selection and sizes.
//
// Its own header and TU rather than a few more keys in settings_io.cpp, for
// the same reason settings_io is split from settings_ini: a section should be
// readable without linking what reads it. These take a value, not the panel,
// so nothing here pulls in RenderService — which is what lets the ini
// round-trip be unit-tested in a device-free build.
// ============================================================================

#include "explorer_state.h"

#include <string>

namespace whiteout::flakes {

// Read `[StorageExplorer]`. Missing keys keep ExplorerState's own defaults, so
// a first run and a hand-trimmed ini both give a sane panel.
tools::ExplorerState LoadStorageExplorerState();

// Write `[StorageExplorer]`, preserving every other section.
void SaveStorageExplorerState(const tools::ExplorerState& state);

// One string covering everything the two above persist. A host polls this to
// notice a change without rewriting the ini every frame; it touches no files.
std::string ExplorerStateKey(const tools::ExplorerState& state);

} // namespace whiteout::flakes
