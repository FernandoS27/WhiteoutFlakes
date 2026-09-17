#include "export/mdx_save.h"

#include "export/export_common.h"
#include "export/texture_codec.h"
#include "export/texture_io.h"
#include "io/mdx_model_adapter.h"
#include "renderer/model/model_instance.h"
#include "renderer/model/model_template.h"
#include "renderer/render_service.h"
#include "renderer/scene_manager.h"
#include "string_util.h"
#include "whiteout/flakes/content_provider.h"
#include "whiteout/flakes/util/path_utf8.h"

#include <whiteout/models/mdx/mdx.h>
#include <whiteout/textures/texture.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <optional>
#include <span>
#include <system_error>
#include <vector>

namespace whiteout::flakes {

namespace model = renderer::model;

namespace {

// Writes the model's file-backed textures into `targetDir`, preserving each
// texture's relative path. A non-empty `formatExt` converts each texture to that
// format and rewrites the model's texture path to match (so the saved model
// references the exported files); "" keeps the referenced format (verbatim copy
// when the source bytes already match, else re-encoded to it — e.g. Reforged
// serves .dds for a .blp path). Textures already present at the target are left
// untouched. `model` is mutated (path rewrites); the caller writes it after.
TextureExportCounters ExportModelTextures(renderer::RenderService& service, whiteout::mdx::Model& model,
                                          const std::filesystem::path& targetDir,
                                          const std::string& formatExt) {
    namespace fs = std::filesystem;
    TextureExportCounters st;
    io::IContentProvider* provider = service.Scene().ActiveContentProvider();

    for (auto& tex : model.textures) {
        // replaceableId != 0 → runtime team-color/glow/tileset, no source file.
        if (tex.replaceableId != 0 || tex.fileName.empty())
            continue;

        const std::string origName = tex.fileName; // capture before any rewrite

        std::string relStr = origName;
        std::replace(relStr.begin(), relStr.end(), '\\', '/');
        const fs::path relPath(io::FsPathFromUtf8(relStr));
        const std::string srcExt = tools::ToLowerAscii(io::PathToUtf8(relPath.extension()));
        const std::string targetExt = formatExt.empty() ? srcExt : formatExt;

        fs::path outRel = relPath;
        outRel.replace_extension(targetExt);
        const fs::path outFile = targetDir / outRel;

        // Point the saved model at the exported file when the format changed
        // (WC3 texture paths use backslashes).
        if (targetExt != srcExt) {
            std::string nn = origName;
            if (const auto dot = nn.rfind('.'); dot != std::string::npos)
                nn.resize(dot);
            tex.fileName = nn + targetExt;
        }

        std::error_code ec;
        if (fs::exists(outFile, ec)) {
            st.skipped++;
            continue;
        }
        if (!provider) {
            st.failed++;
            continue;
        }

        std::string actualExt;
        std::optional<std::vector<u8>> bytes = provider->ReadFile(origName, &actualExt);
        if (!bytes || bytes->empty()) {
            std::fprintf(stderr, "[viewer] Export: source texture not found: %s\n",
                         origName.c_str());
            st.failed++;
            continue;
        }
        actualExt = tools::ToLowerAscii(actualExt);

        std::vector<u8> outBytes;
        if (targetExt == actualExt) {
            outBytes = std::move(*bytes);
        } else {
            const TextureDecode decoded = DecodeTexture(*bytes, actualExt);
            EncodedTexture encoded = decoded.texture ? EncodeTextureAs(*decoded.texture, targetExt)
                                                     : EncodedTexture{{}, decoded.problem};
            if (!encoded) {
                std::fprintf(stderr, "[viewer] Export: convert failed %s -> %s\n", origName.c_str(),
                             targetExt.c_str());
                st.failed++;
                continue;
            }
            outBytes = std::move(encoded.bytes);
        }

        if (!WriteFileCreatingDirs(outFile, outBytes)) {
            st.failed++;
            continue;
        }
        st.exported++;
    }
    return st;
}

} // namespace

bool SaveModelAsMdx(renderer::RenderService& service, model::Actor* focus, const std::filesystem::path& modelPath,
                    const std::string& outPath, whiteout::mdx::MdlFormat dialect, bool exportTextures,
                    const std::string& formatExt) {
    // The active document's actor holds a strong ref to its template — the most
    // reliable source. Fall back to the (weak) path-keyed cache if there's no
    // focused actor.
    std::shared_ptr<model::ModelTemplate> tmpl;
    if (focus)
        tmpl = focus->sourceTemplate;
    if (!tmpl || !tmpl->adapter)
        tmpl = service.Scene().Templates().Lookup(io::PathToUtf8(modelPath));

    // Save As writes an MDX/MDL through whiteout::mdx::Writer, so it needs the
    // parsed MDX model and not a format-neutral snapshot. Refuse a non-MDX
    // source rather than writing something that is not the model the user is
    // looking at.
    const auto* mdxAdapter =
        tmpl ? dynamic_cast<const io::MdxModelAdapter*>(tmpl->adapter.get()) : nullptr;
    if (!mdxAdapter && focus) {
        // No template at all: the actor was spawned from a live source. A
        // `.wem` opened as either Warcraft III profile is exactly that, and
        // writing it back out as MDX is the conversion the user came for.
        mdxAdapter = dynamic_cast<const io::MdxModelAdapter*>(focus->animation.Source().get());
    }
    if (!mdxAdapter) {
        std::fprintf(stderr, "[viewer] Save As: this model is not MDX; nothing to write\n");
        return false;
    }
    // Copy so texture-path rewrites during export don't touch the live template.
    whiteout::mdx::Model model = mdxAdapter->SourceModel();
    if (exportTextures) {
        const TextureExportCounters st =
            ExportModelTextures(service, model, io::FsPathFromUtf8(outPath).parent_path(), formatExt);
        std::printf("[viewer] Textures: %d exported, %d skipped, %d failed\n", st.exported,
                    st.skipped, st.failed);
    }
    try {
        whiteout::mdx::Writer writer;
        writer.write(outPath, model, dialect);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[viewer] Save As FAILED '%s': %s\n", outPath.c_str(), e.what());
        return false;
    }
    std::printf("[viewer] Saved model: %s\n", outPath.c_str());
    return true;
}

// There's no editable model behind an effect — the viewer just plays it — so
// "save" writes the source bytes out verbatim. Only the on-disk case can be a
// plain copy; the rest is read back through the provider the effect loaded with.
bool SaveEffectCopy(renderer::RenderService& service, const std::filesystem::path& modelPath,
                    const std::string& outPath) {
    const std::filesystem::path& src = modelPath;

    std::error_code ec;
    if (std::filesystem::exists(src, ec) && !ec) {
        std::filesystem::copy_file(src, io::FsPathFromUtf8(outPath),
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            std::printf("[viewer] Saved effect: %s\n", outPath.c_str());
            return true;
        }
    }

    const std::string rel = io::PathToUtf8(src);
    std::optional<std::vector<u8>> bytes;
    if (io::IContentProvider* provider = service.Scene().ActiveContentProvider())
        bytes = provider->ReadFile(rel);
    if (!bytes) {
        std::fprintf(stderr, "[viewer] Save As FAILED: cannot read effect '%s'\n", rel.c_str());
        return false;
    }

    std::ofstream out(io::FsPathFromUtf8(outPath), std::ios::binary);
    if (out)
        out.write(reinterpret_cast<const char*>(bytes->data()),
                  static_cast<std::streamsize>(bytes->size()));
    if (!out) {
        std::fprintf(stderr, "[viewer] Save As FAILED: cannot write '%s'\n", outPath.c_str());
        return false;
    }
    std::printf("[viewer] Saved effect: %s (%zu bytes)\n", outPath.c_str(), bytes->size());
    return true;
}

} // namespace whiteout::flakes
