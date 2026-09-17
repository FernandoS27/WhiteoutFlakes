#pragma once

// ============================================================================
// The native file dialogs the viewer pops. Filters come from the model-format
// table, so File > Open and the startup picker offer the same list.
// ============================================================================

#include <filesystem>
#include <optional>

namespace whiteout::flakes {

/// File > Open and the startup picker. Nullopt when the user cancels.
std::optional<std::filesystem::path> PickModelToOpen();

} // namespace whiteout::flakes
