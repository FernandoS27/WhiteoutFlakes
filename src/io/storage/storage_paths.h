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

#include "whiteout/flakes/enums.h"
#include "whiteout/flakes/types.h"

#include <optional>
#include <span>
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

// ---- Warcraft III's mod chain ----------------------------------------------
//
// Warcraft III is the only product whose storage is a chain rather than a
// namespace: one asset can exist three times over, once per art tier, and a
// read has to say which it wants. All three spellings live here because
// everything with an opinion about them — the CASC source that prefixes a
// read, the browser that names an entry, the day/night catalogue that pins a
// rig to one tier, the corn-fx path that arrives with a tier baked in — has
// to agree on them, and a second copy of the list is how one of them silently
// stops agreeing.

// The chain for one art tier, most-derived overlay first: a read tries each in
// turn and takes the first hit.
//
// Leading with its own overlay and then falling through the older ones is what
// makes Definitive additive — it carries the 3,710 paths only it has without
// losing the 2,780 that only Reforged has. The two older tiers also reach
// *forward* into the newer overlays, which the game does not do; showing a
// doodad the classic tier never shipped beats showing a hole, and it is what
// Warcraft III reads have always done here.
std::span<const char* const> Wc3ModChain(Wc3ArtTier tier);

// Does this stored path already name a mod chain of its own?
//
// Testing for the `war3.w3mod:` root rather than for a ':' anywhere is
// deliberate: a loose-folder entry is an absolute Windows path and its drive
// letter is not a mod chain.
bool HasWc3ModChain(std::string_view stored);

// `stored` with the leading `war3.w3mod:` root dropped, or unchanged when it
// carries none. What a display or listing form starts from.
std::string_view StripWc3ModRoot(std::string_view stored);

// Which art tier a stored path names, for a path that names one.
//
// "war3.w3mod:_de.w3mod:units\x.mdx" → Definitive. This is how a path
// picks its own tier — a model opened out of the browser knows which overlay
// it came from, and reading its textures under any other tier finds the wrong
// art or none at all. Nullopt for a path carrying no chain: an MPQ entry, a
// file on disk, or anything belonging to another game.
std::optional<Wc3ArtTier> Wc3TierOfPath(std::string_view stored);

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
