#pragma once

// ============================================================================
// Which textures an export writes.
//
// A texture is written when the model that was written reads it -- a Reforged
// source's diffuse, normal and ORM are what the bake turned into StarCraft II
// maps, and nothing names them afterwards -- and, when a Warcraft III export
// asks for it, not when StarCraft II's War3 (Mod) already ships the same
// picture. This is the device-free half: the paths a written `.m3` names, the
// MDX texture entries anything reads, and the test that two decoded textures
// are the same picture.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <whiteout/models/m3/structures.h>
#include <whiteout/models/mdx/mdx.h>

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace whiteout::textures {
struct Texture;
}

namespace whiteout::flakes {

/// The spelling two texture paths compare by: forward slashes, lowercase.
std::string TexturePathKey(std::string_view path);

/// Every texture path @p model names, as `TexturePathKey`s: each present layer
/// of every material kind and lens flare, and a data-driven material's table.
std::unordered_set<std::string> M3TexturePathsNamed(const ::whiteout::m3::Model& model);

/// Repoint every path in @p model that names @p from at @p to. Returns how
/// many were repointed.
int RenameM3TexturePath(::whiteout::m3::Model& model, std::string_view from,
                        const std::string& to);

/// Per `mdx::Model::textures` entry, whether anything reads it: a layer's own
/// id, its flipbook keys and sub-textures, or a particle emitter 2.
std::vector<bool> MdxTexturesUsed(const ::whiteout::mdx::Model& model);

/// Drop every entry @p keep clears and renumber everything that reads the rest.
void PruneMdxTextures(::whiteout::mdx::Model& model, const std::vector<bool>& keep);

/// The name StarCraft II's War3 (Mod) ships a Warcraft III texture under,
/// `war3_<stem>.dds` in the source's own case. Empty for a path with no stem.
std::string War3ModTextureName(std::string_view sourcePath);

/// Whether @p shipped is the same picture as @p ours: compared at the smaller
/// one's size, every channel correlating at 0.9 or better and the mean colour
/// and alpha errors at most 16 of 255.
bool SameTexturePicture(const ::whiteout::textures::Texture& ours,
                        const ::whiteout::textures::Texture& shipped);

} // namespace whiteout::flakes
