#pragma once

// ============================================================================
// Persistence for the animation-export recipe.
//
// The whole recipe round-trips through `[Export]` in WhiteoutFlakes.ini, and
// the same writer pointed at an arbitrary path gives shareable recipe files
// for free — the dialog's Save…/Load… and `--export-recipe` are the same code.
//
// Device-free, like export_recipe.cpp: it links `ini_file.h` (header-only)
// and settings_io.cpp's SettingsIniPath(), and nothing else.
// ============================================================================

#include "capture/export_recipe.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace whiteout::flakes {

/// @brief What a load could not do, for the dialog to show inline.
struct ExportRecipeLoadReport {
    /// @brief Clips whose saved name is not in this model. They stay in the
    ///        queue with `sequence == -1` so the user sees what was dropped.
    std::vector<std::string> unresolvedClips;
    /// @brief The model the queue was saved against, if any.
    std::string savedModel;
    bool hadSection = false; ///< false when the ini carried no `[Export]` at all
};

/// @brief Read `[Export]` from the viewer's settings ini.
///
/// Clips resolve by name (see @ref ResolveSequenceKey) because sequence
/// indices are export order and mean nothing across models — a saved queue of
/// `3, 7, 12` replayed against the next model silently records three wrong
/// animations, which is worse than not persisting at all.
///
/// Missing keys keep `recipe`'s existing values, so a partial or older ini
/// degrades field by field rather than resetting the lot.
void LoadExportRecipe(ExportRecipe& recipe, std::span<const std::string> sequenceNames,
                      ExportRecipeLoadReport* report = nullptr);

/// @brief Write `[Export]` back, preserving every unrelated key.
void SaveExportRecipe(const ExportRecipe& recipe, std::span<const std::string> sequenceNames,
                      const std::filesystem::path& modelPath);

/// @brief The same recipe as a standalone file — the dialog's Save…, and
///        `--export-recipe` on the CLI.
bool WriteExportRecipeFile(const std::filesystem::path& file, const ExportRecipe& recipe,
                           std::span<const std::string> sequenceNames,
                           const std::filesystem::path& modelPath);

bool ReadExportRecipeFile(const std::filesystem::path& file, ExportRecipe& recipe,
                          std::span<const std::string> sequenceNames,
                          ExportRecipeLoadReport* report = nullptr);

} // namespace whiteout::flakes
