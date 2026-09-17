#pragma once

// ============================================================================
// Texture containers the conversions write: the block-compressed `.dds`
// Reforged and StarCraft II read, classic Warcraft III's BLP1, a texture a bake
// made, and Save As's any-writer re-encode.
// ============================================================================

#include "baked_texture.h"

#include "whiteout/flakes/types.h"

#include <whiteout/textures/texture.h>

#include <string>
#include <string_view>
#include <vector>

namespace whiteout::flakes {

/// Encoded bytes, or why there are none.
struct EncodedTexture {
    std::vector<u8> bytes;
    /// Empty on success.
    std::string problem;
    /// Something worth a diagnostic that did not stop the encode.
    std::string note;

    explicit operator bool() const {
        return problem.empty();
    }
};

/// Whether any texel of level 0 is not fully opaque. @p texture must be RGBA8.
bool HasAlpha(const ::whiteout::textures::Texture& texture);

/// A texture read from a file, as the `.dds` Reforged and StarCraft II read:
/// BC1 where it is opaque and BC3 where it is not, with a full mip chain. The
/// sRGB flag is cleared because the writer promotes an sRGB texture to a DX10
/// header, and the legacy `DXT1`/`DXT5` FourCC is what both games' tools read —
/// colour space is the slot's declaration, not the container's. Mips matter:
/// both engines minify without them and the result crawls.
EncodedTexture EncodeGameDds(const ::whiteout::textures::Texture& source);

/// Classic Warcraft III's BLP1, paletted — not BLP2, which is World of
/// Warcraft's, and not JPEG, which is lossy on exactly the hard edges a model
/// texture is made of. An alpha plane only where a texel needs one.
EncodedTexture EncodeClassicBlp(const ::whiteout::textures::Texture& source);

/// A texture the export MADE rather than read. Every choice `EncodeGameDds`
/// makes is already made: the container is fixed by what the map is, the
/// chain is rebuilt the way `BakedTexture::mips` says, and the alpha sniff
/// would demote a BC5 normal to BC1. A bake that declares no kind is
/// box-filtered and says so in `note`.
EncodedTexture EncodeBaked(const BakedTexture& baked);

/// @p source re-encoded as @p extension (".png", ".blp", ...), through RGBA8,
/// which every writer accepts.
EncodedTexture EncodeTextureAs(const ::whiteout::textures::Texture& source,
                               std::string_view extension);

} // namespace whiteout::flakes
