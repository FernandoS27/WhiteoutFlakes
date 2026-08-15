#pragma once

// ============================================================================
// What each game's storage looks like.
//
// This is the file to open when adding a fourth game, and the only one that
// should need editing: everything below it (sources, builder, provider) is
// game-agnostic, and everything above it asks by ProductId.
// ============================================================================

#include "game_storage.h"
#include "whiteout/flakes/enums.h" // ProductId

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace whiteout::flakes::io {

// Everything a host can configure about where content comes from. What it
// *means* is the game's business — an archive list is load order for Warcraft
// III and dead weight for StarCraft II.
struct StorageConfig {
    ProductId game = ProductId::Wc3;
    // Primary install root. Empty when that game was not found and the host
    // set no override.
    std::string installPath;
    // Second root for a product that spans two installs. Heroes of the Storm
    // only, which shares ProductId::Sc2 with StarCraft II.
    std::string secondaryPath;
    // Archive names relative to installPath, in load order.
    std::vector<std::string> archives;
    // Community `id;path` CSV. World of Warcraft only.
    std::string listfilePath;
    // Community `keyName keyHex` list. World of Warcraft only, and for the same
    // reason as the listfile: without it a chunk of the install reads as
    // missing rather than as encrypted.
    std::string tactKeyPath;
    bool ignoreCasc = false;
    bool ignoreArchives = false;
};

// Opens everything @p config asks for, following @p config.game's rules.
// Never null: a config that resolves to nothing yields an empty storage that
// reports every read as a miss, which is what an unconfigured provider should
// do.
std::unique_ptr<GameStorage> BuildGameStorage(const StorageConfig& config,
                                              const std::atomic<bool>* hdMode);

// The archive load order to use when the host has not chosen one. Warcraft
// III's three are fixed; StarCraft II and Heroes never shipped one; World of
// Warcraft's are a documented starting point that ScanArchives improves on.
std::vector<std::string> DefaultArchives(ProductId game);

// What is actually on disk under @p installPath for @p game, in load order.
// World of Warcraft is why this exists: its archive set was renamed at least
// twice across the MPQ era and is split across a per-locale subdirectory, so
// any hardcoded list is wrong for most installs. Falls back to
// DefaultArchives for products whose names genuinely are fixed.
std::vector<std::string> ScanArchives(ProductId game, const std::string& installPath);

} // namespace whiteout::flakes::io
