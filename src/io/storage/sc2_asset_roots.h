#pragma once

// ============================================================================
// Where a loose StarCraft II model's relative paths resolve.
//
// An .m3 names its textures relative to the root of the component it ships
// in (`Assets/Textures/x.dds`), not relative to its own folder: a model at
// `Foo.SC2Mod/Assets/Units/X/x.m3` means `Foo.SC2Mod/Assets/Textures/x.dds`,
// and one under a `Base.SC2Assets` component means the copy beside it.
// ============================================================================

#include <filesystem>
#include <vector>

namespace whiteout::flakes::io {

/// True when the name ends in .SC2Mod, .SC2Map or .SC2Campaign (any case).
/// Says nothing about whether it is a folder or a packed file.
bool IsSc2DocumentName(const std::filesystem::path& p);

/// A mod/map/campaign folder's asset roots, most specific first:
/// `<doc>/Base.SC2Assets`, then `<doc>`, each kept only if it holds an
/// `Assets` folder. Child names are matched in any case.
std::vector<std::filesystem::path> Sc2DocumentAssetRoots(const std::filesystem::path& docDir);

/// The asset roots a loose StarCraft II model's relative paths
/// (`Assets/Textures/x.dds`) resolve against. Empty when the model sits in no
/// mod and in no `Assets/` tree.
std::vector<std::filesystem::path> FindSc2AssetRoots(const std::filesystem::path& modelPath);

} // namespace whiteout::flakes::io
