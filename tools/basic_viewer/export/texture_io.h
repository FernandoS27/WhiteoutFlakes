#pragma once

// ============================================================================
// The file half of every conversion: reading a texture the way the loader
// would, writing a file into a folder that may not exist yet, the `#<id>`
// spelling of a texture the source only has an id for, and the per-export
// decode cache.
// ============================================================================

#include "whiteout/flakes/content_ref.h"
#include "whiteout/flakes/types.h"

#include <whiteout/models/wem/document.h>
#include <whiteout/textures/texture.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace whiteout::flakes {

namespace io {
class IContentProvider;
}

/// `#<id>` for a texture addressed by fileDataID or SNO id — `ContentRef::Describe`'s
/// spelling, and what a World of Warcraft or Diablo III texture arrives as —
/// and empty for one addressed by path.
std::string TextureIdKey(const ::whiteout::models::wem::TextureRef& ref);

/// The reference @p key resolves through: a `TextureIdKey` is a file id,
/// anything else a path.
ContentRef ContentRefForKey(std::string_view key);

/// The reference the loader resolves @p ref through: its id when it has one,
/// its path otherwise.
ContentRef ContentRefOf(const ::whiteout::models::wem::TextureRef& ref);

/// A file as the provider serves it.
struct TextureFile {
    std::vector<u8> bytes;
    /// As served. An id-addressed asset has no name to take one from — a World
    /// of Warcraft root manifest stores none — so the container's own magic
    /// answers instead.
    std::string extension;
};

/// Null when the provider has nothing, or nothing but an empty file.
std::optional<TextureFile> ReadTextureFile(io::IContentProvider& provider, const ContentRef& ref);

/// A decode, or why there is none.
struct TextureDecode {
    std::optional<::whiteout::textures::Texture> texture;
    /// Empty on success; otherwise short enough to follow a texture's name.
    std::string problem;
};

/// Decode @p bytes by @p extension, in any case. Never throws.
TextureDecode DecodeTexture(std::span<const u8> bytes, std::string_view extension);

/// `ReadTextureFile`, then `DecodeTexture`.
TextureDecode ReadDecodedTexture(io::IContentProvider& provider, const ContentRef& ref);

/// Write @p bytes to @p path, creating its folders first. False when the file
/// could not be opened or written.
bool WriteFileCreatingDirs(const std::filesystem::path& path, std::span<const u8> bytes);

/// Resolves a source path to its `Document::textures` index, decodes it once
/// (failures too, so a missing file is looked for once), and interns the
/// textures a bake invents. Paths compare with trailing NULs and spaces cut —
/// an `.m3` string is a fixed-width field — and case folded.
class ExportTextureCache {
public:
    ExportTextureCache(::whiteout::models::wem::Document& document, io::IContentProvider* provider);

    /// The `Document::textures` index for @p path, or `kInvalidIndex`.
    u32 IndexOf(const std::string& path) const;

    /// The decoded texture behind @p path, or null.
    const ::whiteout::textures::Texture* Decode(const std::string& path);

    /// Adds a texture the source never had and returns its document index.
    u32 Intern(const std::string& path);

private:
    ::whiteout::models::wem::Document& document_;
    io::IContentProvider* provider_ = nullptr;
    std::unordered_map<std::string, u32> byPath_;
    std::unordered_map<std::string, std::optional<::whiteout::textures::Texture>> decoded_;
};

} // namespace whiteout::flakes
