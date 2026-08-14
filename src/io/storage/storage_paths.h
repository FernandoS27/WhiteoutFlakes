#pragma once

// ============================================================================
// Path spelling shared by every storage backend.
//
// A caller says "Textures\Water07-0.blp". CASC stores lowercase backslash
// paths, MPQ stores what the archive author typed, a listing wants lowercase
// forward slashes, and both archive formats may hold the same asset under a
// different extension than the model asked for. These are the conversions in
// between — the parts that are about *naming*, not about storage.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <string>
#include <string_view>
#include <utility>

namespace whiteout::flakes::io {

// CASC form: lowercase, backslash-separated, no leading separator.
std::string NormalizeCascPath(std::string_view relPath);

// Listing form: lowercase, '/'-separated, no leading slash, and with any CASC
// mod chain dropped ("war3.w3mod:_hd.w3mod:units\x.mdx" → "units/x.mdx").
// That is exactly the shape a read expects back — the CASC source re-applies
// its own prefixes — so a listed path can be read verbatim.
std::string ToListingPath(std::string_view stored);

// Same normalisation for a caller-supplied directory, minus any trailing
// separator so "textures/fx" and "Textures\FX\" mean the same thing.
std::string NormalizeListingDir(const std::string& directory);

// Is `relPath` (already in listing form) inside `dir`? A non-recursive match
// additionally requires the file to sit directly in it.
bool MatchesListingDir(const std::string& relPath, const std::string& dir, bool recursive);

std::string StripExtension(std::string_view path);

// Lowercased, including the dot. Empty when there is no extension.
std::string GetLowerExtension(std::string_view relPath);

// Extensions worth retrying when `ext` misses: a Reforged install ships as
// .dds what a classic model asks for as .blp, and .mdl/.mdx shadow each other.
// Returns {nullptr, 0} for anything that is not a texture or a model.
std::pair<const char* const*, usize> AltExtensionsFor(const std::string& ext);

// What an extension says the file is. Only the disk resolver cares — it has a
// separate search path per kind — but the answer comes from the same tables
// AltExtensionsFor uses, so it lives with them.
enum class AssetKind { Other, Texture, Model };
AssetKind ClassifyByExtension(const std::string& ext);

} // namespace whiteout::flakes::io
