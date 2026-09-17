#pragma once

// ============================================================================
// Save As for a model that stays in its own format: a Warcraft III model
// re-serialised as MDX or MDL (optionally with its textures written beside it),
// and a PopcornFX effect copied byte for byte.
// ============================================================================

#include "whiteout/flakes/types.h"

#include <whiteout/models/mdx/writer.h>

#include <filesystem>
#include <string>

namespace whiteout::flakes::renderer {
class RenderService;
namespace model {
struct Actor;
}
} // namespace whiteout::flakes::renderer

namespace whiteout::flakes {

/// The formats Save As can convert a model's textures to — every WhiteoutLib
/// image writer except GIF. `ext` includes the dot and is what the model's
/// texture paths are rewritten to; the empty entry keeps each source format.
/// `label` is not localized; the empty entry's label is drawn from the catalog.
struct SaveAsTextureFormat {
    const char* ext;
    const char* label;
};
inline constexpr SaveAsTextureFormat kSaveAsTextureFormats[] = {
    {"", ""},        {".blp", "BLP"}, {".png", "PNG"},  {".tga", "TGA"},
    {".dds", "DDS"}, {".bmp", "BMP"}, {".jpg", "JPEG"}, {".tif", "TIFF"},
};

/// Re-serialise the model @p focus shows (or, without one, the template cached
/// for @p modelPath) to @p outPath (UTF-8). The writer picks MDX binary or MDL
/// text from the extension; @p dialect matters for MDL only. With
/// @p exportTextures the file-backed textures are written beside it first,
/// converted to @p textureFormat unless that is empty. False, and logged, when
/// the source is not MDX or the write throws.
bool SaveModelAsMdx(renderer::RenderService& service, renderer::model::Actor* focus,
                    const std::filesystem::path& modelPath, const std::string& outPath,
                    ::whiteout::mdx::MdlFormat dialect, bool exportTextures, const std::string& textureFormat);

/// Write the effect at @p modelPath to @p outPath verbatim: a file copy when it
/// is on disk, otherwise its bytes read back through the active scene's provider
/// (a Storage Explorer document's path is archive-relative).
bool SaveEffectCopy(renderer::RenderService& service, const std::filesystem::path& modelPath,
                    const std::string& outPath);

} // namespace whiteout::flakes
